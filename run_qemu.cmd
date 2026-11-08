@echo off
rem Put the open62541 check into a mkqnximage QEMU image, boot it headless,
rem wait for it to finish, and keep the console log.
setlocal
set VM=%USERPROFILE%\o6q-vm
set QEMU=C:\Program Files\qemu\qemu-system-x86_64.exe
if "%QNX_HOST%"=="" call "%USERPROFILE%\qnx800\qnxsdp-env.bat"
if not exist "%~dp0out\opcua_qnx_check" call "%~dp0build.cmd" || exit /b 1
if not exist "%VM%\local\snippets" mkdir "%VM%\local\snippets"
if not exist "%VM%\payload" mkdir "%VM%\payload"
copy /y "%~dp0out\opcua_qnx_check" "%VM%\payload\" >nul
copy /y "%~dp0vm\run.sh" "%VM%\payload\" >nul
copy /y "%~dp0vm\post_start.custom" "%VM%\local\snippets\" >nul
set P=%VM:\=/%/payload
(
echo [uid=0 gid=0 perms=0755] o6q/opcua_qnx_check=%P%/opcua_qnx_check
echo [uid=0 gid=0 perms=0755] o6q/run.sh=%P%/run.sh
) > "%VM%\local\snippets\data_files.custom"
cd /d "%VM%"
call mkqnximage --noprompt --force --type=qemu --arch=x86_64 --hostname=o6q --ssh-ident=none --build || exit /b 1
if exist serial.log del serial.log
if exist qemu.pid del qemu.pid
rem -pidfile lets us wait for / stop only OUR QEMU (other QEMU windows are left alone)
start "qnx" /b "%QEMU%" -accel tcg -cpu max -display none -m 1G -smp 2 -kernel output\ifs.bin -drive file=output\disk-qemu.vmdk,if=ide,id=drv0 -nic user,model=virtio-net-pci -no-reboot -pidfile qemu.pid -serial file:serial.log
timeout /t 10 /nobreak >nul
set QPID=
if exist qemu.pid set /p QPID=<qemu.pid
echo QEMU pid=%QPID%. Waiting for QNX to run the OPC UA check and shut down (up to 15 minutes)...
set /a T=10
:wait
timeout /t 15 /nobreak >nul
set /a T+=15
if defined QPID (
  "%SystemRoot%\System32\tasklist.exe" /fi "PID eq %QPID%" | "%SystemRoot%\System32\findstr.exe" /c:"%QPID%" >nul || goto done
)
if %T% lss 900 goto wait
echo TIMEOUT after %T% s
:done
if defined QPID "%SystemRoot%\System32\taskkill.exe" /pid %QPID% /f >nul 2>&1
timeout /t 3 /nobreak >nul
if not exist "%~dp0results" mkdir "%~dp0results"
copy /y serial.log "%~dp0results\serial.log" >nul
echo elapsed_wait_s=%T%>> "%~dp0results\serial.log"
"%SystemRoot%\System32\findstr.exe" /b /c:"O6Q " "%~dp0results\serial.log"
