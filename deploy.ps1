<#
.SYNOPSIS
Tools3000 CI/CD 自动化部署脚本 (Idempotent Deployment Script)

.DESCRIPTION
此脚本一键完成环境检测、依赖安装、前端构建、C++ 编译和成品打包，完全幂等。支持 sccache 极速编译与 vcpkg 二进制缓存。

.EXAMPLE
.\deploy.ps1 -Configuration Release
.\deploy.ps1 -Quick
#>

param (
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [ValidateSet("x64", "arm64")]
    [string]$Arch = "x64",              # Windows 目标架构；ARM64 使用交叉编译工具链
    [string]$VcpkgRoot = "",
    [switch]$Quick = $false,             # 极速增量开发模式 (跳过 npm ci，复用 node_modules 直奔编译与测试)
    [switch]$SkipUI = $false,            # 跳过前端构建，直接复用 ui/dist/index.html 已有产物
    [switch]$SkipTests = $false,         # 跳过 CTest 单元测试
    [switch]$SkipInstaller = $false,     # 跳过 Inno Setup 安装包生成
    [switch]$Coverage = $false,          # 启用 C++ 代码覆盖率分析与防回退门禁 (OpenCppCoverage)
    [switch]$StaticAnalysis = $false,    # 对核心、搜索插件和索引服务运行 MSVC /analyze
    [switch]$Install = $false,           # 构建完成后立即通过 CLI 执行静默安装与启动
    [switch]$RequireSigning = $false,    # 正式发布门禁：所有自有二进制与安装包必须签名
    [string]$BinaryCacheDir = ""         # 自定义 vcpkg 二进制包缓存目录
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $ScriptDir
$VcpkgTargetTriplet = "$Arch-windows"
$BuildDirectoryName = if ($Arch -eq "x64") { "build" } else { "build-$Arch" }

if ($RequireSigning -and $SkipInstaller) {
    throw "-RequireSigning 不允许与 -SkipInstaller 同时使用：正式发布必须生成并签名安装包。"
}
if ($Coverage -and $SkipTests) {
    throw "-Coverage 不允许与 -SkipTests 同时使用：覆盖率门禁必须执行测试。"
}

$HostArchitecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if ($Install -and $Arch -ne $HostArchitecture) {
    throw "不能在 $HostArchitecture 主机上安装 $Arch 目标；请在目标架构机器上执行 -Install。"
}
if (-not $SkipTests -and $Arch -eq "arm64" -and $HostArchitecture -ne "arm64") {
    throw "当前 $HostArchitecture 主机不能执行 ARM64 测试与生命周期门禁。交叉编译请显式传入 -SkipTests，并在 ARM64 发布机补跑测试。"
}

$VersionFile = Join-Path $ScriptDir "VERSION"
if (-not (Test-Path -LiteralPath $VersionFile)) {
    throw "缺少唯一版本源: $VersionFile"
}
$ProjectVersion = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
if ($ProjectVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION 必须是稳定 SemVer（例如 1.2.3），当前值: $ProjectVersion"
}
# ARM64 is a real target, not a relabelled x64 artifact. Fail before dependency
# installation when the cross-compiler workload is absent.
if ($Arch -eq "arm64") {
    $Arm64Toolchain = & (Join-Path $ScriptDir 'scripts\Get-Arm64Toolchain.ps1')
    $vsPath = $Arm64Toolchain.InstallationPath
}

$TraceID = [guid]::NewGuid().ToString("N").Substring(0, 8)
$LogDir = Join-Path $ScriptDir "deploy_logs"
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir | Out-Null }
$LogFile = Join-Path $LogDir "deploy_$(Get-Date -Format 'yyyyMMdd').log"

# 统一日志函数
function Write-Log ($Message, $Level = "INFO") {
    $TimeStamp = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    $LogStr = "[$TimeStamp] [$TraceID] [$Level] $Message"
    $Color = switch ($Level) {
        "INFO" { "Cyan" }
        "WARN" { "Yellow" }
        "ERROR" { "Red" }
        "SUCCESS" { "Green" }
        default { "White" }
    }
    Write-Host "[$TimeStamp] [$Level] $Message" -ForegroundColor $Color
    Add-Content -Path $LogFile -Value $LogStr -Encoding UTF8
}

Write-Log "======================================================="
Write-Log "启动 Tools3000 一键幂等部署流程"
Write-Log "配置环境: $Configuration"
Write-Log "目标架构: $Arch (vcpkg: $VcpkgTargetTriplet)"
Write-Log "======================================================="

# ------------------------------------------------------------------------------
# 1. 前端构建 (React + Vite)
# ------------------------------------------------------------------------------
Write-Log "检查前端环境 (ui/)..."
if (Test-Path "ui/package.json") {
    $DistIndex = Join-Path $ScriptDir "ui\dist\index.html"
    $SkipFrontend = $false

    if ($SkipUI) {
        if (Test-Path -LiteralPath $DistIndex) {
            $SkipFrontend = $true
            Write-Log "[INFO] 已指定 -SkipUI，智能跳过前端构建并直接复用现有产物 ($DistIndex)" "SUCCESS"
        } else {
            Write-Log "已指定 -SkipUI 但前端产物不存在 ($DistIndex)，执行必要的前端构建..." "WARN"
        }
    } elseif ($Quick -and (Test-Path -LiteralPath $DistIndex)) {
        $DistTime = (Get-Item -LiteralPath $DistIndex).LastWriteTime
        $UiSourceFiles = Get-ChildItem -Path (Join-Path $ScriptDir "ui") -Recurse -File | Where-Object {
            $_.FullName -notlike "*\node_modules\*" -and
            $_.FullName -notlike "*\dist\*" -and
            $_.FullName -notlike "*\.git\*"
        }
        $NewerSources = @($UiSourceFiles | Where-Object { $_.LastWriteTime -gt $DistTime })
        if ($NewerSources.Count -eq 0) {
            $SkipFrontend = $true
            Write-Log "[INFO] 极速模式: 前端源码无变更，智能跳过前端构建并直接复用产物 ($DistIndex)" "SUCCESS"
        } else {
            Write-Log "检测到 $($NewerSources.Count) 个前端源码文件有更新，重新执行前端构建..." "INFO"
        }
    }

    if (-not $SkipFrontend) {
        Push-Location ui
        try {
            if (-not $Quick -or -not (Test-Path "node_modules")) {
                Write-Log "执行 npm ci (锁定依赖)..."
                npm ci --prefer-offline --no-audit
                if ($LASTEXITCODE -ne 0) { throw "npm ci 失败，退出码: $LASTEXITCODE" }
            } else {
                Write-Log "[INFO] 极速模式: 复用本地 node_modules 依赖" "INFO"
            }

            foreach ($Command in @("lint", "i18n-check", "css-check", "typography-check", "trim-workingset-check", "test", "build")) {
                Write-Log "执行 npm run $Command..."
                npm run $Command
                if ($LASTEXITCODE -ne 0) {
                    throw "npm run $Command 失败，退出码: $LASTEXITCODE"
                }
            }
            Write-Log "前端构建完成。" "SUCCESS"
        } catch {
            Write-Log "前端构建失败: $_" "ERROR"
            throw
        } finally {
            Pop-Location
        }
    }
} else {
    Write-Log "未发现前端工程 (ui/package.json)，跳过前端构建。" "WARN"
}

