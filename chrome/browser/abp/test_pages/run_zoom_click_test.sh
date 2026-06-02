#!/usr/bin/env bash
# Launch test for the ABP zoom/click coordinate fix.
#
# Launches ABP (which forces 80% page zoom by default), loads a test page with
# a small target at a known CSS position, then verifies that clicking the
# target's screenshot-pixel (DIP) coordinate actually hits it. Fails before the
# DIP-vs-CSS coordinate fix, passes after.
#
# Usage: run_zoom_click_test.sh [port]
#
# Notes:
#  - Uses --disable-gpu (ABP's supported software-rendering fallback) so it runs
#    headless under WSL/CI where the GPU/EGL stack is unavailable.
#  - Cleans up by PID only. Never `pkill -f <chrome path>`: the launcher's own
#    shell command line contains that path, so it would kill itself.
set -u

PORT="${1:-8333}"
# Page zoom to test at. ABP's executable defaults to 100% (chrome_zoom_level_prefs
# defaults zoom_factor=1.0 unless --abp-zoom is given). The DIP-vs-CSS bug only
# manifests at zoom != 100%, so default this test to the reported 80% scenario.
ZOOM="${2:-0.8}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The ABP fork renames the browser executable to "abp" (chrome/BUILD.gn:
# _chrome_output_name = "abp"). out/Default/chrome is a stale pre-rename leftover.
CHROME="${ABP_CHROME:-/home/paladin/src/src/out/Default/abp}"
PAGE="file://${SRC_DIR}/zoom-click-test.html"
TMP="$(mktemp -d /tmp/abp-zoomtest.XXXXXX)"

CHROME_PID=""
cleanup() {
  [ -n "$CHROME_PID" ] && kill -9 "$CHROME_PID" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT

if [ ! -x "$CHROME" ]; then
  echo "FAIL: chrome binary not found/executable: $CHROME"
  exit 1
fi

echo "Launching ABP on port $PORT at ${ZOOM} page zoom ..."
"$CHROME" \
  --abp-port="$PORT" \
  --abp-zoom="$ZOOM" \
  --abp-session-dir="$TMP/session" \
  --user-data-dir="$TMP/profile" \
  --no-first-run --no-default-browser-check --no-sandbox \
  --disable-gpu \
  "$PAGE" >"$TMP/chrome.log" 2>&1 &
CHROME_PID=$!

python3 "$SRC_DIR/zoom_click_check.py" "$PORT"
RC=$?

if [ "$RC" -ne 0 ]; then
  echo "--- last 25 lines of chrome.log ---"
  tail -25 "$TMP/chrome.log"
fi
exit "$RC"
