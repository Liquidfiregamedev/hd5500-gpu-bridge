# v13.3 — frame-probe fix + swapchain lifecycle trace

Base: `HD5500-GPU-Bridge-v13.2-Focused-Windows10/package/source/v13-renderer-source/D3D11Renderer`
(renderer SHA-256 of the v13.2 DLL: `37CC03FA…E2A5D2`). Diagnostic-only change; no rendering behaviour is altered.

## Why

The 2026-10-18 10:41 laptop run (`gpu-run-20261018-104131-756`) showed:

1. **The frame probe never captured anything.** All 95 captures report `copy{armed=0}` with
   `fakeCreateHr=0x80070057`. The Vulkan-visible swapchain image is `DXGI_FORMAT_B8G8R8A8_TYPELESS` (90),
   which `getPixelBytes()` did not know, so the staging readback was refused. `isCopyCompatible()` also
   required identical formats, so TYPELESS→UNORM (87) always read as incompatible even though
   `CopySubresourceRegion` allows it.
2. **The swapchain is rebuilt continuously.** `RW_HD5500_FRAME_PROBE enabled` (printed once per swapchain
   creation) appears 97 times, every capture is `frame=1`, and frame 2 never appears although frames 1–3
   are always captured. Each swapchain therefore presented once and was destroyed. The trigger is unknown.

## Changes

`Core/D3D11SwapChain.cpp`
- `getPixelBytes()`: adds the R8G8B8A8 / B8G8R8A8 / B8G8R8X8 / R10G10B10A2 / R16G16B16A16 TYPELESS formats.
- New `getCopyFamily()`; `isCopyCompatible()` compares typeless families instead of exact formats.
- `acquireNextImage()` counts calls.

`Core/D3D11SwapChain.hpp`
- Read-only accessors: present count, acquire count, lifetime (ms), window/wrapper extents.

`ash_direct3d11.cpp` — always-on, bounded (≤600 lines per category) `RW_HD5500_WSI` lines on `std::clog`:
- `caps` — `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`: first 5 calls and every change (image count range,
  current/min/max extent, usage).
- `create` — every `vkCreateSwapchainKHR`: sequence no., result, handle, oldSwapchain, requested
  minImageCount/extent/format/colorSpace/usage/presentMode/alpha/transform/clipped, plus the bridge's own
  image count and window/wrapper extents.
- `images` — `vkGetSwapchainImagesKHR`: caller capacity vs. returned count.
- `acquire` — first 3 acquires per swapchain: timeout, semaphore/fence present, result.
- `present` — first 3 presents per swapchain and any non-success: result, wait-semaphore count,
  per-swapchain image index / present number / result.
- `destroy` — every `vkDestroySwapchainKHR`: lifetime, acquires, presents, caps calls so far.

## Build

Apply to a clean v13.2 renderer tree (or copy `patched-files/` over it):

    cd <...>/v13-renderer-source
    patch -p1 --dry-run < v13.3-wsi-trace.patch && patch -p1 < v13.3-wsi-trace.patch

Then build `ashesD3D11Renderer.dll` exactly as for v13.2 (same MinGW/Ashes setup). The helper block was
compile-tested in isolation with g++ -Wall; the full DLL was **not** built here (no MinGW/Ashes SDK in this
sandbox), so treat the first build as the real compile check.

## Launcher hash guard

`Launch-HD5500-GPU.ps1` line ~252 refuses any renderer whose SHA-256 is not the v13.2 value. After building:

    (Get-FileHash .\package\runtime\ashesD3D11Renderer.dll -Algorithm SHA256).Hash

and put that value in place of `37CC03FA6D0D27140EF574BD0E510736301308A167C1B05163E4AFCC40E2A5D2`
(also update `SHA256SUMS.txt`). Keep v13.2 as rollback.

## Test

Run `Diagnose-UI-Frame.cmd` as before (FrameProbe + UiTrace). Leave the game ~1 minute, close normally,
upload the newest `gpu-run-*.zip`.

What to read in `game.stderr.txt`:

    findstr "RW_HD5500_WSI" game.stderr.txt
    findstr "RW_HD5500_FRAME_PROBE frame=" game.stderr.txt

- The `destroy` lines say how long each swapchain lived; the line *before* each `destroy`/`create` pair
  (a `caps changed=1`, a non-zero `present result=`, or nothing) points at the trigger.
- `create minImageCount=N` vs. `bridgeImages=1`, and `images callerCapacity` vs. `returned`, show whether the
  single-image swapchain is what the engine rejects.
- With the probe fixed, `armed=1` frames now carry pre/post colour hashes and `nonzeroBytes`: all-zero in
  the fake image means the game draws nothing; non-zero fake but zero real means the copy/present drops it.
