#!/bin/bash
# usage: ghidra.sh [-g GAME] <program> <outfile> args...   (runs DecompDump on an existing project program)
# GAME defaults to zoe (or $PS2_GAME); the Ghidra project comes from the game's <name>.cfg.
. "$(dirname "${BASH_SOURCE[0]}")/../games/lib.sh"
game_pick_arg "$@"; shift $GAME_SHIFT
game_load "$GAME"
export JAVA_HOME="$G_JAVA_HOME"
prog=$1; shift
cmd //c "$(cygpath -w "$G_GHIDRA_HOME")\support\analyzeHeadless.bat" "$(cygpath -w "$G_GHIDRA_PROJECT_DIR")" "$G_GHIDRA_PROJECT" -process "$prog" -noanalysis -readOnly -scriptPath "$(cygpath -w "$G_SCRIPTS")" -postScript DecompDump.java "$@" > "$G_WORK/ghidra_last.log" 2>&1
grep -E "ERROR|Exception" "$G_WORK/ghidra_last.log" | head -5
