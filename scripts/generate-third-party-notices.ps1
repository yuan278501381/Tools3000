#!/usr/bin/env pwsh
param(
    [Parameter(Mandatory = $true)]
    [string]$VcpkgInstalledDir,
    [Parameter(Mandatory = $true)]
    [string]$Triplet,
    [Parameter(Mandatory = $true)]
    [string]$UiDir,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [Parameter(Mandatory = $true)]
    [string]$ProjectVersion
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$StatusFile = Join-Path $VcpkgInstalledDir "vcpkg\status"
$LockFile = Join-Path $UiDir "package-lock.json"
$DependencyMetadataFile = Join-Path $ProjectRoot "third_party\dependency-metadata.json"
if (-not (Test-Path -LiteralPath $StatusFile)) { throw "缺少 vcpkg 状态文件: $StatusFile" }
if (-not (Test-Path -LiteralPath $LockFile)) { throw "缺少 npm 锁文件: $LockFile" }
if (-not (Test-Path -LiteralPath $DependencyMetadataFile)) {
    throw "缺少仓库内第三方依赖元数据: $DependencyMetadataFile"
}
$DependencyMetadata = Get-Content -LiteralPath $DependencyMetadataFile -Raw |
    ConvertFrom-Json -AsHashtable
if ($DependencyMetadata.schemaVersion -ne 1) {
    throw "不支持的第三方依赖元数据版本: $($DependencyMetadata.schemaVersion)"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$Notice = [System.Text.StringBuilder]::new()
[void]$Notice.AppendLine("Tools3000 Third-Party Notices")
[void]$Notice.AppendLine("Generated from the exact vcpkg triplet and npm lockfile used for this build.")
[void]$Notice.AppendLine()
$Packages = [System.Collections.Generic.List[object]]::new()
$PackageKeys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$SpdxIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)

function Resolve-RepositoryFile {
    param([Parameter(Mandatory = $true)][string]$RelativePath)
    $FullPath = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot $RelativePath))
    $RepositoryPrefix = [System.IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\') + '\'
    if (-not $FullPath.StartsWith($RepositoryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "第三方依赖元数据引用了仓库外路径: $RelativePath"
    }
    return $FullPath
}

function Add-PackageRecord {
    param(
        [string]$Name,
        [string]$Version,
        [string]$License,
        [string]$Source,
        [string[]]$NoticeFiles,
        [string]$DownloadLocation = "NOASSERTION"
    )
    if ([string]::IsNullOrWhiteSpace($License)) { $License = "NOASSERTION" }
    if ([string]::IsNullOrWhiteSpace($DownloadLocation)) { $DownloadLocation = "NOASSERTION" }
    $PackageKey = "$Name`0$Version"
    if (-not $PackageKeys.Add($PackageKey)) { return }

    $IdName = ($Name -replace '[^A-Za-z0-9.-]', '-')
    $IdVersion = ($Version -replace '[^A-Za-z0-9.-]', '-')
    $SpdxIdBase = "SPDXRef-Package-$IdName-$IdVersion"
    $SpdxId = $SpdxIdBase
    $CollisionIndex = 2
    while (-not $SpdxIds.Add($SpdxId)) {
        $SpdxId = "$SpdxIdBase-$CollisionIndex"
        $CollisionIndex++
    }
    $Packages.Add([pscustomobject]@{
        SPDXID = $SpdxId
        name = $Name
        versionInfo = $Version
        downloadLocation = $DownloadLocation
        filesAnalyzed = $false
        licenseConcluded = "NOASSERTION"
        licenseDeclared = $License
        copyrightText = "NOASSERTION"
        sourceInfo = $Source
    })

    [void]$Notice.AppendLine("================================================================================")
    [void]$Notice.AppendLine("$Name $Version")
    [void]$Notice.AppendLine("Declared license: $License")
    [void]$Notice.AppendLine("Source: $Source")
    [void]$Notice.AppendLine("--------------------------------------------------------------------------------")
    $AddedText = $false
    foreach ($NoticeFile in $NoticeFiles) {
        if ($NoticeFile -and (Test-Path -LiteralPath $NoticeFile)) {
            $NoticeText = Get-Content -LiteralPath $NoticeFile -Raw
            if ([string]::IsNullOrWhiteSpace($NoticeText)) { continue }
            $NoticeText = $NoticeText.Trim()
            [void]$Notice.AppendLine($NoticeText)
            [void]$Notice.AppendLine()
            $AddedText = $true
        }
    }
    if (-not $AddedText) {
        throw "缺少第三方许可证正文: $Name $Version ($Source)"
    }
}

$StatusText = (Get-Content -LiteralPath $StatusFile -Raw) -replace "`r`n", "`n"
$StatusBlocks = [regex]::Split($StatusText.Trim(), "\n\s*\n")
foreach ($Block in $StatusBlocks) {
    $Fields = @{}
    foreach ($Line in ($Block -split "`n")) {
        if ($Line -match '^([^:]+):\s*(.*)$') { $Fields[$Matches[1]] = $Matches[2].Trim() }
    }
    # vcpkg status 为每个 feature 额外生成一个段落；它们不是独立的发布包，
    # 且没有 Version，若纳入会产生重复 SPDXID 和重复许可文本。
    if ($Fields.ContainsKey("Feature")) { continue }
    if ($Fields.Architecture -ne $Triplet -or $Fields.Status -ne "install ok installed") { continue }
    $Name = [string]$Fields.Package
    $Version = [string]$Fields.Version
    if ($Fields.'Port-Version' -and $Fields.'Port-Version' -ne "0") {
        $Version = "$Version#$($Fields.'Port-Version')"
    }
    $ShareDir = Join-Path $VcpkgInstalledDir "$Triplet\share\$Name"
    $CopyrightFile = Join-Path $ShareDir "copyright"
    $DownloadLocation = "NOASSERTION"
    $DeclaredLicense = "NOASSERTION"
    $VcpkgSpdxPath = Join-Path $ShareDir "vcpkg.spdx.json"
    if (Test-Path -LiteralPath $VcpkgSpdxPath) {
        try {
            $VcpkgSpdx = Get-Content -LiteralPath $VcpkgSpdxPath -Raw | ConvertFrom-Json
            $PortPackage = $VcpkgSpdx.packages | Where-Object { $_.SPDXID -eq "SPDXRef-port" } |
                Select-Object -First 1
            if ($PortPackage.downloadLocation) { $DownloadLocation = [string]$PortPackage.downloadLocation }
            if ($PortPackage.licenseConcluded -and $PortPackage.licenseConcluded -ne "NOASSERTION") {
                $DeclaredLicense = [string]$PortPackage.licenseConcluded
            } elseif ($PortPackage.licenseDeclared -and $PortPackage.licenseDeclared -ne "NOASSERTION") {
                $DeclaredLicense = [string]$PortPackage.licenseDeclared
            }

            # A LicenseRef is only valid when the generated document also carries
            # the matching ExtractedLicensingInfo. vcpkg uses
            # LicenseRef-vcpkg-null when a port manifest deliberately leaves its
            # license unset, and does not define that reference. Do not propagate
            # this internal placeholder (or any other unresolved LicenseRef) into
            # the release SBOM as a dangling identifier.
            $LicenseRefs = @([regex]::Matches(
                $DeclaredLicense,
                '(?:DocumentRef-[A-Za-z0-9.-]+:)?LicenseRef-[A-Za-z0-9.-]+'
            ) | ForEach-Object { $_.Value } | Sort-Object -Unique)
            if ($LicenseRefs.Count -gt 0) {
                $ExtractedLicenseIds = @($VcpkgSpdx.hasExtractedLicensingInfos | ForEach-Object {
                    [string]$_.licenseId
                })
                $UnresolvedLicenseRefs = @($LicenseRefs | Where-Object {
                    $_ -eq 'LicenseRef-vcpkg-null' -or $_ -notin $ExtractedLicenseIds
                })
                if ($UnresolvedLicenseRefs.Count -gt 0) {
                    Write-Warning "$Name $Version 的 vcpkg SPDX 仅提供未定义的许可证引用 ($($UnresolvedLicenseRefs -join ', '))；使用 NOASSERTION。"
                    $DeclaredLicense = "NOASSERTION"
                } else {
                    # This generator does not yet merge upstream ExtractedLicensingInfo
                    # records into its own document. Being explicit is safer than
                    # emitting a reference whose definition would be lost.
                    Write-Warning "$Name $Version 使用自定义 LicenseRef；当前生成器不复制其定义，使用 NOASSERTION。"
                    $DeclaredLicense = "NOASSERTION"
                }
            }
        } catch {
            Write-Warning "无法解析 $VcpkgSpdxPath；SBOM 下载位置将使用 NOASSERTION"
        }
    }
    Add-PackageRecord -Name $Name -Version $Version -License $DeclaredLicense `
        -Source "vcpkg:$Triplet" -NoticeFiles @($CopyrightFile) `
        -DownloadLocation $DownloadLocation
}

$CMakeText = Get-Content -LiteralPath (Join-Path $ProjectRoot "CMakeLists.txt") -Raw
if ($CMakeText -notmatch 'set\(WEBVIEW2_VERSION\s+"([^"]+)"') {
    throw "无法从 CMakeLists.txt 读取固定的 WEBVIEW2_VERSION"
}
$WebViewVersion = $Matches[1]
$WebViewPackagePath = Join-Path $ProjectRoot "packages\Microsoft.Web.WebView2.$WebViewVersion"
if (-not (Test-Path -LiteralPath $WebViewPackagePath)) {
    throw "缺少固定版本 WebView2 SDK 的许可材料: $WebViewPackagePath"
}
Add-PackageRecord -Name "Microsoft.Web.WebView2" -Version $WebViewVersion -License "BSD-3-Clause" `
    -Source "NuGet" -DownloadLocation "https://www.nuget.org/packages/Microsoft.Web.WebView2/$WebViewVersion" `
    -NoticeFiles @(
        (Join-Path $WebViewPackagePath "LICENSE.txt"),
        (Join-Path $WebViewPackagePath "NOTICE.txt")
    )

$Lock = Get-Content -LiteralPath $LockFile -Raw | ConvertFrom-Json -AsHashtable
foreach ($Entry in ($Lock.packages.GetEnumerator() | Sort-Object Key)) {
    if ($Entry.Key -notlike "node_modules/*" -or $Entry.Value.dev -eq $true) { continue }
    $Name = $Entry.Key.Substring("node_modules/".Length)
    $Version = [string]$Entry.Value.version
    $License = [string]$Entry.Value.license
    $ModuleDir = Join-Path $UiDir ($Entry.Key -replace '/', '\')
    $LicenseFiles = @()
    if (Test-Path -LiteralPath $ModuleDir) {
        $LicenseFiles = @(Get-ChildItem -LiteralPath $ModuleDir -File | Where-Object {
            $_.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)(\.|$)'
        } | Sort-Object Name | Select-Object -ExpandProperty FullName)
    }
    $NpmKey = "$Name@$Version"
    if ($LicenseFiles.Count -eq 0 -and $DependencyMetadata.npmLicenseFiles.ContainsKey($NpmKey)) {
        $LicenseFiles = @(Resolve-RepositoryFile `
            -RelativePath ([string]$DependencyMetadata.npmLicenseFiles[$NpmKey]))
    }
    Add-PackageRecord -Name $Name -Version $Version -License $License `
        -Source "npm package-lock.json" -NoticeFiles $LicenseFiles `
        -DownloadLocation ([string]$Entry.Value.resolved)
}

foreach ($Asset in @($DependencyMetadata.vendoredAssets)) {
    $AssetName = [string]$Asset.name
    $AssetVersion = [string]$Asset.version
    $AssetPath = Resolve-RepositoryFile -RelativePath ([string]$Asset.path)
    $AssetLicensePath = Resolve-RepositoryFile -RelativePath ([string]$Asset.licenseFile)
    if (-not (Test-Path -LiteralPath $AssetPath)) {
        throw "缺少登记的第三方内置资产: $AssetPath"
    }
    $ExpectedHash = ([string]$Asset.sha256).ToLowerInvariant()
    if ($ExpectedHash -notmatch '^[0-9a-f]{64}$') {
        throw "内置资产 SHA-256 格式无效: $AssetName $AssetVersion"
    }
    $ActualHash = (Get-FileHash -LiteralPath $AssetPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($ActualHash -ne $ExpectedHash) {
        throw "内置资产哈希不匹配: $AssetName $AssetVersion ($ActualHash != $ExpectedHash)"
    }
    Add-PackageRecord -Name $AssetName -Version $AssetVersion -License ([string]$Asset.license) `
        -Source "vendored:$($Asset.path) from $($Asset.sourcePath); sha256:$ActualHash" `
        -NoticeFiles @($AssetLicensePath) -DownloadLocation ([string]$Asset.downloadLocation)
}

$NoticePath = Join-Path $OutputDirectory "THIRD_PARTY_NOTICES.txt"
[System.IO.File]::WriteAllText($NoticePath, $Notice.ToString(), [System.Text.UTF8Encoding]::new($false))

$RootPackage = [pscustomobject]@{
    SPDXID = "SPDXRef-Package-Tools3000"
    name = "Tools3000"
    versionInfo = $ProjectVersion
    downloadLocation = "https://github.com/yuan278501381/Tools3000"
    filesAnalyzed = $false
    licenseConcluded = "MIT"
    licenseDeclared = "MIT"
    copyrightText = "Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors"
    sourceInfo = "Project release artifact"
}
$AllPackages = @($RootPackage) + @($Packages)
$AllSpdxIds = @($AllPackages | ForEach-Object { $_.SPDXID })
if (($AllSpdxIds | Sort-Object -Unique).Count -ne $AllSpdxIds.Count) {
    throw "SBOM 包 SPDXID 不唯一，拒绝生成发布材料。"
}
$PackageFingerprint = (($AllPackages | ForEach-Object { "$($_.name)@$($_.versionInfo)" }) -join "`n")
$Hash = [System.Security.Cryptography.SHA256]::HashData([System.Text.Encoding]::UTF8.GetBytes($PackageFingerprint))
$HashPrefix = ([Convert]::ToHexString($Hash)).ToLowerInvariant().Substring(0, 24)
$Relationships = @([ordered]@{
    spdxElementId = "SPDXRef-DOCUMENT"
    relationshipType = "DESCRIBES"
    relatedSpdxElement = $RootPackage.SPDXID
}) + @($Packages | ForEach-Object {
    [ordered]@{
        spdxElementId = $RootPackage.SPDXID
        relationshipType = "DEPENDS_ON"
        relatedSpdxElement = $_.SPDXID
    }
})
$Sbom = [ordered]@{
    spdxVersion = "SPDX-2.3"
    dataLicense = "CC0-1.0"
    SPDXID = "SPDXRef-DOCUMENT"
    name = "Tools3000-$ProjectVersion-$Triplet"
    documentNamespace = "https://github.com/yuan278501381/Tools3000/sbom/$ProjectVersion/$Triplet/$HashPrefix"
    creationInfo = [ordered]@{
        created = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        creators = @("Tool: Tools3000 generate-third-party-notices.ps1")
    }
    packages = $AllPackages
    relationships = $Relationships
}
$SbomPath = Join-Path $OutputDirectory "SBOM.spdx.json"
$SbomJson = $Sbom | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText($SbomPath, $SbomJson, [System.Text.UTF8Encoding]::new($false))

Write-Host "Generated $NoticePath"
Write-Host "Generated $SbomPath ($($AllPackages.Count) packages)"
