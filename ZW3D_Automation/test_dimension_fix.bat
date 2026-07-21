@echo off
echo ========================================
echo ZW3D Auto Dimension Fix Test
echo ========================================
echo.
echo This test will:
echo 1. Create a test block (80 x 60 x 40)
echo 2. Generate drawing with 3 views
echo 3. Add auto dimensions (now with FIXED values)
echo.
echo Prerequisites:
echo - ZW3D must be running
echo - ZW3D_HTTP_Server plugin must be loaded
echo - Check message window for: "HTTP Server listening on port 8081"
echo.
pause

python test_dimension_fix.py

pause
