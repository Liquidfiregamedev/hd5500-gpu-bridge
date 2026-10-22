# HD5500 GPU Bridge — how to test

> **Photosensitive seizure warning.** This is experimental graphics software. During tests the game window can flicker, flash, show rapidly changing colours or patterns, or alternate between black and bright frames. A very small number of people may have seizures from such images even without any history of epilepsy. If you or a family member has epilepsy or has had seizures, consult a doctor first. Stop and close the game at once if you notice dizziness, altered vision, eye or muscle twitching, loss of awareness or involuntary movements. Play in a well-lit room, keep your distance from the screen, take breaks, and do not watch unattended tests while they run.

**Easiest: double-click `Bridge-Menu.cmd`.** One window with every action below, compatibility options as checkboxes (atlas compression off, WARP software GPU, D3D11 debug layer, reset Vulkan fallback, frame pictures, shader dumps) and buttons that open the latest results. Hover over anything for an explanation.

Every launcher writes a run folder `reports\gpu-run-<date-time>\` (game logs, bridge logs, Player.log) and zips it when the game exits. The unattended ones also write `SUMMARY.txt` into that folder and `reports\LATEST-SUMMARY.txt`.

## Unattended tests (start them and leave the PC alone)

| Launcher | What it does | Time |
|---|---|---|
| `Auto-Test-Boot.cmd` | Starts the game, reaches the main menu, quits by itself (`-QuitAfterBoot`). Shows boot time and start-up errors. | 1–4 min |
| `Auto-Test-Map.cmd` | Opens a Dev-quicktest map by itself (`-quicktest`), atlas compression off, saves frame pictures (`frames\*.bmp`) and every translated shader (`hlsl\*.hlsl`), closes after 300 s. | ~6 min |
| `Auto-Test-Map-Compressed.cmd` | Same map test with the engine default (GPU atlas compression on). Compare with the one above. | ~6 min |
| `Laptop-Selftest.cmd` | Runs the developer's emulator test suite on this PC's real GPU and on Microsoft's WARP software reference. RimWorld is not started. Result: `reports\LATEST-SELFTEST.txt` (SELFTEST PASS/FAIL per test). | 1–2 min |
| `Auto-Test-All.cmd` | Runs the three above in a row and writes `reports\AUTO-TEST-ALL.txt`. | ~14 min |

Do not click inside the game window during these; it closes itself.

## Play / manual tests

| Launcher | Use |
|---|---|
| `Play-RimWorld.cmd` | Normal play through the bridge, diagnostics off. |
| `Play-RimWorld-NoAtlasCompression.cmd` | Play with `-noCompressAtlases` (current workaround while atlas compression is suspected). |
| `Diagnose-Shaders.cmd` | Play, and save every translated shader into `reports\hlsl-dump`. |
| `Diagnose-UI-Frame.cmd` | Full start-up checks plus frame trace and frame pictures. |
| `Play-RimWorld-Record.cmd` | Records all Vulkan calls (GFXReconstruct) for replay in the emulator. Large files. |

## Reading a result

Open `SUMMARY.txt`. The important lines:

- **Serious bridge problems** — anything above 0 is a bug in the bridge (failed command buffers, rejected D3D11 states or views). The lines are listed under "Bridge events".
- **Game errors/warnings** — the game's own complaints with mod-metadata noise removed. `[VALIDATION]` lines come from the bridge rejecting something.
- **Frame diagnostics** — for each saved frame: how much of the screen is non-black and the average colour. A frame with 0% non-black is a black screen.

## Descriptor-set recycling (v13.81)
If textures or colours look wrong somewhere after v13.81, add `RW_GPU_SET_RECYCLE=0` to `experiment-env.txt` and compare. The run log `[RW-GPU-SETS]` line shows sets recycled versus newly built.

## Private local build (v13.78)
`..\local\Play-Local.cmd` / `Speedtest-Local.cmd` start this launcher with `-LocalDir`. Settings are in `local\local-env.txt` (`RW_LOCAL_*`). Check the run log for `LOCAL:` lines and Player.log for `[HD5500-LOCAL]` and `HardwareProfile: loaded cached`. Without `-LocalDir` the launcher ignores the local folder completely.

## Other Vulkan driver (v13.77)
Menu → Diagnostics → **Compare: other Vulkan driver**, or `Launch-HD5500-GPU.ps1 -QuickStart -AltIcd "C:\mesa\x64\dzn_icd.x86_64.json" -GameArguments "-quicktest"`. Runs the game on that driver instead of the bridge (nothing installed). On the HD 5500 Mesa Dozen will probably refuse to start (D3D12 binding tier 1); the report says why. Useful on PCs with D3D12 tier 2+ but a broken Vulkan driver.

## Reference run (v13.29)
Menu → Diagnostics → **REFERENCE: real Vulkan on CPU (lavapipe, very slow)**, or `Launch-HD5500-GPU.ps1 -QuickStart -Reference -GameArguments "-quicktest"`. RimWorld runs on Mesa lavapipe instead of the bridge: 1–3 frames per second, but correct Vulkan output. Look at the colonists, trees, smoke and shadows: if they are right here, the bridge is at fault.

## Disk space (v13.29)
Runs are no longer zipped (add `-Zip` if you need a ZIP). After each run only the newest 6 run folders and 3 self-tests are kept (`-KeepRuns N`).

## Switches for `Launch-HD5500-GPU.ps1`

`-QuickStart` (skip the slow start-up checks), `-FrameDiag` (frame trace + pictures), `-DumpShaders` (HLSL dumps), `-AutoCloseSeconds N`, `-GameArguments "<game args>"` (e.g. `-quicktest`, `-noCompressAtlases`, `-QuitAfterBoot`, `-ResetVulkanFallback`), `-Capture`, `-SwapchainImages N`, `-Reference` (lavapipe), `-AltIcd "<driver .json>"` (v13.77: another Vulkan driver, e.g. Mesa Dozen), `-Zip`, `-KeepRuns N`.

Environment switches read by the bridge itself: `RW_HD5500_D3D_DEBUG_LAYER=1` (Microsoft D3D11 debug layer, needs Windows "Graphics Tools"), `RW_GPU_BLIT_MODE=compute` (old blit path for A/B), `RW_GPU_SHADER_DISK_CACHE=<dir>|0` (persistent shader cache, v13.22), `RW_GPU_CB_EMULATE=1` (copy uniform-buffer windows like on feature level 11_0, v13.28), `RW_GPU_UPLOAD_DEDUPE=0` (no upload deduplication, v13.24), `RW_GPU_DOWNLOAD_ALL=1` (old behaviour: copy every storage buffer back after each draw, v13.23), `RW_GPU_SAMPLER_MAXLOD=<n>` (clamp all samplers to mip ≤ n, v13.31), `RW_GPU_RECTCLEAR=clearview` (partial clears through D3D11.1 ClearView instead of draws; v13.21), `RW_GPU_DUMP_HLSL=<folder>`, `RW_HD5500_FRAME_DIAG=1`. With frame diagnostics on, `vtx-compare` lines (v13.30) compare the vertices/indices of pawn-atlas draws on the GPU with the CPU copy the game wrote (`same=0` = stale upload). Read by the game: `WAYLITH_VULKAN_DEVICE=<part of GPU name>` (e.g. `Basic Render` for WARP; the bridge creates its device on that adapter since v13.19).


## One-off experiments (v13.31)
Put `experiment-env.txt` next to `Launch-HD5500-GPU.ps1` with lines `RW_GPU_NAME=value` (or `RW_HD5500_…`). Every normal launch applies them and prints `EXPERIMENT: …` in the run log; reference (lavapipe) runs ignore it. Delete the file to return to normal.


## v13.33 regression test `vbopersist`
Vertex buffers bound before a pipeline bind and a render pass must still be used by the draw (Vulkan keeps them; the old bridge dropped them — the cause of invisible colonists/trees). Expected a blue triangle on red (`209.1,0.0,45.9`). Part of the emulator suite and the laptop self-test.


## v13.35 regression test `varying`
The pixel shader reads only location 1 of the two vertex outputs (0 = green decoy, 1 = blue). Expected a blue triangle on red (`209.1,0.0,45.9`); green means the D3D11 varying registers are misaligned (the cause of invisible plants). Switch: `RW_GPU_VARYING_MASK=0` turns the fix off for A/B tests.


## v13.37 tests `varyblock` / `varystruct`
Same idea as `varying` for interface-block and plain-struct vertex outputs (the game's UI shaders use a struct). Blue triangle = correct; red = the pixel shader read the Color member instead of UV. Performance switch: `RW_GPU_DYNAMIC_NOOVERWRITE=0` restores full-buffer uploads of dynamic buffers; `RW_HD5500_STATE_CACHE=0` disables the native state cache (on by default since v13.39).


## v13.41 test `water` (reference comparison)
`tools\smoke.exe <icd> 4 water` draws RimWorld's water exactly as the game does — the game's own `TerrainWater.vert`, `WaterDepth.frag` (height field into an R16_FLOAT target) and `TerrainWater.frag` (normal from the height-field gradient, lighting, reflection, noise) — and then **reads the frame back itself** and prints `readback … meanRGB=… lumaStddev=…` plus an 8×6 grid of pixel colours. Because the readback does not depend on the bridge, the same program can run on a reference Vulkan driver:
- Emulator suite (`run-suite.sh`): runs `water` on the bridge and on Windows lavapipe (`vulkan_lvp.dll` in the run folder, joined from `reference\lavapipe`); ok when the mean per-sample difference ≤ 4, at most 4 samples differ by > 12 (edge pixels) and the picture is not flat (`lumaStddev` > 10).
- Laptop self-test (`Laptop-Selftest.ps1`): same comparison on the HD 5500 and on WARP, reference = lavapipe (joined on first use, ~56 MB). A `FAIL water` on the HD 5500 with `ok` on WARP points at the Intel driver; both failing points at the bridge. Window should show rippled blue-grey water with a soft red border; a flat colour is the bug seen in the game.
- `SMOKE_READBACK=0` skips the readback for all modes (for ICDs whose swapchain images cannot be copied).
Frame dumps of float targets now carry `floatR=min/mean/max nonzero=%`; the WaterDepth draws also log `atlas-draw`/`vtx-compare` lines (vertex position + Color32 depth weight) in dump frames.

First laptop result (v13.41/v13.42, selftest-113002): HD 5500, WARP and lavapipe all draw the same rippled water (mean difference 2.3 levels) — the water shaders work on this GPU; the in-game flat lake comes from the game's inputs (uniforms, vertex alpha, textures), see changelog v13.42.


## v13.43 test `instuav` and option "Close test windows when finished"
`instuav`: the vertex shader reads per-instance offsets from a storage buffer that is not read-only (bridge: graphics UAV), then a rectangle clear (`vkCmdClearAttachments`) runs between two draws with the same descriptor set. Correct: two blue triangles (left and right). Self-test variant `instuav-old` runs it with `RW_GPU_UAV_RESTORE=0` (fix off): Since v13.44 it EXPECTS the bug picture (237.7,0.0,17.2): measured on the HD 5500 and WARP in selftest-20261019-145657, proving that helper passes dropped the UAV (cause of the flickering forbidden X). Wine keeps UAVs, so in the emulator both pass.
The menu option "Close test windows when finished" (default on) sets `RW_BRIDGE_AUTOCLOSE=1`; finished consoles close after 8 s instead of waiting for a key.


## Diagnostic tools (v13.45-tools)
Bridge menu → "Install diagnostic tools (optional)" installs the laptop tool set through winget (`Install-Tools.ps1 -List` shows what each one sees; `-Only renderdoc,presentmon` installs a subset). What to use when is in `TOOLS.md` (recipes: wrong picture, freeze, crash, capability change).


## v13.45 number format and summary digests
Every start now runs the game in invariant-culture mode (saves always use a dot), shown in the launcher output as `Number format: invariant culture ON`. To compare with the old behaviour add `-NoInvariantCulture` to the launcher arguments. Check after a session: load a save made with v13.45 — pawn ages and need bars must be normal.
SUMMARY.txt now has two new sections: **Game log digest** (from the game's own Player.log) and **Bridge performance digest** (slow command buffers: count, median, p95, max, most expensive command types). Those two sections are the first thing to compare between builds.


## v13.76 checks
- Menu Home page shows a "Graphics: … no Vulkan driver, the bridge is the right tool" line; the PC check has a "RimWorld 1.7 graphics compatibility (Vulkan)" section.
- Speed test as before (same numbers as v13.75 expected; the ring now also stays on in frames with buffer copies).

## v13.75 checks
- `[RW-GPU-UBRING] uniform ring … created` and `first batch` must appear now (v13.74 run 211523 had windows=0); `[RW-GPU-IA]` `uniform ring: windows=` growing, `fallbacks=` small; `descriptor sets bound by content match=` growing (was 5).
- `[RW-GPU-CMDTIME]`: UploadMemory far below ~70 calls/frame; BindDescriptorSet below 0.57 ms/frame (run 211523).
- Pictures as before. Wrong colours/material tints → `RW_GPU_UNIFORM_RING_STATIC=0`; wrong or flickering textures → `RW_GPU_SET_CONTENT_UBO=0`.
- Harness: suite 34 checks (new `ubowin`).

## v13.74 checks
- Log once each: `[RW-GPU-UBRING] uniform ring 4194304 bytes: created` + `first batch: N uniform windows`, `[RW-GPU-IDXPOOL] index pool 16777216 bytes: created` + `first index buffer copied into the pool`. No `switched off for this session` line (if it appears: tell Claude, try `RW_GPU_INDEX_POOL_MB=32`).
- `[RW-GPU-IA]`: `uniform ring: windows=` growing, `fallbacks=` near 0; `index pool: hits=` growing, `copies=` small; `index-buffer binds: D3D11 calls=` far below one per draw.
- `[RW-GPU-CMDTIME]`: UploadMemory count per frame much lower than ~89; BindIndexBuffer µs per call lower (it no longer calls the driver); DrawIndexed may rise slightly (resolves the index binding).
- Pictures: materials/colours (ring) and map geometry (pool) as in v13.72. Wrong colours → `RW_GPU_UNIFORM_RING=0`; broken geometry → `RW_GPU_INDEX_POOL=0`.
- Harness: suite 33 checks (new `ubring`, `idxpool`).

## v13.73 checks
- Log: `[RW-GPU-SET] descriptor-set content fast path in use` and `[RW-GPU-UPLOAD] merging small buffer uploads per recording` once each; `[RW-GPU-IA]` line counts both (`descriptor sets bound by content match=`, `uploads merged at recording=`).
- `[RW-GPU-CMDTIME]`: BindDescriptorSet total ms/frame and UploadMemory count per frame lower than run 150140 (403 binds 1.8–2.2 ms; 89 uploads 1.1 ms).
- Pictures: textures, colonists, forbidden X, water, plants as in v13.72. Wrong or flickering textures → run once with `RW_GPU_SET_CONTENT_FAST=0` (experiment-env.txt); stale/jumping objects → `RW_GPU_UPLOAD_MERGE=0`.
- Harness: suite 31 checks (new `setcopy`, `multiwin`).
- `[RW-GPU-QOS] Windows power throttling for the game process: off (set)`.
- Laptop health > **PC check**: read the Suggestions list at the end of `reports\power-check-*\power-check.txt`; tell Claude the "Graphics adapters", "Memory" and "Firmware speed limits" sections.
- Optional A/B: **Cool mode (Turbo off)** → 10-minute Speed test → compare 1% lows / 100 ms frames with a run without; then **Undo cool mode**.

## v13.72 checks
- `[RW-GPU-CMDTIME]` BindDescriptorSet µs per call lower than ~2.2–2.9 µs (run 150140); no missing textures (a wrong SRV skip would show as black or wrong textures — compare with `RW_GPU_SRV_CACHE_FIRST=0`).

## v13.71 checks
- Laptop health page: tick "Heat guard" and "Frame-rate cap 45 fps", then a Speed test. FRAMES line `heat guard active/armed (engaged N times)`; frametimes.csv: 22 ms frames (45 fps), 33 ms (30 fps) while throttled instead of 100 ms.
- Present path: `present.pacing` now holds the cap wait (expected ~5 ms at 45 fps), with the CPU idle during it.

## v13.70b checks
- Map test: no more 2.5–8 s pauses every 20 s; one swapchain picture per 60 s after frame 3600. If the options menu still freezes in a Speed test, note the time.

## v13.70 checks
- For speed/stutter comparisons use Tests → "Speed test (5 min, no pictures)"; the map test includes diagnostics (present.diag 1.5–9 ms/frame, pauses every 20 s).
- game.stderr.txt: `[RW-GPU-WSI] tearing ...: supported, on` (or "not supported" — then only the 3 buffers help) and `(3 buffers, ...)`.
- Fullscreen part of a test: frametimes.csv `present_ms` should stay < 1 ms (was 7–10 ms), APITIME `present` close to the windowed value, p95 frame time back near 20 ms.
- A/B via experiment-env.txt: `RW_GPU_ALLOW_TEARING=0`, `RW_GPU_FLIP_BUFFERS=2`.
- Hiccup analysis: ignore stalls every ~20 s in map tests (frame-picture dumps).

## v13.69 checks
- `[RW-GPU-IA]` "sub-page uploads without write-watch query" grows; FRAMES `watch-query` ms/frame lower than 0.3.
- FRAMES `cleanup actions kept=` much lower relative to `skipped=` than before (shape key).
- `[RW-GPU-CMDTIME]` BindDescriptorSet / UploadMemory µs per call lower than in runs 131823/133552.
- `[RW-OPT-THREADS] job workers set to normal priority: 3; render thread above normal: yes`; `[RW-OPT-PRESENT]` line.
- `[RW-GPU-APITIME]`: `reset` should fall from ~1 ms to ~0.1 ms; the `present.*` parts add up to `present` — the largest part is the next target.
- A/B: the old behaviour via experiment-env.txt `RW_GPU_DEFERRED_FREE=0`, `RW_GPU_SUBPAGE_DIRECT=0`, `RW_GPU_CLEANUP_SHAPE_KEY=0`, `RW_GPU_UAV_RESOLVE_ALWAYS=1`, `RW_OPT_THREADS=0`.

## v13.68 checks
- `[RW-GPU-APITIME]` lines (summary "Frame-level Vulkan calls"): per-frame ms in submit / present / acquire / waitFences / begin / end / reset. Compare with the profile's `[RenderThread] RenderThread.RenderFrame` self time (~12 ms in run 134332).

## v13.67 checks
- Profile tables now show `[MainThread]` and `[RenderThread]` rows; RenderEnqueue on the main thread = waiting for the render thread.
- Zoom-out limit test: experiment-env.txt `RW_OPT_MAX_ZOOM=40` → `[RW-OPT-ZOOM] maximum zoom-out 40 cells` in game.stderr.txt; compare zoomed-out frame times with a default run at similar CPU speed (frametimes.csv `cpu_speed_pct`).
- Level 3 profile: experiment-env.txt `RW_OPT_PROFILE=3` → `[RW-OPT-PROFILE] ... level 3 (instrumented methods: N)`; the RenderThread rows list Waylith.Rendering methods.
- Normal play: enable "HD5500 Optimizer" in RimWorld's mod list; Options → Mod settings → HD5500 Optimizer → "Maximum zoom-out".

## v13.66 checks
- Summary "Game frame profile" section: `game-profile.txt` exists; session table lists engine scopes (GameUpdate, RenderEnqueue, Throttle, ...).
- `[RW-GPU-IA]`: "offset folded into firstIndex" should be large, "D3D11 calls" much smaller than before.
- `[RW-GPU-CMDTIME]` lines: ms per frame by command type; compare zoomed-in and zoomed-out windows.
- Comparison test: the same map test with the menu option "Bridge work on its own thread" on.
- Suite: 34 checks (`ibofs`; v13.73 `setcopy`, `multiwin`; v13.74 `ubring`, `idxpool`; v13.75 `ubowin`).

## v13.65 checks
- Summary top: "GPU compatibility" block shows the HD 5500, driver 20.19.15.5171, feature level 11_1, `verdict: SUPPORTED (tested: Intel HD 5500)`, 0 degraded items expected.
- On another PC: send `LATEST-SUMMARY.txt`; the verdict and `DEGRADED:` lines tell what that GPU lacks.

## v13.64 checks (project now on F:)
- Start the menu from `F:\HD5500-GPU-Bridge\package\Bridge-Menu.cmd` (or run Create-Desktop-Shortcut.cmd once). Map test: game.stderr.txt has `[RW-GPU-SHADERCACHE] prewarm: N shader objects …`, `[RW-GPU-SHADER-TIME]` lines with "(prewarmed object)" for shaders created during play, and the FRAMES line `recording=X ms/frame (bridge part Y ms, …)`.
- Launcher prints a warning when the system drive has under 500 MB free.

## v13.63 checks
- Map test: FRAMES line shows `descriptor sets fast=N full=M` with N large; picture correct (watch textures on pawns/plants/UI — a wrong fast-path skip would show wrong or missing textures). Compare "Bridge cost per draw" with 11.0 µs (run 012002). If anything looks wrong: `RW_GPU_SET_FASTPATH=0` in experiment-env.txt.

## v13.62 checks
- FRAMES line in game.stderr.txt: `recording=X ms/frame (N commands/frame)` present; nothing else should change (refactor). Map test as usual; compare "Bridge cost per draw".

## v13.61 checks
- Map test with experiment-env `RW_GPU_PROCESS_PRIORITY=above`: game.stderr.txt has `[RW-GPU-PRIORITY] game process priority class above normal (set)`; compare "Bridge cost per draw" and 1%/0.1% lows with the previous run. Afterwards comment the line out (the menu option does the same for normal play).
- Laptop health > Power check: new sections "Memory (RAM) modules", "Intel graphics power plan", "Windows Game Mode", "Process Lasso", "Busiest background programs".

## Measuring note (v13.60c)
- The quicktest map is different every run (draws per command buffer seen: 296–480). Compare runs by bridge ms **per draw** (`DrawIndexedCommand xN` in the `[RW-GPU-PROF] sample` lines) or average several runs; let the laptop cool down between back-to-back tests (CPU dips below 40% show in the "CPU speed" line).

## v13.60 checks (flip model, recording)
- game.stderr.txt: `[RW-GPU-WSI] swapchain 1920x…, presentation model flip-discard` for EVERY swapchain (also the one created after the window resize, 1920x1177); no `flip-model swapchain refused` (v13.60b fix). Picture normal, window resize/alt-tab OK. If the screen stays black: `RW_GPU_FLIP_MODEL=0` in experiment-env.txt.
- FRAMES line: `repeat uploads not recorded=` large; compare fps / 1% low with run 20261020-001754 (47.8 / 16.2) at Normal speed; input should feel at least as responsive.

## v13.59 checks (frame phases)
- After a map test, LATEST-SUMMARY has "Where a frame goes (game side, average): simulation ticks … ms, world+map update/drawing … ms, rest … ms" and "frames over 50 ms by largest part"; `spikes.csv` has the columns ticks_ms, map_update_ms, rest_ms. The three parts add up to the frame time.

## v13.58 checks (submit thread experiment)
- experiment-env.txt contains `RW_GPU_SUBMIT_THREAD=1` → run the Map test at Normal speed. LATEST-SUMMARY "Frame pacing" line must say `submit thread on`; compare fps / 1% low / p99 with run 20261020-001754 (47.8 fps, 1% low 16.2, p99 38.8 ms). Watch for visual errors (flicker, missing objects) and hangs.
- Afterwards comment the line out again (`#RW_GPU_SUBMIT_THREAD=1`) unless it is adopted.

