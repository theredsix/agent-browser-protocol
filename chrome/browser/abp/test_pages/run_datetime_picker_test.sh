#!/usr/bin/env bash
# Launch test for date/time picker interception + the <select> Linux/Windows fix.
#
# Launches ABP, loads a page with the five date/time-family inputs and a
# <select>, triggers each picker, and verifies the agent sees a
# datetime_picker_open event and can apply an ISO value; also checks the
# <select> now emits select_open (Part A).
#
# Usage: run_datetime_picker_test.sh [port]
set -u

PORT="${1:-8355}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The ABP fork's browser executable is "abp", not "chrome".
CHROME="${ABP_CHROME:-/home/paladin/src/src/out/Default/abp}"
PAGE="file://${SRC_DIR}/datetime-picker-test.html"
TMP="$(mktemp -d /tmp/abp-dtpicker.XXXXXX)"

CHROME_PID=""
cleanup() {
  [ -n "$CHROME_PID" ] && kill -9 "$CHROME_PID" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT

if [ ! -x "$CHROME" ]; then
  echo "FAIL: browser binary not found/executable: $CHROME"
  exit 1
fi

echo "Launching ABP on port $PORT ..."
"$CHROME" \
  --abp-port="$PORT" \
  --abp-session-dir="$TMP/session" \
  --user-data-dir="$TMP/profile" \
  --no-first-run --no-default-browser-check --no-sandbox \
  --disable-gpu \
  "$PAGE" >"$TMP/chrome.log" 2>&1 &
CHROME_PID=$!

python3 "$SRC_DIR/datetime_picker_check.py" "$PORT"
RC=$?

if [ "$RC" -ne 0 ]; then
  echo "--- last 25 lines of browser log ---"
  tail -25 "$TMP/chrome.log"
fi
exit "$RC"
