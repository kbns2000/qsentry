#!/bin/sh
# qsentry scripted demo, run once at boot; everything goes to the serial console.
cd /data/qsentry || exit 1
sleep 2
echo "=== QSENTRY AUTODEMO START $(date)"
./qsentryd -p 40 -w 50 > /tmp/qsentryd.log 2>&1 &
sleep 1
./qsensord -p 30 > /tmp/qsensord.log 2>&1 &
sleep 3
echo "--- [1] status: nominal"
cat /dev/qsentry
echo "inject yaw drift" > /dev/qsentry
sleep 2
echo "--- [2] status: 2 s after 'inject yaw drift'"
cat /dev/qsentry
echo reset > /dev/qsentry
sleep 1
./qsload -p 9 -n 2 > /dev/null 2>&1 &
sleep 4
echo "--- [3] status: under CPU load (qsload -p 9 -n 2)"
cat /dev/qsentry
slay qsload
slay qsensord
sleep 1
echo "--- [4] status: after 'slay qsensord' (watchdog)"
cat /dev/qsentry
echo "--- qsentryd.log"
cat /tmp/qsentryd.log
echo "=== QSENTRY AUTODEMO END"
