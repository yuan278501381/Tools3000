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
$AutoDiscoveredCert = $false
$TimestampUrl = if ($env:TOOLS3000_TIMESTAMP_URL) {
    $env:TOOLS3000_TIMESTAMP_URL
} else {
    "http://timestamp.digicert.com"
}

if ([string]::IsNullOrWhiteSpace($Thumbprint) -and [string]::IsNullOrWhiteSpace($PfxPath) -and
    $env:CI -ne 'true' -and $env:GITHUB_ACTIONS -ne 'true') {
    $Now = Get-Date
    $ValidCerts = @(Get-ChildItem -Path Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue | Where-Object {
        if (-not $_.HasPrivateKey -or $_.NotBefore -gt $Now -or $_.NotAfter -le $Now) {
            return $false
        }
        $Chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
        try {
            return $Chain.Build($_)
        } finally {
            $Chain.Dispose()
        }
    } | Sort-Object `
        @{ Expression = { if ($_.Subject -match 'Tools3000|yuan278501381|EasyTools') { 0 } else { 1 } }; Ascending = $true }, `
        @{ Expression = { $_.NotAfter }; Descending = $true })

    if ($ValidCerts.Count -gt 0) {
        $Thumbprint = $ValidCerts[0].Thumbprint
        $AutoDiscoveredCert = $true
        Write-Host "[SIGN] [INFO] 自动挂载本机 CurrentUser\My 受信代码签名证书: $($ValidCerts[0].Subject) ($Thumbprint)" -ForegroundColor Cyan
    }
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
        "未配置 TOOLS3000_SIGNING_CERT_SHA1 / TOOLS3000_SIGNING_PFX 且本机 CurrentUser\My 无受信代码签名证书"
    } else {
        "找不到 signtool.exe"
    }
    if ($Required) { throw "发布签名是强制门禁，但$Reason。" }
    Write-Host "[SIGN] [WARN] $Reason；开发构建跳过 Authenticode 签名。" -ForegroundColor Yellow
    return
}

if ($PfxPath -and -not (Test-Path -LiteralPath $PfxPath)) {
    throw "签名证书文件不存在: $PfxPath"
}

foreach ($Path in $Paths) {
    $ResolvedPath = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $BaseIdentityArgs = @()
    if ($Thumbprint) {
        $NormalizedThumbprint = $Thumbprint -replace '\s', ''
        if ($NormalizedThumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
            throw "TOOLS3000_SIGNING_CERT_SHA1 必须是 40 位 SHA-1 证书指纹。"
        }
        $BaseIdentityArgs += @("/sha1", $NormalizedThumbprint)
    } else {
        $BaseIdentityArgs += @("/f", $PfxPath)
        if ($PfxPassword) { $BaseIdentityArgs += @("/p", $PfxPassword) }
    }

    $Arguments = @("sign", "/fd", "SHA256", "/tr", $TimestampUrl, "/td", "SHA256",
        "/d", "Tools3000", "/v") + $BaseIdentityArgs + @($ResolvedPath)

    Write-Host "[SIGN] Signing $ResolvedPath"
    & $SignTool @Arguments
    $SignedWithTimestamp = ($LASTEXITCODE -eq 0)

    if (-not $SignedWithTimestamp -and -not $Required -and $AutoDiscoveredCert) {
        Write-Host "[SIGN] [WARN] 时间戳服务 ($TimestampUrl) 暂不可达，开发构建回退为本地无时间戳 SHA256 签名: $ResolvedPath" -ForegroundColor Yellow
        $FallbackArgs = @("sign", "/fd", "SHA256", "/d", "Tools3000", "/v") + $BaseIdentityArgs + @($ResolvedPath)
        & $SignTool @FallbackArgs
        if ($LASTEXITCODE -ne 0) { throw "本地回退签名失败: $ResolvedPath" }
    } elseif (-not $SignedWithTimestamp) {
        throw "签名失败: $ResolvedPath"
    }

    if ($SignedWithTimestamp) {
        & $SignTool verify /pa /all /tw $ResolvedPath
    } else {
        & $SignTool verify /pa /all $ResolvedPath
    }
    if ($LASTEXITCODE -ne 0) { throw "签名验证失败: $ResolvedPath" }

    # SignTool documents /tw as a warning when a timestamp is absent. Warnings
    # are not a sufficient release gate, so inspect the resulting Authenticode
    # signature and require both a valid signer and a timestamp certificate when Required.
    $Signature = Get-AuthenticodeSignature -LiteralPath $ResolvedPath
    if ($Signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw "Authenticode 签名状态无效: $ResolvedPath ($($Signature.Status): $($Signature.StatusMessage))"
    }
    if ($Required -and -not $Signature.TimeStamperCertificate) {
        throw "Authenticode 签名缺少 RFC 3161 时间戳: $ResolvedPath"
    }
}
