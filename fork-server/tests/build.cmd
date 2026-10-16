@echo off
setlocal
set HERE=%~dp0
call "D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%HERE%obj" mkdir "%HERE%obj"
if not exist "%HERE%build" mkdir "%HERE%build"
cd /d "%HERE%obj"

echo [1/3] harness (libvips target)
cl /nologo /O2 /MD /Zi /utf-8 /I "%HERE%..\include" ^
   "%HERE%harness_vips_fork.c" "%HERE%..\src\shm.c" "%HERE%..\src\pipe.c" "%HERE%..\src\clone.c" "%HERE%..\src\forkloop.c" ^
   /Fe:"%HERE%build\harness_vips_fork.exe" /link /DEBUG:FULL
if errorlevel 1 exit /b 1

echo [2/3] harness (crash modes, P2.5)
cl /nologo /O2 /MD /Zi /utf-8 /I "%HERE%..\include" ^
   "%HERE%harness_crash_fork.c" "%HERE%..\src\shm.c" "%HERE%..\src\pipe.c" "%HERE%..\src\clone.c" "%HERE%..\src\forkloop.c" ^
   /Fe:"%HERE%build\harness_crash_fork.exe" /link /DEBUG:FULL
if errorlevel 1 exit /b 1

echo [3/3] test driver
cl /nologo /O2 /MD /utf-8 "%HERE%forkloop_test.c" /Fe:"%HERE%build\forkloop_test.exe" /link psapi.lib
if errorlevel 1 exit /b 1

echo built
endlocal
