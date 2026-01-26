# Build Scripts for Portable Binaries

**Date:** 2026-01-25
**Status:** Approved

## Overview

Create build scripts for Linux, Windows, and macOS that produce portable binaries for GitHub releases. Target audience is developers, so simple zip/tarball distribution is preferred over platform installers.

## Directory Structure

```
tools/abp/
├── build-linux.sh       # Build on Linux
├── build-mac.sh         # Build on macOS
├── build-win.ps1        # Build on Windows (PowerShell)
├── package-linux.sh     # Package Linux build
├── package-mac.sh       # Package macOS build
├── package-win.ps1      # Package Windows build
├── release-linux.sh     # Build + validate + package (Linux)
├── release-mac.sh       # Build + validate + package (macOS)
├── release-win.ps1      # Build + validate + package (Windows)
└── common/
    ├── validate.sh      # Shared validation logic (Linux/macOS)
    └── validate.ps1     # Windows validation
```

## Environment Variables

| Variable | Required | Description |
|----------|----------|-------------|
| `ABP_VERSION` | Yes | ABP release version (e.g., `1.0.0`) |
| `CHROME_VERSION` | Yes | Chrome version (e.g., `130.0.6723.0`) |
| `BUILD_ARCH` | macOS only | `arm64` or `universal` (default: both) |
| `SKIP_VALIDATION` | No | Set to `1` to skip validation |

## Output

Archives are placed in `dist/` (added to `.gitignore`).

**Naming convention:** `abp-<abp_version>-chrome-<chrome_version>-<platform>-<arch>.<ext>`

**Examples:**
- `abp-1.0.0-chrome-130.0.6723.0-linux-x64.tar.gz`
- `abp-1.0.0-chrome-130.0.6723.0-mac-arm64.zip`
- `abp-1.0.0-chrome-130.0.6723.0-mac-universal.zip`
- `abp-1.0.0-chrome-130.0.6723.0-win-x64.zip`

## Build Scripts

### GN Args (Release Build)

```
is_debug = false
is_component_build = false
symbol_level = 0
is_official_build = true
chrome_pgo_phase = 0
```

### Build Script Flow

1. Validate environment variables are set (`ABP_VERSION`, `CHROME_VERSION`)
2. Validate we're in the chromium source directory
3. Run `gn gen out/Release --args='...'`
4. Run `autoninja -C out/Release chrome`
5. Exit with build status code

### Platform Differences

| Platform | Shell | Build target | Notes |
|----------|-------|--------------|-------|
| Linux | Bash | `chrome` | Standard build |
| macOS | Bash | `chrome` | Builds arm64 and universal separately |
| Windows | PowerShell | `chrome` | Uses `cmd /c` for depot_tools compatibility |

### macOS Dual Builds

The `build-mac.sh` script accepts `BUILD_ARCH` environment variable:

| `BUILD_ARCH` | GN args addition | Output dir |
|--------------|------------------|------------|
| `arm64` | `target_cpu = "arm64"` | `out/Release-arm64` |
| `universal` | Universal binary build | `out/Release-universal` |

`release-mac.sh` builds both architectures by default.

## Package Scripts

### Contents by Platform

| Platform | Contents | Archive format |
|----------|----------|----------------|
| Linux | `chrome`, `chrome_sandbox`, `*.so`, `*.pak`, `locales/`, `resources/`, `icudtl.dat`, `v8_context_snapshot.bin` | `.tar.gz` |
| macOS | `Chromium.app` bundle (complete) | `.zip` |
| Windows | `chrome.exe`, `*.dll`, `*.pak`, `locales/`, `resources/`, `icudtl.dat`, `v8_context_snapshot.bin` | `.zip` |

### Package Script Flow

1. Validate environment variables (`ABP_VERSION`, `CHROME_VERSION`)
2. Validate build output exists (e.g., `out/Release/chrome`)
3. Create temp staging directory
4. Copy required files (platform-specific list)
5. Create archive with naming convention
6. Output archive to `dist/` directory
7. Print archive path and size

## Validation

Before packaging, the release scripts validate that ABP is functional.

### Validation Flow

1. Start Chrome with `--enable-abp --headless=new --no-sandbox`
2. Wait up to 10 seconds for startup
3. Curl `http://localhost:8222/api/v1/tabs` - expect 200 response
4. Kill Chrome process
5. Return success/failure

### Failure Behavior

- If validation fails, script exits with error code
- No archive is created
- Error message indicates what failed (startup timeout, endpoint unreachable, bad response)

### Skip Validation

Set `SKIP_VALIDATION=1` to bypass (useful for debugging packaging issues).

## Release Scripts (Combined)

### Usage Examples

```bash
# Linux
ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 ./tools/abp/release-linux.sh

# macOS (both architectures)
ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 ./tools/abp/release-mac.sh

# macOS (single architecture)
ABP_VERSION=1.0.0 CHROME_VERSION=130.0.6723.0 BUILD_ARCH=arm64 ./tools/abp/release-mac.sh

# Windows (PowerShell)
$env:ABP_VERSION="1.0.0"; $env:CHROME_VERSION="130.0.6723.0"; .\tools\abp\release-win.ps1
```

### Release Script Flow

1. Validate required env vars or exit with usage message
2. Call build script → exit on failure
3. Call validate script → exit on failure
4. Call package script → exit on failure
5. Print success message with archive path

### Exit Codes

- `0` - Success
- `1` - Missing environment variables
- `2` - Build failed
- `3` - Validation failed
- `4` - Packaging failed
