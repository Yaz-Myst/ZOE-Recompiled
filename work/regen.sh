#!/bin/bash
# Regenerate recompiled C++ for the main ELF and all overlays into scratch folders, then copy
# only changed files into the real output folders so the next build recompiles only those.
# usage: regen.sh [-g GAME] [MAIN] [OVERLAY ...]   (default game: zoe; default: main + all overlays)
# Per-game paths and the overlay list come from games/<game>.cfg.
set -e
PYTHON=${PYTHON:-$(command -v python3 || command -v python)}
[ -n "$PYTHON" ] || { echo "python3 not found"; exit 1; }
. "$(dirname "${BASH_SOURCE[0]}")/../games/lib.sh"
game_pick_arg "$@"; shift $GAME_SHIFT
game_load "$GAME"
W=$G_WORK
RECOMP=$G_RECOMP
SCRATCH=${PS2X_SCRATCH_DIR:-$W/regen_tmp}

sync_dir() {  # $1 = new dir, $2 = live dir (python: fast byte compare, copy only changed)
  "$PYTHON" - "$1" "$2" <<'PY'
import os,sys,shutil
new,live=sys.argv[1:3]; changed=0
for name in os.listdir(new):
    a=os.path.join(new,name); b=os.path.join(live,name)
    with open(a,'rb') as f: da=f.read()
    try:
        with open(b,'rb') as f: same = f.read()==da
    except FileNotFoundError: same=False
    if not same:
        shutil.copyfile(a,b); changed+=1
# drop files the generator no longer produces (e.g. removed entry points), so CMake's glob
# doesn't keep compiling stale functions
fresh=set(os.listdir(new)); removed=0
for name in os.listdir(live):
    if name not in fresh and os.path.isfile(os.path.join(live,name)):
        os.remove(os.path.join(live,name)); removed+=1
print(f"  {os.path.basename(live)}: {changed} file(s) changed, {removed} removed", flush=True)
PY
}

regen() {  # $1 = toml, $2 = live output dir
  local tmp=$SCRATCH/$(basename "$2")
  local tmp_native=$(_m "$tmp")
  rm -rf "$tmp"; mkdir -p "$tmp"
  # Rewrite the three machine-specific paths in the shipped TOML from the game cfg, so the
  # repository carries no developer's drive letters and the same file works on Windows and Linux.
  "$PYTHON" - "$1" "$tmp_native" "$SCRATCH/cfg.toml" "$G_ELF_M" "$G_GHIDRA_MAP_M" <<'PY'
import sys,re
src,out,dst,inp,ghidra=sys.argv[1:6]
s=open(src).read()
for key,value in (("input", inp), ("output", out), ("ghidra_output", ghidra)):
    value=value.replace('\\','/')
    s,n=re.subn(r'(?m)^%s = .*$' % key, '%s = "%s"' % (key, value), s, count=1)
    if not n:
        s += '\n%s = "%s"\n' % (key, value)
open(dst,'w').write(s)
PY
  [ -x "$RECOMP" ] || { echo "recompiler not found or not executable: $RECOMP"; exit 1; }
  "$RECOMP" "$SCRATCH/cfg.toml" > "$tmp.log" 2>&1 || { echo "recomp failed: $1"; tail -20 "$tmp.log"; exit 1; }
  mkdir -p "$2"
  sync_dir "$tmp" "$2"
}

mkdir -p $SCRATCH
# optional args: module names to regenerate (MAIN for the ELF, overlay names otherwise); default all
mods="${*:-MAIN ${G_OVERLAYS[*]}}"
echo "regen: game=$G_NAME modules: $mods"
for m in $mods; do
  if [ "$m" = MAIN ]; then
    [ -f "$W/$(basename "$G_ELF").toml" ] || { echo "missing $W/$(basename "$G_ELF").toml (run the Ghidra export first)"; exit 1; }
    regen "$W/$(basename "$G_ELF").toml" "$G_GENERATED"
  else
    [ -f "$W/overlays/$m.toml" ] || { echo "missing $W/overlays/$m.toml"; exit 1; }
    regen "$W/overlays/$m.toml" "$W/overlays/out_$m"
  fi
done
