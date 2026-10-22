# RimWorld 1.7 / Waylith: engine notes

**Source:** decompiled 1.7.4981. Files: `Assembly-CSharp`, `Waylith.dll`, `RimWorld.dll`, `Waylith.UnityCompat`, `UnityStubs`, `Waylith.AssetBake`, `Brutal.*`.

**How these notes were made (21 Oct 2026):**
- Read the decompiled code (ILSpy).
- Scanned the IL with Mono.Cecil.
- Cross-checked against our bridge logs.
- Native parts (WaylithCLR, GLFW, the Vulkan loader) were not in the uploaded set. Ghidra 12.1.4 is installed in the cloud workspace (`~/tools/ghidra_12.1.4_PUBLIC`) for when they are.

Facts below come from code. Measured numbers are marked as such.

## 1. Process and threads
- `RimWorld.exe` is a custom .NET 10 runtime (`10.0.13-waylithclr.dev`). Its runtime config sets many `WaylithClr.*` compatibility knobs.
  - Only `Assembly-CSharp` is in `JitPatchableAssemblies` (no inlining), so Harmony patches there are reliable.
  - Methods in `Waylith.dll` / `RimWorld.dll` can be inlined. Patch only large methods there and check that the patch actually runs.
- **Main thread:** `EngineApplication` loop:
  - `PollEvents`
  - `GameUpdate`: `Root_Play.Update`, then `TickManager.TickManagerUpdate`, `Game.UpdatePlay` and IMGUI (Layout + Repaint)
  - `CaptureFrame`
  - `RenderEnqueue`, which waits for the render thread
  - `Throttle`: `Sleep` until 1 ms before the target frame time, then a busy spin; `timeBeginPeriod(1)` is set.
- **Render thread** (`Waylith.Rendering.RenderThread.RenderFrame`) records Vulkan commands for the captured frame: `MeshBatchRenderer` / `VulkanMeshDrawEncoder`. Measured: about 12 ms per frame on the i5-5200U.
- **Job runtime** (`WaylithJobRuntime`): `ProcessorCount - 1` workers, 3 on this laptop. `DOTNET_PROCESSOR_COUNT` changes the count.
- **Long events:** loading runs on a separate thread while the main thread keeps drawing the loading screen at the monitor refresh rate.

## 2. Drawing path and why it is CPU-heavy
- Map: 17x17-cell sections, one draw per visible layer submesh. At the maximum zoom-out (`CameraMapConfig.sizeRange` 11–60) about 96 sections are visible.
- Dynamic things: `DynamicDrawManager` culls in jobs, then `PreDrawThings` and the draw.
- **Descriptor sets:**
  - Material sets are cached (`materialSetCache`, `perFrameMaterialSetCache`, persistent UBOs for fully persistent instanced draws).
  - A material that uses globals re-resolves when `GlobalShaderState.Version` changes. `_Time`, camera position and `_GameTime` change it every frame.
  - Any draw with its own `MaterialPropertyBlock` takes `GetOrCreatePerFrameMaterialSet`: a descriptor-set allocation, a UBO upload and `vkUpdateDescriptorSets`, for every draw in every frame.
  - The Cecil scan found about 30 call sites that draw with property blocks. The per-frame ones:
    - pawn overlays (`PawnOverlayDrawer`)
    - designations
    - map-edge clippers
    - `SectionLayer_Darkness`
    - gas
    - motes (`Graphic_Mote`)
    - flecks (`DrawBatch.Flush`, the non-instanced path)
    - `Graphic_WithPropertyBlock`
  - Measured: about 2 million sets per session, but per-frame figures are still missing. Bridge v13.80 logs `[RW-GPU-SETS]` with the top layouts.
- Per-draw uniforms (`VulkanMeshDrawEncoder.TryUploadUniforms`): a window in a 4 MB per-frame uniform buffer, bound as a plain UBO in a new set. That is why the bridge's content comparison and uniform ring (v13.75) helped so much.
- Index buffers: one per map section. The bridge pools them (v13.74).
- Push constants: a 64-byte model matrix per draw.

## 3. Simulation
- `TickManager.TickManagerUpdate` ticks until real time is caught up, capped at `2 × TickRateMultiplier` ticks or 45.45 ms (the `WorstAllowedFPS = 22` constant).
- Variable tick rates already exist in 1.7. `Thing.DoTick` calls `TickInterval(delta)` at the rate given by `UpdateRateTicks`:
  - camera-dependent: 15 off-screen, zoom+1 on screen;
  - animals: always 15.
- Already job-based or cached in vanilla 1.7, so old mods that patch these are obsolete:
  - glow grid
  - pathfinding (async requests)
  - world reachability
  - `GetComp`
  - DesignationManager
- Still costly:
  - per-pawn job fail checks every tick;
  - room stats re-computed whenever an item or filth spawns;
  - wild plant long ticks;
  - turret target searches every 15 ticks;
  - alerts (spread over 24 frames, but drawn every frame).

## 4. Loading and caches
- **Disk caches** under `CacheDataPath`. The default is `%USERPROFILE%\AppData\LocalLow\Ludeon Studios\RimWorld by Ludeon Studios`; `-savedatafolder=` moves it, saves included.
  - `TextureCache\v23\<tag>` (mod textures and baked static atlas pages)
  - `ShaderCache`
  - `MeshCache\v1`
  - `HardwareProfile\gpu-profile.bin`
