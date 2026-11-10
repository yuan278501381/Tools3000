#!/usr/bin/env pwsh
param(
    [ValidateSet("x64", "arm64")]
    [string]$Arch = "x64",

    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Config = "Release",

    [switch]$Package,
    [switch]$All,
    [switch]$SkipUI,
    [switch]$Clean,

    [string]$VcpkgRoot = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$UIDir = Join-Path $ProjectRoot "ui"
if ($Package -and $SkipUI) {
    throw "-Package 不能与 -SkipUI 同时使用；发布包必须包含本次构建生成的前端。"
}
$Arm64Toolchain = $null
if ($Arch -eq 'arm64' -or $All) {
    $Arm64Toolchain = & (Join-Path $PSScriptRoot 'Get-Arm64Toolchain.ps1')
}
$VersionFile = Join-Path $ProjectRoot "VERSION"
if (-not (Test-Path -LiteralPath $VersionFile)) {
    throw "缺少唯一版本源: $VersionFile"
}
$Version = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION 必须是稳定 SemVer（例如 1.2.3），当前值: $Version"
}
$RestoreWebView2Script = Join-Path $ProjectRoot "scripts\restore-webview2.ps1"
if (-not (Test-Path -LiteralPath $RestoreWebView2Script -PathType Leaf)) {
    throw "缺少 WebView2 SDK 恢复脚本: $RestoreWebView2Script"
}
$WebView2Sdk = & $RestoreWebView2Script -ProjectRoot $ProjectRoot
if (-not $WebView2Sdk -or -not (Test-Path -LiteralPath $WebView2Sdk.TargetDirectory)) {
    throw "WebView2 SDK 恢复后仍不可用。"
}
$VcpkgManifest = Get-Content -LiteralPath (Join-Path $ProjectRoot "vcpkg.json") -Raw |
    ConvertFrom-Json
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
if (-not (Get-Command "git" -ErrorAction SilentlyContinue)) {
    throw "找不到 Git；无法获取或校验固定版本的 vcpkg。"
}
$VcpkgToolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $VcpkgToolchain)) {
    if (Test-Path -LiteralPath $VcpkgRoot) {
        throw "vcpkg 目录已存在但不完整，拒绝覆盖: $VcpkgRoot"
    }
    $VcpkgParent = Split-Path -Parent $VcpkgRoot
    if (-not (Test-Path -LiteralPath $VcpkgParent)) {
        New-Item -ItemType Directory -Path $VcpkgParent -Force | Out-Null
    }
    Write-Host "未找到 vcpkg，正在检出固定提交 $VcpkgBaseline 到 $VcpkgRoot" -ForegroundColor Yellow
    git clone --no-checkout https://github.com/microsoft/vcpkg.git $VcpkgRoot
    if ($LASTEXITCODE -ne 0) { throw "克隆 vcpkg 失败" }
    git -C $VcpkgRoot checkout --detach $VcpkgBaseline
    if ($LASTEXITCODE -ne 0) { throw "检出固定 vcpkg 提交失败: $VcpkgBaseline" }
}
$VcpkgHeadOutput = git -C $VcpkgRoot rev-parse HEAD 2>$null
$VcpkgHead = if ($LASTEXITCODE -eq 0) { ([string]$VcpkgHeadOutput).Trim() } else { "" }
if ($VcpkgHead -ne $VcpkgBaseline) {
    throw "vcpkg 必须固定到清单 builtin-baseline $VcpkgBaseline，当前为 $VcpkgHead"
}
$VcpkgTrackedChanges = @(git -C $VcpkgRoot status --porcelain --untracked-files=no 2>$null)
if ($LASTEXITCODE -ne 0) { throw "无法检查 vcpkg 工作树状态: $VcpkgRoot" }
if ($VcpkgTrackedChanges.Count -gt 0) {
    throw "vcpkg 包含本地修改；构建必须使用无修改的固定提交 $VcpkgBaseline。"
}
$VcpkgBootstrap = Join-Path $VcpkgRoot "bootstrap-vcpkg.bat"
if (-not (Test-Path -LiteralPath $VcpkgBootstrap)) {
    throw "固定提交中缺少 vcpkg 引导脚本: $VcpkgBootstrap"
}
$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if ((Test-Path -LiteralPath $VcpkgExe) -and ($VcpkgHead -eq $VcpkgBaseline)) {
    Write-Host "[INFO] vcpkg.exe 已就绪且与基线 ($VcpkgBaseline) 一致，跳过重复 bootstrap。" -ForegroundColor Green
} else {
    Write-Host "已校验固定 vcpkg 源码，引导工具程序..." -ForegroundColor Cyan
    & $VcpkgBootstrap -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "bootstrap vcpkg 失败" }
}

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "  Tools3000 v$Version - Build Script" -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan

