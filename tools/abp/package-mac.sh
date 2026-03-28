#!/bin/bash
# Copyright 2026 Han Wang. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Package Chrome with ABP for macOS distribution
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHROMIUM_SRC="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Read ABP_VERSION from package.json if not set
if [[ -z "${ABP_VERSION:-}" ]]; then
    _pkg_json="$SCRIPT_DIR/../abp-npm/package.json"
    if [[ -f "$_pkg_json" ]]; then
        ABP_VERSION="$(python3 -c "import json; print(json.load(open('$_pkg_json'))['version'])")"
        export ABP_VERSION
    else
        echo "ERROR: ABP_VERSION not set and $_pkg_json not found"
        exit 1
    fi
fi

BUILD_DIR="$CHROMIUM_SRC/out/Release"
DIST_DIR="$CHROMIUM_SRC/dist"

# Detect native architecture
case "$(uname -m)" in
    arm64)  ARCH="arm64" ;;
    x86_64) ARCH="x64" ;;
    *)      echo "ERROR: Unsupported architecture: $(uname -m)"; exit 1 ;;
esac

ARCHIVE_NAME="abp-${ABP_VERSION}-mac-${ARCH}.zip"

# Validate build exists
if [[ ! -d "$BUILD_DIR/ABP.app" ]]; then
    echo "ERROR: ABP.app not found at $BUILD_DIR/ABP.app"
    echo "Run build-mac.sh first"
    exit 1
fi

echo "=== Packaging ABP Chrome for macOS ($ARCH) ==="
echo "ABP Version: $ABP_VERSION"
echo "Build: $BUILD_DIR"
echo "Output: $DIST_DIR/$ARCHIVE_NAME"

# Create dist directory
mkdir -p "$DIST_DIR"

# Create archive (zip preserves macOS app bundle structure)
echo "=== Creating archive ==="
cd "$BUILD_DIR"
zip -r -y -q "$DIST_DIR/$ARCHIVE_NAME" "ABP.app"

# Report results
SIZE=$(du -h "$DIST_DIR/$ARCHIVE_NAME" | cut -f1)
echo "=== Package complete ==="
echo "Archive: $DIST_DIR/$ARCHIVE_NAME"
echo "Size: $SIZE"