# ------------------------------------------------------------------------------
# 2. Vcpkg 依赖安装与二进制归档缓存 (Binary Cache Acceleration)
# ------------------------------------------------------------------------------
Write-Log "检查 Vcpkg 依赖环境与二进制缓存配置..."
if (-not (Get-Command "git" -ErrorAction SilentlyContinue)) {
    throw "找不到 Git；无法获取或校验固定版本的 vcpkg 工具源码。"
}
$VcpkgManifest = Get-Content -LiteralPath (Join-Path $ScriptDir "vcpkg.json") -Raw | ConvertFrom-Json
$VcpkgBaseline = [string]$VcpkgManifest.'builtin-baseline'
if ($VcpkgBaseline -notmatch '^[0-9a-f]{40}$') {
    throw "vcpkg.json 必须固定 40 位 builtin-baseline，当前值: $VcpkgBaseline"
}
if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $LocalAppDataRoot = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::LocalApplicationData)
    if ([string]::IsNullOrWhiteSpace($LocalAppDataRoot)) {
        throw "无法确定当前用户的 LocalAppData 目录；请显式传入 -VcpkgRoot。"
    }
    $UserVcpkgRoot = Join-Path $LocalAppDataRoot "Tools3000\vcpkg"
    $VcpkgCandidates = @($env:VCPKG_ROOT, "C:\vcpkg", $UserVcpkgRoot) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -Unique
    $VcpkgRoot = $VcpkgCandidates | Where-Object {
        Test-Path -LiteralPath (Join-Path $_ "scripts\buildsystems\vcpkg.cmake")
    } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
        $VcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { $UserVcpkgRoot }
    }
}
$VcpkgRoot = [System.IO.Path]::GetFullPath($VcpkgRoot)
$VcpkgToolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $VcpkgToolchain)) {
    if (Test-Path -LiteralPath $VcpkgRoot) {
        throw "vcpkg 目录已存在但不完整，拒绝覆盖: $VcpkgRoot"
    }
    $VcpkgParent = Split-Path -Parent $VcpkgRoot
    if (-not (Test-Path -LiteralPath $VcpkgParent)) {
        New-Item -ItemType Directory -Path $VcpkgParent -Force | Out-Null
    }
    Write-Log "未检测到 Vcpkg ($VcpkgRoot)。按清单固定提交 $VcpkgBaseline 安装..." "WARN"
    git clone --no-checkout https://github.com/microsoft/vcpkg.git $VcpkgRoot
    if ($LASTEXITCODE -ne 0) { throw "克隆 vcpkg 失败" }
    git -C $VcpkgRoot checkout --detach $VcpkgBaseline
    if ($LASTEXITCODE -ne 0) { throw "检出固定 vcpkg 提交失败: $VcpkgBaseline" }
} else {
    Write-Log "发现 Vcpkg 安装在: $VcpkgRoot"
}
$VcpkgHeadOutput = git -C $VcpkgRoot rev-parse HEAD 2>$null
$VcpkgHead = if ($LASTEXITCODE -eq 0) { ([string]$VcpkgHeadOutput).Trim() } else { "" }
if ($VcpkgHead -ne $VcpkgBaseline) {
    if ($env:GITHUB_ACTIONS -eq 'true' -or $env:CI -eq 'true') {
        git config --global --add safe.directory $VcpkgRoot 2>$null
        Write-Log "检测到处于 CI 虚拟环境，当前 Vcpkg HEAD ($VcpkgHead) 与清单基线 ($VcpkgBaseline) 不一致。尝试自动对齐固定基线..." "WARN"
        git -C $VcpkgRoot checkout --force --detach $VcpkgBaseline 2>$null
        if ($LASTEXITCODE -ne 0) {
            Write-Log "本地缺失固定基线对象，尝试从远程 fetch..." "WARN"
            git -C $VcpkgRoot fetch origin $VcpkgBaseline --depth=1 2>$null
            if ($LASTEXITCODE -ne 0) {
                git -C $VcpkgRoot fetch origin 2>$null
            }
            git -C $VcpkgRoot checkout --force --detach $VcpkgBaseline 2>$null
        }
        $VcpkgHeadOutput = git -C $VcpkgRoot rev-parse HEAD 2>$null
        $VcpkgHead = if ($LASTEXITCODE -eq 0) { ([string]$VcpkgHeadOutput).Trim() } else { "" }

        if ($VcpkgHead -ne $VcpkgBaseline) {
            # 预装 Vcpkg 目录无写权限或 fetch 失败时，回退至独立工作区隔离克隆
            $CiVcpkgRoot = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)) "Tools3000\vcpkg-ci"
            Write-Log "预装 Vcpkg 切换基线失败，自动克隆固定基线至独立工作区: $CiVcpkgRoot" "WARN"
            if (Test-Path -LiteralPath $CiVcpkgRoot) {
                Remove-Item -LiteralPath $CiVcpkgRoot -Recurse -Force -ErrorAction SilentlyContinue
            }
            $CiVcpkgParent = Split-Path -Parent $CiVcpkgRoot
            if (-not (Test-Path -LiteralPath $CiVcpkgParent)) {
                New-Item -ItemType Directory -Path $CiVcpkgParent -Force | Out-Null
            }
            git init $CiVcpkgRoot
            git -C $CiVcpkgRoot remote add origin https://github.com/microsoft/vcpkg.git
            git -C $CiVcpkgRoot fetch --depth=1 origin $VcpkgBaseline 2>$null
            if ($LASTEXITCODE -ne 0) {
                Remove-Item -LiteralPath $CiVcpkgRoot -Recurse -Force -ErrorAction SilentlyContinue
                git clone --no-checkout https://github.com/microsoft/vcpkg.git $CiVcpkgRoot
                if ($LASTEXITCODE -ne 0) { throw "CI 独立克隆 vcpkg 失败" }
            }
            git -C $CiVcpkgRoot checkout --detach $VcpkgBaseline
            if ($LASTEXITCODE -ne 0) { throw "CI 独立检出固定 vcpkg 提交失败: $VcpkgBaseline" }
            $VcpkgRoot = $CiVcpkgRoot
            $VcpkgHeadOutput = git -C $VcpkgRoot rev-parse HEAD 2>$null
            $VcpkgHead = if ($LASTEXITCODE -eq 0) { ([string]$VcpkgHeadOutput).Trim() } else { "" }
        }
    }
}
if ($VcpkgHead -ne $VcpkgBaseline) {
    throw "vcpkg 工具源码必须固定到清单提交 $VcpkgBaseline，当前为 $VcpkgHead"
}
$VcpkgTrackedChanges = @(git -C $VcpkgRoot status --porcelain --untracked-files=no 2>$null)
if ($LASTEXITCODE -ne 0) {
    throw "无法检查 vcpkg 工作树状态: $VcpkgRoot"
}
if ($VcpkgTrackedChanges.Count -gt 0) {
    if ($env:GITHUB_ACTIONS -eq 'true' -or $env:CI -eq 'true') {
        Write-Log "CI 环境下检测到 vcpkg 工作树存在修改，执行强制重置与清理..." "WARN"
        git -C $VcpkgRoot reset --hard $VcpkgBaseline 2>$null
        git -C $VcpkgRoot clean -fd 2>$null
        $VcpkgTrackedChanges = @(git -C $VcpkgRoot status --porcelain --untracked-files=no 2>$null)
    }
}
if ($VcpkgTrackedChanges.Count -gt 0) {
    throw "vcpkg 工具源码包含本地修改；发布构建必须使用无修改的固定提交 $VcpkgBaseline。"
}
$env:VCPKG_ROOT = $VcpkgRoot
$env:VCPKG_INSTALLATION_ROOT = $VcpkgRoot
$VcpkgBootstrap = Join-Path $VcpkgRoot "bootstrap-vcpkg.bat"
if (-not (Test-Path -LiteralPath $VcpkgBootstrap)) {
    throw "固定提交中缺少 vcpkg 引导脚本: $VcpkgBootstrap"
}
$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if ((Test-Path -LiteralPath $VcpkgExe) -and ($VcpkgHead -eq $VcpkgBaseline)) {
    Write-Log "[INFO] vcpkg.exe 已就绪且与基线 ($VcpkgBaseline) 一致，跳过重复 bootstrap。" "SUCCESS"
} else {
    Write-Log "已校验固定 vcpkg 源码，引导工具程序..."
    & $VcpkgBootstrap -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "bootstrap vcpkg 失败" }
}

