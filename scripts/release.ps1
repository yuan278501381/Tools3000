<#
.SYNOPSIS
    Tools3000 世界级全自动发版流水线 (DevOps Master Release Pipeline)
.DESCRIPTION
    一键完成：版本自增 +1 -> 提交 Release 说明 -> 非快进合并至 main -> 全量编译与生命周期端到端门禁 ->
    EXE 版本强校验 -> 资产打包与 SHA256 校验 -> Git 打 Tag 与全量推送 -> GitHub Release 官方发布 -> 自动检出下一阶段特性分支。
.EXAMPLE
    pwsh scripts/release.ps1                 # 默认补丁版本自增 +1 (如 1.0.0 -> 1.0.1)
    pwsh scripts/release.ps1 -Bump Minor     # 次版本自增 +1 (如 1.0.0 -> 1.1.0)
    pwsh scripts/release.ps1 -Version 1.0.1  # 显式指定目标版本
#>
param(
    [ValidateSet("Patch", "Minor", "Major")]
    [string]$Bump = "Patch",
    [string]$Version = "",
    [string]$Title = "",
    [string]$NextFeatureBranch = "",
    [switch]$LocalOnly = $false,             # 仅本地演练发版 (不推送到远端 Git，不发布 GitHub Release)
    [switch]$SkipGitHubPublish = $false      # 推送 Git 标签但跳过 GitHub Release 网页发布
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
$ScriptDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Definition)
if (-not $ScriptDir) { $ScriptDir = (Get-Location).Path }
Set-Location $ScriptDir

. (Join-Path $PSScriptRoot "ReleaseArtifacts.ps1")

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host " [INFO] Tools3000 DevOps 一键全自动发版总控流水线" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

# 1. 检查 Git 工作树与分支状态
$CurrentBranch = (git branch --show-current).Trim()
if ($LASTEXITCODE -ne 0) { throw "无法读取当前 Git 分支，退出码: $LASTEXITCODE" }
if ([string]::IsNullOrWhiteSpace($CurrentBranch)) {
    throw "无法获取当前 Git 分支，请检查 Git 环境！"
}

$DirtyFiles = @(git status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw "无法读取 Git 工作树状态，退出码: $LASTEXITCODE" }
if ($DirtyFiles.Count -gt 0) {
    Write-Host "[ERROR] 检测到工作区有未提交的改动，请先提交或暂存：" -ForegroundColor Red
    $DirtyFiles | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    throw "发版流水线要求干净的工作区！"
}

# 2. 版本计算与唯一事实源更新
$VersionFile = Join-Path $ScriptDir "VERSION"
if (-not (Test-Path -LiteralPath $VersionFile)) {
    throw "缺少版本单一事实源: $VersionFile"
}
$CurrentVersion = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
if ($CurrentVersion -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
    throw "当前 VERSION 格式不合规: $CurrentVersion"
}
$Major = [int]$Matches[1]
$Minor = [int]$Matches[2]
$Patch = [int]$Matches[3]

$TargetVersion = ""
if (-not [string]::IsNullOrWhiteSpace($Version)) {
    if ($Version -notmatch '^\d+\.\d+\.\d+$') {
        throw "指定的版本号格式必须为 X.Y.Z，当前值: $Version"
    }
    $TargetVersion = $Version
} else {
    switch ($Bump) {
        "Major" { $Major++; $Minor = 0; $Patch = 0 }
        "Minor" { $Minor++; $Patch = 0 }
        "Patch" { $Patch++ }
    }
    $TargetVersion = "$Major.$Minor.$Patch"
}

$TargetTag = "v$TargetVersion"
Write-Host "[INFO] 当前版本: v$CurrentVersion" -ForegroundColor Yellow
Write-Host "[INFO] 发行版本: $TargetTag (版本号: $TargetVersion)" -ForegroundColor Green

