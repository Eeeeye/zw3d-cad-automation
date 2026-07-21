@echo off
REM ZW3D PMI Feature Test
REM This script tests the complete workflow with PMI annotations

echo ============================================================
echo   ZW3D PMI Feature - Complete Workflow Test
echo ============================================================
echo.
echo This test will:
echo   1. Start ZW3D (auto-loads DLL)
echo   2. Create a block object
echo   3. Generate 3 orthographic views
echo   4. Add PMI dimensions
echo.
echo Press Ctrl+C to abort
echo.
pause

echo.
echo Starting test...
echo.

python test_pmi_complete.py

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ============================================================
    echo   TEST PASSED!
    echo ============================================================
) else (
    echo.
    echo ============================================================
    echo   TEST FAILED!
    echo ============================================================
)

echo.
pause
