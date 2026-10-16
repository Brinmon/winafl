@echo off
rem Build a fork-server harness (x64 Release + PDB).
rem Usage: build.cmd [harness.c]
setlocal
call "D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars64.bat" >nul
if "%~1"=="" (set SRC=harness.c) else (set SRC=%~1)
cl /nologo /O2 /MD /Zi /utf-8 ^
   /I "%~dp0..\include" ^
   "%SRC%" ^
   "%~dp0..\src\shm.c" ^
   "%~dp0..\src\pipe.c" ^
   "%~dp0..\src\clone.c" ^
   "%~dp0..\src\forkloop.c" ^
   /Fe:harness.exe /Fo:obj\ /link /DEBUG:FULL
if errorlevel 1 exit /b 1
echo built harness.exe
endlocal
