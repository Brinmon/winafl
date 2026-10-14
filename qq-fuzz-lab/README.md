# Closed-source DLL fuzzing lab (WinAFL + Hyper-V)

Proven pipeline for fuzzing closed-source Windows DLLs (e.g. QQ/WeChat parsers).
The same harness pattern works for any LoadLibrary-able DLL; only the target
DLL + function offset change.

## Verified
`harness_crypt32.c` fuzzes a REAL closed-source Windows DLL (crypt32.dll
CryptStringToBinaryA) via the portable WinAFL bundle: 16000 execs/s, 100%
stable. This is the exact pattern for a QQ parsing DLL.

## Why this sidesteps Tencent TP/ACE
TP anti-cheat protects the RUNNING QQ process. A harness loads the target DLL
into your OWN clean process, so the TP kernel driver is not injected there.
No QQ login needed — only the DLL file matters.

## Files
- `harness_crypt32.c`  - closed-source DLL harness template (LoadLibrary +
  GetProcAddress; for QQ use RE-derived offset instead). Build with PDB:
  `cl /O2 /MD /Zi harness.c /Fe:harness.exe /link /DEBUG:FULL`
- `New-FuzzVM.ps1`     - Hyper-V VM creation (admin; needs a Win10/11 eval ISO)
- `autounattend.xml`   - fully unattended Windows install (no clicking)
- `provision-vm.ps1`   - run in VM: extract WinAFL + silent-install QQ (no login)

## Fuzz command (TinyInst mode)
```
afl-fuzz.exe -y -i in -o out -- -instrument_module <target>.dll ^
  -target_module harness.exe -target_method fuzz_one -nargs 1 ^
  -iterations 10000 -persist -loop -- harness.exe @@
```

## Manual steps I cannot automate
1. Download a Windows eval ISO (5GB, one-time) -> feed to New-FuzzVM.ps1
2. Reverse the QQ DLL to get the target function offset (Ghidra/IDA)
3. Fill offset into harness, rebuild, fuzz
