#!/bin/sh
# Runs ON the QNX 8.0 target after scripts/deploy_qemu.sh copied everything to /tmp.
# Same scripted sequence as the boot-time demo (scripts/autodemo.sh).
QS_BIN=/tmp exec sh /tmp/autodemo.sh