- **Not cached:** the `VkPipelineCache` (created empty every run), XML loading, patches, def resolving, static constructors, and the forced GC.
- **GPU probe:** about 1.7 s of benchmarking at every boot. On a slow GPU it never finishes, so nothing gets cached. The local build pre-writes the cache file.
- **Save load:** `Map.FinalizeInit` rebuilds regions, rooms and path costs. `MapDrawer.RegenerateEverythingNow` then rebuilds every layer of every section on the main thread. The local build (I3) defers off-screen sections; vanilla `Section.DrawSection` rebuilds dirty layers when they are first drawn.
- **Autosave:** synchronous, uncompressed, tab-indented XML.

## 5. Switches (verified in code)
- **Engine command line:**
  - `-NoVulkanLayers`
  - `-ColdBoot`
  - `-NoAssetPack`, `-AssetPack <path>`
  - `-NoVulkanFallback`, `-ResetVulkanFallback`
  - `-BundledVulkanLoader` / `-SystemVulkanLoader`
  - `-WindowSize=W,H`
  - `-texCacheCompression=none|lz4|lz4hc|zstd`
  - `-texFormat=bc1|bc4|bc5|bc7|none`
  - `-texQuality=0..1`
  - `-noCompressAtlases`, `-noEvictAtlasSources`
  - `-BakeModAssetPacks=<roots>`
  - `-Verbose`
  - `-MemoryReportAfterBoot`, `-QuitAfterBoot`, `-ScreenshotAfterBoot=<file>`
  - `-NoWindow`
  - `-TrackRehydration`
  - `-EagerTextureUpload` (1.7.4982+)
- **Environment variables:**
  - `WAYLITH_VRAM_BUDGET_MB`
  - `WAYLITH_STAGE_UPLOADS_IN_RAM=1`
  - `WAYLITH_NO_ASSET_PREFETCH=1`
  - `WAYLITH_LOAD_ENCODE=gpu|verify`
- **Built-in diagnostics:**
  - `DebugViewSettings.showFpsCounter`, `showTpsCounter`, `showMemoryInfo`. The TPS shown is theoretical, from the mean tick time.
  - Frame Profiler: Dev mode → Debug actions → Frame profiler. Settings in `Config\Profiler.xml`. The API is `Waylith.Profiling.FrameProfiler`; Dubs Performance Analyzer's 1.7 source is a good reference.
  - Alt+` diagnostics dump.
  - `Prefs.LogVerbose`, which turns on DeepProfiler load timings.

## 6. What this means for us
| Finding | Applied where |
|---|---|
| Sets are created and destroyed constantly | Bridge v13.81: sets are recycled per layout on pool reset or free, and pool free is now O(1). Also fixed an upstream double free (a set freed with `vkFreeDescriptorSets` was deleted again on pool reset). |
| Property-block draws produce a new set per draw | Counter in v13.80; local mod switches E2/E3 (fewer flecks), F5 (mote alpha steps), F6 (fleck instancing) |
| Globals bump versions every frame | Local mod F7 (`RW_LOCAL_GLOBALS_EVERY`) |
| Section regeneration at load and during snow/sand | Local mod I3 (lazy sections), E7 (weather mesh batching) |
| Tick budget and per-tick costs | Local mod A2, F1, I5–I8 |
| UI drawn twice per frame | Local mod C1/C2, E1, F2, F3, H1 |
| GPU probe and loading | Local mod A1, I4, G1 |
| Diagnostics | Local mod I1/I2 (HUD and CSV); bridge `[RW-GPU-SETS]` |

## 7. Linux
The HD 5500 gets Vulkan 1.3 natively on Linux through Mesa's Intel driver for Gen7/Gen8 (hasvk), so no bridge is needed there. RimWorld 1.7 has a Linux build. Our Harmony mod is platform-independent. See `LINUX-PLAN.md` and `local/linux/` (launch script).

## 8. Native layer (added v13.82; game 1.7.4984 rev1242)
- **Files:** `RimWorld.exe` (host), `coreclr.dll` / `clrjit.dll` / `clrgc.dll` / `clrgcexp.dll` (WaylithCLR 10.0.13 fork), `glfw3.dll` (with `glfw3.pdb`, so it has symbols), `vma.dll`, `waylithaudio.dll`, `VulkanEx.dll`, `imgui.dll`, `slang-*.dll`, `Ktx.dll`, `freetype.dll`.
- **WaylithCLR runtimeconfig knobs** (strings in coreclr.dll):
  - `AllowInitOnlyStaticWrites`, `AssemblyNameRedirects`, `CatchableAccessViolations`
  - `JitNoInlinePatchableIntoDynamicMethods`, `JitPatchableAssemblies`
  - `LaxAssemblyArchitecture`, `LaxAssemblyBinding`, `LaxCallvirt`, `LaxInheritedFieldRefs`, `LaxInterfaceDispatch`, `LaxMemberSignatures`, `LaxPrivateFieldNames`, `LaxTypeRefResolution`
  - `MissingAssemblyTypeLoadException`, `SkipAccessChecks`
  - **`SatoriGC`**: Satori's region-based, low-pause GC (`SatoriRecycler` / `SatoriRegion` symbols) is compiled in. It is off in the shipped config. Local switch `RW_LOCAL_SATORI_GC=1`.
- Standard GC config names are also present (`gcServer`, `gcConcurrent`, `GCName`/`GCPath` for a standalone GC such as `clrgc.dll` / `clrgcexp.dll`, `GCRetainVM` and others). The `DOTNET_` environment form should work for these, but that is untested.
- Ghidra 12.1.4 (headless) and radare2 are installed in the cloud workspace for deeper native analysis.
