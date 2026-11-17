#Requires -Version 7
<#
.SYNOPSIS
  LA_FastImport harness tests without starting Gothic.
  Reads Gothic2.exe and your level meshes READ-ONLY, writes logs to -OutDir.
.PARAMETER Test
  patch   : fingerprint + every patch site original -> patched -> restored (BIN checks: jbe -> jmp, same target)
  import  : the untiled level 3DS (original vs. patched, bit compare)
  tiles   : the level tiles as import
  vobload : game-time vob visual load: world-order vob 3DS list (-VobList), real zCMesh objects,
            DeleteAll after each file, zCVertex pool free-list check (orig vs. patched)
  merge   : BSP merge of all tiles in ZEN order (orig limited by -OrigSeconds, then compare at the same tile count)
  all     : all of the above (tests that need a missing input are skipped)
.PARAMETER GameDir  Gothic II folder with System\Gothic2.exe (Steam 2.6 fix). Mandatory, only read.
.PARAMETER OutDir   logs (default: test\out next to this script)
.PARAMETER LevelDir folder with the level 3DS and its WK_TILES subfolder (import, tiles, merge)
.PARAMETER ZenFile  world ZEN listing the tiles in order (merge)
.PARAMETER VobList  text file with vob 3DS paths in load order (vobload)
.EXAMPLE
  pwsh -NoProfile -File test\run_tests.ps1 -GameDir '<Gothic II folder>' -Test patch
#>
param([Parameter(Mandatory = $true)][string]$GameDir,
    [ValidateSet('patch', 'import', 'tiles', 'vobload', 'merge', 'all')][string]$Test = 'all', [int]$OrigSeconds = 1500,
    [string]$OutDir = '', [string]$LevelDir = '', [string]$ZenFile = '', [string]$VobList = '')
$ErrorActionPreference = 'Stop'
$out = if ($OutDir) { $OutDir } else { Join-Path $PSScriptRoot 'out' }
New-Item -ItemType Directory -Force $out | Out-Null
$harness = Join-Path $PSScriptRoot '..\build\lafi_harness.exe'
if (-not (Test-Path $harness)) { & cmd /c (Join-Path $PSScriptRoot '..\build.bat'); if ($LASTEXITCODE) { exit 1 } }
$exe = Join-Path $GameDir 'System\Gothic2.exe'
$level = $LevelDir
$zen = $ZenFile
function Need([string]$what, [string]$path) { if (-not $path -or -not (Test-Path $path)) { Write-Host "   skipped: $what not given or missing"; return $false }; return $true }
$fail = 0
function Run([string]$name, [string[]]$a) {
    $log = Join-Path $out "test_$name.log"
    Write-Host "== $name -> $log"
    & $harness @a *> $log
    $code = $LASTEXITCODE
    Get-Content $log | Select-String '^RESULT|FATAL|DIFF|fingerprint' | ForEach-Object { Write-Host "   $_" }
    if ($code -ne 0) { Write-Host "   exit $code"; $script:fail++ }
}
if ($Test -in 'patch', 'all') {
    Run 'fingerprint' @($exe, 'fingerprint')
    Run 'patchcheck' @($exe, 'patchcheck')
}
if ($Test -in 'import', 'all' -and (Need '-LevelDir' $level)) {
    $list = Join-Path $out 'list_level.txt'
    Get-ChildItem $level -Filter *.3DS | Sort-Object Length -Descending | ForEach-Object FullName | Set-Content $list
    Run 'import_level' @($exe, 'import', $list, 'both')
}
if ($Test -in 'tiles', 'all' -and (Need '-LevelDir' $level)) {
    $list = Join-Path $out 'list_tiles.txt'
    Get-ChildItem (Join-Path $level 'WK_TILES') -Filter *.3DS | ForEach-Object FullName | Set-Content $list
    Run 'import_tiles' @($exe, 'import', $list, 'both')
}
if ($Test -in 'vobload', 'all') {
    if (Need '-VobList' $VobList) { Run 'vobload' @($exe, 'vobload', $VobList, 'both') }
}
if ($Test -in 'merge', 'all' -and (Need '-ZenFile' $zen) -and (Need '-LevelDir' $level)) {
    Run 'merge_both' @($exe, 'merge', $zen, (Join-Path $level 'WK_TILES'), 'both', '-tlimit', "$OrigSeconds")
}
exit $fail
