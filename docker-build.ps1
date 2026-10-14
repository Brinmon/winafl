# docker-build.ps1 - build the WinAFL portable Docker image
# Chains: (build_all.bat if needed) -> package.ps1 -> docker build
# Usage:
#   .\docker-build.ps1                 # package (if needed) + docker build
#   .\docker-build.ps1 -SkipPackage    # docker build from existing dist
#   .\docker-build.ps1 -Tag mytag:1.0
param(
  [string]$Tag = 'winfuzz:latest',
  [switch]$SkipPackage,
  [switch]$SkipBuild            # skip compile; only package + docker
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$pkg  = Join-Path $root 'dist\winafl-portable'

if (-not $SkipPackage) {
  if (-not $SkipBuild) {
    Write-Host '=== [0/2] compile (build_all.bat) ==='
    & cmd /c "`"$root\build_all.bat`""
    if ($LASTEXITCODE -ne 0) { throw "build_all.bat failed: $LASTEXITCODE" }
  }
  Write-Host '=== [1/2] package (package.ps1) ==='
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'package.ps1')
  if ($LASTEXITCODE -ne 0) { throw "package.ps1 failed" }
}

if (-not (Test-Path (Join-Path $pkg 'afl-fuzz.exe'))) {
  throw "portable package not found at $pkg - run package.ps1 first"
}

Write-Host "=== [2/2] docker build -t $Tag  (context: dist\winafl-portable) ==="
docker build -t $Tag -f (Join-Path $root 'Dockerfile') $pkg
if ($LASTEXITCODE -ne 0) { throw "docker build failed: $LASTEXITCODE" }
Write-Host "`nImage ready: $Tag"
Write-Host "Run demo:    docker run --rm $Tag cmd /c C:\winafl\demo.cmd"
Write-Host "Shell:       docker run --rm -it $Tag cmd"
