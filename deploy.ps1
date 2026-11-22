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
    [switch]$Quick = $false,             # 极速增量开发模式 (复用 node_modules 与未改动的前端产物)
    [switch]$CleanBuild = $false,        # 强制全量冷建 (清理 build/、重新 npm ci、清空第三方 DLL 签名缓存)
    [switch]$SkipUI = $false,            # 跳过前端构建，直接复用 ui/dist/index.html 已有产物
    [switch]$SkipTests = $false,         # 跳过单元测试与端到端门禁
    [switch]$SkipInstaller = $false,     # 跳过 Inno Setup 安装包生成
    [switch]$Coverage = $false,          # 启用 C++ 代码覆盖率分析与防回退门禁 (OpenCppCoverage)
    [switch]$StaticAnalysis = $false,    # 对核心、搜索插件和索引服务运行 MSVC /analyze
    [switch]$Install = $false,           # 构建完成后立即通过 CLI 执行静默安装与启动
    [switch]$RequireSigning = $false,    # 正式发布门禁：所有自有二进制与安装包必须签名
    [switch]$RequirePublicTrust = $false,# 公网发布门禁：严禁自签名证书，必须由微软信任根 CA 签发（Azure Trusted Signing / EV / SignPath）
    [string]$BinaryCacheDir = ""         # 自定义 vcpkg 二进制包缓存目录
)

$ErrorActionPreference = "Stop"
$DeployTotalSw = [System.Diagnostics.Stopwatch]::StartNew()
$PhaseTimings = [System.Collections.Generic.List[object]]::new()
function Record-PhaseTiming {
    param([string]$PhaseName, [double]$Seconds)
    $PhaseTimings.Add([pscustomobject]@{ Phase = $PhaseName; Seconds = [math]::Round($Seconds, 2) })
}

function Get-StringSha256Hex {
    param([Parameter(Mandatory = $true)][string]$Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Start-BackgroundPwshTask {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDir
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Get-Command "pwsh.exe" -ErrorAction Stop).Source
    $psi.WorkingDirectory = $WorkingDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    $psi.CreateNoWindow = $true
    foreach ($arg in $Arguments) {
        [void]$psi.ArgumentList.Add($arg)
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = [System.Diagnostics.Process]::Start($psi)
    return [pscustomobject]@{
        Name    = $Name
        Proc    = $proc
        Sw      = $sw
        OutTask = $proc.StandardOutput.ReadToEndAsync()
        ErrTask = $proc.StandardError.ReadToEndAsync()
    }
}

function Complete-BackgroundPwshTask {
    param([Parameter(Mandatory = $true)]$Task)
    if (-not $Task) { return 0.0 }
    $Task.Proc.WaitForExit()
    if ($Task.Sw.IsRunning) { $Task.Sw.Stop() }
    $elapsedSec = $Task.Sw.Elapsed.TotalSeconds
    $outText = $Task.OutTask.Result
    $errText = $Task.ErrTask.Result
    Write-Log ("--- 并行轨道 [{0}] 输出汇总 ({1:N2}s, exit={2}) ---" -f $Task.Name, $elapsedSec, $Task.Proc.ExitCode)
    if (-not [string]::IsNullOrWhiteSpace($outText)) {
        [Console]::Out.WriteLine($outText.TrimEnd())
    }
    if (-not [string]::IsNullOrWhiteSpace($errText)) {
        [Console]::Error.WriteLine($errText.TrimEnd())
    }
    if ($Task.Proc.ExitCode -ne 0) {
        throw "并行轨道 [$($Task.Name)] 执行失败！退出码: $($Task.Proc.ExitCode)"
    }
    return $elapsedSec
}
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
# 1. 前端构建策略判定 (实际编译将与 Rust / C++ 三轨并发执行)
# ------------------------------------------------------------------------------
Write-Log "检查前端环境 (ui/)..."
$ShouldRunFrontendTrack = $false
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
    } elseif ($Quick -and -not $CleanBuild -and (Test-Path -LiteralPath $DistIndex)) {
        $DistTime = (Get-Item -LiteralPath $DistIndex).LastWriteTime
        $UiSourceFiles = @(Get-ChildItem -Path (Join-Path $ScriptDir "ui") -Recurse -File | Where-Object {
            $_.FullName -notlike "*\node_modules\*" -and
            $_.FullName -notlike "*\dist\*" -and
            $_.FullName -notlike "*\.git\*"
        })
        if (Test-Path -LiteralPath $VersionFile) {
            $UiSourceFiles += Get-Item -LiteralPath $VersionFile
        }
        $NewerSources = @($UiSourceFiles | Where-Object { $_.LastWriteTime -gt $DistTime })
        if ($NewerSources.Count -eq 0) {
            $SkipFrontend = $true
            Write-Log "[INFO] 极速模式: 前端源码与 VERSION 无变更，智能跳过前端构建并执行 C++ 静态规范门禁 ($DistIndex)" "SUCCESS"
            Push-Location ui
            try {
                foreach ($NativeGate in @("logger-check", "trim-workingset-check")) {
                    npm run $NativeGate
                    if ($LASTEXITCODE -ne 0) {
                        throw "C++ 静态规范门禁失败: npm run $NativeGate (退出码: $LASTEXITCODE)"
                    }
                }
            } finally {
                Pop-Location
            }
        } else {
            Write-Log "检测到 $($NewerSources.Count) 个前端源码或版本文件有更新，将在并行轨道执行前端构建..." "INFO"
        }
    }

    if (-not $SkipFrontend) {
        $ShouldRunFrontendTrack = $true
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
    $UserVcpkgRoot = Join-Path $LocalAppDataRoot "Tools3000-BuildTools\vcpkg"
    $LegacyUserVcpkgRoot = Join-Path $LocalAppDataRoot "Tools3000\vcpkg"
    $VcpkgCandidates = @($env:VCPKG_ROOT, "C:\vcpkg", $UserVcpkgRoot, $LegacyUserVcpkgRoot) |
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

$CMakeExtraArgs = @(
    "-DTOOLS3000_SKIP_POSTBUILD_UI_SYNC=ON"
)
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

function Assert-PeMachineArchitecture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][ValidateSet("x64", "arm64")][string]$ExpectedArch
    )
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        throw "PE 架构头校验失败，文件不存在: $FilePath"
    }
    $Fs = [System.IO.File]::OpenRead($FilePath)
    try {
        if ($Fs.Length -lt 0x40) {
            throw "PE 架构头校验失败，文件长度异常并非有效 PE 二进制: $FilePath"
        }
        $DosHeader = New-Object byte[] 0x40
        [void]$Fs.Read($DosHeader, 0, 0x40)
        if ($DosHeader[0] -ne 0x4D -or $DosHeader[1] -ne 0x5A) {
            throw "PE 架构头校验失败，缺少 MZ 魔数: $FilePath"
        }
        $PeOffset = [System.BitConverter]::ToInt32($DosHeader, 0x3C)
        if ($PeOffset -le 0 -or ($PeOffset + 6) -gt $Fs.Length) {
            throw "PE 架构头校验失败，PE 头偏移无效 ($PeOffset): $FilePath"
        }
        [void]$Fs.Seek($PeOffset, [System.IO.SeekOrigin]::Begin)
        $PeSigAndMachine = New-Object byte[] 6
        [void]$Fs.Read($PeSigAndMachine, 0, 6)
        if ($PeSigAndMachine[0] -ne 0x50 -or $PeSigAndMachine[1] -ne 0x45 -or
            $PeSigAndMachine[2] -ne 0x00 -or $PeSigAndMachine[3] -ne 0x00) {
            throw "PE 架构头校验失败，缺少 PE 签名: $FilePath"
        }
        $ActualMachine = [System.BitConverter]::ToUInt16($PeSigAndMachine, 4)
        $ExpectedMachine = if ($ExpectedArch -eq "arm64") { [uint16]0xAA64 } else { [uint16]0x8664 }
        if ($ActualMachine -ne $ExpectedMachine) {
            throw ("PE Machine 架构不匹配！文件={0}，实际=0x{1:X4}，期望={2} (0x{3:X4})，严禁混装错架构二进制！" -f $FilePath, $ActualMachine, $ExpectedArch, $ExpectedMachine)
        }
        Write-Log ("PE Machine 架构头校验通过: {0} -> {1} (0x{2:X4})" -f (Split-Path -Leaf $FilePath), $ExpectedArch, $ActualMachine) "SUCCESS"
    } finally {
        $Fs.Dispose()
    }
}

