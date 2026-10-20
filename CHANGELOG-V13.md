# Focused v13 changes

V12's 5333×3000 BC7 background path is unchanged. The v12 laptop report proved that background uploads and 38 strict test cases succeed on Intel, but did not prove the UI draw path worked.

## Actual source changes

- `UploadMemoryCommand`: guarded execution-time mapped-input refresh and weak parent allocation lifetime. Current map state is checked on execution, not used to decide whether the resource-use command exists.
- `CommandBuffer`: track the persistent current IBO; refresh index input before indexed draws; record VBO/descriptor/transfer/indirect input refresh independently of record-time map state. Correct actual descriptor-set ordinal and dynamic-offset partition. Constructor/recording failures become recording results, not escaped C++ exceptions.
- `BindDescriptorSetCommand` / `DescriptorSet` / `PipelineLayout`: explicit set indices through bind/unbind/gather, duplicate-layout-safe binding, dynamic UBO per-array-element offsets and bounds, proper byte-to-constant units. Dynamic UBOs are no longer treated as UAVs during command construction. Dynamic storage ranges are refused with FEATURE_NOT_PRESENT; the exposed dynamic-storage limit is zero.
- `DrawIndexedCommand`: optional sparse actual IBO/draw diagnostics. Zero-instance and nonzero-base-instance cases use the instanced native draw instead of accidentally drawing once/ignoring the base instance.
- `PhysicalDevice`: typed-UAV creation validation for storage formats; no depth storage claims; single-mip X8_D24 restriction and single-sample storage images. No native Intel v13 capability pass is asserted yet.
- `DebugReportCallback` / `SwapChain`: SUCCEEDED classification of positive HRESULT statuses, actual HRESULT text, device-loss-aware failed-Present result mapping. Optional read-only frame-copy descriptors/readback hashes, first3 then at most twelve captures spaced10seconds. Probe-only waits are bounded; no new normal-frame waits/flushes.
- Launcher: selected key checks by default, opt-in broad/stress checks, new indexed UI preflight, combined stdout/stderr mismatch counting, `Diagnose-UI-Frame.cmd` for practical native testing.

## Not included

No new BC representation, FPS promise, broad caching work, driver setting, system loader replacement, mod/save change or semaphore implementation. Existing caches and lifetime repairs remain. The next native run is needed to establish the actual black-screen outcome.
