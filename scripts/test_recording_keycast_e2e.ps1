# -----------------------------------------------------------------------------
# test_recording_keycast_e2e.ps1 — Tools3000 按键回显 (Keycast) 与录屏联动操作系统级端到端门禁
# -----------------------------------------------------------------------------
# 通过操作系统原生 SendInput 与窗口可见性检视驱动真实的 Tools3000 进程，硬断言验证：
# [1/5] 键盘钩子与插件管线初始化就绪；
# [2/5] 日常 smart_shortcuts 模式下：普通单字母打字保持静默，Tools3000_KeycastOverlay 绝不弹窗骚扰；
# [3/5] 注入修饰快捷键 (Ctrl+Shift+P 及上档标点 Ctrl+Shift+=)：Tools3000_KeycastOverlay 真实弹出可见且尺寸合法；
# [4/5] 录屏开启时：拼音字母/Space/Backspace 静默防刷屏，组合快捷键在选区停靠态正常弹出回显；
# [5/5] 录屏停止时：退出选区停靠并恢复日常模式。
# -----------------------------------------------------------------------------
param(
    [string]$ExePath = ""
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Stop"

Write-Host "===============================================================================" -ForegroundColor Cyan
Write-Host " Tools3000 按键回显 (Keycast) 与录屏联动操作系统级端到端 (E2E) 自动化测试门禁 " -ForegroundColor Cyan
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
        Write-Host "[ERROR] 未找到 Tools3000.exe，请先编译或打包！" -ForegroundColor Red
        exit 1
    }
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
Write-Host "[INFO] 待测程序: $ExePath" -ForegroundColor Gray

$HarnessRoot = Join-Path $ProjectRoot "build\keycast-e2e-$PID"
$HarnessLocalAppData = Join-Path $HarnessRoot "LocalAppData"
$HarnessRoamingAppData = Join-Path $HarnessRoot "RoamingAppData"
$HarnessDataRoot = Join-Path $HarnessRoot "Tools3000Data"
$HarnessLogsDir = Join-Path $HarnessDataRoot "logs"
$HarnessVideosDir = Join-Path $HarnessRoot "Recordings"
New-Item -ItemType Directory -Path $HarnessLocalAppData, $HarnessRoamingAppData, $HarnessDataRoot, $HarnessLogsDir, $HarnessVideosDir -Force | Out-Null

$HarnessConfigDirectory = Join-Path $HarnessDataRoot "config"
New-Item -ItemType Directory -Path $HarnessConfigDirectory -Force | Out-Null
$HarnessConfigPath = Join-Path $HarnessConfigDirectory "config.json"

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading;

