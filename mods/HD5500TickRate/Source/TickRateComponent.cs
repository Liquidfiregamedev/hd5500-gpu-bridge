// HD5500 GPU Bridge - tick-rate counter (read-only diagnostic mod, v13.56; GC + spike log v13.57; frame phases v13.59).
// Measures how many game ticks RimWorld actually simulates per real second, compared with the
// target (60 ticks/s x speed multiplier). No Harmony, no patches: one GameComponent that reads
// Find.TickManager once per rendered frame.
// Output: RW_GPU_TICK_FILE=<path> -> CSV, one line per second; every 10 s a [RW-GPU-TICKS] line on
// the console (the launcher captures it in game.stderr.txt; Summarize-Run.ps1 reads it).
// v13.57: also watches the .NET garbage collector, to find out what the game-side frame spikes
// are (42 of 49 frames over 50 ms in the v13.56 map test had little bridge time):
//   * per-second CSV gets gc0/gc1/gc2 (collections in that second), gc_pause_ms, alloc_mb;
//   * spikes.csv (next to the tick CSV): one line per frame over 50 ms with the ticks simulated
//     in that frame, GCs and GC pause inside it -> "GC", "many ticks" or "other" per spike;
//   * the 10 s console line adds GC and spike counts.
// v13.59: frame phases (a light version of what Dubs Performance Analyzer shows). Frame order in
// 1.7 (Game.UpdatePlay): TickManagerUpdate (simulation ticks) -> World/Map update (map meshes,
// drawing) -> GameComponentUpdate (here) -> ... UI, rendering, bridge, present -> next frame.
// GameComponentTick runs at the end of every tick, so per frame:
//   ticks_ms  = from the first tick's start (first tick end - TickManager.MeanTickTime) to the last tick end
//   map_ms    = last tick end -> this GameComponentUpdate (world + map update and drawing)
//   rest_ms   = the remainder (UI, render submission, bridge, present, Windows)
// Frames without ticks (paused) only have frame and rest.
// Opt-in experiment RW_GPU_GC_LOWLATENCY=1: GCSettings.LatencyMode = SustainedLowLatency
// (avoids blocking full collections while background GC is available). Default: unchanged.
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime;
using Verse;

namespace HD5500Bridge.TickRate
{
    public class TickRateComponent : GameComponent
    {
        static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
        readonly Stopwatch clock = Stopwatch.StartNew();
        StreamWriter csv;
        bool csvTried;
        double secondStart;
        int secondTicks = -1;
        int secondFrames;
        // 10 s window (only seconds where the game was running, not paused)
        double windowStart;
        double windowTps, windowTarget, windowFps;
        int windowSeconds;
        // whole session (running seconds only)
        double sessionTicks, sessionTarget;
        int sessionSeconds, sessionBelow90;
        // v13.57: GC + per-frame spikes
        double lastFrame = -1.0;
        int frameTicksStart;
        int[] gcFrame = new int[3];
        double gcPauseFrame;
        int[] gcSecond = new int[3];
        double gcPauseSecond;
        long allocSecond;
        StreamWriter spikes;
        int windowSpikes, windowSpikesGc, windowSpikesTicks;
        int[] windowGc = new int[3];
        double windowGcPause;
        const double SpikeMs = 50.0;
        // v13.59 phases
        double firstTickEnd = -1.0, lastTickEnd = -1.0;
        double windowTickMs, windowMapMs, windowRestMs; int windowPhaseFrames;
        int windowSpikesMap, windowSpikesRest, windowSpikesTickPhase;

        public override void GameComponentTick()
        {
            if (broken) return;
            double t = clock.Elapsed.TotalSeconds;
            if (firstTickEnd < 0.0) firstTickEnd = t;
            lastTickEnd = t;
        }

        static double PauseMs() { return GC.GetTotalPauseDuration().TotalMilliseconds; }
        static int[] GcCounts() { return new[] { GC.CollectionCount(0), GC.CollectionCount(1), GC.CollectionCount(2) }; }

        public TickRateComponent(Game game) { }

        // v13.64: never let a diagnostic break the game: a full disk made Console.Error throw
        // (IOException) on every frame. Any failure switches the counter off for the session.
        bool broken;
        public override void GameComponentUpdate()
        {
            if (broken) return;
            try { UpdateInner(); }
            catch (Exception) { broken = true; }
        }

