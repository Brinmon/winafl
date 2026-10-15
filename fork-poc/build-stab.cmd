@echo off
call "D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /utf-8 stability_test.c /Fe:stability_test.exe
