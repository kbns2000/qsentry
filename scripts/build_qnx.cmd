@echo off
rem Cross-compile qsentry for QNX 8.0 on a Windows host (no make needed).
rem Usage: scripts\build_qnx.cmd [x86_64|aarch64le]   (SDP default: %USERPROFILE%\qnx800)
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x86_64
if "%QNX_HOST%"=="" (
  if exist "%USERPROFILE%\qnx800\qnxsdp-env.bat" call "%USERPROFILE%\qnx800\qnxsdp-env.bat"
)
where qcc >nul 2>nul || (echo qcc not found: run ^<SDP^>\qnxsdp-env.bat first & exit /b 1)
cd /d "%~dp0.."
set OUT=build\qnx-%ARCH%
if not exist "%OUT%" mkdir "%OUT%"
set QF=-Vgcc_nto%ARCH% -O2 -std=gnu11 -Wall -Wextra
set INC=-Iinclude -Isrc/core -Isrc/qnx
set CORE=src/core/sim.c src/core/features.c src/core/model.c src/core/supervisor.c
qcc %QF% %INC% -o %OUT%/qsentryd src/qnx/qsentryd.c %CORE% -lm || exit /b 1
qcc %QF% %INC% -o %OUT%/qsensord src/qnx/qsensord.c %CORE% -lm || exit /b 1
qcc %QF% -o %OUT%/qsentry-top src/qnx/qsentry_top.c || exit /b 1
qcc %QF% -o %OUT%/qsload src/qnx/qsload.c || exit /b 1
echo BUILD OK: %OUT%
dir /b %OUT%