function Generate-UserCentricReleaseNotes {
    param(
        [string]$BaseBranch = "main",
        [string]$CurrentBranch = "dev",
        [string]$TargetTag = "v1.0.0"
    )

    $features = [System.Collections.Generic.List[string]]::new()
    $fixes = [System.Collections.Generic.List[string]]::new()
    $improvements = [System.Collections.Generic.List[string]]::new()
    $technologies = [System.Collections.Generic.List[string]]::new()

    # 提取当前分支与主分支之间的真实差异提交
    $diffCommits = @(git log "$BaseBranch..$CurrentBranch" --no-merges --pretty=format:"%H")
    if ($LASTEXITCODE -ne 0) { throw "无法读取 $BaseBranch..$CurrentBranch 的提交记录" }
    foreach ($commitHash in $diffCommits) {
        if ([string]::IsNullOrWhiteSpace($commitHash)) { continue }
        $commitMsg = (git log -n 1 --pretty=format:"%B" $commitHash) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "无法读取提交信息: $commitHash" }
        if ([string]::IsNullOrWhiteSpace($commitMsg)) { continue }

        $lines = $commitMsg -split '\r?\n'
        $subj = $lines[0].Trim()

        # 忽略发布自增与合并元数据提交
        if ($subj -match '^chore\(release\):' -or $subj -match '^merge\(') {
            continue
        }

        # 尝试提取 "[业务角度]" 中针对用户体感的描述清单
        $bizItems = [System.Collections.Generic.List[string]]::new()
        if ($commitMsg -match '(?s)(?:\[业务角度\]|💼\s*业务角度)[：:]?\s*(.*?)(?=(?:\r?\n\s*(?:\[技术角度\]|🔧|💻|⚙️|##)|\z))') {
            $bizBlock = $Matches[1].Trim()
            $bizLines = $bizBlock -split '\r?\n'
            foreach ($bLine in $bizLines) {
                $trimmed = $bLine.Trim()
                if ($trimmed.StartsWith('-') -or $trimmed.StartsWith('*')) {
                    $item = $trimmed.Substring(1).Trim()
                    if ($item.EndsWith(';') -or $item.EndsWith('；')) {
                        $item = $item.Substring(0, $item.Length - 1)
                    }
                    if (-not [string]::IsNullOrWhiteSpace($item)) {
                        $bizItems.Add($item)
                    }
                }
            }
        }

        # 尝试提取 "[技术角度]" 中架构与底层实现的描述清单
        if ($commitMsg -match '(?s)(?:\[技术角度\]|🔧\s*技术角度)[：:]?\s*(.*?)(?=(?:\r?\n\s*(?:\[业务角度\]|💼|💻|⚙️|##)|\z))') {
            $techBlock = $Matches[1].Trim()
            $techLines = $techBlock -split '\r?\n'
            foreach ($tLine in $techLines) {
                $trimmed = $tLine.Trim()
                if ($trimmed.StartsWith('-') -or $trimmed.StartsWith('*')) {
                    $item = $trimmed.Substring(1).Trim()
                    if ($item.EndsWith(';') -or $item.EndsWith('；')) {
                        $item = $item.Substring(0, $item.Length - 1)
                    }
                    if (-not [string]::IsNullOrWhiteSpace($item)) {
                        if (-not $technologies.Contains($item)) {
                            $technologies.Add($item)
                        }
                    }
                }
            }
        }

        # 按照用户体感类型归类
        if ($subj -match '^feat(\(.*\))?:') {
            if ($bizItems.Count -gt 0) {
                $bizItems | ForEach-Object { if (-not $features.Contains($_)) { $features.Add($_) } }
            } else {
                $clean = ($subj -replace '^feat(\(.*\))?:\s*', '').Trim()
                if (-not $features.Contains($clean)) { $features.Add($clean) }
            }
        } elseif ($subj -match '^fix(\(.*\))?:') {
            if ($bizItems.Count -gt 0) {
                $bizItems | ForEach-Object { if (-not $fixes.Contains($_)) { $fixes.Add($_) } }
            } else {
                $clean = ($subj -replace '^fix(\(.*\))?:\s*', '').Trim()
                if (-not $fixes.Contains($clean)) { $fixes.Add($clean) }
            }
        } else {
            if ($bizItems.Count -gt 0) {
                $bizItems | ForEach-Object { if (-not $improvements.Contains($_)) { $improvements.Add($_) } }
            } else {
                $clean = ($subj -replace '^[a-zA-Z]+(\(.*\))?:\s*', '').Trim()
                if (-not $improvements.Contains($clean)) { $improvements.Add($clean) }
            }
        }
    }

    $sb = [System.Text.StringBuilder]::new()
    $sb.AppendLine("# Tools3000 $TargetTag 官方正式版发布说明") | Out-Null
    $sb.AppendLine() | Out-Null
    $sb.AppendLine("欢迎体验 **Tools3000 $TargetTag**！本次更新专注于解决日常使用中的体感痛点，全面重构了截图与录屏全链路、大幅升级标注与离线 OCR 能力，并从底层彻底根治系统启动与高并发渲染的稳定性隐患。") | Out-Null
    $sb.AppendLine() | Out-Null
    $sb.AppendLine("---") | Out-Null
    $sb.AppendLine() | Out-Null
    $sb.AppendLine("## 本次重点更新与体验改进") | Out-Null
    $sb.AppendLine() | Out-Null

    if ($features.Count -gt 0) {
        $sb.AppendLine("### 业务与功能体验进化 (用户视角)") | Out-Null
        foreach ($f in $features) {
            $sb.AppendLine("- $f") | Out-Null
        }
        $sb.AppendLine() | Out-Null
    }

    if ($fixes.Count -gt 0) {
        $sb.AppendLine("### 痛点修复与体验优化") | Out-Null
        foreach ($fx in $fixes) {
            $sb.AppendLine("- $fx") | Out-Null
        }
        $sb.AppendLine() | Out-Null
    }

    if ($technologies.Count -gt 0) {
        $sb.AppendLine("### 架构演进与技术重构 (底层实现)") | Out-Null
        foreach ($tech in $technologies) {
            $sb.AppendLine("- $tech") | Out-Null
        }
        $sb.AppendLine() | Out-Null
    }

    if ($improvements.Count -gt 0) {
        $sb.AppendLine("### 稳定性与底层演进") | Out-Null
        foreach ($imp in $improvements) {
            $sb.AppendLine("- $imp") | Out-Null
        }
        $sb.AppendLine() | Out-Null
    }

    if ($features.Count -eq 0 -and $fixes.Count -eq 0 -and $improvements.Count -eq 0) {
        $sb.AppendLine("### 1. 核心功能演进与体验提升") | Out-Null
        $sb.AppendLine("- 优化日常使用体感与操作响应速度；") | Out-Null
        $sb.AppendLine("- 修复已知使用问题与边缘交互缺陷。") | Out-Null
        $sb.AppendLine() | Out-Null
        $sb.AppendLine("### 2. 系统稳定性与架构加固") | Out-Null
        $sb.AppendLine("- 全模块生命周期与防死锁端到端保障；") | Out-Null
        $sb.AppendLine("- 跨系统（Windows 10/11/Server 2025）通用圆角与纯净渲染支持。") | Out-Null
        $sb.AppendLine() | Out-Null
    }

    return $sb.ToString()
}