## v13.57 checks (frame spikes)
- Run the **Map test** (menu → Home → Map test). Afterwards LATEST-SUMMARY.txt should contain "Frame spikes (gameplay frames over 50 ms): N - X mostly bridge work, Y mostly game/.NET/Windows time", a "game-side view" line with causes from `spikes.csv`, a ".NET garbage collection while in game" line, and (when the camera was moved) "Bridge frames of 30-150 ms".
- The run folder should contain `spikes.csv` next to `ticks.csv` and `frametimes.csv`.
- Fps and bridge ms per frame should be equal or slightly better than v13.56 (~49 fps, ~9.8 ms).
- GC experiment (only after a normal run): put `RW_GPU_GC_LOWLATENCY=1` in experiment-env.txt, run the map test again, compare 1%/0.1% lows and the spike causes; remove the line afterwards.
- Old per-command health check for comparison: `RW_GPU_HEALTH_EVERY_COMMAND=1`.

## v13.56 checks
Menu → Results → **Clean up disk space**: the console lists deleted old runs, "pictures: N BMP -> PNG, … MB saved" and the free space before/after; `reports\cleanup-<date>.txt` has the same. Map test: the launcher prints "Tick-rate counter: on"; SUMMARY.txt has "Tick rate (game simulation speed): … ticks/s of 60.0 target (…%)" and the run folder has `ticks.csv`. If the game does not start with the counter, add `-NoTickCounter` (or untick the Play option) and report it.

