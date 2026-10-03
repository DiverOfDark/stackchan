#!/usr/bin/env bash
# Restore the factory (XiaoZhi) firmware from a full-flash backup (PRD D6).
# Usage: tools/restore-stock.sh backup/stackchan-stock-<mac>.bin [/dev/ttyACM0]
set -euo pipefail
img=${1:?backup image}
port=${2:-/dev/ttyACM0}
sha256sum -c "$img.sha256"
espflash write-bin --port "$port" 0x0 "$img"