# 自动激活本地/CI vcpkg 二进制包归档缓存 (按 ABI 哈希缓存，彻底免除重编译)
if ([string]::IsNullOrEmpty($BinaryCacheDir)) {
    $BinaryCacheDir = if ($env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE } else { Join-Path $env:LOCALAPPDATA "vcpkg\archives" }
}
if (-not (Test-Path $BinaryCacheDir)) {
    New-Item -ItemType Directory -Path $BinaryCacheDir -Force | Out-Null
}
$env:VCPKG_DEFAULT_BINARY_CACHE = $BinaryCacheDir
Write-Log "[INFO] 已激活 Vcpkg 二进制归档加速缓存: $BinaryCacheDir" "SUCCESS"

$VcpkgToolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $VcpkgToolchain)) {
    throw "Vcpkg Toolchain 文件丢失: $VcpkgToolchain"
}

# ------------------------------------------------------------------------------
# 3. WebView2 SDK 固定哈希恢复
# ------------------------------------------------------------------------------
Write-Log "检查 WebView2 SDK..."
$RestoreWebView2Script = Join-Path $ScriptDir "scripts\restore-webview2.ps1"
if (-not (Test-Path -LiteralPath $RestoreWebView2Script -PathType Leaf)) {
    throw "缺少 WebView2 SDK 恢复脚本: $RestoreWebView2Script"
}
$WebView2Sdk = & $RestoreWebView2Script -ProjectRoot $ScriptDir
$WebView2Version = [string]$WebView2Sdk.Version
$WebView2TargetDir = [string]$WebView2Sdk.TargetDirectory
if (-not $WebView2Version -or -not (Test-Path -LiteralPath $WebView2TargetDir)) {
    throw "WebView2 SDK 恢复后仍不可用。"
}
Write-Log "已就绪 WebView2 SDK: $WebView2TargetDir" "SUCCESS"

# ------------------------------------------------------------------------------
# 4. CMake 构建 C++ 核心与插件 (含 sccache 编译器级缓存)
# ------------------------------------------------------------------------------
Write-Log "准备 C++ 构建环境..."

# 动态挂载 VS 开发环境工具链
if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue) -or -not (Get-Command "cl.exe" -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $VsRequiredComponents = @("Microsoft.VisualStudio.Workload.VCTools")
        if ($Arch -eq "arm64") { $VsRequiredComponents += "Microsoft.VisualStudio.Component.VC.Tools.ARM64" }
        $vsPath = & $vswhere -latest -products * -requires $VsRequiredComponents -property installationPath
        if ($vsPath) {
            $devShell = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
            if (Test-Path $devShell) {
                Import-Module $devShell
                Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
                    -DevCmdArguments "-arch=$Arch -host_arch=x64" | Out-Null
                Write-Log "[OK] 成功挂载 VS $Arch 编译环境 ($vsPath)!"
            }
        }
    }
}

if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue)) {
    throw "仍然无法找到 CMake！请确保已安装 C++ 桌面开发工作负载，或尝试在 'Developer PowerShell for VS' 窗口中运行此脚本。"
}

