#!/bin/bash
# Regenerate recompiled C++ for the main ELF and all overlays into scratch folders, then copy
# only changed files into the real output folders so the next build recompiles only those.
# usage: regen.sh [-g GAME] [MAIN] [OVERLAY ...]   (default game: zoe; default: main + all overlays)
# Per-game paths and the overlay list come from games/<game>.cfg.
set -e
. "$(dirname "${BASH_SOURCE[0]}")/../games/lib.sh"
game_pick_arg "$@"; shift $GAME_SHIFT
game_load "$GAME"
W=$G_WORK
RECOMP=$G_RECOMP
SCRATCH=$W/regen_tmp

sync_dir() {  # $1 = new dir, $2 = live dir (python: fast byte compare, copy only changed)
  python - "$1" "$2" <<'PY'
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
  rm -rf "$tmp"; mkdir -p "$tmp"
  python - "$1" "$tmp" "$SCRATCH/cfg.toml" <<'PY'
import sys,re
src,out,dst=sys.argv[1:4]
s=open(src).read()
s=re.sub(r'(?m)^output = .*$', 'output = "%s"' % out.replace('\\','/'), s, count=1)
open(dst,'w').write(s)
PY
  "$RECOMP" "$SCRATCH/cfg.toml" > "$tmp.log" 2>&1 || { echo "recomp failed: $1"; tail -5 "$tmp.log"; exit 1; }
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
