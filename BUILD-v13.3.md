# v13.3 build record

Built 2026-10-18 in a Claude (Cowork) Linux sandbox. Renderer only; every other runtime file is unchanged from v13.2.

| File | SHA-256 |
|---|---|
| `runtime/ashesD3D11Renderer.dll` (v13.3) | `7810619D7BD9A8480A42842DB244FEE5192E6B018894F5AF8A64BCA39547653C` |
| `runtime/ashesD3D11Renderer.dll` (v13.2, rollback) | `37CC03FA6D0D27140EF574BD0E510736301308A167C1B05163E4AFCC40E2A5D2` |

`Launch-HD5500-GPU.ps1` line 252 now expects the v13.3 hash. `SHA256SUMS.txt` is regenerated.

## Source

1. Ashes `c2127f497ae500929858fe1138421fc15b638309` with submodules CMake `469ce45`, Vulkan-Headers `c468508`, SPIRV-Cross `a0fba56`.
2. `source/patches/FINAL-complete-tracked-source.patch` applied. The resulting `D3D11Renderer` tree is byte-identical to `source/v13-renderer-source/D3D11Renderer` from v13.2 (`diff -r` is empty).
3. `source/patches/V13.3-wsi-trace.patch` applied with `patch -p1` from **inside** `source/ashes/renderer/D3D11Renderer` (its paths are `Core/...` and `ash_direct3d11.cpp`). The result matches the `patched-files/` copies byte for byte.
4. Build-only tweak, not part of the renderer code: in `source/CMakeLists.txt`, the six `spirv_cross_*_DIR` entries under `SPIRV_ROOT_DIR` point to `share/<pkg>/cmake`, because that is where the pinned SPIRV-Cross installs its config files.

## Toolchain

- Ubuntu 24.04, clang-cl / lld-link 18.1.3, CMake + Ninja
- MSVC 14.40.33807 and Windows SDK 10.0.22621, from the official Microsoft packages via msvc-wine `vsdownload.py` (build-only; not redistributed)
- SPIRV-Cross is a static build with the options from `BUILD-D3D11-EXPERIMENT.md`.
- Ashes is configured with exactly the options in `BUILD-D3D11-EXPERIMENT.md`, plus `-DCMAKE_POLICY_DEFAULT_CMP0091=NEW` on the command line. Ashes declares `cmake_minimum_required(3.13)`, so without this flag the static-CRT request is ignored and the DLL links the dynamic VC++ runtime (MSVCP140/VCRUNTIME140).

## Checks done

- The compile has 0 errors. The only warnings in the patched files are pre-existing style warnings.
- PE32+ AMD64. Imports are D3DCOMPILER_47, KERNEL32, USER32, d3d11 and dxgi, the same set as v13.2, with no VC++ runtime dependency.
- The export table is identical to the v13.2 DLL.
- The binary contains all six `RW_HD5500_WSI` categories: caps, create, images, acquire, present and destroy.
- `ashesD3D11Renderer_icd.json` is unchanged (API 1.0.0).
- The DLL was not run in the sandbox. The first laptop run is the real runtime test.