$CMakeExtraArgs = @()
if ($StaticAnalysis) {
    $CMakeExtraArgs += "-DTOOLS3000_ENABLE_MSVC_ANALYSIS=ON"
} else {
    $CMakeExtraArgs += "-DTOOLS3000_ENABLE_MSVC_ANALYSIS=OFF"
}
if (Get-Command "sccache" -ErrorAction SilentlyContinue) {
    Write-Log "[INFO] 检测到 sccache 编译器缓存，自动启用 C/C++ 极速编译加速..." "SUCCESS"
    $CMakeExtraArgs += "-DCMAKE_C_COMPILER_LAUNCHER=sccache"
    $CMakeExtraArgs += "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache"
}

Write-Log "执行 CMake Configure..."
$BuildDir = Join-Path $ScriptDir $BuildDirectoryName
if (-not $Quick -and (Test-Path -LiteralPath $BuildDir)) {
    $ResolvedBuildDir = [System.IO.Path]::GetFullPath($BuildDir)
    $WorkspacePrefix = [System.IO.Path]::GetFullPath($ScriptDir).TrimEnd('\') + '\'
    $AllowedBuildNames = @("build", "build-arm64")
    if (-not $ResolvedBuildDir.StartsWith($WorkspacePrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $ResolvedBuildDir) -notin $AllowedBuildNames) {
        throw "拒绝清理未经验证的构建目录: $ResolvedBuildDir"
    }
    $BuildItem = Get-Item -LiteralPath $ResolvedBuildDir -Force
    if (($BuildItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "拒绝清理重解析点构建目录: $ResolvedBuildDir"
    }
    Write-Log "正式构建使用全新构建树，清理: $ResolvedBuildDir"
    Remove-Item -LiteralPath $ResolvedBuildDir -Recurse -Force
}
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

$CMakePlatformArgs = @()
if ($Arch -eq "arm64") {
    # Use a dedicated build tree so CMake never reuses an x64 generator cache.
    $CMakePlatformArgs = @("-A", "ARM64", "-G", $Arm64Toolchain.Generator,
        "-DCMAKE_GENERATOR_INSTANCE=$($Arm64Toolchain.InstallationPath)")
} elseif (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) {
    $CMakePlatformArgs = @("-A", "x64")
}
$VcpkgInstalledDir = Join-Path $ScriptDir "vcpkg_installed"
cmake -B $BuildDir -S . @CMakePlatformArgs -DCMAKE_TOOLCHAIN_FILE="$VcpkgToolchain" `
    -DVCPKG_TARGET_TRIPLET="$VcpkgTargetTriplet" -DVCPKG_INSTALLED_DIR="$VcpkgInstalledDir" @CMakeExtraArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败！退出码: $LASTEXITCODE"
}

# 确保全套多分辨率 Windows 图标与托盘图标存在，并清理旧版 .res 确保资源强制重新链接
$IconScript = "$ScriptDir\resources\build_master_production_icons.py"
$AppIco = "$ScriptDir\resources\app.ico"
$TrayIco = "$ScriptDir\resources\tray.ico"
$TrayDarkIco = "$ScriptDir\resources\tray_dark.ico"
$TrayColorIco = "$ScriptDir\resources\tray_color.ico"
$AllIconsExist = (Test-Path $AppIco) -and (Test-Path $TrayIco) -and (Test-Path $TrayDarkIco) -and (Test-Path $TrayColorIco)
$ScriptNewer = $AllIconsExist -and ((Get-Item $IconScript).LastWriteTime -gt (Get-Item $AppIco).LastWriteTime)
# 在 CI 环境中，git checkout 文件时间戳为当前时间，微秒差异不应误判母版更新；只要图标均存在即可直接复用
if (-not $AllIconsExist -or ($ScriptNewer -and -not ($env:GITHUB_ACTIONS -eq 'true' -or $env:CI -eq 'true'))) {
    Write-Log "检测到图标未初始化或母版脚本已更新，调用母版管道生成 (resources/build_master_production_icons.py)..."
    python "$ScriptDir\resources\build_master_production_icons.py" 1
}
Get-ChildItem -Path $BuildDir -Filter "*.res" -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

$CpuCount = [Environment]::ProcessorCount
Write-Log "执行 CMake Build ($Configuration, 并发核心数: $CpuCount)..."
cmake --build $BuildDir --config $Configuration --parallel $CpuCount

if ($LASTEXITCODE -ne 0) {
    throw "C++ 编译失败！退出码: $LASTEXITCODE"
}
Write-Log "C++ 编译完成。" "SUCCESS"

# ------------------------------------------------------------------------------
# 4.5 运行单元测试与代码覆盖率防回退分析 (失败则中断流水线)
# ------------------------------------------------------------------------------
if (-not $SkipTests) {
    $OpenCppCoverageExe = $null
    if (Get-Command "OpenCppCoverage" -ErrorAction SilentlyContinue) {
        $OpenCppCoverageExe = "OpenCppCoverage"
    } elseif (Test-Path "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe") {
        $OpenCppCoverageExe = "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe"
    }
    if ($Coverage -and -not $OpenCppCoverageExe) {
        throw "已要求覆盖率门禁，但找不到 OpenCppCoverage；请安装后重试。"
    }

    if ($Coverage) {
        Write-Log "运行 C++ 单元测试与代码覆盖率防回退分析..."
        $CoverageReportDir = Join-Path $ScriptDir "coverage_report\$TraceID"
        if (-not (Test-Path $CoverageReportDir)) { New-Item -ItemType Directory -Path $CoverageReportDir | Out-Null }

        $TestExe = Join-Path $BuildDir "bin\$Configuration\Tools3000Tests.exe"
        if (-not (Test-Path $TestExe)) { $TestExe = Join-Path $BuildDir "bin\Tools3000Tests.exe" }

        if ((Test-Path $TestExe) -and $OpenCppCoverageExe) {
            & $OpenCppCoverageExe --sources "$ScriptDir\src" `
                                  --excluded_sources "$BuildDir" `
                                  --excluded_sources "$ScriptDir\packages" `
                                  --excluded_sources "$ScriptDir\tests" `
                                  --export_type "html:$CoverageReportDir" `
                                  --export_type "cobertura:$CoverageReportDir\cobertura.xml" `
                                  -- $TestExe --gtest_output="xml:$CoverageReportDir\junit.xml"
            if ($LASTEXITCODE -ne 0) {
                throw "单元测试与代码覆盖率分析执行失败！退出码: $LASTEXITCODE"
            }
            $CoberturaPath = Join-Path $CoverageReportDir "cobertura.xml"
            if (-not (Test-Path -LiteralPath $CoberturaPath)) {
                throw "覆盖率工具未生成本轮 Cobertura 报告: $CoberturaPath"
            }
            [xml]$CoverageXml = Get-Content -LiteralPath $CoberturaPath
            $LineRate = [double]$CoverageXml.coverage.'line-rate'
            if ([double]::IsNaN($LineRate) -or $LineRate -lt 0.32 -or $LineRate -gt 1) {
                throw "C++ 行覆盖率 $([math]::Round($LineRate * 100, 2))% 未达到有效的 32% 防回退门禁"
            }
            Write-Log "C++ 行覆盖率: $([math]::Round($LineRate * 100, 2))% (门禁 >= 32%)" "SUCCESS"
            Write-Log "代码覆盖率报告与 GTest JUnit 报表已生成: $CoverageReportDir" "SUCCESS"
        } else {
            throw "覆盖率门禁缺少待测二进制或工具: $TestExe"
        }
    } else {
        Write-Log "运行 CTest 测试套件..."
        ctest --test-dir $BuildDir -C $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            throw "测试失败！退出码: $LASTEXITCODE"
        }
    }
    Write-Log "测试套件通过。" "SUCCESS"
} else {
    Write-Log "已指定 -SkipTests，跳过单测执行。" "WARN"
}

# ------------------------------------------------------------------------------
# 5. 打包输出物 (Deploy & Artifact Purification)
# ------------------------------------------------------------------------------
Write-Log "开始提纯输出物并写入暂存区..."
$DeployDir = Join-Path $ScriptDir "deploy_dist"
$StagingDir = Join-Path $ScriptDir "deploy_dist_staging_$TraceID"
$SymbolsDir = Join-Path $ScriptDir "deploy_symbols"

if (Test-Path $StagingDir) {
    Remove-Item -Recurse -Force $StagingDir
}
New-Item -ItemType Directory -Path $StagingDir | Out-Null

# 符号隔离归档 (Symbol Stripping & Independent Archive)
if (-not (Test-Path $SymbolsDir)) {
    New-Item -ItemType Directory -Path $SymbolsDir | Out-Null
}
$SymbolsZip = Join-Path $SymbolsDir "Tools3000-Symbols-v$ProjectVersion.zip"
$PdbFiles = Get-ChildItem -Path (Join-Path $BuildDir "bin") -Filter "*.pdb" -Recurse -ErrorAction SilentlyContinue
if ($PdbFiles) {
    Write-Log "正在将 $($PdbFiles.Count) 个调试符号 (*.pdb) 归档到: $SymbolsZip..."
    if (Test-Path $SymbolsZip) { Remove-Item -Force $SymbolsZip -ErrorAction SilentlyContinue }
    Compress-Archive -Path $PdbFiles.FullName -DestinationPath $SymbolsZip -Force
    Write-Log "调试符号已安全剥离并独立归档至 deploy_symbols/。" "SUCCESS"
}

# 二进制主文件
$ExePath = Join-Path $BuildDir "bin\$Configuration\Tools3000.exe"
if (-not (Test-Path $ExePath)) {
    $ExePath = Join-Path $BuildDir "bin\Tools3000.exe"
}

if (Test-Path $ExePath) {
    Copy-Item $ExePath -Destination $StagingDir
    $TargetExeName = Split-Path $ExePath -Leaf
    Write-Log "已复制可执行文件: $TargetExeName"
    Copy-Item $ExePath -Destination (Join-Path $StagingDir "Tools3000.exe") -Force

    $ExeDir = Split-Path $ExePath -Parent
    $ServicePath = Join-Path $ExeDir "Tools3000_Service.exe"
    if (-not (Test-Path $ServicePath)) {
        throw "找不到编译后的 Tools3000_Service.exe"
    }
    Copy-Item $ServicePath -Destination $StagingDir
    $TargetServiceName = Split-Path $ServicePath -Leaf
    Write-Log "已复制文件索引服务: $TargetServiceName"
    Copy-Item $ServicePath -Destination (Join-Path $StagingDir "Tools3000_Service.exe") -Force
    
    # 复制所有同一目录下的运行库 DLL 文件 (排除 gtest, gmock 等测试库)
    $DllFiles = Get-ChildItem -Path $ExeDir -Filter "*.dll" | Where-Object {
        $_.Name -notlike "gtest*.dll" -and $_.Name -notlike "gmock*.dll"
    }
    foreach ($dll in $DllFiles) {
        Copy-Item $dll.FullName -Destination $StagingDir
    }
    Write-Log "已复制生产运行库 DLL 文件 (已排除测试库 gtest)"
    
    # 复制插件目录 (仅复制插件 DLL 与 plugin.json，严禁复制 pdb 及重复三方库)
    $PluginsDir = Join-Path $BuildDir "bin\plugins\$Configuration"
    if (Test-Path $PluginsDir) {
        $TargetPluginsDir = Join-Path $StagingDir "plugins"
        New-Item -ItemType Directory -Path $TargetPluginsDir -ErrorAction SilentlyContinue | Out-Null
        $PluginFiles = Get-ChildItem -Path $PluginsDir | Where-Object {
            $_.Name -like "Plugin_*.dll" -or $_.Name -like "*.plugin.json"
        }
        foreach ($pf in $PluginFiles) {
            Copy-Item $pf.FullName -Destination $TargetPluginsDir
        }
        Write-Log "已复制纯净插件集合 (plugins/)，无冗余 DLL 与 PDB"

        # 补充收集插件所需但主程序未直接引用的第三方运行库 (如 opencv_photo4.dll 等) 到根运行目录
        $PluginDependencyDlls = Get-ChildItem -Path $PluginsDir -Filter "*.dll" | Where-Object {
            $_.Name -notlike "Plugin_*.dll" -and $_.Name -notlike "gtest*.dll" -and $_.Name -notlike "gmock*.dll"
        }
        foreach ($pDll in $PluginDependencyDlls) {
            $destFile = Join-Path $StagingDir $pDll.Name
            if (-not (Test-Path $destFile)) {
                Copy-Item $pDll.FullName -Destination $StagingDir
                Write-Log "补充复制插件依赖运行库到根目录: $($pDll.Name)"
            }
        }
    } else {
        Write-Log "未找到插件构建目录: $PluginsDir" "WARN"
    }
} else {
    throw "找不到编译后的 Tools3000.exe"
}

# MSVC 动态运行库。Tools3000 与 vcpkg 动态 triplet 依赖均使用动态 CRT；
# 便携包不能假设目标机器预装了 Visual C++ Redistributable。优先使用当前
# DevShell 精确对应的 Redist 目录，CI 中再通过 vswhere 定位同一工具链。
$VcRuntimeNames = @(
    "msvcp140.dll", "msvcp140_atomic_wait.dll", "concrt140.dll", "vcruntime140.dll"
)
if ($Arch -eq 'x64') { $VcRuntimeNames += 'vcruntime140_1.dll' }
$VcCrtDir = $null
$VcRedistRoots = @()
if ($env:VCToolsRedistDir -and (Test-Path $env:VCToolsRedistDir)) {
    $VcRedistRoots += $env:VCToolsRedistDir
}

$RedistVsPath = $vsPath
if (-not $RedistVsPath) {
    $pf86 = ${env:ProgramFiles(x86)}
    if (-not $pf86) { $pf86 = $env:ProgramFiles }
    $vswhere = Join-Path $pf86 "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $RedistVsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath
    }
}
if ($RedistVsPath) {
    $MsVcRedistRoot = Join-Path $RedistVsPath "VC\Redist\MSVC"
    if (Test-Path $MsVcRedistRoot) {
        $VcRedistRoots += Get-ChildItem $MsVcRedistRoot -Directory |
            Sort-Object Name -Descending | ForEach-Object { $_.FullName }
    }
}

foreach ($Root in $VcRedistRoots | Select-Object -Unique) {
    $ArchitectureRoot = Join-Path $Root $Arch
    if (-not (Test-Path $ArchitectureRoot)) { continue }
    $Candidate = Get-ChildItem $ArchitectureRoot -Directory -Filter "Microsoft.VC*.CRT" |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($Candidate) {
        $MissingRuntime = @($VcRuntimeNames | Where-Object {
            -not (Test-Path (Join-Path $Candidate.FullName $_))
        })
        if ($MissingRuntime.Count -eq 0) {
            $VcCrtDir = $Candidate.FullName
            break
        }
    }
}
if (-not $VcCrtDir) {
    throw "找不到与 MSVC 工具链匹配的 $Arch Visual C++ Runtime 可再发行文件"
}
Copy-Item (Join-Path $VcCrtDir "*.dll") -Destination $StagingDir
Write-Log "已复制 Visual C++ Runtime: $VcCrtDir"

# WebView2Loader.dll
$WebView2PackageDir = Get-Item $WebView2TargetDir -ErrorAction SilentlyContinue
if ($WebView2PackageDir) {
    $LoaderPath = Join-Path $WebView2PackageDir.FullName "build\native\$Arch\WebView2Loader.dll"
    if (Test-Path $LoaderPath) {
        Copy-Item $LoaderPath -Destination $StagingDir
        Write-Log "已复制 WebView2Loader.dll"
    }
}

# 白名单资产注入 (resources/)
$TargetResourcesDir = Join-Path $StagingDir "resources"
New-Item -ItemType Directory -Path $TargetResourcesDir -ErrorAction SilentlyContinue | Out-Null

$WhitelistedResourceFiles = @("app.ico", "tray.ico", "tray_dark.ico", "tray_color.ico", "app_icon_hires.png")
foreach ($rFile in $WhitelistedResourceFiles) {
    $srcPath = Join-Path "resources" $rFile
    if (Test-Path $srcPath) {
        Copy-Item $srcPath -Destination $TargetResourcesDir
    }
}
if (Test-Path "resources\scripts") {
    Copy-Item "resources\scripts" -Destination $TargetResourcesDir -Recurse
}
Write-Log "已通过白名单注入运行必需资源 (ico, png, scripts/)"

if (Test-Path "ui/dist") {
    $UiDeployDir = Join-Path $StagingDir "ui"
    New-Item -ItemType Directory -Path $UiDeployDir | Out-Null
    Copy-Item "ui/dist\*" -Destination $UiDeployDir -Recurse
    # 排除测试用 demo 网页
    if (Test-Path (Join-Path $UiDeployDir "theme_demo.html")) {
        Remove-Item -Force (Join-Path $UiDeployDir "theme_demo.html")
    }
    Write-Log "已复制纯净前端产物 (ui/)"
}
if (Test-Path "LICENSE") { Copy-Item "LICENSE" -Destination $StagingDir }
$NoticeScript = Join-Path $ScriptDir "scripts\generate-third-party-notices.ps1"
if (-not (Test-Path -LiteralPath $NoticeScript)) {
    throw "缺少第三方许可与 SBOM 生成脚本: $NoticeScript"
}
$InstalledVcpkgDir = Join-Path $ScriptDir "vcpkg_installed"
if (-not (Test-Path $InstalledVcpkgDir)) {
    $InstalledVcpkgDir = Join-Path $BuildDir "vcpkg_installed"
}
& $NoticeScript -VcpkgInstalledDir $InstalledVcpkgDir `
    -Triplet $VcpkgTargetTriplet -UiDir (Join-Path $ScriptDir "ui") `
    -OutputDirectory $StagingDir -ProjectVersion $ProjectVersion
if ($LASTEXITCODE -ne 0) { throw "第三方许可与 SBOM 生成失败" }

$MainExeArtifact = "Tools3000.exe"
$ServiceExeArtifact = "Tools3000_Service.exe"

$RequiredArtifacts = @(
    $MainExeArtifact, $ServiceExeArtifact, "Tools3000Core.dll",
    "LICENSE", "THIRD_PARTY_NOTICES.txt", "SBOM.spdx.json",
    "ui\index.html", "plugins\Plugin_Gesture.dll", "plugins\Plugin_Capture.dll",
    "plugins\Plugin_Keycast.dll", "plugins\Plugin_Search.dll",
    "plugins\Plugin_DialogEnhancer.dll",
    "plugins\Plugin_Gesture.plugin.json", "plugins\Plugin_Capture.plugin.json",
    "plugins\Plugin_Keycast.plugin.json", "plugins\Plugin_Search.plugin.json",
    "plugins\Plugin_DialogEnhancer.plugin.json"
) + $VcRuntimeNames
foreach ($Artifact in $RequiredArtifacts) {
    if (-not (Test-Path (Join-Path $StagingDir $Artifact))) {
        throw "发布产物不完整，缺少: $Artifact"
    }
}

$SignScript = Join-Path $ScriptDir "scripts\sign-artifacts.ps1"
if (-not (Test-Path -LiteralPath $SignScript)) { throw "缺少签名脚本: $SignScript" }
$OwnedArtifacts = @(
    (Join-Path $StagingDir $MainExeArtifact),
    (Join-Path $StagingDir $ServiceExeArtifact),
    (Join-Path $StagingDir "Tools3000Core.dll")
) + @(Get-ChildItem -LiteralPath (Join-Path $StagingDir "plugins") -Filter "Plugin_*.dll" -File |
    Select-Object -ExpandProperty FullName)
& $SignScript -Paths $OwnedArtifacts -Required:$RequireSigning

# ------------------------------------------------------------------------------
# 6. 优雅关闭及原子交换 (Atomic Swap)
# ------------------------------------------------------------------------------
Write-Log "开始执行原子目录交换..."

# 优雅停止可能正在运行的 Tools3000 进程
$runningProcesses = Get-Process -Name "Tools3000*" -ErrorAction SilentlyContinue
if ($runningProcesses) {
    Write-Log "检测到 Tools3000 进程正在运行，尝试发送优雅关闭信号 (CloseMainWindow)..." "WARN"
    foreach ($p in $runningProcesses) {
        try {
            $p.CloseMainWindow() | Out-Null
        } catch { }
    }
    
    # 等待最多 3 秒让其优雅退出
    $waited = 0
    while ((Get-Process -Name "Tools3000*" -ErrorAction SilentlyContinue) -and $waited -lt 3) {
        Start-Sleep -Seconds 1
        $waited++
    }
    
    $remaining = Get-Process -Name "Tools3000*" -ErrorAction SilentlyContinue
    if ($remaining) {
        Write-Log "进程未在规定时间内退出，执行强制关闭 (Force Stop)..." "WARN"
        foreach ($p in $remaining) {
            try {
                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            } catch { }
        }
        Start-Sleep -Seconds 1
    }
}

$BackupDir = Join-Path $ScriptDir "deploy_dist_backup"
taskkill /F /T /IM Tools3000.exe 2>$null | Out-Null
taskkill /F /T /IM Tools3000_Service.exe 2>$null | Out-Null
Get-Process -Name "Tools3000*", "Tools3000_Service*" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600
if (Test-Path $DeployDir) {
    if (Test-Path $BackupDir) {
        Remove-Item -Recurse -Force $BackupDir -ErrorAction SilentlyContinue
    }
    $renamed = $false
    for ($i = 0; $i -lt 5; $i++) {
        try {
            Rename-Item -Path $DeployDir -NewName "deploy_dist_backup" -ErrorAction Stop
            $renamed = $true
            break
        } catch {
            Start-Sleep -Milliseconds 600
        }
    }
    if ($renamed) {
        Rename-Item -Path $StagingDir -NewName "deploy_dist"
        Write-Log "旧版本已安全备份到: deploy_dist_backup"
    } else {
        # 若 Windows 锁住父级目录名，则执行安全原子覆盖
        Write-Log "目录重命名受阻，执行原子文件集快速覆盖..." "WARN"
        Copy-Item -Path "$StagingDir\*" -Destination $DeployDir -Recurse -Force
        Remove-Item -Recurse -Force $StagingDir -ErrorAction SilentlyContinue
    }
} else {
    Rename-Item -Path $StagingDir -NewName "deploy_dist"
}
Write-Log "新版本秒级切换上线完成。" "SUCCESS"

# ------------------------------------------------------------------------------
# 7. 生成 Windows 安装程序 (Inno Setup)
# ------------------------------------------------------------------------------
Write-Log "检查是否可以生成安装程序 (Inno Setup)..."
$InnoSetupDirs = @(
    "C:\Program Files (x86)\Inno Setup 6",
    "C:\Program Files\Inno Setup 6",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6"
)
$ISCC = $null
if (Get-Command "ISCC.exe" -ErrorAction SilentlyContinue) {
    $ISCC = (Get-Command "ISCC.exe").Source
} else {
    foreach ($dir in $InnoSetupDirs) {
        if (Test-Path "$dir\ISCC.exe") {
            $ISCC = "$dir\ISCC.exe"
            break
        }
    }
}

if ($SkipInstaller) {
    Write-Log "已指定 -SkipInstaller，跳过安装包生成。" "WARN"
} elseif ($ISCC) {
    Write-Log "找到 Inno Setup 编译器: $ISCC"
    $InstallerScript = Join-Path $ScriptDir "installer.iss"
    $OutputInstallerDir = Join-Path $ScriptDir "Output"
    
    if (Test-Path $InstallerScript) {
        $SetupBaseName = if ($Arch -eq "arm64") { "Tools3000-Setup-arm64" } else { "Tools3000-Setup" }
        Write-Log "正在编译安装包 ($SetupBaseName.exe)..."
        $TargetSetupFile = Join-Path $OutputInstallerDir "$SetupBaseName.exe"
        if (Test-Path $TargetSetupFile) {
            Remove-Item -Force $TargetSetupFile -ErrorAction SilentlyContinue
        }
        $InnoArgs = @(
            "/DTools3000Version=$ProjectVersion",
            "/DTools3000Architecture=$Arch",
            "/DTools3000SetupBaseFilename=$SetupBaseName"
        )
        if ($Quick) {
            $InnoArgs += "/DCompressionLevel=lzma2/fast"
        }
        $HasSigningIdentity = $env:TOOLS3000_SIGNING_CERT_SHA1 -or $env:TOOLS3000_SIGNING_PFX
        if ($HasSigningIdentity) {
            $InnoArgs += "/DTools3000SignedBuild=1"
            $InnoArgs += "/Stools3000=pwsh.exe -NoProfile -ExecutionPolicy Bypass -File $SignScript -Required -Paths `$f"
        }
        & $ISCC @InnoArgs $InstallerScript
        if ($LASTEXITCODE -eq 0) {
            if (Test-Path $TargetSetupFile) {
                $now = Get-Date
                (Get-Item $TargetSetupFile).CreationTime = $now
                (Get-Item $TargetSetupFile).LastWriteTime = $now
            }
            if ($HasSigningIdentity) {
                $SetupSignature = Get-AuthenticodeSignature -LiteralPath $TargetSetupFile
                if ($SetupSignature.Status -ne "Valid") {
                    throw "Inno Setup 生成的安装包签名验证失败: $($SetupSignature.Status)"
                }
            } else {
                & $SignScript -Paths $TargetSetupFile -Required:$RequireSigning
            }
            Write-Log "安装包已成功生成到: $OutputInstallerDir" "SUCCESS"
        } else {
            throw "安装包编译失败！退出码: $LASTEXITCODE"
        }
    } else {
        if ($RequireSigning) { throw "正式发布要求生成并签名安装包，但缺少安装脚本: $InstallerScript" }
        Write-Log "未找到安装脚本 $InstallerScript" "WARN"
    }
} else {
    if ($RequireSigning) { throw "正式发布要求生成并签名安装包，但未找到 Inno Setup 编译器。" }
    Write-Log "未找到 Inno Setup 编译器，跳过安装包生成步骤。" "WARN"
}

