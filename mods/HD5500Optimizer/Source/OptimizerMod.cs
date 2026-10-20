// HD5500 Optimizer 0.2 - part 2: switchable game-side settings for slow PCs.
//
// Measured on the HD 5500 laptop (bridge v13.66, map tests with RimWorld's own frame profiler):
// Waylith builds a frame description on the main thread and hands it to its render thread
// ("RenderEnqueue" = the main thread waiting for the render thread). The render thread encodes
// one Vulkan draw per visible mesh; zoomed out, about twice as many map sections are visible
// (820 instead of ~400 draws), the render thread (game encoding + bridge) needs 8-14 ms, and the
// frame rate drops from 60 to 33-45 fps. Zoom-out is where the lag is felt.
//
// Setting 1 - maximum zoom-out: RimWorld's camera allows a view size of up to 60 cells
// (CameraMapConfig.sizeRange.max). Lowering it (e.g. 40-45) keeps the number of visible map
// sections, and so the draw count, in the range this PC handles at 60 fps. 0 = game default.
// Applied to Find.CameraDriver.config every frame it differs (cheap field compare). No patches.
// Test override: RW_OPT_MAX_ZOOM=<cells> (bridge launcher test runs).
using System;
using System.Globalization;
using UnityEngine;
using Verse;

namespace HD5500Bridge.Optimizer
{
    public class OptimizerSettings : ModSettings
    {
        public float maxZoomOut;   // 0 = game default
        public bool threadTuning = true;   // 0.3: job workers Normal, render thread AboveNormal
        public int frameCap;               // 0.3: 0 = game default (monitor refresh rate)

        public override void ExposeData()
        {
            Scribe_Values.Look(ref maxZoomOut, "maxZoomOut", 0f);
            Scribe_Values.Look(ref threadTuning, "threadTuning", true);
            Scribe_Values.Look(ref frameCap, "frameCap", 0);
        }
    }

    public class OptimizerMod : Mod
    {
        public static OptimizerSettings Settings;

        public OptimizerMod(ModContentPack content) : base(content)
        {
            Settings = GetSettings<OptimizerSettings>();
        }

        public override string SettingsCategory() { return "HD5500 Optimizer"; }

        public override void DoSettingsWindowContents(Rect inRect)
        {
            var list = new Listing_Standard();
            list.Begin(inRect);
            list.Label("Settings for slow PCs (built for an Intel HD 5500 laptop). Each one can be switched off again.");
            list.Gap();
            float v = Settings.maxZoomOut;
            string text = v <= 0f ? "game default (60)" : ((int)v).ToString(CultureInfo.InvariantCulture) + " cells";
            v = list.SliderLabeled("Maximum zoom-out: " + text, v <= 0f ? 60f : v, 30f, 60f, 0.5f,
                "How far the map camera can zoom out. Zoomed far out, the game draws about twice as many map sections, which costs frame rate on weak CPUs/GPUs (measured: 60 fps zoomed in, 33-45 fps fully zoomed out on the HD 5500 laptop). 60 = game default.");
            Settings.maxZoomOut = v >= 59.5f ? 0f : Mathf.Round(v);
            list.Gap();
            list.CheckboxLabeled("Render thread before job workers (thread priorities)", ref Settings.threadTuning,
                "RimWorld 1.7 starts its job worker threads at above-normal priority and its render thread at normal priority. On a 2-core laptop the render thread is the bottleneck (measured), so this swaps them: job workers normal, render thread above normal. No visual change. Applies from the next map load.");
            list.Gap();
            float c = Settings.frameCap <= 0 ? 61f : Settings.frameCap;
            c = list.SliderLabeled("Frame-rate cap: " + (Settings.frameCap <= 0 ? "game default (monitor)" : Settings.frameCap + " fps"), c, 30f, 61f, 0.5f,
                "Lower cap = less CPU/GPU work per second, so a laptop that overheats throttles less and stutters less. Game speed is unaffected down to 30 fps (the game simulates at most 2x speed-multiplier ticks per frame). 61 = game default.");
            Settings.frameCap = c >= 60.5f ? 0 : Mathf.RoundToInt(c);
            list.End();
        }
    }

    public class ZoomLimiter : GameComponent
    {
        bool broken;
        readonly float envMax;
        float originalMax = -1f;

        public ZoomLimiter(Game game)
        {
            string e = Environment.GetEnvironmentVariable("RW_OPT_MAX_ZOOM");
            if (!string.IsNullOrEmpty(e) && float.TryParse(e, NumberStyles.Float, CultureInfo.InvariantCulture, out float f)) envMax = f;
        }

