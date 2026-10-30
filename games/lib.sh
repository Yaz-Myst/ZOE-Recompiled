#!/bin/bash
# Shared helpers for the bash scripts in work/ (source this, do not run it).
#
#   . "$(dirname "${BASH_SOURCE[0]}")/../games/lib.sh"
#   game_pick_arg "$@"; shift $GAME_SHIFT     # optional leading "-g <game>" / "--game <game>"
#   game_load "$GAME"                         # reads games/<game>.cfg, else ../<dir>/<game>.cfg
#
# After game_load these are set (paths in Git-Bash style, /a/PS2Recomp/...):
#   G_NAME G_ELF G_DISC_DIR G_CD_IMAGE G_WORK G_GHIDRA_PROJECT_DIR G_GHIDRA_PROJECT
#   G_GENERATED G_BUILD_DIR G_OVERLAYS (array of names) G_EXTRA_SOURCES (raw ';' list)
# and the shared tool locations G_GHIDRA_HOME, G_JAVA_HOME, G_SCRIPTS (Ghidra scripts, shared
# by all games), G_PROPS_DIR (per-game Ghidra script parameters), G_RECOMP (recompiler exe).

GAMES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Default game: $PS2_GAME, else zoe.
game_pick_arg() {
  GAME=${PS2_GAME:-zoe}
  GAME_SHIFT=0
  if [ "$1" = -g ] || [ "$1" = --game ]; then
    [ -n "$2" ] || { echo "$1 needs a game name" >&2; exit 2; }
    GAME=$2; GAME_SHIFT=2
  fi
}

# A game's config is games/<name>.cfg, else <repo root>/<any dir>/<name>.cfg (one level down;
# e.g. ZOE-Recompiled/zoe.cfg). Relative paths in a cfg are taken from the cfg's own folder.
game_find_cfg() {
  local f
  for f in "$GAMES_DIR/$1.cfg" "$GAMES_DIR"/../*/"$1.cfg"; do
    [ -f "$f" ] && { echo "$f"; return 0; }
  done
  return 1
}

game_load() {
  local name=$1 cfg k v
  if ! cfg=$(game_find_cfg "$name"); then
    echo "unknown game '$name': no games/$name.cfg and no <repo root>/*/$name.cfg (known: $(ls "$GAMES_DIR"/*.cfg "$GAMES_DIR"/../*/*.cfg 2>/dev/null | sed 's#.*/##; s/\.cfg$//' | sort -u | tr '\n' ' '))" >&2
    exit 2
  fi
  # plain KEY=VALUE parse (no quoting rules: the value is everything after the first '=')
  while IFS='=' read -r k v || [ -n "$k" ]; do
    k=${k%$'\r'}; v=${v%$'\r'}
    case $k in ''|'#'*) continue ;; esac
    printf -v "CFG_$k" '%s' "$v"
  done < "$cfg"

  # cfg path -> Git-Bash path (/a/x). Absolute values (A:/x, /a/x) are converted; relative ones (work/output) are
  # taken from the cfg's own folder.
  local cfgdir root
  cfgdir="$(cd "$(dirname "$cfg")" && pwd)"
  root="$(cd "$GAMES_DIR/.." && pwd)"
  _u() {
    case $1 in
      '') echo "" ;;
      /*|[A-Za-z]:*) cygpath -u "$1" ;;
      *) echo "$cfgdir/$1" ;;
    esac
  }
  G_NAME=$CFG_NAME
  G_ELF=$(_u "$CFG_ELF")
  G_DISC_DIR=$(_u "$CFG_DISC_DIR")
  G_CD_IMAGE=$(_u "$CFG_CD_IMAGE")
  G_WORK=$(_u "$CFG_WORK_DIR")
  G_GHIDRA_PROJECT_DIR=$(_u "$CFG_GHIDRA_PROJECT_DIR")
  G_GHIDRA_PROJECT=$CFG_GHIDRA_PROJECT
  G_GENERATED=$(_u "$CFG_GENERATED_DIR")
  G_BUILD_DIR=$(_u "$CFG_BUILD_DIR")
  # ';' list of override sources/dirs, each resolved like the paths above
  local _item _extra
  G_EXTRA_SOURCES=""
  IFS=';' read -r -a _extra <<< "$CFG_EXTRA_SOURCES"
  for _item in "${_extra[@]}"; do
    [ -n "$_item" ] && G_EXTRA_SOURCES="${G_EXTRA_SOURCES:+$G_EXTRA_SOURCES;}$(_u "$_item")"
  done
  IFS=';' read -r -a G_OVERLAYS <<< "$CFG_OVERLAYS"

  # shared tools (same for every game), Windows-style (A:/x) because they go to .bat files and JAVA_HOME.
  # Ghidra and the JDK are not part of the repository: PS2X_GHIDRA_HOME / PS2X_JAVA_HOME point at your own install.
  local root_w
  root_w="$(cygpath -m "$root")"
  G_GHIDRA_HOME=${PS2X_GHIDRA_HOME:-$root_w/tools/ghidra_12.1.3_PUBLIC}
  G_JAVA_HOME=${PS2X_JAVA_HOME:-$root_w/tools/jdk-21.0.12.1+1}
  G_SCRIPTS=$root_w/work/scripts
  # Ghidra script parameters (ExportPS2Functions.properties) live per game, in <work>/scripts.
  G_PROPS_DIR=$G_WORK/scripts
  # translator: from the game's own source tree (cfg SOURCE_DIR) when it has one, else <repo root>/PS2Recomp
  G_SOURCE=$(_u "${CFG_SOURCE_DIR:-$root/PS2Recomp}")
  G_RECOMP=$G_SOURCE/out/build/ps2xRecomp/Release/ps2_recomp.exe
}
