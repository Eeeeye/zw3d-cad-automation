# Auto-Execute-ZW3DCommands.ps1 - Automatically execute commands in ZW3D
# This script sends commands directly to ZW3D without manual typing

param(
    [string]$Command = "~HelloZW3D",
    [int]$DelaySeconds = 3
)

Write-Host "=== ZW3D Auto Command Execution ===" -ForegroundColor Cyan
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

# Activate ZW3D window
$wshell = New-Object -ComObject wscript.shell
$result = $wshell.AppActivate($zw3d.Id)

if ($result) {
    Write-Host "ZW3D window activated!" -ForegroundColor Green
    Write-Host ""
    
    # Wait for user to see the window
    Write-Host "Preparing to execute command in $DelaySeconds seconds..." -ForegroundColor Yellow
    for ($i = $DelaySeconds; $i -gt 0; $i--) {
        Write-Host "  $i..." -NoNewline
        Start-Sleep -Seconds 1
    }
    Write-Host ""
    Write-Host ""
    
    # Method 1: Try to use F2 to activate command line
    Write-Host "Step 1: Activating command line (F2)..." -ForegroundColor Gray
    [System.Windows.Forms.SendKeys]::SendWait("{F2}")
    Start-Sleep -Milliseconds 500
    
    # Method 2: Send Ctrl+2 to ensure bottom panel is active
    Write-Host "Step 2: Ensuring command panel is active (Ctrl+2)..." -ForegroundColor Gray
    [System.Windows.Forms.SendKeys]::SendWait("^2")
    Start-Sleep -Milliseconds 500
    
    # Clear any existing text and type the command
    Write-Host "Step 3: Sending command..." -ForegroundColor Gray
    [System.Windows.Forms.SendKeys]::SendWait("{HOME}{SHIFT}+{END}{DELETE}")  # Clear line
    Start-Sleep -Milliseconds 200
    
    # Type the command character by character
    Write-Host "Typing: $Command" -ForegroundColor Cyan
    foreach ($char in $Command.ToCharArray()) {
        [System.Windows.Forms.SendKeys]::SendWait($char)
        Start-Sleep -Milliseconds 50
    }
    
    Start-Sleep -Milliseconds 300
    
    # Press Enter to execute
    Write-Host "Step 4: Executing command (Enter)..." -ForegroundColor Gray
    [System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
    
    Write-Host ""
    Write-Host "Command executed!" -ForegroundColor Green
    Write-Host "Check ZW3D message window for results." -ForegroundColor Yellow
    
} else {
    Write-Host "Failed to activate ZW3D window!" -ForegroundColor Red
}

Write-Host ""
Write-Host "=== Execution Complete ===" -ForegroundColor Cyan
