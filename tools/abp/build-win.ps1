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