$ViewerProjectDir = Join-Path $ScriptDir "src\viewer"
$ViewerExeName = "Tools3000_Picasa_Viewer.exe"
$CliProjectDir = Join-Path $ScriptDir "src\cli"
$CliExeName = "tools3000-cli.exe"
$ViewerRustTarget = if ($Arch -eq "arm64") { "aarch64-pc-windows-msvc" } else { "x86_64-pc-windows-msvc" }
$ViewerTargetBinPath = Join-Path $ViewerProjectDir "target\$ViewerRustTarget\release\$ViewerExeName"
$ViewerBinCandidates = @($ViewerTargetBinPath)
$CliTargetBinPath = Join-Path $CliProjectDir "target\$ViewerRustTarget\release\$CliExeName"
$CliBinCandidates = @($CliTargetBinPath)
if ($Arch -eq "x64" -and -not $RequireSigning -and -not $RequirePublicTrust) {
    $ViewerBinCandidates += (Join-Path $ViewerProjectDir "target\release\$ViewerExeName")
    $CliBinCandidates += (Join-Path $CliProjectDir "target\release\$CliExeName")
}

$CargoCmd = Get-Command "cargo" -ErrorAction SilentlyContinue
if (-not $CargoCmd -and (Test-Path (Join-Path $env:USERPROFILE ".cargo\bin\cargo.exe"))) {
    $CargoCmd = Get-Command (Join-Path $env:USERPROFILE ".cargo\bin\cargo.exe") -ErrorAction SilentlyContinue
}
if (-not $CargoCmd -and ($RequireSigning -or $RequirePublicTrust)) {
    throw "正式发布门禁 (-RequireSigning / -RequirePublicTrust) 要求必须具备 Rust cargo 工具链以现场编译最新 $ViewerExeName，严禁静默复用过期旧二进制！"
}

# ── 启动三轨并行编译引擎 (前端 UI || Rust 看图器 || C++ CMake 增量构建) ──────────
$ParallelBuildSw = [System.Diagnostics.Stopwatch]::StartNew()
$FrontendTrackTask = $null
$RustTrackTask = $null

