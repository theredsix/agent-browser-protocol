#!/bin/bash
# Package Chrome with ABP for macOS distribution
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHROMIUM_SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Validate environment
if [[ -z "${ABP_VERSION:-}" ]]; then
    echo "ERROR: ABP_VERSION environment variable is required"
    exit 1
fi

if [[ -z "${CHROME_VERSION:-}" ]]; then
    echo "ERROR: CHROME_VERSION environment variable is required"
    exit 1
fi

BUILD_ARCH="${BUILD_ARCH:-all}"
DIST_DIR="$CHROMIUM_SRC/dist"

package_arch() {
    local arch=$1
    local build_dir="$CHROMIUM_SRC/out/Release-$arch"
    local archive_name="abp-${ABP_VERSION}-chrome-${CHROME_VERSION}-mac-${arch}.zip"

    # Validate build exists
    if [[ ! -d "$build_dir/Chromium.app" ]]; then
        echo "ERROR: Chromium.app not found at $build_dir/Chromium.app"
        echo "Run build-mac.sh with BUILD_ARCH=$arch first"
        return 1
    fi

    echo "=== Packaging ABP Chrome for macOS ($arch) ==="
    echo "ABP Version: $ABP_VERSION"
    echo "Chrome Version: $CHROME_VERSION"
    echo "Build: $build_dir"
    echo "Output: $DIST_DIR/$archive_name"

    # Create dist directory
    mkdir -p "$DIST_DIR"

    # Create archive (zip preserves macOS app bundle structure)
    echo "=== Creating archive ==="
    cd "$build_dir"
    zip -r -q "$DIST_DIR/$archive_name" "Chromium.app"

    # Report results
    SIZE=$(du -h "$DIST_DIR/$archive_name" | cut -f1)
    echo "=== Package complete for $arch ==="
    echo "Archive: $DIST_DIR/$archive_name"
    echo "Size: $SIZE"
}

case "$BUILD_ARCH" in
    arm64)
        package_arch "arm64"
        ;;
    universal)
        package_arch "universal"
        ;;
    all)
        package_arch "arm64"
        package_arch "universal"
        ;;
    *)
        echo "ERROR: Invalid BUILD_ARCH: $BUILD_ARCH"
        echo "Valid values: arm64, universal, all"
        exit 1
        ;;
esac

echo "=== All macOS packages complete ==="
