# Building and playing ZOE-Recompiled on Linux / Steam Deck

This guide adds a native Linux x86-64 build to the existing Windows workflow. The game code is
still generated from **your own** copy of *Zone of the Enders* (USA, `SLUS_201.48`); no game
data is in the repository or in the AppImage.

Tested so far: Ubuntu 22.04 x86-64 under WSL2, GCC 11.4, CMake 4.4.4, Ninja, Mesa llvmpipe
(software OpenGL). Physical Steam Deck testing is still required; see
[Known issues](#known-issues).

## 1. What you need

- A 64-bit x86 Linux (SteamOS 3, Arch, Ubuntu 22.04+).
- Your own USA disc image of *Zone of the Enders* as a 2048-byte-sector `.iso`. The undub
  release works too. The build checks `SLUS_201.48` = 3,001,304 bytes, CRC-32 `A0A48DD1`.
- ~2 GB free disk for the generated C++ and build objects (the final executable is ~170 MB).
- An OpenGL 3.3 driver. SteamOS's Mesa driver is fine; the game uses the existing OpenGL GS
  renderer, not Vulkan.

### Steam Deck: Distrobox container

SteamOS is immutable; build inside a container rather than modifying `/usr`.

```bash
# Desktop Mode terminal
distrobox create --name zoe-build --image ubuntu:22.04
distrobox enter zoe-build

sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git python3 pkg-config \
  libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
  libasound2-dev libpulse-dev libavcodec-dev libavformat-dev libavutil-dev \
  libswresample-dev libswscale-dev libwayland-dev libxkbcommon-dev
```

For an Arch container the equivalent packages are `base-devel cmake ninja git python
pkgconf mesa libx11 libxrandr libxinerama libxcursor libxi alsa-lib libpulse ffmpeg
wayland libxkbcommon`.

## 2. Clone the development branch

```bash
git clone https://github.com/omegakatana92/ZOE-Recompiled.git
cd ZOE-Recompiled
git remote add upstream https://github.com/Yaz-Myst/ZOE-Recompiled.git
git fetch upstream
git switch steamdeck-linux-port
```

## 3. Build the translator and the game

The one-shot script verifies dependencies, builds `ps2_recomp`, validates your executable,
regenerates the ~37,000 C++ files, runs the stub-review gate, and links `ps2EntryRunner`.

```bash
# Point at your own ISO; the script extracts only SLUS_201.48 into the git-ignored game folder.
./build-linux.sh --iso "/path/to/Zone of the Enders (U) UNDUB v1.1.iso"

# or, if you already extracted the executable:
./build-linux.sh --elf ZOE-Recompiled/game/SLUS_201.48

# conservative parallelism for a Deck (default is min(nproc, 6)):
./build-linux.sh --elf ZOE-Recompiled/game/SLUS_201.48 --jobs 4
```

Useful options:

| Option | Meaning |
|---|---|
| `--build-dir DIR` | CMake build root (default `build-linux`) |
| `--generated-dir DIR` | where regenerated game C++ goes (default `ZOE-Recompiled/work/output`) |
| `--skip-regen` | reuse generated code; fast rebuild of the runtime |
| `--skip-translator` | reuse the already built `ps2_recomp` |
| `--portable-cpu` | x86-64-v2 instead of the default AVX2 baseline |
| `--clean` | remove the CMake build root first |

The script prints the executable path at the end, normally:

```text
build-linux/game/ps2xRuntime/ps2EntryRunner
```

### Manual steps (what the script does)

```bash
cmake -S PS2Recomp -B build-linux/translator -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_RECOMP=ON -DPS2X_BUILD_RUNTIME=OFF \
  -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
cmake --build build-linux/translator --target ps2_recomp -j 6

PS2X_RECOMP="$PWD/build-linux/translator/ps2xRecomp/ps2_recomp" \
PS2X_GENERATED_DIR="$PWD/ZOE-Recompiled/work/output" \
  bash work/regen.sh -g zoe MAIN

cmake -S PS2Recomp -B build-linux/game -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_RUNTIME=ON \
  -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF \
  -DPS2X_RUNNER_GENERATED_DIR="$PWD/ZOE-Recompiled/work/output" \
  -DPS2X_RUNNER_EXTRA_SOURCES="$PWD/ZOE-Recompiled/src/zoe_overrides.cpp;$PWD/ZOE-Recompiled/src/zoe_vu_aot.cpp;$PWD/ZOE-Recompiled/src/zoe_frontend.cpp" \
  -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF
cmake --build build-linux/game --target ps2EntryRunner -j 6
```

`PS2X_ENABLE_AGRESSIVE_LOGS=OFF` matters: with it on, every recompiled function is logged and
the game spends most of its time writing logs.

## 4. First run

```bash
./build-linux/game/ps2xRuntime/ps2EntryRunner
```

The boot front-end opens. Choose **DISC** → **BROWSE** and pick your `.iso` with keyboard,
controller or mouse. The five-step check reads the ISO 9660 directory, verifies `SYSTEM.CNF`
and the ELF size/CRC, and copies the program, `SYSTEM.CNF` and `MODULES/*.IRX` to:

```text
$XDG_DATA_HOME/PS2Recomp/SLUS-20148/
  settings.ini
  game/SLUS_201.48
  game/MODULES/...
  game/mc0/          # memory cards
  states/            # save states
```

`$XDG_DATA_HOME` defaults to `~/.local/share`. Nothing is written inside the installation
directory, so the same data works for a native build and an AppImage, including read-only
mounts. On Windows the old `%LOCALAPPDATA%\PS2Recomp\SLUS-20148\` layout is unchanged.

Start with **START**. The defaults are the tested configuration: GPU renderer, two render
threads, frame-on-flip presentation, 16:9 widescreen option. Press **Esc** for the menu,
**F5**/**F8** for save/load state, **F6/F7** for the slot.

To boot straight into the game (skip the launcher screen), pass the extracted ELF and set the
image environment variable:

```bash
PS2X_CD_IMAGE="/path/to/game.iso" \
  ./build-linux/game/ps2xRuntime/ps2EntryRunner \
  "$HOME/.local/share/PS2Recomp/SLUS-20148/game/SLUS_201.48"
```

## 5. Steam Gaming Mode

1. Put the executable (or AppImage) somewhere the `deck` user can read, e.g.
   `~/Games/ZOE-Recompiled/`.
2. Steam → **Games** → **Add a Non-Steam Game** → **Browse** and select the executable or
   `.AppImage`.
3. Properties → Compatibility: leave **Proton disabled** (this is a native Linux build).
4. Launch from Gaming Mode. The boot front-end and in-game menu accept the Deck controller;
   use the touchscreen/mouse if a first-run file pick is easier.
5. If the built-in controls are not seen, enable **Steam Input** for the shortcut (Gaming Mode
   normally does this automatically). External Xbox/PlayStation pads are handled by SDL2 and
   can be hot-plugged.

## 6. AppImage

Only after the native executable works:

```bash
./build-appimage.sh
```

It creates `dist/ZOE-Recompiled-SteamDeck-x86_64.AppImage`. The script downloads
`linuxdeploy` and `appimagetool` into the build directory, bundles FFmpeg and other
non-core libraries, excludes the host's OpenGL/X11/audio drivers, adds a desktop entry and a
simple original icon, and writes a persistent-data launcher. The AppImage contains **no game
assets**.

```bash
chmod +x dist/ZOE-Recompiled-SteamDeck-x86_64.AppImage
./dist/ZOE-Recompiled-SteamDeck-x86_64.AppImage
```

## 7. Collecting Steam Deck test evidence

Physical Deck testing is the remaining milestone. From Desktop Mode's Konsole (or an SSH
session), run the artifact with logging so failures can be diagnosed:

```bash
chmod +x ZOE-Recompiled-SteamDeck-x86_64.AppImage
cd ~/Downloads
./ZOE-Recompiled-SteamDeck-x86_64.AppImage > ~/zoe-run.log 2>&1
```

Useful switches (set before launching):

```bash
# per-frame timeline for FPS/frame-pacing analysis
PS2X_FRAME_LOG="$HOME/zoe-frames.csv" ./ZOE-Recompiled-SteamDeck-x86_64.AppImage > ~/zoe-run.log 2>&1

# graphics diagnostic capture (also F9 in game) into a writable folder
PS2X_DIAG_DIR="$HOME/zoe-diag" ./ZOE-Recompiled-SteamDeck-x86_64.AppImage > ~/zoe-run.log 2>&1

# force the CPU GS renderer for a correctness comparison
PS2X_GPU=0 ./ZOE-Recompiled-SteamDeck-x86_64.AppImage > ~/zoe-cpu.log 2>&1
```

For a Gaming Mode run, add the AppImage as a non-Steam game, launch it once, then use
**Steam → Settings → System → Developer → Open logs** or `journalctl --user -b` for the
Steam-side log. Include `~/zoe-run.log`, the model (LCD/OLED), SteamOS version, and whether
Steam Input was on.

## 8. Troubleshooting

| Symptom | Fix |
|---|---|
| `Missing dependencies: ...` | Install the packages listed in section 1; the message names the missing pkg-config modules. |
| CMake cannot download raylib/SDL2/imgui | The first configure needs network access (FetchContent). Re-run once the network works. |
| `SLUS_201.48 ... not the supported program` | Wrong region/version or a raw `.bin`. Use a 2048-byte-sector USA ISO; the undub works. |
| Black window / `missing GL function` | The driver does not expose OpenGL 3.3. On Steam Deck use the Mesa driver; under a VM enable 3D acceleration or use `LIBGL_ALWAYS_SOFTWARE=1` for a slow but working test. |
| No audio | Check `PS2X_AUDIO=0` is not set and the desktop's PipeWire/PulseAudio is running. The log should say `SPU2 output stream started`. |
| Controller not detected | `[input] controller connected:` should appear. Try `PS2X_PAD_PORTS=1` for the old single-port behaviour; enable Steam Input in Gaming Mode. |
| `ps2EntryRunner` link runs out of memory | Lower `--jobs` (e.g. 3–4) and close other apps. Generated code is compiled in unity batches. |
| Full rebuild after editing a runtime header | Expected: generated files include the runtime headers. Runtime-only edits are quick; header edits rebuild the game. |

## 9. Known issues

- **Physical Steam Deck validation is still pending.** The Linux executable and the AppImage
  boot, extract, render, play audio and reach the game under Mesa llvmpipe, but FPS/frame pacing
  on the Deck's AMD GPU and the Gaming Mode/Steam Input path must be measured on hardware.
- Software rendering reached ~31 FPS in the intro and up to ~60 vblank/s in a menu scene; do not
  treat those as Deck numbers. 60 FPS is the target, not yet a verified Deck result.
- The AppImage was built and launched in a separate process under Xvfb with a fresh HOME, but
  not yet from Steam Gaming Mode or on a clean SteamOS machine.
- The in-game disc picker, boot screen, settings, memory cards and save states are implemented;
  movie playback/audio sync, two-controller Versus mode and long-session stability still need
  hardware testing.
- The Windows build remains the reference. Linux changes are guarded, but a Windows rebuild
  should be run before upstreaming.
- `steamdeck-linux-port` is intentionally separate from `main` until hardware testing is done.

## 10. Licensing and attribution

ZOE-Recompiled and PS2Recomp are GPL-3.0. The SPU2 core is PCSX2's GPL-3.0+ code; see
`PS2Recomp/ps2xIOP/src/emulator/spu2/README.md`. *Zone of the Enders* is © Konami. This
project is not affiliated with or endorsed by Konami and ships no game data.
