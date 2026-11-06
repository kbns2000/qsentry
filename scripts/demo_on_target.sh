#!/bin/sh
# Runs ON the QNX 8.0 target (QEMU or Raspberry Pi).
cd /tmp
slay -f qsentryd qsensord qsload 2>/dev/null
./qsentryd -p 40 -w 50 > /tmp/qsentryd.log 2>&1 &
sleep 1
./qsensord -p 30 > /tmp/qsensord.log 2>&1 &
sleep 2
echo "--- cat /dev/qsentry ---"
cat /dev/qsentry
echo
echo "Live dashboard: ./qsentry-top    (keys: 1-4 channel, s/d/p/n inject, c clear, r reset)"
echo "Scripted faults: echo 'inject yaw drift' > /dev/qsentry ; echo clear > /dev/qsentry"
echo "Watchdog demo:   slay qsensord   (SAFE_STOP within the 50 ms deadline)"
echo "Load demo:       ./qsload -p 9 -n 2 &   (jitter stays flat for prio-40 detector)"
