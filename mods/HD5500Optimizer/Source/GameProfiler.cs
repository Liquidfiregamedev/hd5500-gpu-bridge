// HD5500 Optimizer 0.2 (0.1 = main thread only) - part 1: find out where RimWorld's own frame time goes on this PC.
//
// RimWorld 1.7 (Waylith engine) contains a frame profiler (Waylith.Profiling.FrameProfiler, the
// dev-mode "Frame Profiler" window). This component switches it on without opening any window,
// reads the finished frames from its history ring once per second and adds up, per scope
// ("GameUpdate", "RenderEnqueue", "Throttle", ...; with level 2 also every Verse.*/RimWorld.*
// method), the time spent in it. Every 30 s it writes a ranked table to RW_OPT_PROFILE_FILE and a
// short [RW-OPT-PROFILE] line to the console (the bridge launcher captures it).
//
//   RW_OPT_PROFILE=1  built-in engine scopes only (cheap, main thread)
//   RW_OPT_PROFILE=3  instead instruments the engine's rendering code (Waylith.Rendering.*, the
//                     host's RimWorldWaylith.Rendering.*, Verse.MapDrawer/Section): render thread detail
//   RW_OPT_PROFILE=2  plus method instrumentation of Verse.* and RimWorld.* (the engine rewrites
//                     the method bodies; trivial methods are muted automatically). Heavier: the
//                     numbers are inflated, use them to rank, not as absolute times.
//
// Read-only: nothing in the game changes. Any failure switches the profiler off for the session.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using Verse;
using Waylith.Profiling;
using Waylith.Profiling.Instrumentation;

namespace HD5500Bridge.Optimizer
{
    public class GameProfiler : GameComponent
    {
        static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
        readonly int level;
        readonly string file;
        bool started, broken;
        int lastFrameIndex = -1;
        int frames, framesWindow;
        double frameMsSum, frameMsWindow;
        readonly System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();
        double lastRead, lastReport;

        struct Acc { public double total, self; public long calls; }
        readonly Dictionary<long, Acc> session = new Dictionary<long, Acc>();
        readonly Dictionary<long, Acc> window = new Dictionary<long, Acc>();
        readonly Dictionary<int, string> names = new Dictionary<int, string>();
        // frames over 50 ms: what was big in them (self time per scope)
        readonly Dictionary<long, double> spikeSelf = new Dictionary<long, double>();
        int spikeFrames;

        public GameProfiler(Game game)
        {
            string v = Environment.GetEnvironmentVariable("RW_OPT_PROFILE");
            level = v == "3" ? 3 : (v == "2" ? 2 : (v == "1" ? 1 : 0));
            file = Environment.GetEnvironmentVariable("RW_OPT_PROFILE_FILE");
        }

        void Start()
        {
            started = true;
            if (level == 0) return;
            FrameProfiler.SetHistoryCapacity(600);
            FrameProfiler.SetMainThreadOnly(false);   // 0.2: also the render thread and workers
            FrameProfiler.SetEnabled(true);
            if (level >= 2 && ProfilerInstrumentation.IsSupported)
            {
                FrameProfiler.AutoMuteTrivialLabels = true;
                // level 2: the game's own code; level 3: the engine's rendering code instead (render thread)
                ProfilerInstrumentation.SetTargets(level >= 3
                    ? new[] { "Waylith!Waylith.Rendering.*::*", "RimWorld!RimWorldWaylith.Rendering.*::*", "Assembly-CSharp!Verse.MapDrawer::*", "Assembly-CSharp!Verse.Section::*" }
                    : new[] { "Assembly-CSharp!Verse.*::*", "Assembly-CSharp!RimWorld.*::*" });
                ProfilerInstrumentation.SetEnabled(true);
            }
            Console.Error.WriteLine("[RW-OPT-PROFILE] frame profiler on, level " + level
                + (level >= 2 ? (ProfilerInstrumentation.IsSupported ? " (instrumented methods: " + ProfilerInstrumentation.InstrumentedMethodCount + ")" : " (instrumentation not supported, scopes only)") : "")
                + ", scope overhead " + FrameProfiler.ScopeOverheadNanoseconds.ToString("0", Inv) + " ns");
        }

