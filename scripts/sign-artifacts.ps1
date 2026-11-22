#!/usr/bin/env pwsh
<#
.SYNOPSIS
Tools3000 世界级多 Provider Authenticode 代码签名与公网信任审计引擎

.DESCRIPTION
支持策略分发多签名提供方（按公网信任度自动发现或显式指定）：
  1. azure-trusted-signing : 微软官方 Artifact Signing / Trusted Signing（零 SmartScreen 警告，云端短效证书，支持本地 CLI 与 GitHub OIDC）
  2. pfx                   : 商业 OV/EV 代码签名 PFX 证书文件
  3. cert-store            : Windows 证书存储区（硬件 EV USB Token 或本机开发证书）
内置多 RFC 3161 时间戳服务器自动故障切换、公网受信任根 CA 链强断言 (-RequirePublicTrust) 以及只读审计模式 (-VerifyOnly)。
#>
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Paths,
    [ValidateSet("auto", "azure-trusted-signing", "pfx", "cert-store")]
    [string]$Provider = $(if ($env:TOOLS3000_SIGNING_PROVIDER) { $env:TOOLS3000_SIGNING_PROVIDER } else { "auto" }),
    [switch]$Required,
    [switch]$RequirePublicTrust,
    [switch]$VerifyOnly
)

$ErrorActionPreference = "Stop"

# ── 常量定义（零硬编码散落） ──────────────────────────────────────────────────
$AZURE_TSA_URL = "http://timestamp.acs.microsoft.com"
$DEFAULT_TSA_POOL = @(
    "http://timestamp.digicert.com",
    "http://timestamp.sectigo.com",
    "http://rfc3161timestamp.globalsign.com/advanced"
)

if (-not $RequirePublicTrust -and ($env:TOOLS3000_REQUIRE_PUBLIC_TRUST -in @("1", "true", "TRUE"))) {
    $RequirePublicTrust = $true
}

function Resolve-SignToolPath {
    $cmd = Get-Command "signtool.exe" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $windowsKitsBin = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path -LiteralPath $windowsKitsBin) {
        $found = Get-ChildItem -LiteralPath $windowsKitsBin -Filter "signtool.exe" -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.Directory.Name -eq "x64" } |
            Sort-Object FullName -Descending |
            Select-Object -First 1 -ExpandProperty FullName
        if ($found) { return $found }
    }
    return $null
}

function Resolve-AzureDlibPath {
    if (-not [string]::IsNullOrWhiteSpace($env:TOOLS3000_AZURE_DLIB_PATH)) {
        if (-not (Test-Path -LiteralPath $env:TOOLS3000_AZURE_DLIB_PATH)) {
            throw "TOOLS3000_AZURE_DLIB_PATH 指定的文件不存在: $($env:TOOLS3000_AZURE_DLIB_PATH)"
        }
        return (Resolve-Path -LiteralPath $env:TOOLS3000_AZURE_DLIB_PATH).Path
    }

    $repoRoot = Split-Path -Parent $PSScriptRoot
    $searchRoots = @(
        (Join-Path $repoRoot "packages"),
        (Join-Path $env:USERPROFILE ".nuget\packages\microsoft.trusted.signing.client"),
        (Join-Path $env:LOCALAPPDATA "Microsoft\MicrosoftArtifactSigningClientTools"),
        (Join-Path $env:LOCALAPPDATA "Microsoft\MicrosoftTrustedSigningClientTools")
    )
    foreach ($root in $searchRoots) {
        if (-not $root -or -not (Test-Path -LiteralPath $root)) { continue }
        $candidate = Get-ChildItem -LiteralPath $root -Filter "Azure.CodeSigning.Dlib.dll" -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1 -ExpandProperty FullName
        if ($candidate) { return $candidate }
    }
    return $null
}

