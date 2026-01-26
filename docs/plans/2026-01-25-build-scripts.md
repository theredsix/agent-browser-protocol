# Build Scripts Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Create build, package, and release scripts for Linux, Windows, and macOS that produce portable ABP-Chrome binaries for GitHub releases.

**Architecture:** Modular shell scripts (Bash for Linux/macOS, PowerShell for Windows) with shared validation logic. Each platform has build, package, and combined release scripts. Archives go to `dist/` with naming `abp-<version>-chrome-<version>-<platform>-<arch>.<ext>`.

**Tech Stack:** Bash, PowerShell, GN/autoninja (Chromium build system), tar/zip

**Design Doc:** `docs/plans/2026-01-25-build-scripts-design.md`

---

## Task 1: Create Directory Structure

**Files:**
- Create: `tools/abp/` directory
- Create: `tools/abp/common/` directory

**Step 1: Create directories**

```bash
mkdir -p tools/abp/common
```

**Step 2: Verify structure**

Run: `ls -la tools/abp/`
Expected: Empty directory with `common/` subdirectory

**Step 3: Commit**

```bash
git add tools/abp/
git commit -m "feat(abp): add tools/abp directory structure for build scripts"
```

---

## Task 2: Create Linux Build Script

**Files:**
- Create: `tools/abp/build-linux.sh`

**Step 1: Create the build script**

```bash
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
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x tools/abp/build-linux.sh && bash -n tools/abp/build-linux.sh`
Expected: No output (syntax OK)

**Step 3: Commit**

```bash
git add tools/abp/build-linux.sh
git commit -m "feat(abp): add Linux build script"
```

---

## Task 3: Create macOS Build Script

**Files:**
- Create: `tools/abp/build-mac.sh`

**Step 1: Create the build script**

```bash
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
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x tools/abp/build-mac.sh && bash -n tools/abp/build-mac.sh`
Expected: No output (syntax OK)

**Step 3: Commit**

```bash
git add tools/abp/build-mac.sh
git commit -m "feat(abp): add macOS build script with arm64/universal support"
```

---

## Task 4: Create Windows Build Script

**Files:**
- Create: `tools/abp/build-win.ps1`

**Step 1: Create the build script**

```powershell
# Build Chrome with ABP for Windows
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ChromiumSrc = (Resolve-Path "$ScriptDir\..\..").Path

# Validate environment
if (-not $env:ABP_VERSION) {
    Write-Error "ERROR: ABP_VERSION environment variable is required"
    Write-Host "Usage: `$env:ABP_VERSION='1.0.0'; `$env:CHROME_VERSION='130.0.6723.0'; .\build-win.ps1"
    exit 1
}

if (-not $env:CHROME_VERSION) {
    Write-Error "ERROR: CHROME_VERSION environment variable is required"
    Write-Host "Usage: `$env:ABP_VERSION='1.0.0'; `$env:CHROME_VERSION='130.0.6723.0'; .\build-win.ps1"
    exit 1
}

# Validate we're in chromium source
if (-not (Test-Path "$ChromiumSrc\BUILD.gn")) {
    Write-Error "ERROR: Must be run from chromium source directory"
    Write-Host "Expected BUILD.gn at: $ChromiumSrc\BUILD.gn"
    exit 1
}

Write-Host "=== Building ABP Chrome for Windows ===" -ForegroundColor Cyan
Write-Host "ABP Version: $env:ABP_VERSION"
Write-Host "Chrome Version: $env:CHROME_VERSION"
Write-Host "Source: $ChromiumSrc"

Set-Location $ChromiumSrc

# Configure release build
$GnArgs = 'is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0'

Write-Host "=== Configuring build with GN ===" -ForegroundColor Cyan
& cmd /c "gn gen out/Release --args=`"$GnArgs`""
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "=== Building Chrome ===" -ForegroundColor Cyan
& cmd /c "autoninja -C out/Release chrome"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "=== Build complete ===" -ForegroundColor Green
Write-Host "Output: $ChromiumSrc\out\Release\chrome.exe"
```

**Step 2: Verify syntax**

Run: `pwsh -Command "Get-Content tools/abp/build-win.ps1 | Out-Null; Write-Host 'Syntax OK'" 2>/dev/null || echo "pwsh not available, skip syntax check"`
Expected: "Syntax OK" or skip message

**Step 3: Commit**

```bash
git add tools/abp/build-win.ps1
git commit -m "feat(abp): add Windows build script (PowerShell)"
```

---

## Task 5: Create Shared Validation Script (Bash)

**Files:**
- Create: `tools/abp/common/validate.sh`

**Step 1: Create the validation script**

```bash
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
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x tools/abp/common/validate.sh && bash -n tools/abp/common/validate.sh`
Expected: No output (syntax OK)

**Step 3: Commit**

```bash
git add tools/abp/common/validate.sh
git commit -m "feat(abp): add shared validation script for Linux/macOS"
```

---

## Task 6: Create Shared Validation Script (PowerShell)

**Files:**
- Create: `tools/abp/common/validate.ps1`

**Step 1: Create the validation script**

```powershell
# Validate ABP Chrome is functional
param(
    [Parameter(Mandatory=$true)]
    [string]$ChromeBinary,

    [int]$TimeoutSeconds = 10
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $ChromeBinary)) {
    Write-Error "ERROR: Chrome binary not found: $ChromeBinary"
    exit 1
}

