@echo off
setlocal
call "D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /MD /Zi /utf-8 dr_clone_probe2.c /Fe:dr_clone_probe2.exe /link /DEBUG:FULL psapi.lib
if errorlevel 1 exit /b 1
echo built dr_clone_probe2.exe
endlocal
