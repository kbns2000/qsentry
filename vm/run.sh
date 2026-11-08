#!/bin/sh
# Runs at boot inside the QEMU image (started by post_start.custom).
echo "=== O6Q START"
uname -a
pidin info | head -4
ifconfig lo0 2>&1 | head -3
ls -l /data/o6q/opcua_qnx_check
/data/o6q/opcua_qnx_check 1000
echo "O6Q EXIT $?"
echo "=== O6Q END"
sleep 2
# reboot request; QEMU runs with -no-reboot, so this ends the VM
shutdown
