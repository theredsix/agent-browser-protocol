#!/bin/bash
# Build, validate, and package ABP Chrome for macOS
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHROMIUM_SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Validate environment
if [[ -z "${ABP_VERSION:-}" ]]; then
    echo "ERROR: ABP_VERSION environment variable is required"
    echo "Usage: ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 [BUILD_ARCH=arm64|universal|all] $0"
    exit 1
fi

if [[ -z "${CHROME_VERSION:-}" ]]; then
    echo "ERROR: CHROME_VERSION environment variable is required"
    echo "Usage: ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 [BUILD_ARCH=arm64|universal|all] $0"
    exit 1
fi

BUILD_ARCH="${BUILD_ARCH:-all}"

release_arch() {
    local arch=$1
    local build_dir="$CHROMIUM_SRC/out/Release-$arch"

    echo ""
    echo "============================================"
    echo "ABP Chrome Release - macOS $arch"
    echo "============================================"

    # Build
    echo ""
    echo ">>> Building $arch..."
    BUILD_ARCH="$arch" "$SCRIPT_DIR/build-mac.sh"

    # Validate
    if [[ "${SKIP_VALIDATION:-}" == "1" ]]; then
        echo ""
        echo ">>> Validation SKIPPED (SKIP_VALIDATION=1)"
    else
        echo ""
        echo ">>> Validating $arch..."
        # macOS app bundle has different binary path
        local chrome_bin="$build_dir/Chromium.app/Contents/MacOS/Chromium"
        if ! "$SCRIPT_DIR/common/validate.sh" "$chrome_bin"; then
            echo "ERROR: Validation failed for $arch"
            exit 3
        fi
    fi

    # Package
    echo ""
    echo ">>> Packaging $arch..."
    BUILD_ARCH="$arch" "$SCRIPT_DIR/package-mac.sh"
}

echo "============================================"
echo "ABP Chrome Release - macOS"
echo "============================================"
echo "ABP Version: $ABP_VERSION"
echo "Chrome Version: $CHROME_VERSION"
echo "Architecture(s): $BUILD_ARCH"
echo "============================================"

case "$BUILD_ARCH" in
    arm64)
        release_arch "arm64"
        ;;
    universal)
        release_arch "universal"
        ;;
    all)
        release_arch "arm64"
        release_arch "universal"
        ;;
    *)
        echo "ERROR: Invalid BUILD_ARCH: $BUILD_ARCH"
        exit 1
        ;;
esac

echo ""
echo "============================================"
echo "All macOS releases complete!"
echo "Archives in: $CHROMIUM_SRC/dist/"
echo "============================================"
