<#
.SYNOPSIS
  Build ps2EntryRunner for one game (games\<game>.cfg) and verify the result.

.DESCRIPTION
  usage:  .\build.ps1 -Game zoe [-DryRun]      (any game with a <name>.cfg works the same way)

  Encodes the pitfalls we hit:
   * a missing build dir is configured with that game's PS2X_RUNNER_* values and
     PS2X_ENABLE_AGRESSIVE_LOGS=OFF (upstream default ON logs every recompiled function: GBs, >50% of runtime).
   * cmake configure is ALWAYS re-run before building. When generated files were added/removed MSBuild
     otherwise keeps its stale file lists, silently skips unity chunks (including the one holding an
     overlay's register_functions.cpp) and still links (/FORCE:MULTIPLE, exit 0) -> exe with overlays missing.
   * after the build: unresolved-symbol errors (LNK2001/LNK2019) fail the run; ps2EntryRunner.map must contain a
     symbol for every generated function of the main ELF and of EVERY overlay in the game's list; the exe
     size is printed and compared with the last good build.
   * a header the generated code includes (list: games\chain-headers.txt, plus the generated ps2_recompiled_*.h)
     that is newer than this game's exe means a 35-40 min full rebuild: the build stops unless the change is
     approved (games\edit-approved.txt, valid 3 hours) or -AllowFullRebuild is passed.
  Exit code 0 = everything checked out, 1 = build or check failed.

  Note on LNK4088 ("image being generated due to /FORCE option"): CMakeLists links the runner with
  /FORCE:MULTIPLE, so this warning appears in EVERY good build (raylib/SDL2 duplicates). It is reported as
  information only; failing on it would fail every build.

  Logs: <WORK_DIR>\build_<timestamp>.log (configure + build output).
#>
[CmdletBinding()]
param(
    [string]$Game = $(if ($env:PS2_GAME) { $env:PS2_GAME } else { 'zoe' }),
    [switch]$DryRun,       # print what would be done (after the preflight checks), run nothing
    [switch]$AllowFullRebuild   # the user accepts a full rebuild after a chain-header change (see below)
)

# native commands write to stderr (CMake warnings); do not turn that into terminating errors
$ErrorActionPreference = 'Continue'
Set-StrictMode -Version 2.0
. "$PSScriptRoot\games\lib.ps1"

function Fail([string]$Msg) { Write-Host "FAILED: $Msg" -ForegroundColor Red; exit 1 }

$g        = Get-GameConfig $Game
$srcDir   = $(if ($g.SourceDir) { ConvertTo-WinPath $g.SourceDir } else { Join-Path $PSScriptRoot 'PS2Recomp' })   # per-game runtime source (cfg SOURCE_DIR)
$buildDir = ConvertTo-WinPath $g.BuildDir
$workDir  = ConvertTo-WinPath $g.WorkDir
$exe      = "$buildDir\ps2xRuntime\Release\ps2EntryRunner.exe"
$map      = "$buildDir\ps2xRuntime\Release\ps2EntryRunner.map"
$cache    = "$buildDir\CMakeCache.txt"
Write-Host "== build.ps1: game '$($g.Name)'  build dir $buildDir"

# ---- preflight: inputs must exist -------------------------------------------------------------
function Get-CppCount([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir)) { return 0 }
    @([IO.Directory]::EnumerateFiles($Dir, '*.cpp')).Count
}
$genDir = ConvertTo-WinPath $g.GeneratedDir
if ((Get-CppCount $genDir) -eq 0) { Fail "no generated .cpp files in $genDir (run: bash work/regen.sh -g $($g.Name) MAIN)" }

$overlayDirs = @()   # forward-slash paths for CMake
$overlayWin  = @{}   # name -> dir
foreach ($n in $g.Overlays) {
    $d = "$($g.WorkDir)/overlays/out_$n"
    if ((Get-CppCount (ConvertTo-WinPath $d)) -eq 0) { Fail "overlay '$n': no generated .cpp files in $(ConvertTo-WinPath $d) (run: bash work/regen.sh -g $($g.Name) $n)" }
    $overlayDirs += $d
    $overlayWin[$n] = ConvertTo-WinPath $d
}

