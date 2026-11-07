#!/bin/sh
# Runs at boot inside the QEMU image (started by post_start.custom).
echo "=== IPCBENCH START"
uname -a
pidin info | head -5
/data/ipcbench/ipcbench
echo "=== IPCBENCH END"
sleep 2
# reboot request; QEMU runs with -no-reboot, so this ends the VM
shutdown
