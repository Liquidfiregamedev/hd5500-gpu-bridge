# Vulkan 1.0 GPU probe

Run `Check-GPU-Bridge.cmd` normally on Windows 10 x64. Do not run as Administrator or manually register an ICD.

The native probe uses `--loader "full path" --icd "full manifest path"`. Its environment is process-local. It requests Vulkan API 1.0, explicitly enables the advertised portability-enumeration extension/flag and mandatory advertised portability-subset device extension, then requires an Intel, non-CPU/non-WARP candidate. Reported adapter identity is checked; actual laptop results must still be assessed.

It validates storage-buffer descriptors, shader translation, compute execution and exact readback `0x00005500`. It then prefers an **advertised** conventional B8G8R8A8_UNORM or R8G8B8A8_UNORM surface format, requests a one-image FIFO swapchain matching the driver's limits, and records a clear/submit/present sequence. Errors are traced to their operation and returned as a nonzero exit code. A successful API sequence is not a full game or screenshot-quality test.

For the second compatibility-proxy preflight, the launcher adds `--no-portability-optin`. That diagnostic switch causes the application to omit the portability extensions/flag; the proxy must add the advertised opt-in while preserving the original API and feature requests.

`--allow-non-intel-test` exists only for non-production integration testing. In the known sandbox environment, Wine exposes a synthetic NVIDIA adapter backed by software, despite its reported adapter type/name. **The user-facing launcher does not use the override.** Do not use it to claim Intel hardware acceleration.

## Build/audit

The source and embedded shader are under `src/`. `src/build.sh` cross-compiles with MinGW and regenerates/validates SPIR-V 1.0 with `glslangValidator` and `spirv-val`. Its include-path default refers to the build workspace; adjust it to your installed Vulkan headers if rebuilding elsewhere. The executable does not link a Vulkan import library or depend on an external shader file at runtime.

## Actual test status

Final standard-loader and proxy integration runs both completed with exit code 0, exact compute readback and successful Win32 WSI API presentation. See the package-root `TEST-RESULTS.md` and included logs. **These were Wine/software-backed integration tests, not actual HD 5500 or RimWorld/FPS tests.**

The game is not started in check-only mode, and no game files are changed. Upload the newest generated report ZIP for interpretation. Actual full-game compatibility and frame rate remain untested.
