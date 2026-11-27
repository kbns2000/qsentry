#!/bin/sh
# Runs inside the QNX 8.0 guest at boot (vm/post_start.custom starts it).
# The console goes to serial.log on the host; scripts/qv.py report reads the
# "QV ..." lines from there.
# SPDX-License-Identifier: Apache-2.0
D=/data/qv
cd $D || exit 1

run() {
    name=$1
    shift
    echo "QV RUN $name"
    "$@"
    echo "QV EXIT $name $?"
}

echo "QV GUEST START"
uname -a
pidin info

# The resource manager: two buses, /dev/vcan0 for the tests and the bridge,
# /dev/vcan1 for the demo
./devvcan -n 2 &
waitfor /dev/vcan0 5
waitfor /dev/vcan1 5
ls -l /dev/vcan0 /dev/vcan1
pidin -p devvcan

# The test program: every test prints QV BEGIN / PASS / FAIL, then a summary
run tests ./vcan_test

# The UDP bridge to the host. QEMU's user network gives the guest 10.0.2.15
# by DHCP and forwards host port 47001 to it; the host is 10.0.2.2.
i=0
while [ $i -lt 15 ]; do
    case "$(ifconfig vtnet0 2>&1)" in *"inet 10.0.2."*) break ;; esac
    sleep 1
    i=$((i + 1))
done
case "$(ifconfig vtnet0 2>&1)" in
    *"inet 10.0.2."*) echo "QV GUEST vtnet0 has its DHCP address" ;;
    *) echo "QV GUEST vtnet0 has no DHCP address after 15 s; setting 10.0.2.15"
       slay -f dhcpcd >/dev/null 2>&1
       ifconfig vtnet0 inet 10.0.2.15 netmask 255.255.255.0 up
       route add default 10.0.2.2 >/dev/null 2>&1 ;;
esac
ifconfig vtnet0
./vcanbridge /dev/vcan0 47001 10.0.2.2:47002 &
br=$!
sleep 1
run bridge ./vcan_test bridge
kill $br
wait $br

# The demo: four nodes on one 125 kbit/s bus, one of them faulty (it destroys
# every third frame it sends, so its error counter climbs to bus-off and the
# generator resets it), one node that sees only error frames, one that dumps.
echo "QV DEMO four nodes on /dev/vcan1 at 125 kbit/s, 0x2B0 is faulty"
./vcan rate /dev/vcan1 125000
./vcan dump /dev/vcan1 -e -f 7FF:FFFFFFFF -t 7 > /tmp/qv_err.txt &
e=$!
./vcan gen /dev/vcan1 -I 0C0 -L 8 -g 10 -n 500 > /tmp/qv_g1.txt &
g1=$!
./vcan gen /dev/vcan1 -I 1A0 -L 4 -g 20 -n 250 > /tmp/qv_g2.txt &
g2=$!
./vcan gen /dev/vcan1 -I 18FEF100 -L 8 -g 100 -n 50 > /tmp/qv_g3.txt &
g3=$!
./vcan gen /dev/vcan1 -I 2B0 -L 8 -g 20 -n 250 -F 3 > /tmp/qv_g4.txt &
g4=$!
sleep 1
run demo.dump ./vcan dump /dev/vcan1 -n 24
run demo.stat ./vcan stat /dev/vcan1
pidin -p devvcan
wait $g1
wait $g2
wait $g3
wait $g4
wait $e
cat /tmp/qv_g1.txt /tmp/qv_g2.txt /tmp/qv_g3.txt /tmp/qv_g4.txt
echo "QV DEMO error frames seen by the error-only node (first 12):"
head -n 12 /tmp/qv_err.txt
echo "QV DEMO error frames in total: $(grep -c ERROR /tmp/qv_err.txt), bus-off events: $(grep -c bus-off /tmp/qv_err.txt)"
run demo.stat.end ./vcan stat /dev/vcan1
run demo.stat.bus0 ./vcan stat /dev/vcan0

echo "QV GUEST DONE"
sleep 2
# QEMU runs with -no-reboot: the reboot ends the virtual machine
shutdown
