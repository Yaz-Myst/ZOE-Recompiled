# Replay captured VU calls through the current build's fast VU engine (and compiled programs).
# usage: vu_bench.ps1 [-Opts "iters=3,check=...,checkr=...,aotgen=..."] [-Capture "a.bin|b.bin"] [-Flags] [-NoAot] [-Exe ...]
#   -Capture: file names in work\ (or full paths), several joined with '|' (default vucap_battle.bin)
#   -NoAot:   interpreter only (PS2X_VU_AOT=0), e.g. to make reference checksums for a new capture
param([string]$Exe = "ps2EntryRunner.exe", [string]$Opts = "iters=3", [switch]$Flags, [string]$Capture = "vucap_battle.bin", [switch]$NoAot)
$w = $PSScriptRoot   # this script lives in ZOE-Recompiled\work
$caps = ($Capture -split "\|" | ForEach-Object { if ($_ -match ":") { $_ } else { "$w\$_" } }) -join "|"
$env:PS2X_VU_BENCH = "$caps,$Opts"
$env:PS2X_VU_FLAGS = $(if ($Flags) { "1" } else { $null })
$env:PS2X_VU_AOT = $(if ($NoAot) { "0" } else { $null })
Push-Location (Join-Path $PSScriptRoot '..\build\ps2xRuntime\Release')
& ".\$Exe" "x" 2>&1 | Where-Object { $_ -match '\[vu:bench\]|\[vu:aotgen\]' }
"exit $LASTEXITCODE"
Pop-Location
$env:PS2X_VU_BENCH = $null
$env:PS2X_VU_FLAGS = $null
$env:PS2X_VU_AOT = $null
