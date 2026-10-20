# HD5500 GPU Bridge — play RimWorld 1.7 on older laptops

**Does RimWorld 1.7 refuse to start on your older PC with an error about Vulkan? This might fix it.**

Since version 1.7, RimWorld needs a graphics feature called **Vulkan**. Many older laptops and PCs, for example those with **Intel HD Graphics 4000 / 4600 / 5500 / 6000**, never got Vulkan support, so the game won't start on them. Their graphics chip is fast enough; it just speaks a different language (Direct3D 11).

The HD5500 GPU Bridge **translates between the two**, so the game can run on these PCs anyway. You don't need to change Windows or your drivers.

> ⚠️ **Flashing warning:** the game window may flicker or flash briefly while it starts, and during the built-in test runs. Look away during start-up if flashing images affect you.

> This is an unofficial fan project. It is **not** made or supported by Ludeon Studios, and it does **not** include RimWorld. You need your own copy of RimWorld 1.7 (Steam).

---

## How well does it work?

On the test laptop (Lenovo Z70-80, Intel HD 5500, 2015) RimWorld 1.7 runs **smoothly at about 45 fps**: the menu, new colonies, the whole map, pawns, animals, plants, water and saves.

| | Before (first working version) | Now (v13.75) |
|---|---|---|
| Average frame rate | 35 fps | **44 fps** (capped at 45) |
| Worst moments (1% low) | 9 fps | **21 fps** |
| Noticeable stutters in 5 minutes | 181 | **12** |

Your results depend on your PC. On laptops, heat matters most: a hot laptop slows itself down. The bridge has tools for that (see below).

---

## How to use it

1. **Download** this project: green **Code** button → **Download ZIP**. Unpack the whole folder anywhere; a USB stick is fine too.
2. Double-click **`Bridge-Menu.cmd`**. A normal double-click; you don't need "Run as administrator".
3. Click **Play RimWorld**.

That's it. The first start takes a bit longer, because the bridge prepares the game's graphics once. Later starts are faster.

If RimWorld isn't in the usual Steam folder, the menu asks you once where `RimWorld.exe` is.

### Will it work on my PC?

| Your graphics | Chance |
|---|---|
| Intel HD 4000, 4400, 4600, 5000, 5500, 6000, Iris (about 2012–2015) | 👍 Should work. Only the HD 5500 has been tested so far. |
| Older NVIDIA (GeForce 400/500 series) or AMD (Radeon HD 5000/6000) | 🤞 Should work, not tested |
| Intel HD 2000 / 3000 (about 2011) | ❌ Too old, won't work |
| Any newer graphics card | Not needed: your PC has Vulkan already |

Not sure what you have? Press Win + R, type `dxdiag` and look under *Display*. Or start the menu: the Home page tells you in one line whether your PC needs the bridge, and **Laptop health → PC check** gives the details (including which driver to install if your GPU does have Vulkan).

---

## Is it safe?

- **Nothing on your PC is changed.** No drivers are installed, and Windows, the registry and System32 stay as they are. The bridge is used only while RimWorld runs from this menu.
- Your **saves and mods stay as they are.**
- Close the game and everything is back to normal. To remove it, delete the folder.
- The complete source code is included.

---

## The menu at a glance

- **Play**: start the game normally.
- **Tests**: let the game run for 5 minutes by itself and get a report: frame rate, stutters, problems. Handy for comparing settings.
- **Laptop health**: for laptops that get hot or slow down:
  - **Throttle monitor**: a small window showing live fps, CPU speed and temperature.
  - **PC check**: a report with tips for your PC. It changes nothing.
  - **Heat guard**: drops to 30 fps while the laptop is overheating, instead of stuttering.
  - **45 fps cap**: less heat, steadier play.
  - **Cool mode** and **Game Bar recording off**: optional Windows tweaks, each with an undo button.
- **Results**: open the report of your last game or test.

---

## Something wrong?

| Problem | Try this |
|---|---|
| The game doesn't start, or the screen stays black | Menu → **Diagnostics → Full start-up checks**, then look at the report (Results page) |
| Strange colours, missing or broken graphics | Open `experiment-env.txt` and switch off the newest improvement (see `TESTING.md`). Please report what you saw. |
| Runs fine at first, then starts stuttering | Usually your laptop getting hot. Use **Laptop health**: heat guard, 45 fps cap, *Cool mode*, and keep the fan vents clean. |
| Slow in general | Close other programs (Discord, browsers) while playing; zoom in a bit (zoomed far out = much more to draw) |

---

## For the technically curious

The bridge is a small **Vulkan → Direct3D 11 translator**: a heavily extended version of the open-source [Ashes](https://github.com/DragonJoker/Ashes) project. The game thinks it is talking to a Vulkan driver, and the bridge passes every drawing command on to the Direct3D 11 driver that older graphics chips do have.

Most of the work went into two things: making RimWorld 1.7's graphics come out right (plants, pawns, water, UI), and making the translation cheap. The bridge now uses about **1.2 ms per frame**, down from 3–8 ms in earlier versions.

| File | What's in it |
|---|---|
| `CHANGELOG-HD5500-BRIDGE.md` | Every version: what changed, why, and measured results |
| `HANDOFF.md` | Current state and how everything fits together |
| `TESTING.md` | What to check after an update, plus all on/off switches |
| `LUDEON-REPORT-older-GPUs.md` | Suggestions for Ludeon that would help all older PCs |
| `source/` | Complete source code of the bridge and the optimizer mod |
| `tests/` | Automatic tests (34 checks) |

### Credits and licences

- **Ashes**, © Sylvain Doremus, MIT licence
- **SPIRV-Cross**, Apache 2.0
- **bcdec**, MIT / Unlicense
- **Launcher and tools**, MIT

The licence texts are in `licenses/`. RimWorld and Waylith are © Ludeon Studios.
