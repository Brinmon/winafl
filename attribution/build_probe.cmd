@echo off
setlocal
call "D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo VCVARS FAILED & exit /b 1 )
cl /nologo /O2 /W3 /Fe:probe_r4.exe probe_r4.c /link psapi.lib
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo BUILD OK
