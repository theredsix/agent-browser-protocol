#!/bin/bash
# Copyright 2026 Han Wang. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Build, validate, sign, notarize, and package ABP Chrome for macOS
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

echo "============================================"
echo "ABP Chrome Release - macOS"
echo "============================================"
echo "ABP Version: $ABP_VERSION"
echo "============================================"

# Build
echo ""
echo ">>> Building..."
"$SCRIPT_DIR/build-mac.sh"

# Validate
if [[ "${SKIP_VALIDATION:-}" == "1" ]]; then
    echo ""
    echo ">>> Validation SKIPPED (SKIP_VALIDATION=1)"
else
    echo ""
    echo ">>> Validating..."
    chrome_bin="$BUILD_DIR/ABP.app/Contents/MacOS/ABP"
    if ! "$SCRIPT_DIR/common/validate.sh" "$chrome_bin"; then
        echo "ERROR: Validation failed"
        exit 3
    fi
fi

# Sign + Notarize
if [[ "${SKIP_SIGNING:-}" == "1" ]]; then
    echo ""
    echo ">>> Signing SKIPPED (SKIP_SIGNING=1)"
else
    echo ""
    "$SCRIPT_DIR/sign-mac.sh" "$BUILD_DIR"
fi

# Package (after notarization so the stapled ticket is included in the zip)
echo ""
echo ">>> Packaging..."
"$SCRIPT_DIR/package-mac.sh"

echo ""
echo "============================================"
echo "macOS release complete!"
echo "Archives in: $CHROMIUM_SRC/dist/"
echo "============================================"