function Resolve-TimestampUrls {
    param([string]$SelectedProvider)
    if (-not [string]::IsNullOrWhiteSpace($env:TOOLS3000_TIMESTAMP_URL)) {
        return @($env:TOOLS3000_TIMESTAMP_URL) + @($DEFAULT_TSA_POOL | Where-Object { $_ -ne $env:TOOLS3000_TIMESTAMP_URL })
    }
    if ($SelectedProvider -eq "azure-trusted-signing") {
        return @($AZURE_TSA_URL) + $DEFAULT_TSA_POOL
    }
    return $DEFAULT_TSA_POOL
}

$script:LocalMachineRootThumbprints = $null

function Get-LocalMachineRootThumbprints {
    if ($null -ne $script:LocalMachineRootThumbprints) {
        return $script:LocalMachineRootThumbprints
    }
    $set = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $lmRoot = New-Object System.Security.Cryptography.X509Certificates.X509Store(
        [System.Security.Cryptography.X509Certificates.StoreName]::Root,
        [System.Security.Cryptography.X509Certificates.StoreLocation]::LocalMachine
    )
    try {
        $lmRoot.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
        foreach ($c in $lmRoot.Certificates) {
            if (-not [string]::IsNullOrEmpty($c.Thumbprint)) {
                [void]$set.Add($c.Thumbprint)
            }
        }
    } finally {
        $lmRoot.Close()
    }
    $script:LocalMachineRootThumbprints = $set
    return $script:LocalMachineRootThumbprints
}

function Test-CertificatePubliclyTrusted {
    param([System.Security.Cryptography.X509Certificates.X509Certificate2]$Cert)
    if (-not $Cert) { return $false }

    # 自签名证书 (Subject == Issuer) 绝不是公网受信任 CA 签发
    if ($Cert.Subject -eq $Cert.Issuer) {
        return $false
    }

    $chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
    try {
        $chain.ChainPolicy.RevocationMode = [System.Security.Cryptography.X509Certificates.X509RevocationMode]::NoCheck
        $chain.ChainPolicy.VerificationFlags = [System.Security.Cryptography.X509Certificates.X509VerificationFlags]::NoFlag
        if (-not $chain.Build($Cert)) {
            return $false
        }
        $elements = $chain.ChainElements
        if ($elements.Count -lt 2) {
            return $false
        }
        $rootCert = $elements[$elements.Count - 1].Certificate
        $rootSet = Get-LocalMachineRootThumbprints
        return $rootSet.Contains($rootCert.Thumbprint)
    } finally {
        $chain.Dispose()
    }
}

function Assert-SignedArtifact {
    param(
        [string]$FilePath,
        [string]$SignTool,
        [bool]$ExpectTimestamp,
        [bool]$EnforceRequired,
        [bool]$EnforcePublicTrust,
        [bool]$SkipSignToolVerify = $false
    )

    if ($SignTool -and -not $SkipSignToolVerify) {
        if ($ExpectTimestamp) {
            & $SignTool verify /pa /all /tw $FilePath | Out-Null
        } else {
            & $SignTool verify /pa /all $FilePath | Out-Null
        }
        if ($LASTEXITCODE -ne 0) {
            throw "[ERROR] signtool 验签失败: $FilePath"
        }
    }

    $sig = Get-AuthenticodeSignature -LiteralPath $FilePath
    if ($sig.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw "[ERROR] Authenticode 签名状态无效: $FilePath ($($sig.Status): $($sig.StatusMessage))"
    }

    $hasTimestamp = ($null -ne $sig.TimeStamperCertificate)
    if ($EnforceRequired -and -not $hasTimestamp) {
        throw "[ERROR] Authenticode 签名缺少 RFC 3161 时间戳: $FilePath"
    }

    $isPublicTrusted = Test-CertificatePubliclyTrusted -Cert $sig.SignerCertificate
    $trustTag = if ($isPublicTrusted) { "PUBLIC_TRUSTED_CA (SmartScreen Ready)" } else { "LOCAL_OR_SELF_SIGNED" }
    Write-Host "[SIGN] [OK] Verified: $(Split-Path -Leaf $FilePath) | Trust=$trustTag | Subject=$($sig.SignerCertificate.Subject) | Timestamp=$($hasTimestamp)" -ForegroundColor Green

    if ($EnforcePublicTrust -and -not $isPublicTrusted) {
        throw "[ERROR] 公网信任门禁阻断 (-RequirePublicTrust): $FilePath 使用的是非公网 CA / 自签名证书 ($($sig.SignerCertificate.Subject))，在外部设备会触发 SmartScreen 警告！请改用 Azure Trusted Signing / SignPath / 商业 EV 证书。"
    }
}

