#!/usr/bin/env pwsh
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Paths,
    [switch]$Required
)

$ErrorActionPreference = "Stop"
$Thumbprint = $env:TOOLS3000_SIGNING_CERT_SHA1
$PfxPath = $env:TOOLS3000_SIGNING_PFX
$PfxPassword = $env:TOOLS3000_SIGNING_PASSWORD
$TimestampUrl = if ($env:TOOLS3000_TIMESTAMP_URL) {
    $env:TOOLS3000_TIMESTAMP_URL
} else {
    "http://timestamp.digicert.com"
}

$SignToolCommand = Get-Command "signtool.exe" -ErrorAction SilentlyContinue
$SignTool = if ($SignToolCommand) { $SignToolCommand.Source } else { $null }
if (-not $SignTool) {
    $WindowsKitsBin = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path -LiteralPath $WindowsKitsBin) {
        $SignTool = Get-ChildItem -LiteralPath $WindowsKitsBin -Filter "signtool.exe" -File -Recurse |
            Where-Object { $_.Directory.Name -eq "x64" } |
            Sort-Object FullName -Descending |
            Select-Object -First 1 -ExpandProperty FullName
    }
}

$HasIdentity = -not [string]::IsNullOrWhiteSpace($Thumbprint) -or
    -not [string]::IsNullOrWhiteSpace($PfxPath)
if (-not $HasIdentity -or -not $SignTool) {
    $Reason = if (-not $HasIdentity) {
        "未配置 TOOLS3000_SIGNING_CERT_SHA1 或 TOOLS3000_SIGNING_PFX"
    } else {
        "找不到 signtool.exe"
    }
    if ($Required) { throw "发布签名是强制门禁，但$Reason。" }
    Write-Host "[SIGN] $Reason；开发构建跳过 Authenticode 签名。" -ForegroundColor Yellow
    return
}

if ($PfxPath -and -not (Test-Path -LiteralPath $PfxPath)) {
    throw "签名证书文件不存在: $PfxPath"
}

foreach ($Path in $Paths) {
    $ResolvedPath = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $Arguments = @("sign", "/fd", "SHA256", "/tr", $TimestampUrl, "/td", "SHA256",
        "/d", "Tools3000", "/v")
    if ($Thumbprint) {
        $NormalizedThumbprint = $Thumbprint -replace '\s', ''
        if ($NormalizedThumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
            throw "TOOLS3000_SIGNING_CERT_SHA1 必须是 40 位 SHA-1 证书指纹。"
        }
        $Arguments += @("/sha1", $NormalizedThumbprint)
    } else {
        $Arguments += @("/f", $PfxPath)
        if ($PfxPassword) { $Arguments += @("/p", $PfxPassword) }
    }
    $Arguments += $ResolvedPath

    Write-Host "[SIGN] Signing $ResolvedPath"
    & $SignTool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "签名失败: $ResolvedPath" }

    & $SignTool verify /pa /all /tw $ResolvedPath
    if ($LASTEXITCODE -ne 0) { throw "签名验证失败: $ResolvedPath" }

    # SignTool documents /tw as a warning when a timestamp is absent. Warnings
    # are not a sufficient release gate, so inspect the resulting Authenticode
    # signature and require both a valid signer and a timestamp certificate.
    $Signature = Get-AuthenticodeSignature -LiteralPath $ResolvedPath
    if ($Signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw "Authenticode 签名状态无效: $ResolvedPath ($($Signature.Status): $($Signature.StatusMessage))"
    }
    if (-not $Signature.TimeStamperCertificate) {
        throw "Authenticode 签名缺少 RFC 3161 时间戳: $ResolvedPath"
    }
}
