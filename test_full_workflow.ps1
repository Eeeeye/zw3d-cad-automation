<#
.SYNOPSIS
Full test ZW3D HTTP plugin - Start ZW3D -> Create part -> Generate 3-views

.DESCRIPTION
This script will:
1. Check if ZW3D is running, start if not
2. Wait for ZW3D to load plugin and HTTP server
3. Create a test block via HTTP API
4. Call generate_drafting to create 3-views with auto dimensioning
#>

$ErrorActionPreference = "Continue"
$baseUrl = "http://localhost:8081"
$zw3dPath = "${env:ProgramFiles}\ZWSOFT\ZW3D 2026\ZW3D.exe"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  ZW3D HTTP Plugin - Full Workflow Test" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Check if HTTP server is already running
Write-Host "Step 1: Check HTTP server status..." -ForegroundColor Yellow
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/status" -Method GET -TimeoutSec 3
    Write-Host "OK: HTTP server running: $($response.status) - $($response.message)" -ForegroundColor Green
    $serverRunning = $true
} catch {
    Write-Host "WARNING: HTTP server not responding, need to start ZW3D" -ForegroundColor Yellow
    $serverRunning = $false
}

Write-Host ""

# Step 2: Start ZW3D if not running
if (-not $serverRunning) {
    Write-Host "Step 2: Start ZW3D..." -ForegroundColor Yellow
    if (Test-Path $zw3dPath) {
        Start-Process -FilePath $zw3dPath
        Write-Host "OK: ZW3D started, waiting 30 seconds for loading..." -ForegroundColor Yellow
        Start-Sleep -Seconds 30
    } else {
        Write-Host "ERROR: ZW3D not found: $zw3dPath" -ForegroundColor Red
        Write-Host "Please start ZW3D manually and re-run this script" -ForegroundColor Red
        exit 1
    }
}

# Step 3: Wait for server to be ready
Write-Host ""
Write-Host "Step 3: Waiting for HTTP server to be ready..." -ForegroundColor Yellow
$maxAttempts = 10
$attempt = 0
$ready = $false
while ($attempt -lt $maxAttempts -and -not $ready) {
    $attempt++
    try {
        $response = Invoke-RestMethod -Uri "$baseUrl/status" -Method GET -TimeoutSec 2
        if ($response.status -eq "ok") {
            Write-Host "OK: HTTP server ready (attempt $attempt/$maxAttempts)" -ForegroundColor Green
            $ready = $true
        }
    } catch {
        Write-Host "  Waiting... ($attempt/$maxAttempts)" -ForegroundColor Gray
        Start-Sleep -Seconds 5
    }
}

if (-not $ready) {
    Write-Host "ERROR: HTTP server not ready, please check:" -ForegroundColor Red
    Write-Host "   1. Is ZW3D started successfully?" -ForegroundColor Gray
    Write-Host "   2. Is plugin loaded? Check ZW3D message window for 'HTTP Server listening on port 8081'" -ForegroundColor Gray
    Write-Host "   3. Is port 8081 already used?" -ForegroundColor Gray
    exit 1
}

Write-Host ""

# Step 4: Create a new part with a test block
Write-Host "Step 4: Create new part with test block (100x50x30mm)..." -ForegroundColor Yellow
try {
    $body = @{
        length = 100
        width = 50
        height = 30
    } | ConvertTo-Json
    $response = Invoke-RestMethod -Uri "$baseUrl/create_block" -Method POST -Body $body -ContentType "application/json" -TimeoutSec 10
    if ($response.status -eq "ok") {
        Write-Host "OK: Block created: $($response.message)" -ForegroundColor Green
        Write-Host "   Dimensions: $($response.dimensions.length) x $($response.dimensions.width) x $($response.dimensions.height) mm" -ForegroundColor Gray
    } else {
        Write-Host "ERROR: Block creation failed: $($response.message)" -ForegroundColor Red
        Write-Host "Check ZW3D message window for details" -ForegroundColor Yellow
        exit 1
    }
} catch {
    Write-Host "ERROR: Request failed: $_" -ForegroundColor Red
    exit 1
}

Write-Host ""

# Step 5: Generate 3-view drafting with PMI dimensions
Write-Host "Step 5: Generate 3-views with auto dimensioning..." -ForegroundColor Yellow
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/generate_drafting" -Method POST -ContentType "application/json" -Body "{}" -TimeoutSec 60
    if ($response.status -eq "ok") {
        Write-Host "OK: 3-view drafting generated: $($response.message)" -ForegroundColor Green
        Write-Host "   Output file: C:\Users\Ey\Documents\ZW3D\AutoDraftOutput.Z3" -ForegroundColor Gray
    } else {
        Write-Host "ERROR: 3-view generation failed: $($response.message)" -ForegroundColor Red
        Write-Host "Check ZW3D message window for details" -ForegroundColor Yellow
        exit 1
    }
} catch {
    Write-Host "ERROR: Request failed or timeout: $_" -ForegroundColor Red
    Write-Host "Drafting generation may take longer, check ZW3D message window" -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "          TEST COMPLETED" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Output location:" -ForegroundColor White
Write-Host "  - Part file: C:\Users\Ey\Documents\ZW3D\AutoDraftPart.Z3PRT" -ForegroundColor Gray
Write-Host "  - Drawing file: C:\Users\Ey\Documents\ZW3D\AutoDraftOutput.Z3" -ForegroundColor Gray
Write-Host ""
Write-Host "Please open the output file in ZW3D to view the 3-views!" -ForegroundColor Yellow
Write-Host ""

exit 0