function Assert-SignedArtifactsBatch {
    param(
        [string[]]$FilePaths,
        [string]$SignTool,
        [bool]$ExpectTimestamp,
        [bool]$EnforceRequired,
        [bool]$EnforcePublicTrust
    )
    if (-not $FilePaths -or $FilePaths.Count -eq 0) { return }

    $batchVerified = $false
    if ($SignTool -and $FilePaths.Count -gt 1) {
        $verifyArgs = if ($ExpectTimestamp) {
            @("verify", "/pa", "/all", "/tw") + $FilePaths
        } else {
            @("verify", "/pa", "/all") + $FilePaths
        }
        & $SignTool @verifyArgs | Out-Null
        if ($LASTEXITCODE -eq 0) {
            $batchVerified = $true
        }
    }

    foreach ($file in $FilePaths) {
        Assert-SignedArtifact -FilePath $file -SignTool $SignTool `
            -ExpectTimestamp:$ExpectTimestamp `
            -EnforceRequired:$EnforceRequired `
            -EnforcePublicTrust:$EnforcePublicTrust `
            -SkipSignToolVerify:$batchVerified
    }
}

function Resolve-SigningContext {
    param([string]$RequestedProvider)

    $hasAzureConfig =
        (-not [string]::IsNullOrWhiteSpace($env:TOOLS3000_AZURE_SIGNING_ENDPOINT)) -and
        (-not [string]::IsNullOrWhiteSpace($env:TOOLS3000_AZURE_SIGNING_ACCOUNT)) -and
        (-not [string]::IsNullOrWhiteSpace($env:TOOLS3000_AZURE_SIGNING_PROFILE))
    $hasPfxConfig = -not [string]::IsNullOrWhiteSpace($env:TOOLS3000_SIGNING_PFX)
    $hasThumbprintConfig = -not [string]::IsNullOrWhiteSpace($env:TOOLS3000_SIGNING_CERT_SHA1)

    $resolvedProvider = $RequestedProvider
    if ($resolvedProvider -eq "auto") {
        if ($hasAzureConfig) {
            $resolvedProvider = "azure-trusted-signing"
        } elseif ($hasPfxConfig) {
            $resolvedProvider = "pfx"
        } elseif ($hasThumbprintConfig) {
            $resolvedProvider = "cert-store"
        } elseif ($env:CI -ne 'true' -and $env:GITHUB_ACTIONS -ne 'true') {
            $resolvedProvider = "cert-store"
        } else {
            return @{ Provider = "none"; Reason = "未配置云端签名凭证 (Azure Trusted Signing) 或 PFX/证书指纹" }
        }
    }

    if ($resolvedProvider -eq "azure-trusted-signing") {
        if (-not $hasAzureConfig) {
            return @{ Provider = "none"; Reason = "已指定 azure-trusted-signing，但缺少 TOOLS3000_AZURE_SIGNING_ENDPOINT / ACCOUNT / PROFILE 环境变量" }
        }
        $dlibPath = Resolve-AzureDlibPath
        if (-not $dlibPath) {
            return @{ Provider = "none"; Reason = "找不到 Azure.CodeSigning.Dlib.dll（请安装 Microsoft.Trusted.Signing.Client 或配置 TOOLS3000_AZURE_DLIB_PATH）" }
        }
        return @{
            Provider            = "azure-trusted-signing"
            Endpoint            = $env:TOOLS3000_AZURE_SIGNING_ENDPOINT.Trim()
            AccountName         = $env:TOOLS3000_AZURE_SIGNING_ACCOUNT.Trim()
            CertProfileName     = $env:TOOLS3000_AZURE_SIGNING_PROFILE.Trim()
            DlibPath            = $dlibPath
            CorrelationId       = if ($env:TOOLS3000_AZURE_CORRELATION_ID) { $env:TOOLS3000_AZURE_CORRELATION_ID } else { "Tools3000-$([guid]::NewGuid().ToString('N').Substring(0, 8))" }
            AutoDiscoveredLocal = $false
        }
    }

    if ($resolvedProvider -eq "pfx") {
        if (-not $hasPfxConfig) {
            return @{ Provider = "none"; Reason = "已指定 pfx 但未配置 TOOLS3000_SIGNING_PFX" }
        }
        if (-not (Test-Path -LiteralPath $env:TOOLS3000_SIGNING_PFX)) {
            throw "签名证书文件不存在: $($env:TOOLS3000_SIGNING_PFX)"
        }
        return @{
            Provider            = "pfx"
            PfxPath             = (Resolve-Path -LiteralPath $env:TOOLS3000_SIGNING_PFX).Path
            PfxPassword         = $env:TOOLS3000_SIGNING_PASSWORD
            AutoDiscoveredLocal = $false
        }
    }

    if ($resolvedProvider -eq "cert-store") {
        $thumbprint = $env:TOOLS3000_SIGNING_CERT_SHA1
        $autoDiscovered = $false
        if ([string]::IsNullOrWhiteSpace($thumbprint) -and $env:CI -ne 'true' -and $env:GITHUB_ACTIONS -ne 'true') {
            $now = Get-Date
            $validCerts = @(Get-ChildItem -Path Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue | Where-Object {
                if (-not $_.HasPrivateKey -or $_.NotBefore -gt $now -or $_.NotAfter -le $now) {
                    return $false
                }
                $chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
                try { return $chain.Build($_) } finally { $chain.Dispose() }
            } | Sort-Object `
                @{ Expression = { if (Test-CertificatePubliclyTrusted -Cert $_) { 0 } else { 1 } }; Ascending = $true }, `
                @{ Expression = { if ($_.Subject -match 'Tools3000|yuan278501381|EasyTools') { 0 } else { 1 } }; Ascending = $true }, `
                @{ Expression = { $_.NotAfter }; Descending = $true })

            if ($validCerts.Count -gt 0) {
                $thumbprint = $validCerts[0].Thumbprint
                $autoDiscovered = $true
                Write-Host "[SIGN] [INFO] 自动挂载本机 CurrentUser\My 受信代码签名证书: $($validCerts[0].Subject) ($thumbprint)" -ForegroundColor Cyan
            }
        }
        if ([string]::IsNullOrWhiteSpace($thumbprint)) {
            return @{ Provider = "none"; Reason = "未配置 TOOLS3000_SIGNING_CERT_SHA1 且本机 CurrentUser\My 无有效代码签名证书" }
        }
        $normalizedThumbprint = $thumbprint -replace '\s', ''
        if ($normalizedThumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
            throw "TOOLS3000_SIGNING_CERT_SHA1 必须是 40 位 SHA-1 证书指纹。"
        }
        return @{
            Provider            = "cert-store"
            Thumbprint          = $normalizedThumbprint
            AutoDiscoveredLocal = $autoDiscovered
        }
    }

    return @{ Provider = "none"; Reason = "不支持的签名策略提供方: $resolvedProvider" }
}

# ── 策略分发处理器表 (Strategy Dispatch) ──────────────────────────────────────
$SigningStrategies = @{
    "azure-trusted-signing" = {
        param([hashtable]$Ctx, [string]$SignTool, [string[]]$ResolvedFiles, [string[]]$TsaUrls)
        $metaObj = [ordered]@{
            Endpoint               = $Ctx.Endpoint
            CodeSigningAccountName = $Ctx.AccountName
            CertificateProfileName = $Ctx.CertProfileName
            CorrelationId          = $Ctx.CorrelationId
        }
        $metaPath = Join-Path $env:TEMP ("tools3000_azure_signing_" + [guid]::NewGuid().ToString("N") + ".json")
        try {
            $metaJson = $metaObj | ConvertTo-Json -Depth 4
            [System.IO.File]::WriteAllText($metaPath, $metaJson, (New-Object System.Text.UTF8Encoding($false)))
            $signedOk = $false
            foreach ($tsa in $TsaUrls) {
                Write-Host "[SIGN] [Azure Trusted Signing] Account=$($Ctx.AccountName) Profile=$($Ctx.CertProfileName) Files=$($ResolvedFiles.Count) TSA=$tsa" -ForegroundColor Cyan
                $argsList = @(
                    "sign", "/v", "/fd", "SHA256",
                    "/tr", $tsa, "/td", "SHA256",
                    "/d", "Tools3000",
                    "/dlib", $Ctx.DlibPath,
                    "/dmdf", $metaPath
                ) + $ResolvedFiles
                & $SignTool @argsList | Out-Host
                if ($LASTEXITCODE -eq 0) {
                    $signedOk = $true
                    break
                }
                Write-Host "[SIGN] [WARN] Azure Trusted Signing 使用 TSA ($tsa) 失败，尝试下一个备用时间戳服务器..." -ForegroundColor Yellow
            }
            if (-not $signedOk) {
                throw "Azure Trusted Signing 批量签名失败（请确认已登录 Azure CLI / OIDC 且具备 Artifact Signing Certificate Profile Signer 角色）。"
            }
            return $true
        } finally {
            Remove-Item -LiteralPath $metaPath -Force -ErrorAction SilentlyContinue
        }
    }

    "pfx" = {
        param([hashtable]$Ctx, [string]$SignTool, [string[]]$ResolvedFiles, [string[]]$TsaUrls)
        $identityArgs = @("/f", $Ctx.PfxPath)
        if ($Ctx.PfxPassword) { $identityArgs += @("/p", $Ctx.PfxPassword) }
        # 优先按批量签名一次提交，显著降低多文件签名进程与 TSA 握手开销
        foreach ($tsa in $TsaUrls) {
            Write-Host "[SIGN] [PFX] Batch signing $($ResolvedFiles.Count) file(s) (TSA=$tsa)" -ForegroundColor Cyan
            $batchArgs = @("sign", "/fd", "SHA256", "/tr", $tsa, "/td", "SHA256", "/d", "Tools3000", "/v") + $identityArgs + $ResolvedFiles
            & $SignTool @batchArgs | Out-Host
            if ($LASTEXITCODE -eq 0) { return $true }
            Write-Host "[SIGN] [WARN] PFX 批量时间戳签名 ($tsa) 未成功，尝试备用 TSA 或逐文件回退..." -ForegroundColor Yellow
        }
        foreach ($file in $ResolvedFiles) {
            $signedWithTs = $false
            foreach ($tsa in $TsaUrls) {
                Write-Host "[SIGN] [PFX] Signing $file (TSA=$tsa)"
                $argsList = @("sign", "/fd", "SHA256", "/tr", $tsa, "/td", "SHA256", "/d", "Tools3000", "/v") + $identityArgs + @($file)
                & $SignTool @argsList | Out-Host
                if ($LASTEXITCODE -eq 0) { $signedWithTs = $true; break }
            }
            if (-not $signedWithTs) {
                throw "PFX 签名失败: $file"
            }
        }
        return $true
    }

    "cert-store" = {
        param([hashtable]$Ctx, [string]$SignTool, [string[]]$ResolvedFiles, [string[]]$TsaUrls)
        $identityArgs = @("/sha1", $Ctx.Thumbprint)
        # 优先按批量签名一次提交，显著降低多 DLL 签名的进程与 TSA 握手开销
        foreach ($tsa in $TsaUrls) {
            Write-Host "[SIGN] [CertStore] Batch signing $($ResolvedFiles.Count) file(s) (TSA=$tsa)" -ForegroundColor Cyan
            $batchArgs = @("sign", "/fd", "SHA256", "/tr", $tsa, "/td", "SHA256", "/d", "Tools3000", "/v") + $identityArgs + $ResolvedFiles
            & $SignTool @batchArgs | Out-Host
            if ($LASTEXITCODE -eq 0) {
                return $true
            }
            Write-Host "[SIGN] [WARN] 批量时间戳签名 ($tsa) 未成功，尝试备用 TSA 或逐文件回退..." -ForegroundColor Yellow
        }

        $allTimestamped = $true
        foreach ($file in $ResolvedFiles) {
            $signedWithTs = $false
            foreach ($tsa in $TsaUrls) {
                Write-Host "[SIGN] Signing $file"
                $argsList = @("sign", "/fd", "SHA256", "/tr", $tsa, "/td", "SHA256", "/d", "Tools3000", "/v") + $identityArgs + @($file)
                & $SignTool @argsList | Out-Host
                if ($LASTEXITCODE -eq 0) { $signedWithTs = $true; break }
            }
            if (-not $signedWithTs) {
                if (-not $Required -and $Ctx.AutoDiscoveredLocal) {
                    Write-Host "[SIGN] [WARN] 全部时间戳服务暂不可达，开发构建回退为本地无时间戳 SHA256 签名: $file" -ForegroundColor Yellow
                    $fallbackArgs = @("sign", "/fd", "SHA256", "/d", "Tools3000", "/v") + $identityArgs + @($file)
                    & $SignTool @fallbackArgs | Out-Host
                    if ($LASTEXITCODE -ne 0) { throw "本地回退签名失败: $file" }
                    $allTimestamped = $false
                } else {
                    throw "签名失败: $file"
                }
            }
        }
        return $allTimestamped
    }
}

# ── 主执行管线 ────────────────────────────────────────────────────────────────
$ResolvedPaths = @()
foreach ($p in $Paths) {
    $ResolvedPaths += (Resolve-Path -LiteralPath $p -ErrorAction Stop).Path
}
$SignToolPath = Resolve-SignToolPath

if ($VerifyOnly) {
    Write-Host "[SIGN] [INFO] 启动只读 Authenticode 信任审计模式 (Files=$($ResolvedPaths.Count), RequirePublicTrust=$RequirePublicTrust)..." -ForegroundColor Cyan
    Assert-SignedArtifactsBatch -FilePaths $ResolvedPaths -SignTool $SignToolPath `
        -ExpectTimestamp:($Required -or $RequirePublicTrust) `
        -EnforceRequired:$Required `
        -EnforcePublicTrust:$RequirePublicTrust
    return
}

$Context = Resolve-SigningContext -RequestedProvider $Provider
if ($Context.Provider -eq "none" -or -not $SignToolPath) {
    $reason = if ($Context.Provider -eq "none") { $Context.Reason } else { "找不到 signtool.exe" }
    if ($Required -or $RequirePublicTrust) {
        throw "发布签名是强制门禁，但$reason。"
    }
    Write-Host "[SIGN] [WARN] $reason；开发构建跳过 Authenticode 签名。" -ForegroundColor Yellow
    return
}

$TsaUrls = Resolve-TimestampUrls -SelectedProvider $Context.Provider
$Strategy = $SigningStrategies[$Context.Provider]
if (-not $Strategy) {
    throw "未注册的签名策略提供方: $($Context.Provider)"
}

$RawStrategyResult = & $Strategy $Context $SignToolPath $ResolvedPaths $TsaUrls
$SignedWithTimestamp = [bool]($RawStrategyResult | Select-Object -Last 1)

Assert-SignedArtifactsBatch -FilePaths $ResolvedPaths -SignTool $SignToolPath `
    -ExpectTimestamp:$SignedWithTimestamp `
    -EnforceRequired:$Required `
    -EnforcePublicTrust:$RequirePublicTrust