## v13.55 checks
SUMMARY.txt: "Frame-time statistics (gameplay)" + "-> while playing: average …, 1% low …, 0.1% low …" + the "excluded from gameplay" line; `frametimes.csv` opens in Excel/LibreOffice with 7 clean columns (dots as decimal separator). Menu: dark title bar, readable blue primary button, full tip line at the bottom.

## v13.54 checks
Open the Bridge menu: it should be dark (title bar too on Windows 10 22H2). Click through the sidebar pages; hover over buttons and options — the bottom panel must explain each one. "Switch to light mode" must change the colours and be remembered at the next start. Home shows the last run's fps and lows and the last self-test verdict (F5 refreshes). All buttons start the same things as before.

## v13.53 checks
**Self-test** (Bridge menu → "Bridge self-test on this GPU"): `uavalias` must be ok (two blue triangles). `uavalias-old` runs with the fix off and must show the old bug (only one black triangle, debug-layer-msgs=4: "UAV at slot 1 overlaps slot 2") — confirmed in selftest-20261019-231715. **Map test or play:** find a steam geyser (it sprays every 8–33 s for 3–8 s at speed 1) and pawns walking on sand/soil (dust puffs): the puffs must be visible now. `game.stderr.txt` shows `[RW-GPU-UAV-ALIAS]` when the fix is active. **Frame statistics:** SUMMARY.txt → "Frame-time statistics (whole session)": average, 1% low, 0.1% low FPS, frame-time percentiles, stutters, missed refreshes; the run folder has `frametimes.csv` (one line per frame — open in Excel/LibreOffice and chart the `frametime_ms` column).

