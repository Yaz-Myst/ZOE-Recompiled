#!/usr/bin/env bash
# Package the native Linux build as a Steam Deck AppImage.
#
#   ./build-appimage.sh
#   ./build-appimage.sh --exe build-linux/game/ps2xRuntime/ps2EntryRunner --output dist/
#
# The AppImage contains only the recompiled executable and redistributable libraries. It never
# contains game assets; the player supplies their own disc image through the boot screen.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PS2X_BUILD_DIR:-$ROOT/build-linux}"
EXE="$BUILD_DIR/game/ps2xRuntime/ps2EntryRunner"
OUT_DIR="$ROOT/dist"
NAME="ZOE-Recompiled-SteamDeck-x86_64.AppImage"

usage() {
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

options:
  --exe PATH       ps2EntryRunner to package (default: build-linux/game/ps2xRuntime/ps2EntryRunner)
  --output DIR     output folder (default: dist/)
  --no-download    fail instead of downloading linuxdeploy/appimagetool
EOF
}

DOWNLOAD=1
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) EXE="$2"; shift 2 ;;
        --output) OUT_DIR="$2"; shift 2 ;;
        --no-download) DOWNLOAD=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done

fail() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }
[ -x "$EXE" ] || fail "native executable not found: $EXE (run ./build-linux.sh first)"
for tool in curl patchelf mksquashfs; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing $tool (Debian/Ubuntu: sudo apt-get install patchelf squashfs-tools curl)"
done
command -v convert >/dev/null 2>&1 || fail "missing ImageMagick convert (needed to make the icon)"

TOOLS="$BUILD_DIR/tools"
mkdir -p "$TOOLS" "$OUT_DIR"

fetch_tool() { # $1 = local name, $2 = URL
    local path="$TOOLS/$1"
    if [ -x "$path" ]; then echo "$path"; return; fi
    [ "$DOWNLOAD" = 1 ] || fail "$1 not found in $TOOLS (run without --no-download)"
    echo "downloading $1" >&2
    curl -L --fail --retry 3 -o "$path" "$2"
    chmod +x "$path"
    echo "$path"
}

LINUXDEPLOY=$(fetch_tool linuxdeploy-x86_64.AppImage \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage")
APPIMAGETOOL=$(fetch_tool appimagetool-x86_64.AppImage \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage")

APPDIR="$BUILD_DIR/appdir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
         "$APPDIR/usr/share/doc/zoe-recompiled"

install -m 0755 "$EXE" "$APPDIR/usr/bin/zoe-recompiled"
install -m 0644 "$ROOT/packaging/zoe-recompiled.desktop" "$APPDIR/usr/share/applications/zoe-recompiled.desktop"
install -m 0644 "$ROOT/packaging/zoe-recompiled.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/zoe-recompiled.png"
install -m 0644 "$ROOT/packaging/zoe-recompiled.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/zoe-recompiled.svg" 2>/dev/null || true
cp "$ROOT/LICENSE" "$APPDIR/usr/share/doc/zoe-recompiled/LICENSE"
cp "$ROOT/README.md" "$APPDIR/usr/share/doc/zoe-recompiled/README.md"

# Bundle libraries the game links dynamically (FFmpeg, SDL/raylib dependencies), but never
# bundle the host's graphics driver, libc, or the X11/GL core the desktop must provide.
export APPIMAGE_EXTRACT_AND_RUN=1
"$LINUXDEPLOY" --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/zoe-recompiled" \
    --desktop-file "$APPDIR/usr/share/applications/zoe-recompiled.desktop" \
    --icon-file "$ROOT/packaging/zoe-recompiled.png" \
    --exclude-library "libGL.so*" --exclude-library "libGLX.so*" --exclude-library "libOpenGL.so*" \
    --exclude-library "libEGL.so*" --exclude-library "libGLdispatch.so*" --exclude-library "libdrm.so*" \
    --exclude-library "libX11.so*" --exclude-library "libXext.so*" --exclude-library "libXcursor.so*" \
    --exclude-library "libXi.so*" --exclude-library "libXrandr.so*" --exclude-library "libXinerama.so*" \
    --exclude-library "libxkbcommon.so*" --exclude-library "libwayland-*.so*" \
    --exclude-library "libasound.so*" --exclude-library "libpulse.so*" --exclude-library "libpipewire-*.so*" \
    --exclude-library "libudev.so*" --exclude-library "libsystemd.so*"

install -m 0755 "$ROOT/packaging/AppRun" "$APPDIR/AppRun"

OUT="$OUT_DIR/$NAME"
rm -f "$OUT"
ARCH=x86_64 "$APPIMAGETOOL" --appimage-extract-and-run "$APPDIR" "$OUT"
chmod +x "$OUT"

echo
echo "AppImage: $OUT"
echo "size:     $(stat -c '%s' "$OUT") bytes"
echo "Test on the Deck: mark it executable, double-click, or add it to Steam as a non-Steam game."
