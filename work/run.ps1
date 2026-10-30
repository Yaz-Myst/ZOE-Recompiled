# Run the recompiled game for a fixed time and capture logs.
# usage: run.ps1 [-Seconds 60] [-Log run] [-Game zoe]     (game: <name>.cfg; default zoe or $env:PS2_GAME)
param([int]$Seconds = 60, [string]$Log = "run", [string]$Game = $(if ($env:PS2_GAME) { $env:PS2_GAME } else { "zoe" }))
. "$PSScriptRoot\..\games\lib.ps1"
$g = Get-GameConfig $Game
$exe = (ConvertTo-WinPath $g.BuildDir) + "\ps2xRuntime\Release\ps2EntryRunner.exe"
$out = (ConvertTo-WinPath $g.WorkDir) + "\$Log.log"
New-Item -ItemType Directory -Force (ConvertTo-WinPath $g.WorkDir) | Out-Null
$env:PS2X_CD_IMAGE = ConvertTo-WinPath $g.CdImage
$p = Start-Process $exe -ArgumentList ('"' + (ConvertTo-WinPath $g.Elf) + '"') -WorkingDirectory (ConvertTo-WinPath $g.DiscDir) `
    -RedirectStandardOutput $out -RedirectStandardError "$out.err" -PassThru
if (-not $p.WaitForExit($Seconds * 1000)) { Stop-Process $p -Force; "killed after ${Seconds}s" } else { "exited code $($p.ExitCode)" }
"stdout $((Get-Item $out).Length) bytes, stderr $((Get-Item "$out.err").Length) bytes"
