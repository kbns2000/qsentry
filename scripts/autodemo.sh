#!/bin/sh
# qsentry scripted demo for a QNX 8.0 target (QEMU or hardware).
# Runs once at boot from the image (or by hand: sh autodemo.sh); everything goes to the console.
# Order: nominal -> yaw drift -> clear/recover -> speed stuck (fault isolation) -> clear/recover
#        -> accel noise + yaw drift (both redundant pairs) -> SAFE_STOP -> reset
#        -> 10 s under low-priority CPU load -> sensor process killed (deadline watchdog).
D=/dev/qsentry
BIN=${QS_BIN:-/data/qsentry}
cd "$BIN" || exit 1

st()   { grep '^state=' $D | cut -d= -f2; }
show() { echo "--- [$1]"; cat $D; echo; }
# wait_state <STATE> <max seconds>: poll once per second, print how long it took
wait_state() {
    n=0
    while [ "$(st)" != "$1" ] && [ $n -lt $2 ]; do sleep 1; n=$((n + 1)); done
    echo ">>> state=$(st) after ~${n}s (wanted $1, max $2s)"
}

sleep 2
echo "=== QSENTRY AUTODEMO START $(date)"
slay -f qsentryd qsensord qsload 2>/dev/null
./qsentryd -p 40 -w 50 > /tmp/qsentryd.log 2>&1 &
sleep 1
./qsensord -p 30 > /tmp/qsensord.log 2>&1 &
sleep 5
show "1 nominal, 5 s of clean driving"

echo "inject yaw drift" > $D
wait_state DEGRADED 5
show "2 yaw drift detected"
echo clear > $D
wait_state NOMINAL 4
show "3 fault removed, recovered"

echo "inject speed stuck" > $D
wait_state DEGRADED 5
show "4 speed stuck (pair-B alarms suppressed by fault isolation)"
echo clear > $D
wait_state NOMINAL 4

echo "inject accel noise" > $D
wait_state DEGRADED 5
echo "inject yaw drift" > $D
wait_state SAFE_STOP 6
show "5 faults on both redundant pairs"
echo clear > $D
echo reset > $D
sleep 3
show "6 operator reset"

./qsload -p 9 -n 2 > /dev/null 2>&1 &
sleep 10
show "7 after 10 s under CPU load (qsload -p 9 -n 2)"
slay qsload

slay qsensord
sleep 1
show "8 sensor process killed (deadline watchdog)"
echo "--- qsentryd event log"
cat /tmp/qsentryd.log
echo "=== QSENTRY AUTODEMO END"
