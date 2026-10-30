#!/bin/bash
# Ghidra-analyze each overlay ELF and export a PS2Recomp TOML/CSV with overlay_name set.
# usage: analyze_overlays.sh [-g GAME] MODULE...     (GAME defaults to zoe, or $PS2_GAME)
#   MODULE = an overlay name (work/overlays/<NAME>.elf), or MAIN for the game's boot ELF
#            (imported from the ELF path in games/<game>.cfg, exported to <work>/<elf>.toml).
. "$(dirname "${BASH_SOURCE[0]}")/../games/lib.sh"
game_pick_arg "$@"; shift $GAME_SHIFT
game_load "$GAME"
export JAVA_HOME="$G_JAVA_HOME"
W=$G_WORK
WW=$CFG_WORK_DIR      # same dir, forward-slash Windows form (goes into the properties file)
MAIN_NAME=$(basename "$G_ELF")
PROPS=$G_PROPS_DIR/ExportPS2Functions.properties
mkdir -p "$G_PROPS_DIR" "$G_GHIDRA_PROJECT_DIR" "$W/overlays"

write_props() {  # $1 = toml, $2 = csv (forward-slash Windows paths)
  printf 'Choose output TOML config file Save = %s\nChoose output CSV file Save = %s\n' "$1" "$2" > "$PROPS"
}

for m in "$@"; do
  if [ "$m" = MAIN ]; then
    elf=$CFG_ELF; toml=$W/$MAIN_NAME.toml; log=$W/ghidra_main.log
    write_props "$WW/$MAIN_NAME.toml" "$WW/${MAIN_NAME}_map.csv"
    out=$CFG_GENERATED_DIR; extra=""
  else
    elf=$WW/overlays/$m.elf; toml=$W/overlays/$m.toml; log=$W/overlays/$m.ghidra.log
    write_props "$WW/overlays/$m.toml" "$WW/overlays/${m}_map.csv"
    out=$WW/overlays/out_$m; extra="overlay_name = \"$m\""
  fi
  [ -f "$(cygpath -u "$elf")" ] || { echo "$m: missing $elf"; continue; }
  # Optional per-game Ghidra scripts, run right before the export on MAIN only (e.g. to apply known
  # function names, since the exporter classifies stubs by name): GHIDRA_PRE_EXPORT_SCRIPTS=A.java;B.java
  # in games/<game>.cfg, found in the game's <work>/scripts. That directory is passed as -scriptPath in a
  # first headless pass (import + analysis + these scripts); the export then runs as a second pass with the
  # shared scripts. Two passes because a ';' in -scriptPath would be split by cmd.exe, and headless skips
  # every later script once one fails. Empty/unset: single pass, command line unchanged.
  AH="$(cygpath -w "$G_GHIDRA_HOME")\\support\\analyzeHeadless.bat"
  if [ "$m" = MAIN ] && [ -n "$CFG_GHIDRA_PRE_EXPORT_SCRIPTS" ]; then
    pre_args=()
    IFS=';' read -r -a _pre <<< "$CFG_GHIDRA_PRE_EXPORT_SCRIPTS"
    for _s in "${_pre[@]}"; do [ -n "$_s" ] && pre_args+=(-postScript $_s); done   # unquoted on purpose: an entry may carry script args ("Foo.java arg")
    cmd //c "$AH" "$(cygpath -w "$G_GHIDRA_PROJECT_DIR")" "$G_GHIDRA_PROJECT" -import "$elf" -overwrite -processor 'r5900:LE:32:default' -scriptPath "$(cygpath -w "$G_PROPS_DIR")" -propertiesPath "$(cygpath -w "$G_PROPS_DIR")" "${pre_args[@]}" < /dev/null > "$log" 2>&1
    if grep -q "SCRIPT ERROR" "$log"; then echo "$m: a pre-export script failed, see $log"; continue; fi
    cmd //c "$AH" "$(cygpath -w "$G_GHIDRA_PROJECT_DIR")" "$G_GHIDRA_PROJECT" -process "$(basename "$elf")" -noanalysis -scriptPath "$(cygpath -w "$G_SCRIPTS")" -propertiesPath "$(cygpath -w "$G_PROPS_DIR")" -postScript ExportPS2Functions.java < /dev/null >> "$log" 2>&1
  else
  # (the properties file is looked up in -propertiesPath first, then next to the script)
  cmd //c "$(cygpath -w "$G_GHIDRA_HOME")\\support\\analyzeHeadless.bat" "$(cygpath -w "$G_GHIDRA_PROJECT_DIR")" "$G_GHIDRA_PROJECT" -import "$elf" -overwrite -processor 'r5900:LE:32:default' -scriptPath "$(cygpath -w "$G_SCRIPTS")" -propertiesPath "$(cygpath -w "$G_PROPS_DIR")" -postScript ExportPS2Functions.java < /dev/null > "$log" 2>&1
  fi
  python - "$toml" "$out" "$extra" <<'PY'
import sys,re
p,out,extra=sys.argv[1:4]; s=open(p).read()
repl = f'output = "{out}"' + ('\n' + extra if extra else '')
s=re.sub(r'output = .*', lambda _: repl, s, count=1)
open(p,'w').write(s)
PY
  echo "$m: $(grep -o 'Exported [0-9]* functions[^(]*' "$log")"
done
# restore main-ELF properties (so a plain Ghidra GUI export still targets the right file by default)
write_props "$WW/$MAIN_NAME.toml" "$WW/${MAIN_NAME}_map.csv"
