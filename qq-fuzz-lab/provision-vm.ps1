# provision-vm.ps1 - run INSIDE the fuzz VM after Windows install completes.
# Sets up: WinAFL portable bundle + QQ (silent, no login) + RE tools dir.
# No QQ login required — we only need the DLL files, not a running/logged-in app.
#
# Usage (inside VM, as admin):
#   .\provision-vm.ps1 -WinAflZip \\hostshare\winafl-portable-win64.zip
# or copy the zip in first and: .\provision-vm.ps1 -WinAflZip C:\share\winafl.zip
param(
  [string]$WorkDir = 'C:\fuzzlab',
  [string]$WinAflZip = ''          # path to winafl-portable-win64.zip inside VM
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

# --- 1. extract WinAFL portable bundle ---
if ($WinAflZip -and (Test-Path $WinAflZip)) {
  Write-Host "=== extracting WinAFL to $WorkDir\winafl ==="
  Expand-Archive $WinAflZip -DestinationPath "$WorkDir\winafl" -Force
} else {
  Write-Warning "WinAflZip not provided/found; copy winafl-portable-win64.zip in and re-run, or place it under $WorkDir"
}

# --- 2. download QQ installer (silent install, NO login needed) ---
Write-Host '=== downloading QQ installer ==='
$qqUrl = 'https://dldir1.qq.com/qqfile/qq/QQNT/Windows/QQ_9.9.19_250821_x64_01.exe'   # QQNT; update URL if stale
$qqInst = "$WorkDir\qq_installer.exe"
try {
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  Invoke-WebRequest -Uri $qqUrl -OutFile $qqInst -UseBasicParsing
  "downloaded: {0:N1} MB" -f ((Get-Item $qqInst).Length/1MB)
} catch {
  Write-Warning "QQ download failed ($($_.Exception.Message)). Download manually into $WorkDir and re-run the install step."
}

# --- 3. silent install QQ (QQNT supports --silent) ---
if (Test-Path $qqInst) {
  Write-Host '=== silent-installing QQ (no login) ==='
  $p = Start-Process $qqInst -ArgumentList '--silent' -Wait -PassThru
  "installer exit: $($p.ExitCode)"
}

# --- 4. locate QQ install dir + DLL inventory ---
Write-Host '=== locating QQ install + DLLs ==='
$candidates = @(
  "$env:ProgramFiles\Tencent\QQNT",
  "${env:ProgramFiles(x86)}\Tencent\QQNT",
  "$env:LOCALAPPDATA\Programs\Tencent\QQNT"
)
foreach ($c in $candidates) {
  if (Test-Path $c) { "FOUND QQ at: $c"; Get-ChildItem $c -Recurse -Filter *.dll -ErrorAction SilentlyContinue | Select-Object -First 40 FullName | Out-String }
}

# --- 5. RE notes ---
Write-Host @"

Next steps (manual RE, inside VM or via Ghidra/IDA):
  1. Pick a parsing DLL under the QQ dir (image/document/mini-program codec).
  2. Open it in Ghidra/IDA, find a function that reads+parses input.
  3. offset = function_address - module_image_base   (relative, NOT absolute)
  4. Fill offset into the harness template (see harness_crypt32.c on host),
     rebuild harness inside VM (needs VS Build Tools) or bring a prebuilt one.
  5. Fuzz (TinyInst mode), no QQ login needed:
       afl-fuzz.exe -y -i in -o out -- -instrument_module <target>.dll ^
         -target_module harness.exe -target_method fuzz_one -nargs 1 ^
         -iterations 10000 -persist -loop -- harness.exe @@

WinAFL bundle:  $WorkDir\winafl
"@
