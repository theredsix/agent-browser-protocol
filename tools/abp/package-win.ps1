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
