#!/usr/bin/env bash
# Copy binaries + demo script into the running QNX VM.
set -euo pipefail
ARCH="${ARCH:-x86_64}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VM="${VM_DIR:-$HOME/qsentry-vm}"
IP="${QNX_IP:-$(cd "$VM" && mkqnximage --getip 2>/dev/null || echo 172.31.1.11)}"
scp -o StrictHostKeyChecking=no "$ROOT"/build/qnx-"$ARCH"/* "$ROOT"/scripts/demo_on_target.sh "$ROOT"/scripts/autodemo.sh root@"$IP":/tmp/
echo "deployed to $IP:/tmp  ->  ssh root@$IP  then  sh /tmp/demo_on_target.sh"