# 3. 智能提炼 Git 差异并生成以用户体感为核心的 Release Notes
$NotesFile = Join-Path $ScriptDir "docs\RELEASE_NOTES_$TargetTag.md"
if (-not (Test-Path $NotesFile)) {
    Write-Host "[INFO] 正在分析与主分支的差异，智能生成用户体感 Release Notes: $NotesFile..." -ForegroundColor Cyan
    $GeneratedNotes = Generate-UserCentricReleaseNotes -BaseBranch "main" -CurrentBranch $CurrentBranch -TargetTag $TargetTag
    $GeneratedNotes | Set-Content -Path $NotesFile -Encoding utf8
    Write-Host "[OK] Release Notes 已智能生成！预览如下：" -ForegroundColor Green
    Write-Host "-------------------------------------------------------" -ForegroundColor Gray
    Write-Host $GeneratedNotes -ForegroundColor Gray
    Write-Host "-------------------------------------------------------" -ForegroundColor Gray
} else {
    Write-Host "[INFO] 使用已存在的 Release Notes: $NotesFile" -ForegroundColor Cyan
}

# 4. 更新 VERSION 文件
Set-Content -Path $VersionFile -Value $TargetVersion -NoNewline -Encoding utf8
Write-Host "[OK] VERSION 已更新为: $TargetVersion" -ForegroundColor Green