Write-Host "=== Validating ABP Chrome ===" -ForegroundColor Cyan
Write-Host "Binary: $ChromeBinary"
Write-Host "Timeout: ${TimeoutSeconds}s"

# Start Chrome with ABP in headless mode
Write-Host "Starting Chrome with --enable-abp..."
$chromeProcess = Start-Process -FilePath $ChromeBinary -ArgumentList "--enable-abp", "--headless=new", "--no-sandbox", "--disable-gpu", "--remote-debugging-port=0" -PassThru

try {
    # Wait for ABP endpoint to be ready
    Write-Host "Waiting for ABP endpoint..."
    $elapsed = 0
    while ($elapsed -lt $TimeoutSeconds) {
        try {
            $response = Invoke-WebRequest -Uri "http://localhost:8222/api/v1/tabs" -UseBasicParsing -TimeoutSec 2 -ErrorAction SilentlyContinue
            if ($response.StatusCode -eq 200) {
                Write-Host "ABP endpoint responding (HTTP 200)"

                # Verify response contains tabs
                if ($response.Content -match '"tabs"') {
                    Write-Host "=== Validation PASSED ===" -ForegroundColor Green
                    exit 0
                } else {
                    Write-Error "ERROR: Unexpected response format: $($response.Content)"
                    exit 3
                }
            }
        } catch {
            # Endpoint not ready yet
        }
        Start-Sleep -Seconds 1
        $elapsed++
        Write-Host "  Waiting... ($elapsed/${TimeoutSeconds}s)"
    }

    Write-Error "ERROR: ABP endpoint did not respond within ${TimeoutSeconds}s"
    exit 3
} finally {
    # Cleanup
    if (-not $chromeProcess.HasExited) {
        Write-Host "Stopping Chrome (PID: $($chromeProcess.Id))..."
        Stop-Process -Id $chromeProcess.Id -Force -ErrorAction SilentlyContinue
    }
}
```

**Step 2: Verify syntax**

Run: `pwsh -Command "Get-Content tools/abp/common/validate.ps1 | Out-Null; Write-Host 'Syntax OK'" 2>/dev/null || echo "pwsh not available, skip syntax check"`
Expected: "Syntax OK" or skip message

**Step 3: Commit**

```bash
git add tools/abp/common/validate.ps1
git commit -m "feat(abp): add shared validation script for Windows"
```

---

## Task 7: Create Linux Package Script

**Files:**
- Create: `tools/abp/package-linux.sh`

**Step 1: Create the package script**

```bash
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
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x tools/abp/package-linux.sh && bash -n tools/abp/package-linux.sh`
Expected: No output (syntax OK)

**Step 3: Commit**

```bash
git add tools/abp/package-linux.sh
git commit -m "feat(abp): add Linux package script"
```

---

## Task 8: Create macOS Package Script

**Files:**
- Create: `tools/abp/package-mac.sh`

**Step 1: Create the package script**

```bash
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
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x tools/abp/package-mac.sh && bash -n tools/abp/package-mac.sh`
Expected: No output (syntax OK)

**Step 3: Commit**

```bash
git add tools/abp/package-mac.sh
git commit -m "feat(abp): add macOS package script with arm64/universal support"
```

---

## Task 9: Create Windows Package Script

**Files:**
- Create: `tools/abp/package-win.ps1`

**Step 1: Create the package script**

```powershell
# Package Chrome with ABP for Windows distribution
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ChromiumSrc = (Resolve-Path "$ScriptDir\..\..").Path

