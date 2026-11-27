@echo off
rem One command: cross-compile qsentry if needed, build a QNX 8.0 QEMU image with it, boot it; console -> serial.log
setlocal
set ROOT=%~dp0..\..
set VM=%USERPROFILE%\qsentry-vm
set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
if "%QNX_HOST%"=="" call "%USERPROFILE%\qnx800\qnxsdp-env.bat"
if not exist "%ROOT%\build\qnx-x86_64\qsentryd" call "%ROOT%\scripts\build_qnx.cmd" x86_64 || exit /b 1
if not exist "%VM%\local\snippets" mkdir "%VM%\local\snippets"
if not exist "%VM%\payload" mkdir "%VM%\payload"
copy /y "%ROOT%\build\qnx-x86_64\*" "%VM%\payload\" >nul
copy /y "%ROOT%\scripts\autodemo.sh" "%VM%\payload\" >nul
copy /y "%~dp0snippets\post_start.custom" "%VM%\local\snippets\" >nul
set P=%VM:\=/%/payload
(
echo [uid=0 gid=0 perms=0755] qsentry/qsentryd=%P%/qsentryd
echo [uid=0 gid=0 perms=0755] qsentry/qsensord=%P%/qsensord
echo [uid=0 gid=0 perms=0755] qsentry/qsentry-top=%P%/qsentry-top
echo [uid=0 gid=0 perms=0755] qsentry/qsload=%P%/qsload
echo [uid=0 gid=0 perms=0755] qsentry/autodemo.sh=%P%/autodemo.sh
) > "%VM%\local\snippets\data_files.custom"
cd /d "%VM%"
call mkqnximage --noprompt --force --type=qemu --arch=x86_64 --hostname=qsentry --ssh-ident=none --build || exit /b 1
if exist serial.log del serial.log
if exist qemu.pid del qemu.pid
rem waits use ping, not timeout: timeout exits at once when the console is not interactive
rem only the QEMU started here is stopped (its PID from -pidfile), never other QEMU processes
set ACCEL=whpx
start "qnx-qemu" /b "%QEMU%" -accel whpx -display none -m 1G -smp 2 -kernel output\ifs.bin -drive file=output\disk-qemu.vmdk,if=ide,id=drv0 -nic none -serial file:serial.log -pidfile qemu.pid
ping -n 9 127.0.0.1 >nul
call :alive
if not defined ALIVE (
  set ACCEL=tcg
  if exist qemu.pid del qemu.pid
  start "qnx-qemu" /b "%QEMU%" -accel tcg -cpu max -display none -m 1G -smp 2 -kernel output\ifs.bin -drive file=output\disk-qemu.vmdk,if=ide,id=drv0 -nic none -serial file:serial.log -pidfile qemu.pid
  ping -n 4 127.0.0.1 >nul
)
ping -n 201 127.0.0.1 >nul
set QPID=
if exist qemu.pid set /p QPID=<qemu.pid
if defined QPID taskkill /pid %QPID% /f >nul 2>&1
echo accel=%ACCEL%>> serial.log
type serial.log
exit /b 0

:alive
set ALIVE=
set QPID=
if exist qemu.pid set /p QPID=<qemu.pid
if not defined QPID exit /b 0
"%SystemRoot%\System32\tasklist.exe" /fi "PID eq %QPID%" /nh | "%SystemRoot%\System32\findstr.exe" /i "qemu-system" >nul && set ALIVE=1
exit /b 0
