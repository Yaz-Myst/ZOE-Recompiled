# Building and running ZOE

## Folder layout

Everything ZOE lives in `A:\PS2Recomp\ZOE-Recompiled\`:

| Folder / file | What |
|---|---|
| `game\` | Files extracted from the disc: `SLUS_201.48` (the main program), `MODULES\` (IOP drivers), `SYSTEM.CNF` |
| `work\` | Ghidra export, the TOML config, generated C++ (`work\output\`, ~37,000 files), logs, replays, dumps |
| `work\SLUS_201.48.toml` | Translator config, including the `recompile = [...]` list. **The Ghidra export overwrites this file**, so re-add the list after any re-export (backup: `SLUS_201.48.toml.bak_prepkfix`) |
| `work\pcsx2\` | Memory dumps read from PCSX2 through PINE |
| `work\diag\` | Your F9/F10 captures from `play.bat` |
| `src\zoe_overrides.cpp` | ZOE-only runtime code: stage-file interpreter, `isceSifSendCmd` routing |
| `build\` | ZOE's own CMake build folder |
| `tools\` | ZOE helper scripts (stage file checker, PINE VU1 poller) |
| `zoe.cfg` | Per-game settings for `build.ps1` |
| `play.bat` | Play the current build |
| `docs\` | This write-up |

The disc image is read directly from `A:\ps2games\Zone of the Enders (U) UNDUB v1.1.iso`.

## Playing

Run `ZOE-Recompiled\play.bat`. It:

- copies the current build to `work\play_bin` (so rebuilding while you play doesn't clash);
- uses the graphics-card renderer (`PS2X_GPU=1`);
- saves F9 captures and F10 profiles to `work\diag\`;
- records your button presses to `work\input_record.txt`.

Keys: **F9** capture this moment, **F10** start/stop the profiler, **F1** debug panel.

Two controllers (Versus, added 29 Oct): the first controller plugged in is player 1 (with the keyboard), the second
is player 2; each keeps its port while connected, a replugged pad gets its port back. Esc > Controls shows
"Player 1 / Player 2" and a **Swap players** button (needs two controllers). With one controller, pad 2 is
unplugged as far as the game is concerned. `PS2X_PAD_PORTS=1` restores the old single-port behaviour (every port
mirrors player 1).

### Playing from the exe alone (what a player does; added 2026-10-29)

Run `ps2EntryRunner.exe` with no argument. The boot front-end opens in the game window:

- **First run** (no checked disc stored): "Insert your disc". BROWSE picks the disc image (`.iso`); the check reads
  it, makes sure it boots `SLUS_201.48` and that the program is the one this build was made from (3,001,304 bytes,
  CRC-32 `A0A48DD1`, retail and undub alike), then copies `SLUS_201.48`, `SYSTEM.CNF` and `MODULES\` (27 files) to
  `%LOCALAPPDATA%\PS2Recomp\SLUS-20148\game\` and stores the image path in `settings.ini` (`disc_image`).
  `ZOE.DAT` stays in the image and is read from it while playing.
- **Boot screen**: Start, Settings (the Esc menu, applied before the game starts), Disc (check another image), Quit.
- A front-end run uses `play.bat`'s renderer switches (`PS2X_GPU=1`, `PS2X_GS_THREAD=2`, `PS2X_PRESENT=flip`) unless
  the environment sets them; widescreen follows the saved setting. Memory cards: `...\SLUS-20148\game\mc0`, save
  states: `...\SLUS-20148\states`.
- `play.bat` and test runs pass the ELF, so they never see the front-end. `PS2X_FRONTEND=0` turns it off,
  `PS2X_FRONTEND=1` shows it even with an ELF argument. Unattended captures: `PS2X_FRONTEND_TEST=1` (2 also opens
  Settings) with `PS2X_FRONTEND_PICK=<iso>`; give the test its own `LOCALAPPDATA` so the real settings are untouched.

## Building

From `A:\PS2Recomp` in PowerShell:

```
.\build.ps1 -Game zoe *> ZOE-Recompiled\work\last_build_console.txt
```

Then look for `BUILD OK` in that file. `build.ps1` prints to the console host, so redirect with
`*>` to keep the output. A runtime-only change takes about **2½ minutes**.

### The rule that saves 35 minutes

**Never edit a header that the generated code includes.** All ~37,000 generated files include
`ps2_runtime.h`, which pulls in `ps2_runtime_macros.h`, `ps2_syscalls.h`, `ps2_stubs.h`,
`ps2_log.h`, `runtime/ps2_memory.h`, `runtime/ps2_vu1.h`, `runtime/gs/*`, and more; they also
include `ps2_recompiled_stubs.h`. Touching any of these triggers a 30–35 minute full rebuild. Put
new state in `.cpp` files (file-static) or in a new header that only `.cpp` files include
(examples: `ps2_code_resolver.h`, `runtime/ps2_vu_fast.h`).

### After a translator change

1. Rebuild the translator:
   `"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build A:\PS2Recomp\PS2Recomp\out\build --target ps2_recomp --config Release`
   (plain `cmake` isn't on the PATH).
2. Regenerate into a scratch folder, not over `work\output`: copy the TOML, change `output =` to
   the scratch folder, run `PS2Recomp\out\build\ps2xRecomp\Release\ps2_recomp.exe <toml>`
   (about 90 seconds).
3. Compare the scratch output with `work\output` and copy **only the changed `.cpp` files**. Keep
   the old `ps2_recompiled_stubs.h` if the new one only *removes* declarations; copying it forces
   the full rebuild.
4. Build. Rebuild time depends on how many files changed (the `rsqrt.s` fix changed 1,620 files:
   29 minutes).

## Replaying and measuring without you

`A:\PS2Recomp\work\run.ps1 -Game zoe -Seconds N -Log name` runs the build for N seconds and kills it.
Useful switches (set as environment variables first):

| Switch | Effect |
|---|---|
| `PS2X_INPUT_SCRIPT="start@13.72,..."` | Press buttons at those seconds (use the contents of a recorded route file) |
| `PS2X_DUMP_FRAMES=<dir>` | Save one frame per second |
| `PS2X_DUMP_BURST=<tick>,<count>` | Save every frame for a stretch |
| `PS2X_DIAG_AT=<seconds>` | Press F9 automatically at that second |
| `PS2X_PROFILE_WINDOW="a,b"` | Profile from second a to b |
| `PS2X_AUDIO=0` | Mute (the sound chip still runs) |
| `PS2X_SPU2_WAV=<file>` | Record the sound output |
| `PS2X_IOP_TRACE=<n>[,<library>]` | Log the IOP's library calls |
| `PS2X_WATCH=<hex addrs>` (+ `PS2X_WATCH_AFTER`, `PS2X_WATCH_FIRST`) | Hardware write watchpoints |
| `PS2X_VU_FAST=0` / `PS2X_VU_VERIFY=1` / `PS2X_VU1_STATS=1` | Old VU1 engine / compare both / VU1 workload counter |
| `PS2X_NO_DEINTERLACE=1` | Turn off the blend deinterlacer |

Routes: `work\route_stage1_cutscene.txt` (into the first in-engine cutscene, ~95 s at the old speed),
`work\route_battle_2026-09-30.txt` (your battle session; timings are from a slow build).

Reading the profiler: `python tools\profile_report.py <profile.txt>
ZOE-Recompiled\build\ps2xRuntime\Release\ps2EntryRunner.map 40`.

### Widescreen audit (added 25 Oct)

`work\scripts\widescreen_audit.ps1 [-Saves "a.p2s,b.p2s@route.txt"] [-Tag name] [-Settle 120] [-Frames 300] [-Env "K=V"]`
runs every save in `work\states` (or the list) twice from the same state, 16:9 and 4:3, at native internal resolution
(`PS2X_GPU_SCALE=1`), dumping every frame from `Settle` ticks after the save's tick for `Frames` ticks. The 16:9 run
also writes one CSV row per drawn 2D piece (`PS2X_ZOE_WIDE_AUDIT=<csv>,<lo>,<hi>`: wrapper, object, flags, the rule
applied, position / x axes / vertex span before and after the mapping). Then `work\scripts\wsaudit.py <index.csv>`
writes `work\audit\<Tag>\report.md` (+ `crops\`, left 4:3 / right 16:9) with three checks:

| Check | Looks for | How |
|---|---|---|
| A not mapped | pieces left at 4:3 size and place | CSV rows the wrappers left untouched (not full-width, not off-screen), and image regions identical between the 16:9 and 4:3 frame of the same tick that no squeeze (centre, left edge, right edge) explains as well |
| C revealed | the part of a piece outside the 4:3 picture drawn inside the 16:9 one | CSV: original span past x 0 / 512, mapped with the piece's own map, overlapping what is drawn |
| B jumping | a piece whose 16:9 movement does not match its 4:3 movement | CSV: consecutive game frames, 16:9 move vs x scale x 4:3 move (tolerance 2 px + 5 %), and rule changes |

Validated on old bugs brought back by their switches (`work\audit\val_*`): `PS2X_ZOE_WIDE_CLIP=0` on `ws_game_over`
-> C (the sweep, 64 px); `PS2X_ZOE_WIDE_ENEMYBOX=0` on `ws_enemy_box` + `route_enemy_box.txt` -> B (~125 px zone
switches); `PS2X_ZOE_WIDE_PANELS=0` on `ws_item_cockpit` -> A (the item panel). Blind spots: draw paths that never
reach a wrapper show only through the image check (a static piece; a moving one is not caught), and C / B only cover
pieces the wrappers see. ~70 s per save.

## Traps

- `Remove-Item` on `A:\PS2Recomp` paths is blocked by a safety check; use new folder names, and
  clear environment variables with `$env:X=$null`.
- Windows shows files that a killed process was writing (WAV captures, dumps) as 0 bytes. Read the
  real size from inside a program.
- The F10 key can register twice. Use `PS2X_PROFILE_WINDOW` for reliable profiles.
- The runtime doesn't create `diag` folders; `play.bat` does.