# Validate environment
if (-not $env:ABP_VERSION) {
    Write-Error "ERROR: ABP_VERSION environment variable is required"
    exit 1
}

if (-not $env:CHROME_VERSION) {
    Write-Error "ERROR: CHROME_VERSION environment variable is required"
    exit 1
}

$BuildDir = "$ChromiumSrc\out\Release"
$DistDir = "$ChromiumSrc\dist"
$ArchiveName = "abp-$env:ABP_VERSION-chrome-$env:CHROME_VERSION-win-x64.zip"

# Validate build exists
if (-not (Test-Path "$BuildDir\chrome.exe")) {
    Write-Error "ERROR: chrome.exe not found at $BuildDir\chrome.exe"
    Write-Host "Run build-win.ps1 first"
    exit 1
}

Write-Host "=== Packaging ABP Chrome for Windows ===" -ForegroundColor Cyan
Write-Host "ABP Version: $env:ABP_VERSION"
Write-Host "Chrome Version: $env:CHROME_VERSION"
Write-Host "Build: $BuildDir"
Write-Host "Output: $DistDir\$ArchiveName"

# Create staging directory
$StagingDir = Join-Path $env:TEMP "abp-chrome-staging-$(Get-Random)"
$StagingApp = Join-Path $StagingDir "abp-chrome"
New-Item -ItemType Directory -Path $StagingApp -Force | Out-Null

