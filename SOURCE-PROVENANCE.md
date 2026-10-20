# Experimental GPU bridge: source provenance

## Renderer baseline

- Repository: https://github.com/DragonJoker/Ashes
- Commit: `c2127f497ae500929858fe1138421fc15b638309`
- Commit subject: `Updated Vulkan-Headers submodule`
- Author date: `2026-10-13T09:17:35+02:00`
- Source and submodules were fetched at pinned revisions, not from a random DLL site.

| Submodule | Revision |
|---|---|
| CMakeUtils | `469ce45abb94e18a87484e155bce8e88527822a6` |
| AGS SDK | `7d2d1a293dae2aed39af8b1248f6ac27a554d974` |
| Vulkan-Headers | `c46850864f4661461b0f6cb9922c058ffea4915e` |
| gli | `779b99ac6656e4d30c3b24e96e0136a59649a869` |
| glm | `6ad79aae3eb5bf809c30bf1168171e9e55857e45` |
| glslang | `e966816ab28ab7cb448d5b33270b43c941b343d4` |
| imgui | `8555335935d059b2d260b2a449bdf2716d0c2b0f` |
| SPIRV-Cross | `a0fba56c34a6700f1724bf9b751da5b488a3775c` |
| vcpkg | `ce35b1a53aac26d7fcdb8ee1ef7a8e4eea02d27b` |

## V13 renderer build and source

Final DLL SHA-256: `46539af6c13097749e8aaa8e3fa98b56f454bc2e2bbf0f9ddfb42b191605f555`. Windows x64 Release built with the existing Clang/MSVC-compatible CMake/Ninja toolchain. Microsoft SDK/toolchain files are not redistributed.

Apply `src/patches/FINAL-complete-tracked-source.patch` once to the pinned Ashes baseline above. It includes all retained v1–v12 changes and the focused v13 changes. Do not apply the delta on top. The alternative `V13-delta-from-v12.patch` applies only to the exact v12 patched renderer tree, with `patch -p1` from the source-tree root. The delta file list is supplied. Consolidated baseline+patch matches current renderer byte-for-byte, reverse-check succeeds, and the delta dry-applies to the v12 patched tree. See `tests/final-v13/source-patch-check.log`.

Probe C and shader/SPIR-V header sources are included. Runtime integration evidence and native-hardware limits are in `TEST-RESULTS.md`. Probe binaries use MinGW; no compiler is required to run the package. The loader proxy is retained from v12; its source/DEF is included. The stock system Vulkan loader used for sandbox tests is **not shipped**. Windows launch makes a byte-identical local copy of the user's existing System32 loader without changing that system file.

The bridge remains an experimental Vulkan 1.0 portability driver, not native Vulkan support or a conformant complete implementation. Its unresolved semaphore and dynamic-storage scope is explicit in the README. Ashes is MIT; SPIRV-Cross Apache-2.0; custom probes/launcher/proxy MIT; bcdec MIT or Unlicense. Notices are retained under `licenses/`. Decoder source provenance and v12 image limitations remain embedded in the consolidated patch. Final package hashes are in `SHA256SUMS.txt`.
