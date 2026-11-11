param(
    [switch]$NoRun
)

$ErrorActionPreference = "Stop"
$sw = [System.Diagnostics.Stopwatch]::StartNew()

Write-Host "[INFO] [Tools3000 Rapid Dev] 正在准备极速增量编译..." -ForegroundColor Cyan

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
        Write-Host "[ERROR] 无法终止旧的 Tools3000 进程 ($($remaining.Name))，文件句柄仍被占用！" -ForegroundColor Red
        exit 1
    }
    # 额外微量缓冲以保证 Windows 内核解除 EXE/DLL Section 映射
    Start-Sleep -Milliseconds 50
}

# 2. 增量编译 C++ (仅编译修改的模块，毫秒级)
& $cmakeExe --build build --config Release --parallel 16
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] 增量编译失败！" -ForegroundColor Red
    exit 1
}

# 3. 快速同步二进制到 deploy_dist (严格剔除 gtest/gmock 与 plugins 重复第三方 DLL)
$distDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\deploy_dist"))
if (-not (Test-Path $distDir)) {
    New-Item -ItemType Directory -Path $distDir -Force | Out-Null
}
$pluginsDist = Join-Path $distDir "plugins"
if (-not (Test-Path $pluginsDist)) {
    New-Item -ItemType Directory -Path $pluginsDist -Force | Out-Null
}

Copy-Item "build\bin\Release\Tools3000.exe" (Join-Path $distDir "Tools3000.exe") -Force
Copy-Item "build\bin\Release\Tools3000_Service.exe" (Join-Path $distDir "Tools3000_Service.exe") -Force

# 复制主程序根目录运行所需 DLL (排除测试库与插件本体)
Get-ChildItem -Path "build\bin\Release" -Filter "*.dll" -File -ErrorAction SilentlyContinue | Where-Object {
    $_.Name -notlike "*gtest*.dll" -and $_.Name -notlike "*gmock*.dll" -and $_.Name -notlike "Plugin_*.dll"
} | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $distDir $_.Name) -Force
}

# 主动清理根目录可能残留的测试库或误拷入的插件 DLL
Get-ChildItem -Path $distDir -File -ErrorAction SilentlyContinue | Where-Object {
    $_.Name -like "*gtest*.dll" -or $_.Name -like "*gmock*.dll" -or $_.Name -like "Plugin_*.dll" -or $_.Name -like "Tools3000Tests*"
} | Remove-Item -Force -ErrorAction SilentlyContinue

if (Test-Path "build\bin\plugins\Release") {
    # 主动清理 deploy_dist/plugins/ 下历史残留的非 Plugin_*.dll 重复第三方运行库与调试符号
    Get-ChildItem -Path $pluginsDist -File -ErrorAction SilentlyContinue | Where-Object {
        ($_.Extension -ieq ".dll" -and $_.Name -notlike "Plugin_*.dll") -or $_.Extension -ieq ".pdb"
    } | Remove-Item -Force -ErrorAction SilentlyContinue

    # 严格仅复制 Plugin_*.dll 与 *.plugin.json 至 plugins/ 目录
    Get-ChildItem -Path "build\bin\plugins\Release" -File | Where-Object {
        $_.Name -like "Plugin_*.dll" -or $_.Name -like "*.plugin.json"
    } | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $pluginsDist $_.Name) -Force
    }

    # 补充插件依赖但主程序未直接引用的第三方运行库至主程序根目录 (唯一保留一份)
    Get-ChildItem -Path "build\bin\plugins\Release" -Filter "*.dll" -File | Where-Object {
        $_.Name -notlike "Plugin_*.dll" -and $_.Name -notlike "*gtest*.dll" -and $_.Name -notlike "*gmock*.dll"
    } | ForEach-Object {
        $rootDest = Join-Path $distDir $_.Name
        if (-not (Test-Path $rootDest)) {
            Copy-Item $_.FullName $rootDest -Force
        }
    }
}

$signScript = Join-Path $PSScriptRoot "sign-artifacts.ps1"
if (Test-Path -LiteralPath $signScript) {
    $ownedToSign = @(
        (Join-Path $distDir "Tools3000.exe"),
        (Join-Path $distDir "Tools3000_Service.exe"),
        (Join-Path $distDir "Tools3000Core.dll")
    ) + @(Get-ChildItem -Path $pluginsDist -Filter "Plugin_*.dll" -File -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
    $existingToSign = @($ownedToSign | Where-Object { Test-Path -LiteralPath $_ })
    if ($existingToSign.Count -gt 0) {
        & $signScript -Paths $existingToSign
    }
}

$sw.Stop()
Write-Host "[OK] [增量编译就绪] 耗时: $($sw.ElapsedMilliseconds) ms" -ForegroundColor Green

if (-not $NoRun) {
    Write-Host "[INFO] 正在启动 Tools3000 桌面客户端..." -ForegroundColor Yellow
    Start-Process -FilePath (Join-Path $distDir "Tools3000.exe") -WorkingDirectory $distDir
}

