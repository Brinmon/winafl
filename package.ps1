# package.ps1 - assemble a portable WinAFL package (all instrumentation modes)
# Works locally and on CI. Override paths via params.
param(
  [string]$BuildDir = 'E:\GitCode\winafl\build64\bin\Release',
  [string]$DrDir   = 'D:\HackerCode\devtools\DynamoRIO',
  [string]$OutDir  = 'E:\GitCode\winafl\dist\winafl-portable',
  [string]$VcRedist = ''   # auto-detect when empty
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

if (Test-Path (Split-Path $OutDir)) { Remove-Item (Split-Path $OutDir) -Recurse -Force -ErrorAction SilentlyContinue }
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

Write-Host '=== [3] DynamoRIO runtime (for -D mode) ==='
$drdirOut = Join-Path $OutDir 'dynamorio'
foreach ($d in "$drdirOut\bin64","$drdirOut\lib64\release","$drdirOut\ext\lib64\release") {
  New-Item -ItemType Directory -Path $d -Force | Out-Null
}
Copy-Item "$DrDir\bin64\*"             "$drdirOut\bin64" -Recurse -Force
Copy-Item "$DrDir\lib64\release\*"     "$drdirOut\lib64\release" -Recurse -Force
Copy-Item "$DrDir\ext\lib64\release\*" "$drdirOut\ext\lib64\release" -Recurse -Force

Write-Host '=== [4] README ==='
Copy-Item 'E:\GitCode\winafl\README-FORK.md' (Join-Path $OutDir 'README.md') -ErrorAction SilentlyContinue

$size = (Get-ChildItem $OutDir -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("`nPackage assembled: {0}  ({1:N1} MB)" -f $OutDir, ($size/1MB))
