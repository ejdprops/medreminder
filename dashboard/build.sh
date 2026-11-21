#!/usr/bin/env bash
# Build/flash the dashboard (CrowPanel 7", ESP32-S3).
#   ./build.sh                                        # build
#   ./build.sh -p /dev/cu.usbserial-110 -b 115200 flash   # flash (the panel's USB serial: 115200 only)
# Needs `getidf` (not getmatter). The Xtensa compiler isn't on PATH from export.sh yet: added here.
set -euo pipefail
cd "$(dirname "$0")"
command -v idf.py >/dev/null || { echo "ESP-IDF environment not loaded. Run: getidf"; exit 1; }
XTENSA="$(ls -d "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin 2>/dev/null | tail -1)"
[[ -n "$XTENSA" ]] && export PATH="$XTENSA:$PATH"
[[ -f build/CMakeCache.txt ]] || idf.py set-target esp32s3
[[ $# -eq 0 ]] && set -- build
exec idf.py "$@"