# 5. 在开发分支提交版本升级 commit
git add VERSION docs/RELEASE_NOTES_$TargetTag.md CHANGELOG.md
if ($LASTEXITCODE -ne 0) { throw "暂存版本与发布说明失败！" }
$HasStaged = @(git diff --cached --name-only)
if ($LASTEXITCODE -ne 0) { throw "无法检查待提交的发布文件！" }
if ($HasStaged.Count -gt 0) {
    git commit -m "chore(release): 升级项目版本至 $TargetTag 并同步官方发布日志与版本元数据`n`n[业务角度]`n- 发布 Tools3000 $TargetTag 官方正式版；`n- 同步生成以用户体感与痛点解决为视角的官方 Release Notes。`n`n[技术角度]`n- 升级唯一事实源 VERSION 至 $TargetVersion；`n- 准备主分支发布与资产打包。"
    if ($LASTEXITCODE -ne 0) { throw "提交版本元数据失败！" }
    Write-Host "[OK] 版本元数据已在当前分支提交" -ForegroundColor Green
}

# 6. 切换至 main 分支并执行非快进合并
Write-Host "[GIT] 切换至 main 分支并执行 --no-ff 显式合并..." -ForegroundColor Cyan
git checkout main
if ($LASTEXITCODE -ne 0) { throw "切换至 main 分支失败！" }

git merge --no-ff $CurrentBranch -m "merge($CurrentBranch): 合并 $CurrentBranch 分支至 main 分支，发布 $TargetTag 正式版"
if ($LASTEXITCODE -ne 0) { throw "合并至 main 分支失败！" }
Write-Host "[OK] main 分支已成功完成显式非快进合并" -ForegroundColor Green

# 7. 全量清理并执行本地编译构建与生命周期端到端门禁
Write-Host "[BUILD] 执行全量编译构建与生命周期测试门禁..." -ForegroundColor Cyan
foreach ($ArtifactDirectory in @("Output", "deploy_dist", "ui\dist", "release_assets")) {
    Remove-ProjectArtifactDirectorySafely -ProjectRoot $ScriptDir -RelativePath $ArtifactDirectory
}

& pwsh -ExecutionPolicy Bypass -File .\deploy.ps1 -RequireSigning
if ($LASTEXITCODE -ne 0) { throw "构建部署门禁失败，终止发版！" }

$MainExe = if (Test-Path "deploy_dist\Tools3000.exe") { "deploy_dist\Tools3000.exe" } else { "deploy_dist\Tools3000.exe" }
$ServiceExe = if (Test-Path "deploy_dist\Tools3000_Service.exe") { "deploy_dist\Tools3000_Service.exe" } else { "deploy_dist\Tools3000_Service.exe" }
$SignedReleaseInputs = @(
    $MainExe,
    $ServiceExe,
    "deploy_dist\Tools3000Core.dll",
    "Output\Tools3000-Setup.exe"
) + @(Get-ChildItem "deploy_dist\plugins\Plugin_*.dll" -File | Select-Object -ExpandProperty FullName)
foreach ($SignedInput in $SignedReleaseInputs) {
    $Signature = Get-AuthenticodeSignature -LiteralPath $SignedInput
    if ($Signature.Status -ne "Valid") {
        throw "正式发布签名验证失败: $SignedInput ($($Signature.Status))"
    }
}
Write-Host "[OK] 自有二进制与安装包 Authenticode 签名验证通过" -ForegroundColor Green

