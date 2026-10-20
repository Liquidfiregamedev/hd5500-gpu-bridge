# v13.1 launchable diagnostic update

This is a launcher-only update to v13. The renderer DLL is unchanged.

The indexed UI fixture previously failed on the real HD 5500 with a simple clear/readback mismatch and prevented RimWorld from starting. In v13.1 that fixture remains enabled and its complete output is retained, but it is informational rather than a launch blocker. This allows the actual RimWorld 1.7 menu path to be tested.

Blocking checks remain: Intel adapter selection, stock/proxy loader setup, compute/presentation, texture upload, BC7 including the 5333x3000/13-mip background, compact buffers, fence/query, and lifetime checks.

Run `Diagnose-UI-Frame.cmd` normally, not as Administrator. If the game remains black, leave it at the menu for roughly one minute, close normally, and provide the report ZIP plus `Player.log`. Do not interpret a passing preflight as proof of game compatibility.