# ---- stub review: no function may be replaced by a runtime stand-in without a recorded review ----------------------
# (tools\check_stubs.py; a wrong automatic name match once deleted ZOE's floor collision without any error)
if ($g.StubReview) {
    $review = ConvertTo-WinPath $g.StubReview
    $stubDirs = @($genDir) + @($overlayWin.Values)
    & python (Join-Path $PSScriptRoot 'tools\check_stubs.py') $review $g.LibStart @stubDirs
    if ($LASTEXITCODE -ne 0) { Fail "stub review: functions would be replaced by runtime stand-ins without review (see above; $review)" }
} else {
    Write-Host "  WARNING: no STUB_REVIEW in $($g.Name)'s config: runtime stand-ins are NOT checked (see tools\check_stubs.py)" -ForegroundColor Yellow
}

$extra = @()         # EXTRA_SOURCES entries: file, or directory (all *.cpp in it)
foreach ($e in $g.ExtraSources) {
    $w = ConvertTo-WinPath $e
    if (Test-Path -LiteralPath $w -PathType Container) {
        $extra += @(Get-ChildItem -LiteralPath $w -Filter *.cpp -File | Sort-Object Name | ForEach-Object { $_.FullName.Replace('\', '/') })
    } elseif (Test-Path -LiteralPath $w -PathType Leaf) {
        $extra += $e
    } elseif ($e -notmatch '\.[A-Za-z0-9]+$') {
        Write-Host "  note: extra-sources dir $w does not exist yet (no game-specific sources)"
    } else {
        Fail "extra source $w does not exist"
    }
}

# ---- chain headers: every generated file includes them, so a change means a 35-40 min full rebuild ----------------
# This catches edits made by any means (editor, scripts, git). The header list is games\chain-headers.txt (one path
# per line; wildcard lines are covered by the ps2_recompiled_*.h scan below); approvals are games\edit-approved.txt.
if ((Test-Path -LiteralPath $exe) -and -not $AllowFullRebuild) {
    $exeTime = (Get-Item -LiteralPath $exe).LastWriteTime
    $listFile = Join-Path $PSScriptRoot 'games\chain-headers.txt'
    $chainHeaders = @()
    if (Test-Path -LiteralPath $listFile) {
        foreach ($l in [IO.File]::ReadLines($listFile)) {
            $p = $l.Trim()
            if ($p -eq '' -or $p.StartsWith('#') -or $p.Contains('*')) { continue }
            $chainHeaders += (Join-Path $PSScriptRoot $p.Replace('/', '\'))
        }
    } else {
        Write-Host "  WARNING: $listFile is missing: chain-header check skipped" -ForegroundColor Yellow
    }
    foreach ($d in @($genDir) + @($overlayWin.Values)) {
        $chainHeaders += @(Get-ChildItem -LiteralPath $d -Filter 'ps2_recompiled_*.h' -File -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    }
    $changed = @($chainHeaders | Where-Object { (Test-Path -LiteralPath $_) -and (Get-Item -LiteralPath $_).LastWriteTime -gt $exeTime })
    if ($changed.Count -gt 0) {
        $approvalFile = Join-Path $PSScriptRoot 'games\edit-approved.txt'
        $approved = @()
        if ((Test-Path -LiteralPath $approvalFile) -and ((Get-Date) - (Get-Item -LiteralPath $approvalFile).LastWriteTime).TotalHours -lt 3) {
            $approved = @([IO.File]::ReadAllLines($approvalFile) | ForEach-Object { $_.Trim().Replace('\', '/').ToLowerInvariant() } | Where-Object { $_ -ne '' })
        }
        $unapproved = @($changed | Where-Object {
            $p = $_.Replace('\', '/').ToLowerInvariant()
            @($approved | Where-Object { $_ -eq '*' -or $p.EndsWith($_) }).Count -eq 0
        })
        Write-Host "  headers the generated code includes, changed since this game's exe was built ($exeTime):"
        $changed | ForEach-Object { Write-Host "    $_" }
        if ($unapproved.Count -gt 0) {
            Fail ("building now means a 35-40 min full rebuild of every generated file, and $($unapproved.Count) of these " +
                  "changes are not approved. Ask the user first. After they agree, list the header(s) (or *) in " +
                  "$approvalFile (valid 3 hours); the user can also run build.ps1 with -AllowFullRebuild.")
        }
        Write-Host "  approved: expect a 35-40 min full rebuild" -ForegroundColor Yellow
    }
}

# the exe is locked while running: stop early instead of failing at the link step
$running = @(Get-Process -Name ps2EntryRunner -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($_.Path -ieq $exe) })
if ($running.Count -gt 0) { Fail "$exe is running (pid $($running[0].Id)); the link would fail. Run a copy (play_bin) instead." }

# ---- cmake ------------------------------------------------------------------------------------
function Find-CMake {
    if (Test-Path -LiteralPath $cache) {   # the cmake that made this tree
        $m = Select-String -LiteralPath $cache -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$' | Select-Object -First 1
        if ($m -and (Test-Path -LiteralPath $m.Matches[0].Groups[1].Value)) { return $m.Matches[0].Groups[1].Value }
    }
    $vs = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vs) { return $vs.FullName }
    $c = Get-Command cmake -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    Fail 'cmake not found (expected the Visual Studio 2022 bundled one)'
}
$cmake = Find-CMake

$cmakeVars = @(
    "-DPS2X_RUNNER_GENERATED_DIR:PATH=$($g.GeneratedDir)",
    "-DPS2X_RUNNER_OVERLAY_DIRS:STRING=$($overlayDirs -join ';')",
    "-DPS2X_RUNNER_EXTRA_SOURCES:STRING=$($extra -join ';')",
    '-DPS2X_ENABLE_AGRESSIVE_LOGS:BOOL=OFF'
)
$watched = 'PS2X_RUNNER_GENERATED_DIR', 'PS2X_RUNNER_OVERLAY_DIRS', 'PS2X_RUNNER_EXTRA_SOURCES', 'PS2X_ENABLE_AGRESSIVE_LOGS'

function Read-CacheValues {
    $h = @{}
    if (Test-Path -LiteralPath $cache) {
        foreach ($l in [IO.File]::ReadLines($cache)) {
            if ($l -match '^(PS2X_[A-Z0-9_]+):[A-Z]+=(.*)$' -and $watched -contains $Matches[1]) { $h[$Matches[1]] = $Matches[2] }
        }
    }
    $h
}

$fresh = -not (Test-Path -LiteralPath $cache)
if ($fresh) {
    $cfgArgs = @('-S', $srcDir, '-B', $buildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64') + $cmakeVars
    Write-Host "  build dir missing: fresh configure (this also fetches the third-party deps; allow a while)"
} else {
    $cfgArgs = $cmakeVars + @($buildDir)      # "cmake [options] <path-to-existing-build>"
}
$buildArgs = @('--build', $buildDir, '--target', 'ps2EntryRunner', '--config', 'Release', '--parallel')

Write-Host "  cmake: $cmake"
Write-Host "  configure: cmake $($cfgArgs -join ' ')"
Write-Host "  build:     cmake $($buildArgs -join ' ')"
if ($DryRun) { Write-Host "(dry run: nothing executed)"; exit 0 }

New-Item -ItemType Directory -Force $workDir | Out-Null
$log = Join-Path $workDir ('build_{0:yyyyMMdd_HHmmss}.log' -f (Get-Date))
$sw  = [Diagnostics.Stopwatch]::StartNew()
$buildStart = Get-Date
$before = Read-CacheValues
$writer = [IO.StreamWriter]::new($log, $false, [Text.UTF8Encoding]::new($false))
$writer.AutoFlush = $true

# Run a native command, tee everything to the log, echo only the interesting lines + a progress heartbeat.
function Invoke-Logged([string]$Title, [string]$Exe, [string[]]$CmdArgs) {
    $writer.WriteLine("===== $Title : $Exe $($CmdArgs -join ' ')")
    $st = @{ compiled = 0; unity = 0 }
    & $Exe @CmdArgs 2>&1 | ForEach-Object {
        $line = "$_"
        $writer.WriteLine($line)
        if ($line -match '\.vcxproj ->|Time Elapsed|CMake Error|fatal error|: error |\berror (LNK|MSB|C)\d+|LNK2001|LNK2019|LNK4088') {
            Write-Host $line
        } elseif ($line -match '^\s+\S+\.(cpp|cxx|c)\s*$') {
            $st.compiled++
            if ($line -match 'unity_\d+_cxx\.cxx') { $st.unity++ }
            if ($st.compiled % 50 -eq 0) { Write-Host ("  ... {0} source files compiled so far ({1} unity chunks), {2:mm\:ss} elapsed" -f $st.compiled, $st.unity, $sw.Elapsed) }
        }
    }
    $code = $LASTEXITCODE
    $writer.WriteLine(("===== $Title exit code $code ({0} source files compiled, {1} unity chunks)" -f $st.compiled, $st.unity))
    Write-Host ("  {0}: exit code {1}; {2} source files compiled ({3} unity chunks)" -f $Title, $code, $st.compiled, $st.unity)
    $code
}

# ---- 1. configure (always) ----------------------------------------------------------------------
Write-Host "== configure"
$code = Invoke-Logged 'configure' $cmake $cfgArgs
if ($code -ne 0) { $writer.Close(); Fail "cmake configure exit code $code (log: $log)" }
$after = Read-CacheValues
if ($after['PS2X_ENABLE_AGRESSIVE_LOGS'] -ne 'OFF') { $writer.Close(); Fail "PS2X_ENABLE_AGRESSIVE_LOGS is '$($after['PS2X_ENABLE_AGRESSIVE_LOGS'])' in the cache, must be OFF" }
if (-not $fresh) {
    foreach ($k in $watched) {
        if ($before.ContainsKey($k) -and $before[$k] -ne $after[$k]) {
            Write-Host "  WARNING: cache value $k changed by this configure (expect a large rebuild):" -ForegroundColor Yellow
            Write-Host "    was: $($before[$k])"; Write-Host "    now: $($after[$k])"
        }
    }
}

# ---- 2. build ------------------------------------------------------------------------------------
Write-Host "== build ps2EntryRunner (Release)"
$code = Invoke-Logged 'build' $cmake $buildArgs
$writer.Close()
$elapsed = $sw.Elapsed
Write-Host ("  build finished in {0:hh\:mm\:ss} (log: {1})" -f $elapsed, $log)

# ---- 3. checks -----------------------------------------------------------------------------------
$problems = New-Object System.Collections.Generic.List[string]
if ($code -ne 0) { $problems.Add("cmake --build exit code $code") }

$logLines = [IO.File]::ReadAllLines($log)
$unres = @($logLines | Where-Object { $_ -match 'LNK2001|LNK2019' } | Select-Object -Unique)
if ($unres.Count -gt 0) {
    $problems.Add("$($unres.Count) unresolved-symbol message(s) (LNK2001/LNK2019)")
    $unres | Select-Object -First 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
}
$errs = @($logLines | Where-Object { $_ -match '\berror (LNK|MSB|C)\d+' -and $_ -notmatch 'LNK2001|LNK2019' } | Select-Object -Unique)
if ($errs.Count -gt 0) {
    $problems.Add("$($errs.Count) compiler/linker error line(s)")
    $errs | Select-Object -First 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
}
$lnk4088 = @($logLines | Where-Object { $_ -match 'LNK4088' }).Count
Write-Host "  LNK4088 x $lnk4088 (expected: CMakeLists links with /FORCE:MULTIPLE; info only)"

# exe + map must exist and be fresh
if (-not (Test-Path -LiteralPath $exe)) { $problems.Add("$exe does not exist") }
if (-not (Test-Path -LiteralPath $map)) { $problems.Add("$map does not exist") }

if ((Test-Path -LiteralPath $exe) -and (Test-Path -LiteralPath $map)) {
    $exeInfo = Get-Item -LiteralPath $exe
    $mb = $exeInfo.Length / 1MB
    Write-Host ("  exe: {0:N0} bytes ({1:N1} MiB), written {2}" -f $exeInfo.Length, $mb, $exeInfo.LastWriteTime)
    if ($exeInfo.LastWriteTime -lt $buildStart) {
        Write-Host "  note: exe was not relinked by this build (already up to date)"
    }
    # compare with the last good build of this game
    $sizeFile = Join-Path $workDir 'build_exe_size.txt'
    $lastGood = 0
    if (Test-Path -LiteralPath $sizeFile) { $lastGood = [int64](([IO.File]::ReadAllText($sizeFile)).Trim() -split '\s+')[0] }
    if ($lastGood -gt 0) {
        $pct = 100.0 * $exeInfo.Length / $lastGood
        Write-Host ("  exe size vs last good build ({0:N0} bytes): {1:N1}%" -f $lastGood, $pct)
        if ($pct -lt 90) { Write-Host "  WARNING: exe is more than 10% smaller than the last good build - overlays probably dropped" -ForegroundColor Yellow }
    } else {
        Write-Host "  (no previous exe size recorded for this game)"
    }

    # Map check. The map lists MSVC-mangled publics: ?<name>@@YAXPEAEPEAUR5900Context@@PEAVPS2Runtime@@@Z.
    # Every generated function file <name>.cpp defines <name> (overlay ones are <OVERLAY>_FUN_/entry_/caseD_/
    # ps2_default_<addr>_0x<addr>). Every one of them must be in the map: if unity chunks were skipped, whole
    # overlays vanish (nothing references them once register_functions.cpp is missing).
    $mapText = [IO.File]::ReadAllText($map)
    $syms = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($m in [regex]::Matches($mapText, '(?m)^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+\?([^@\s]+)@@YAX')) { [void]$syms.Add($m.Groups[1].Value) }
    Write-Host ("  map check ({0:N0} function symbols in ps2EntryRunner.map):" -f $syms.Count)

    $checks = @(, @('MAIN', $genDir))
    foreach ($n in $g.Overlays) { $checks += , @($n, $overlayWin[$n]) }
    foreach ($c in $checks) {
        $stems = @([IO.Directory]::EnumerateFiles($c[1], '*.cpp') | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) } | Where-Object { $_ -ne 'register_functions' })
        $missing = @($stems | Where-Object { -not $syms.Contains($_) })
        $found = $stems.Count - $missing.Count
        $line = "    {0,-10} {1,6} / {2,-6}" -f $c[0], $found, $stems.Count
        if ($stems.Count -eq 0) {
            Write-Host "$line  NO GENERATED FUNCTIONS" -ForegroundColor Red; $problems.Add("$($c[0]): no generated functions found in $($c[1])")
        } elseif ($missing.Count -gt 0) {
            Write-Host "$line  MISSING $($missing.Count), e.g. $(($missing | Select-Object -First 3) -join ', ')" -ForegroundColor Red
            $problems.Add("$($c[0]): $($missing.Count) of $($stems.Count) generated functions are not in the exe")
        } else {
            Write-Host "$line  ok"
        }
    }

    if ($problems.Count -eq 0) { [IO.File]::WriteAllText($sizeFile, "$($exeInfo.Length) bytes $(Get-Date -Format s)`r`n") }
}

if ($problems.Count -gt 0) {
    Write-Host ""
    Write-Host "BUILD CHECK FAILED ($($problems.Count) problem(s)):" -ForegroundColor Red
    $problems | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    Write-Host "  log: $log"
    exit 1
}
Write-Host ""
Write-Host ("BUILD OK: {0} in {1:hh\:mm\:ss}, exe {2:N1} MiB, all overlays present" -f $g.Name, $sw.Elapsed, $mb) -ForegroundColor Green
exit 0
