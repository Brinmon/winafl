@echo off
rem fuzz-harness 构建脚本模板（VS2022 x64 + PDB，-target_method 依赖符号）
rem 用法: build_harness.cmd <src.c> <out.exe>
if "%~2"=="" ( echo usage: %~nx0 ^<src.c^> ^<out.exe^> & exit /b 1 )
call "D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS FAILED & exit /b 1 )
cl /nologo /O2 /MD /Zi /utf-8 "%~1" /Fe:"%~2" /link /DEBUG:FULL
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo built %~2
