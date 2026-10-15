# package.ps1 - assemble a portable WinAFL package (all instrumentation modes)
# Works locally and on CI. Override paths via params.
#
# DynamoRIO bundling notes (regression fixed 2026-10):
#  - afl-fuzz/-showmap launch the client as "<DrDir>\drrun.exe ...", so the path given
#    to -D must be the directory CONTAINING drrun.exe (bin64 / bin32), not the DR root.
#  - DynamoRIO itself derives its root from drrun.exe's location and then requires a
#    COMPLETE root: lib64+lib32 with release/ AND debug/ dynamorio.dll plus drpreinject.dll.
#    Shipping only lib64\release made -D mode abort with
#    "cannot find ...\lib64\drpreinject.dll".
#  - We therefore copy exe/dll only (no .pdb/.lib) for both bit widths, and step [6]
#    fails the build if anything required is missing.
param(
  [string]$BuildDir = 'E:\GitCode\winafl\build64\bin\Release',
  [string]$DrDir   = 'D:\HackerCode\devtools\DynamoRIO',
  [string]$OutDir  = 'E:\GitCode\winafl\dist\winafl-portable',
  [string]$VcRedist = '',   # auto-detect when empty
  [switch]$Zip              # also emit winafl-portable-win64.zip next to dist/
)
$ErrorActionPreference = 'Stop'

# --- auto-detect VC CRT redist dir when not supplied ---
if (-not $VcRedist) {
  $vsroot = 'D:\HackerCode\Microsoft Visual Studio\2022'
  if (-not (Test-Path $vsroot)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { $vsroot = & $vswhere -latest -products * -property installationPath }
  }
  $cand = Get-ChildItem "$vsroot\VC\Redist\MSVC\*\onecore\x64\Microsoft.VC143.CRT" -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
  if ($cand) { $VcRedist = $cand.FullName } else { Write-Warning 'VC redist not found; target may need VC++ Redistributable' }
}

if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

Write-Host '=== [1] core fuzzing tools (TinyInst + Intel PT built-in) ==='
$core = @('afl-fuzz.exe','afl-showmap.exe','afl-tmin.exe','afl-analyze.exe',
          'winafl.dll','winaflpt-debug.exe','libipt.dll',
          'custom_net_fuzzer.dll','custom_winafl_server.dll',
          'test.exe','test_gdiplus.exe','test_netmode.exe','test_servermode.exe')
foreach ($f in $core) {
  if (Test-Path "$BuildDir\$f") { Copy-Item "$BuildDir\$f" $OutDir } else { Write-Warning "missing $f" }
}
Get-ChildItem "$BuildDir\*.pdb" -ErrorAction SilentlyContinue | Copy-Item -Destination $OutDir

Write-Host '=== [2] VC runtime (app-local; no VC++ Redist needed on target) ==='
if ($VcRedist) {
  foreach ($f in 'msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll','msvcp140_1.dll','msvcp140_2.dll') {
    if (Test-Path "$VcRedist\$f") { Copy-Item "$VcRedist\$f" $OutDir }
  }
}