# 8. EXE 二进制版本强校验门禁
$BuiltExe = if (Test-Path "deploy_dist\Tools3000.exe") { "deploy_dist\Tools3000.exe" } else { "deploy_dist\Tools3000.exe" }
if (-not (Test-Path $BuiltExe)) { throw "未找到编译产物: $BuiltExe" }
$VersionInfo = (Get-Item $BuiltExe).VersionInfo
$ExeVersion = $VersionInfo.ProductVersion
$SemVerExeVersion = "$($VersionInfo.ProductMajorPart).$($VersionInfo.ProductMinorPart).$($VersionInfo.ProductBuildPart)"
if ($ExeVersion -ne $TargetVersion -and $SemVerExeVersion -ne $TargetVersion) {
    throw "版本强校验失败！编译产物版本 ($ExeVersion / $SemVerExeVersion) 与目标发版版本 ($TargetVersion) 不匹配！"
}
Write-Host "[OK] 二进制版本强校验通过: $BuiltExe ProductVersion = $ExeVersion (SemVer: $SemVerExeVersion)" -ForegroundColor Green

# 9. 资产归档与 SHA256 校验和生成
$ReleaseDir = Join-Path $ScriptDir "release_assets"
Remove-ProjectArtifactDirectorySafely -ProjectRoot $ScriptDir -RelativePath "release_assets"
New-Item -ItemType Directory -Path $ReleaseDir | Out-Null

$SetupExe = "Output\Tools3000-Setup.exe"
if (-not (Test-Path $SetupExe)) { throw "未找到安装包: $SetupExe" }
$TargetSetup = "$ReleaseDir\Tools3000-$TargetTag-Setup.exe"
Copy-Item $SetupExe $TargetSetup
$TargetGenericSetup = "$ReleaseDir\Tools3000-Setup.exe"
Copy-Item $SetupExe $TargetGenericSetup

