# escape=`
# ============================================================
# WinAFL portable fuzzing container
#
# The build context is dist\winafl-portable (produced by package.ps1),
# NOT the repo root. Use docker-build.ps1 which wires this up:
#     docker build -f Dockerfile dist\winafl-portable
#
# Ships the pre-built, verified WinAFL bundle (TinyInst/IntelPT/DynamoRIO)
# so the image needs no in-container compilation.
# ============================================================
FROM mcr.microsoft.com/windows/servercore:ltsc2022

LABEL description="WinAFL portable fuzzing environment (TinyInst/IntelPT/DynamoRIO)"

# context = dist\winafl-portable, so COPY . grabs the whole bundle
COPY . C:\winafl

WORKDIR C:\winafl

# default: show fuzzer help. For a live demo:  docker run --rm <img> cmd /c C:\winafl\demo.cmd
CMD ["cmd", "/c", "afl-fuzz.exe"]