        void UpdateInner()
        {
            TickManager tm = Find.TickManager;
            if (tm == null) return;
            double now = clock.Elapsed.TotalSeconds;
            if (secondTicks < 0)
            {
                secondTicks = tm.TicksGame; secondStart = now; windowStart = now;
                lastFrame = now; frameTicksStart = tm.TicksGame; gcFrame = GcCounts(); gcPauseFrame = PauseMs();
                gcSecond = GcCounts(); gcPauseSecond = gcPauseFrame; allocSecond = GC.GetTotalAllocatedBytes(false);
                if (Environment.GetEnvironmentVariable("RW_GPU_GC_LOWLATENCY") == "1")
                {
                    try { GCSettings.LatencyMode = GCLatencyMode.SustainedLowLatency; } catch (Exception) { }
                }
                Console.Error.WriteLine(string.Format(Inv, "[RW-GPU-TICKS] GC mode: server={0} concurrent-latency={1} (RW_GPU_GC_LOWLATENCY={2})",
                    GCSettings.IsServerGC, GCSettings.LatencyMode, Environment.GetEnvironmentVariable("RW_GPU_GC_LOWLATENCY") ?? "unset"));
                return;
            }
            secondFrames++;
            CheckFrame(now, tm);
            double elapsed = now - secondStart;
            if (elapsed < 1.0) return;

            int ticks = tm.TicksGame - secondTicks;
            double tps = ticks / elapsed;
            double fps = secondFrames / elapsed;
            bool paused = tm.Paused;
            double target = paused ? 0.0 : 60.0 * tm.TickRateMultiplier;
            string speed = tm.CurTimeSpeed.ToString();
            int[] gcNow = GcCounts();
            double pauseNow = PauseMs();
            long allocNow = GC.GetTotalAllocatedBytes(false);
            WriteCsv(now, tps, target, speed, fps, tm.TicksGame, gcNow[0] - gcSecond[0], gcNow[1] - gcSecond[1], gcNow[2] - gcSecond[2],
                pauseNow - gcPauseSecond, (allocNow - allocSecond) / 1048576.0 / elapsed);
            for (int g = 0; g < 3; g++) windowGc[g] += gcNow[g] - gcSecond[g];
            windowGcPause += pauseNow - gcPauseSecond;
            gcSecond = gcNow; gcPauseSecond = pauseNow; allocSecond = allocNow;

            if (!paused && target > 0.0)
            {
                windowTps += tps; windowTarget += target; windowFps += fps; windowSeconds++;
                sessionTicks += tps; sessionTarget += target; sessionSeconds++;
                if (tps < 0.9 * target) sessionBelow90++;
            }
            if (now - windowStart >= 10.0)
            {
                if (windowSeconds > 0)
                {
                    Console.Error.WriteLine(string.Format(Inv,
                        "[RW-GPU-TICKS] last {0}s running: {1:F1} ticks/s of {2:F1} target ({3:F0}%), {4:F1} fps | session: {5:F0}% of target over {6} s, {7} s below 90%",
                        windowSeconds, windowTps / windowSeconds, windowTarget / windowSeconds, 100.0 * windowTps / windowTarget, windowFps / windowSeconds,
                        sessionTarget > 0 ? 100.0 * sessionTicks / sessionTarget : 0.0, sessionSeconds, sessionBelow90));
                }
                Console.Error.WriteLine(string.Format(Inv,
                    "[RW-GPU-GC] last {0:F0}s: collections gen0={1} gen1={2} gen2={3}, GC pause {4:F0} ms | frames over {5:F0} ms: {6} ({7} with a GC, {8} with many ticks)",
                    now - windowStart, windowGc[0], windowGc[1], windowGc[2], windowGcPause, SpikeMs, windowSpikes, windowSpikesGc, windowSpikesTicks));
                double windowLength = now - windowStart;
                windowStart = now; windowTps = windowTarget = windowFps = 0.0; windowSeconds = 0;
                if (windowPhaseFrames > 0)
                {
                    Console.Error.WriteLine(string.Format(Inv,
                        "[RW-GPU-PHASES] last {0:F0}s, average per frame: simulation ticks {1:F1} ms, world+map update/drawing {2:F1} ms, rest (UI, rendering, bridge, present) {3:F1} ms | frames over {4:F0} ms by largest part: ticks {5}, map-update {6}, rest {7}",
                        windowLength, windowTickMs / windowPhaseFrames, windowMapMs / windowPhaseFrames, windowRestMs / windowPhaseFrames, SpikeMs, windowSpikesTickPhase, windowSpikesMap, windowSpikesRest));
                }
                windowTickMs = windowMapMs = windowRestMs = 0.0; windowPhaseFrames = 0; windowSpikesMap = windowSpikesRest = windowSpikesTickPhase = 0;
                windowGc = new int[3]; windowGcPause = 0.0; windowSpikes = windowSpikesGc = windowSpikesTicks = 0;
            }
            secondStart = now; secondTicks = tm.TicksGame; secondFrames = 0;
        }