        public override void GameComponentUpdate()
        {
            if (broken) return;
            try
            {
                if (!started) Start();
                if (level == 0) return;
                double t = clock.Elapsed.TotalSeconds;
                if (t - lastRead >= 1.0) { lastRead = t; ReadNewFrames(); }
                if (t - lastReport >= 30.0 && framesWindow > 0) { lastReport = t; Report(); }
            }
            catch (Exception e)
            {
                broken = true;
                try { Console.Error.WriteLine("[RW-OPT-PROFILE] switched off after an error: " + e.GetType().Name + ": " + e.Message); } catch { }
                try { FrameProfiler.SetEnabled(false); } catch { }
            }
        }

        void ReadNewFrames()
        {
            int count = FrameProfiler.HistoryCount;
            // newest first; stop at the first frame we already have
            var fresh = new List<FrameProfiler.FrameSnapshot>();
            int newest = -1;
            for (int i = 0; i < count && i < 590; i++)
            {
                if (!FrameProfiler.TryGetHistorySnapshot(i, out var s) || s == null) continue;
                if (s.frameIndex <= lastFrameIndex) break;
                if (newest < 0) newest = s.frameIndex;
                s.Retain();
                fresh.Add(s);
            }
            if (newest >= 0) lastFrameIndex = newest;
            foreach (var s in fresh)
            {
                try { Add(s); } finally { s.Release(); }
            }
        }

        void Add(FrameProfiler.FrameSnapshot s)
        {
            frames++; framesWindow++;
            frameMsSum += s.totalMs; frameMsWindow += s.totalMs;
            bool spike = s.totalMs > 50f;
            if (spike) spikeFrames++;
            int sections = Math.Min(s.sectionCount, s.sections.Length);
            for (int k = 0; k < sections; k++)
            {
                var sec = s.sections[k];
                int thread = ThreadKey(sec);
                int end = Math.Min(sec.firstPathEntry + sec.pathEntryCount, Math.Min(s.pathEntryCount, s.pathEntries.Length));
                for (int i = Math.Max(0, sec.firstPathEntry); i < end; i++)
                {
                    var e = s.pathEntries[i];
                    long key = ((long)thread << 32) | (uint)e.pathId;
                    Bump(session, key, e); Bump(window, key, e);
                    if (spike) { spikeSelf.TryGetValue(key, out double v); spikeSelf[key] = v + e.selfMicroseconds; }
                }
            }
        }

        // thread name label (0.2): "Main", "Render", job workers ...; workers are folded into one key
        readonly Dictionary<int, string> threadNames = new Dictionary<int, string>();
        int ThreadKey(FrameProfiler.ThreadSection sec)
        {
            int id = sec.nameLabelId.Value;
            if (!threadNames.ContainsKey(id))
            {
                string n;
                try { n = FrameProfiler.GetLabel(sec.nameLabelId); } catch { n = "thread"; }
                if (string.IsNullOrEmpty(n)) n = "thread " + sec.threadId;
                threadNames[id] = n;
            }
            return id;
        }

        static void Bump(Dictionary<long, Acc> d, long key, FrameProfiler.PathEntry e)
        {
            d.TryGetValue(key, out Acc a);
            a.total += e.totalMicroseconds; a.self += e.selfMicroseconds; a.calls += e.calls;
            d[key] = a;
        }

        string Name(long key)
        {
            int thread = (int)(key >> 32);
            threadNames.TryGetValue(thread, out string tn);
            return "[" + (tn ?? "?") + "] " + PathName((int)(key & 0xffffffff));
        }

