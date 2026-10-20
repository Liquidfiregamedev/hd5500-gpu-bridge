# v10 standalone checks

The normal `Check-GPU-Bridge.cmd` configures and runs these probes automatically. Both executables dynamically load the exact supplied Vulkan loader and driver manifest and have no Vulkan import-library dependency. Their default selects a non-software Intel adapter. `--allow-non-intel-test` is a sandbox integration option only and is never passed by the shipped launcher.

## AtlasBlitProbe.exe

```bat
AtlasBlitProbe.exe --loader "C:\absolute\vulkan-1.dll" --icd "C:\absolute\ashesD3D11Renderer_icd.json" --iterations 3
```

The loader can be the existing Windows loader or the configured compatibility proxy. The proxy requires its local byte-identical `rw_system_vulkan.dll` backend; the launcher prepares it safely.

Options:

- `--source-needs-temp`: submit the original source without sampled usage, exercising the driver's real temporary source and destination branches.
- `--destination-direct`: use a storage-capable original destination, avoiding a destination temporary.
- `--clone-secondary`: record a real secondary atlas batch, execute it through a primary command buffer and verify shared ownership during reset/cleanup.
- `--bc7-source`: upload valid solid-color BC7 mode-6 blocks; enable queried BC support; perform real GPU BC7 sampling into RGBA8 and compare exact decoded pixels.
- `--iterations N`: 1–8; the launcher requires 3, giving nine cases per mode.

All cases use actual `vkCmdBlitImage` and complete 32×32 destination readback. Three odd/scaled nonzero-origin rectangles must match their target pixels and every unchanged outside sentinel. No comparison tolerance or synthetic target pixels are used. Pool reset must retain an allocated command-buffer handle that can begin/end again. Final exit zero and all nine pixel/case/lifetime markers are required by the launcher.

The source has a Vulkan 1.0 API request, explicit supplied-loader discovery and advertised portability negotiation. Its Windows entry point supports Unicode file paths.

## FailurePathProbe.exe

```bat
FailurePathProbe.exe --loader "C:\absolute\vulkan-1.dll" --icd "C:\absolute\ashesD3D11Renderer_icd.json"
```

Three conditions must pass: a **valid callback function returning null** yields OUT_OF_HOST_MEMORY and a null image; a deliberately invalid null-image view request frees custom constructor storage with balanced counters; a fresh valid image/memory/view sequence succeeds afterward. The invalid request is a defensive negative control, not valid-input conformance. This does not reset the GPU or establish game-level recovery after an actual driver failure.

Successful markers:

- `PASS: allocation refusal preserved`
- `PASS: failed constructor storage freed`
- `PASS: healthy resource after failures`
- `FINAL: FAILURE-PATH PASS`

Sources are in `src/atlas_blit_probe.c` and `src/failure_probe.c`. `Build-V10-Checks.cmd` is optional and source-reviewed, not tested on native Windows; prebuilt x64 binaries are included. Runtime integration evidence is under `tests/final-v10/`, and its Wine/synthetic-adapter boundary is explicit in `TEST-RESULTS.md`.
