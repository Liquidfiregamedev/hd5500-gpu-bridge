# v13.4: where does the menu go? (frame-composition diagnostics + swapchain A/B)

Base: v13.3. Renderer SHA-256: `1C3C11C1509955B35563CA522A64AA1FA163C9E142FC882F1911A906CA7A1429`.
Rollback: keep the v13.3 folder (`7810619D…47653C`).

## What v13.3 showed (2026-10-18 11:50 run, no mods active)

- **The image the game presents is solid opaque black.** In all 171 frames, every one of the 25 sample points read `00 00 00 FF`, in both the Vulkan-visible image and the real backbuffer. The bridge's copy and present work. Music played, so the game was at the main menu, and the full-screen menu background never reached the swapchain image.
- **The swapchain is recreated after every present.** Each one got exactly 1 acquire + 1 present (both VK_SUCCESS), with no caps change. This holds the game to about 3 fps. The bridge offered only 1 image.

## What v13.4 adds

**1. Frame-composition trace** (`RW_HD5500_FRAME_DIAG=1`, set with `-FrameDiag`, game process only). It covers frames 1, 2, 3, 5, 10, 20, 30, 60, 100, 150, 200, 300, 450, 600, 900, 1200, 1800, 2400 and 3600. Lines start with `RW_HD5500_FRAME_DIAG` in `game.stderr.txt`:

- `pass`: each render pass, marked SWAP or offscreen, with attachment formats and sizes, loadOp and clear colour.
- `draw-swap`: each draw into the swapchain image, with the full D3D11 state at draw time: render target, depth view, viewport, scissor, rasterizer (cull/scissor/clip), blend (enable/factors/**write mask**), depth-stencil, which shaders are bound, and the pixel-shader textures (format, size, mips).
- `draw-offscreen`: the first 8 off-screen draws of each traced frame, with the same state.
- `clear-swap` / `copy-swap` / `blit-to-swap-recorded`: other writes into the swapchain image.
- `frame-summary`: totals per frame (draws, swap draws, passes, clears, copies, image index).

**2. Frame dumps.** On frames 3, 10, 30, 100, 200, 450, 900, 1800 and 3600, BMPs (max 640 px wide) are written to `gpu-run-…\frames\`, together with full-frame statistics (non-black pixel %, bounding box, alpha range, mean colour):

- `frameNNNNN-swapchain.bmp`: what the game presented.
- `frameNNNNN-swapdraw-srv0.bmp` / `-srv1`: the textures sampled by the **last draw into the swapchain**. If the game composites an off-screen image, this is that image.
- `frameNNNNN-last-offscreen-rt.bmp`: the render target of the last off-screen draw.

**3. Frame-probe schedule is now global.** The probe captures the first 3 presents of the process, then one every 10 s. It no longer captures every frame when swapchains are recreated each frame, which removes a per-frame stall.

**4. A/B switch: 3 swapchain images** (`RW_HD5500_SWAPCHAIN_IMAGES=1..3`, set with `-SwapchainImages N`). It is off by default; with N=1 the behaviour is unchanged. With N>1 the surface reports `maxImageCount=N`, the swapchain creates up to N Vulkan-visible images, acquire cycles through them, and present copies the presented index. The trace (`WSI create`/`destroy`) shows whether the recreation loop stops.

## How to test

Run both scripts, one after the other. Each run: start it, wait until the music plays (main menu), leave it for about 1 minute, close the game normally, then upload the newest `gpu-run-*.zip`.

1. `Diagnose-UI-Frame.cmd`: default 1 image, with the frame trace and dumps.
2. `Diagnose-UI-Frame-3Images.cmd`: same, with 3 swapchain images.

## Tested here

- Compiles with clang-cl/MSVC 14.40/SDK 22621 using a static CRT. Imports are unchanged (d3d11, dxgi, D3DCOMPILER_47, kernel32, user32), and so are the exports.
- Smoke-tested under Wine 9 (wined3d on llvmpipe) with a direct-ICD test program (12 frames each):
  - Clear red + triangle straight into the swapchain: correct pixels in the dump. `pass`/`draw-swap`/`frame-summary`/`dump` lines are correct.
  - Off-screen R8G8B8A8 pass + full-screen sampled composite into the swapchain: the off-screen draw, the composite draw with its sampled texture, and all three dumps are correct.
  - `RW_HD5500_SWAPCHAIN_IMAGES=3`: caps report 1..3, the app got 2 images, and presents alternate 0/1 correctly.
- `Launch-HD5500-GPU.ps1` parses with PowerShell 7 (0 errors). Not run on Windows here.
- Not tested on the HD 5500 itself or with RimWorld. The laptop run is the real test.
