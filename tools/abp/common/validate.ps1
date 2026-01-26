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