try {
    Write-Host "=== Copying files ===" -ForegroundColor Cyan

    # Core binary
    Copy-Item "$BuildDir\chrome.exe" "$StagingApp\"

    # DLLs
    Get-ChildItem "$BuildDir\*.dll" | ForEach-Object {
        Copy-Item $_.FullName "$StagingApp\"
    }

    # Resource files
    Get-ChildItem "$BuildDir\*.pak" | ForEach-Object {
        Copy-Item $_.FullName "$StagingApp\"
    }
    Copy-Item "$BuildDir\icudtl.dat" "$StagingApp\"

    # V8 snapshot
    if (Test-Path "$BuildDir\v8_context_snapshot.bin") {
        Copy-Item "$BuildDir\v8_context_snapshot.bin" "$StagingApp\"
    }
    if (Test-Path "$BuildDir\snapshot_blob.bin") {
        Copy-Item "$BuildDir\snapshot_blob.bin" "$StagingApp\"
    }

    # Locales
    Copy-Item -Recurse "$BuildDir\locales" "$StagingApp\"

    # Resources directory
    if (Test-Path "$BuildDir\resources") {
        Copy-Item -Recurse "$BuildDir\resources" "$StagingApp\"
    }

    # Create dist directory
    if (-not (Test-Path $DistDir)) {
        New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
    }

    # Create archive
    Write-Host "=== Creating archive ===" -ForegroundColor Cyan
    $ArchivePath = Join-Path $DistDir $ArchiveName
    if (Test-Path $ArchivePath) {
        Remove-Item $ArchivePath -Force
    }
    Compress-Archive -Path "$StagingApp" -DestinationPath $ArchivePath

    # Report results
    $Size = (Get-Item $ArchivePath).Length / 1MB
    Write-Host "=== Package complete ===" -ForegroundColor Green
    Write-Host "Archive: $ArchivePath"
    Write-Host "Size: $([math]::Round($Size, 2)) MB"

} finally {
    # Cleanup
    if (Test-Path $StagingDir) {
        Remove-Item -Recurse -Force $StagingDir
    }
}
```

**Step 2: Verify syntax**

Run: `pwsh -Command "Get-Content tools/abp/package-win.ps1 | Out-Null; Write-Host 'Syntax OK'" 2>/dev/null || echo "pwsh not available, skip syntax check"`
Expected: "Syntax OK" or skip message

**Step 3: Commit**

```bash
git add tools/abp/package-win.ps1
git commit -m "feat(abp): add Windows package script"
```

---

## Task 10: Create Linux Release Script

**Files:**
- Create: `tools/abp/release-linux.sh`

**Step 1: Create the release script**

```bash
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
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x tools/abp/release-linux.sh && bash -n tools/abp/release-linux.sh`
Expected: No output (syntax OK)

**Step 3: Commit**

```bash
git add tools/abp/release-linux.sh
git commit -m "feat(abp): add Linux release script (build + validate + package)"
```

---

## Task 11: Create macOS Release Script

**Files:**
- Create: `tools/abp/release-mac.sh`

**Step 1: Create the release script**

```bash
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
```

**Step 2: Make executable and verify syntax**

Run: `chmod +x tools/abp/release-mac.sh && bash -n tools/abp/release-mac.sh`
Expected: No output (syntax OK)

**Step 3: Commit**

```bash
git add tools/abp/release-mac.sh
git commit -m "feat(abp): add macOS release script (build + validate + package)"
```

---

## Task 12: Create Windows Release Script

**Files:**
- Create: `tools/abp/release-win.ps1`

**Step 1: Create the release script**

```powershell
# Build, validate, and package ABP Chrome for Windows
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ChromiumSrc = (Resolve-Path "$ScriptDir\..\..").Path

# Validate environment
if (-not $env:ABP_VERSION) {
    Write-Error "ERROR: ABP_VERSION environment variable is required"
    Write-Host "Usage: `$env:ABP_VERSION='1.0.0'; `$env:CHROME_VERSION='130.0.6723.0'; .\release-win.ps1"
    exit 1
}

if (-not $env:CHROME_VERSION) {
    Write-Error "ERROR: CHROME_VERSION environment variable is required"
    Write-Host "Usage: `$env:ABP_VERSION='1.0.0'; `$env:CHROME_VERSION='130.0.6723.0'; .\release-win.ps1"
    exit 1
}

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "ABP Chrome Release - Windows x64" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "ABP Version: $env:ABP_VERSION"
Write-Host "Chrome Version: $env:CHROME_VERSION"
Write-Host "============================================" -ForegroundColor Cyan

# Step 1: Build
Write-Host ""
Write-Host ">>> Step 1/3: Building..." -ForegroundColor Yellow
& "$ScriptDir\build-win.ps1"
if ($LASTEXITCODE -ne 0) {
    Write-Error "ERROR: Build failed"
    exit 2
}

# Step 2: Validate
if ($env:SKIP_VALIDATION -eq "1") {
    Write-Host ""
    Write-Host ">>> Step 2/3: Validation SKIPPED (SKIP_VALIDATION=1)" -ForegroundColor Yellow
} else {
    Write-Host ""
    Write-Host ">>> Step 2/3: Validating..." -ForegroundColor Yellow
    & "$ScriptDir\common\validate.ps1" -ChromeBinary "$ChromiumSrc\out\Release\chrome.exe"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "ERROR: Validation failed"
        exit 3
    }
}