        public override void GameComponentUpdate()
        {
            if (broken) return;
            try
            {
                var driver = Find.CameraDriver;
                if (driver == null || driver.config == null) return;
                float want = envMax > 0f ? envMax : (OptimizerMod.Settings != null ? OptimizerMod.Settings.maxZoomOut : 0f);
                var range = driver.config.sizeRange;
                if (originalMax < 0f) originalMax = range.max;
                float target = want > 0f ? Math.Max(range.min + 1f, want) : originalMax;
                if (Math.Abs(range.max - target) > 0.01f)
                {
                    driver.config.sizeRange = new FloatRange(range.min, target);
                    Console.Error.WriteLine("[RW-OPT-ZOOM] maximum zoom-out " + target.ToString("0", CultureInfo.InvariantCulture) + " cells (game default " + originalMax.ToString("0", CultureInfo.InvariantCulture) + ")");
                }
            }
            catch (Exception ex)
            {
                broken = true;
                try { Console.Error.WriteLine("[RW-OPT-ZOOM] switched off after an error: " + ex.Message); } catch { }
            }
        }
    }

    // 0.3: thread priorities and frame cap (both no visual change).
    // Measured with the all-thread profile: the render thread is the bottleneck on the HD 5500
    // laptop; Waylith's 3 job workers (ProcessorCount-1 on a 2-core/4-thread i5) run at
    // AboveNormal (WaylithJobRuntime static constructor), the render thread at Normal.
    public class ThreadTuner : GameComponent
    {
        bool done, broken;
        int appliedCap = -1;
        int originalCap = -1;

        public ThreadTuner(Game game) { }

        public override void GameComponentUpdate()
        {
            if (broken) return;
            try
            {
                var s = OptimizerMod.Settings;
                if (!done)
                {
                    done = true;
                    LogPresentMode();
                    if (s == null || s.threadTuning || Environment.GetEnvironmentVariable("RW_OPT_THREADS") == "1")
                    {
                        if (Environment.GetEnvironmentVariable("RW_OPT_THREADS") != "0") ApplyPriorities();
                    }
                }
                int cap = s != null ? s.frameCap : 0;
                string e = Environment.GetEnvironmentVariable("RW_OPT_FPS_CAP");
                if (!string.IsNullOrEmpty(e) && int.TryParse(e, NumberStyles.Integer, CultureInfo.InvariantCulture, out int ec)) cap = ec;
                if (cap != appliedCap)
                {
                    if (originalCap < 0) originalCap = Waylith.EngineWindow.TargetFrameRate;
                    int target = cap >= 30 ? cap : originalCap;
                    Waylith.EngineWindow.TargetFrameRate = target;
                    appliedCap = cap;
                    Console.Error.WriteLine("[RW-OPT-FPS] frame-rate cap " + target + " (game default " + originalCap + ")");
                }
            }
            catch (Exception ex)
            {
                broken = true;
                try { Console.Error.WriteLine("[RW-OPT-THREADS] switched off after an error: " + ex.Message); } catch { }
            }
        }

        static void ApplyPriorities()
        {
            const System.Reflection.BindingFlags Any = System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.Instance;
            int workers = 0;
            var jobs = typeof(Waylith.Jobs.WaylithJobRuntime).GetField("Workers", Any);
            if (jobs != null && jobs.GetValue(null) is System.Threading.Thread[] list)
            {
                foreach (var th in list)
                {
                    if (th != null && th.IsAlive) { th.Priority = System.Threading.ThreadPriority.Normal; workers++; }
                }
            }
            bool render = false;
            var app = Waylith.EngineApplication.Instance;
            var rtField = typeof(Waylith.EngineApplication).GetField("_renderThread", Any);
            object rt = app != null && rtField != null ? rtField.GetValue(app) : null;
            var thField = rt != null ? rt.GetType().GetField("_thread", Any) : null;
            if (thField != null && thField.GetValue(rt) is System.Threading.Thread rthread && rthread.IsAlive)
            {
                rthread.Priority = System.Threading.ThreadPriority.AboveNormal;
                render = true;
            }
            Console.Error.WriteLine("[RW-OPT-THREADS] job workers set to normal priority: " + workers + "; render thread above normal: " + (render ? "yes" : "not found"));
        }

        static void LogPresentMode()
        {
            try
            {
                var app = Waylith.EngineApplication.Instance;
                object ctx = app?.RenderContext;
                var prop = ctx?.GetType().GetProperty("PresentMode");
                if (prop != null) Console.Error.WriteLine("[RW-OPT-PRESENT] swapchain present mode " + prop.GetValue(ctx) + " (0 immediate, 1 mailbox, 2 fifo)");
            }
            catch { }
        }
    }
}
