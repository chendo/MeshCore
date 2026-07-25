#!/usr/bin/env bash
# Flash the ThinkNode M5 multi-identity firmware.
# Usage:
#   ./flash_multi.sh full  <merged.bin>   # full erase + write at 0x0 (use when partition layout changed)
#   ./flash_multi.sh app   <firmware.bin> # fast: overwrite just the app slot at 0x10000 (same layout)
#   PORT=/dev/ttyACM0 ./flash_multi.sh ... # override auto-detected port
#
# Needs esptool:  pip install esptool
set -euo pipefail

MODE="${1:-full}"
BIN="${2:?path to .bin required}"
CHIP="esp32s3"

# auto-detect port if not given
if [ -z "${PORT:-}" ]; then
  for p in /dev/tty.usbmodem* /dev/tty.usbserial* /dev/ttyACM* /dev/ttyUSB*; do
    [ -e "$p" ] && PORT="$p" && break
  done
fi
: "${PORT:?no serial port found - set PORT=/dev/xxx}"
echo "port: $PORT   chip: $CHIP   mode: $MODE   bin: $BIN"

# put the board in download mode automatically; if it won't sync, hold BOOT and
# tap RESET, or double-click RESET, then re-run.
case "$MODE" in
  full)
    esptool.py --chip "$CHIP" --port "$PORT" erase_flash
    esptool.py --chip "$CHIP" --port "$PORT" --baud 460800 write_flash 0x0 "$BIN"
    ;;
  app)
    esptool.py --chip "$CHIP" --port "$PORT" --baud 460800 write_flash 0x10000 "$BIN"
    ;;
  *)
    echo "unknown mode '$MODE' (use: full | app)"; exit 1 ;;
esac
echo "done - press RESET and open a serial monitor at 115200"
