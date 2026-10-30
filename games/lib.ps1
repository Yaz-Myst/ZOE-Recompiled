# Shared helpers for the PowerShell scripts (dot-source this: . "$PSScriptRoot\..\games\lib.ps1").
# Reads <name>.cfg (plain KEY=VALUE lines, '#' comments; see ZOE-Recompiled\zoe.cfg for the keys).

$script:GamesDir = $PSScriptRoot

# A game's config is games\<name>.cfg, else <repo root>\<any dir>\<name>.cfg (one level down;
# e.g. ZOE-Recompiled\zoe.cfg). games\ is listed first. Relative paths in a cfg are taken from the cfg's own folder.
function Get-GameCfgFiles {
    $root = Split-Path -Parent $script:GamesDir
    @(Get-ChildItem -LiteralPath $script:GamesDir -Filter *.cfg -File -ErrorAction SilentlyContinue)
    Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -ne $script:GamesDir } |
        ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Filter *.cfg -File -ErrorAction SilentlyContinue }
}

function Get-GameNames { Get-GameCfgFiles | ForEach-Object { $_.BaseName } | Sort-Object -Unique }

# A:/x/y -> A:\x\y (for exe paths, working dirs, robocopy and the like)
function ConvertTo-WinPath([string]$Path) { $Path.Replace('/', '\') }

function Get-GameConfig {
    param([Parameter(Mandatory)][string]$Name)
    $found = @(Get-GameCfgFiles | Where-Object { $_.BaseName -ieq $Name })
    if ($found.Count -eq 0) {
        throw "unknown game '$Name': no games\$Name.cfg and no $(Split-Path -Parent $script:GamesDir)\*\$Name.cfg (known: $((Get-GameNames) -join ', '))"
    }
    $file = $found[0].FullName
    $kv = @{}
    foreach ($line in [IO.File]::ReadAllLines($file)) {
        if ($line -match '^\s*(#|$)') { continue }
        $i = $line.IndexOf('=')
        if ($i -lt 1) { throw "${file}: bad line (want KEY=VALUE): $line" }
        $kv[$line.Substring(0, $i).Trim()] = $line.Substring($i + 1).TrimEnd("`r")
    }
    foreach ($k in 'NAME', 'ELF', 'DISC_DIR', 'CD_IMAGE', 'WORK_DIR', 'GHIDRA_PROJECT_DIR', 'GHIDRA_PROJECT',
                   'GENERATED_DIR', 'OVERLAYS', 'EXTRA_SOURCES', 'BUILD_DIR') {
        if (-not $kv.ContainsKey($k)) { throw "${file}: missing key $k" }
    }
    $split = { param($s) @($s -split ';' | Where-Object { $_ -ne '' }) }
    # A path value may be absolute (A:/x/y) or relative to the cfg's folder (work/output). Forward slashes are kept.
    $cfgDir = Split-Path -Parent $file
    $path = {
        param($v)
        if ([string]::IsNullOrEmpty($v)) { return '' }
        if ([IO.Path]::IsPathRooted($v.Replace('/', '\'))) { return $v }
        return (Join-Path $cfgDir $v).Replace('\', '/')
    }
    $paths = { param($s) @(& $split $s | ForEach-Object { & $path $_ }) }
    [pscustomobject]@{
        Name             = $kv['NAME']
        Elf              = & $path $kv['ELF']
        DiscDir          = & $path $kv['DISC_DIR']
        CdImage          = & $path $kv['CD_IMAGE']
        WorkDir          = & $path $kv['WORK_DIR']
        GhidraProjectDir = & $path $kv['GHIDRA_PROJECT_DIR']
        GhidraProject    = $kv['GHIDRA_PROJECT']
        GeneratedDir     = & $path $kv['GENERATED_DIR']
        Overlays         = @(& $split $kv['OVERLAYS'])
        ExtraSources     = @(& $paths $kv['EXTRA_SOURCES'])
        BuildDir         = & $path $kv['BUILD_DIR']
        # Optional: reviewed list of runtime stand-ins and where the game's linked libraries start (tools\check_stubs.py)
        StubReview       = $(if ($kv.ContainsKey('STUB_REVIEW')) { & $path $kv['STUB_REVIEW'] } else { '' })
        LibStart         = $(if ($kv.ContainsKey('LIB_START')) { $kv['LIB_START'] } else { '' })
        # Optional: this game's own copy of the runtime/translator source tree (default <repo root>\PS2Recomp)
        SourceDir        = $(if ($kv.ContainsKey('SOURCE_DIR')) { & $path $kv['SOURCE_DIR'] } else { '' })
    }
}