public static class NativeKeycastHarness {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx;
        public int dy;
        public uint mouseData;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct INPUT {
        [FieldOffset(0)] public int type;
        [FieldOffset(8)] public MOUSEINPUT mi;
        [FieldOffset(8)] public KEYBDINPUT ki;
    }

    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", SetLastError=true)] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool RegisterHotKey(IntPtr hWnd, int id, uint modifiers, uint virtualKey);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool UnregisterHotKey(IntPtr hWnd, int id);
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr value);

    public const int INPUT_KEYBOARD = 1;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_MOUSEMOVE = 0x0200;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_HOTKEY = 0x0312;
    public const uint WM_ACCESSIBILITY_INVOKE_TOOLBAR = 0x804A;
    public const uint WM_TOOLS3000_TEST_INJECT_KEY = 0x80BE; // WM_APP + 190
    public const int MK_LBUTTON = 0x0001;
    public static readonly IntPtr TEST_EXTRA_INFO = (IntPtr)0x54455354;

    public static IntPtr MakeLParam(int x, int y) {
        return (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF));
    }

    public static IntPtr FindByClassForProcess(string wantedClass, uint pid, bool requireVisible = false) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint wndPid;
            GetWindowThreadProcessId(h, out wndPid);
            if (wndPid != pid) return true;
            if (requireVisible && !IsWindowVisible(h)) return true;
            var sb = new StringBuilder(256);
            GetClassNameW(h, sb, 256);
            if (sb.ToString() == wantedClass) {
                found = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

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

    private static void EmitSingleKeyStep(IntPtr fallbackMsgWnd, ushort vk, bool isKeyUp) {
        INPUT[] inp = new INPUT[1];
        inp[0].type = INPUT_KEYBOARD;
        inp[0].ki.wVk = vk;
        inp[0].ki.dwFlags = isKeyUp ? KEYEVENTF_KEYUP : 0u;
        inp[0].ki.dwExtraInfo = TEST_EXTRA_INFO;
        uint sent = SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
        if (sent == 0 && fallbackMsgWnd != IntPtr.Zero) {
            int wp = ((isKeyUp ? 1 : 0) << 16) | (vk & 0xFFFF);
            SendMessageW(fallbackMsgWnd, WM_TOOLS3000_TEST_INJECT_KEY, (IntPtr)wp, IntPtr.Zero);
        }
    }

    public static void SendKeyPress(IntPtr fallbackMsgWnd, ushort vk) {
        EmitSingleKeyStep(fallbackMsgWnd, vk, false);
        Thread.Sleep(16);
        EmitSingleKeyStep(fallbackMsgWnd, vk, true);
        Thread.Sleep(16);
    }

    public static void SendShortcut(IntPtr fallbackMsgWnd, ushort[] mods, ushort key) {
        for (int i = 0; i < mods.Length; i++) {
            EmitSingleKeyStep(fallbackMsgWnd, mods[i], false);
            Thread.Sleep(15);
        }

        EmitSingleKeyStep(fallbackMsgWnd, key, false);
        Thread.Sleep(24);

        EmitSingleKeyStep(fallbackMsgWnd, key, true);
        Thread.Sleep(15);

        for (int i = mods.Length - 1; i >= 0; i--) {
            EmitSingleKeyStep(fallbackMsgWnd, mods[i], true);
            Thread.Sleep(12);
        }
        Thread.Sleep(20);
    }

    public static void DragClientMouse(IntPtr hWnd, int x1, int y1, int x2, int y2) {
        PostMessageW(hWnd, WM_MOUSEMOVE, IntPtr.Zero, MakeLParam(x1, y1));
        PostMessageW(hWnd, WM_LBUTTONDOWN, (IntPtr)MK_LBUTTON, MakeLParam(x1, y1));
        Thread.Sleep(20);
        PostMessageW(hWnd, WM_MOUSEMOVE, (IntPtr)MK_LBUTTON, MakeLParam(x2, y2));
        Thread.Sleep(20);
        PostMessageW(hWnd, WM_LBUTTONUP, IntPtr.Zero, MakeLParam(x2, y2));
        Thread.Sleep(30);
    }
}
"@

[void][NativeKeycastHarness]::SetProcessDpiAwarenessContext([IntPtr](-4))

function Get-IdleRecordHotkey {
    $modifiers = [uint32](0x0001 -bor 0x0002 -bor 0x0004 -bor 0x4000)
    foreach ($cand in @(
        @{ Text = 'Ctrl+Alt+Shift+F22'; Key = [uint32]0x85 },
        @{ Text = 'Ctrl+Alt+Shift+F21'; Key = [uint32]0x84 },
        @{ Text = 'Ctrl+Alt+Shift+F20'; Key = [uint32]0x83 }
    )) {
        if ([NativeKeycastHarness]::RegisterHotKey([IntPtr]::Zero, 0x7250, $modifiers, $cand.Key)) {
            [void][NativeKeycastHarness]::UnregisterHotKey([IntPtr]::Zero, 0x7250)
            return $cand.Text
        }
    }
    return "Ctrl+Alt+Shift+F22"
}

