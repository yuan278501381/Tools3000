param(
    [switch]$NoRun
)

$ErrorActionPreference = "Stop"
$sw = [System.Diagnostics.Stopwatch]::StartNew()

Write-Host "⚡ [Tools3000 Rapid Dev] 正在准备极速增量编译..." -ForegroundColor Cyan

# 寻找 CMake 路径
$cmakeExe = "cmake.exe"
if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue)) {
    $candidates = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    foreach ($cand in $candidates) {
        if (Test-Path $cand) {
            $cmakeExe = $cand
            break
        }
    }
}

# 1. 杀掉运行中的 Tools3000 并同步等待句柄完全释放
$running = Get-Process -Name "Tools3000*", "Tools3000*" -ErrorAction SilentlyContinue
if ($running) {
    $running | Stop-Process -Force -ErrorAction SilentlyContinue
    $stopTimeoutMs = 3000
    $waitSw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($waitSw.ElapsedMilliseconds -lt $stopTimeoutMs) {
        $remaining = Get-Process -Name "Tools3000*", "Tools3000*" -ErrorAction SilentlyContinue
        if (-not $remaining) { break }
        Start-Sleep -Milliseconds 50
    }
    $remaining = Get-Process -Name "Tools3000*", "Tools3000*" -ErrorAction SilentlyContinue
    if ($remaining) {
        Write-Host "❌ 无法终止旧的 Tools3000 进程 ($($remaining.Name))，文件句柄仍被占用！" -ForegroundColor Red
        exit 1
    }
    # 额外微量缓冲以保证 Windows 内核解除 EXE/DLL Section 映射
    Start-Sleep -Milliseconds 50
}

# 2. 增量编译 C++ (仅编译修改的模块，毫秒级)
& $cmakeExe --build build --config Release --parallel 16
if ($LASTEXITCODE -ne 0) {
    Write-Host "❌ 增量编译失败！" -ForegroundColor Red
    exit 1
}

# 3. 快速同步二进制到 deploy_dist (0.1秒)
$distDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\deploy_dist"))
if (-not (Test-Path $distDir)) {
    New-Item -ItemType Directory -Path $distDir -Force | Out-Null
}
$pluginsDist = Join-Path $distDir "plugins"
if (-not (Test-Path $pluginsDist)) {
    New-Item -ItemType Directory -Path $pluginsDist -Force | Out-Null
}

$primaryExe = if (Test-Path "build\bin\Release\Tools3000.exe") { "Tools3000.exe" } else { "Tools3000.exe" }
$primaryService = if (Test-Path "build\bin\Release\Tools3000_Service.exe") { "Tools3000_Service.exe" } else { "Tools3000_Service.exe" }

Copy-Item "build\bin\Release\$primaryExe" (Join-Path $distDir "Tools3000.exe") -Force
Copy-Item "build\bin\Release\$primaryExe" (Join-Path $distDir "Tools3000.exe") -Force
Copy-Item "build\bin\Release\$primaryService" (Join-Path $distDir "Tools3000_Service.exe") -Force
Copy-Item "build\bin\Release\$primaryService" (Join-Path $distDir "Tools3000_Service.exe") -Force
Copy-Item "build\bin\Release\Tools3000Core.dll" (Join-Path $distDir "Tools3000Core.dll") -Force
if (Test-Path "build\bin\plugins\Release") {
    Copy-Item "build\bin\plugins\Release\*.dll" $pluginsDist -Force
}

$sw.Stop()
Write-Host "✅ [增量编译就绪] 耗时: $($sw.ElapsedMilliseconds) ms" -ForegroundColor Green

if (-not $NoRun) {
    Write-Host "🚀 正在启动 Tools3000 桌面客户端..." -ForegroundColor Yellow
    Start-Process -FilePath (Join-Path $distDir "Tools3000.exe") -WorkingDirectory $distDir
}

