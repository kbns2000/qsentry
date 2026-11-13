@echo off
rem Port open62541 (OPC UA) to QNX 8.0 and cross-compile the end-to-end check
setlocal
if "%QNX_HOST%"=="" call "%USERPROFILE%\qnx800\qnxsdp-env.bat"
cd /d "%~dp0"
python scripts\port.py || exit /b 1
if not exist out mkdir out
echo Compiling open62541.c (13 MB single file, takes a few minutes)...
qcc -Vgcc_ntox86_64 -O1 -w -c -Ithird_party -o out\open62541.o third_party\open62541.c || exit /b 1
qcc -Vgcc_ntox86_64 -O2 -Wall -Ithird_party -o out\opcua_qnx_check src\opcua_qnx_check.c out\open62541.o -lsocket -lcrypto -lm || exit /b 1
echo BUILD OK: out\opcua_qnx_check