$TargetZip = "$ReleaseDir\Tools3000-$TargetTag-win-x64-portable.zip"
Compress-Archive -Path (Join-Path $ScriptDir "deploy_dist\*") `
    -DestinationPath $TargetZip -CompressionLevel Optimal

$Checksums = @()
Get-ChildItem -Path $ReleaseDir -File | ForEach-Object {
    $Hash = (Get-FileHash -Path $_.FullName -Algorithm SHA256).Hash.ToLower()
    $Checksums += "$Hash  $($_.Name)"
}
$ChecksumFile = "$ReleaseDir\SHA256SUMS.txt"
$Checksums | Out-File -FilePath $ChecksumFile -Encoding utf8
Write-Host "[OK] 发布资产已就绪并生成 SHA256 校验和" -ForegroundColor Green

# 10. 创建 Tag 并推送到 GitHub (若 LocalOnly 则跳过推送)
Write-Host "[GIT] 创建标签 $TargetTag..." -ForegroundColor Cyan
$LocalTag = @(git tag --list $TargetTag)
if ($LASTEXITCODE -ne 0) { throw "检查本地标签失败！" }
if ($LocalTag.Count -gt 0) { throw "标签 $TargetTag 已存在；正式发布标签不可覆盖。" }
if (-not $LocalOnly) {
    $RemoteTag = @(git ls-remote --tags origin "refs/tags/$TargetTag")
    if ($LASTEXITCODE -ne 0) { throw "检查远程标签 $TargetTag 失败！" }
    if ($RemoteTag.Count -gt 0) { throw "远程标签 $TargetTag 已存在；正式发布标签不可覆盖。" }
}
git tag -a $TargetTag -m "Tools3000 $TargetTag"
if ($LASTEXITCODE -ne 0) { throw "创建标签失败！" }

if (-not $LocalOnly) {
    Write-Host "[GIT] 正在推送 main 与标签至远程 GitHub..." -ForegroundColor Cyan
    git push origin main
    if ($LASTEXITCODE -ne 0) { throw "推送 main 分支失败！" }
    git push origin "refs/tags/$TargetTag"
    if ($LASTEXITCODE -ne 0) { throw "推送标签 $TargetTag 失败！" }
    if ($CurrentBranch -ne "main") {
        git checkout $CurrentBranch
        if ($LASTEXITCODE -ne 0) { throw "切回分支 $CurrentBranch 失败！" }
        git merge main
        if ($LASTEXITCODE -ne 0) { throw "将 main 同步回 $CurrentBranch 失败！" }
        git push origin $CurrentBranch
        if ($LASTEXITCODE -ne 0) { throw "推送分支 $CurrentBranch 失败！" }
    }
    Write-Host "[OK] Git 分支与 Tag 已全量同步至远程 GitHub" -ForegroundColor Green
} else {
    Write-Host "[INFO] [LocalOnly] 跳过 Git 远程推送" -ForegroundColor Yellow
}

# 11. GitHub Release 官方发布 (若 LocalOnly 或 SkipGitHubPublish 则跳过)
if (-not $LocalOnly -and -not $SkipGitHubPublish) {
    Write-Host "[GITHUB] 发布官方 GitHub Release ($TargetTag)..." -ForegroundColor Cyan
    $ReleaseExists = $false
    try {
        gh release view $TargetTag 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { $ReleaseExists = $true }
    } catch {}

    if ($ReleaseExists) {
        throw "GitHub Release $TargetTag 已存在；正式发布资产不可覆盖。"
    } else {
        $ActualTitle = if (-not [string]::IsNullOrWhiteSpace($Title)) {
            $Title
        } else {
            "Tools3000 $TargetTag - 全场景 Windows 极客级系统效率工具箱正式发布"
        }
        gh release create $TargetTag $TargetSetup $TargetGenericSetup $TargetZip $ChecksumFile --title $ActualTitle --notes-file $NotesFile --latest
        if ($LASTEXITCODE -ne 0) { throw "创建 GitHub Release 失败！" }
    }
    Write-Host "[OK] GitHub Release 发布成功！" -ForegroundColor Green
} elseif ($LocalOnly) {
    Write-Host "[INFO] [LocalOnly] 跳过 GitHub Release 官方发布" -ForegroundColor Yellow
}

# 12. 检出后续特性分支
if ([string]::IsNullOrWhiteSpace($NextFeatureBranch)) {
    $NextFeatureBranch = $CurrentBranch
    if ($NextFeatureBranch -eq "main" -or $NextFeatureBranch -eq "dev") {
        $NextFeatureBranch = "feature/optimize-capture-and-recording"
    }
}
Write-Host "[GIT] 切换至下一阶段特性分支: $NextFeatureBranch" -ForegroundColor Cyan
git show-ref --verify --quiet "refs/heads/$NextFeatureBranch"
$NextFeatureBranchExists = $LASTEXITCODE -eq 0
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1) {
    throw "检查后续分支 $NextFeatureBranch 失败！"
}
if ($NextFeatureBranchExists) {
    git checkout $NextFeatureBranch
    if ($LASTEXITCODE -ne 0) { throw "切换至已有分支 $NextFeatureBranch 失败！" }
    git merge main
    if ($LASTEXITCODE -ne 0) { throw "将 main 合并至已有分支 $NextFeatureBranch 失败！" }
} else {
    git checkout -b $NextFeatureBranch main
    if ($LASTEXITCODE -ne 0) { throw "创建后续分支 $NextFeatureBranch 失败！" }
}
if (-not $LocalOnly) {
    git push origin -u $NextFeatureBranch
    if ($LASTEXITCODE -ne 0) { throw "推送后续分支 $NextFeatureBranch 失败！" }
}

Write-Host "=======================================================" -ForegroundColor Green
Write-Host " [OK] 恭喜！Tools3000 $TargetTag 全自动化发版流水线执行完毕！" -ForegroundColor Green
Write-Host " - 最新安装包: Output\Tools3000-Setup.exe" -ForegroundColor Green
Write-Host " - 发布资产库: release_assets" -ForegroundColor Green
Write-Host " - GitHub 发布页: https://github.com/yuan278501381/Tools3000/releases/tag/$TargetTag" -ForegroundColor Green
Write-Host " - 当前所在分支: $NextFeatureBranch" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Green
