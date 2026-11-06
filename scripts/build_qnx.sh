#!/usr/bin/env bash
# Cross-compile qsentry for QNX 8.0.  Usage: scripts/build_qnx.sh [x86_64|aarch64le]
set -euo pipefail
ARCH="${1:-x86_64}"
if [ -z "${QNX_HOST:-}" ]; then
  for f in "$HOME/qnx800/qnxsdp-env.sh" /opt/qnx800/qnxsdp-env.sh; do
    [ -f "$f" ] && { source "$f"; break; }
  done
fi
command -v qcc >/dev/null || { echo "qcc not found: source <SDP>/qnxsdp-env.sh first"; exit 1; }
cd "$(dirname "$0")/.."
make qnx QNX_ARCH="$ARCH"
file build/qnx-"$ARCH"/* || true
