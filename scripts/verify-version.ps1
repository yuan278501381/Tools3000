#!/usr/bin/env pwsh
param(
    [string]$Tag = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$VersionFile = Join-Path $ProjectRoot "VERSION"

if (-not (Test-Path -LiteralPath $VersionFile)) {
    throw "Missing product version source: $VersionFile"
}

$Version = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION must contain one stable SemVer value (for example 1.2.3): $Version"
}

$UiPackageJson = Join-Path $ProjectRoot "ui\package.json"
if (Test-Path -LiteralPath $UiPackageJson) {
    $Pkg = Get-Content -LiteralPath $UiPackageJson -Raw | ConvertFrom-Json
    if ([string]$Pkg.version -ne $Version) {
        throw "ui/package.json version ($($Pkg.version)) does not match root VERSION ($Version)"
    }
}

if (-not [string]::IsNullOrWhiteSpace($Tag)) {
    if ($Tag -notmatch '^v\d+\.\d+\.\d+$') {
        throw "Release tag must use vMAJOR.MINOR.PATCH: $Tag"
    }
    if ($Tag.Substring(1) -ne $Version) {
        throw "Release tag $Tag does not match VERSION ($Version)"
    }
}

Write-Output "Tools3000 product version verified: $Version"
