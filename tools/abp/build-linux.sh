#!/bin/bash
# Build Chrome with ABP for Linux
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHROMIUM_SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Validate environment
if [[ -z "${ABP_VERSION:-}" ]]; then
    echo "ERROR: ABP_VERSION environment variable is required"
    echo "Usage: ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 $0"
    exit 1
fi

if [[ -z "${CHROME_VERSION:-}" ]]; then
    echo "ERROR: CHROME_VERSION environment variable is required"
    echo "Usage: ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 $0"
    exit 1
fi

# Validate we're in chromium source
if [[ ! -f "$CHROMIUM_SRC/BUILD.gn" ]]; then
    echo "ERROR: Must be run from chromium source directory"
    echo "Expected BUILD.gn at: $CHROMIUM_SRC/BUILD.gn"
    exit 1
fi

echo "=== Building ABP Chrome for Linux ==="
echo "ABP Version: $ABP_VERSION"
echo "Chrome Version: $CHROME_VERSION"
echo "Source: $CHROMIUM_SRC"

cd "$CHROMIUM_SRC"

# Configure release build
GN_ARGS='is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0'

echo "=== Configuring build with GN ==="
gn gen out/Release --args="$GN_ARGS"

echo "=== Building Chrome ==="
autoninja -C out/Release chrome

echo "=== Build complete ==="
echo "Output: $CHROMIUM_SRC/out/Release/chrome"
