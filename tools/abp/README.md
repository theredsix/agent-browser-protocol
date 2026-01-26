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
