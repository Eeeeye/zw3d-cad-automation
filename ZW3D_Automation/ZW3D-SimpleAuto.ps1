# ZW3D-SimpleAuto.ps1 - Simple ZW3D automation

Write-Host "=== ZW3D Simple Automation ===" -ForegroundColor Cyan
Write-Host ""

# Check ZW3D
$zw3d = Get-Process zw3d -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $zw3d) {
    Write-Host "Starting ZW3D..." -ForegroundColor Yellow
    Start-Process "C:\Program Files\ZWSOFT\ZW3D 2026\zw3d.exe"
    Start-Sleep -Seconds 15
    $zw3d = Get-Process zw3d -ErrorAction SilentlyContinue | Select-Object -First 1
}

if (-not $zw3d) {
    Write-Host "Failed to start ZW3D!" -ForegroundColor Red
    exit 1
}

Write-Host "ZW3D is running (PID: $($zw3d.Id))" -ForegroundColor Green
Write-Host ""

# Show available commands
Write-Host "Available Commands:" -ForegroundColor Yellow
Write-Host "  1. ~HelloZW3D" -ForegroundColor White
Write-Host "  2. ~ZW3DCreateComplexShape" -ForegroundColor White
Write-Host "  3. ~ZW3DCreateGearShape" -ForegroundColor White
Write-Host "  4. ~ZW3DCreateAssembly" -ForegroundColor White
Write-Host "  5. ~ZW3DAutoAnnotatePart" -ForegroundColor White
Write-Host "  6. ~ZW3DAddNote" -ForegroundColor White
Write-Host ""

# Get user choice
$choice = Read-Host "Enter command number (1-6) or type 'quit' to exit"

# Map choice to command
$command = switch ($choice) {
    "1" { "~HelloZW3D" }
    "2" { "~ZW3DCreateComplexShape" }
    "3" { "~ZW3DCreateGearShape" }
    "4" { "~ZW3DCreateAssembly" }
    "5" { "~ZW3DAutoAnnotatePart" }
    "6" { "~ZW3DAddNote" }
    "quit" { $null }
    default { "~HelloZW3D" }
}

if ($command) {
    Write-Host ""
    Write-Host "Executing: $command" -ForegroundColor Green
    
    # Activate window and send command
    $wshell = New-Object -ComObject wscript.shell
    $wshell.AppActivate($zw3d.Id)
    Start-Sleep -Milliseconds 500
    
    # Send F2 and command
    $wshell.SendKeys("{F2}")
    Start-Sleep -Milliseconds 300
    $wshell.SendKeys("^a")
    Start-Sleep -Milliseconds 100
    $wshell.SendKeys($command)
    Start-Sleep -Milliseconds 200
    $wshell.SendKeys("{ENTER}")
    
    Write-Host ""
    Write-Host "Command sent to ZW3D!" -ForegroundColor Green
    Write-Host "Check ZW3D message window for results." -ForegroundColor Yellow
} else {
    Write-Host "Exiting..." -ForegroundColor Gray
}

Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Cyan
