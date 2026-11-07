@echo off
rem Cross-compile ipcbench for QNX 8.0 x86_64 on a Windows host
setlocal
if "%QNX_HOST%"=="" call "%USERPROFILE%\qnx800\qnxsdp-env.bat"
cd /d "%~dp0"
if not exist out mkdir out
qcc -Vgcc_ntox86_64 -O2 -Wall -Wextra -Isrc -o out\ipcbench src\ipcbench.c src\stats.c || exit /b 1
echo BUILD OK: out\ipcbench
