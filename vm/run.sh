#!/bin/sh
# Runs inside the QNX guest at boot (vm/post_start.custom starts it).
# The output goes to the console, which QEMU writes to serial.log on the host;
# scripts/o6q.py report reads the "O6Q ..." lines from there.
# SPDX-License-Identifier: Apache-2.0
D=/data/o6q
cd $D || exit 1
LD_LIBRARY_PATH=$D/lib:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH

run() {
    name=$1
    shift
    echo "O6Q RUN $name"
    "$@"
    echo "O6Q EXIT $name $?"
}

echo "O6Q GUEST START"
uname -a
pidin info
ls -l $D $D/lib

# What the operating system does (no open62541 in this program)
run envprobe ./o6q_envprobe

# The port, then the unchanged release, suite by suite
for s in core security pubsub highfd robust; do
    run check.$s ./o6q_check $s
done
for s in core security pubsub highfd robust; do
    run upstream.$s ./o6q_check_upstream $s
done

# A system without the pipe manager. From here until "pipe &" no pipelines
# and no command substitution: they need the pipe manager.
echo "O6Q GUEST stopping the pipe manager"
slay -f pipe
sleep 1
run check.nopipe ./o6q_check nopipe
run upstream.nopipe ./o6q_check_upstream nopipe
pipe &
sleep 1

# The demo server for the host client (only in the image made with --interop)
if [ -f $D/demo.conf ]; then
    . $D/demo.conf
    echo "O6Q GUEST demo server for the host client"
    ./o6q_server -p 4840 -s 420 --secure --trust $D/pki/trusted \
        --user "$O6Q_DEMO_USER:$O6Q_DEMO_PASS" &
    srv=$!
    wait $srv
    echo "O6Q EXIT demo $?"
    echo "O6Q GUEST slog2 messages of the demo server:"
    slog2info -b o6q_server
fi

echo "O6Q GUEST DONE"
sleep 2
# QEMU runs with -no-reboot: the reboot ends the virtual machine
shutdown