## v13.52 checks
Map test, then compare with run 215428 (v13.51b): `SUMMARY.txt` frame pacing "bridge=… ms" per frame and frames over 50/100 ms; in `game.stderr.txt` the `[RW-GPU-PROF] sample` lines: "BindDescriptorSetCommand x150=…ms" and "binds tex=…ms" should drop from 2–4 ms to well under 1 ms. The frame pacing line ends with `push constants uploaded=N unchanged=M` (M should be large). Look for anything that looks wrong (UI, pawns, shadows, water): if something does, run once with `RW_HD5500_STATE_CACHE=0` and once with `RW_GPU_PUSH_SKIP_SAME=0` in `experiment-env.txt` to find which part.

## v13.51b checks
Map test: `SUMMARY.txt` frame pacing line ends with `cleanup actions kept=N skipped=M` — M should be several times N. Compare bridge ms per frame with v13.51 (12.1 ms in run 213701). Nothing should look different. If something stays bound or flickers after this version, run once with `RW_GPU_CLEANUP_DEDUPE=0` in `experiment-env.txt` to compare. Shadows: for a fair comparison take a screenshot at the same zoom on the same map with the normal bridge and with "REFERENCE: real Vulkan on CPU".

## v13.51 checks
Run a Map test (or play 5+ minutes). In `SUMMARY.txt` → "Bridge performance digest": the `[RW-GPU-STATECACHE]` line should show far fewer `invalidations` than v13.50 (run 210336: ~2.6 million) and more `hits`; compare the "Frame pacing" line (bridge ms per frame, fps) with the previous run on a similar map. Throttle monitor: "Firmware limit" must read `none` while the laptop runs normally (v13.50 wrongly showed a limit all the time). Nothing should look different in the game; if a single-channel texture (grey or one-colour icons, masks) looks wrong, report it — that is the swizzle path this version touched.

