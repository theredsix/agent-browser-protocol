#!/bin/bash
# Package Chrome with ABP for Linux distribution
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

BUILD_DIR="$CHROMIUM_SRC/out/Release"
DIST_DIR="$CHROMIUM_SRC/dist"
ARCHIVE_NAME="abp-${ABP_VERSION}-chrome-${CHROME_VERSION}-linux-x64.tar.gz"

# Validate build exists
if [[ ! -f "$BUILD_DIR/chrome" ]]; then
    echo "ERROR: Chrome binary not found at $BUILD_DIR/chrome"
    echo "Run build-linux.sh first"
    exit 1
fi

echo "=== Packaging ABP Chrome for Linux ==="
echo "ABP Version: $ABP_VERSION"
echo "Chrome Version: $CHROME_VERSION"
echo "Build: $BUILD_DIR"
echo "Output: $DIST_DIR/$ARCHIVE_NAME"

# Create staging directory
STAGING_DIR=$(mktemp -d)
STAGING_APP="$STAGING_DIR/abp-chrome"
mkdir -p "$STAGING_APP"

cleanup() {
    rm -rf "$STAGING_DIR"
}
trap cleanup EXIT

echo "=== Copying files ==="

# Core binary
cp "$BUILD_DIR/chrome" "$STAGING_APP/"

# Chrome sandbox (if exists)
if [[ -f "$BUILD_DIR/chrome_sandbox" ]]; then
    cp "$BUILD_DIR/chrome_sandbox" "$STAGING_APP/"
fi

# Shared libraries
cp "$BUILD_DIR"/*.so "$STAGING_APP/" 2>/dev/null || true
cp "$BUILD_DIR"/*.so.* "$STAGING_APP/" 2>/dev/null || true

# Resource files
cp "$BUILD_DIR"/*.pak "$STAGING_APP/"
cp "$BUILD_DIR/icudtl.dat" "$STAGING_APP/"

# V8 snapshot
if [[ -f "$BUILD_DIR/v8_context_snapshot.bin" ]]; then
    cp "$BUILD_DIR/v8_context_snapshot.bin" "$STAGING_APP/"
fi
if [[ -f "$BUILD_DIR/snapshot_blob.bin" ]]; then
    cp "$BUILD_DIR/snapshot_blob.bin" "$STAGING_APP/"
fi

# Locales
cp -r "$BUILD_DIR/locales" "$STAGING_APP/"

# Resources directory (if exists)
if [[ -d "$BUILD_DIR/resources" ]]; then
    cp -r "$BUILD_DIR/resources" "$STAGING_APP/"
fi

# Create dist directory
mkdir -p "$DIST_DIR"

# Create archive
echo "=== Creating archive ==="
tar -czf "$DIST_DIR/$ARCHIVE_NAME" -C "$STAGING_DIR" "abp-chrome"

# Report results
SIZE=$(du -h "$DIST_DIR/$ARCHIVE_NAME" | cut -f1)
echo "=== Package complete ==="
echo "Archive: $DIST_DIR/$ARCHIVE_NAME"
echo "Size: $SIZE"
