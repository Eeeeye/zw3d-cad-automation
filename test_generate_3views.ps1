<#
.SYNOPSIS
Test 3-view generation - for when ZW3D is already open manually

.DESCRIPTION
When ZW3D is open and plugin is loaded, run this script to:
1. Create a test block part
2. Generate 3-views with PMI auto dimensioning
#>

$ErrorActionPreference = "Continue"
$baseUrl = "http://localhost:8081"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  ZW3D 3-View Generation Test" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check server
Write-Host "Check HTTP server..." -ForegroundColor Yellow
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/status" -Method GET -TimeoutSec 3
    Write-Host "OK: HTTP server running" -ForegroundColor Green
} catch {
    Write-Host "ERROR: Cannot connect to HTTP server" -ForegroundColor Red
    Write-Host "Please ensure:" -ForegroundColor Yellow
    Write-Host "  1. ZW3D is started" -ForegroundColor Gray
    Write-Host "  2. ZW3D_HTTP_Server.dll plugin is loaded" -ForegroundColor Gray
    Write-Host "  3. Port 8081 is listening" -ForegroundColor Gray
    exit 1
}

Write-Host ""

# Create block
Write-Host "1. Create test block 100x60x40 mm..." -ForegroundColor Yellow
try {
    $body = @{
        length = 100
        width = 60
        height = 40
    } | ConvertTo-Json
    $response = Invoke-RestMethod -Uri "$baseUrl/create_block" -Method POST -Body $body -ContentType "application/json" -TimeoutSec 15
    if ($response.status -eq "ok") {
        Write-Host "OK: Block created successfully" -ForegroundColor Green
    } else {
        Write-Host "ERROR: Creation failed: $($response.message)" -ForegroundColor Red
        exit 1
    }
} catch {
    Write-Host "ERROR: Request failed: $_" -ForegroundColor Red
    exit 1
}

Write-Host ""

# Generate 3-views
Write-Host "2. Generate 3-views with auto dimensioning..." -ForegroundColor Yellow
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/generate_drafting" -Method POST -ContentType "application/json" -Body "{}" -TimeoutSec 120
    if ($response.status -eq "ok") {
        Write-Host "OK: 3-view generation successful!" -ForegroundColor Green
        Write-Host "   Output file: C:\Users\Ey\Documents\ZW3D\AutoDraftOutput.Z3" -ForegroundColor Gray
    } else {
        Write-Host "ERROR: Generation failed: $($response.message)" -ForegroundColor Red
        Write-Host "Check ZW3D message window for detailed error info" -ForegroundColor Yellow
        exit 1
    }
} catch {
    Write-Host "ERROR: Request failed: $_" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "TEST COMPLETE - Please open the output file in ZW3D to view." -ForegroundColor Green
Write-Host ""
exit 0
