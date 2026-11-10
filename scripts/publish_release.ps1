param(
    [string]$Tag = "",
    [switch]$ForceRebuild = $false
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Definition)
if (-not $ScriptDir) { $ScriptDir = (Get-Location).Path }
Set-Location $ScriptDir

. (Join-Path $PSScriptRoot "ReleaseArtifacts.ps1")

$VersionFile = Join-Path $ScriptDir "VERSION"
if (-not (Test-Path -LiteralPath $VersionFile)) {
    throw "缺少唯一版本源: $VersionFile"
}
$ProjectVersion = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
if ($ProjectVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION 必须是稳定 SemVer（例如 1.2.3），当前值: $ProjectVersion"
}

if ([string]::IsNullOrWhiteSpace($Tag)) {
    $Tag = "v$ProjectVersion"
}
if ($Tag -notmatch '^v\d+\.\d+\.\d+$') {
    throw "发布标签必须使用 vMAJOR.MINOR.PATCH 格式，当前值: $Tag"
}
$CleanVersion = $Tag.Substring(1)
if ($CleanVersion -ne $ProjectVersion) {
    throw "发布标签 $Tag 与 VERSION ($ProjectVersion) 不一致。请只修改根目录 VERSION。"
}

$HeadCommit = (git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($HeadCommit)) {
    throw "无法读取当前 Git 提交"
}
$LocalTag = @(git tag --list $Tag)
if ($LASTEXITCODE -ne 0) { throw "检查本地标签 $Tag 失败" }
if ($LocalTag.Count -gt 0) { throw "标签 $Tag 已存在；正式发布标签不可覆盖。" }
$RemoteTag = @(git ls-remote --tags origin "refs/tags/$Tag")
if ($LASTEXITCODE -ne 0) { throw "检查远程标签 $Tag 失败" }
if ($RemoteTag.Count -gt 0) { throw "远程标签 $Tag 已存在；正式发布标签不可覆盖。" }

$DirtyFiles = @(git status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw "无法读取 Git 工作树状态" }
if ($DirtyFiles.Count -gt 0) {
    throw "发布要求干净工作树；请先审查并提交所有改动。"
}

Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "发布 Tools3000 $Tag (版本号: $CleanVersion) 到 GitHub Releases" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

# VERSION 是唯一产品版本源。为彻底杜绝旧安装包缓存混淆，发布过程必须无条件清理旧产物并触发全量一键编译与打包。
Write-Host "[BUILD] 正在清理旧产物并触发全量一键构建部署流水线 (VERSION: $CleanVersion)..." -ForegroundColor Cyan
if (Test-Path "Output\Tools3000-Setup.exe") {
    Remove-Item -Force "Output\Tools3000-Setup.exe"
}
& pwsh -ExecutionPolicy Bypass -File .\deploy.ps1 -RequireSigning
if ($LASTEXITCODE -ne 0) {
    throw "构建部署失败，终止发布！"
}

$SignedReleaseInputs = @(
    (if (Test-Path "deploy_dist\Tools3000.exe") { "deploy_dist\Tools3000.exe" } else { "deploy_dist\Tools3000.exe" }),
    (if (Test-Path "deploy_dist\Tools3000_Service.exe") { "deploy_dist\Tools3000_Service.exe" } else { "deploy_dist\Tools3000_Service.exe" }),
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

# 强校验：提取生成 EXE 的二进制版本元数据，必须与 VERSION 文件 100% 精确一致
$BuiltExe = if (Test-Path "deploy_dist\Tools3000.exe") { "deploy_dist\Tools3000.exe" } else { "deploy_dist\Tools3000.exe" }
if (-not (Test-Path $BuiltExe)) { throw "未找到编译产物: $BuiltExe" }
$VersionInfo = (Get-Item $BuiltExe).VersionInfo
$ExeVersion = $VersionInfo.ProductVersion
$SemVerExeVersion = "$($VersionInfo.ProductMajorPart).$($VersionInfo.ProductMinorPart).$($VersionInfo.ProductBuildPart)"
if ($ExeVersion -ne $CleanVersion -and $SemVerExeVersion -ne $CleanVersion) {
    throw "版本强校验失败！编译产物版本 ($ExeVersion / $SemVerExeVersion) 与发布版本 ($CleanVersion) 不一致！"
}
Write-Host "[OK] 版本强校验通过: $BuiltExe ProductVersion = $ExeVersion (SemVer: $SemVerExeVersion)" -ForegroundColor Green

$ReleaseDir = Join-Path $ScriptDir "release_assets"
Remove-ProjectArtifactDirectorySafely -ProjectRoot $ScriptDir -RelativePath "release_assets"
New-Item -ItemType Directory -Path $ReleaseDir | Out-Null

$SetupExe = "Output\Tools3000-Setup.exe"
if (-not (Test-Path $SetupExe)) {
    throw "未找到安装包文件: $SetupExe"
}

$TargetSetup = "$ReleaseDir\Tools3000-$Tag-Setup.exe"
Copy-Item $SetupExe $TargetSetup
Write-Host "[OK] 安装包已就绪: $TargetSetup" -ForegroundColor Green

$TargetZip = "$ReleaseDir\Tools3000-$Tag-win-x64-portable.zip"
Compress-Archive -Path (Join-Path $ScriptDir "deploy_dist\*") `
    -DestinationPath $TargetZip -CompressionLevel Optimal
Write-Host "[OK] 绿色便携包已压缩: $TargetZip" -ForegroundColor Green

$Checksums = @()
Get-ChildItem -Path $ReleaseDir -File | ForEach-Object {
    $Hash = (Get-FileHash -Path $_.FullName -Algorithm SHA256).Hash.ToLower()
    $Checksums += "$Hash  $($_.Name)"
}
$ChecksumFile = "$ReleaseDir\SHA256SUMS.txt"
$Checksums | Out-File -FilePath $ChecksumFile -Encoding utf8
Write-Host "[OK] 校验和文件已生成: $ChecksumFile" -ForegroundColor Green

git tag -a $Tag -m "Tools3000 $Tag"
if ($LASTEXITCODE -ne 0) { throw "创建标签 $Tag 失败" }
Write-Host "[OK] 标签 $Tag 已更新并指向当前 HEAD ($HeadCommit)" -ForegroundColor Green
git push origin "refs/tags/$Tag"
if ($LASTEXITCODE -ne 0) { throw "推送标签 $Tag 失败" }

$NotesFile = Join-Path $ScriptDir "docs\RELEASE_NOTES_$Tag.md"
if (-not (Test-Path $NotesFile)) {
    $NotesFile = Join-Path $ScriptDir "CHANGELOG.md"
}

gh release create $Tag $TargetSetup $TargetZip $ChecksumFile --title "Tools3000 $Tag (Windows x64)" --notes-file $NotesFile --latest
if ($LASTEXITCODE -ne 0) { throw "创建 GitHub Release $Tag 失败" }

Write-Host "=========================================" -ForegroundColor Green
Write-Host "Tools3000 $Tag 发布成功！" -ForegroundColor Green
Write-Host "=========================================" -ForegroundColor Green
