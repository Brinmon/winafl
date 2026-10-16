@echo off
dbgkit cdb run --binary "E:\GitCode\winafl\build64\bin\Release\afl-fuzz.exe" --cwd "E:\fuzzwork-real\harnesses\qq_image_vips" --script "E:\GitCode\winafl\attribution\cdb\run.cmd" --timeout 70 ^
 --args=-i --args=./_seeds --args=-o --args=E:\GitCode\winafl\attribution\out_a3d --args=-t --args=20000 --args=-I --args=15000 --args=-y --args=-- ^
 --args=-instrument_module --args=libvips-42.dll --args=-target_module --args=harness_vips.exe --args=-target_method --args=fuzz_one --args=-nargs --args=1 --args=-iterations --args=2000 --args=-persist --args=-loop --args=-- ^
 --args=artifacts/harness_vips.exe --args=@@
