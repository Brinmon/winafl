# smoke-test.ps1 - bounded TinyInst fuzz run for CI. Exit 0 on success.
# Runs afl-fuzz for N seconds, captures logs, validates fuzzer_stats.
param([int]$Seconds = 25, [string]$WorkDir = 'bin\Release')
$ErrorActionPreference = 'Continue'
Set-Location $WorkDir

# seeds
if (-not (Test-Path in)) { New-Item -ItemType Directory in | Out-Null }
Set-Content in\s1 'AAAA' -NoNewline
Set-Content in\s2 'BBBB' -NoNewline
Remove-Item out -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item fuzz.log,fuzz_err.log -Force -ErrorAction SilentlyContinue

$argList = @('-y','-i','in','-o','out','-t','30000','--',
  '-instrument_module','test.exe','-target_module','test.exe',
  '-target_method','test_target','-nargs','2','-iterations','5000',
  '-persist','-loop','--','test.exe','@@')

Write-Host ">>> starting afl-fuzz (TinyInst) for ${Seconds}s..."
$p = Start-Process -FilePath '.\afl-fuzz.exe' -ArgumentList $argList `
      -RedirectStandardOutput fuzz.log -RedirectStandardError fuzz_err.log `
      -PassThru -NoNewWindow
Start-Sleep -Seconds $Seconds
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Stop-Process -Name test -Force -ErrorAction SilentlyContinue

Write-Host "=== fuzzer_stats ==="
if (Test-Path 'out\fuzzer_stats') { Get-Content 'out\fuzzer_stats' | Select-String 'execs_done|execs_per_sec|paths_total|stability' }
Write-Host "=== fuzz.log tail ==="
if (Test-Path fuzz.log)     { Get-Content fuzz.log -Tail 8 }
Write-Host "=== fuzz_err.log tail ==="
if (Test-Path fuzz_err.log) { Get-Content fuzz_err.log -Tail 8 }

$ok = (Test-Path 'out\fuzzer_stats') -and
      (Select-String -Path 'out\fuzzer_stats' -Pattern 'execs_done' -Quiet)
if ($ok) { Write-Host 'SMOKE-OK'; exit 0 } else { Write-Host 'SMOKE-FAILED'; exit 1 }