## v13.50 checks
Bridge menu → "Laptop speed, heat and power": open **Throttle monitor (live window)**, then start Play or a Map test. The window must switch from "Waiting for RimWorld" to live values within a few seconds; the graph turns red when the laptop throttles. Try the **Heat guard** checkbox: when throttling starts, "Heat guard: ACTIVE" and fps settle at ~30; after 20 s of normal speed it lifts. **Power check** writes `reports\power-check-...`. Geyser: watch a steam geyser for at least 40 s at speed 1 (it sprays only every 8–33 s); if the puffs stay invisible, `game.stderr.txt` lines `[RW-GPU-UAV-MISSING]` tell which shader lost data.

## v13.49 checks
Geyser: find a steam geyser (or anything that throws dust puffs: pawns walking on sand, fire) — the puffs must be visible. `game.stderr.txt`: `[RW-GPU-UAV-RESOLVE] set N layout differs (used by set+binding): bound{...} pipeline{...}` shows which layouts differ. Vulkan version: put `RW_GPU_API_VERSION=1.1` in `experiment-env.txt` and play; `SUMMARY.txt` shows "Effective Vulkan version 1.1.0" and no "baked shader packs will not be used" warning. Report anything that looks different from 1.0. Emulator: `api11` and `water ... spirv13=identical` in the suite.

