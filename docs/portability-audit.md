# Linux / Steam Deck portability audit

Repository: `omegakatana92/ZOE-Recompiled` (fork of `Yaz-Myst/ZOE-Recompiled`)
Branch: `steamdeck-linux-port`
Target: Linux x86-64, SteamOS 3 (Steam Deck LCD/OLED)
Audit date: 2026-10-30

This is the Phase 1 audit, updated with the fixes that were implemented and verified in
Phases 2–3. "Fixed" means the change is in this branch; "verified" means it was exercised by
the Linux build/runtime described in `docs/steamdeck-build.md`.

## Confirmed blockers and fixes

| # | Area | Location | Severity | Finding | Fix / status |
|---|---|---|---|---|---|
| 1 | Build | `PS2Recomp/ps2xRecomp/include/ps2recomp/code_generator.h` | High | `std::unordered_map<uint32_t, Symbol>` with only a forward declaration of `Symbol`; libstdc++ rejects this, MSVC accepted it. | Included `ps2recomp/types.h` (fixed, translator builds with GCC 11). |
| 2 | Build | `PS2Recomp/ps2xRuntime/cmake/ReleaseMode.cmake` | High | Only MSVC got optimisation/ISA flags. Linux built without `-msse4.1`, so SSE4.1 intrinsics (`_mm_extract_epi32`) failed with "target specific option mismatch". | Linux branch adds `-O3 -mavx2 -mfma -msse4.2` (matches the Windows `/arch:AVX2` baseline); `-DPS2X_PORTABLE_CPU=ON` builds x86-64-v2 instead. |
| 3 | Build | same file | Medium | IPO/LTO is enabled for every target by default. On a 37,000-file game this makes Linux linking/memory use impractical. | `PS2X_ENABLE_LTO` (default OFF for GCC/Clang; MSVC keeps `/GL`+`/LTCG`). |
| 4 | Graphics | `ps2xRuntime/src/lib/gs/gs_gpu_backend.cpp` | High | OpenGL 3.3 function loader is Windows-only (`wglGetProcAddress`/`opengl32.dll`); `gl::load()` returned false on Linux. | Added GLX (`glXGetProcAddressARB`) + `dlsym` loader for Linux; linked `OpenGL::GL` and `${CMAKE_DL_LIBS}`. Verified shader/GS startup with Mesa llvmpipe. |
| 5 | Filesystem | `ps2xRuntime/src/lib/ps2_host_settings.cpp` | High | Settings always used `%LOCALAPPDATA%`; on Linux it silently fell back to the current directory. | XDG: `$XDG_DATA_HOME/PS2Recomp/<game-id>` (default `~/.local/share/...`), Windows path unchanged. |
| 6 | Front-end | `ps2xRuntime/src/lib/ps2_frontend_dialog.cpp` | High | The disc-image picker was a Win32 `GetOpenFileNameW`; on Linux the function always returned false, so a first run could not select a disc. | In-game ImGui file browser for Linux: folders + `.iso` files, keyboard, SDL2 controller, mouse; no zenity/kdialog dependency (works in Gaming Mode/gamescope). Verified by selecting an ISO and booting through the front-end. |
| 7 | Front-end | `ps2xRuntime/src/lib/ps2_frontend.cpp` | Medium | Windows-only path check rejected non-ANSI paths; on Linux UTF-8 paths are valid. | Linux branch accepts paths unchanged; Windows behaviour preserved. |
| 8 | Input | `ps2xRuntime/src/lib/ps2_frontend.cpp` | Medium | Boot-screen menu entries use `ImGui::InvisibleButton`, which ImGui marks `NoNav` by default; keyboard/gamepad navigation skipped them, matching the reported "boot screen needs a keyboard" issue. | Pass `ImGuiButtonFlags_EnableNav` for menu items and command buttons. Settings menu already uses standard widgets. |
| 9 | Audio | `ps2xIOP/.../SPU2/spu2sys.cpp` | High (link) | `GetMemPtr`, `CounterUpdate`, `TimeUpdate` were `__forceinline` in one TU and declared `extern` in headers used by other TUs. MSVC emits copies; GCC/Clang produced undefined references. | Made those three cross-TU entry points non-inline. SPU2 linked and produced audio under Linux. |
| 10 | Build | `work/regen.sh`, `games/lib.sh` | High | Scripts assumed Git Bash/cygpath and `python`; the translator path was hard-coded to `ps2_recomp.exe`; TOML contained absolute `A:/...` paths. | Linux-aware path conversion, `python3` fallback, `PS2X_RECOMP` override, `PS2X_GENERATED_DIR`/`PS2X_WORK_DIR` overrides. `regen.sh` rewrites `input`/`output`/`ghidra_output` on both platforms, so the shipped TOML has no developer drive letters. Verified: 37,387 generated `.cpp` files on Linux. |
| 11 | Build | `ZOE-Recompiled/zoe.cfg` | Medium | `CD_IMAGE` contained a developer-specific `A:/ps2games/...` path. | Empty by default; the front-end asks once and stores the user's path. |
| 12 | Packaging | repository root | Medium | No Linux build script or AppImage path. | Added `build-linux.sh`, `build-appimage.sh`, `packaging/`, and `tools/iso_extract.py`. |
| 13 | Profiler | `ps2xRuntime/src/lib/ps2_profiler.cpp` | Low | Windows sampling/crash-reporting code. | Already guarded by `#if defined(_WIN32)`; Linux gets no-op stubs. No change needed. |
| 14 | Time | various Kernel/SPU2 files | Low | `localtime_s`/`gmtime_s` calls. | Already guarded with `localtime_r`/`gmtime_r` on non-Windows. |
| 15 | Paths | `ps2_iop_host.cpp`, kernel helpers | Low | Wide `_wfopen` / `_fseeki64` calls. | Already guarded with narrow `fopen`/`fseeko` on Linux. |
| 16 | Fonts | `ps2xRuntime/src/lib/ps2_ui_theme.cpp` | Low | UI font lookup only used `%WINDIR%\Fonts`. | Linux fallback list (DejaVu/Noto) plus `PS2X_FONT_UI`/`PS2X_FONT_MONO`; ImGui default font is the final fallback. |
| 17 | Video | `ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp` | Low | FFmpeg is API-stable here, but the Windows build downloads a prebuilt FFmpeg 7.1 while Linux uses the distribution's FFmpeg (4.4 on Ubuntu 22.04). | Build uses `pkg-config` for libavcodec/format/util/swresample/swscale; verified with FFmpeg 4.4 (58.134.100). |
| 18 | Audio device | `ps2xRuntime/src/lib/ps2_runtime.cpp` | Low | raylib/miniaudio audio device; no Windows API. | Works with PulseAudio/PipeWire via raylib on Linux. Verified stream starts and SPU2 produces samples. |