        // One frame = time between two GameComponentUpdate calls. A frame over 50 ms is written to
        // spikes.csv with what happened inside it.
        void CheckFrame(double now, TickManager tm)
        {
            double frameMs = (now - lastFrame) * 1000.0;
            int ticks = tm.TicksGame - frameTicksStart;
            double tickMs = 0.0, mapMs = 0.0;
            bool havePhases = firstTickEnd >= 0.0 && firstTickEnd >= lastFrame;
            if (havePhases)
            {
                tickMs = Math.Max(0.0, (lastTickEnd - firstTickEnd) * 1000.0 + tm.MeanTickTime);
                mapMs = Math.Max(0.0, (now - lastTickEnd) * 1000.0);
                if (tickMs + mapMs > frameMs) tickMs = Math.Max(0.0, frameMs - mapMs);
                windowTickMs += tickMs; windowMapMs += mapMs; windowRestMs += frameMs - tickMs - mapMs; windowPhaseFrames++;
            }
            firstTickEnd = lastTickEnd = -1.0;
            double restMs = frameMs - tickMs - mapMs;
            if (frameMs > SpikeMs)
            {
                int[] gcNow = GcCounts();
                double pauseNow = PauseMs();
                int g0 = gcNow[0] - gcFrame[0], g1 = gcNow[1] - gcFrame[1], g2 = gcNow[2] - gcFrame[2];
                double pause = pauseNow - gcPauseFrame;
                // "many ticks": more than 4 simulation ticks in one frame at a speed that needs that
                // many (the game runs up to 2x the speed multiplier ticks per frame).
                bool gc = g0 + g1 + g2 > 0;
                bool manyTicks = ticks > 4;
                // v13.59: largest phase decides when it is not a GC pause.
                string phase = !havePhases ? "rest" : (tickMs >= mapMs && tickMs >= restMs ? "ticks" : (mapMs >= restMs ? "map-update" : "rest"));
                string cause = gc && pause > 0.3 * frameMs ? "GC" : (manyTicks ? "many-ticks" : phase);
                if (phase == "map-update") windowSpikesMap++; else if (phase == "ticks") windowSpikesTickPhase++; else windowSpikesRest++;
                windowSpikes++;
                if (gc) windowSpikesGc++;
                if (manyTicks) windowSpikesTicks++;
                WriteSpike(now, frameMs, ticks, tm.CurTimeSpeed.ToString(), g0, g1, g2, pause, cause, havePhases ? tickMs : double.NaN, havePhases ? mapMs : double.NaN, restMs);
                gcFrame = gcNow; gcPauseFrame = pauseNow;
            }
            else
            {
                // baseline for the next frame (three counter reads, a few nanoseconds)
                gcFrame[0] = GC.CollectionCount(0); gcFrame[1] = GC.CollectionCount(1); gcFrame[2] = GC.CollectionCount(2);
                gcPauseFrame = PauseMs();
            }
            lastFrame = now; frameTicksStart = tm.TicksGame;
        }

        void WriteSpike(double now, double frameMs, int ticks, string speed, int g0, int g1, int g2, double pause, string cause, double tickMs, double mapMs, double restMs)
        {
            if (spikes == null)
            {
                if (csv == null) return;   // only when the tick CSV is on (launcher test runs)
                try
                {
                    string path = Path.Combine(Path.GetDirectoryName(Environment.GetEnvironmentVariable("RW_GPU_TICK_FILE")) ?? ".", "spikes.csv");
                    spikes = new StreamWriter(path, false) { AutoFlush = true };
                    spikes.WriteLine("time_s,frame_ms,ticks_in_frame,speed,gc0,gc1,gc2,gc_pause_ms,cause,ticks_ms,map_update_ms,rest_ms");
                }
                catch (Exception) { spikes = null; return; }
            }
            try { spikes.WriteLine(string.Format(Inv, "{0:F2},{1:F1},{2},{3},{4},{5},{6},{7:F1},{8},{9:F1},{10:F1},{11:F1}", now, frameMs, ticks, speed, g0, g1, g2, pause, cause, tickMs, mapMs, restMs)); }
            catch (Exception) { }
        }

        void WriteCsv(double now, double tps, double target, string speed, double fps, int ticksGame, int gc0, int gc1, int gc2, double gcPauseMs, double allocMbPerS)
        {
            if (!csvTried)
            {
                csvTried = true;
                string path = Environment.GetEnvironmentVariable("RW_GPU_TICK_FILE");
                if (!string.IsNullOrEmpty(path))
                {
                    try
                    {
                        csv = new StreamWriter(path, false) { AutoFlush = true };
                        csv.WriteLine("time_s,ticks_per_s,target_ticks_per_s,speed,fps,ticks_game,gc0,gc1,gc2,gc_pause_ms,alloc_mb_per_s");
                    }
                    catch (Exception) { csv = null; }
                }
            }
            if (csv == null) return;
            try { csv.WriteLine(string.Format(Inv, "{0:F1},{1:F1},{2:F1},{3},{4:F1},{5},{6},{7},{8},{9:F1},{10:F1}", now, tps, target, speed, fps, ticksGame, gc0, gc1, gc2, gcPauseMs, allocMbPerS)); }
            catch (Exception) { }
        }
    }
}
