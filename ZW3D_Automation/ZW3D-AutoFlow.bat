@echo off
cd /d "C:\Users\Ey\.openclaw\workspace"

powershell -Command ^
    "$host.UI.RawUI.WindowTitle = 'ZW3D 自动化流水线'; ^
    Write-Host \"===================================== \" -ForegroundColor Cyan; ^
    Write-Host \"🦊 ZW3D 自动化流水线启动\" -ForegroundColor Green; ^
    Write-Host \"===================================== \" -ForegroundColor Cyan; ^
    ^
    # 检查 ZW3D 是否运行
    $zw3d = Get-Process zw3d -ErrorAction SilentlyContinue; ^
    if (-not $zw3d) { ^
        Write-Host \"❌ ZW3D 未运行！请先手动启动 ZW3D。\" -ForegroundColor Red; ^
        pause; ^
        exit; ^
    } ^
    ^
    # 检查 HTTP Server 是否就绪
    Write-Host \"正在检查 HTTP Server 是否就绪...\" -ForegroundColor Yellow; ^
    try { ^
        $response = Invoke-WebRequest -Uri \"http://localhost:8081/status\" -Method Head -TimeoutSec 5 -ErrorAction Stop; ^
        Write-Host \"✅ ZW3D 与 HTTP Server 都已就绪，开始自动化流程...\" -ForegroundColor Green; ^
    } catch { ^
        Write-Host \"❌ HTTP Server 未就绪！请确认 ZW3D-HTTP-Server 已启动。\" -ForegroundColor Red; ^
        pause; ^
        exit; ^
    } ^
    ^
    # 执行 Python 脚本
    python auto_flow.py; ^
    ^
    Write-Host \"===================================== \" -ForegroundColor Cyan; ^
    Write-Host \"🎉 流程完成！请检查输出目录。\" -ForegroundColor Green; ^
    Write-Host \"===================================== \" -ForegroundColor Cyan; ^
    pause"