# v11 D3D11 Image Lifetime Implementation

## Changed renderer scope

Only these renderer sources were changed:

- `/home/ubuntu/rimworld-gpu-d3d11/source-work/source/ashes/renderer/D3D11Renderer/Image/D3D11Image.cpp`
- `/home/ubuntu/rimworld-gpu-d3d11/source-work/source/ashes/renderer/D3D11Renderer/Image/D3D11Image.hpp`

No Blit, DeviceMemory, launcher, package, parent `ObjectMemory` metadata, or memory-pool source was edited.

## Ownership and release design

`Image` now stores the non-owning `std::weak_ptr<uint8_t>` token obtained from `DeviceMemory::getLifetimeToken()` **only after** a successful native image bind. `ObjectMemory` remains parent-owned in `DeviceMemory::m_objects`; Image never erases or deletes it.

When an Image wrapper is destroyed or overwritten through move assignment:

1. It releases `ObjectMemory::resource` with the public, idempotent `ObjectMemory::releaseResource()` only if an `ObjectMemory` pointer exists and the parent-lifetime token has not expired.
2. It then clears the local `ObjectMemory` and D3D resource pointers. This permits the native backing resource to be released early while preserving the parent metadata needed by recorded references and parent shadow loops.
3. If `vkFreeMemory` destroyed the parent wrapper first, the weak token is expired and the potentially stale `ObjectMemory *` is not dereferenced. This is intentionally a no-op release path.
4. Borrowed DXGI/swapchain images have no `ObjectMemory`, so they are never released by this mechanism.

`Image::isMapped()` now returns `false` rather than dereferencing the native parent when the image is unbound, lacks metadata, or its parent lifetime token has expired. This covers the valid unused-image ordering in which memory is freed before the Image wrapper is destroyed.

The move constructor and move assignment now transfer the memory handle, offset, `ObjectMemory *`, weak lifetime token, debug metadata, resource union, and requirements. Move assignment first releases its old live owned binding. Both moved-from objects clear binding metadata, pointers, token, debug name, and requirements so their destructors cannot release the transferred resource.

`bindMemory()` uses local binding state and commits `m_memory`, offset, object metadata, resource pointer, and token only after the selected `bindToImage*` call returns success and supplies a resource. It preserves actual bind failure results. Vulkan requires a valid `VkDeviceMemory` for a bind call; no wrapper can safely recover a raw handle that the caller has already freed before calling `vkBindImageMemory`.

Image create-info, logical extent, BC native mip-prefix/subresource behavior, image feature checks, and normal bind error propagation remain unchanged.

## Optional diagnostic marker

Set `RW_HD5500_IMAGE_LIFETIME_TRACE=1` to emit up to 4096 process-wide lines:

```text
[RW-HD5500-IMAGE-LIFETIME] IMAGE_RESOURCE_RELEASE
```

The marker is emitted only in the Image release helper, only for an owned, still-populated resource, and immediately before calling `releaseResource()`. It demonstrates that the new early-release branch ran; it is **not** a claim that all COM references or opaque GPU allocation bytes were freed.

## Independent raw Vulkan probe

Created and built:

- Source: `/home/ubuntu/rimworld-gpu-d3d11-v11/image-lifetime-probe/ImagePoolLifetimeProbe.c`
- Build recipe: `/home/ubuntu/rimworld-gpu-d3d11-v11/image-lifetime-probe/build.sh`
- Executable: `/home/ubuntu/rimworld-gpu-d3d11-v11/image-lifetime-probe/ImagePoolLifetimeProbe.exe`
- Instructions/scope: `/home/ubuntu/rimworld-gpu-d3d11-v11/image-lifetime-probe/README.md`

The PE is a raw `VK_NO_PROTOTYPES` program that dynamically loads a caller-specified absolute loader, validates all dispatch functions, and imports no Vulkan DLL. It requires a non-software Intel adapter by default; `--allow-non-intel-test` is a clearly labelled non-hardware override. Both `--loader` and `--icd` must be supplied as absolute Windows paths.

Its intended API coverage is:

- one retained 4 MiB device-local parent allocation;
- six sampled RGBA8 2D image/view cycles at one fixed nonzero aligned offset;
- image-view destruction before the corresponding image in every cycle, with the parent retained through all cycles;
- a host-visible/coherent sampled/transfer image with real staging upload, buffer-to-image copy, image-to-buffer copy, host invalidate, and exact 16-byte pixel comparison;
- an unused bound image parent-free-first control (`vkFreeMemory`, then `vkDestroyImage`) with no GPU use.

## Validation performed

- `git diff --check` passed for the two assigned renderer files.
- `ImagePoolLifetimeProbe.c` compiled cleanly through the supplied independent static-MinGW recipe using `/home/ubuntu/rimworld-vulkan-bridge/build-include` and `-Wall -Wextra -Werror`.
- The executable is a PE32+ Windows x64 console binary.
- `x86_64-w64-mingw32-objdump -p` reported only `KERNEL32.dll` and `msvcrt.dll`; no Vulkan loader import was present.
- No Wine execution and no full renderer build were run.

## Limits

The renderer implementation relies on the normal Vulkan external-synchronization/lifetime contract: it does not attempt to make concurrent parent destruction and Image destruction safe. The early release occurs only while the parent token reports live. Callers must not destroy an image whose recorded GPU work is still pending.

The raw probe was **built but not run**, so it provides no hardware or integration result. Even if run successfully, public Vulkan cannot reveal D3D11 COM reference counts or exact native GPU-memory reclamation; the probe can prove only the requested Vulkan API sequence and byte-level copy/readback result.
