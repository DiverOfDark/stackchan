#!/usr/bin/env bash
# Build the firmware image and update Femto over Wi-Fi (no USB).
# Usage: tools/ota.sh [host]   (default femto.local)
# Same as uploading firmware/target/femto.bin in the web UI → System.
set -euo pipefail
host=${1:-femto.local}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root/firmware"
. ~/export-esp.sh
cargo build --release
elf=target/xtensa-esp32s3-espidf/release/femto
espflash save-image --chip esp32s3 --flash-size 16mb "$elf" target/femto.bin
size=$(stat -c %s target/femto.bin)
echo "uploading $size bytes to http://$host/api/ota …"
curl --fail-with-body -sS -X POST --data-binary @target/femto.bin \
     -H 'Content-Type: application/octet-stream' "http://$host/api/ota"
echo "done; Femto reboots into the new image (kept if healthy for 60 s)."
