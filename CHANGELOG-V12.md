# v12 changes and implementation scope

## 1. Representation-safe large odd BC textures

`D3D11BcLayout.hpp` selects native-prefixed, decoded RGBA8 or unsupported storage. Creation, upload and image views use the same selection. The oversized 5333×3000 BC7 background no longer requires a 21332-wide native texture. The bcdec decoder is vendored with its licence. Decoded uploads use bounded strips. Unsupported copy/readback combinations fail at command recording, not by silently copying incompatible bytes. Decoded-to-decoded copies preserve sampled values, not a recoverable compressed block stream.

## 2. Exact menu-background regression

`LargeBC7MenuBackgroundProbe.exe` covers 5333×3000 with 13 mips, exact sampled UNORM pixels at mips 0/1/9/11/12, quadrant/padded uploads, a second image and decoded copy, deterministic compressed readback refusal, native-to-decoded boundaries, tail mips and sRGB conversion. The full default run passes 38 cases; serial passes 17, no-fallback passes 5 and the RGBA8 control passes 7.

## 3. Truthful adapter memory reporting

Physical-device heaps derive from the selected DXGI adapter description, rather than `~0ULL`. DXGI 1.4 budget/usage queries are labelled with their source and unavailable state. UMA heap selection is a documented heuristic, not an exact residency or allocatable-budget promise. Native texture/backing byte estimates join the existing buffer counters. `VK_EXT_memory_budget` is not exposed.

## 4. Bounded reuse and shader caching

Staging images use an exact composite key, mutex protection, an LRU byte bound and shared ownership retained by recorded commands. Resources referenced by commands are not evicted as dangling objects. The 256 MiB bound covers cache retention, not every outstanding command-owned resource. `RW_HD5500_STAGING_CACHE_MB=0` disables cache retention.

Compiled D3D shader bytecode is cached by exact source, entry point, target and flags, with an LRU byte cap (32 MiB default); `RW_HD5500_SHADER_CACHE=0` disables it. There is no disk cache or new general VkPipelineCache persistence. Existing immutable blit-pipeline/sampler reuse is retained. This implements the safe staging/compiled-shader slice of the plan, not universal pipeline caching. Sandbox runs demonstrate reuse, not a promised game FPS increase.

## 5. Rendering-error detection and bounded diagnostics

Representation choices and rejections carry logical/native dimensions, format, mips, usage, byte estimates, signature and repeat count. Reports are deduplicated and bounded. The launcher groups repeated fresh engine exception signatures and flags Vulkan error names or rejected operations even when the native process returns 0. `v12-classification.txt` distinguishes process exit from render-error evidence. Synthetic classifier tests cover a repeated OnGUI/format-error pattern, clean logs and a native representation rejection.

## 6. Conservative per-context state cache

Opt-in caching covers sampler, SRV and constant-buffer setters by actual stage/slot/pointer, including first-constant/count for ranged bindings. Invalidation is wired to execution, pipeline/push-constant changes, descriptor cleanup and render-target transitions. It remains off by default (`RW_HD5500_STATE_CACHE=1` enables). The 18 retained normal-mode regressions also pass with it enabled. The specific descriptor fixture still fails on both v11 and v12; skipped-setter/performance benefit is not established.

## 7. Resource-hazard tracking

Native resource identity and mip/array ranges identify overlapping SRV/UAV/RTV/DSV bindings. Stale overlapping compute UAV slots are explicitly unbound before SRV binding and caches invalidated. Active RTV/DSV conflicts are diagnosed, not universally resolved. Sparse OM UAV arrays now include explicit null holes. Default is on; `-NoHazardTracking` supplies a reversible A/B control. The dedicated four-case probe remains non-passing identically on v11 and v12, so this is implemented but not specifically validated as a fix.

## 8. Real submission fence completion

Queue submission attaches an EVENT query to a supplied fence. Status, reset, wait-any/all and timeouts observe native completion instead of the previous success stubs. Waits use bounded sleeping while respecting Vulkan wait semantics; GPU work is flushed when needed for progress, not universally checkpointed after every command. Query ownership persists through pending work. `RW_HD5500_FENCE_EVENTS=0` is the slower completion fallback. All three fence probe modes pass.

## 9. Query handling and delayed timing

Query result handling respects WAIT and availability and returns NOT_READY for pending non-WAIT results without fabricating completed values. Timestamp period derives from a native TIMESTAMP_DISJOINT measurement when available, with explicitly labelled fallback. Optional `-GpuTiming` enables a delayed timestamp/disjoint ring and prints raw diagnostic summary values without a per-sample synchronous spin. It is off by default, and its sandbox timings are not HD 5500/game FPS.

## 10. Capability/query-create consistency

Queries use the actual adapter and checked native format-support results. BC storage/attachment claims are withdrawn, BC samples are limited to one and oversized extension is representation-aware. A native trial creation prevents advertising image formats whose mipped SRV/RTV backing fails; unusable surface formats are filtered before surface usage intersection. The default matrix passes **1054 queries / 842 creations / zero mismatches** on the synthetic adapter; no-decode passes **1054 / 840 / zero**. Legacy mode deliberately restores overclaims for comparison and records 42 informational mismatches.

## Unresolved limits

The persistent-mapping control fails on v11 and v12: HOST_COHERENT writes kept mapped across submission can remain invisible; the strict new probes flush/unmap uploads before submission. Dynamic UBO and other descriptor cases in the dedicated hazard fixture fail on the old and new renderer. These are not claimed fixed or erased by classifying their probes informational. All ten areas are represented in source, but this release is not a complete Vulkan bridge or a native-laptop-validated finished product.
