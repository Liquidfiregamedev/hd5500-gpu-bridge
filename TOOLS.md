# Diagnostic tool set (v13.45)

Two places: the **laptop** (Lenovo Z70-80, HD 5500, where the game runs) and the **cloud workspace** (Claude's Linux machine, where the bridge is built and the emulator suite runs). `Install-Tools.ps1` (Bridge menu → "Install diagnostic tools") installs the laptop set through winget; each tool is optional and independent.

## What each tool sees, and what we use it for

| Tool | Where | Sees | Our use |
|---|---|---|---|
| **Bridge frame diagnostics** (`-FrameDiag`, `RW_HD5500_FRAME_DIAG`) | laptop, emulator | every draw the bridge executes: shaders, textures, blend/depth state, vertex data, dumps of render targets | first look at any visual bug; the `drawlist`/`atlas-draw`/`vtx-compare` lines in `game.stderr.txt` |
| **`[RW-GPU-PROF]` / `[RW-GPU-STATECACHE]`** | laptop | per command buffer: upload, bind and draw time; cache hit rate | performance rounds |
| **Microsoft D3D11 debug layer** (`-DebugLayer`, needs "Graphics Tools") | laptop | the D3D11 runtime's complaints about the bridge's calls | rejected calls, register mismatches |
| **Laptop self-test** (`Laptop-Selftest.ps1`) | laptop | 22 smoke tests on the HD 5500, on WARP and (water) against lavapipe | regression check for every build |
| **WARP** (Microsoft software rasterizer) | laptop | the same D3D11 stream on a reference rasterizer | driver bug vs bridge bug |
| **Mesa lavapipe** (`-Reference`, `reference\lavapipe`) | laptop, emulator | real Vulkan on the CPU, no bridge | what correct output looks like |
| **RenderDoc** | laptop | one captured frame: the game's Vulkan calls *and* the bridge's D3D11 calls, every texture and shader, pixel history | compare a wrong draw call by call instead of through dump files |
| **PIX for Windows** | laptop | D3D11 capture with GPU timings, per-pixel shader debugging | overlaps RenderDoc; better timing on Windows |
| **Intel Graphics Performance Analyzers** | laptop | the HD 5500's own counters: per-draw GPU time, shader busy, bandwidth | find which draws cost the GPU most |
| **GPUView / Windows Performance Analyzer** (Windows Performance Toolkit) | laptop | ETW timeline: CPU threads, GPU queue, driver, DWM, presents | explain a freeze: who waited on what |
| **PresentMon** | laptop | a CSV of every Present(): frame time, GPU busy, display latency | objective stutter numbers per build |
| **HWiNFO** | laptop | clocks, temperatures, power limits over time | thermal throttling of the laptop during a test |
| **Vulkan SDK**: vkconfig, validation layers, GFXReconstruct, Hardware Capability Viewer | laptop | vkconfig switches layers per game; validation reports the *game's* protocol mistakes; GFXReconstruct records the game's whole Vulkan stream; the Capability Viewer lists what our ICD advertises next to a real driver | separate engine bugs from bridge bugs; record once, replay in the cloud |
| **GFXReconstruct replay** (`gfxrecon-replay`) | cloud | replays a laptop capture against the bridge or lavapipe | test the bridge with real game traffic without the laptop |
| **apitrace** | cloud (and laptop build) | records/replays D3D11 or Vulkan call streams | second opinion on what the bridge emits |
| **Vulkan Configurator / vulkaninfo** | both | layer setup; the ICD's capability report | `vulkaninfo` on the bridge ICD before a "report 1.2" build |
| **Fossilize** (`VK_LAYER_VALVE_steam_fossilize`, ships with Steam) | laptop | records every pipeline the game creates; replays them to warm the shader cache | shader-compile stutter: pre-build the bridge's HLSL cache before play |
| **dotnet-trace / dotnet-counters / dotnet-dump** | both | the .NET side of Waylith: GC pauses, JIT, lock waits, a hang's stacks | freezes that are not GPU at all |
| **Dependencies (lucasg)** | laptop | DLL resolution of RimWorld.exe / the bridge | loader stage problems, wrong Vulkan loader picked |
| **Process Monitor** | laptop | file and registry activity | map-load freeze: is it disk I/O? |
| **Process Explorer / Task Manager GPU tab** | laptop | per-process VRAM (shared memory on the HD 5500) | the VRAM growth seen on Discord |
| **ILSpy (`ilspycmd`)**, **dnSpyEx** | cloud, laptop | the game's and Waylith's C# code; dnSpy can set breakpoints in the running game | how the game draws a thing (water, overlays, instancing) |
| **REA 6.7.0** | cloud | member inventories, decompiled functions, evidence bundles over ILSpy/Rizin | structured reverse engineering of Waylith.dll |
| **Rizin / Cutter, Ghidra** | cloud | native code: Vulkan loader, Intel driver DLLs | only if a native D3D11/driver call misbehaves |
| **SPIRV-Cross, glslang** | cloud | shader translation by hand | the water test; check what the bridge's HLSL should look like |
| **Vulkan CTS** | cloud | the official conformance tests for the features we claim | the gate before reporting Vulkan 1.2 |
| **Game frame profiler** (dev mode → debug list → "frame profiler"; instrumentation profiling for mods) and **Alt+`** diagnostics dump (`Crashes\diagnostics-*.txt`, includes the VRAM profiler) | laptop | the game's own view of frame time and VRAM | cross-check our numbers; the dump goes into SUMMARY.txt from v13.45 |
| **Dubs Performance Analyzer** (mod, when a 1.7 build exists) | laptop | per-mod, per-system tick cost | separate game-logic slowness from rendering |

## Recipes
- **A wrong picture**: Map test (frame dumps) → if unclear, RenderDoc capture of one frame (laptop) → compare with the same frame on lavapipe. GFXReconstruct capture if the bug needs many iterations: replay in the cloud.
- **A freeze**: PresentMon log + `[RW-GPU-PROF]` first; if the slow frames are not in the bridge's own numbers, GPUView trace (who waited) or dotnet-trace (GC pause); HWiNFO alongside to rule out throttling.
- **A crash / "device lost"**: D3D11 debug layer on, Dependencies for the loader path, Vulkan validation layer via vkconfig to see whether the game's own call sequence is at fault (compare with the Discord validation log in `DiscordLogs\`).
- **Before a capability change** (reporting Vulkan 1.1/1.2): `vulkaninfo` on the bridge ICD, Vulkan Hardware Capability Viewer, then the self-test and the CTS subset.

## Cloud workspace status (2026-10-19)
Installed: vulkan-tools (vulkaninfo), apitrace 11.1, GFXReconstruct 1.0.2 (`gfxrecon-replay`, `gfxrecon-convert`, capture layer), spirv-cross, glslangValidator, Khronos validation layer, Mesa overlay layer, dotnet-trace/counters/dump 10.0, ilspycmd, REA 6.7.0, Windows lavapipe + Wine for the suite. Not installed in the cloud: Ghidra, Vulkan CTS (large; on demand).