try {
    if ($ShouldRunFrontendTrack) {
        $FrontendScript = Join-Path $ScriptDir "scripts\Build-FrontendTrack.ps1"
        $FrontendArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $FrontendScript, "-UiDir", (Join-Path $ScriptDir "ui"))
        if ($Quick) { $FrontendArgs += "-Quick" }
        if ($CleanBuild) { $FrontendArgs += "-CleanBuild" }
        Write-Log "[PARALLEL] 启动轨道 1/3 (后台): 前端 UI 全量门禁 + Vitest + Vite 打包..." "INFO"
        $FrontendTrackTask = Start-BackgroundPwshTask -Name "Frontend-UI" -Arguments $FrontendArgs -WorkingDir $ScriptDir
    }

    if ($CargoCmd) {
        $RustScript = Join-Path $ScriptDir "scripts\Build-RustViewerTrack.ps1"
        $RustArgs = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $RustScript,
            "-ViewerProjectDir", $ViewerProjectDir,
            "-CliProjectDir", $CliProjectDir,
            "-Arch", $Arch,
            "-CargoPath", $CargoCmd.Source
        )
        if ($SkipTests) { $RustArgs += "-SkipTests" }
        Write-Log "[PARALLEL] 启动轨道 2/3 (后台): Rust Picasa 看图器单元测试 + Release 编译..." "INFO"
        $RustTrackTask = Start-BackgroundPwshTask -Name "Rust-PicasaViewer" -Arguments $RustArgs -WorkingDir $ScriptDir
    } else {
        Write-Log "未检测到 cargo 工具链，开发构建尝试复用已有预编译产物: $ViewerExeName" "WARN"
    }

    Write-Log "[PARALLEL] 启动轨道 3/3 (主线): C++ CMake 增量构建 ($Configuration, $Arch)..." "INFO"
    $CppSw = [System.Diagnostics.Stopwatch]::StartNew()
    $BuildDir = Join-Path $ScriptDir $BuildDirectoryName
    if ($CleanBuild -and (Test-Path -LiteralPath $BuildDir)) {
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
        Write-Log "已指定 -CleanBuild，使用全新构建树清理: $ResolvedBuildDir"
        Remove-Item -LiteralPath $ResolvedBuildDir -Recurse -Force
    }
    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir | Out-Null
    }

    $CMakeCachePath = Join-Path $BuildDir "CMakeCache.txt"
    $VcpkgInstalledDir = Join-Path $ScriptDir "vcpkg_installed"
    $ConfigureStampPath = Join-Path $BuildDir ".tools3000_configure_stamp.sha256"
    $CmakeListsHash = (Get-FileHash -LiteralPath (Join-Path $ScriptDir "CMakeLists.txt") -Algorithm SHA256).Hash
    $VcpkgJsonHash = (Get-FileHash -LiteralPath (Join-Path $ScriptDir "vcpkg.json") -Algorithm SHA256).Hash
    $SrcFileInventory = @(
        Get-ChildItem -Path (Join-Path $ScriptDir "src") -Include "*.cpp","*.h","*.in","*.rc" -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notlike "*\src\viewer\*" -and $_.FullName -notlike "*\src\cli\*" } |
            ForEach-Object { $_.FullName.Substring($ScriptDir.Length) }
    ) + @(
        Get-ChildItem -Path (Join-Path $ScriptDir "plugins") -Filter "*.in" -File -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName.Substring($ScriptDir.Length) }
    ) | Sort-Object
    $ConfigureFingerprint = Get-StringSha256Hex -Text (
        "v2|$ProjectVersion|$Arch|$StaticAnalysis|$VcpkgToolchain|$VcpkgInstalledDir|$($CMakeExtraArgs -join ';')|$CmakeListsHash|$VcpkgJsonHash|" + ($SrcFileInventory -join "`n")
    )
    $SavedConfigureFingerprint = if (Test-Path -LiteralPath $ConfigureStampPath) { (Get-Content -LiteralPath $ConfigureStampPath -Raw).Trim() } else { "" }
    $GeneratedVersionH = Join-Path $BuildDir "generated\Tools3000Version.h"
    $GeneratedVersionRc = Join-Path $BuildDir "generated\Tools3000_version.rc"
    $GeneratedVersionMatched = (Test-Path -LiteralPath $GeneratedVersionH) -and (Test-Path -LiteralPath $GeneratedVersionRc) -and
        ((Get-Content -LiteralPath $GeneratedVersionH -Raw -ErrorAction SilentlyContinue) -match ("String\[\]\s*=\s*`"" + [regex]::Escape($ProjectVersion) + "`"")) -and
        ((Get-Content -LiteralPath $GeneratedVersionRc -Raw -ErrorAction SilentlyContinue) -match ("ProductVersion`",\s*`"" + [regex]::Escape($ProjectVersion) + "\.0\\0`""))

    if ($CleanBuild -or -not (Test-Path -LiteralPath $CMakeCachePath) -or ($SavedConfigureFingerprint -ne $ConfigureFingerprint) -or -not $GeneratedVersionMatched) {
        Write-Log "执行 CMake Configure (检测到配置/源码清单/生成版本资源变化或首次构建)..."
        $CMakePlatformArgs = @()
        if ($Arch -eq "arm64") {
            $CMakePlatformArgs = @("-A", "ARM64", "-G", $Arm64Toolchain.Generator,
                "-DCMAKE_GENERATOR_INSTANCE=$($Arm64Toolchain.InstallationPath)")
        } elseif (-not (Test-Path -LiteralPath $CMakeCachePath)) {
            $CMakePlatformArgs = @("-A", "x64")
        }
        cmake -B $BuildDir -S . @CMakePlatformArgs -DCMAKE_TOOLCHAIN_FILE="$VcpkgToolchain" `
            -DVCPKG_TARGET_TRIPLET="$VcpkgTargetTriplet" -DVCPKG_INSTALLED_DIR="$VcpkgInstalledDir" @CMakeExtraArgs
        if ($LASTEXITCODE -ne 0) {
            throw "CMake 配置失败！退出码: $LASTEXITCODE"
        }
        [System.IO.File]::WriteAllText($ConfigureStampPath, $ConfigureFingerprint, [System.Text.UTF8Encoding]::new($false))
    } else {
        Write-Log "[OK] CMake 配置/源码清单指纹未变化，0ms 复用增量构建树 ($BuildDir)" "SUCCESS"
    }

    # 确保全套多分辨率 Windows 图标与托盘图标存在，仅当版本/图标/rc 指纹变化或版本生成物重建时才清理 .res 强制重链
    $IconScript = "$ScriptDir\resources\build_master_production_icons.py"
    $AppIco = "$ScriptDir\resources\app.ico"
    $TrayIco = "$ScriptDir\resources\tray.ico"
    $TrayDarkIco = "$ScriptDir\resources\tray_dark.ico"
    $TrayColorIco = "$ScriptDir\resources\tray_color.ico"
    $AllIconsExist = (Test-Path $AppIco) -and (Test-Path $TrayIco) -and (Test-Path $TrayDarkIco) -and (Test-Path $TrayColorIco)
    $ScriptNewer = $AllIconsExist -and ((Get-Item $IconScript).LastWriteTime -gt (Get-Item $AppIco).LastWriteTime)
    if (-not $AllIconsExist -or ($ScriptNewer -and -not ($env:GITHUB_ACTIONS -eq 'true' -or $env:CI -eq 'true'))) {
        Write-Log "检测到图标未初始化或母版脚本已更新，调用母版管道生成 (resources/build_master_production_icons.py)..."
        python "$ScriptDir\resources\build_master_production_icons.py" 1
    }

    $ResStampPath = Join-Path $BuildDir ".tools3000_res_stamp.sha256"
    $ResInputs = @($VersionFile, $AppIco, $TrayIco, $TrayDarkIco, $TrayColorIco,
        (Join-Path $ScriptDir "src\Tools3000.rc"), (Join-Path $ScriptDir "src\VersionInfo.rc.in"), $GeneratedVersionRc)
    $ResHashParts = foreach ($rf in $ResInputs) {
        if (Test-Path -LiteralPath $rf) { (Get-FileHash -LiteralPath $rf -Algorithm SHA256).Hash } else { "MISSING" }
    }
    $CurrentResFingerprint = Get-StringSha256Hex -Text ($ResHashParts -join "|")
    $SavedResFingerprint = if (Test-Path -LiteralPath $ResStampPath) { (Get-Content -LiteralPath $ResStampPath -Raw).Trim() } else { "" }
    if ($CleanBuild -or -not $GeneratedVersionMatched -or $SavedResFingerprint -ne $CurrentResFingerprint) {
        Write-Log "检测到版本或图标资源变更，刷新 *.res 确保版本号与图标强制重新链接..."
        Get-ChildItem -Path $BuildDir -Filter "*.res" -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
        [System.IO.File]::WriteAllText($ResStampPath, $CurrentResFingerprint, [System.Text.UTF8Encoding]::new($false))
    }

    $CpuCount = [Environment]::ProcessorCount
    Write-Log "执行 CMake Build ($Configuration, 并发核心数: $CpuCount)..."
    cmake --build $BuildDir --config $Configuration --parallel $CpuCount
    if ($LASTEXITCODE -ne 0) {
        throw "C++ 编译失败！退出码: $LASTEXITCODE"
    }
    $CppSw.Stop()
    Write-Log ("C++ 增量编译完成 ({0:N2}s)。" -f $CppSw.Elapsed.TotalSeconds) "SUCCESS"

    # ── 三轨汇合点 (Join) ────────────────────────────────────────────────────
    if ($RustTrackTask) {
        $RustSec = Complete-BackgroundPwshTask -Task $RustTrackTask
        $RustTrackTask = $null
        Write-Log ("Rust Picasa 看图器轨道汇合完成 ({0:N2}s)。" -f $RustSec) "SUCCESS"
    }
    if ($FrontendTrackTask) {
        $UiSec = Complete-BackgroundPwshTask -Task $FrontendTrackTask
        $FrontendTrackTask = $null
        Write-Log ("前端 UI 轨道汇合完成 ({0:N2}s)。" -f $UiSec) "SUCCESS"
    }
} finally {
    # 任一主线异常退出时确保彻底回收未结束的后台编译子进程，零僵尸残留
    foreach ($bgTask in @($FrontendTrackTask, $RustTrackTask)) {
        if ($bgTask -and $bgTask.Proc -and -not $bgTask.Proc.HasExited) {
            try { $bgTask.Proc.Kill($true) } catch {}
        }
    }
}

$ParallelBuildSw.Stop()
Record-PhaseTiming -PhaseName "Parallel Build (UI || Rust || C++)" -Seconds $ParallelBuildSw.Elapsed.TotalSeconds

# 三轨汇合后无竞争同步最新 ui/dist 到 build/bin/$Configuration/ui（保障直接运行 build 输出亦为最新前端）
$BuiltExeCheck = Join-Path $BuildDir "bin\$Configuration\Tools3000.exe"
if (-not (Test-Path -LiteralPath $BuiltExeCheck)) {
    $BuiltExeCheck = Join-Path $BuildDir "bin\Tools3000.exe"
}
if ((Test-Path -LiteralPath (Join-Path $ScriptDir "ui\dist")) -and (Test-Path -LiteralPath $BuiltExeCheck)) {
    $BuildUiSyncDir = Join-Path (Split-Path -Parent $BuiltExeCheck) "ui"
    if (Test-Path -LiteralPath $BuildUiSyncDir) {
        Remove-Item -LiteralPath $BuildUiSyncDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    New-Item -ItemType Directory -Path $BuildUiSyncDir -Force | Out-Null
    Copy-Item -Path (Join-Path $ScriptDir "ui\dist\*") -Destination $BuildUiSyncDir -Recurse -Force
}

$BuiltViewerExe = $ViewerBinCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $BuiltViewerExe) {
    throw "找不到编译后的 $ViewerExeName (架构: $Arch, 目标: $ViewerRustTarget)。请安装 Rust 工具链 (cargo) 后重试。"
}
Assert-PeMachineArchitecture -FilePath $BuiltViewerExe -ExpectedArch $Arch

