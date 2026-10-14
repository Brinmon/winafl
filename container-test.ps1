# container-test.ps1 - bounded WinAFL TinyInst fuzz inside the container
$ErrorActionPreference = 'Continue'
Set-Location C:\winafl
if (-not (Test-Path in)) { New-Item -ItemType Directory in | Out-Null }
Set-Content in\seed1 'AAAA' -NoNewline
Set-Content in\seed2 'BBBB' -NoNewline
Remove-Item out -Recurse -Force -ErrorAction SilentlyContinue

$argList = @('-y','-i','in','-o','out','-t','30000','--',
  '-instrument_module','test.exe','-target_module','test.exe',
  '-target_method','test_target','-nargs','2','-iterations','5000',
  '-persist','-loop','--','test.exe','@@')

Write-Host ">>> starting afl-fuzz (TinyInst) inside container..."
$p = Start-Process -FilePath '.\afl-fuzz.exe' -ArgumentList $argList `
      -RedirectStandardOutput fuzz.log -RedirectStandardError err.log -PassThru -NoNewWindow
Start-Sleep -Seconds 20
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Stop-Process -Name test -Force -ErrorAction SilentlyContinue

Write-Host "=== fuzzer_stats ==="
if (Test-Path 'out\fuzzer_stats') {
  Get-Content 'out\fuzzer_stats' | Select-String 'execs_done|execs_per_sec|paths_total|stability'
} else {
  Write-Host 'NO STATS - err.log tail:'
  Get-Content err.log -Tail 15 -ErrorAction SilentlyContinue
  Get-Content fuzz.log -Tail 15 -ErrorAction SilentlyContinue
}
