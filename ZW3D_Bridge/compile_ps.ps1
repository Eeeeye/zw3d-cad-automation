# Compile ZW3D HTTP Server DLL
Write-Host "Compiling ZW3D_HTTP_Server.dll..." -ForegroundColor Cyan

# Find Visual Studio
$vsPaths = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
)

$vsPath = $null
foreach ($path in $vsPaths) {
    if (Test-Path $path) {
        $vsPath = $path
        Write-Host "Found Visual Studio at: $path" -ForegroundColor Green
        break
    }
}

if (-not $vsPath) {
    Write-Host "ERROR: Visual Studio not found!" -ForegroundColor Red
    exit 1
}

# Build command
$buildCmd = @"
@echo off
call "$vsPath" >nul 2>&1
cl.exe /LD /MD /EHsc /I"C:\Program Files\ZWSOFT\ZW3D 2026\api\inc" src\ZW3D_HTTP_Server.cpp /link /DEF:src\ZW3D_HTTP_Server.def /OUT:bin\ZW3D_HTTP_Server.dll "C:\Program Files\ZWSOFT\ZW3D 2026\ZW3D.lib" ws2_32.lib
if errorlevel 1 exit /b %errorlevel%
"@

# Execute build
$buildCmd | cmd.exe

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nCompilation SUCCESS!" -ForegroundColor Green
    Write-Host "Output: bin\ZW3D_HTTP_Server.dll" -ForegroundColor Cyan

    # Copy to ZW3D apilibs
    $targetDir = "$env:APPDATA\ZWSOFT\ZW3D\ZW3D2026\custom\apilibs"
    if (Test-Path $targetDir) {
        Copy-Item -Path "bin\ZW3D_HTTP_Server.dll" -Destination $targetDir -Force
        Write-Host "`nInstalled to: $targetDir" -ForegroundColor Green
    } else {
        Write-Host "`nWarning: Target directory not found: $targetDir" -ForegroundColor Yellow
    }
} else {
    Write-Host "`nCompilation FAILED!" -ForegroundColor Red
    exit 1
}