$BuiltCliExe = $CliBinCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $BuiltCliExe) {
    throw "找不到编译后的 $CliExeName (架构: $Arch, 目标: $ViewerRustTarget)。请安装 Rust 工具链 (cargo) 后重试。"
}
Assert-PeMachineArchitecture -FilePath $BuiltCliExe -ExpectedArch $Arch

# ------------------------------------------------------------------------------
# 4.5 运行单元测试与代码覆盖率防回退分析 (失败则中断流水线)
# ------------------------------------------------------------------------------
if (-not $SkipTests) {
    $UnitTestSw = [System.Diagnostics.Stopwatch]::StartNew()
    $OpenCppCoverageExe = $null
    if (Get-Command "OpenCppCoverage" -ErrorAction SilentlyContinue) {
        $OpenCppCoverageExe = "OpenCppCoverage"
    } elseif (Test-Path "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe") {
        $OpenCppCoverageExe = "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe"
    }
    if ($Coverage -and -not $OpenCppCoverageExe) {
        throw "已要求覆盖率门禁，但找不到 OpenCppCoverage；请安装后重试。"
    }

    $TestExe = Join-Path $BuildDir "bin\$Configuration\Tools3000Tests.exe"
    if (-not (Test-Path $TestExe)) { $TestExe = Join-Path $BuildDir "bin\Tools3000Tests.exe" }

    if ($Coverage) {
        Write-Log "运行 C++ 单元测试与代码覆盖率防回退分析..."
        $CoverageReportDir = Join-Path $ScriptDir "coverage_report\$TraceID"
        if (-not (Test-Path $CoverageReportDir)) { New-Item -ItemType Directory -Path $CoverageReportDir | Out-Null }

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
        $ParallelTestRunner = Join-Path $ScriptDir "scripts\Invoke-ParallelNativeTests.ps1"
        if ((Test-Path -LiteralPath $ParallelTestRunner) -and (Test-Path -LiteralPath $TestExe)) {
            Write-Log "运行 4 路沙箱隔离并发 C++ 单元测试套件..."
            & pwsh -NoProfile -ExecutionPolicy Bypass -File $ParallelTestRunner -TestExePath $TestExe -TraceId $TraceID
            if ($LASTEXITCODE -ne 0) {
                throw "测试失败！退出码: $LASTEXITCODE"
            }
        } else {
            Write-Log "运行 CTest 测试套件..."
            ctest --test-dir $BuildDir -C $Configuration --output-on-failure
            if ($LASTEXITCODE -ne 0) {
                throw "测试失败！退出码: $LASTEXITCODE"
            }
        }
    }
    $UnitTestSw.Stop()
    Record-PhaseTiming -PhaseName "Native C++ Unit Tests (All)" -Seconds $UnitTestSw.Elapsed.TotalSeconds
    Write-Log ("测试套件全部通过 ({0:N2}s)。" -f $UnitTestSw.Elapsed.TotalSeconds) "SUCCESS"
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

# 符号隔离归档 (后台 .NET ZipFile Fastest 并发归档，边际壁钟耗时 = 0s)
if (-not (Test-Path $SymbolsDir)) {
    New-Item -ItemType Directory -Path $SymbolsDir | Out-Null
}
$FastZipScript = Join-Path $ScriptDir "scripts\New-FastZipArchive.ps1"
$SymbolsZip = Join-Path $SymbolsDir "Tools3000-Symbols-v$ProjectVersion.zip"
$PdbFiles = @(Get-ChildItem -Path (Join-Path $BuildDir "bin") -Filter "*.pdb" -Recurse -ErrorAction SilentlyContinue)
$PdbArchiveTask = $null
if ($PdbFiles.Count -gt 0) {
    Write-Log "[PARALLEL] 后台启动 $($PdbFiles.Count) 个调试符号 (*.pdb) 高速归档 -> $SymbolsZip..."
    $pdbFilesJoined = ($PdbFiles | Select-Object -ExpandProperty FullName) -join ';'
    $pdbZipArgs = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $FastZipScript,
        "-DestinationPath", $SymbolsZip,
        "-CompressionLevel", "Fastest",
        "-SourceFilesList", $pdbFilesJoined
    )
    $PdbArchiveTask = Start-BackgroundPwshTask -Name "Symbols-PDB-Zip" -Arguments $pdbZipArgs -WorkingDir $ScriptDir
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
    
    # 全仓 0 引用冗余 DLL 黑名单（经全量 PE 导入/延迟导入表审计确认 0 依赖；WebView2 已静态链接 WebView2LoaderStatic.lib）
    $ObsoleteUnusedDlls = @(
        "WebView2Loader.dll",
        "vccorlib140.dll",
        "msvcp140_1.dll",
        "msvcp140_2.dll",
        "msvcp140_codecvt_ids.dll",
        "vcruntime140_threads.dll"
    )

    # 复制所有同一目录下的运行库 DLL 文件 (显式排除 *gtest*.dll, *gmock*.dll, Plugin_*.dll 及 0 引用冗余 DLL)
    $DllFiles = Get-ChildItem -Path $ExeDir -Filter "*.dll" -File | Where-Object {
        $_.Name -notlike "*gtest*.dll" -and $_.Name -notlike "*gmock*.dll" -and $_.Name -notlike "Plugin_*.dll" -and
        ($ObsoleteUnusedDlls -notcontains $_.Name)
    }
    foreach ($dll in $DllFiles) {
        Copy-Item $dll.FullName -Destination $StagingDir
    }
    Write-Log "已复制生产运行库 DLL 文件 (已排除测试库 *gtest*/*gmock*、插件本体及 0 引用冗余 DLL)"
    
    # 复制插件目录 (严格仅复制 Plugin_*.dll 与 *.plugin.json，严禁复制 pdb 及重复三方库)
    $PluginsDir = Join-Path $BuildDir "bin\plugins\$Configuration"
    if (Test-Path $PluginsDir) {
        $TargetPluginsDir = Join-Path $StagingDir "plugins"
        New-Item -ItemType Directory -Path $TargetPluginsDir -ErrorAction SilentlyContinue | Out-Null
        $PluginFiles = Get-ChildItem -Path $PluginsDir -File | Where-Object {
            $_.Name -like "Plugin_*.dll" -or $_.Name -like "*.plugin.json"
        }
        foreach ($pf in $PluginFiles) {
            Copy-Item $pf.FullName -Destination $TargetPluginsDir
        }
        Write-Log "已复制纯净插件集合 (plugins/)，仅含 Plugin_*.dll 与 *.plugin.json"

        # 补充收集插件所需但主程序未直接引用的第三方运行库 (如 opencv_photo4.dll 等) 到根运行目录 (唯一保留一份)
        $PluginDependencyDlls = Get-ChildItem -Path $PluginsDir -Filter "*.dll" -File | Where-Object {
            $_.Name -notlike "Plugin_*.dll" -and $_.Name -notlike "*gtest*.dll" -and $_.Name -notlike "*gmock*.dll" -and
            ($ObsoleteUnusedDlls -notcontains $_.Name)
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

    # 递归补齐 /DELAYLOAD 延迟加载 DLL 及其传递依赖 (防止 vcpkg applocal 漏拷贝)
    $DelaySyncScript = Join-Path $ScriptDir "scripts\sync-delayed-dlls.ps1"
    if (Test-Path -LiteralPath $DelaySyncScript) {
        $VcpkgSubBin = if ($Configuration -eq "Debug") { "debug\bin" } else { "bin" }
        $SearchPool = @(
            (Join-Path $ScriptDir "vcpkg_installed\$VcpkgTargetTriplet\$VcpkgSubBin"),
            (Join-Path $BuildDir "vcpkg_installed\$VcpkgTargetTriplet\$VcpkgSubBin"),
            $ExeDir,
            $PluginsDir
        )
        & $DelaySyncScript -TargetDir $StagingDir -SearchDirs ($SearchPool -join ';')
    }
    $StagedViewerExe = Join-Path $StagingDir $ViewerExeName
    Copy-Item -LiteralPath $BuiltViewerExe -Destination $StagedViewerExe -Force
    Assert-PeMachineArchitecture -FilePath $StagedViewerExe -ExpectedArch $Arch
    Write-Log "已复制并校验 Rust Picasa 图片查看器: $ViewerExeName"

    $StagedCliExe = Join-Path $StagingDir $CliExeName
    Copy-Item -LiteralPath $BuiltCliExe -Destination $StagedCliExe -Force
    Assert-PeMachineArchitecture -FilePath $StagedCliExe -ExpectedArch $Arch
    Write-Log "已复制并校验 Rust CLI / MCP 工具: $CliExeName"
} else {
    throw "找不到编译后的 Tools3000.exe"
}

# MSVC 动态运行库。Tools3000 与 vcpkg 动态 triplet 依赖均使用动态 CRT；
# 便携包不能假设目标机器预装了 Visual C++ Redistributable。优先使用当前
# DevShell 精确对应的 Redist 目录，CI 中再通过 vswhere 定位同一工具链。
# 严格按实测引用白名单拷贝，剔除 0 引用的 vccorlib140 / msvcp140_1 / msvcp140_2 / msvcp140_codecvt_ids / vcruntime140_threads。
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
foreach ($VcDll in $VcRuntimeNames) {
    Copy-Item -LiteralPath (Join-Path $VcCrtDir $VcDll) -Destination $StagingDir -Force
}
Write-Log "已按白名单复制必需 Visual C++ Runtime ($($VcRuntimeNames -join ', ')): $VcCrtDir"

# WebView2 已通过 WebView2LoaderStatic.lib 静态链接入 Tools3000.exe，无需额外分发外部 WebView2Loader.dll。

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
if (Test-Path "resources\wallpapers") {
    Copy-Item "resources\wallpapers" -Destination $TargetResourcesDir -Recurse
}
if (Test-Path "resources\locales") {
    Copy-Item "resources\locales" -Destination $TargetResourcesDir -Recurse
}
Write-Log "已通过白名单注入运行必需资源 (ico, png, scripts/, wallpapers/, locales/)"

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
if (Test-Path "LICENSE-EXCEPTION.md") { Copy-Item "LICENSE-EXCEPTION.md" -Destination $StagingDir }
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
$ViewerExeArtifact = "Tools3000_Picasa_Viewer.exe"

$RequiredArtifacts = @(
    $MainExeArtifact, $ServiceExeArtifact, $ViewerExeArtifact, $CliExeName, "Tools3000Core.dll",
    "LICENSE", "LICENSE-EXCEPTION.md", "THIRD_PARTY_NOTICES.txt", "SBOM.spdx.json",
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
$HasAzureTrustedSigning =
    (-not [string]::IsNullOrWhiteSpace($env:TOOLS3000_AZURE_SIGNING_ENDPOINT)) -and
    (-not [string]::IsNullOrWhiteSpace($env:TOOLS3000_AZURE_SIGNING_ACCOUNT)) -and
    (-not [string]::IsNullOrWhiteSpace($env:TOOLS3000_AZURE_SIGNING_PROFILE))
if ($HasAzureTrustedSigning) {
    Write-Log "[SIGN] 已启用微软 Azure Trusted Signing 云端可信签名: Account=$($env:TOOLS3000_AZURE_SIGNING_ACCOUNT), Profile=$($env:TOOLS3000_AZURE_SIGNING_PROFILE)" "SUCCESS"
}
if (-not $HasAzureTrustedSigning -and
    [string]::IsNullOrWhiteSpace($env:TOOLS3000_SIGNING_CERT_SHA1) -and
    [string]::IsNullOrWhiteSpace($env:TOOLS3000_SIGNING_PFX) -and
    $env:CI -ne 'true' -and $env:GITHUB_ACTIONS -ne 'true') {
    $Now = Get-Date
    $AutoCert = @(Get-ChildItem -Path Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue | Where-Object {
        if (-not $_.HasPrivateKey -or $_.NotBefore -gt $Now -or $_.NotAfter -le $Now) { return $false }
        $Chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
        try { return $Chain.Build($_) } finally { $Chain.Dispose() }
    } | Sort-Object `
        @{ Expression = { if ($_.Subject -ne $_.Issuer) { 0 } else { 1 } }; Ascending = $true }, `
        @{ Expression = { if ($_.Subject -match 'Tools3000|yuan278501381|EasyTools') { 0 } else { 1 } }; Ascending = $true }, `
        @{ Expression = { $_.NotAfter }; Descending = $true }) | Select-Object -First 1
    if ($AutoCert) {
        $env:TOOLS3000_SIGNING_CERT_SHA1 = $AutoCert.Thumbprint
        Write-Log "[SIGN] 自动发现并挂载本机 CurrentUser\My 受信代码签名证书: $($AutoCert.Subject) ($($AutoCert.Thumbprint))" "SUCCESS"
    }
}
$HasSigningIdentity = $HasAzureTrustedSigning -or
                      (-not [string]::IsNullOrWhiteSpace($env:TOOLS3000_SIGNING_CERT_SHA1)) -or
                      (-not [string]::IsNullOrWhiteSpace($env:TOOLS3000_SIGNING_PFX))
$SignSw = [System.Diagnostics.Stopwatch]::StartNew()
$OwnedArtifacts = @(
    (Join-Path $StagingDir $MainExeArtifact),
    (Join-Path $StagingDir $ServiceExeArtifact),
    (Join-Path $StagingDir $ViewerExeArtifact),
    (Join-Path $StagingDir $CliExeName),
    (Join-Path $StagingDir "Tools3000Core.dll")
) + @(Get-ChildItem -LiteralPath (Join-Path $StagingDir "plugins") -Filter "Plugin_*.dll" -File |
    Select-Object -ExpandProperty FullName)

$FirstPassSignTargets = [System.Collections.Generic.List[string]]::new()
foreach ($item in $OwnedArtifacts) {
    if (-not $FirstPassSignTargets.Contains($item)) {
        $FirstPassSignTargets.Add($item)
    }
}

# 本地非 CI 环境下为不可变的第三方 vcpkg/CRT DLL 启用确定性 (签名身份 + 源 SHA-256) 签名缓存；
# 所有 9 个自有构建产物永远强制现场重新签名，绝不走缓存。
$UseThirdPartySignCache = $HasSigningIdentity -and (-not $CleanBuild) -and ($env:CI -ne 'true') -and ($env:GITHUB_ACTIONS -ne 'true')
$ThirdPartySignCacheDir = $null
$PendingThirdPartyCacheWrites = [System.Collections.Generic.List[object]]::new()
$ThirdPartyCacheHits = 0

if ($UseThirdPartySignCache) {
    $IdentitySeed = "$($env:TOOLS3000_AZURE_SIGNING_ENDPOINT)|$($env:TOOLS3000_AZURE_SIGNING_ACCOUNT)|$($env:TOOLS3000_AZURE_SIGNING_PROFILE)|$($env:TOOLS3000_SIGNING_CERT_SHA1)|$($env:TOOLS3000_SIGNING_PFX)|$RequireSigning|$RequirePublicTrust"
    $IdentityKey = (Get-StringSha256Hex -Text $IdentitySeed).Substring(0, 16)
    $ThirdPartySignCacheDir = Join-Path $BuildDir ".signed_third_party_cache\$IdentityKey"
    if (-not (Test-Path -LiteralPath $ThirdPartySignCacheDir)) {
        New-Item -ItemType Directory -Path $ThirdPartySignCacheDir -Force | Out-Null
    }
} elseif ($CleanBuild) {
    $OldSignCacheRoot = Join-Path $BuildDir ".signed_third_party_cache"
    if (Test-Path -LiteralPath $OldSignCacheRoot) {
        Remove-Item -LiteralPath $OldSignCacheRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($HasSigningIdentity) {
    # 本地存在签名证书时，对暂存发布区内全部尚未具备有效官方签名的 .exe/.dll 一并完成第一轮签名
    $AllDistBinaries = Get-ChildItem -LiteralPath $StagingDir -Include "*.exe", "*.dll" -Recurse -File
    foreach ($bin in $AllDistBinaries) {
        if (-not $FirstPassSignTargets.Contains($bin.FullName)) {
            $existingSig = Get-AuthenticodeSignature -LiteralPath $bin.FullName -ErrorAction SilentlyContinue
            if (-not $existingSig -or $existingSig.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
                $reusedFromCache = $false
                if ($UseThirdPartySignCache -and $ThirdPartySignCacheDir) {
                    $origSha256 = (Get-FileHash -LiteralPath $bin.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                    $cachedSignedPath = Join-Path $ThirdPartySignCacheDir "${origSha256}_$($bin.Name)"
                    if (Test-Path -LiteralPath $cachedSignedPath -PathType Leaf) {
                        $cachedSig = Get-AuthenticodeSignature -LiteralPath $cachedSignedPath -ErrorAction SilentlyContinue
                        $tsOk = (-not $RequireSigning) -or ($null -ne $cachedSig.TimeStamperCertificate)
                        if ($cachedSig -and $cachedSig.Status -eq [System.Management.Automation.SignatureStatus]::Valid -and $tsOk) {
                            Copy-Item -LiteralPath $cachedSignedPath -Destination $bin.FullName -Force
                            $reusedFromCache = $true
                            $ThirdPartyCacheHits++
                        } else {
                            Remove-Item -LiteralPath $cachedSignedPath -Force -ErrorAction SilentlyContinue
                        }
                    }
                    if (-not $reusedFromCache) {
                        $PendingThirdPartyCacheWrites.Add([pscustomobject]@{
                            StagedPath = $bin.FullName
                            CachePath  = $cachedSignedPath
                        })
                    }
                }
                if (-not $reusedFromCache) {
                    $FirstPassSignTargets.Add($bin.FullName)
                }
            }
        }
    }
}
if ($ThirdPartyCacheHits -gt 0) {
    Write-Log "[SIGN] 第三方不变 DLL 确定性签名缓存命中 $ThirdPartyCacheHits 个，本轮需实时签名 $($FirstPassSignTargets.Count) 个二进制。" "SUCCESS"
}
& $SignScript -Paths $FirstPassSignTargets.ToArray() -Required:$RequireSigning -RequirePublicTrust:$RequirePublicTrust
if ($UseThirdPartySignCache -and $PendingThirdPartyCacheWrites.Count -gt 0) {
    foreach ($entry in $PendingThirdPartyCacheWrites) {
        if (Test-Path -LiteralPath $entry.StagedPath -PathType Leaf) {
            Copy-Item -LiteralPath $entry.StagedPath -Destination $entry.CachePath -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Log "[SIGN] 已回写 $($PendingThirdPartyCacheWrites.Count) 个第三方已签名 DLL 至确定性本地签名缓存。" "INFO"
}
$SignSw.Stop()
Record-PhaseTiming -PhaseName "Authenticode First-Pass Signing" -Seconds $SignSw.Elapsed.TotalSeconds

# ------------------------------------------------------------------------------
# 6. 优雅关闭及原子交换 (Atomic Swap)
# ------------------------------------------------------------------------------
Write-Log "开始执行原子目录交换..."

# 快速停止可能正在运行的 Tools3000 进程（仅有可见主窗时才尝试 CloseMainWindow，无主窗后台服务直接释放锁）
$runningProcesses = @(Get-Process -Name "Tools3000*" -ErrorAction SilentlyContinue)
if ($runningProcesses.Count -gt 0) {
    $guiProcs = @($runningProcesses | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero })
    if ($guiProcs.Count -gt 0) {
        Write-Log "检测到 $($guiProcs.Count) 个可见 Tools3000 窗口进程，发送优雅关闭信号..." "WARN"
        foreach ($p in $guiProcs) {
            try { $p.CloseMainWindow() | Out-Null } catch { }
        }
        $waitDeadline = [DateTime]::UtcNow.AddMilliseconds(1200)
        while ((Get-Process -Name "Tools3000*" -ErrorAction SilentlyContinue) -and [DateTime]::UtcNow -lt $waitDeadline) {
            Start-Sleep -Milliseconds 150
        }
    }
    $remaining = @(Get-Process -Name "Tools3000*" -ErrorAction SilentlyContinue)
    if ($remaining.Count -gt 0) {
        Write-Log "正在快速停止 $($remaining.Count) 个后台 Tools3000 进程以释放文件锁..." "WARN"
        foreach ($p in $remaining) {
            try { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch { }
        }
    }
}

$BackupDir = Join-Path $ScriptDir "deploy_dist_backup"
if (Get-Process -Name "Tools3000*", "Tools3000_Service*" -ErrorAction SilentlyContinue) {
    taskkill /F /T /IM Tools3000.exe 2>$null | Out-Null
    taskkill /F /T /IM Tools3000_Service.exe 2>$null | Out-Null
    taskkill /F /T /IM Tools3000_Picasa_Viewer.exe 2>$null | Out-Null
    taskkill /F /T /IM tools3000-cli.exe 2>$null | Out-Null
    Get-Process -Name "Tools3000*", "Tools3000_Service*" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 250
}
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
            Start-Sleep -Milliseconds 300
        }
    }
    if ($renamed) {
        Rename-Item -Path $StagingDir -NewName "deploy_dist"
        Write-Log "旧版本已安全备份到: deploy_dist_backup"
    } else {
        # 若 Windows 锁住父级目录名，则执行安全原子覆盖并主动清理历史遗留测试库与重复第三方 DLL
        Write-Log "目录重命名受阻，执行原子文件集快速覆盖与残留去污..." "WARN"
        Copy-Item -Path "$StagingDir\*" -Destination $DeployDir -Recurse -Force
        Remove-Item -Recurse -Force $StagingDir -ErrorAction SilentlyContinue
    }
} else {
    Rename-Item -Path $StagingDir -NewName "deploy_dist"
}

# 终局瘦身去污守卫：确保 deploy_dist 根目录无测试库/插件本体/0 引用僵尸 DLL，且 deploy_dist/plugins 严格仅保留 Plugin_*.dll 与 *.plugin.json
Get-ChildItem -LiteralPath $DeployDir -File -ErrorAction SilentlyContinue | Where-Object {
    $_.Name -like "*gtest*.dll" -or $_.Name -like "*gmock*.dll" -or $_.Name -like "Plugin_*.dll" -or $_.Name -like "Tools3000Tests*" -or $_.Extension -ieq ".pdb" -or
    ($ObsoleteUnusedDlls -contains $_.Name)
} | Remove-Item -Force -ErrorAction SilentlyContinue

$DeployPluginsDir = Join-Path $DeployDir "plugins"
if (Test-Path -LiteralPath $DeployPluginsDir) {
    Get-ChildItem -LiteralPath $DeployPluginsDir -File -ErrorAction SilentlyContinue | Where-Object {
        ($_.Extension -ieq ".dll" -and $_.Name -notlike "Plugin_*.dll") -or $_.Extension -ieq ".pdb"
    } | Remove-Item -Force -ErrorAction SilentlyContinue
}
Assert-PeMachineArchitecture -FilePath (Join-Path $DeployDir $ViewerExeArtifact) -ExpectedArch $Arch
Assert-PeMachineArchitecture -FilePath (Join-Path $DeployDir $CliExeName) -ExpectedArch $Arch
if ($RequireSigning -or $RequirePublicTrust) {
    & $SignScript -VerifyOnly -Required:$RequireSigning -RequirePublicTrust:$RequirePublicTrust -Paths @(
        (Join-Path $DeployDir $MainExeArtifact),
        (Join-Path $DeployDir $ServiceExeArtifact),
        (Join-Path $DeployDir $ViewerExeArtifact),
        (Join-Path $DeployDir $CliExeName),
        (Join-Path $DeployDir "Tools3000Core.dll")
    )
}
Write-Log "新版本秒级切换上线完成，发布目录已完成瘦身去污与架构/签名校验。" "SUCCESS"

# ------------------------------------------------------------------------------
# 7. 并行生成绿色便携包与 Windows 安装程序 (便携 ZIP || 多线程 Inno Setup 6)
# ------------------------------------------------------------------------------
$PackSw = [System.Diagnostics.Stopwatch]::StartNew()
$OutputInstallerDir = Join-Path $ScriptDir "Output"
if (-not (Test-Path -LiteralPath $OutputInstallerDir)) {
    New-Item -ItemType Directory -Path $OutputInstallerDir -Force | Out-Null
}
$PortableArchivePath = Join-Path $OutputInstallerDir "Tools3000-v$ProjectVersion-win-$Arch-portable.zip"
$SetupBaseName = if ($Arch -eq "arm64") { "Tools3000-v$ProjectVersion-Setup-arm64" } else { "Tools3000-v$ProjectVersion-Setup" }
$TargetSetupFile = Join-Path $OutputInstallerDir "$SetupBaseName.exe"
$PortableZipTask = $null

try {
    if (-not $SkipInstaller) {
        # 每次构建前清理 Output\ 中可能残留的旧版或旧别名打包产物，确保收口为唯一规范 2 件套
        Get-ChildItem -LiteralPath $OutputInstallerDir -File -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -like "Tools3000-*.exe" -or $_.Name -like "Tools3000-*.zip"
        } | Remove-Item -Force -ErrorAction SilentlyContinue
        Write-Log "已清理 Output\ 中的历史残留与旧别名产物 (Tools3000-*.exe / Tools3000-*.zip)"

        Write-Log "[PARALLEL] 后台启动绿色便携包 (.NET ZipFile Optimal) 打包 -> $PortableArchivePath..."
        $portableZipArgs = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $FastZipScript,
            "-DestinationPath", $PortableArchivePath,
            "-SourceDirectory", $DeployDir,
            "-CompressionLevel", "Optimal"
        )
        $PortableZipTask = Start-BackgroundPwshTask -Name "Portable-ZIP" -Arguments $portableZipArgs -WorkingDir $ScriptDir
    }

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
        
        if (Test-Path $InstallerScript) {
            $LzmaThreads = [Math]::Min([Math]::Max(2, [Environment]::ProcessorCount), 8)
            Write-Log "正在多线程编译安装包 ($SetupBaseName.exe, LZMA2 BlockThreads=$LzmaThreads)..."
            if (Test-Path -LiteralPath $TargetSetupFile) {
                Remove-Item -Force -LiteralPath $TargetSetupFile -ErrorAction SilentlyContinue
            }
            $InnoArgs = @(
                "/DTools3000Version=$ProjectVersion",
                "/DTools3000Architecture=$Arch",
                "/DTools3000SetupBaseFilename=$SetupBaseName",
                "/DDeployDir=$DeployDir",
                "/DLZMABlockThreads=$LzmaThreads"
            )
            if ($Quick) {
                $InnoArgs += "/DCompressionLevel=lzma2/fast"
            }
            if ($HasSigningIdentity) {
                $InnoSignRequiredFlag = if ($RequireSigning) { "-Required " } else { "" }
                $InnoSignPublicFlag = if ($RequirePublicTrust) { "-RequirePublicTrust " } else { "" }
                $InnoArgs += "/DTools3000SignedBuild=1"
                $InnoArgs += "/Stools3000=pwsh.exe -NoProfile -ExecutionPolicy Bypass -File `$q$SignScript`$q ${InnoSignRequiredFlag}${InnoSignPublicFlag}-Paths `$q`$f`$q"
            }
            & $ISCC @InnoArgs $InstallerScript
            if ($LASTEXITCODE -eq 0) {
                if ($HasSigningIdentity) {
                    $SetupSignature = Get-AuthenticodeSignature -LiteralPath $TargetSetupFile
                    if ($SetupSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or ($RequireSigning -and -not $SetupSignature.TimeStamperCertificate)) {
                        Write-Log "正在对生成的安装包执行外层 Authenticode 签名与时间戳加固: $TargetSetupFile"
                        & $SignScript -Paths $TargetSetupFile -Required:$RequireSigning -RequirePublicTrust:$RequirePublicTrust
                    } elseif ($RequirePublicTrust) {
                        & $SignScript -Paths $TargetSetupFile -VerifyOnly -Required:$RequireSigning -RequirePublicTrust
                    }
                    $SetupSignature = Get-AuthenticodeSignature -LiteralPath $TargetSetupFile
                    if ($SetupSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or ($RequireSigning -and -not $SetupSignature.TimeStamperCertificate)) {
                        throw "Inno Setup 生成的安装包外层签名或时间戳验证失败: $($SetupSignature.Status)"
                    }
                } else {
                    & $SignScript -Paths $TargetSetupFile -Required:$RequireSigning -RequirePublicTrust:$RequirePublicTrust
                }
                Write-Log "安装包已成功生成到: $TargetSetupFile" "SUCCESS"
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

    if ($PortableZipTask) {
        $PortableSec = Complete-BackgroundPwshTask -Task $PortableZipTask
        $PortableZipTask = $null
        Write-Log ("绿色便携包已生成: $PortableArchivePath ({0:N2}s)" -f $PortableSec) "SUCCESS"
    }
    if ($PdbArchiveTask) {
        $PdbSec = Complete-BackgroundPwshTask -Task $PdbArchiveTask
        $PdbArchiveTask = $null
        Write-Log ("调试符号已安全剥离并独立归档至 deploy_symbols/ ({0:N2}s)。" -f $PdbSec) "SUCCESS"
    }
} finally {
    foreach ($bgZip in @($PortableZipTask, $PdbArchiveTask)) {
        if ($bgZip -and $bgZip.Proc -and -not $bgZip.Proc.HasExited) {
            try { $bgZip.Proc.Kill($true) } catch {}
        }
    }
}
$PackSw.Stop()
Record-PhaseTiming -PhaseName "Packaging (Portable ZIP || Inno MT)" -Seconds $PackSw.Elapsed.TotalSeconds

# ------------------------------------------------------------------------------
# 8. 全功能端到端生命周期与防死锁自动化审计门禁 (DevOps Lifecycle Gate)
# ------------------------------------------------------------------------------
$E2eSw = [System.Diagnostics.Stopwatch]::StartNew()
$MpCmdRun = Join-Path $env:ProgramFiles "Windows Defender\MpCmdRun.exe"
if ((Test-Path -LiteralPath $MpCmdRun) -and $env:CI -ne 'true' -and $env:GITHUB_ACTIONS -ne 'true') {
    Write-Log "[GATE] 正在执行 Microsoft Defender 隔离沙盒静态防误报扫描门禁 (MpCmdRun)..." "INFO"
    $AvGateDir = Join-Path ([System.IO.Path]::GetTempPath()) "tools3000_av_gate_$TraceID"
    New-Item -ItemType Directory -Path $AvGateDir -Force | Out-Null
    try {
        foreach ($CoreBin in @("Tools3000.exe", "Tools3000_Service.exe", "Tools3000_Picasa_Viewer.exe", "Tools3000Core.dll")) {
            $SrcBin = Join-Path $DeployDir $CoreBin
            if (Test-Path -LiteralPath $SrcBin) {
                Copy-Item -LiteralPath $SrcBin -Destination (Join-Path $AvGateDir $CoreBin) -Force
            }
        }
        Get-ChildItem -LiteralPath (Join-Path $DeployDir "plugins") -Filter "Plugin_*.dll" -File -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $AvGateDir $_.Name) -Force
        }
        $AvScanOut = & $MpCmdRun -Scan -ScanType 3 -File $AvGateDir -DisableRemediation 2>&1
        $AvScanCode = $LASTEXITCODE
        if ($AvScanCode -ne 0) {
            throw "Microsoft Defender 防误报扫描门禁未通过 (退出码 $AvScanCode): $($AvScanOut -join ' ')"
        }
        Write-Log "[OK] Microsoft Defender 静态防误报扫描门禁全部通过 (0 威胁误报)。" "SUCCESS"
    } finally {
        Remove-Item -LiteralPath $AvGateDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

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

    $GestureSuiteE2EScript = Join-Path $ScriptDir "scripts\test_gesture_suite_e2e.ps1"
    if (Test-Path $GestureSuiteE2EScript) {
        Write-Log "[GATE] 正在执行鼠标手势合并版单次切屏 E2E / 1000Hz / 对抗极限总门禁 (test_gesture_suite_e2e.ps1)..." "INFO"
        $TargetE2EExe = Join-Path $DeployDir "Tools3000.exe"

        & pwsh.exe -File $GestureSuiteE2EScript -ExePath $TargetE2EExe
        if ($LASTEXITCODE -ne 0) {
            throw "鼠标手势合并版单次切屏总门禁未通过！退出码: $LASTEXITCODE"
        }
        Write-Log "鼠标手势合并版单次切屏 E2E / 1000Hz / 对抗极限总门禁全部通过。" "SUCCESS"
    } else {
        $GestureE2EScript = Join-Path $ScriptDir "scripts\test_gesture_e2e.ps1"
        if (Test-Path $GestureE2EScript) {
            Write-Log "[GATE] 正在执行鼠标手势操作系统级端到端划动与自愈自动化审计 (test_gesture_e2e.ps1)..." "INFO"
            $TargetE2EExe = Join-Path $DeployDir "Tools3000.exe"
            & pwsh.exe -File $GestureE2EScript -ExePath $TargetE2EExe
            if ($LASTEXITCODE -ne 0) { throw "鼠标手势操作系统级端到端门禁未通过！退出码: $LASTEXITCODE" }
        }
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

    $CaptureRecordingE2EScript = Join-Path $ScriptDir "scripts\test_capture_recording_e2e.ps1"
    if (Test-Path $CaptureRecordingE2EScript) {
        Write-Log "[GATE] 正在执行截图·贴图·录屏·倒计时·演播室全链路操作系统级端到端门禁 (test_capture_recording_e2e.ps1)..." "INFO"
        $TargetE2EExe = Join-Path $DeployDir "Tools3000.exe"

        & pwsh.exe -File $CaptureRecordingE2EScript -ExePath $TargetE2EExe
        if ($LASTEXITCODE -ne 0) {
            throw "截图与录屏全链路操作系统级端到端门禁未通过！退出码: $LASTEXITCODE"
        }
        Write-Log "截图·贴图·录屏·倒计时·演播室全链路操作系统级端到端门禁全部通过。" "SUCCESS"
    }

    $SearchContentE2EScript = Join-Path $ScriptDir "scripts\test_search_content_e2e.ps1"
    if (Test-Path $SearchContentE2EScript) {
        Write-Log "[GATE] 正在执行文件搜索与文档内容全文搜索跨进程端到端门禁 (test_search_content_e2e.ps1)..." "INFO"
        $TargetE2EExe = Join-Path $DeployDir "Tools3000.exe"

        & pwsh.exe -File $SearchContentE2EScript -ExePath $TargetE2EExe
        if ($LASTEXITCODE -ne 0) {
            throw "文件搜索与文档内容全文搜索端到端门禁未通过！退出码: $LASTEXITCODE"
        }
        Write-Log "文件搜索与文档内容全文搜索跨进程端到端门禁全部通过。" "SUCCESS"
    }
}
$E2eSw.Stop()
Record-PhaseTiming -PhaseName "Defender Scan + 5 E2E Gates" -Seconds $E2eSw.Elapsed.TotalSeconds

$DeployTotalSw.Stop()
Write-Log "======================================================="
Write-Log ("Tools3000 一键原子部署成功！ (总耗时: {0:N1}s)" -f $DeployTotalSw.Elapsed.TotalSeconds) "SUCCESS"
foreach ($pt in $PhaseTimings) {
    Write-Log ("  [TIMING] {0,-38}: {1,6:N2}s" -f $pt.Phase, $pt.Seconds) "INFO"
}
Write-Log "您的纯净发布版位于: $DeployDir"
if (Test-Path -LiteralPath $TargetSetupFile) {
    Write-Log "您的安装包位于: $TargetSetupFile"
}
if (Test-Path -LiteralPath $PortableArchivePath) {
    Write-Log "您的便携包位于: $PortableArchivePath"
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


