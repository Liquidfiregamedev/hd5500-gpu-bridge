# v13 final test results

## Release identity

- Renderer DLL SHA-256: `46539AF6C13097749E8AAE8F3FA98B56F454BC2E2BBF0F9DDFB42B191605F555`
- Target: Windows 10 x64, Intel HD Graphics 5500, RimWorld 1.7/Rocketwerkz-Waylith path
- Build environment: MinGW/Wine/Xvfb sandbox. The sandbox is not an Intel HD 5500 laptop and does not prove native Windows game success.

## Passed in the sandbox

The retained normal regression suite completed successfully, including stock/proxy compute-present, staging, RGBA and host-visible texture upload, BC7 mip/source paths, compact buffers, suballocated compute, atlas blits, failure cleanup and pooled lifetime. The exact runner output is in `tests/final-v13/key-regressions.log`.

The exact 5333x3000 BC7, 13-mip menu-background suite passed in normal mode. The persistent-upload control, fence/query probe, and frame-probe execution path also returned exit code 0. The large BC7 path uses decoded RGBA8 backing only for the bounded decodable oversized representation; unsupported semantic cases are refused rather than silently treated as BC data.

The capability correction and integrated descriptor changes compiled. PowerShell parsing of the launcher reported zero syntax errors. The consolidated source patch and v12 delta both passed their dry-apply/reverse checks.

## Deliberately failing diagnostic

`UiIndexedDrawProbe.exe` is included as a strict reproduction of the remaining rendering blocker. Both the pinned v12 baseline and current v13 report a simple flushed clear/readback control mismatch: expected `01020304`, received `00000000` at four sampled pixels after successful submit, fence wait, map and invalidate. Therefore the probe does **not** claim indexed UI drawing, descriptor routing or WSI presentation passed. Its exit status is intentionally nonzero until the destination clear/copy/readback path is repaired.

This is the reason the v13 launcher must be treated as an experimental diagnostic package. A passing retained regression suite is not evidence that RimWorld's menu is visible. The requested next Windows test is still valuable because the real Intel D3D11 device can expose behavior not reproduced by Wine, but no native-game success or FPS improvement is promised.

## Native test procedure

Extract the complete ZIP into a new folder. Keep v12 as rollback. Run `Diagnose-UI-Frame.cmd` normally, not as Administrator; do not copy the DLL into System32 or permanently replace files beside the game. If the key checks pass, it stages process-local compatibility files and launches RimWorld. Let it remain at the menu for approximately one minute if it is black, then close normally and upload the newest report ZIP and a fresh `Player.log`.

`Check-V13-Bridge.cmd` runs the focused checks without starting RimWorld. `Check-V13-Full.cmd` runs the optional broader matrix/stress checks. The renderer's frame probe is off by default and is enabled only by `Diagnose-UI-Frame.cmd`; it is bounded to the first three presents plus at most twelve ten-second samples.

## Known limits

The bridge remains experimental. Vulkan semaphore wait/signal/acquire semantics are not claimed as implemented. Dynamic storage-buffer descriptor ranges are explicitly unsupported. The indexed fixture itself still fails on the sandbox's destination clear/readback control. No driver, registry, TDR, pagefile, save, mod or System32 changes are made by the package.
