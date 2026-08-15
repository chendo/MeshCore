#!/usr/bin/env bash
# Run meshweb and restart it whenever its code changes.
#
#   ./run.sh --serial dev=/dev/cu.usbmodem1101 --ble prod=<address>
#
# Any arguments are passed straight through to meshweb.py. Logs go to
# meshweb.log next to this script; the server keeps running after this shell
# exits only if you background it yourself (e.g. `./run.sh … &`).
set -uo pipefail
cd "$(dirname "$0")"

LOG="$PWD/meshweb.log"
WATCH=(meshweb.py index.html)
PY=(uv run --with aiohttp --with pyserial --with bleak python meshweb.py)

stamp() { for f in "${WATCH[@]}"; do stat -f %m "$f" 2>/dev/null || stat -c %Y "$f" 2>/dev/null; done | tr '\n' ' '; }

child=""
cleanup() { [[ -n "$child" ]] && kill "$child" 2>/dev/null; exit 0; }
trap cleanup INT TERM

# One server at a time -- two would fight over the BLE peripherals, which
# CoreBluetooth will not share between processes.
pkill -f "python meshweb.py" 2>/dev/null && sleep 1

echo "watching: ${WATCH[*]}   logging to $LOG" >&2
last="$(stamp)"
while true; do
  : > "$LOG"
  "${PY[@]}" "$@" >>"$LOG" 2>&1 &
  child=$!
  echo "[run.sh] started pid $child -- $(date '+%H:%M:%S')" >&2

  # poll rather than depend on fswatch/inotify being installed
  while kill -0 "$child" 2>/dev/null; do
    sleep 1
    now="$(stamp)"
    if [[ "$now" != "$last" ]]; then
      last="$now"
      echo "[run.sh] code changed, restarting" >&2
      kill "$child" 2>/dev/null
      wait "$child" 2>/dev/null
      sleep 1
      break
    fi
  done

  if ! kill -0 "$child" 2>/dev/null; then
    wait "$child" 2>/dev/null
    code=$?
    # 143 is SIGTERM -- our own restart, not a crash. A real crash loop should
    # be visible rather than silently hammered.
    if [[ $code -ne 0 && $code -ne 143 ]]; then
      echo "[run.sh] exited $code -- last lines:" >&2
      tail -5 "$LOG" >&2
      sleep 3
    fi
  fi
done
