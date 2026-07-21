@echo off
chcp 65001 >nul
echo === ZW3D Auto Command ===
echo.
echo Select a command to execute in ZW3D:
echo.
echo [1] Hello Test (~HelloZW3D)
echo [2] Create Complex Shape (~ZW3DCreateComplexShape)
echo [3] Create Gear (~ZW3DCreateGearShape)
echo [4] Create Assembly (~ZW3DCreateAssembly)
echo [5] Auto Annotate (~ZW3DAutoAnnotatePart)
echo [6] Add Note (~ZW3DAddNote)
echo.
echo [0] Exit
echo.

set /p choice="Enter number (0-6): "

if "%choice%"=="1" set CMD=~HelloZW3D
if "%choice%"=="2" set CMD=~ZW3DCreateComplexShape
if "%choice%"=="3" set CMD=~ZW3DCreateGearShape
if "%choice%"=="4" set CMD=~ZW3DCreateAssembly
if "%choice%"=="5" set CMD=~ZW3DAutoAnnotatePart
if "%choice%"=="6" set CMD=~ZW3DAddNote
if "%choice%"=="0" goto :exit

if not defined CMD (
    echo Invalid choice!
    pause
    exit /b 1
)

echo.
echo Preparing to execute: %CMD%
echo.
echo IMPORTANT: Make sure ZW3D is running and visible!
echo.
echo The script will:
echo   1. Activate ZW3D window
echo   2. Wait 2 seconds
echo   3. Send F2 to activate command line
echo   4. Type the command
echo   5. Press Enter
echo.
pause

echo.
echo Executing command...
echo.

powershell -ExecutionPolicy Bypass -Command "& {Add-Type -AssemblyName System.Windows.Forms; $zw3d = Get-Process zw3d -ErrorAction SilentlyContinue | Select-Object -First 1; if (-not $zw3d) { Write-Host 'ERROR: ZW3D not running!' -ForegroundColor Red; exit 1; }; Write-Host ('ZW3D PID: ' + $zw3d.Id) -ForegroundColor Green; [System.Windows.Forms.SendKeys]::SendWait('%%'); Start-Sleep -Milliseconds 500; $wshell = New-Object -ComObject wscript.shell; $result = $wshell.AppActivate($zw3d.Id); Write-Host ('Window activation: ' + $result) -ForegroundColor Yellow; if ($result) { Start-Sleep -Seconds 2; $wshell.SendKeys('{F2}'); Start-Sleep -Milliseconds 500; $wshell.SendKeys('%CMD%'); Start-Sleep -Milliseconds 300; $wshell.SendKeys('{ENTER}'); Write-Host 'Command sent successfully!' -ForegroundColor Green; } else { Write-Host 'Failed to activate window!' -ForegroundColor Red; } }"

echo.
echo.
echo Command execution attempted.
echo Check ZW3D message window for results.
echo.
pause

:exit
echo Goodbye!