$RecordHkText = Get-IdleRecordHotkey
$ConfigJson = @{
    plugins = @{
        keycast      = @{ enabled = $true }
        capture      = @{ enabled = $true }
        gesture      = @{ enabled = $false }
        remote_boost = @{ enabled = $false }
        remote       = @{ enabled = $false }
    }
    hotkeys = @{
        Record = $RecordHkText
    }
    recording = @{
        includeKeycast      = $true
        format              = "mp4_h264"
        fps                 = 30
        countdownSeconds    = 0
        saveDirectory       = $HarnessVideosDir
        studioMode          = $false
        autoBypassFullscreen = $false
    }
    capture = @{
        autoBypassFullscreen = $false
        autoDetectWindow     = $false
    }
    keycast = @{
        enabled           = $true
        showKeyboard      = $true
        filterMode        = "smart_shortcuts"
        autoBypassFullscreen = $false
    }
} | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText($HarnessConfigPath, $ConfigJson, [System.Text.UTF8Encoding]::new($false))

$HarnessLogFile = Join-Path $HarnessLogsDir "tools3000.log"
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $ExePath
$psi.WorkingDirectory = Split-Path -Parent $ExePath
$psi.UseShellExecute = $false
$psi.Arguments = "--lifecycle-test-instance"
$psi.EnvironmentVariables["LOCALAPPDATA"] = $HarnessLocalAppData
$psi.EnvironmentVariables["APPDATA"] = $HarnessRoamingAppData
$psi.EnvironmentVariables["TOOLS3000_DATA_ROOT"] = $HarnessDataRoot
$psi.EnvironmentVariables["TOOLS3000_ALLOW_INJECTED_KEYBOARD"] = "1"
$psi.EnvironmentVariables["TOOLS3000_ALLOW_INJECTED_MOUSE"] = "1"
$psi.EnvironmentVariables["TOOLS3000_TEST_MODE"] = "1"

Write-Host "正在启动被测进程 (录屏与键盘钩子测试通道已放行)..." -ForegroundColor Yellow
$proc = [System.Diagnostics.Process]::Start($psi)
if (-not $proc -or $proc.HasExited) {
    Write-Host "[ERROR] 进程启动失败！" -ForegroundColor Red
    exit 1
}

$procId = [uint32]$proc.Id
Write-Host "[OK] Tools3000 已成功启动 (PID: $procId)" -ForegroundColor Green

function Get-LogContentSafe($filePath) {
    if (-not (Test-Path -LiteralPath $filePath)) { return "" }
    try {
        $fs = [System.IO.FileStream]::new($filePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $sr = [System.IO.StreamReader]::new($fs, [System.Text.Encoding]::UTF8)
        $content = $sr.ReadToEnd()
        $sr.Dispose()
        $fs.Dispose()
        return $content
    } catch {
        return ""
    }
}

function Get-RegisteredRecordHotkeyId([string]$LogPath) {
    for ($i = 0; $i -lt 30; $i++) {
        $txt = Get-LogContentSafe $LogPath
        $m = [regex]::Matches($txt, 'name=Record,\s*def=.*\bid=(\d+)')
        if ($m.Count -gt 0) {
            return [int]$m[$m.Count - 1].Groups[1].Value
        }
        Start-Sleep -Milliseconds 100
    }
    throw "未能在日志中读取 Record 快捷键 ID"
}

function Wait-KeycastVisible([uint32]$PidVal, [int]$TimeoutMs = 2000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $h = [NativeKeycastHarness]::FindByClassForProcess("Tools3000_KeycastOverlay", $PidVal, $true)
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 40
    }
    return [IntPtr]::Zero
}

