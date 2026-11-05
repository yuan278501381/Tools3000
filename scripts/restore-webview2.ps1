#!/usr/bin/env pwsh
[CmdletBinding()]
param(
    [string]$ProjectRoot = "",
    [string]$PackagesRoot = ""
)

$ErrorActionPreference = "Stop"
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $ScriptRoot
}
$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
if ([string]::IsNullOrWhiteSpace($PackagesRoot)) {
    $PackagesRoot = Join-Path $ProjectRoot "packages"
}
$PackagesRoot = [System.IO.Path]::GetFullPath($PackagesRoot)

$CMakeProjectFile = Join-Path $ProjectRoot "CMakeLists.txt"
if (-not (Test-Path -LiteralPath $CMakeProjectFile -PathType Leaf)) {
    throw "Cannot find CMakeLists.txt: $CMakeProjectFile"
}
$CMakeProjectText = Get-Content -LiteralPath $CMakeProjectFile -Raw
if ($CMakeProjectText -notmatch 'set\(WEBVIEW2_VERSION\s+"([^"]+)"') {
    throw "Cannot read the pinned WEBVIEW2_VERSION from $CMakeProjectFile"
}
$WebView2Version = $Matches[1]

$LockFile = Join-Path $ScriptRoot "webview2-sdk.lock.json"
if (-not (Test-Path -LiteralPath $LockFile -PathType Leaf)) {
    throw "Cannot find the WebView2 SDK lock file: $LockFile"
}
$Lock = Get-Content -LiteralPath $LockFile -Raw | ConvertFrom-Json -AsHashtable
if ($Lock.schemaVersion -ne 1 -or $Lock.packageId -ne "Microsoft.Web.WebView2") {
    throw "Unsupported or invalid WebView2 SDK lock file: $LockFile"
}
$VersionLock = $Lock.versions[$WebView2Version]
if (-not $VersionLock) {
    throw "WEBVIEW2_VERSION $WebView2Version has no hash entry in $LockFile"
}
$ExpectedSha256 = ([string]$VersionLock.sha256).ToLowerInvariant()
if ($ExpectedSha256 -notmatch '^[0-9a-f]{64}$') {
    throw "Invalid SHA-256 for WebView2 SDK $WebView2Version in $LockFile"
}
$ExpectedSize = [long]$VersionLock.packageSize
if ($ExpectedSize -le 0) {
    throw "Invalid package size for WebView2 SDK $WebView2Version in $LockFile"
}

$PackageId = [string]$Lock.packageId
$PackageIdLower = $PackageId.ToLowerInvariant()
$VersionLower = $WebView2Version.ToLowerInvariant()
$FeedBaseUrl = ([string]$Lock.feedBaseUrl).TrimEnd('/')
$PackageUri = [uri]("{0}/{1}/{2}/{1}.{2}.nupkg" -f
    $FeedBaseUrl, $PackageIdLower, $VersionLower)
if ($PackageUri.Scheme -ne "https" -or $PackageUri.Host -ne "api.nuget.org") {
    throw "WebView2 SDK downloads must use the official HTTPS NuGet feed: $PackageUri"
}

$TargetDirectory = Join-Path $PackagesRoot "$PackageId.$WebView2Version"
$RequiredFiles = @(
    "LICENSE.txt",
    "NOTICE.txt",
    "build/native/include/WebView2.h",
    "build/native/include/WebView2EnvironmentOptions.h",
    "build/native/x64/WebView2Loader.dll",
    "build/native/x64/WebView2LoaderStatic.lib",
    "build/native/arm64/WebView2Loader.dll",
    "build/native/arm64/WebView2LoaderStatic.lib"
)

function Get-MissingWebView2Files {
    param([Parameter(Mandatory = $true)][string]$Directory)

    return @($RequiredFiles | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $Directory $_) -PathType Leaf)
    })
}

function New-WebView2Result {
    return [pscustomobject]@{
        Version = $WebView2Version
        TargetDirectory = $TargetDirectory
        PackageUri = $PackageUri.AbsoluteUri
        Sha256 = $ExpectedSha256
    }
}

$MissingFiles = Get-MissingWebView2Files -Directory $TargetDirectory
if ($MissingFiles.Count -eq 0) {
    Write-Host "WebView2 SDK $WebView2Version is ready: $TargetDirectory"
    New-WebView2Result
    return
}
if (Test-Path -LiteralPath $TargetDirectory) {
    throw "WebView2 SDK directory is incomplete; refusing to merge into it: $TargetDirectory (missing: $($MissingFiles -join ', '))"
}

New-Item -ItemType Directory -Path $PackagesRoot -Force | Out-Null
$RestoreLeaf = ".webview2-restore-$([guid]::NewGuid().ToString('N'))"
$RestoreDirectory = Join-Path $PackagesRoot $RestoreLeaf
$NupkgPath = Join-Path $RestoreDirectory "$PackageIdLower.$VersionLower.nupkg"
$ExtractDirectory = Join-Path $RestoreDirectory "content"

try {
    New-Item -ItemType Directory -Path $RestoreDirectory | Out-Null
    Write-Host "Downloading pinned WebView2 SDK $WebView2Version from $PackageUri"
    Invoke-WebRequest -Uri $PackageUri -OutFile $NupkgPath -MaximumRedirection 5 -TimeoutSec 300

    $ActualSize = (Get-Item -LiteralPath $NupkgPath).Length
    if ($ActualSize -ne $ExpectedSize) {
        throw "WebView2 SDK package size mismatch: expected $ExpectedSize, got $ActualSize"
    }
    $ActualSha256 = (Get-FileHash -LiteralPath $NupkgPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($ActualSha256 -cne $ExpectedSha256) {
        throw "WebView2 SDK package SHA-256 mismatch: expected $ExpectedSha256, got $ActualSha256"
    }

    [System.IO.Compression.ZipFile]::ExtractToDirectory($NupkgPath, $ExtractDirectory)
    $MissingExtractedFiles = Get-MissingWebView2Files -Directory $ExtractDirectory
    if ($MissingExtractedFiles.Count -gt 0) {
        throw "Verified WebView2 SDK package is incomplete (missing: $($MissingExtractedFiles -join ', '))"
    }

    # Another concurrent build may have completed the same restore while this
    # download was in flight. Reuse it only when its complete SDK surface exists.
    if (Test-Path -LiteralPath $TargetDirectory) {
        $ConcurrentMissingFiles = Get-MissingWebView2Files -Directory $TargetDirectory
        if ($ConcurrentMissingFiles.Count -gt 0) {
            throw "A concurrent restore left an incomplete SDK directory: $TargetDirectory"
        }
    } else {
        Move-Item -LiteralPath $ExtractDirectory -Destination $TargetDirectory
    }

    Write-Host "Restored WebView2 SDK ${WebView2Version}: $TargetDirectory"
    New-WebView2Result
} finally {
    if (Test-Path -LiteralPath $RestoreDirectory) {
        $ResolvedRestore = [System.IO.Path]::GetFullPath($RestoreDirectory)
        $ResolvedPackages = $PackagesRoot.TrimEnd(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar)
        $ActualParent = [System.IO.Path]::GetDirectoryName($ResolvedRestore).TrimEnd(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar)
        if ($ActualParent.Equals($ResolvedPackages, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $ResolvedRestore).StartsWith(".webview2-restore-", [System.StringComparison]::Ordinal)) {
            Remove-Item -LiteralPath $ResolvedRestore -Recurse -Force
        } else {
            Write-Warning "Refusing to clean an unexpected WebView2 restore directory: $ResolvedRestore"
        }
    }
}
