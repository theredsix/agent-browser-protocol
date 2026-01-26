#!/bin/bash
# Build Chrome with ABP for macOS
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHROMIUM_SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Validate environment
if [[ -z "${ABP_VERSION:-}" ]]; then
    echo "ERROR: ABP_VERSION environment variable is required"
    echo "Usage: ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 [BUILD_ARCH=arm64|universal] $0"
    exit 1
fi

if [[ -z "${CHROME_VERSION:-}" ]]; then
    echo "ERROR: CHROME_VERSION environment variable is required"
    echo "Usage: ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 [BUILD_ARCH=arm64|universal] $0"
    exit 1
fi

# Validate we're in chromium source
if [[ ! -f "$CHROMIUM_SRC/BUILD.gn" ]]; then
    echo "ERROR: Must be run from chromium source directory"
    echo "Expected BUILD.gn at: $CHROMIUM_SRC/BUILD.gn"
    exit 1
fi

BUILD_ARCH="${BUILD_ARCH:-all}"

build_arch() {
    local arch=$1
    local out_dir="out/Release-$arch"

    echo "=== Building ABP Chrome for macOS ($arch) ==="
    echo "ABP Version: $ABP_VERSION"
    echo "Chrome Version: $CHROME_VERSION"
    echo "Source: $CHROMIUM_SRC"
    echo "Output: $out_dir"

    cd "$CHROMIUM_SRC"

    # Configure release build
    local GN_ARGS='is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0'

    if [[ "$arch" == "arm64" ]]; then
        GN_ARGS="$GN_ARGS target_cpu=\"arm64\""
    elif [[ "$arch" == "universal" ]]; then
        GN_ARGS="$GN_ARGS target_cpu=\"arm64\" use_lipo=true"
    fi

    echo "=== Configuring build with GN ==="
    gn gen "$out_dir" --args="$GN_ARGS"

    echo "=== Building Chrome ==="
    autoninja -C "$out_dir" chrome

    echo "=== Build complete for $arch ==="
}

case "$BUILD_ARCH" in
    arm64)
        build_arch "arm64"
        ;;
    universal)
        build_arch "universal"
        ;;
    all)
        build_arch "arm64"
        build_arch "universal"
        ;;
    *)
        echo "ERROR: Invalid BUILD_ARCH: $BUILD_ARCH"
        echo "Valid values: arm64, universal, all"
        exit 1
        ;;
esac

echo "=== All macOS builds complete ==="
