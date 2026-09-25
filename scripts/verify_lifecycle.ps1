# ─────────────────────────────────────────────────────────────────────────────
# verify_lifecycle.ps1 — Tools3000 关键端到端生命周期与防死锁自动化门禁
# ─────────────────────────────────────────────────────────────────────────────
# 覆盖冷启动、截图取消自愈、搜索按需常驻、窗口清单与停机收割。
# ─────────────────────────────────────────────────────────────────────────────
param(
    [string]$BinDirectory = ""
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class LifecycleHarness {
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool RegisterHotKey(IntPtr hWnd, int id, uint modifiers, uint virtualKey);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool UnregisterHotKey(IntPtr hWnd, int id);
    [DllImport("user32.dll", SetLastError=true)] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int max);

    public static IntPtr FindByClassForProcess(string wanted, uint pid) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint windowPid;
            GetWindowThreadProcessId(h, out windowPid);
            if (windowPid != pid) return true;
            var cls = new StringBuilder(256);
            GetClassNameW(h, cls, 256);
            if (cls.ToString() == wanted) { result = h; return false; }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    public static IntPtr FindMessageWindowForProcess(uint pid) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint windowPid;
            GetWindowThreadProcessId(h, out windowPid);
            if (windowPid == pid) {
                var cls = new StringBuilder(256);
                GetClassNameW(h, cls, 256);
                if (cls.ToString().IndexOf("MessageWindow", StringComparison.OrdinalIgnoreCase) >= 0) {
                    result = h;
                    return false;
                }
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    public static List<string> DumpWindowsForProcess(uint pid) {
        var list = new List<string>();
        EnumWindows((h, l) => {
            uint windowPid;
            GetWindowThreadProcessId(h, out windowPid);
            if (windowPid == pid) {
                var cls = new StringBuilder(256);
                GetClassNameW(h, cls, 256);
                var title = new StringBuilder(256);
                GetWindowTextW(h, title, 256);
                bool visible = IsWindowVisible(h);
                list.Add(string.Format("HWND: 0x{0:X8} | Visible: {1} | Class: {2} | Title: {3}", (long)h, visible, cls.ToString(), title.ToString()));
            }
            return true;
        }, IntPtr.Zero);
        return list;
    }

}
"@

function Show-AuditHeader() {
    Write-Host "===============================================================================" -ForegroundColor Cyan
    Write-Host " Tools3000 关键端到端生命周期与防死锁自动化门禁 " -ForegroundColor Cyan
    Write-Host "===============================================================================" -ForegroundColor Cyan
}

Show-AuditHeader

# 1. 使用隔离的测试配置和数据目录，既避免已安装版本的搜索管道干扰，也绝不
# 终止 Program Files 中由 SCM 托管的真实服务。
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ResolvedBinDirectory = if ([string]::IsNullOrWhiteSpace($BinDirectory)) {
    Join-Path $ProjectRoot "deploy_dist"
} elseif ([System.IO.Path]::IsPathRooted($BinDirectory)) {
    $BinDirectory
} else {
    Join-Path $ProjectRoot $BinDirectory
}

function Get-AvailableHarnessHotkeys {
    # Probe the exact Win32 combinations that Tools3000 will register. Keep every
    # successful probe reserved until two distinct keys have been selected, then
    # release them immediately before the test app starts. This avoids colliding
    # with an installed Tools3000 instance without weakening the real hotkey path.
    $modifiers = [uint32](0x0001 -bor 0x0002 -bor 0x0004 -bor 0x4000) # Alt|Ctrl|Shift|NoRepeat
    $candidates = @(
        @{ Text = 'Ctrl+Alt+Shift+F24'; Key = [uint32]0x87 },
        @{ Text = 'Ctrl+Alt+Shift+F23'; Key = [uint32]0x86 },
        @{ Text = 'Ctrl+Alt+Shift+F22'; Key = [uint32]0x85 },
        @{ Text = 'Ctrl+Alt+Shift+F21'; Key = [uint32]0x84 },
        @{ Text = 'Ctrl+Alt+Shift+F20'; Key = [uint32]0x83 },
        @{ Text = 'Ctrl+Alt+Shift+F19'; Key = [uint32]0x82 },
        @{ Text = 'Ctrl+Alt+Shift+F18'; Key = [uint32]0x81 },
        @{ Text = 'Ctrl+Alt+Shift+F17'; Key = [uint32]0x80 }
    )
    $chosen = [System.Collections.Generic.List[object]]::new()
    $registeredIds = [System.Collections.Generic.List[int]]::new()
    try {
        foreach ($candidate in $candidates) {
            $probeId = 0x6E00 + $chosen.Count
            if ([LifecycleHarness]::RegisterHotKey(
                    [IntPtr]::Zero, $probeId, $modifiers, $candidate.Key)) {
                $chosen.Add($candidate)
                $registeredIds.Add($probeId)
                if ($chosen.Count -eq 3) { break }
            }
        }
        if ($chosen.Count -ne 3) {
            throw '无法为隔离生命周期测试找到三个未占用的全局快捷键。'
        }
        return @($chosen)
    } finally {
        foreach ($probeId in $registeredIds) {
            [void][LifecycleHarness]::UnregisterHotKey([IntPtr]::Zero, $probeId)
        }
    }
}
$exePath = Join-Path $ResolvedBinDirectory "Tools3000.exe"
if (-not (Test-Path -LiteralPath $exePath)) {
    Write-Host "[ERROR] 未找到可执行文件 ($exePath)，请先执行 deploy.ps1 构建部署目录！" -ForegroundColor Red
    exit 1
}
$exePath = (Resolve-Path -LiteralPath $exePath).Path
$TestBinDirectory = Split-Path -Parent $exePath
$TestServicePath = [System.IO.Path]::GetFullPath((Join-Path $TestBinDirectory "Tools3000_Service.exe"))
$HarnessRoot = Join-Path $ProjectRoot "build\lifecycle-harness-$PID"
$HarnessLocalAppData = Join-Path $HarnessRoot "LocalAppData"
$HarnessRoamingAppData = Join-Path $HarnessRoot "RoamingAppData"
$HarnessDataRoot = Join-Path $HarnessRoot "Tools3000Data"
New-Item -ItemType Directory -Path $HarnessLocalAppData, $HarnessRoamingAppData, $HarnessDataRoot -Force | Out-Null
$HarnessEnvironment = @{
    LOCALAPPDATA = $HarnessLocalAppData
    APPDATA = $HarnessRoamingAppData
    TOOLS3000_DATA_ROOT = $HarnessDataRoot
}

# 运行时探测低冲突测试热键，并写入隔离配置。真实用户设置既不读取也不修改。
$HarnessConfigDirectory = Join-Path $HarnessDataRoot "config"
New-Item -ItemType Directory -Path $HarnessConfigDirectory -Force | Out-Null
$HarnessConfigPath = Join-Path $HarnessConfigDirectory "config.json"
$HarnessHotkeys = Get-AvailableHarnessHotkeys
$HarnessConfigJson = @{
    hotkeys = @{
        'Toggle Search' = $HarnessHotkeys[0].Text
        Screenshot = $HarnessHotkeys[1].Text
        Record = $HarnessHotkeys[2].Text
    }
} | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText(
    $HarnessConfigPath, $HarnessConfigJson, [System.Text.UTF8Encoding]::new($false))

function Get-ExecutablePath($Process) {
    try { return [string]$Process.Path } catch { return "" }
}

function Test-IsHarnessExecutable($Process) {
    $candidate = Get-ExecutablePath $Process
    if ([string]::IsNullOrWhiteSpace($candidate)) { return $false }
    $full = [System.IO.Path]::GetFullPath($candidate)
    return $full.Equals($exePath, [System.StringComparison]::OrdinalIgnoreCase) -or
        $full.Equals($TestServicePath, [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-HarnessProcesses {
    return @(Get-Process -Name "Tools3000", "Tools3000_Service" `
        -ErrorAction SilentlyContinue | Where-Object { Test-IsHarnessExecutable $_ })
}

function Get-HarnessSearchServices {
    return @(Get-Process -Name "Tools3000_Service" -ErrorAction SilentlyContinue |
        Where-Object { Test-IsHarnessExecutable $_ })
}

function Get-RegisteredHarnessHotkeyId([string]$Name) {
    $logPath = Join-Path $HarnessDataRoot "logs\tools3000.log"
    $pattern = 'name=' + [regex]::Escape($Name) + ',\s*def=.*\bid=(\d+)'
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        if (Test-Path -LiteralPath $logPath -PathType Leaf) {
            $matches = @(Select-String -LiteralPath $logPath -Pattern $pattern -AllMatches)
            if ($matches.Count -gt 0) {
                $match = $matches[-1].Matches[-1]
                return [int]$match.Groups[1].Value
            }
        }
        Start-Sleep -Milliseconds 100
    }
    throw "待测进程没有成功注册快捷键: $Name"
}

function Invoke-HarnessHotkey($Process, [string]$Name) {
    $messageWindow = [LifecycleHarness]::FindMessageWindowForProcess([uint32]$Process.Id)
    if ($messageWindow -eq [IntPtr]::Zero) {
        throw "找不到待测进程的主消息窗口，无法触发快捷键: $Name"
    }
    $hotkeyId = Get-RegisteredHarnessHotkeyId $Name
    # WM_HOTKEY is what RegisterHotKey asks Windows to deliver. Posting it to
    # the isolated process avoids UIPI dropping synthetic keyboard input when
    # an elevated app happens to own the foreground window.
    if (-not [LifecycleHarness]::PostMessageW(
            $messageWindow, 0x0312, [IntPtr]$hotkeyId, [IntPtr]::Zero)) {
        throw "无法向待测进程投递 WM_HOTKEY: $Name"
    }
}

function Test-StartupLogCleanliness([string]$LogPath) {
    $hasShellTray = $false
    try {
        $trayHwnd = [LifecycleHarness]::FindWindowW("Shell_TrayWnd", $null)
        $hasShellTray = ($trayHwnd -ne [IntPtr]::Zero)
    } catch {
        $hasShellTray = $true
    }

    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
        return
    }

    $logLines = Get-Content -LiteralPath $LogPath -Encoding UTF8 -ErrorAction SilentlyContinue
    if (-not $logLines) { return }

    if ($hasShellTray) {
        $trayErrors = @($logLines | Where-Object {
            $_ -match "创建/更新托盘图标未成功" -or
            $_ -match "0x80004005" -or
            $_ -match "启动自愈定时器"
        })
        if ($trayErrors.Count -gt 0) {
            Write-Host "[ERROR] 启动日志纯净度门禁 (Zero-Warning Log Gate) 拦截到托盘创建异常：" -ForegroundColor Red
            foreach ($err in $trayErrors) {
                Write-Host "  -> $err" -ForegroundColor Red
            }
            throw "待测实例在交互式物理桌面启动时托盘图标创建失败，严禁带有托盘异常的进程通过门禁！"
        }
        Write-Host "[OK] 启动日志纯净度门禁 PASS (托盘图标零警告/零自愈异常)" -ForegroundColor Green
    } else {
        Write-Host "ℹ️ 检测到当前处于无头或沙箱环境 (无 Shell_TrayWnd)，跳过交互任务栏图标断言" -ForegroundColor DarkGray
    }

    $criticalErrors = @($logLines | Where-Object {
        $_ -match "\[fatal\]" -or
        $_ -match "panic"
    })
    if ($criticalErrors.Count -gt 0) {
        Write-Host "[ERROR] 启动日志中检测到致命错误：" -ForegroundColor Red
        foreach ($err in $criticalErrors) {
            Write-Host "  -> $err" -ForegroundColor Red
        }
        throw "待测实例启动日志包含致命错误，门禁阻断！"
    }
}

# 动态感知隔离配置中的快捷键绑定；首次运行使用产品默认值。
$configPath = $HarnessConfigPath
$config = @{}
if (Test-Path $configPath) {
    try {
        $config = Get-Content $configPath -Raw | ConvertFrom-Json
        Write-Host "[OK] 成功加载隔离测试配置: $configPath" -ForegroundColor Green
    } catch {
        Write-Host "[WARN] 隔离测试配置解析失败，回退默认" -ForegroundColor Yellow
    }
}

$searchHotkeyStr    = if ($config.hotkeys -and $config.hotkeys.'Toggle Search') { $config.hotkeys.'Toggle Search' } else { "Alt+Space" }
$captureHotkeyStr   = if ($config.hotkeys -and $config.hotkeys.Screenshot)      { $config.hotkeys.Screenshot }      else { "Ctrl+Shift+A" }
$recordHotkeyStr    = if ($config.hotkeys -and $config.hotkeys.Record)          { $config.hotkeys.Record }          else { "Ctrl+Shift+R" }
Write-Host ("动态快捷键映射: [搜索: {0}] [截图: {1}] [录屏: {2}]" -f $searchHotkeyStr, $captureHotkeyStr, $recordHotkeyStr) -ForegroundColor DarkGray

try {
# 3. 确保干净起点 (无残留进程)
Get-HarnessProcesses | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

Write-Host "`n── [1/5] 启动待测应用并验证静默状态 ──" -ForegroundColor Yellow
$proc = Start-Process $exePath -ArgumentList "--no-elevate", "--force-portable-search-service", "--lifecycle-test-instance" `
    -Environment $HarnessEnvironment -PassThru
Start-Sleep -Seconds 2

if ($proc.HasExited) {
    Write-Host "[ERROR] Tools3000 启动即崩溃退出！" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Tools3000 启动成功 (PID: $($proc.Id))" -ForegroundColor Green

# 启动日志纯净度门禁：断言在交互桌面环境下启动严禁出现托盘创建失败警告或自愈定时器
$initialLogPath = Join-Path $HarnessDataRoot "logs\tools3000.log"
Test-StartupLogCleanliness -LogPath $initialLogPath

# WebView 预加载、设置页状态查询和普通应用启动都不能提前拉起重型索引进程。
$serviceBeforeSearch = Get-HarnessSearchServices
if ($serviceBeforeSearch) {
    Write-Host "[ERROR] 搜索服务在用户主动唤起搜索前已经启动，违反按需加载契约！" -ForegroundColor Red
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
Write-Host "[OK] 搜索服务保持休眠，应用/WebView 预加载未触发索引" -ForegroundColor Green

# 先单独验证“本次从未唤起搜索”的退出路径。若 shutdown 代码
# 误用了可自启的管道请求，这一阶段会在主进程退出后捕获到服务。
$coldExitWindow = [LifecycleHarness]::FindMessageWindowForProcess([uint32]$proc.Id)
if ($coldExitWindow -eq [IntPtr]::Zero) {
    Write-Host "[ERROR] 找不到首轮待测进程的主消息窗口！" -ForegroundColor Red
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
[void][LifecycleHarness]::PostMessageW($coldExitWindow, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
for ($i = 0; $i -lt 30; $i++) {
    if (-not (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Milliseconds 100
}
if (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue) {
    Write-Host "[ERROR] 未唤起搜索的首轮 Tools3000 未能正常退出！" -ForegroundColor Red
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
Start-Sleep -Milliseconds 500
if (Get-HarnessSearchServices) {
    Write-Host "[ERROR] Tools3000 在从未唤起搜索的退出路径中启动了索引服务！" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] 从未唤起搜索时，Tools3000 退出也不会启动索引服务" -ForegroundColor Green

# 启动第二轮进程，继续验证显式唤起与唤起后常驻。
$proc = Start-Process $exePath -ArgumentList "--no-elevate", "--force-portable-search-service", "--lifecycle-test-instance" `
    -Environment $HarnessEnvironment -PassThru
Start-Sleep -Seconds 2
if ($proc.HasExited) {
    Write-Host "[ERROR] Tools3000 第二轮启动即崩溃退出！" -ForegroundColor Red
    exit 1
}

# 2. 截图与录屏选区生命周期与取消路径验证 (Cancel-Path & Re-entry Testing)
Write-Host "`n── [2/5] 截图与录屏选区取消、再次唤起与防死锁测试 ──" -ForegroundColor Yellow
Write-Host "  -> 触发动态截图快捷键: $captureHotkeyStr" -ForegroundColor DarkGray
Invoke-HarnessHotkey $proc "Screenshot"
$captureWindow = [IntPtr]::Zero
for ($i = 0; $i -lt 20; $i++) {
    $captureWindow = [LifecycleHarness]::FindByClassForProcess(
        "Tools3000_CaptureOverlay", [uint32]$proc.Id)
    if ($captureWindow -ne [IntPtr]::Zero -and [LifecycleHarness]::IsWindowVisible($captureWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ($captureWindow -eq [IntPtr]::Zero -or -not [LifecycleHarness]::IsWindowVisible($captureWindow)) {
    throw '截图快捷键没有显示待测进程的截图覆盖层。'
}

# 验证 HUD 快捷键操作提示微晶卡片生命周期联动
$hintWindow = [LifecycleHarness]::FindByClassForProcess(
    "Tools3000_ShortcutHintOverlay", [uint32]$proc.Id)
if ($hintWindow -ne [IntPtr]::Zero -and [LifecycleHarness]::IsWindowVisible($hintWindow)) {
    Write-Host "  -> 联动验证：HUD 快捷键操作提示微晶卡片同步唤起 (HWND: 0x$($hintWindow.ToString('X8')))" -ForegroundColor DarkGray
}

# 模拟中途按 Esc 取消
Write-Host "  -> 模拟中途按 Esc 退出截图" -ForegroundColor DarkGray
[void][LifecycleHarness]::PostMessageW($captureWindow, 0x0100, [IntPtr]0x1B, [IntPtr]::Zero)
[void][LifecycleHarness]::PostMessageW($captureWindow, 0x0101, [IntPtr]0x1B, [IntPtr]::Zero)
for ($i = 0; $i -lt 20; $i++) {
    if (-not [LifecycleHarness]::IsWindowVisible($captureWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ([LifecycleHarness]::IsWindowVisible($captureWindow)) {
    throw 'Esc 后截图覆盖层仍可见，取消路径没有完成。'
}
for ($i = 0; $i -lt 20; $i++) {
    $hintWindow = [LifecycleHarness]::FindByClassForProcess(
        "Tools3000_ShortcutHintOverlay", [uint32]$proc.Id)
    if ($hintWindow -eq [IntPtr]::Zero -or -not [LifecycleHarness]::IsWindowVisible($hintWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ($hintWindow -ne [IntPtr]::Zero -and [LifecycleHarness]::IsWindowVisible($hintWindow)) {
    throw 'Esc 后 HUD 快捷键操作提示微晶卡片仍可见，存在悬挂残留。'
}

# 关键自愈验证：再次按下截图快捷键，断言必须重新拉起。
Write-Host "  -> 再次触发截图 (验证原子自愈与零死锁)" -ForegroundColor DarkGray
Invoke-HarnessHotkey $proc "Screenshot"
$secondCaptureWindow = [IntPtr]::Zero
for ($i = 0; $i -lt 20; $i++) {
    $secondCaptureWindow = [LifecycleHarness]::FindByClassForProcess(
        "Tools3000_CaptureOverlay", [uint32]$proc.Id)
    if ($secondCaptureWindow -ne [IntPtr]::Zero -and [LifecycleHarness]::IsWindowVisible($secondCaptureWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ($secondCaptureWindow -eq [IntPtr]::Zero -or
    -not [LifecycleHarness]::IsWindowVisible($secondCaptureWindow)) {
    throw '取消后再次触发截图未能重新显示覆盖层。'
}
[void][LifecycleHarness]::PostMessageW($secondCaptureWindow, 0x0100, [IntPtr]0x1B, [IntPtr]::Zero)
[void][LifecycleHarness]::PostMessageW($secondCaptureWindow, 0x0101, [IntPtr]0x1B, [IntPtr]::Zero)
for ($i = 0; $i -lt 20; $i++) {
    if (-not [LifecycleHarness]::IsWindowVisible($secondCaptureWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ([LifecycleHarness]::IsWindowVisible($secondCaptureWindow)) {
    throw '第二轮截图覆盖层未能取消。'
}
Write-Host "[OK] 截图生命周期与取消路径自愈测试 PASS" -ForegroundColor Green

# 2.2 录屏选区生命周期与取消路径验证 (RecordRegion Cancel-Path & Re-entry Testing)
Write-Host "  -> 触发动态录屏选区快捷键: $recordHotkeyStr" -ForegroundColor DarkGray
Invoke-HarnessHotkey $proc "Record"
$recordWindow = [IntPtr]::Zero
for ($i = 0; $i -lt 20; $i++) {
    $recordWindow = [LifecycleHarness]::FindByClassForProcess(
        "Tools3000_CaptureOverlay", [uint32]$proc.Id)
    if ($recordWindow -ne [IntPtr]::Zero -and [LifecycleHarness]::IsWindowVisible($recordWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ($recordWindow -eq [IntPtr]::Zero -or -not [LifecycleHarness]::IsWindowVisible($recordWindow)) {
    throw '录屏快捷键没有显示待测进程的选区覆盖层。'
}

# 验证 HUD 快捷键操作提示微晶卡片在录屏选区阶段联动
$hintWindow = [LifecycleHarness]::FindByClassForProcess(
    "Tools3000_ShortcutHintOverlay", [uint32]$proc.Id)
if ($hintWindow -ne [IntPtr]::Zero -and [LifecycleHarness]::IsWindowVisible($hintWindow)) {
    Write-Host "  -> 联动验证：录屏选区 HUD 快捷键卡片同步唤起 (HWND: 0x$($hintWindow.ToString('X8')))" -ForegroundColor DarkGray
}

# 模拟中途按 Esc 取消录屏选区
Write-Host "  -> 模拟中途按 Esc 退出录屏选区" -ForegroundColor DarkGray
[void][LifecycleHarness]::PostMessageW($recordWindow, 0x0100, [IntPtr]0x1B, [IntPtr]::Zero)
[void][LifecycleHarness]::PostMessageW($recordWindow, 0x0101, [IntPtr]0x1B, [IntPtr]::Zero)
for ($i = 0; $i -lt 20; $i++) {
    if (-not [LifecycleHarness]::IsWindowVisible($recordWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ([LifecycleHarness]::IsWindowVisible($recordWindow)) {
    throw 'Esc 后录屏选区覆盖层仍可见，取消路径没有完成。'
}
for ($i = 0; $i -lt 20; $i++) {
    $hintWindow = [LifecycleHarness]::FindByClassForProcess(
        "Tools3000_ShortcutHintOverlay", [uint32]$proc.Id)
    if ($hintWindow -eq [IntPtr]::Zero -or -not [LifecycleHarness]::IsWindowVisible($hintWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ($hintWindow -ne [IntPtr]::Zero -and [LifecycleHarness]::IsWindowVisible($hintWindow)) {
    throw 'Esc 后录屏 HUD 快捷键操作提示卡片仍可见，存在悬挂残留。'
}

# 再次按下录屏快捷键，验证原子自愈与零死锁
Write-Host "  -> 再次触发录屏选区 (验证原子自愈与零死锁)" -ForegroundColor DarkGray
Invoke-HarnessHotkey $proc "Record"
$secondRecordWindow = [IntPtr]::Zero
for ($i = 0; $i -lt 20; $i++) {
    $secondRecordWindow = [LifecycleHarness]::FindByClassForProcess(
        "Tools3000_CaptureOverlay", [uint32]$proc.Id)
    if ($secondRecordWindow -ne [IntPtr]::Zero -and [LifecycleHarness]::IsWindowVisible($secondRecordWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ($secondRecordWindow -eq [IntPtr]::Zero -or
    -not [LifecycleHarness]::IsWindowVisible($secondRecordWindow)) {
    throw '取消后再次触发录屏未能重新显示覆盖层。'
}
[void][LifecycleHarness]::PostMessageW($secondRecordWindow, 0x0100, [IntPtr]0x1B, [IntPtr]::Zero)
[void][LifecycleHarness]::PostMessageW($secondRecordWindow, 0x0101, [IntPtr]0x1B, [IntPtr]::Zero)
for ($i = 0; $i -lt 20; $i++) {
    if (-not [LifecycleHarness]::IsWindowVisible($secondRecordWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ([LifecycleHarness]::IsWindowVisible($secondRecordWindow)) {
    throw '第二轮录屏选区覆盖层未能取消。'
}
Write-Host "[OK] 录屏选区生命周期与取消路径自愈测试 PASS" -ForegroundColor Green

# 2.3 剪贴板全矩阵注入与内存修剪零破坏审计 (Clipboard Multi-Format & Zero-Damage Lifecycle Gate)
Write-Host "  -> 执行剪贴板全矩阵注入 (PNG/CF_DIBV5/CF_DIB/CF_BITMAP/CF_HDROP) 与内存修剪零破坏门禁" -ForegroundColor DarkGray
$testExeCandidates = @(
    (Join-Path $TestBinDirectory "Tools3000Tests.exe"),
    (Join-Path $ProjectRoot "build\bin\Release\Tools3000Tests.exe"),
    (Join-Path $ProjectRoot "build\bin\Tools3000Tests.exe")
)
$testExe = $testExeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($testExe) {
    & $testExe --gtest_filter=ClipboardUtilsTest.*
    if ($LASTEXITCODE -ne 0) {
        throw "剪贴板多格式注入与物理内存修剪零破坏审计门禁失败！退出码: $LASTEXITCODE"
    }
    Write-Host "[OK] 剪贴板多矩阵注入与内存修剪零破坏门禁 PASS" -ForegroundColor Green
} else {
    Write-Host "[WARN] 未检测到 Tools3000Tests.exe，跳过单测级剪贴板深度矩阵断言" -ForegroundColor Yellow
}

# 3. 搜索窗口与索引服务按需唤醒生命周期
Write-Host "`n── [3/5] 搜索中心与按需服务唤醒生命周期测试 ──" -ForegroundColor Yellow
# 在 WebView 预载有充足时间完成后持续观察，避免“2 秒时没启动、
# 稍后 NavigationCompleted 又误启动”被当成按需成功。
for ($i = 0; $i -lt 8; $i++) {
    if (Get-HarnessSearchServices) {
        Write-Host "[ERROR] 搜索服务在用户唤起前的稳定观察窗口内自行启动！" -ForegroundColor Red
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        exit 1
    }
    Start-Sleep -Milliseconds 250
}
Write-Host "  -> 触发动态搜索快捷键: $searchHotkeyStr" -ForegroundColor DarkGray
Invoke-HarnessHotkey $proc "Toggle Search"
$searchService = $null
for ($i = 0; $i -lt 20; $i++) {
    $searchService = @(Get-HarnessSearchServices) |
        Select-Object -First 1
    if ($searchService) { break }
    Start-Sleep -Milliseconds 250
}
if (-not $searchService) {
    Write-Host "[ERROR] 用户主动唤起搜索后，索引服务未在 5 秒内按需启动！" -ForegroundColor Red
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
$searchWindow = [IntPtr]::Zero
for ($i = 0; $i -lt 20; $i++) {
    $searchWindow = [LifecycleHarness]::FindByClassForProcess(
        "Tools3000_SearchWindow", [uint32]$proc.Id)
    if ($searchWindow -ne [IntPtr]::Zero -and [LifecycleHarness]::IsWindowVisible($searchWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ($searchWindow -eq [IntPtr]::Zero -or -not [LifecycleHarness]::IsWindowVisible($searchWindow)) {
    Write-Host "[ERROR] 快捷键未显示搜索窗口，无法将服务启动归因于显式用户操作！" -ForegroundColor Red
    Stop-Process -Id $searchService.Id -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
$searchServicePid = $searchService.Id
Write-Host "[OK] 搜索窗口唤起后索引服务已按需启动 (PID: $searchServicePid)" -ForegroundColor Green

# 按 Esc 隐藏搜索窗口
[void][LifecycleHarness]::PostMessageW($searchWindow, 0x0100, [IntPtr]0x1B, [IntPtr]::Zero)
[void][LifecycleHarness]::PostMessageW($searchWindow, 0x0101, [IntPtr]0x1B, [IntPtr]::Zero)
for ($i = 0; $i -lt 20; $i++) {
    if (-not [LifecycleHarness]::IsWindowVisible($searchWindow)) { break }
    Start-Sleep -Milliseconds 100
}
if ([LifecycleHarness]::IsWindowVisible($searchWindow)) {
    Write-Host "[ERROR] Esc 未隐藏本轮 Tools3000 的搜索窗口！" -ForegroundColor Red
    Stop-Process -Id $searchServicePid -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
if (-not (Get-Process -Id $searchServicePid -ErrorAction SilentlyContinue)) {
    Write-Host "[ERROR] 搜索窗口隐藏后索引服务被错误停止！" -ForegroundColor Red
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
Write-Host "[OK] 搜索窗口隐藏后同一索引服务仍常驻" -ForegroundColor Green
Write-Host "[OK] 搜索窗口显式呼出与索引服务按需常驻生命周期测试 PASS" -ForegroundColor Green

# 4. 记录待测进程的顶层窗口，供失败时快速定位隐藏窗口残留。
Write-Host "`n── [4/5] 顶层窗口可见性诊断清单 ──" -ForegroundColor Yellow
$dumpList = [LifecycleHarness]::DumpWindowsForProcess([uint32]$proc.Id)
foreach ($item in $dumpList) {
    Write-Host "  -> $item" -ForegroundColor DarkGray
}
Write-Host "[OK] 顶层窗口诊断清单记录完成" -ForegroundColor Green

# 5. 优雅退出与主进程收尾
Write-Host "`n── [5/5] 托盘消息交互、真实退出命令与主进程收尾 ──" -ForegroundColor Yellow
$msgHwnd = [LifecycleHarness]::FindMessageWindowForProcess([uint32]$proc.Id)
if ($msgHwnd -ne [IntPtr]::Zero) {
    # 5.1 验证向主窗口投递 WM_TRAYICON (WM_USER + 100 = 0x0464) 消息回路 (模拟托盘右键单击 WM_RBUTTONUP 0x0205)
    Write-Host "  -> 向主消息窗口 (0x$($msgHwnd.ToString('X8'))) 投递 WM_TRAYICON (WM_RBUTTONUP) 验证托盘处理回路" -ForegroundColor DarkGray
    [void][LifecycleHarness]::PostMessageW($msgHwnd, 0x0464, [IntPtr]1, [IntPtr]0x0205)
    Start-Sleep -Milliseconds 200

    # 5.2 发送真实托盘菜单退出命令 (WM_COMMAND 0x0111, ID=1099 为 TrayMenuId::Exit)
    Write-Host "  -> 向主消息窗口投递真实托盘退出命令 (WM_COMMAND, ID=1099 [TrayMenuId::Exit])" -ForegroundColor DarkGray
    [void][LifecycleHarness]::PostMessageW($msgHwnd, 0x0111, [IntPtr]1099, [IntPtr]::Zero)
} else {
    Write-Host "[ERROR] 找不到主消息窗口，不能用强杀代替优雅退出生命周期验证！" -ForegroundColor Red
    Stop-Process -Id $searchServicePid -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

$elapsed = 0.0
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 300
    $elapsed += 0.3
    if (-not (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)) { break }
}

if (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue) {
    Write-Host "[ERROR] 退出收割失败：目标主进程未能在规定时间内全部归零！" -ForegroundColor Red
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
} else {
    Write-Host "[OK] 退出收尾测试 PASS ($elapsed 秒内主进程干净退出)" -ForegroundColor Green
    $finalLogPath = Join-Path $HarnessDataRoot "logs\tools3000.log"
    Test-StartupLogCleanliness -LogPath $finalLogPath
}

if (-not (Get-Process -Id $searchServicePid -ErrorAction SilentlyContinue)) {
    Write-Host "[ERROR] Tools3000 主进程退出后索引服务未保持常驻！" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Tools3000 退出后同一索引服务仍常驻" -ForegroundColor Green

# 测试显式使用便携服务，收尾时只回收本轮记录的 PID，避免污染后续门禁。
Stop-Process -Id $searchServicePid -Force -ErrorAction SilentlyContinue

Write-Host "`n===============================================================================" -ForegroundColor Cyan
Write-Host " Tools3000 关键生命周期与防死锁自动化门禁全部通过！" -ForegroundColor Green
Write-Host "===============================================================================" -ForegroundColor Cyan
} finally {
    # 只清理由本轮待测目录启动的进程；已安装版本及其 SCM 服务不受影响。
    Get-HarnessProcesses | Stop-Process -Force -ErrorAction SilentlyContinue
}
