@echo off
echo Building ZW3D_HTTP_Server.dll...

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo VS2022 Community not found, trying Enterprise...
    call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
    if errorlevel 1 (
        echo VS2022 Enterprise not found, trying Professional...
        call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
        if errorlevel 1 (
            echo ERROR: Visual Studio 2022 not found!
            exit /b 1
        )
    )
)

cl.exe /LD /MD /EHsc ^
    /I"C:\Program Files\ZWSOFT\ZW3D 2026\api\inc" ^
    /I"C:\Users\Ey\Desktop\claude\ZW3D_Bridge\include" ^
    src\ZW3D_HTTP_Server.cpp ^
    /link ^
    /DEF:src\ZW3D_HTTP_Server.def ^
    /OUT:bin\ZW3D_HTTP_Server.dll ^
    "C:\Program Files\ZWSOFT\ZW3D 2026\ZW3D.lib" ^
    ws2_32.lib

if errorlevel 1 (
    echo Compilation FAILED!
    exit /b 1
)

echo.
echo Compilation SUCCESS!
echo Output: bin\ZW3D_HTTP_Server.dll
echo.
echo Installing to ZW3D apilibs folder...
copy /Y "bin\ZW3D_HTTP_Server.dll" "%APPDATA%\ZWSOFT\ZW3D\ZW3D2026\custom\apilibs\"

echo.
echo Done! Please restart ZW3D to reload the plugin.
pause
