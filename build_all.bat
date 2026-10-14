@echo off
rem ============================================================
rem WinAFL (Brinmon fork) one-shot build script, x64 Release
rem Prereqs: VS2022 C++ workload, Python3, DynamoRIO at D:\HackerCode\devtools\DynamoRIO
rem Fork patches (auto-applied):
rem   1. xed --no-mscrt      : mbuild builds XED /MT -> CRT mismatch (tinyinst is /MD)
rem   2. winafl /FORCE:MULTIPLE : ntdll_imports vs libucrt atoi clash; upstream CI red since 2025-12
rem   3. USE_DRSYMS + PDB    : -target_method needs symbols for non-exported functions
rem ============================================================
call "D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars64.bat" >nul
rem /utf-8: fix C4819 warnings-as-errors on Chinese-locale Windows
rem /MD: unify CRT for mbuild-invoked cl (cmake targets carry own runtime flag)
set _CL_=/utf-8 /MD
cd /d E:\GitCode\winafl

rem --- patch 1 (idempotent): xed no implicit /MT ---
findstr /c:"no-mscrt" third_party\TinyInst\third_party\CMakeLists.txt >nul
if errorlevel 1 (
  powershell -NoProfile -Command "(Get-Content 'third_party\TinyInst\third_party\CMakeLists.txt') -replace 'mfile.py \$\{XED_HOST_CPU\}', 'mfile.py ${XED_HOST_CPU} --no-mscrt' | Set-Content 'third_party\TinyInst\third_party\CMakeLists.txt' -Encoding UTF8"
  echo [patch] xed --no-mscrt applied
)

rmdir /s /q build64 2>nul
mkdir build64
cd build64
set CMAKE=D:\HackerCode\Microsoft Visual Studio\2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
"%CMAKE%" -G "Visual Studio 17 2022" -A x64 .. ^
  -DTINYINST=1 ^
  -DUSE_DRSYMS=1 ^
  -DDynamoRIO_DIR=D:/HackerCode/devtools/DynamoRIO/cmake ^
  -DCMAKE_C_FLAGS_RELEASE="/O2 /MD /Zi /utf-8" ^
  -DCMAKE_CXX_FLAGS_RELEASE="/O2 /MD /Zi /utf-8" ^
  -DCMAKE_EXE_LINKER_FLAGS_RELEASE="/DEBUG /INCREMENTAL:NO" ^
  -DCMAKE_SHARED_LINKER_FLAGS_RELEASE="/DEBUG /INCREMENTAL:NO" ^
  > configure.log 2>&1
if errorlevel 1 ( type configure.log & exit /b 1 )
"%CMAKE%" --build . --config Release -- -m
exit /b %ERRORLEVEL%
