# Unattended run into the first battle (route_battle1_ticks.txt with the game-time input clock).
# usage: battle_run.ps1 -Log name [-Seconds 175] [-Profile "140,170"] [-Capture "file,firstTick,lastTick"] [-Audio]
param([string]$Log = "battle", [int]$Seconds = 175, [string]$Profile = "", [string]$Capture = "", [switch]$Audio, [string]$Env = "", [string]$Route = "route_battle1_ticks.txt")
$d = Join-Path $PSScriptRoot $Log   # this script lives in ZOE-Recompiled\work
New-Item -ItemType Directory -Force "$d\frames", "$d\diag" | Out-Null
$env:PS2X_GPU = "1"
$env:PS2X_GS_THREAD = "2"   # same as play.bat; -Env "PS2X_GS_THREAD=0" for the single-threaded path
$env:PS2X_INPUT_LIVE = "0"         # script only: ignore keyboard/controllers (safe beside a game being played)
$env:PS2X_WINDOW_UNFOCUSED = "1"   # window visible but does not take the focus
$env:PS2X_AUDIO = $(if ($Audio) { $null } else { "0" })
$env:PS2X_DUMP_FRAMES = "$d\frames"
$env:PS2X_DIAG_DIR = "$d\diag"
$env:PS2X_DIAG_AT = $null
$env:PS2X_PROFILE_WINDOW = $(if ($Profile) { $Profile } else { $null })
$env:PS2X_VU_CAPTURE = $(if ($Capture) { $Capture } else { $null })
$env:PS2X_VU_BENCH = $null
$env:PS2X_INPUT_CLOCK = "ticks"
$env:PS2X_INPUT_RECORD = $null
# The scripted title-menu routes assume the stock menu: the user's "Unlock Versus mode" setting adds an entry and
# shifts them (29 Oct: the battle route then died ~50 s early). Off for scripted runs; -Env can override.
$env:PS2X_ZOE_VERSUS_UNLOCK = "0"
$env:PS2X_INPUT_SCRIPT = (Get-Content (Join-Path $PSScriptRoot $Route) -Raw).Trim().TrimEnd(',')
$extra = @()
foreach ($kv in ($Env -split ';')) { if ($kv -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2]; $extra += $Matches[1] } }
& (Join-Path $PSScriptRoot '..\..\work\run.ps1') -Game zoe -Seconds $Seconds -Log $Log *> "$d\run_console.txt"   # shared runner at <repo root>\work
Get-Content "$d\run_console.txt"
foreach ($k in $extra) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
