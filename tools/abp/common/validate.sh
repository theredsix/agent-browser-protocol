#!/bin/bash
# Validate ABP Chrome is functional
set -euo pipefail

CHROME_BINARY="${1:-}"
TIMEOUT_SECONDS="${2:-10}"

if [[ -z "$CHROME_BINARY" ]]; then
    echo "ERROR: Chrome binary path required"
    echo "Usage: $0 <chrome-binary> [timeout-seconds]"
    exit 1
fi

if [[ ! -x "$CHROME_BINARY" ]]; then
    echo "ERROR: Chrome binary not found or not executable: $CHROME_BINARY"
    exit 1
fi

echo "=== Validating ABP Chrome ==="
echo "Binary: $CHROME_BINARY"
echo "Timeout: ${TIMEOUT_SECONDS}s"

# Start Chrome with ABP in headless mode
echo "Starting Chrome with --enable-abp..."
"$CHROME_BINARY" --enable-abp --headless=new --no-sandbox --disable-gpu --remote-debugging-port=0 &
CHROME_PID=$!

cleanup() {
    if kill -0 "$CHROME_PID" 2>/dev/null; then
        echo "Stopping Chrome (PID: $CHROME_PID)..."
        kill "$CHROME_PID" 2>/dev/null || true
        wait "$CHROME_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Wait for ABP endpoint to be ready
echo "Waiting for ABP endpoint..."
ELAPSED=0
while [[ $ELAPSED -lt $TIMEOUT_SECONDS ]]; do
    if curl -s -o /dev/null -w "%{http_code}" http://localhost:8222/api/v1/tabs 2>/dev/null | grep -q "200"; then
        echo "ABP endpoint responding (HTTP 200)"

        # Verify response is valid JSON with tabs array
        RESPONSE=$(curl -s http://localhost:8222/api/v1/tabs)
        if echo "$RESPONSE" | grep -q '"tabs"'; then
            echo "=== Validation PASSED ==="
            exit 0
        else
            echo "ERROR: Unexpected response format: $RESPONSE"
            exit 3
        fi
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
    echo "  Waiting... ($ELAPSED/${TIMEOUT_SECONDS}s)"
done

echo "ERROR: ABP endpoint did not respond within ${TIMEOUT_SECONDS}s"
exit 3
