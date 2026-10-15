@echo off
call "D:\HackerCode\Microsoft Visual Studio\2022\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /utf-8 shared_mem_poc.c /Fe:shared_mem_poc.exe
