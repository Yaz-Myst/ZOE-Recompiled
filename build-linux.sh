#!/usr/bin/env bash
# Build ZOE-Recompiled natively on Linux (Steam Deck / SteamOS, x86-64).
#
#   ./build-linux.sh --iso "/path/to/Zone of the Enders.iso"
#   ./build-linux.sh --elf ZOE-Recompiled/game/SLUS_201.48
#   ./build-linux.sh --skip-regen          # after the game code was generated once
#
# The game program is your own copy of SLUS_201.48 (USA, 3,001,304 bytes, CRC-32 A0A48DD1).
# Generated game C++ stays in the git-ignored work/output folder and is never committed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME="${PS2_GAME:-zoe}"
BUILD_DIR="${PS2X_BUILD_DIR:-$ROOT/build-linux}"
JOBS="${JOBS:-}"
ELF=""
ISO=""
SKIP_REGEN=0
SKIP_TRANSLATOR=0
PORTABLE_CPU=0
CLEAN=0
GENERATED_OVERRIDE=""

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

options:
  --iso PATH             extract SLUS_201.48 from your own ISO 9660 image
  --elf PATH             use this SLUS_201.48 (default: ZOE-Recompiled/game/SLUS_201.48)
  --build-dir PATH       CMake build root (default: <repo>/build-linux)
  --generated-dir PATH   where regenerated game C++ goes (default: ZOE-Recompiled/work/output)
  --jobs N               parallel compile jobs (default: min(nproc, 6))
  --skip-regen           reuse the existing generated sources
  --skip-translator      reuse an already built ps2_recomp
  --portable-cpu         x86-64-v2 instead of AVX2 (older CPUs, slower)
  --clean                remove the CMake build root first
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --iso) ISO="$2"; shift 2 ;;
        --elf) ELF="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --generated-dir) GENERATED_OVERRIDE="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --skip-regen) SKIP_REGEN=1; shift ;;
        --skip-translator) SKIP_TRANSLATOR=1; shift ;;
        --portable-cpu) PORTABLE_CPU=1; shift ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done

say() { printf '\n== %s\n' "$*"; }
fail() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

[ -f "$ROOT/games/lib.sh" ] || fail "run this script from the repository checkout (games/lib.sh missing)"
. "$ROOT/games/lib.sh"
game_pick_arg -g "$GAME"
game_load "$GAME"
GAME_CFG="$(game_find_cfg "$GAME")"

if [ -n "$GENERATED_OVERRIDE" ]; then
    export PS2X_GENERATED_DIR="$GENERATED_OVERRIDE"
    game_load "$GAME"
fi
GENERATED_DIR="${PS2X_GENERATED_DIR:-$G_GENERATED}"
GENERATED_DIR="$(python3 -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "$GENERATED_DIR")"
[ -n "$ELF" ] || ELF="$G_ELF"
BUILD_DIR="$(python3 -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "$BUILD_DIR")"
[ -n "$JOBS" ] || JOBS="$(( $(nproc) < 6 ? $(nproc) : 6 ))"
[ "$JOBS" -ge 1 ] 2>/dev/null || fail "--jobs needs a number"
if [ "$CLEAN" = 1 ]; then
    say "clean: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

say "checking build dependencies"
missing=""
need() { command -v "$1" >/dev/null 2>&1 || missing="$missing $1"; }
need cmake
need python3
need pkg-config
need c++
if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
else
    GENERATOR="Unix Makefiles"
    echo "  note: ninja not found; using Unix Makefiles (slower)"
fi
for module in libavcodec libavformat libavutil libswresample libswscale; do
    pkg-config --exists "$module" 2>/dev/null || missing="$missing $module-dev"
done
[ -f /usr/include/GL/gl.h ] || [ -f /usr/local/include/GL/gl.h ] || missing="$missing libgl-dev"
[ -f /usr/include/X11/Xlib.h ] || [ -f /usr/local/include/X11/Xlib.h ] || missing="$missing libx11-dev"
if [ -n "$missing" ]; then
    cat >&2 <<EOF

Missing dependencies:$missing

Debian/Ubuntu (SteamOS uses Arch; see docs/steamdeck-build.md):
  sudo apt-get install build-essential cmake ninja-build python3 pkg-config \\
    libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \\
    libasound2-dev libpulse-dev libavcodec-dev libavformat-dev libavutil-dev \\
    libswresample-dev libswscale-dev
EOF
    exit 1
fi
echo "  cmake: $(cmake --version | head -1)"
echo "  compiler: $(${CXX:-c++} --version | head -1)"
echo "  generator: $GENERATOR"
echo "  jobs: $JOBS"

# ---- 1. translator --------------------------------------------------------------------------
TRANSLATOR="$BUILD_DIR/translator/ps2xRecomp/ps2_recomp"
if [ "$SKIP_TRANSLATOR" = 0 ]; then
    say "configuring and building the PS2Recomp translator"
    cmake -S "$ROOT/PS2Recomp" -B "$BUILD_DIR/translator" -G "$GENERATOR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPS2X_BUILD_RECOMP=ON \
        -DPS2X_BUILD_RUNTIME=OFF \
        -DPS2X_BUILD_ANALYZER=OFF \
        -DPS2X_BUILD_TEST=OFF \
        -DPS2X_BUILD_STUDIO=OFF
    cmake --build "$BUILD_DIR/translator" --target ps2_recomp -j "$JOBS"
