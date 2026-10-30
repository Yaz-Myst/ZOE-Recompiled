# ZOE-Recompiled — Native Linux / Steam Deck (v0.1.0)

Native Linux x86-64 port of **ZOE-Recompiled**, the static recompilation of *Zone of the
Enders* (PlayStation 2, USA, `SLUS_201.48`). This release preserves the Windows project and
adds the Linux runtime, build scripts and a portable AppImage.

> **You need your own copy of the game.** No disc image, textures, music, movies, extracted
> assets, or save files are included. The boot screen asks for your own USA `.iso` and reads
> the game data from it. The expected program is `SLUS_201.48`, 3,001,304 bytes, CRC-32
> `A0A48DD1`; the undub release carries the same program.

## Downloads

| File | What it is |
|---|---|
| `ZOE-Recompiled-SteamDeck-x86_64.AppImage` | Portable build for Steam Deck / Linux. Recommended. Bundles the non-core libraries (FFmpeg and dependencies) and keeps settings/saves in XDG directories. |
| `ZOE-Recompiled-linux-x86_64.tar.gz` | Native executable plus license/desktop files. Use this if you prefer not to use AppImage; it expects the distribution's FFmpeg runtime libraries. |
| `SHA256SUMS.txt` | Checksums for both artifacts. |

## Quick start (AppImage)

```bash
chmod +x ZOE-Recompiled-SteamDeck-x86_64.AppImage
./ZOE-Recompiled-SteamDeck-x86_64.AppImage
```

Or mark it executable and double-click it. To add it to Steam: **Games → Add a Non-Steam
Game → Browse**, select the AppImage, and leave **Proton disabled** (this is a native Linux
build).

On first start:

1. Choose **DISC → BROWSE** and pick your own Zone of the Enders `.iso`.
2. The front-end checks the ISO and copies `SLUS_201.48`, `SYSTEM.CNF` and the IOP modules to
   `$XDG_DATA_HOME/PS2Recomp/SLUS-20148/` (default `~/.local/share/...`).
3. Choose **START**.

Settings, extracted data, memory cards and save states live outside the AppImage, so they
persist across updates.

## Quick start (native tarball)

```bash
tar xzf ZOE-Recompiled-linux-x86_64.tar.gz
cd ZOE-Recompiled-linux-x86_64
./ZOE-Recompiled
```

The tarball executable dynamically uses the system FFmpeg libraries
(`libavcodec`, `libavformat`, `libavutil`, `libswresample`, `libswscale`) plus the desktop's
OpenGL/X11/audio stack. On Debian/Ubuntu:

```bash
sudo apt-get install libavcodec58 libavformat58 libavutil56 libswresample3 libswscale5
```

On Arch/SteamOS install `ffmpeg`. The AppImage already includes these libraries.

## Verified in this build

- Translator and runtime compile with GCC 11.4 / CMake / Ninja on Linux x86-64.
- 37,387 recompiled C++ files generated from the supplied undub ISO; stub review: 329/329
  reviewed, 0 problems.
- Native executable links successfully (~172 MB).
- Boot front-end extracts the game program and 27 IOP modules, validates size/CRC, loads the
  ELF and reaches gameplay.
- OpenGL 3.3 GS renderer initializes and renders (Jupiter intro and Konami logo captured).
- SPU2 audio stream starts at 48 kHz stereo; FFmpeg MPEG-2 movie decoding works.
- In-game disc picker works with keyboard, mouse and SDL2 controllers.
- Second run loads settings and skips extraction.
- AppImage built with linuxdeploy/appimagetool and launched from a fresh HOME in a separate
  process under Xvfb.

## Not yet verified / known issues

- **Physical Steam Deck hardware testing is still pending.** This release was built and
  functionally tested under Ubuntu 22.04 / WSL2 with Mesa llvmpipe (software OpenGL). FPS,
  frame pacing, AMD GPU behavior, Steam Input and Gaming Mode launch must be measured on a
  real Deck.
- Software rendering reached ~31 FPS in the intro and ~60 vblank/s in a menu scene. These are
  not Deck numbers; 60 FPS is the target, not a verified Deck result.
- Save-state save/load, long movie playback/audio sync, two-controller Versus mode and
  long-session stability need further testing.
- Movie playback emitted FFmpeg `ac-tex damaged` warnings under software rendering; likely
  stream/read accuracy, unconfirmed on Deck.
- The AppImage was not yet run from Steam Gaming Mode or on a clean SteamOS machine.
- The Windows build remains the reference; Linux changes are guarded, and a full Windows
  rebuild with generated game code should be run before upstreaming.

## Build from source

The source tree is GPL-3.0; the generated game code is not in the repository. Build with your
own disc image:

```bash
git clone https://github.com/omegakatana92/ZOE-Recompiled.git
cd ZOE-Recompiled
git switch steamdeck-linux-port
./build-linux.sh --iso "/path/to/your/Zone of the Enders.iso"
```

Linux prerequisites, Distrobox setup for SteamOS, Gaming Mode notes, troubleshooting and the
portability audit are in:

- [`docs/steamdeck-build.md`](https://github.com/omegakatana92/ZOE-Recompiled/blob/steamdeck-linux-port/docs/steamdeck-build.md)
- [`docs/portability-audit.md`](https://github.com/omegakatana92/ZOE-Recompiled/blob/steamdeck-linux-port/docs/portability-audit.md)

AppImage packaging:

```bash
./build-appimage.sh
```

## Licensing and credits

- ZOE-Recompiled: Yz. PS2Recomp by ran-j and contributors; this fork's Linux port preserves
  the GPL-3.0 licensing and all original notices.
- The SPU2 core is PCSX2's GPL-3.0+ code (see
  `PS2Recomp/ps2xIOP/src/emulator/spu2/README.md`).
- *Zone of the Enders* is © Konami. This project is not affiliated with or endorsed by Konami.
- Use this release only with a copy of the game you legally own.
