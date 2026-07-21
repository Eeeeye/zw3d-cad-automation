# Direct-ZW3D-Execute.ps1 - Directly execute commands in ZW3D

param(
    [Parameter(Mandatory=$false)]
    [ValidateSet("Hello", "ComplexShape", "Gear", "Assembly", "Annotate", "Note")]
    [string]$Action = "Hello"
)

# Map actions to commands
$commandMap = @{
    "Hello" = "~HelloZW3D"
    "ComplexShape" = "~ZW3DCreateComplexShape"
    "Gear" = "~ZW3DCreateGearShape"
    "Assembly" = "~ZW3DCreateAssembly"
    "Annotate" = "~ZW3DAutoAnnotatePart"
    "Note" = "~ZW3DAddNote"
}

$command = $commandMap[$Action]

Write-Host "=== Direct ZW3D Command Execution ===" -ForegroundColor Cyan
Write-Host "Action: $Action" -ForegroundColor Yellow
Write-Host "Command: $command" -ForegroundColor Yellow
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

Write-Host "ZW3D PID: $($zw3d.Id)" -ForegroundColor Green

# Activate window
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinAPI {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    public const int SW_RESTORE = 9;
}
"@

$hwnd = $zw3d.MainWindowHandle
[WinAPI]::ShowWindow($hwnd, [WinAPI]::SW_RESTORE) | Out-Null
[WinAPI]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 500

Write-Host "Window activated!" -ForegroundColor Green
Write-Host ""

# Use WScript.Shell to send keys
$wshell = New-Object -ComObject wscript.shell

# Activate command line with F2
Write-Host "Activating command line..." -ForegroundColor Gray
$wshell.SendKeys("{F2}")
Start-Sleep -Milliseconds 300

# Clear and type command
Write-Host "Sending command..." -ForegroundColor Gray
$wshell.SendKeys("^a")  # Select all
Start-Sleep -Milliseconds 100
$wshell.SendKeys($command)
Start-Sleep -Milliseconds 200

# Execute
Write-Host "Executing..." -ForegroundColor Gray
$wshell.SendKeys("{ENTER}")

Write-Host ""
Write-Host "Command sent successfully!" -ForegroundColor Green
Write-Host "Check ZW3D for results." -ForegroundColor Yellow
Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Cyan
