@echo off
rem Build a QNX 8.0 QEMU image containing qsentry and boot it; console -> serial.log
setlocal
set ROOT=%~dp0..\..
set VM=%USERPROFILE%\qsentry-vm
set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
if "%QNX_HOST%"=="" call "%USERPROFILE%\qnx800\qnxsdp-env.bat"
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
set ACCEL=whpx
start "qnx-qemu" /b "%QEMU%" -accel whpx -display none -m 1G -smp 2 -kernel output\ifs.bin -drive file=output\disk-qemu.vmdk,if=ide,id=drv0 -nic none -serial file:serial.log
timeout /t 8 /nobreak >nul
tasklist /fi "imagename eq qemu-system-x86_64.exe" | find /i "qemu-system" >nul || (
  set ACCEL=tcg
  start "qnx-qemu" /b "%QEMU%" -accel tcg -cpu max -display none -m 1G -smp 2 -kernel output\ifs.bin -drive file=output\disk-qemu.vmdk,if=ide,id=drv0 -nic none -serial file:serial.log
)
timeout /t 200 /nobreak >nul
taskkill /im qemu-system-x86_64.exe /f >nul 2>&1
echo accel=%ACCEL%>> serial.log
type serial.log