## Deliberately unchanged

- The OpenGL 3.3 GS renderer, VU engines, EE scheduler, IOP emulation and save-state format. No Vulkan rewrite.
- Windows build paths and MSVC flags. The Linux changes are guarded by `_WIN32`/`MSVC` or by
  platform CMake branches wherever practical.
- Generated game code. It is derived from the user's copyrighted disc and remains git-ignored.
- PCSX2 SPU2 sources were only touched for the C++ linkage issue above; no DSP behaviour change.

## Remaining risks / unverified on hardware

1. **No physical Steam Deck run yet.** Verification so far is Ubuntu 22.04 x86-64 under WSL2
   with Mesa llvmpipe (software OpenGL). Graphics, audio, extraction and boot are real, but
   performance and the Deck's AMD driver/Steam Input path must be checked on hardware.
2. **Performance.** Software rasterisation reached ~31 FPS in the intro; a second run reached
   ~60 vblanks/s and ~55 presented frames/s at 720p. The Windows target is 60 FPS; the Deck's
   AMD GPU should be much faster than llvmpipe, but 60 FPS is not claimed until measured.
3. **Steam Input.** The game reads controllers through SDL2. In Gaming Mode Steam Input normally
   presents the Deck controls as an X360 pad. Desktop Mode may need Steam Input enabled or an
   external pad. The in-game picker and boot menu now accept controller navigation.
4. **AppImage runtime.** `build-appimage.sh` bundles FFmpeg and other non-core libraries; the
   AppImage still needs an independent run on a clean SteamOS/Deck environment.
5. **Movie audio sync, memory-card persistence across restarts, save states and two-controller
   Versus mode** are implemented but not all were exercised in this audit.

## Build/test evidence recorded in this branch

- Translator: GCC 11.4, CMake 4.4.4, Ninja; `ps2_recomp` built and produced 37,387 `.cpp` files.
- Runtime/game: `ps2EntryRunner` linked on Linux x86-64 (172,433,728 bytes at the recorded build).
- Boot: front-end extracted `SLUS_201.48`, `SYSTEM.CNF` and 27 IOP modules from the supplied
  undub ISO (verified 3,001,304 bytes, CRC-32 `A0A48DD1`), then loaded the ELF.
- Rendering: OpenGL 3.3 context (Mesa llvmpipe), GS shaders compiled, Jupiter intro frame captured.
- Audio: SPU2 stream opened at 48 kHz stereo; samples/underrun counters were non-zero/zero as
  expected in the test run.
- Filesystem: settings and extracted data appeared under
  `~/.local/share/PS2Recomp/SLUS-20148/`; a second run loaded them and skipped extraction.
- Disc picker: selected a symlinked ISO through the in-game browser and reached the game.
- Stub review: 329 stand-ins, 329 reviewed, 0 problems.