## v13.48 checks (freezes)
Play a quickstart or a save for 5+ minutes. `SUMMARY.txt` → "Bridge performance digest": the "Frame pacing" line (fps, p95, hitches) and "CPU speed (100% = nominal clock)". If CPU speed drops below 40% the stutter is the laptop throttling: check the adapter (Lenovo original, enough watts), battery health, fans; ThrottleStop shows the reason ("PROCHOT", "EDP", "POWER"). In `game.stderr.txt`, `[RW-GPU-PROF] sample` vs `slow` lines with "speed probe" ~900 µs vs ~5000 µs show the same. Experiments in `experiment-env.txt`: `RW_GPU_FPS_CAP=30` (less heat), `RW_GPU_WRITE_WATCH=0` (old upload path, for comparison), `DOTNET_GCgen0size=0x4000000` (fewer .NET garbage collections).

## v13.47b checks
Self-test: `uavoffset` ok (two blue triangles), `uavoffset-old` shows the old bug (plain red). In a game: forbid a few items (survival packs from the quicktest crash are ideal), pause, zoom in: every forbidden item must show the red X; unpause at speed 1–3 and it must stay. In `game.stderr.txt` the `inst-data` lines for the `count=6` batch with a varying `inst=` must show a UAV with `first=` > 0 and real numbers in `f=[...]`.

## v13.47 checks
Self-test: `uavlayout` must be ok (two blue triangles) and `uavlayout-old` must show the old bug (plain red, fix off). Map test: watch forbidden items at speed 1–3 — the red X must stay on every forbidden item. In `SUMMARY.txt`, "Bridge performance digest" shows `UAV slot corrections: N` (N > 0 means the game hit the path the fix corrects). `RW_GPU_UAV_RESOLVE=0` in `experiment-env.txt` switches the fix off for a comparison run.

## v13.46 switches
`RW_GPU_UPLOAD_DIRECT=0` (in `experiment-env.txt`) restores the old texture upload path if any texture looks wrong after v13.46; `RW_GPU_UPLOAD_TRACE=1` logs the first direct uploads. Map test dump frames now contain `inst-data` lines for instanced draws (forbidden-X investigation).