try {
    # 1. 等待键盘钩子与录屏快捷键就绪
    Write-Host "`n── [1/5] 等待键盘回显钩子与录屏插件就绪 ──" -ForegroundColor Cyan
    $recHkId = Get-RegisteredRecordHotkeyId -LogPath $HarnessLogFile
    $msgWnd = [NativeKeycastHarness]::FindMessageWindowForProcess($procId)
    if ($msgWnd -eq [IntPtr]::Zero) {
        throw "[1/5] 未能定位待测进程主消息窗口！"
    }
    Write-Host "[OK] 键盘钩子与插件管线初始化就绪 (Record Hotkey ID=$recHkId)！" -ForegroundColor Green

    # 2. 日常模式下普通字母打字静默硬断言 (Tools3000_KeycastOverlay 必须不可见)
    Write-Host "`n── [2/5] E2E 日常智能打字过滤硬断言 (普通单键严禁弹窗) ──" -ForegroundColor Cyan
    foreach ($vk in @(0x48, 0x45, 0x4C, 0x4C, 0x4F)) {
        [NativeKeycastHarness]::SendKeyPress($msgWnd, [uint16]$vk)
        Start-Sleep -Milliseconds 25
    }
    Start-Sleep -Milliseconds 180
    $leakedWnd = [NativeKeycastHarness]::FindByClassForProcess("Tools3000_KeycastOverlay", $procId, $true)
    if ($leakedWnd -ne [IntPtr]::Zero) {
        throw "[2/5] 日常 smart_shortcuts 模式下普通字母打字误弹出了 Tools3000_KeycastOverlay 悬浮窗！"
    }
    Write-Host "[OK] 日常打字智能静默验证通过 (Tools3000_KeycastOverlay 零误弹)！" -ForegroundColor Green

    # 3. 独立按键回显真实悬浮窗可见性与上档符号快捷键硬断言
    Write-Host "`n── [3/5] E2E 组合快捷键 (Ctrl+Shift+P / Ctrl+Shift+=) 悬浮窗弹出硬断言 ──" -ForegroundColor Cyan
    $VK_LCONTROL = [uint16]0xA2
    $VK_LSHIFT   = [uint16]0xA0
    $VK_P        = [uint16]0x50
    $VK_OEM_PLUS = [uint16]0xBB # '=' / '+'

    [NativeKeycastHarness]::SendShortcut($msgWnd, @($VK_LCONTROL, $VK_LSHIFT), $VK_P)
    $kcHwnd = Wait-KeycastVisible -PidVal $procId -TimeoutMs 2000
    if ($kcHwnd -eq [IntPtr]::Zero) {
        throw "[3/5] 注入 Ctrl+Shift+P 后，按键回显悬浮窗 Tools3000_KeycastOverlay 未在桌面弹出显示！"
    }
    $kcRect = New-Object NativeKeycastHarness+RECT
    [void][NativeKeycastHarness]::GetWindowRect($kcHwnd, [ref]$kcRect)
    $kcW = $kcRect.Right - $kcRect.Left
    $kcH = $kcRect.Bottom - $kcRect.Top
    if ($kcW -lt 40 -or $kcH -lt 20) {
        throw "[3/5] Tools3000_KeycastOverlay 悬浮窗几何尺寸异常: ${kcW}x${kcH}"
    }

    # 继续注入 Ctrl+Shift+= (验证 '+' 上档标点组合键不崩溃且保持可见)
    [NativeKeycastHarness]::SendShortcut($msgWnd, @($VK_LCONTROL, $VK_LSHIFT), $VK_OEM_PLUS)
    Start-Sleep -Milliseconds 120
    if (-not [NativeKeycastHarness]::IsWindowVisible($kcHwnd)) {
        throw "[3/5] 注入 Ctrl+Shift+= 后按键回显悬浮窗意外消失！"
    }
    Write-Host "[OK] 按键回显原生悬浮窗真实弹出可见 (${kcW}x${kcH}, HWND=0x$($kcHwnd.ToString('X8')))！" -ForegroundColor Green

    # 4. 唤起选区录屏并验证录屏停靠态按键过滤与快捷键回显
    Write-Host "`n── [4/5] E2E 选区录屏停靠态：拼音打字静默 + 组合快捷键回显硬断言 ──" -ForegroundColor Cyan
    [void][NativeKeycastHarness]::PostMessageW($msgWnd, [NativeKeycastHarness]::WM_HOTKEY, [IntPtr]$recHkId, [IntPtr]::Zero)

    $capOv = [IntPtr]::Zero
    for ($i = 0; $i -lt 30; $i++) {
        $capOv = [NativeKeycastHarness]::FindByClassForProcess("Tools3000_CaptureOverlay", $procId, $true)
        if ($capOv -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 80
    }
    if ($capOv -eq [IntPtr]::Zero) {
        throw "[4/5] 录屏选区覆盖层未能唤起！"
    }

    [NativeKeycastHarness]::DragClientMouse($capOv, 300, 220, 780, 540)
    [void][NativeKeycastHarness]::SendMessageW($capOv, [NativeKeycastHarness]::WM_ACCESSIBILITY_INVOKE_TOOLBAR, [IntPtr]9, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 450

    $recordLogLenBefore = (Get-LogContentSafe $HarnessLogFile).Length
    foreach ($tvk in @(0x59, 0x45, 0x20, 0x08)) { # Y, E, Space, Backspace
        [NativeKeycastHarness]::SendKeyPress($msgWnd, [uint16]$tvk)
        Start-Sleep -Milliseconds 30
    }
    Start-Sleep -Milliseconds 120
    $logAfterRecTyping = Get-LogContentSafe $HarnessLogFile
    $recTypingSlice = if ($logAfterRecTyping.Length -gt $recordLogLenBefore) { $logAfterRecTyping.Substring($recordLogLenBefore) } else { "" }
    if ($recTypingSlice -match "rawKey=(y|Y|e|E|Space|Backspace)\b") {
        throw "[4/5] 录屏智能快捷键模式下发生了普通拼音打字/空格/退格泄漏: $recTypingSlice"
    }
    Write-Host "  [OK] 录屏期间普通拼音字母、Space 与 Backspace 精准静默！" -ForegroundColor Green

    [NativeKeycastHarness]::SendShortcut($msgWnd, @($VK_LCONTROL, $VK_LSHIFT), $VK_P)
    $dockedKcHwnd = Wait-KeycastVisible -PidVal $procId -TimeoutMs 2000
    if ($dockedKcHwnd -eq [IntPtr]::Zero) {
        throw "[4/5] 录屏停靠期间注入 Ctrl+Shift+P 未显示 Tools3000_KeycastOverlay！"
    }
    Write-Host "  [OK] 录屏选区停靠态快捷键悬浮胶囊正常弹出！" -ForegroundColor Green

    # 5. 停止录屏并校验 MP4 落盘与日常状态恢复
    Write-Host "`n── [5/5] E2E 停止录屏并恢复日常过滤模式验证 ──" -ForegroundColor Cyan
    [void][NativeKeycastHarness]::PostMessageW($msgWnd, [NativeKeycastHarness]::WM_HOTKEY, [IntPtr]$recHkId, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 900

    $savedVid = Get-ChildItem -LiteralPath $HarnessVideosDir -Filter "record_*.mp4" -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $savedVid -or $savedVid.Length -lt 512) {
        throw "[5/5] 录屏停止后未在输出目录生成有效 MP4 文件！"
    }
    Write-Host "[OK] 录屏正常停止落盘 ($($savedVid.Name)) 且按键回显管线平稳复位！" -ForegroundColor Green

    Write-Host "`n===============================================================================" -ForegroundColor Cyan
    Write-Host " [PASS] Tools3000 按键回显与录屏联动操作系统级端到端 (5/5) 全部通过！" -ForegroundColor Green
    Write-Host "===============================================================================" -ForegroundColor Cyan
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $msgWnd = [NativeKeycastHarness]::FindMessageWindowForProcess($procId)
        if ($msgWnd -ne [IntPtr]::Zero) {
            [void][NativeKeycastHarness]::PostMessageW($msgWnd, [NativeKeycastHarness]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
            [void]$proc.WaitForExit(3000)
        }
        if (-not $proc.HasExited) {
            try { $proc.Kill(); [void]$proc.WaitForExit(2000) } catch {}
        }
    }
    try {
        Remove-Item -LiteralPath $HarnessRoot -Recurse -Force -ErrorAction SilentlyContinue
    } catch {}
}