fi
[ -x "$TRANSLATOR" ] || fail "translator not found: $TRANSLATOR"
echo "  translator: $TRANSLATOR"

# ---- 2. user-supplied game program ----------------------------------------------------------
say "checking the game program"
if [ ! -f "$ELF" ]; then
    if [ -z "$ISO" ]; then
        fail "missing $ELF. Pass --elf /path/to/SLUS_201.48 or --iso /path/to/your.iso."
    fi
    [ -f "$ISO" ] || fail "ISO not found: $ISO"
    echo "  extracting SLUS_201.48 from your image (this is your own game data; it stays git-ignored)"
    mkdir -p "$(dirname "$ELF")"
    python3 "$ROOT/tools/iso_extract.py" "$ISO" --file SLUS_201.48 --out "$(dirname "$ELF")" \
        --size 3001304 --crc32 A0A48DD1 || { rm -f "$ELF"; fail "could not extract a valid SLUS_201.48"; }
fi

python3 - "$ELF" <<'PY'
import sys, zlib, struct
path = sys.argv[1]
data = open(path, 'rb').read()
if len(data) != 3001304 or zlib.crc32(data) & 0xFFFFFFFF != 0xA0A48DD1:
    sys.exit("%s is not the supported Zone of the Enders program (need 3,001,304 bytes, CRC-32 A0A48DD1)" % path)
if data[:4] != b'\x7fELF' or struct.unpack_from('<H', data, 18)[0] != 8:
    sys.exit("%s is not a 32-bit little-endian MIPS ELF" % path)
print("  verified: %s (%d bytes, CRC-32 A0A48DD1)" % (path, len(data)))
PY

# ---- 3. regenerate the game's C++ -----------------------------------------------------------
if [ "$SKIP_REGEN" = 0 ]; then
    say "regenerating the recompiled game code"
    mkdir -p "$GENERATED_DIR"
    export PS2X_RECOMP="$TRANSLATOR"
    export PS2X_GENERATED_DIR="$GENERATED_DIR"
    bash "$ROOT/work/regen.sh" -g "$GAME"
else
    say "reusing generated game code"
fi
COUNT=$(find "$GENERATED_DIR" -maxdepth 1 -name '*.cpp' 2>/dev/null | wc -l)
[ "$COUNT" -gt 0 ] || fail "no generated .cpp files in $GENERATED_DIR (drop --skip-regen or run work/regen.sh)"
echo "  generated files: $COUNT"

# Stub review gate from build.ps1: no function may silently become a runtime stand-in.
STUB_REVIEW=$(awk -F= '/^STUB_REVIEW=/{print $2}' "$GAME_CFG" | tr -d '\r')
LIB_START=$(awk -F= '/^LIB_START=/{print $2}' "$GAME_CFG" | tr -d '\r')
if [ -n "$STUB_REVIEW" ]; then
    REVIEW_PATH="$(cd "$(dirname "$GAME_CFG")" && pwd)/$STUB_REVIEW"
    if [ -f "$REVIEW_PATH" ] && [ -n "$LIB_START" ]; then
        say "checking the runtime stand-in review"
        python3 "$ROOT/tools/check_stubs.py" "$REVIEW_PATH" "$LIB_START" "$GENERATED_DIR"
    fi
fi

# ---- 4. runtime + generated game ------------------------------------------------------------
say "configuring and building ps2EntryRunner"
EXTRA_SOURCES=""
for file in "$ROOT/ZOE-Recompiled/src"/*.cpp; do
    [ -f "$file" ] || continue
    EXTRA_SOURCES="${EXTRA_SOURCES:+$EXTRA_SOURCES;}$file"
done

cmake -S "$ROOT/PS2Recomp" -B "$BUILD_DIR/game" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPS2X_BUILD_RECOMP=OFF \
    -DPS2X_BUILD_RUNTIME=ON \
    -DPS2X_BUILD_ANALYZER=OFF \
    -DPS2X_BUILD_TEST=OFF \
    -DPS2X_BUILD_STUDIO=OFF \
    -DPS2X_RUNNER_GENERATED_DIR="$GENERATED_DIR" \
    -DPS2X_RUNNER_EXTRA_SOURCES="$EXTRA_SOURCES" \
    -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF \
    -DPS2X_PORTABLE_CPU="$([ "$PORTABLE_CPU" = 1 ] && echo ON || echo OFF)"

cmake --build "$BUILD_DIR/game" --target ps2EntryRunner -j "$JOBS"

EXE="$BUILD_DIR/game/ps2xRuntime/ps2EntryRunner"
[ -x "$EXE" ] || fail "link finished but the executable is missing: $EXE"

say "BUILD OK"
printf '  executable: %s\n' "$EXE"
printf '  size:       %s bytes\n' "$(stat -c '%s' "$EXE")"
printf '  generated:  %s (%s files)\n' "$GENERATED_DIR" "$COUNT"
printf '\nRun it (the boot screen asks for your disc image on first start):\n  %q\n' "$EXE"
