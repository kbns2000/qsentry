#!/bin/sh
# Runs at boot inside the QEMU image: start tickdev and exercise /dev/tick.
echo "=== TICKDEV CHECK START"
uname -a
/data/codelab/tickdev &
sleep 1
cat /dev/tick
sleep 2
cat /dev/tick
echo "period 5" > /dev/tick
sleep 1
cat /dev/tick
echo reset > /dev/tick
cat /dev/tick
pidin -p tickdev
echo "=== TICKDEV CHECK END"
