# smoke-test.ps1 - bounded fuzz run for CI. Exit 0 on success.
# Tests BOTH instrumentation modes by default (TinyInst + DynamoRIO): a DynamoRIO
# packaging regression once shipped unnoticed because only TinyInst was exercised.
param(
  [int]$Seconds = 25,
  [string]$WorkDir = 'bin\Release',
  [string]$DrDir = '',   # DynamoRIO root (the one containing bin64\drrun.exe)
  [ValidateSet('tinyinst','dynamorio','both')]
  [string]$Mode = 'both'
)
$ErrorActionPreference = 'Continue'
Set-Location $WorkDir

# seeds
if (-not (Test-Path in)) { New-Item -ItemType Directory in | Out-Null }
Set-Content in\s1 'AAAA' -NoNewline
Set-Content in\s2 'BBBB' -NoNewline

function Invoke-Mode {
  param([string]$Name, [string[]]$ArgList)
  $out = "out_$Name"
  $log = "fuzz_$Name.log"
  $err = "fuzz_${Name}_err.log"
  Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item $log, $err -Force -ErrorAction SilentlyContinue

  Write-Host ">>> [$Name] afl-fuzz for ${Seconds}s"
  Write-Host "    $($ArgList -join ' ')"
  $p = Start-Process -FilePath '.\afl-fuzz.exe' -ArgumentList $ArgList `
        -RedirectStandardOutput $log -RedirectStandardError $err -PassThru -NoNewWindow
  Start-Sleep -Seconds $Seconds
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  Stop-Process -Name test -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 1

  Write-Host "=== [$Name] fuzzer_stats ==="
  if (Test-Path "$out\fuzzer_stats") {
    Get-Content "$out\fuzzer_stats" | Select-String 'execs_done|execs_per_sec|paths_total|stability'
    if (Test-Path $log) { Get-Content $log -Tail 5 }
    if (Select-String -Path "$out\fuzzer_stats" -Pattern 'execs_done' -Quiet) {
      Write-Host "MODE-OK: $Name"
      return $true
    }
  } else {
    Write-Host "no $out\fuzzer_stats produced"
  }
  Write-Host "=== [$Name] stdout tail ==="
  if (Test-Path $log) { Get-Content $log -Tail 12 }
  Write-Host "=== [$Name] stderr tail ==="
  if (Test-Path $err) { Get-Content $err -Tail 12 }
  Write-Host "MODE-FAILED: $Name"
  return $false
}

$results = [ordered]@{}

if ($Mode -eq 'tinyinst' -or $Mode -eq 'both') {
  $results['tinyinst'] = Invoke-Mode -Name 'tinyinst' -ArgList @(
    '-y','-i','in','-o','out_tinyinst','-t','30000','--',
    '-instrument_module','test.exe','-target_module','test.exe',
    '-target_method','test_target','-nargs','2','-iterations','5000',
    '-persist','-loop','--','test.exe','@@')
}

if ($Mode -eq 'dynamorio' -or $Mode -eq 'both') {
  if (-not $DrDir) {
    if ($env:DYNAMORIO_DIR) { $DrDir = $env:DYNAMORIO_DIR }
    elseif (Test-Path '..\..\..\DynamoRIO') { $DrDir = (Resolve-Path '..\..\..\DynamoRIO').Path }
  }
  # -D must be the directory holding drrun.exe; DR derives its root from there
  # and requires a complete root (lib64+lib32, release+debug).
  if ($DrDir -and (Test-Path (Join-Path $DrDir 'bin64\drrun.exe'))) {
    $drBin = (Resolve-Path (Join-Path $DrDir 'bin64')).Path
    $client = (Resolve-Path 'winafl.dll').Path
    $results['dynamorio'] = Invoke-Mode -Name 'dynamorio' -ArgList @(
      '-i','in','-o','out_dynamorio','-t','30000',
      '-D',$drBin,'-w',$client,'--',
      '-coverage_module','test.exe','-target_module','test.exe',
      '-target_method','test_target','-nargs','2','--','test.exe','@@')
  } else {
    Write-Host "SKIP dynamorio: DrDir not found (pass -DrDir <root containing bin64> or set DYNAMORIO_DIR)"
    $results['dynamorio'] = $null
  }
}

$failed = @()
foreach ($k in $results.Keys) { if ($results[$k] -eq $false) { $failed += $k } }
if ($failed.Count -gt 0) {
  Write-Host "SMOKE-FAILED: $($failed -join ', ')"
  exit 1
}
Write-Host 'SMOKE-OK'
exit 0