        string PathName(int pathId)
        {
            if (names.TryGetValue(pathId, out string cached)) return cached;
            var parts = new List<string>();
            var entries = ProfilerPathTable.Entries;
            int id = pathId;
            for (int depth = 0; depth < 12 && id > 1 && id < ProfilerPathTable.Count; depth++)
            {
                parts.Add(FrameProfiler.GetLabel(entries[id].labelId));
                id = entries[id].parentPathId;
            }
            parts.Reverse();
            string name = parts.Count == 0 ? "?" : string.Join(" > ", parts);
            names[pathId] = name;
            return name;
        }

        string ThreadShort(long key)
        {
            threadNames.TryGetValue((int)(key >> 32), out string tn);
            return string.IsNullOrEmpty(tn) ? "" : tn.Substring(0, Math.Min(tn.Length, 12)) + ":";
        }

        static string Leaf(string path) { int i = path.LastIndexOf(" > ", StringComparison.Ordinal); return i < 0 ? path : path.Substring(i + 3); }

        void Report()
        {
            var sb = new StringBuilder();
            sb.AppendLine("HD5500 Optimizer - game frame profile (RimWorld's own frame profiler), level " + level);
            sb.AppendLine(string.Format(Inv, "session: {0} frames, average frame {1:0.0} ms; frames over 50 ms: {2}", frames, frames > 0 ? frameMsSum / frames : 0.0, spikeFrames));
            sb.AppendLine();
            Table(sb, "Last 30 s window: scopes by SELF time per frame (time not inside a child scope)", window, framesWindow, true, 30);
            Table(sb, "Last 30 s window: scopes by TOTAL time per frame (incl. children)", window, framesWindow, false, 30);
            Table(sb, "Whole session: scopes by SELF time per frame", session, frames, true, 40);
            if (spikeFrames > 0)
            {
                sb.AppendLine("Frames over 50 ms (" + spikeFrames + "): self time per spike frame");
                var list = new List<KeyValuePair<long, double>>(spikeSelf);
                list.Sort((a, b) => b.Value.CompareTo(a.Value));
                for (int i = 0; i < list.Count && i < 20; i++)
                    sb.AppendLine(string.Format(Inv, "  {0,8:0.00} ms  {1}", list[i].Value / 1000.0 / spikeFrames, Name(list[i].Key)));
                sb.AppendLine();
            }
            if (!string.IsNullOrEmpty(file))
            {
                try { File.WriteAllText(file, sb.ToString()); } catch { }
            }
            // console: top 6 by self time in this window
            var top = new List<KeyValuePair<long, Acc>>(window);
            top.Sort((a, b) => b.Value.self.CompareTo(a.Value.self));
            var line = new StringBuilder();
            line.Append(string.Format(Inv, "[RW-OPT-PROFILE] last window {0} frames, avg {1:0.0} ms; self ms/frame:", framesWindow, frameMsWindow / framesWindow));
            for (int i = 0; i < top.Count && i < 6; i++)
                line.Append(string.Format(Inv, " {0} {1:0.00}", ThreadShort(top[i].Key) + Leaf(PathName((int)(top[i].Key & 0xffffffff))), top[i].Value.self / 1000.0 / framesWindow));
            Console.Error.WriteLine(line.ToString());
            window.Clear(); framesWindow = 0; frameMsWindow = 0;
        }

        void Table(StringBuilder sb, string title, Dictionary<long, Acc> d, int frameCount, bool bySelf, int rows)
        {
            if (frameCount <= 0) return;
            sb.AppendLine(title);
            sb.AppendLine("   ms/frame   calls/frame  scope path");
            var list = new List<KeyValuePair<long, Acc>>(d);
            list.Sort((a, b) => bySelf ? b.Value.self.CompareTo(a.Value.self) : b.Value.total.CompareTo(a.Value.total));
            for (int i = 0; i < list.Count && i < rows; i++)
            {
                var a = list[i].Value;
                sb.AppendLine(string.Format(Inv, "  {0,8:0.000}  {1,10:0.0}   {2}", (bySelf ? a.self : a.total) / 1000.0 / frameCount, (double)a.calls / frameCount, Name(list[i].Key)));
            }
            sb.AppendLine();
        }
    }
}
