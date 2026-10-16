@echo off
call "D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /MD /Zi /utf-8 /W3 /I "%~dp0..\include" ^
   /c "%~dp0..\src\shm.c" "%~dp0..\src\pipe.c" "%~dp0..\src\clone.c" "%~dp0..\src\forkloop.c" ^
   /Fo:obj\
