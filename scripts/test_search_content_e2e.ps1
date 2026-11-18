# -----------------------------------------------------------------------------
# test_search_content_e2e.ps1 — Tools3000 文件搜索与文档内容全文搜索跨进程端到端门禁
# -----------------------------------------------------------------------------
# 在真实 NTFS 卷上创建隔离测试语料库（含 .ch / .md / .cpp 等中英文样本文件），
# 拉起隔离便携实例并按需唤醒真实的 Tools3000_Service.exe 进程，通过原生 Windows
# Named Pipe (4 字节小端帧头协议) 跨进程执行端到端断言：
# [1/4] DEMAND_START 契约校验：主程序启动期间搜索服务绝不自启；显式唤起搜索后启动且 getDbStats <500ms 就绪
# [2/4] 文件名搜索 E2E：多关键词中文检索 ("星云科技 阶梯价") 与拼音首字母检索 ("xykj cgjtj") 精准命中目标文件
# [3/4] 文档内容全文搜索 E2E (content:)：跨进程驱动 Rust SIMD + C++ 提取器精准命中 .ch 与 .md 正文并返回高亮 snippets
# [4/4] 会话常驻与干净收割：主进程退出后搜索服务保持会话常驻且管道继续可用，随后安全清理沙箱
# -----------------------------------------------------------------------------
param(
    [string]$ExePath = ""
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Stop"

Write-Host "===============================================================================" -ForegroundColor Cyan
Write-Host " Tools3000 文件搜索与文档内容搜索 (Named Pipe IPC) 操作系统级端到端门禁 " -ForegroundColor Cyan
Write-Host "===============================================================================" -ForegroundColor Cyan

$ProjectRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $candidate1 = Join-Path $ProjectRoot "deploy_dist\Tools3000.exe"
    $candidate2 = Join-Path $ProjectRoot "build\bin\Release\Tools3000.exe"
    if (Test-Path -LiteralPath $candidate1) {
        $ExePath = $candidate1
    } elseif (Test-Path -LiteralPath $candidate2) {
        $ExePath = $candidate2
    } else {
        Write-Host "[ERROR] 未找到 Tools3000.exe，请先编译或部署！" -ForegroundColor Red
        exit 1
    }
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
$BinDir = Split-Path -Parent $ExePath
$ServiceExePath = [System.IO.Path]::GetFullPath((Join-Path $BinDir "Tools3000_Service.exe"))
if (-not (Test-Path -LiteralPath $ServiceExePath)) {
    throw "未找到配套搜索服务可执行文件: $ServiceExePath"
}
Write-Host "[INFO] 待测主程序: $ExePath" -ForegroundColor Gray
Write-Host "[INFO] 待测搜索服务: $ServiceExePath" -ForegroundColor Gray

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class SearchE2EHarness {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", SetLastError=true)] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool RegisterHotKey(IntPtr hWnd, int id, uint modifiers, uint virtualKey);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool UnregisterHotKey(IntPtr hWnd, int id);

    public static IntPtr FindMessageWindowForProcess(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint wndPid;
            GetWindowThreadProcessId(h, out wndPid);
            if (wndPid != pid) return true;
            var sb = new StringBuilder(256);
            GetClassNameW(h, sb, 256);
            if (sb.ToString().IndexOf("MessageWindow", StringComparison.OrdinalIgnoreCase) >= 0) {
                found = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

function Get-IdleSearchHotkey {
    $modifiers = [uint32](0x0001 -bor 0x0002 -bor 0x0004 -bor 0x4000)
    foreach ($cand in @(
        @{ Text = 'Ctrl+Alt+Shift+F24'; Key = [uint32]0x87 },
        @{ Text = 'Ctrl+Alt+Shift+F23'; Key = [uint32]0x86 },
        @{ Text = 'Ctrl+Alt+Shift+F21'; Key = [uint32]0x84 }
    )) {
        if ([SearchE2EHarness]::RegisterHotKey([IntPtr]::Zero, 0x7350, $modifiers, $cand.Key)) {
            [void][SearchE2EHarness]::UnregisterHotKey([IntPtr]::Zero, 0x7350)
            return $cand.Text
        }
    }
    return "Ctrl+Alt+Shift+F24"
}

$UniqueTag = "$PID" + "_" + ([Guid]::NewGuid().ToString("N").Substring(0, 6))
$PipeToken = [Guid]::NewGuid().ToString("N").ToLowerInvariant() # 32 hex chars
$PipeShortName = "Tools3000SearchPipe-$PipeToken"

$HarnessRoot = Join-Path $ProjectRoot "build\search-content-e2e-$PID"
$HarnessLocalAppData = Join-Path $HarnessRoot "LocalAppData"
$HarnessRoamingAppData = Join-Path $HarnessRoot "RoamingAppData"
$HarnessDataRoot = Join-Path $HarnessRoot "Tools3000Data"
$HarnessConfigDir = Join-Path $HarnessDataRoot "config"
$HarnessLogsDir = Join-Path $HarnessDataRoot "logs"
$CorpusDir = Join-Path $HarnessRoot "Corpus_$UniqueTag"
New-Item -ItemType Directory -Path $HarnessLocalAppData, $HarnessRoamingAppData, $HarnessDataRoot, $HarnessConfigDir, $HarnessLogsDir, $CorpusDir -Force | Out-Null

# 写入真实多格式测试样本语料（触发 NTFS USN 实时变更与 MFT 收录）
$ChFileName = "星云科技-采购阶梯价_${UniqueTag}.ch"
$ChFilePath = Join-Path $CorpusDir $ChFileName
$ChSentinel = "T3K_CH_LADDER_SENTINEL_$UniqueTag"
$ChContent = @"
<?xml version="1.0" encoding="UTF-8"?>
<UserFormDefine Name="星云科技采购阶梯价配置" Token="$ChSentinel">
  <ChooseListSQL content="SELECT ItemCode, Price FROM ITM1 WHERE Token = '$ChSentinel'" />
</UserFormDefine>
"@
[System.IO.File]::WriteAllText($ChFilePath, $ChContent, [System.Text.UTF8Encoding]::new($false))

$MdFileName = "星云高可用网关部署手册_${UniqueTag}.md"
$MdFilePath = Join-Path $CorpusDir $MdFileName
$MdSentinel = "星云零信任内网穿透专线架构_$UniqueTag"
$MdContent = @"
# 星云网关技术白皮书

本文档定义了核心控制面：$MdSentinel，支持毫秒级故障自愈切换与全链路追踪。
"@
[System.IO.File]::WriteAllText($MdFilePath, $MdContent, [System.Text.UTF8Encoding]::new($false))

$CorpusDrive = ([System.IO.Path]::GetPathRoot($CorpusDir)).Substring(0, 1).ToUpperInvariant()
$SearchHotkeyText = Get-IdleSearchHotkey
$HarnessConfigPath = Join-Path $HarnessConfigDir "config.json"
$ConfigObj = @{
    plugins = @{
        search  = @{ enabled = $true }
        capture = @{ enabled = $false }
        gesture = @{ enabled = $false }
        keycast = @{ enabled = $false }
    }
    hotkeys = @{
        'Toggle Search' = $SearchHotkeyText
    }
    search = @{
        pipeToken           = $PipeToken
        enabledDrives       = $CorpusDrive
        pinyinEnabled       = $true
        autoBypassFullscreen = $false
        customContentFormats = @("ch")
    }
} | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText($HarnessConfigPath, $ConfigObj, [System.Text.UTF8Encoding]::new($false))

function Get-HarnessServiceProcesses {
    return @(Get-Process -Name "Tools3000_Service" -ErrorAction SilentlyContinue | Where-Object {
        try {
            ([System.IO.Path]::GetFullPath([string]$_.Path)).Equals($ServiceExePath, [System.StringComparison]::OrdinalIgnoreCase)
        } catch { $false }
    })
}

function Invoke-SearchPipeRpc([string]$PipeName, [hashtable]$PayloadObj, [int]$ConnectTimeoutMs = 4000) {
    $jsonText = $PayloadObj | ConvertTo-Json -Depth 6 -Compress
    $reqBytes = [System.Text.Encoding]::UTF8.GetBytes($jsonText)
    $headerBytes = [System.BitConverter]::GetBytes([uint32]$reqBytes.Length)

    $client = [System.IO.Pipes.NamedPipeClientStream]::new(
        ".",
        $PipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None,
        [System.Security.Principal.TokenImpersonationLevel]::Impersonation
    )
    try {
        $client.Connect($ConnectTimeoutMs)
        $client.Write($headerBytes, 0, 4)
        $client.Write($reqBytes, 0, $reqBytes.Length)
        $client.Flush()

        $respHeader = New-Object byte[] 4
        $readHdr = 0
        while ($readHdr -lt 4) {
            $n = $client.Read($respHeader, $readHdr, 4 - $readHdr)
            if ($n -le 0) { throw "搜索管道在读取响应长度头时提前断开" }
            $readHdr += $n
        }
        $respLen = [System.BitConverter]::ToUInt32($respHeader, 0)
        if ($respLen -eq 0 -or $respLen -gt 67108864) {
            throw "搜索管道返回非法帧长度: $respLen"
        }

        $respBuf = New-Object byte[] $respLen
        $readBody = 0
        while ($readBody -lt $respLen) {
            $n = $client.Read($respBuf, $readBody, [int]($respLen - $readBody))
            if ($n -le 0) { throw "搜索管道在读取响应体时提前断开 ($readBody / $respLen)" }
            $readBody += $n
        }
        $respJson = [System.Text.Encoding]::UTF8.GetString($respBuf)
        return ($respJson | ConvertFrom-Json)
    } finally {
        $client.Dispose()
    }
}

function Get-LogTextSafe([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return "" }
    try {
        $fs = [System.IO.FileStream]::new($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $sr = [System.IO.StreamReader]::new($fs, [System.Text.Encoding]::UTF8)
        $txt = $sr.ReadToEnd()
        $sr.Dispose()
        $fs.Dispose()
        return $txt
    } catch {
        return ""
    }
}

$HarnessLogFile = Join-Path $HarnessLogsDir "tools3000.log"
$proc = $null

try {
    # 清理可能残留的同二进制测试服务进程（绝不触碰 Program Files 中 SCM 托管的正式服务）
    Get-HarnessServiceProcesses | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300

    # -------------------------------------------------------------------------
    # [1/4] 启动主程序校验 DEMAND_START 休眠 -> 触发搜索热键唤醒服务并等待索引就绪
    # -------------------------------------------------------------------------
    Write-Host "`n── [1/4] E2E DEMAND_START 按需唤醒与 getDbStats 低延迟健康探针 ──" -ForegroundColor Cyan
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $ExePath
    $psi.WorkingDirectory = $BinDir
    $psi.UseShellExecute = $false
    $psi.Arguments = "--no-elevate --force-portable-search-service --lifecycle-test-instance"
    $psi.EnvironmentVariables["LOCALAPPDATA"] = $HarnessLocalAppData
    $psi.EnvironmentVariables["APPDATA"] = $HarnessRoamingAppData
    $psi.EnvironmentVariables["TOOLS3000_DATA_ROOT"] = $HarnessDataRoot
    $psi.EnvironmentVariables["TOOLS3000_TEST_ONLY_DRIVE"] = $CorpusDrive

    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $proc -or $proc.HasExited) {
        throw "[1/4] 待测主进程启动失败！"
    }
    Start-Sleep -Milliseconds 1500
    if ((Get-HarnessServiceProcesses).Count -gt 0) {
        throw "[1/4] 违反 DEMAND_START 契约：主进程冷启动阶段未唤起搜索却提前拉起了 Tools3000_Service.exe！"
    }
    Write-Host "  [OK] 冷启动静默断言通过：Tools3000_Service 保持休眠未自启。" -ForegroundColor Green

    # 读取 Toggle Search 热键 ID 并投递 WM_HOTKEY 显式唤起搜索窗口
    $searchHkId = -1
    for ($i = 0; $i -lt 30; $i++) {
        $logTxt = Get-LogTextSafe $HarnessLogFile
        if ($logTxt.Length -gt 0) {
            $m = [regex]::Matches($logTxt, 'name=Toggle Search,\s*def=.*\bid=(\d+)')
            if ($m.Count -gt 0) {
                $searchHkId = [int]$m[$m.Count - 1].Groups[1].Value
                break
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($searchHkId -lt 0) {
        throw "[1/4] 未能获取 Toggle Search 快捷键注册 ID！"
    }

    $msgWnd = [SearchE2EHarness]::FindMessageWindowForProcess([uint32]$proc.Id)
    [void][SearchE2EHarness]::PostMessageW($msgWnd, 0x0312, [IntPtr]$searchHkId, [IntPtr]::Zero)

    # 等待隔离搜索服务进程启动并完成卷 MFT 索引构建
    $indexReady = $false
    $readySw = [System.Diagnostics.Stopwatch]::StartNew()
    $totalIndexedFiles = 0
    $lastProbeErr = ""
    while ($readySw.Elapsed.TotalSeconds -lt 90) {
        try {
            $probeRes = Invoke-SearchPipeRpc -PipeName $PipeShortName -PayloadObj @{
                query      = $UniqueTag
                searchMode = "name"
                drives     = @($CorpusDrive)
                limit      = 5
            } -ConnectTimeoutMs 1500
            if ($probeRes.error -and -not $probeRes.initializing) {
                $lastProbeErr = [string]$probeRes.error
            } elseif (-not $probeRes.initializing) {
                $indexReady = $true
                break
            } else {
                $lastProbeErr = "initializing=true"
            }
        } catch {
            $lastProbeErr = $_.Exception.Message
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $indexReady) {
        $svcCount = (Get-HarnessServiceProcesses).Count
        Get-ChildItem -LiteralPath $HarnessLogsDir -Filter "*.log" -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host "=== LOG: $($_.Name) ===" -ForegroundColor Yellow
            Get-Content -LiteralPath $_.FullName -Tail 35 -Encoding UTF8 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkYellow }
        }
        throw "[1/4] 唤起搜索后 Tools3000_Service 未在 90 秒内完成索引就绪！(svcCount=$svcCount, lastErr=$lastProbeErr)"
    }

    # 验证 getDbStats 非阻塞纳秒锁分离后的毫秒级响应 (< 500ms)
    $statsSw = [System.Diagnostics.Stopwatch]::StartNew()
    $statsRes = Invoke-SearchPipeRpc -PipeName $PipeShortName -PayloadObj @{ action = "getDbStats" } -ConnectTimeoutMs 1500
    $statsSw.Stop()
    if ($statsSw.ElapsedMilliseconds -gt 500) {
        throw "[1/4] getDbStats 响应过慢 ($($statsSw.ElapsedMilliseconds) ms > 500ms)，疑似存在锁竞争！"
    }
    $totalIndexedFiles = [int64]$statsRes.totalRecords
    if ($totalIndexedFiles -lt 100) {
        throw "[1/4] 索引总文件数异常偏低: $totalIndexedFiles"
    }
    Write-Host ("  [OK] 搜索服务按需唤醒并完成索引 ({0:N0} 个文件, getDbStats 耗时 {1} ms)！" -f $totalIndexedFiles, $statsSw.ElapsedMilliseconds) -ForegroundColor Green

    # 触发一次轻量增量追赶确保刚创建的语料文件已 100% 汇入内存索引
    [void](Invoke-SearchPipeRpc -PipeName $PipeShortName -PayloadObj @{ action = "catchup" } -ConnectTimeoutMs 3000)

    # -------------------------------------------------------------------------
    # [2/4] 多关键词中文文件名检索 + 拼音首字母检索端到端验证
    # -------------------------------------------------------------------------
    Write-Host "`n── [2/4] E2E 多关键词中文文件名搜索与拼音首字母搜索断言 ──" -ForegroundColor Cyan
    $zhNameQuery = "星云科技 阶梯价 $UniqueTag"
    $zhNameRes = Invoke-SearchPipeRpc -PipeName $PipeShortName -PayloadObj @{
        query         = $zhNameQuery
        searchMode    = "name"
        pinyinEnabled = $true
        drives        = @($CorpusDrive)
        limit         = 20
    }
    $matchedChByZh = @($zhNameRes.results | Where-Object { $_.name -eq $ChFileName })
    if ($matchedChByZh.Count -eq 0) {
        throw "[2/4] 多词中文搜索 '$zhNameQuery' 未命中目标文件 $ChFileName！"
    }
    Write-Host "  [OK] 多关键词中文搜索精准命中: $($matchedChByZh[0].path) (耗时 $($zhNameRes.elapsedMs) ms)" -ForegroundColor Green

    $pyNameQuery = "xykj cgjtj $UniqueTag"
    $pyNameRes = Invoke-SearchPipeRpc -PipeName $PipeShortName -PayloadObj @{
        query         = $pyNameQuery
        searchMode    = "name"
        pinyinEnabled = $true
        drives        = @($CorpusDrive)
        limit         = 20
    }
    $matchedChByPinyin = @($pyNameRes.results | Where-Object { $_.name -eq $ChFileName })
    if ($matchedChByPinyin.Count -eq 0) {
        throw "[2/4] 拼音首字母搜索 '$pyNameQuery' 未命中目标文件 $ChFileName！"
    }
    Write-Host "  [OK] 拼音首字母搜索精准命中: $($matchedChByPinyin[0].name) (耗时 $($pyNameRes.elapsedMs) ms)" -ForegroundColor Green

    # -------------------------------------------------------------------------
    # [3/4] 文档内容全文搜索 (content:) 跨 .ch 与 .md 双格式与高亮 snippets 验证
    # -------------------------------------------------------------------------
    Write-Host "`n── [3/4] E2E 文档内容全文搜索 (content: 跨 .ch 与 .md + snippets 摘要) ──" -ForegroundColor Cyan
    $chContentQuery = "`"$CorpusDir`" content:$ChSentinel"
    $chContentRes = Invoke-SearchPipeRpc -PipeName $PipeShortName -PayloadObj @{
        query             = $chContentQuery
        searchMode        = "content"
        drives            = @($CorpusDrive)
        contentCustomExts = @("ch")
        limit             = 20
    } -ConnectTimeoutMs 10000
    $hitChDoc = @($chContentRes.results | Where-Object { $_.name -eq $ChFileName })
    if ($hitChDoc.Count -eq 0) {
        throw "[3/4] .ch 源码全文检索 '$chContentQuery' 未命中 $ChFileName！"
    }
    $chSnippetsJson = ($hitChDoc[0].snippets | ConvertTo-Json -Compress)
    if ($chSnippetsJson -notmatch [regex]::Escape($ChSentinel)) {
        throw "[3/4] .ch 全文检索命中结果缺失匹配行上下文 snippets: $chSnippetsJson"
    }
    Write-Host "  [OK] .ch 自定义格式正文全文检索命中并返回精确代码行 snippets！" -ForegroundColor Green

    $mdContentQuery = "`"$CorpusDir`" content:$MdSentinel"
    $mdContentRes = Invoke-SearchPipeRpc -PipeName $PipeShortName -PayloadObj @{
        query      = $mdContentQuery
        searchMode = "content"
        drives     = @($CorpusDrive)
        limit      = 20
    } -ConnectTimeoutMs 10000
    $hitMdDoc = @($mdContentRes.results | Where-Object { $_.name -eq $MdFileName })
    if ($hitMdDoc.Count -eq 0) {
        throw "[3/4] Markdown 中文正文全文检索 '$mdContentQuery' 未命中 $MdFileName！"
    }
    $mdSnippetsJson = ($hitMdDoc[0].snippets | ConvertTo-Json -Compress)
    if ($mdSnippetsJson -notmatch [regex]::Escape($MdSentinel)) {
        throw "[3/4] Markdown 中文正文检索缺失匹配行上下文 snippets: $mdSnippetsJson"
    }
    Write-Host "  [OK] Markdown 中文正文全文检索 (Rust SIMD + UTF-8 管线) 精准命中并返回中文摘要！" -ForegroundColor Green

    # -------------------------------------------------------------------------
    # [4/4] 主程序退出后服务会话常驻校验与隔离环境干净收割
    # -------------------------------------------------------------------------
    Write-Host "`n── [4/4] E2E 主程序退出后搜索服务会话常驻契约校验 ──" -ForegroundColor Cyan
    [void][SearchE2EHarness]::PostMessageW($msgWnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    [void]$proc.WaitForExit(4000)
    Start-Sleep -Milliseconds 300

    $survivingSvc = Get-HarnessServiceProcesses
    if ($survivingSvc.Count -eq 0) {
        throw "[4/4] 主程序退出时误杀了本会话已按需唤醒的 Tools3000_Service.exe！"
    }
    $postExitProbe = Invoke-SearchPipeRpc -PipeName $PipeShortName -PayloadObj @{ action = "getDbStats" } -ConnectTimeoutMs 1500
    if ([int64]$postExitProbe.totalRecords -lt 100) {
        throw "[4/4] 主程序退出后常驻搜索服务管道未能正常响应请求！"
    }
    Write-Host "  [OK] 主程序退出后搜索服务稳定常驻且 Named Pipe 持续秒回！" -ForegroundColor Green

    Write-Host "`n===============================================================================" -ForegroundColor Cyan
    Write-Host " [PASS] 文件搜索与文档内容全文搜索跨进程端到端 (4/4 关卡) 100% 全部通过！" -ForegroundColor Green
    Write-Host "===============================================================================" -ForegroundColor Cyan
}
finally {
    if ($proc -and -not $proc.HasExited) {
        try { $proc.Kill(); [void]$proc.WaitForExit(2000) } catch {}
    }
    Get-HarnessServiceProcesses | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 200
    try {
        Remove-Item -LiteralPath $HarnessRoot -Recurse -Force -ErrorAction SilentlyContinue
    } catch {}
}
