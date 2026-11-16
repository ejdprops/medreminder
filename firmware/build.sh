#!/usr/bin/env bash
# Build/flash medreminder firmware for one board.
#   ./build.sh <board> [fresh] [idf.py args...]
# Examples:
#   ./build.sh c5_waveshare                                   # build
#   ./build.sh c5_waveshare -p /dev/cu.usbmodem1101 flash monitor
#   ./build.sh c5_waveshare fresh                             # regenerate sdkconfig from defaults, then build
#   NET=wifi ./build.sh c5_waveshare                          # use boards/<board>/sdkconfig.defaults.wifi
set -euo pipefail
cd "$(dirname "$0")"

BOARD="${1:-}"
if [[ -z "$BOARD" || ! -d "boards/$BOARD" ]]; then
    echo "Usage: $0 <board> [fresh] [idf.py args...]"
    echo "Boards: $(ls boards | tr '\n' ' ')"
    exit 1
fi
shift

if ! command -v idf.py >/dev/null || [[ -z "${ESP_MATTER_PATH:-}" ]]; then
    echo "ESP-IDF/esp-matter environment not loaded. Run: getmatter"
    exit 1
fi

TARGET="$(cat "boards/$BOARD/target")"
NET="${NET:-thread}"
NET_DEFAULTS="boards/$BOARD/sdkconfig.defaults.$NET"
[[ -f "$NET_DEFAULTS" ]] || { echo "Missing $NET_DEFAULTS"; exit 1; }

BUILD_DIR="build/$BOARD"
SDKCONFIG="$BUILD_DIR/sdkconfig"
DEFAULTS="sdkconfig.defaults;$NET_DEFAULTS;boards/$BOARD/sdkconfig.defaults"
IDF=(idf.py -B "$BUILD_DIR" -DBOARD="$BOARD" -DSDKCONFIG="$SDKCONFIG" -DSDKCONFIG_DEFAULTS="$DEFAULTS")

if [[ "${1:-}" == "fresh" ]]; then
    rm -f "$SDKCONFIG"
    shift
fi

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "First build for $BOARD ($TARGET): setting target"
    "${IDF[@]}" set-target "$TARGET"
fi

[[ $# -eq 0 ]] && set -- build
exec "${IDF[@]}" "$@"