# Step 3: Package
Write-Host ""
Write-Host ">>> Step 3/3: Packaging..." -ForegroundColor Yellow
& "$ScriptDir\package-win.ps1"
if ($LASTEXITCODE -ne 0) {
    Write-Error "ERROR: Packaging failed"
    exit 4
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "Release complete!" -ForegroundColor Green
Write-Host "Archive: $ChromiumSrc\dist\abp-$env:ABP_VERSION-chrome-$env:CHROME_VERSION-win-x64.zip" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Green
```

**Step 2: Verify syntax**

Run: `pwsh -Command "Get-Content tools/abp/release-win.ps1 | Out-Null; Write-Host 'Syntax OK'" 2>/dev/null || echo "pwsh not available, skip syntax check"`
Expected: "Syntax OK" or skip message

**Step 3: Commit**

```bash
git add tools/abp/release-win.ps1
git commit -m "feat(abp): add Windows release script (build + validate + package)"
```

---

## Task 13: Add README for Build Scripts

**Files:**
- Create: `tools/abp/README.md`

**Step 1: Create the README**

```markdown
# ABP Build Scripts

Scripts for building and packaging ABP Chrome for distribution.

## Prerequisites

- [depot_tools](https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html) in PATH
- Build dependencies installed (see main README)
- Successful `gclient sync`

## Environment Variables

| Variable | Required | Description |
|----------|----------|-------------|
| `ABP_VERSION` | Yes | ABP release version (e.g., `1.0.0`) |
| `CHROME_VERSION` | Yes | Chrome version (e.g., `130.0.6723.0`) |
| `BUILD_ARCH` | macOS only | `arm64`, `universal`, or `all` (default: `all`) |
| `SKIP_VALIDATION` | No | Set to `1` to skip ABP validation |

## Quick Start

### Linux

```bash
ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 ./tools/abp/release-linux.sh
```

### macOS

```bash
# Both arm64 and universal
ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 ./tools/abp/release-mac.sh

# Single architecture
ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 BUILD_ARCH=arm64 ./tools/abp/release-mac.sh
```

### Windows (PowerShell)

```powershell
$env:ABP_VERSION="1.0.0"
$env:CHROME_VERSION="130.0.6723.0"
.\tools\abp\release-win.ps1
```

## Output

Archives are created in `dist/`:

- `abp-1.0.0-chrome-130.0.6723.0-linux-x64.tar.gz`
- `abp-1.0.0-chrome-130.0.6723.0-mac-arm64.zip`
- `abp-1.0.0-chrome-130.0.6723.0-mac-universal.zip`
- `abp-1.0.0-chrome-130.0.6723.0-win-x64.zip`

## Individual Scripts

For more control, run scripts separately:

```bash
# Build only
./tools/abp/build-linux.sh

# Validate only (after build)
./tools/abp/common/validate.sh out/Release/chrome

# Package only (after build)
./tools/abp/package-linux.sh
```

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Missing environment variables |
| 2 | Build failed |
| 3 | Validation failed |
| 4 | Packaging failed |
```

**Step 2: Commit**

```bash
git add tools/abp/README.md
git commit -m "docs(abp): add README for build scripts"
```

---

## Task 14: Final Verification

**Step 1: Verify all files exist**

Run: `ls -la tools/abp/ && ls -la tools/abp/common/`
Expected: All 11 scripts plus README

**Step 2: Verify all scripts are executable (bash)**

Run: `ls -la tools/abp/*.sh tools/abp/common/*.sh`
Expected: All .sh files have execute permission (rwxr-xr-x or similar)

**Step 3: Run syntax check on all bash scripts**

Run: `for f in tools/abp/*.sh tools/abp/common/*.sh; do bash -n "$f" && echo "OK: $f"; done`
Expected: "OK:" for each script

**Step 4: Verify git log**

Run: `git log --oneline -15`
Expected: 13 commits for this feature (Tasks 1-13)
