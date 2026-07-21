$ErrorActionPreference = "Continue"

$vsPath = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$msvcPath = "$vsPath\VC\Tools\MSVC\14.38.33130"
$cl = "$msvcPath\bin\Hostx64\x64\cl.exe"

$env:INCLUDE = "$msvcPath\include;C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\ucrt;C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\shared;C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\um;C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\winrt"
$env:LIB = "C:\Program Files\ZWSOFT\ZW3D 2026;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\um\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\ucrt\x64;$msvcPath\lib\x64"
$env:PATH = "$msvcPath\bin\Hostx64\x64;$vsPath\Common7\IDE;$env:PATH"

$src = "C:\Users\Ey\Desktop\claude\ZW3D_Bridge\src\ZW3D_HTTP_Server.cpp"
$def = "C:\Users\Ey\Desktop\claude\ZW3D_Bridge\src\ZW3D_HTTP_Server.def"
$out = "C:\Users\Ey\Desktop\claude\ZW3D_Bridge\bin\ZW3D_HTTP_Server.dll"
$outNew = "C:\Users\Ey\Desktop\claude\ZW3D_Bridge\bin\ZW3D_HTTP_Server_v2.dll"
$zw3d = "C:\Program Files\ZWSOFT\ZW3D 2026\ZW3D.lib"
$inc1 = "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc"
$inc2 = "C:\Users\Ey\Desktop\claude\ZW3D_Bridge\include"

Write-Host "INCLUDE=$env:INCLUDE"
Write-Host "Compiling..."
& $cl /LD /MD /EHsc /I"$inc1" /I"$inc2" $src /link /DEF:$def /OUT:$outNew $zw3d ws2_32.lib 2>&1

if ($LASTEXITCODE -eq 0) {
    Write-Host "BUILD SUCCESS: $outNew"
    # Also copy to bin as the main DLL (in case linker can't overwrite)
    Copy-Item $outNew $out -Force 2>$null
    Write-Host "Output: $out"
    # Try to copy to ZW3D apilibs (may fail if ZW3D has it locked)
    $apilibsDll = "$env:APPDATA\ZWSOFT\ZW3D\ZW3D2026\custom\apilibs\ZW3D_HTTP_Server.dll"
    try {
        Copy-Item $outNew $apilibsDll -Force -ErrorAction Stop
        Write-Host "Installed to ZW3D apilibs"
    } catch {
        Write-Host "Note: Could not overwrite locked DLL in apilibs. Close ZW3D and copy manually."
    }
} else {
    Write-Host "BUILD FAILED (exit $LASTEXITCODE)"
}
