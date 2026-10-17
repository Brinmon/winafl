@echo off
rem fuzz-harness build script (VS2022, PDB required for -target_method)
rem usage: build_harness.cmd <src.c|src.cpp> <out.exe> [x64|x86]
rem   x64 (default) - matches build64 afl-fuzz (TinyInst mode -y)
rem   x86           - vcvars32; TinyInst x64 natively instruments WOW64 targets
rem                    (TinyInst README Note#1), but the harness itself must be
rem                    32-bit because the harness process defines the bitness.
rem NOTE: keep comments ASCII-only. GBK cmd misreads UTF-8 CJK in .cmd files.
if "%~2"=="" ( echo usage: %~nx0 ^<src^> ^<out.exe^> [x64^|x86] & exit /b 1 )
setlocal
set ARCH=x64
if not "%~3"=="" set ARCH=%~3
cd /d "%~dp0"
if not exist artifacts mkdir artifacts
if /I "%ARCH%"=="x86" (
  call "D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
) else (
  call "D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)
if errorlevel 1 ( echo VCVARS FAILED & exit /b 1 )
set _CL_=/utf-8 /MD
set SRC=%~1
set EXTRA=
if /I "%~x1"==".cpp" set EXTRA=/EHsc
cl /nologo /O2 /MD /Zi /utf-8 %EXTRA% "%SRC%" /Fe:"%~2" /Fo:src\obj_tmp.obj /link /DEBUG:FULL oleaut32.lib ole32.lib
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo built %~2 (%ARCH%)
endlocal