# ------------------------------------------------------------------------------
# 7. 全功能端到端生命周期与防死锁自动化审计门禁 (DevOps Lifecycle Gate)
# ------------------------------------------------------------------------------
if (-not $SkipTests) {
    $LifecycleScript = Join-Path $ScriptDir "scripts\verify_lifecycle.ps1"
    if (-not (Test-Path $LifecycleScript)) {
        $LifecycleScript = Join-Path $BuildDir "verify_lifecycle.ps1"
    }
    if (Test-Path $LifecycleScript) {
        Write-Log "[GATE] 正在执行全模块生命周期与防死锁自动化端到端审计 (verify_lifecycle.ps1)..." "INFO"
        & pwsh.exe -File $LifecycleScript
        if ($LASTEXITCODE -ne 0) {
            throw "关键生命周期自动化端到端门禁未通过！退出码: $LASTEXITCODE"
        }
        Write-Log "关键生命周期自动化门禁全部通过。" "SUCCESS"
    }

    $GestureE2EScript = Join-Path $ScriptDir "scripts\test_gesture_e2e.ps1"
    if (Test-Path $GestureE2EScript) {
        Write-Log "[GATE] 正在执行鼠标手势操作系统级端到端划动与自愈自动化审计 (test_gesture_e2e.ps1)..." "INFO"
        $TargetE2EExe = Join-Path $DeployDir "Tools3000.exe"

        & pwsh.exe -File $GestureE2EScript -ExePath $TargetE2EExe
        if ($LASTEXITCODE -ne 0) {
            throw "鼠标手势操作系统级端到端门禁未通过！退出码: $LASTEXITCODE"
        }
        Write-Log "鼠标手势操作系统级端到端门禁全部通过。" "SUCCESS"
    }

    $Stress1000HzScript = Join-Path $ScriptDir "scripts\stress_gesture_1000hz.ps1"
    if (Test-Path $Stress1000HzScript) {
        Write-Log "[GATE] 正在执行 1000Hz 鼠标手势与 DirectComposition 极限高压压力测试门禁 (stress_gesture_1000hz.ps1)..." "INFO"
        $TargetE2EExe = Join-Path $DeployDir "Tools3000.exe"

        & pwsh.exe -File $Stress1000HzScript -ExePath $TargetE2EExe
        if ($LASTEXITCODE -ne 0) {
            throw "1000Hz 鼠标手势与 DirectComposition 压力测试门禁未通过！退出码: $LASTEXITCODE"
        }
        Write-Log "1000Hz 鼠标手势与 DirectComposition 压力测试门禁全部通过。" "SUCCESS"
    }

    $AdversarialStressScript = Join-Path $ScriptDir "scripts\adversarial_stress_gesture.ps1"
    if (Test-Path $AdversarialStressScript) {
        Write-Log "[GATE] 正在执行鼠标手势对抗性高并发与防死锁压力测试门禁 (adversarial_stress_gesture.ps1)..." "INFO"
        $TargetE2EExe = Join-Path $DeployDir "Tools3000.exe"

        & pwsh.exe -File $AdversarialStressScript -ExePath $TargetE2EExe
        if ($LASTEXITCODE -ne 0) {
            throw "鼠标手势对抗性高并发与防死锁压力测试门禁未通过！退出码: $LASTEXITCODE"
        }
        Write-Log "鼠标手势对抗性高并发与防死锁压力测试门禁全部通过。" "SUCCESS"
    }

    $RecordingKeycastE2EScript = Join-Path $ScriptDir "scripts\test_recording_keycast_e2e.ps1"
    if (Test-Path $RecordingKeycastE2EScript) {
        Write-Log "[GATE] 正在执行录屏按键回显操作系统级端到端自动化审计 (test_recording_keycast_e2e.ps1)..." "INFO"
        $TargetE2EExe = Join-Path $DeployDir "Tools3000.exe"

        & pwsh.exe -File $RecordingKeycastE2EScript -ExePath $TargetE2EExe
        if ($LASTEXITCODE -ne 0) {
            throw "录屏按键回显操作系统级端到端门禁未通过！退出码: $LASTEXITCODE"
        }
        Write-Log "录屏按键回显操作系统级端到端门禁全部通过。" "SUCCESS"
    }
}

Write-Log "======================================================="
Write-Log "Tools3000 一键原子部署成功！" "SUCCESS"
Write-Log "您的纯净发布版位于: $DeployDir"
if (Test-Path (Join-Path $ScriptDir "Output\Tools3000-Setup.exe")) {
    Write-Log "您的安装包位于: $(Join-Path $ScriptDir "Output\Tools3000-Setup.exe")"
}
Write-Log "全链路 TraceID: $TraceID (详见 deploy_logs)"

# 新版、测试与安装包都已完成后才删除回滚副本。流程中途失败时保留该目录，
# 便于人工恢复；成功流程不在工作区遗留一整份过期发布物。
if (Test-Path $BackupDir) {
    Remove-Item -Recurse -Force $BackupDir
    Write-Log "旧版本回滚副本已清理。"
}
Write-Log "直接双击运行 deploy_dist/Tools3000.exe 即可启动工具。"
Write-Log "======================================================="

if ($Install) {
    Write-Log "正在执行自动化 CLI 安装..." "INFO"
    & pwsh.exe -File (Join-Path $ScriptDir "install.ps1") -Silent -Launch
    if ($LASTEXITCODE -ne 0) { throw "自动化 CLI 安装失败，退出码: $LASTEXITCODE" }
}