Write-Host '=== [3] DynamoRIO runtime (needed by -D mode, 64-bit and 32-bit) ==='
function Copy-DynamoRIO {
  param([string]$Source, [string]$Dest)
  if (-not (Test-Path $Source)) { Write-Warning "DynamoRIO: $Source not found (skipped)"; return }
  $files = Get-ChildItem $Source -Recurse -File -ErrorAction SilentlyContinue |
           Where-Object { $_.Extension -in '.exe', '.dll' }
  foreach ($f in $files) {
    $rel = $f.FullName.Substring($Source.Length).TrimStart('\')
    $target = Join-Path $Dest $rel
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    Copy-Item $f.FullName $target -Force
  }
  Write-Host ("    {0,-16} {1,3} exe/dll" -f (Split-Path $Source -Leaf), $files.Count)
}
$drdirOut = Join-Path $OutDir 'dynamorio'
foreach ($sub in 'bin64','bin32','lib64','lib32','ext\lib64','ext\lib32') {
  Copy-DynamoRIO -Source (Join-Path $DrDir $sub) -Dest (Join-Path $drdirOut $sub)
}

Write-Host '=== [4] demo assets (bundled test target seeds + launcher) ==='
$indemo = Join-Path $OutDir 'in'
New-Item -ItemType Directory -Path $indemo -Force | Out-Null
Set-Content -Path (Join-Path $indemo 'seed1') -Value 'AAAA' -NoNewline
Set-Content -Path (Join-Path $indemo 'seed2') -Value 'BBBB' -NoNewline
$demo = @'
@echo off
rem Run the bundled WinAFL test target in TinyInst mode (recommended).
cd /d %~dp0
if not exist in mkdir in
if not exist in\seed1 (echo AAAA> in\seed1)
if not exist in\seed2 (echo BBBB> in\seed2)
echo [demo] afl-fuzz TinyInst mode against bundled test.exe ...
afl-fuzz.exe -y -i in -o out -t 30000 -- -instrument_module test.exe -target_module test.exe -target_method test_target -nargs 2 -iterations 5000 -persist -loop -- test.exe @@
'@
Set-Content -Path (Join-Path $OutDir 'demo.cmd') -Value $demo -Encoding ASCII

$demoDr = @'
@echo off
rem Run the bundled WinAFL test target in DynamoRIO mode.
rem Note: -D must point at the directory containing drrun.exe; winafl.dll is loaded
rem by name, so this script cd's into the bundle first.
cd /d %~dp0
if not exist in mkdir in
if not exist in\seed1 (echo AAAA> in\seed1)
if not exist in\seed2 (echo BBBB> in\seed2)
echo [demo] afl-fuzz DynamoRIO mode against bundled test.exe ...
afl-fuzz.exe -i in -o out_dr -t 30000 -D dynamorio\bin64 -w %~dp0winafl.dll -- -coverage_module test.exe -target_module test.exe -target_method test_target -nargs 2 -- test.exe @@
'@
Set-Content -Path (Join-Path $OutDir 'demo-dynamorio.cmd') -Value $demoDr -Encoding ASCII

Write-Host '=== [5] README ==='
Copy-Item (Join-Path $PSScriptRoot 'README-FORK.md') (Join-Path $OutDir 'README.md') -ErrorAction SilentlyContinue

Write-Host '=== [6] verify bundle completeness ==='
$required = @(
  'afl-fuzz.exe','afl-showmap.exe','afl-tmin.exe','afl-analyze.exe','winafl.dll',
  # DynamoRIO 64-bit: drrun (launched via -D), root validity files, client deps
  'dynamorio\bin64\drrun.exe',
  'dynamorio\bin64\drconfig.exe',
  'dynamorio\lib64\drpreinject.dll',
  'dynamorio\lib64\drconfiglib.dll',
  'dynamorio\lib64\release\dynamorio.dll',
  'dynamorio\lib64\debug\dynamorio.dll',
  'dynamorio\ext\lib64\release\drmgr.dll',
  'dynamorio\ext\lib64\release\drx.dll',
  # DynamoRIO 32-bit: required for DR to accept the root, and for 32-bit targets
  'dynamorio\bin32\drrun.exe',
  'dynamorio\lib32\drpreinject.dll',
  'dynamorio\lib32\release\dynamorio.dll',
  'dynamorio\lib32\debug\dynamorio.dll'
)
$missing = @()
foreach ($r in $required) {
  if (-not (Test-Path (Join-Path $OutDir $r))) { $missing += $r }
}
# every lib64/lib32 top-level DLL from the source must be present in the bundle
foreach ($bit in '64','32') {
  $srcLib = Join-Path $DrDir "lib$bit"
  if (Test-Path $srcLib) {
    Get-ChildItem "$srcLib\*.dll" -File | ForEach-Object {
      if (-not (Test-Path (Join-Path $drdirOut "lib$bit\$($_.Name)"))) { $missing += "dynamorio\lib$bit\$($_.Name)" }
    }
  }
}
if ($missing.Count -gt 0) {
  Write-Host 'MISSING REQUIRED FILES:'
  $missing | Sort-Object -Unique | ForEach-Object { Write-Host "  - $_" }
  throw "package incomplete: $($missing.Count) required file(s) missing"
}
Write-Host ("verify OK: {0} required paths present; DR root complete (64+32)" -f $required.Count)

$size = (Get-ChildItem $OutDir -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("`nPackage assembled: {0}  ({1:N1} MB)" -f $OutDir, ($size/1MB))

if ($Zip) {
  $zipPath = Join-Path (Split-Path $OutDir) 'winafl-portable-win64.zip'
  if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
  Compress-Archive -Path (Join-Path $OutDir '*') -DestinationPath $zipPath -Force
  $zmb = (Get-Item $zipPath).Length / 1MB
  Write-Host ("Zip: {0}  ({1:N1} MB)" -f $zipPath, $zmb)
}
