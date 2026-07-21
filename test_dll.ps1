# Test DLL endpoints
$baseUrl = "http://localhost:8081"

Write-Host "=== Testing ZW3D HTTP Server DLL ===" -ForegroundColor Cyan
Write-Host ""

# Test 1: Status
Write-Host "Test 1: GET /status" -ForegroundColor Yellow
try {
    $response = Invoke-RestMethod -Uri "$baseUrl/status" -Method GET
    Write-Host "Result: $response" -ForegroundColor Green
} catch {
    Write-Host "Failed: $_" -ForegroundColor Red
}
Write-Host ""

# Test 2: Create Block
Write-Host "Test 2: POST /create_block" -ForegroundColor Yellow
try {
    $body = @{
        length = 50
        width = 30
        height = 20
    } | ConvertTo-Json
    $response = Invoke-RestMethod -Uri "$baseUrl/create_block" -Method POST -Body $body -ContentType "application/json"
    Write-Host "Result: $($response | ConvertTo-Json -Compress)" -ForegroundColor Green
} catch {
    Write-Host "Failed: $_" -ForegroundColor Red
}
Write-Host ""

# Test 3: Create Gear
Write-Host "Test 3: POST /create_gear" -ForegroundColor Yellow
try {
    $body = @{
        teeth = 20
        radius = 40
        thickness = 10
    } | ConvertTo-Json
    $response = Invoke-RestMethod -Uri "$baseUrl/create_gear" -Method POST -Body $body -ContentType "application/json"
    Write-Host "Result: $($response | ConvertTo-Json -Compress)" -ForegroundColor Green
} catch {
    Write-Host "Failed: $_" -ForegroundColor Red
}
Write-Host ""

# Test 4: Create Complex
Write-Host "Test 4: POST /create_complex" -ForegroundColor Yellow
try {
    $body = @{} | ConvertTo-Json
    $response = Invoke-RestMethod -Uri "$baseUrl/create_complex" -Method POST -Body $body -ContentType "application/json"
    Write-Host "Result: $($response | ConvertTo-Json -Compress)" -ForegroundColor Green
} catch {
    Write-Host "Failed: $_" -ForegroundColor Red
}
Write-Host ""

# Test 5: Create Create Assembly
Write-Host "Test 5: POST /create_assembly" -ForegroundColor Yellow
try {
    $body = @{} | ConvertTo-Json
    $response = Invoke-RestMethod -Uri "$baseUrl/create_assembly" -Method POST -Body $body -ContentType "application/json"
    Write-Host "Result: $($response | ConvertTo-Json -Compress)" -ForegroundColor Green
} catch {
    Write-Host "Failed: $_" -ForegroundColor Red
}
Write-Host ""

# Test 6: Execute Command
Write-Host "Test 6: POST /execute" -ForegroundColor Yellow
try {
    $body = @{ command = "CvxPrtCreate" } | ConvertTo-Json
    $response = Invoke-RestMethod -Uri "$baseUrl/execute" -Method POST -Body $body -ContentType "application/json"
    Write-Host "Result: $($response | ConvertTo-Json -Compress)" -ForegroundColor Green
} catch {
    Write-Host "Failed: $_" -ForegroundColor Red
}
Write-Host ""

Write-Host "=== Done ===" -ForegroundColor Cyan