function Write-Step {
    param([string]$Message)
    Write-Host "`n[STEP] $Message" -ForegroundColor Green
}

function Write-Error-Exit {
    param([string]$Message)
    Write-Host "[ERROR] $Message" -ForegroundColor Red
    exit 1
}

function Remove-PackageDirectorySafely {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedParent,
        [Parameter(Mandatory = $true)][string]$ExpectedLeaf
    )

    if (-not (Test-Path -LiteralPath $Path)) { return }
    $ResolvedPath = [System.IO.Path]::GetFullPath($Path)
    $ResolvedParent = [System.IO.Path]::GetFullPath($ExpectedParent).TrimEnd('\')
    $ActualParent = [System.IO.Path]::GetDirectoryName($ResolvedPath).TrimEnd('\')
    if (-not $ActualParent.Equals($ResolvedParent, [System.StringComparison]::OrdinalIgnoreCase) -or
        -not (Split-Path -Leaf $ResolvedPath).Equals($ExpectedLeaf, [System.StringComparison]::Ordinal)) {
        Write-Error-Exit "拒绝清理未经验证的打包目录: $ResolvedPath"
    }
    $PackageItem = Get-Item -LiteralPath $ResolvedPath -Force
    if (($PackageItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        Write-Error-Exit "拒绝递归清理重解析点打包目录: $ResolvedPath"
    }
    Remove-Item -LiteralPath $ResolvedPath -Recurse -Force
}

function Invoke-BuildArch {
    param(
        [string]$TargetArch,
        [string]$BuildConfig
    )

    $BuildDirName = if ($TargetArch -eq "x64") { "build" } else { "build-$TargetArch" }
    $BuildDir = Join-Path $ProjectRoot $BuildDirName

    if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
        $ResolvedBuildDir = [System.IO.Path]::GetFullPath($BuildDir)
        $WorkspacePrefix = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\') + '\'
        if (-not $ResolvedBuildDir.StartsWith($WorkspacePrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $ResolvedBuildDir) -notin @("build", "build-arm64")) {
            Write-Error-Exit "拒绝清理未经验证的构建目录: $ResolvedBuildDir"
        }
        $BuildItem = Get-Item -LiteralPath $ResolvedBuildDir -Force
        if (($BuildItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            Write-Error-Exit "拒绝清理重解析点构建目录: $ResolvedBuildDir"
        }
        Write-Step "清理构建目录: $BuildDir"
        Remove-Item -LiteralPath $ResolvedBuildDir -Recurse -Force
    }

    $CMakeCommand = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
    $cmakeExe = if ($CMakeCommand) { $CMakeCommand.Source } else { $null }
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $VsInstallationPath = $null
    if (-not $cmakeExe -and (Test-Path -LiteralPath $VsWhere)) {
        $VsInstallationPath = & $VsWhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
        if ($LASTEXITCODE -eq 0 -and $VsInstallationPath) {
            $VsCMake = Join-Path ([string]$VsInstallationPath) `
                "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path -LiteralPath $VsCMake) { $cmakeExe = $VsCMake }
        }
    }
    if (-not $cmakeExe) {
        $CMakeCandidates = @(
            "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
            "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
            "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
            "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        )
        $cmakeExe = $CMakeCandidates | Where-Object { Test-Path -LiteralPath $_ } |
            Select-Object -First 1
    }
    if (-not $cmakeExe) {
        Write-Error-Exit "找不到 CMake。请安装 Visual Studio C++ 桌面开发工作负载。"
    }

    $VcpkgToolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path -LiteralPath $VcpkgToolchain)) {
        Write-Error-Exit "找不到 vcpkg toolchain: $VcpkgToolchain"
    }

    Write-Step "CMake Configure [$TargetArch / $BuildConfig]"
    $VcpkgInstalledDir = Join-Path $ProjectRoot "vcpkg_installed"
    $ConfigureArgs = @(
        "-S", $ProjectRoot,
        "-B", $BuildDir,
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain",
        "-DVCPKG_TARGET_TRIPLET=$TargetArch-windows",
        "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledDir"
    )
    if ($TargetArch -eq "arm64") {
        $ConfigureArgs += @("-A", "ARM64", "-G", $Arm64Toolchain.Generator,
            "-DCMAKE_GENERATOR_INSTANCE=$($Arm64Toolchain.InstallationPath)")
    } elseif (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) {
        $ConfigureArgs += @("-A", "x64")
    }
    & $cmakeExe @ConfigureArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error-Exit "CMake Configure 失败"
    }

    Write-Step "CMake Build [$TargetArch / $BuildConfig]"

    & $cmakeExe --build $BuildDir --config $BuildConfig --parallel
    if ($LASTEXITCODE -ne 0) {
        Write-Error-Exit "CMake Build 失败"
    }

    Write-Host "[OK] 构建完成: $TargetArch / $BuildConfig" -ForegroundColor Green

    $BuiltExe = Join-Path $BuildDir "bin\$BuildConfig\Tools3000.exe"
    if (-not (Test-Path -LiteralPath $BuiltExe)) {
        $BuiltExe = Join-Path $BuildDir "bin\Tools3000.exe"
    }
    if (-not (Test-Path -LiteralPath $BuiltExe)) {
        Write-Error-Exit "构建完成但缺少主程序 Tools3000.exe: $BuiltExe"
    }
    $Bytes = [System.IO.File]::ReadAllBytes($BuiltExe)
    if ($Bytes.Length -lt 64) {
        Write-Error-Exit "无效的 PE 文件: $BuiltExe"
    }
    $PeOffset = [BitConverter]::ToInt32($Bytes, 0x3c)
    if ($PeOffset -lt 0 -or $PeOffset + 6 -gt $Bytes.Length) {
        Write-Error-Exit "无效的 PE 头: $BuiltExe"
    }
    $Machine = [BitConverter]::ToUInt16($Bytes, $PeOffset + 4)
    $ExpectedMachine = if ($TargetArch -eq "arm64") { 0xAA64 } else { 0x8664 }
    if ($Machine -ne $ExpectedMachine) {
        Write-Error-Exit ("产物架构校验失败: 请求 {0}，PE Machine=0x{1:X4}" -f $TargetArch, $Machine)
    }

    if ($Package) {
        Write-Step "打包发布质量门禁校验 [$TargetArch]"
        $HostArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
        if ($TargetArch -eq "arm64" -and $HostArch -ne "arm64") {
            Write-Error-Exit "当前 $HostArch 主机无法直接运行 ARM64 原生测试门禁；禁止直接在交叉编译宿主机生成正式发布包，请在 ARM64 目标机运行测试验证后打包。"
        }
        $TestExe = Join-Path $BuildDir "bin\$BuildConfig\Tools3000Tests.exe"
        if (-not (Test-Path $TestExe)) {
            $TestExe = Join-Path $BuildDir "bin\Tools3000Tests.exe"
        }
        if (-not (Test-Path $TestExe)) {
            Write-Error-Exit "缺少单元测试二进制产物: $TestExe"
        }
        Write-Step "运行原生单元测试门禁: $TestExe"
        & $TestExe
        if ($LASTEXITCODE -ne 0) {
            Write-Error-Exit "C++ 单元测试未通过，终止打包！退出码: $LASTEXITCODE"
        }
        Write-Host "[OK] C++ 单元测试门禁通过" -ForegroundColor Green

        Write-Step "打包发布包 [$TargetArch]"

        $OutDir = Join-Path $BuildDir "bin" $BuildConfig
        if (!(Test-Path $OutDir)) {
            $OutDir = Join-Path $BuildDir "bin"
        }

        $PackageName = "Tools3000-v${Version}-${TargetArch}"
        $PackageDir = Join-Path $ProjectRoot "dist" $PackageName
        $ZipPath = Join-Path $ProjectRoot "dist" "${PackageName}.zip"

        Remove-PackageDirectorySafely -Path $PackageDir `
            -ExpectedParent (Join-Path $ProjectRoot "dist") -ExpectedLeaf $PackageName
        New-Item -ItemType Directory -Path $PackageDir | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $PackageDir "plugins") | Out-Null

        foreach ($requiredBinary in @("Tools3000.exe", "Tools3000_Service.exe", "Tools3000Core.dll")) {
            $sourceBinary = Join-Path $OutDir $requiredBinary
            if (-not (Test-Path -LiteralPath $sourceBinary)) {
                Write-Error-Exit "打包缺少必需文件: $sourceBinary"
            }
            Copy-Item -LiteralPath $sourceBinary -Destination $PackageDir -Force
        }
        Get-ChildItem -LiteralPath $OutDir -Filter "*.dll" -File | Where-Object {
            $_.Name -notlike "gtest*.dll" -and $_.Name -notlike "gmock*.dll"
        } | Copy-Item -Destination $PackageDir -Force

        $PluginsDir = Join-Path $OutDir "plugins" $BuildConfig
        if (-not (Test-Path -LiteralPath $PluginsDir)) {
            $PluginsDir = Join-Path $OutDir "plugins"
        }
        if (-not (Test-Path -LiteralPath $PluginsDir)) {
            Write-Error-Exit "打包缺少插件构建目录: $PluginsDir"
        }
        $RequiredPluginNames = @(
            "Plugin_Gesture", "Plugin_Capture", "Plugin_Keycast",
            "Plugin_Search", "Plugin_DialogEnhancer"
        )
        foreach ($PluginName in $RequiredPluginNames) {
            foreach ($Extension in @(".dll", ".plugin.json")) {
                $PluginSource = Join-Path $PluginsDir "$PluginName$Extension"
                if (-not (Test-Path -LiteralPath $PluginSource)) {
                    Write-Error-Exit "打包缺少必需插件文件: $PluginSource"
                }
                Copy-Item -LiteralPath $PluginSource -Destination (Join-Path $PackageDir "plugins") -Force
            }
        }
        Get-ChildItem -LiteralPath $PluginsDir -Filter "*.dll" -File | Where-Object {
            $_.Name -notlike "Plugin_*.dll" -and
            $_.Name -notlike "gtest*.dll" -and $_.Name -notlike "gmock*.dll"
        } | Copy-Item -Destination $PackageDir -Force

        $VcRuntimeNames = @(
            "msvcp140.dll", "msvcp140_atomic_wait.dll", "concrt140.dll", "vcruntime140.dll"
        )
        if ($TargetArch -eq 'x64') { $VcRuntimeNames += 'vcruntime140_1.dll' }
        $VcRedistRoots = @()
        if ($env:VCToolsRedistDir -and (Test-Path -LiteralPath $env:VCToolsRedistDir)) {
            $VcRedistRoots += $env:VCToolsRedistDir
        }
        $RedistVsPath = $null
        if ($TargetArch -eq 'arm64') {
            $RedistVsPath = $Arm64Toolchain.InstallationPath
        } elseif (Test-Path -LiteralPath $VsWhere) {
            $RedistVsPath = & $VsWhere -latest -products * `
                -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath
        }
        if ($RedistVsPath) {
            $MsVcRedistRoot = Join-Path ([string]$RedistVsPath) "VC\Redist\MSVC"
            if (Test-Path -LiteralPath $MsVcRedistRoot) {
                $VcRedistRoots += Get-ChildItem -LiteralPath $MsVcRedistRoot -Directory |
                    Sort-Object Name -Descending | ForEach-Object { $_.FullName }
            }
        }
        $VcCrtDir = $null
        foreach ($Root in ($VcRedistRoots | Select-Object -Unique)) {
            $ArchitectureRoot = Join-Path $Root $TargetArch
            if (-not (Test-Path -LiteralPath $ArchitectureRoot)) { continue }
            $Candidate = Get-ChildItem -LiteralPath $ArchitectureRoot -Directory -Filter "Microsoft.VC*.CRT" |
                Sort-Object Name -Descending | Select-Object -First 1
            if ($Candidate -and @($VcRuntimeNames | Where-Object {
                    -not (Test-Path -LiteralPath (Join-Path $Candidate.FullName $_))
                }).Count -eq 0) {
                $VcCrtDir = $Candidate.FullName
                break
            }
        }
        if (-not $VcCrtDir) {
            Write-Error-Exit "找不到与 MSVC 工具链匹配的 $TargetArch Visual C++ Runtime 可再发行文件"
        }
        Get-ChildItem -LiteralPath $VcCrtDir -Filter '*.dll' -File |
            Copy-Item -Destination $PackageDir -Force

        $UIDist = Join-Path $UIDir "dist"
        if (-not (Test-Path -LiteralPath (Join-Path $UIDist "index.html"))) {
            Write-Error-Exit "打包缺少本次生成的 UI: $UIDist\index.html"
        }
        Copy-Item -LiteralPath $UIDist -Destination (Join-Path $PackageDir "ui") -Recurse -Force

        $ResourceSource = Join-Path $ProjectRoot "resources"
        $ResourceTarget = Join-Path $PackageDir "resources"
        New-Item -ItemType Directory -Path $ResourceTarget | Out-Null
        foreach ($ResourceName in @("app.ico", "tray.ico", "tray_dark.ico", "tray_color.ico", "app_icon_hires.png")) {
            $ResourcePath = Join-Path $ResourceSource $ResourceName
            if (Test-Path -LiteralPath $ResourcePath) {
                Copy-Item -LiteralPath $ResourcePath -Destination $ResourceTarget -Force
            }
        }
        $ResourceScripts = Join-Path $ResourceSource "scripts"
        if (Test-Path -LiteralPath $ResourceScripts) {
            Copy-Item -LiteralPath $ResourceScripts -Destination $ResourceTarget -Recurse -Force
        }

        if (Test-Path (Join-Path $ProjectRoot "LICENSE")) {
            Copy-Item (Join-Path $ProjectRoot "LICENSE") $PackageDir -Force
        }
        $NoticeScript = Join-Path $ProjectRoot "scripts\generate-third-party-notices.ps1"
        if (Test-Path -LiteralPath $NoticeScript) {
            $InstalledVcpkgDir = Join-Path $ProjectRoot "vcpkg_installed"
            if (-not (Test-Path -LiteralPath $InstalledVcpkgDir)) {
                $InstalledVcpkgDir = Join-Path $BuildDir "vcpkg_installed"
            }
            & pwsh -NoProfile -File $NoticeScript `
                -VcpkgInstalledDir $InstalledVcpkgDir `
                -Triplet "$TargetArch-windows" `
                -UiDir $UIDir `
                -OutputDirectory $PackageDir `
                -ProjectVersion $Version
            if ($LASTEXITCODE -ne 0) {
                Write-Error-Exit "第三方许可与 SBOM 生成失败"
            }
        }
        $RequiredPackageArtifacts = @(
            "Tools3000.exe", "Tools3000_Service.exe", "Tools3000Core.dll", "ui\index.html",
            "plugins\Plugin_Gesture.dll", "plugins\Plugin_Gesture.plugin.json",
            "plugins\Plugin_Capture.dll", "plugins\Plugin_Capture.plugin.json",
            "plugins\Plugin_Keycast.dll", "plugins\Plugin_Keycast.plugin.json",
            "plugins\Plugin_Search.dll", "plugins\Plugin_Search.plugin.json",
            "plugins\Plugin_DialogEnhancer.dll", "plugins\Plugin_DialogEnhancer.plugin.json",
            "LICENSE", "THIRD_PARTY_NOTICES.txt", "SBOM.spdx.json"
        ) + $VcRuntimeNames
        foreach ($releaseDocument in $RequiredPackageArtifacts) {
            if (-not (Test-Path -LiteralPath (Join-Path $PackageDir $releaseDocument))) {
                Write-Error-Exit "打包缺少发布合规材料: $releaseDocument"
            }
        }

        if (Test-Path $ZipPath) {
            Remove-Item -Force $ZipPath
        }
        Compress-Archive -Path "$PackageDir\*" -DestinationPath $ZipPath
        Write-Host "[OK] 发布包生成: $ZipPath" -ForegroundColor Green
    }
}

if (-not $SkipUI) {
    Write-Step "构建前端 UI (Vite)"
    Push-Location $UIDir
    try {
        if ($Package -or -not (Test-Path -LiteralPath (Join-Path $UIDir "node_modules"))) {
            npm ci --prefer-offline --no-audit --no-fund
            if ($LASTEXITCODE -ne 0) {
                Write-Error-Exit "npm ci 失败"
            }
        }
        if ($Package) {
            Write-Step "执行前端发布质量门禁 (lint & test)..."
            foreach ($Command in @("lint", "test")) {
                npm run $Command
                if ($LASTEXITCODE -ne 0) {
                    Write-Error-Exit "前端质量门禁失败: npm run $Command"
                }
            }
        }
        npm run build
        if ($LASTEXITCODE -ne 0) {
            Write-Error-Exit "UI 构建失败"
        }
        Write-Host "[OK] UI 构建与门禁完成" -ForegroundColor Green
    } finally {
        Pop-Location
    }
}

if ($All) {
    Invoke-BuildArch "x64" $Config
    Invoke-BuildArch "arm64" $Config
} else {
    Invoke-BuildArch $Arch $Config
}

Write-Host "全部构建任务已完成！" -ForegroundColor Green
