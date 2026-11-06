#!/usr/bin/env bash
# Build a QNX 8.0 QEMU image with mkqnximage (ships with SDP 8.0), boot it,
# and copy the qsentry binaries in over ssh.
# Linux / WSL2 host. Usage: scripts/run_qemu.sh            (x86_64 target)
set -euo pipefail
ARCH="${ARCH:-x86_64}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VM="${VM_DIR:-$HOME/qsentry-vm}"
[ -n "${QNX_HOST:-}" ] || source "$HOME/qnx800/qnxsdp-env.sh"

mkdir -p "$VM"
cd "$VM"
if [ ! -f output/ifs.bin ] && [ ! -d output ]; then
  mkqnximage --type=qemu --arch="$ARCH" --hostname=qsentry --build
fi
echo "Booting QNX in QEMU (console in this terminal; Ctrl-A X quits)."
echo "In a second terminal run: scripts/deploy_qemu.sh"
mkqnximage --run
