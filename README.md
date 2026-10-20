# HD 5500 GPU Bridge v13 — focused indexed UI correction

An experimental Windows 10 x64 build aimed at the remaining black menu in RimWorld 1.7. **This is the next testable iteration, not a guaranteed game fix or a complete Vulkan driver.** V12's large-BC7 background correction is retained.

## Your next test

1. Close RimWorld and let the old launcher finish cleanup. Keep v12 unchanged for rollback.
2. Extract the **whole v13 ZIP into a new separate folder** in Downloads. Do not copy the loose DLL into the game or System32.
3. Double-click **`Diagnose-UI-Frame.cmd` normally, not as Administrator**. It runs the key checks once, then starts RimWorld with bounded UI/frame diagnostics. Keep Steam and the launcher console open.
4. If the checks pass, see whether the menu now appears. If it stays black, leave it running for about a minute after loading settles, then close the game normally and let the launcher finish.
5. Upload the newest ZIP from the extracted package's **`reports` folder**. This time the report should contain final classification plus before/after backbuffer sample records. Include a fresh Player.log if available.

This direct diagnostic launch avoids running a separate check script and then repeating all checks at launch. No compiler or Wine is needed on your Windows computer. If a blocking key check fails, do not bypass it: the game is deliberately not started.

## What changed

Mapped host-visible vertex/index/descriptor input use is recorded independently of map state and refreshed when consumed if the memory is currently mapped. The missing refresh for indexed draw buffers is added. Transfer/indirect inputs receive the same guarded treatment. Weak parent-lifetime checks prevent dereferencing freed allocation metadata. GPU-only resources are not blindly overwritten by CPU shadows.

Descriptor binding now uses the actual pipeline `firstSet`, including duplicate layouts, and routes dynamic uniform offsets per set and descriptor-array element. Dynamic UBO byte offsets/ranges are converted to D3D11 constant-buffer units with checked ranges. Dynamic storage-buffer ranges are explicitly unsupported instead of silently ignoring offsets.

Positive DXGI statuses such as occlusion are no longer logged as failed HRESULTs. Optional bounded frame probes sample the offscreen image before the final copy and the native backbuffer afterward. Larger images use only 25 pixel samples; small supported images can also be compared exactly. The mode is diagnostic, off in normal launches, and does not paint markers or change game pixels.

Storage-image capability claims now require real typed-UAV support and creation. Depth storage claims are withdrawn, X8_D24 is limited to one mip, and capability mismatch counting reads both stdout and stderr. These capability changes require the next native Intel run to confirm them.

## Faster focused checks

Normal and diagnostic launches keep the key GPU/loader/presentation, transfer, compact-buffer, BC7, atlas, image-lifetime, fence and indexed-UI checks. Redundant broad matrices and retained batch stress checks are now **optional**, using `Check-V13-Full.cmd` or `-FullChecks`. They were not removed or converted into automatic success.

`Check-V13-Bridge.cmd` runs only the selected key checks without starting the game. `Launch-RimWorld-GPU.cmd` is the normal launch without frame readback diagnostics. `Diagnose-UI-Frame.cmd` is the recommended first v13 test. Serial/verbose diagnostic wrappers remain optional and are not performance modes.

## Limits and safety

Native HD 5500 v13 game success and FPS remain unverified. Semaphore wait/signal/acquire semantics are still an unresolved bridge gap; this release does not claim to implement them. Legacy feature-level nonzero UBO ranges and general coherent GPU-write/readback semantics are not fully repaired. The new buffer refresh targets input visibility, not universal Vulkan conformance. Limited frame samples cannot prove every screen pixel is correct.

The launcher remains process-local and reversible, refuses unrelated game-root overwrites, records the expected renderer hash, restores process settings and removes its own unchanged temporary game-root compatibility files after exit. System32, registry, drivers, TDR, pagefile, saves and mods are not modified. A local copy of your existing Vulkan loader and report files remain in the extracted folder. Nothing uploads automatically.

Source patches, probe sources, licences, file hashes and actual test results are included. Use the **whole ZIP**, not the DLL or check script alone.
