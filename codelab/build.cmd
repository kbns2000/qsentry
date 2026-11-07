@echo off
rem Codelab: build tickdev for QNX 8.0 x86_64 on a Windows host
setlocal
if "%QNX_HOST%"=="" call "%USERPROFILE%\qnx800\qnxsdp-env.bat"
cd /d "%~dp0"
if not exist out mkdir out
qcc -Vgcc_ntox86_64 -O2 -Wall -Wextra -o out\tickdev src\tickdev.c || exit /b 1
echo BUILD OK: out\tickdev
