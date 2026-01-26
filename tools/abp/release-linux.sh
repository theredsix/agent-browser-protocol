#!/bin/bash
# Build, validate, and package ABP Chrome for Linux
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

echo "============================================"
echo "ABP Chrome Release - Linux x64"
echo "============================================"
echo "ABP Version: $ABP_VERSION"
echo "Chrome Version: $CHROME_VERSION"
echo "============================================"

# Step 1: Build
echo ""
echo ">>> Step 1/3: Building..."
if ! "$SCRIPT_DIR/build-linux.sh"; then
    echo "ERROR: Build failed"
    exit 2
fi

# Step 2: Validate
if [[ "${SKIP_VALIDATION:-}" == "1" ]]; then
    echo ""
    echo ">>> Step 2/3: Validation SKIPPED (SKIP_VALIDATION=1)"
else
    echo ""
    echo ">>> Step 2/3: Validating..."
    if ! "$SCRIPT_DIR/common/validate.sh" "$CHROMIUM_SRC/out/Release/chrome"; then
        echo "ERROR: Validation failed"
        exit 3
    fi
fi

# Step 3: Package
echo ""
echo ">>> Step 3/3: Packaging..."
if ! "$SCRIPT_DIR/package-linux.sh"; then
    echo "ERROR: Packaging failed"
    exit 4
fi

echo ""
echo "============================================"
echo "Release complete!"
echo "Archive: $CHROMIUM_SRC/dist/abp-${ABP_VERSION}-chrome-${CHROME_VERSION}-linux-x64.tar.gz"
echo "============================================"
