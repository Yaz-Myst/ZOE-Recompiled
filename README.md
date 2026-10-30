# ZOE-Recompiled — Zone of the Enders on PC

By **Yz**.

A native Windows port of **Zone of the Enders** (PlayStation 2, USA, `SLUS_201.48`), made by statically
recompiling the game's MIPS code to C++ with [PS2Recomp](PS2Recomp/) and running it on a PC runtime (OpenGL
renderer, compiled vector-unit programs, IOP and sound emulation). The game runs at a steady 60 frames per second
(measured at up to three times the PS2's internal resolution), with widescreen, progressive output, save states,
remappable keyboard and controller input, DualShock 2 pressure buttons, rumble, and two controllers for Versus mode.

**You need your own copy of the game.** This repository and its releases contain no game assets: no disc image, no
textures, no music, no movies. On first start the program asks for your disc image (`.iso`), checks that it is the
USA Zone of the Enders, and copies the few files it needs out of it. The game's data stays inside the image and is
read from there while you play.

> **Made for a controller.** This release is set up and tested for playing with a game controller (DualShock 4,
> DualSense, Xbox and similar). Keyboard controls exist but are not tuned yet. For now the options menu (Esc) needs
> the keyboard: Q/E switch sections, arrow keys move and change values, Enter selects, Esc closes.

## What works, what does not

- The whole game is playable from the intro to the final area, including in-engine cutscenes, movies with sound,
  the Hub, and the hidden Versus mode (an option unlocks it without a cleared save).
- Known issues you may meet: the Esc menu and the boot screen do not follow a controller reliably yet (use the keyboard
  there: Q/E sections, arrows, Enter, Esc; in game controllers work normally), a rare freeze when the warehouse
  before the second Neith fight loads (a stream chunk goes missing; reload your save), a freeze reported once after
  the long lift movie to the final area, and small HUD details in the cockpit view.

## Requirements

- Windows 10/11, 64-bit, a GPU with OpenGL 3.3 (anything from the last ten years).
- Your own retail USA disc image of Zone of the Enders as an `.iso` with 2048-byte sectors (the undub release
  carries the same program and works too). The check expects `SLUS_201.48`, 3,001,304 bytes, CRC-32 `A0A48DD1`.
- To build: Visual Studio 2022 (C++ desktop workload, which includes CMake), Python 3, and Git for Windows
  (the regeneration script runs in Git Bash). The first configure downloads the third-party libraries.

## Playing

1. Download the latest zip from the [Releases page](https://github.com/Yaz-Myst/ZOE-Recompiled/releases), unzip it
   anywhere (not inside Program Files) and run `ZOE-Recompiled.exe`. The boot screen opens in the game window. If
   Windows reports `MSVCP140.dll` missing, install the
   [Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).
2. First start: pick your disc image. The program checks it (five steps shown on screen), copies the program and
   its IOP modules to `%LOCALAPPDATA%\PS2Recomp\SLUS-20148\game\`, and remembers the image path.
3. **Start** plays; **Settings** opens the same menu as Esc in game; **Disc** picks another image.

Keys: Enter = Start, arrows/WASD = D-pad, X/Space = Cross, C = Circle, Z = Square, V = Triangle, Q/E = L1/R1,
Shift = L2/R2, Tab = Select. **Esc** opens the menu (display, sound, controls, overlay, extras). **F5** saves a state,
**F8** loads it, **F6/F7** change the slot. Controllers (SDL2: DualShock 4, DualSense, Xbox, Switch Pro, …) are
picked up live; the first one is player 1 together with the keyboard, the second is player 2 for Versus; Esc >
Controls shows who is who and has a **Swap players** button.

Settings and memory cards live in `%LOCALAPPDATA%\PS2Recomp\SLUS-20148\` (`settings.ini`, `game\mc0`, `states\`).

## Building it yourself

The recompiled C++ (about 37,000 files, 335 MB) is generated from your own copy of the game's program and is not
in the repository. The build is three steps.

1. **The translator.** Configure `PS2Recomp/` with CMake (Visual Studio 2022, Release) and build the target
   `ps2_recomp`. The scripts expect it at `PS2Recomp/out/build/ps2xRecomp/Release/ps2_recomp.exe` (the folder
   Visual Studio's "Open Folder" uses; set `SOURCE_DIR` in `ZOE-Recompiled/zoe.cfg` for another location).
2. **The game's code.** Put `SLUS_201.48` from your disc into `ZOE-Recompiled/game/` (7-Zip opens the `.iso`), open
   `ZOE-Recompiled/work/SLUS_201.48.toml` and set the three paths at the top (`input` = that ELF, `output` =
   `.../ZOE-Recompiled/work/output`, `ghidra_output` = `.../ZOE-Recompiled/work/SLUS_201.48_map.csv`; absolute paths
   for now), then in Git Bash:

   ```bash
   bash work/regen.sh -g zoe MAIN
   ```

   This runs the translator with the shipped function map (`SLUS_201.48_map.csv`) and the reviewed configuration
   (the `recompile` list in the TOML, the stand-in review `stubs_reviewed.csv`) and writes about 37,000 files in
   two to three minutes. No Ghidra needed.
3. **The runner.** In PowerShell, from the repository root:

   ```powershell
   .\build.ps1 -Game zoe
   ```

   The first build compiles all generated files (35-40 minutes); later builds take about 2½ minutes. The script
   checks the result (every function present, no unresolved symbols, the stand-in review) and prints `BUILD OK`.
   The exe is `ZOE-Recompiled/build/ps2xRuntime/Release/ps2EntryRunner.exe`; `ZOE-Recompiled/play.bat` runs a copy
   of it with the development settings and logs.

Paths in `zoe.cfg` are relative to that file; only `CD_IMAGE` (your disc, used by `play.bat`) is yours to set.

## How it is put together

| Folder | What |
|---|---|
| [`PS2Recomp/`](PS2Recomp/) | the translator, the runtime (GS renderer, VU engines, scheduler, save states, front-end, menu), the IOP and sound emulation; a modified copy of [ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp) ([MODIFIED.md](PS2Recomp/MODIFIED.md)) |
| [`ZOE-Recompiled/`](ZOE-Recompiled/) | everything specific to this game: overrides and compiled VU programs in `src/`, the translator configuration and reviewed stand-in list in `work/`, test routes, and the documentation in `docs/` |
| [`build.ps1`](build.ps1), [`games/`](games/), [`tools/`](tools/), [`work/`](work/) | the per-game build script, its config loaders, shared Python tools (stub review, name matching, profiling, PINE), and the shared pipeline scripts |

More detail: [building and debug switches](ZOE-Recompiled/docs/05-build-and-run.md), [ZOE's internals](ZOE-Recompiled/docs/notes/02_zoe_internals.md) and the [glossary](ZOE-Recompiled/docs/glossary.md).

## Credits and licence

- ZOE-Recompiled: Yz. `PS2Recomp/` is a modified copy of PS2Recomp kept in this repository; what changed and from
  which upstream commit: [PS2Recomp/MODIFIED.md](PS2Recomp/MODIFIED.md).

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors: the translator and runtime this port
  is built on.
- The SPU2 sound core in `PS2Recomp/ps2xIOP/src/emulator/spu2/pcsx2/` is PCSX2's, copied unmodified from
  [PCSX2](https://github.com/PCSX2/pcsx2) at commit `1d102db` (GPL-3.0+; provenance and file list in
  [that folder's README](PS2Recomp/ps2xIOP/src/emulator/spu2/README.md)). PCSX2 was
  also the behaviour reference throughout.
- Third-party libraries fetched at build time: raylib, SDL2, Dear ImGui (with rlImGui), fmt, toml11, nlohmann/json,
  ELFIO, rabbitizer, libdwarf, FFmpeg.
- Zone of the Enders is © Konami. This project is not affiliated with or endorsed by Konami.

Licensed under the **GNU General Public License v3** (see [PS2Recomp/LICENSE](PS2Recomp/LICENSE)).
