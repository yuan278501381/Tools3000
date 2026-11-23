<#
.SYNOPSIS
一键安装轻量级 C++ 纯命令行编译环境 (Visual Studio Build Tools)
.DESCRIPTION
无需安装庞大臃肿的完整版 Visual Studio IDE，通过 Winget 自动为您在后台静默安装微软官方的 C++ 纯命令行编译工具链（含 MSVC 编译器、Windows SDK 和 CMake）。
安装完成后，您可以在本地极速运行 deploy.ps1 进行编译。
#>

Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "    开始一键安装轻量级 C++ 命令行编译环境 (Build Tools)" -ForegroundColor Cyan
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host ""

# 检查是否安装了 winget
if (-not (Get-Command "winget" -ErrorAction SilentlyContinue)) {
    Write-Host "[错误] 您的系统未安装 Winget 包管理器，请先在 Microsoft Store 中更新 '应用安装程序'。" -ForegroundColor Red
    exit 1
}

Write-Host "[1/2] 正在请求管理员权限并启动静默安装..." -ForegroundColor Yellow
Write-Host "      (安装包约需下载数 GB 的 Windows SDK 和 MSVC，请耐心等待，过程完全静默)" -ForegroundColor Gray

# 使用 Winget 安装 VS Build Tools 2022
# 参数说明：
# --silent: 静默安装，不弹界面
# --accept-package-agreements: 自动接受许可协议
# --override: 传递给底层 vs_setup.exe 的安装参数
#   --passive: 显示进度条但不需要用户干预
#   --wait: 等待安装完成
#   --add: 添加 C++ 桌面开发工作负载
#   --includeRecommended: 包含推荐组件（如 CMake, Windows SDK）
$wingetArgs = "install Microsoft.VisualStudio.2022.BuildTools --silent --accept-package-agreements --accept-source-agreements --override `"--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`""

try {
    # 必须以管理员权限运行
    Start-Process -FilePath "winget.exe" -ArgumentList $wingetArgs -Verb RunAs -Wait
    
    Write-Host "`n[2/2] ✅ 安装完成！" -ForegroundColor Green
    Write-Host "---------------------------------------------------------"
    Write-Host "现在您可以直接在 PowerShell 中执行本地部署脚本了：" -ForegroundColor Cyan
    Write-Host ".\deploy.ps1" -ForegroundColor Yellow
    Write-Host "---------------------------------------------------------"
} catch {
    Write-Host "`n[!] 安装被取消或发生异常：" $_.Exception.Message -ForegroundColor Red
}

Write-Host "`n按任意键退出..."
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
