#!/usr/bin/env pwsh
<#
.SYNOPSIS
基于 .NET ZipFile / ZipArchive 的高速确定性 ZIP 打包工具 (替代慢速 Compress-Archive)
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$DestinationPath,
    [string]$SourceDirectory = "",
    [string]$SourceFilesList = "",
    [ValidateSet("Optimal", "Fastest", "NoCompression")]
    [string]$CompressionLevel = "Optimal",
    [string]$ExtraMarkerEntryName = "",
    [string]$ExtraMarkerContent = ""
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$level = [System.IO.Compression.CompressionLevel]::$CompressionLevel
$parentDir = Split-Path -Parent $DestinationPath
if ($parentDir -and -not (Test-Path -LiteralPath $parentDir)) {
    New-Item -ItemType Directory -Path $parentDir -Force | Out-Null
}
if (Test-Path -LiteralPath $DestinationPath) {
    Remove-Item -LiteralPath $DestinationPath -Force
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
if (-not [string]::IsNullOrWhiteSpace($SourceDirectory)) {
    [System.IO.Compression.ZipFile]::CreateFromDirectory($SourceDirectory, $DestinationPath, $level, $false)
    if (-not [string]::IsNullOrWhiteSpace($ExtraMarkerEntryName)) {
        $zip = [System.IO.Compression.ZipFile]::Open($DestinationPath, [System.IO.Compression.ZipArchiveMode]::Update)
        try {
            $existing = $zip.GetEntry($ExtraMarkerEntryName)
            if ($existing) { $existing.Delete() }
            $entry = $zip.CreateEntry($ExtraMarkerEntryName, $level)
            $writer = New-Object System.IO.StreamWriter($entry.Open(), (New-Object System.Text.UTF8Encoding($false)))
            try {
                $writer.Write($ExtraMarkerContent)
            } finally {
                $writer.Dispose()
            }
        } finally {
            $zip.Dispose()
        }
    }
} elseif (-not [string]::IsNullOrWhiteSpace($SourceFilesList)) {
    $sourceFiles = @($SourceFilesList -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $zip = [System.IO.Compression.ZipFile]::Open($DestinationPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $usedNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($f in $sourceFiles) {
            if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { continue }
            $leaf = Split-Path -Leaf $f
            if ($usedNames.Add($leaf)) {
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f, $leaf, $level)
            }
        }
    } finally {
        $zip.Dispose()
    }
} else {
    throw "必须提供 -SourceDirectory 或 -SourceFilesList 之一。"
}
$sw.Stop()
$sizeMb = [math]::Round((Get-Item -LiteralPath $DestinationPath).Length / 1MB, 2)
Write-Host ("[ZIP] [OK] 已高速归档: {0} ({1} MB, {2:N2}s, level={3})" -f (Split-Path -Leaf $DestinationPath), $sizeMb, $sw.Elapsed.TotalSeconds, $CompressionLevel) -ForegroundColor Green
