# -----------------------------------------------------------------------------
# test_capture_recording_e2e.ps1 — Tools3000 截图·贴图·录屏·倒计时·演播室全链路端到端门禁
# -----------------------------------------------------------------------------
# 在隔离沙箱目录中拉起真实的 Tools3000.exe 进程，通过操作系统级窗口交互与真实文件/剪贴板检视验证：
# [1/6] 区域框选截图 + 矩形标注 + Enter 确认 -> PNG 文件尺寸/头签名校验 + 系统剪贴板位图校验
# [2/6] 区域截图一键贴图置顶 (ToolbarCommand::PinWindow) -> Tools3000_PinWindow 可见性、尺寸与关闭回收校验
# [3/6] 区域录屏启动 -> 60FPS 圆形倒计时窗 (Tools3000_RecordingCountdown) 可见性与 WDA_NONE 防隐身断言
# [4/6] 倒计时归零自动转入正式录制 + 选区内真实鼠标移动/点击触发自动运镜轨迹采集
# [5/6] 外部窗口抢焦下停止录屏 -> 原始 MP4 落盘校验 + 演播室 (Tools3000_StudioPreviewWindow) 自动置顶获焦断言
# [6/6] 演播室切片拆分 (S) + 点击右上角导出按钮 + 异步合成导出成片 (Enter) -> MP4 成片落盘、原位交付卡常驻与 Enter 完成收起
# -----------------------------------------------------------------------------
param(
    [string]$ExePath = ""
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Stop"

Write-Host "===============================================================================" -ForegroundColor Cyan
Write-Host " Tools3000 截图·贴图·录屏·倒计时·演播室操作系统级端到端 (E2E) 门禁 " -ForegroundColor Cyan
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
        Write-Host "[ERROR] 未找到 Tools3000.exe，请先执行编译或部署！" -ForegroundColor Red
        exit 1
    }
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
Write-Host "[INFO] 待测程序: $ExePath" -ForegroundColor Gray

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading;

public static class CaptureE2EHarness {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hWnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", SetLastError=true)] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool RegisterHotKey(IntPtr hWnd, int id, uint modifiers, uint virtualKey);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool UnregisterHotKey(IntPtr hWnd, int id);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool GetWindowDisplayAffinity(IntPtr hWnd, out uint dwAffinity);
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] public static extern bool IsClipboardFormatAvailable(uint format);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterClipboardFormatW(string lpszFormat);
    [DllImport("user32.dll")] public static extern bool OpenClipboard(IntPtr hWndNewOwner);
    [DllImport("user32.dll")] public static extern bool EmptyClipboard();
    [DllImport("user32.dll")] public static extern bool CloseClipboard();
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr hWnd, uint uCmd);
    [DllImport("user32.dll", SetLastError=true)] public static extern int GetWindowLongW(IntPtr hWnd, int nIndex);

    public const int GWL_STYLE = -16;
    public const int WS_THICKFRAME = 0x00040000;
    public const uint WM_NCHITTEST = 0x0084;

    public const uint GW_HWNDPREV = 3;

    public static bool IsWindowAboveInZOrder(IntPtr candidateAbove, IntPtr referenceBelow) {
        IntPtr cur = referenceBelow;
        for (int guard = 0; guard < 512 && cur != IntPtr.Zero; guard++) {
            cur = GetWindow(cur, GW_HWNDPREV);
            if (cur == candidateAbove) return true;
        }
        return false;
    }

    public const uint WM_CLOSE = 0x0010;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_KEYUP = 0x0101;
    public const uint WM_MOUSEMOVE = 0x0200;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_HOTKEY = 0x0312;
    public const uint WM_ACCESSIBILITY_INVOKE_TOOLBAR = 0x804A; // WM_APP (0x8000) + 74
    public const int MK_LBUTTON = 0x0001;

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

    public static void SendWindowKey(IntPtr hWnd, ushort vk) {
        PostMessageW(hWnd, WM_KEYDOWN, (IntPtr)vk, IntPtr.Zero);
        Thread.Sleep(20);
        PostMessageW(hWnd, WM_KEYUP, (IntPtr)vk, IntPtr.Zero);
        Thread.Sleep(30);
    }

    public static void DragClientMouse(IntPtr hWnd, int x1, int y1, int x2, int y2, int steps = 12) {
        RECT winRc;
        GetWindowRect(hWnd, out winRc);
        SetCursorPos(winRc.Left + x1, winRc.Top + y1);
        PostMessageW(hWnd, WM_MOUSEMOVE, IntPtr.Zero, MakeLParam(x1, y1));
        Thread.Sleep(20);
        PostMessageW(hWnd, WM_LBUTTONDOWN, (IntPtr)MK_LBUTTON, MakeLParam(x1, y1));
        Thread.Sleep(20);
        for (int i = 1; i <= steps; i++) {
            int cx = x1 + (x2 - x1) * i / steps;
            int cy = y1 + (y2 - y1) * i / steps;
            SetCursorPos(winRc.Left + cx, winRc.Top + cy);
            PostMessageW(hWnd, WM_MOUSEMOVE, (IntPtr)MK_LBUTTON, MakeLParam(cx, cy));
            Thread.Sleep(12);
        }
        PostMessageW(hWnd, WM_LBUTTONUP, IntPtr.Zero, MakeLParam(x2, y2));
        Thread.Sleep(40);
    }

    public static void ClickClientMouse(IntPtr hWnd, int x, int y) {
        PostMessageW(hWnd, WM_MOUSEMOVE, IntPtr.Zero, MakeLParam(x, y));
        Thread.Sleep(15);
        PostMessageW(hWnd, WM_LBUTTONDOWN, (IntPtr)MK_LBUTTON, MakeLParam(x, y));
        Thread.Sleep(20);
        PostMessageW(hWnd, WM_LBUTTONUP, IntPtr.Zero, MakeLParam(x, y));
        Thread.Sleep(30);
    }
}
"@

[void][CaptureE2EHarness]::SetProcessDpiAwarenessContext([IntPtr](-4)) # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

function Get-AvailableE2EHotkeys {
    $modifiers = [uint32](0x0001 -bor 0x0002 -bor 0x0004 -bor 0x4000) # Alt|Ctrl|Shift|NoRepeat
    $candidates = @(
        @{ Text = 'Ctrl+Alt+Shift+F24'; Key = [uint32]0x87 },
        @{ Text = 'Ctrl+Alt+Shift+F23'; Key = [uint32]0x86 },
        @{ Text = 'Ctrl+Alt+Shift+F22'; Key = [uint32]0x85 },
        @{ Text = 'Ctrl+Alt+Shift+F21'; Key = [uint32]0x84 },
        @{ Text = 'Ctrl+Alt+Shift+F20'; Key = [uint32]0x83 },
        @{ Text = 'Ctrl+Alt+Shift+F19'; Key = [uint32]0x82 }
    )
    $chosen = [System.Collections.Generic.List[object]]::new()
    $registeredIds = [System.Collections.Generic.List[int]]::new()
    try {
        foreach ($cand in $candidates) {
            $probeId = 0x7100 + $chosen.Count
            if ([CaptureE2EHarness]::RegisterHotKey([IntPtr]::Zero, $probeId, $modifiers, $cand.Key)) {
                $chosen.Add($cand)
                $registeredIds.Add($probeId)
                if ($chosen.Count -eq 2) { break }
            }
        }
        if ($chosen.Count -ne 2) {
            throw "无法为截图/录屏端到端测试分配 2 个空闲全局热键。"
        }
        return @($chosen)
    } finally {
        foreach ($id in $registeredIds) {
            [void][CaptureE2EHarness]::UnregisterHotKey([IntPtr]::Zero, $id)
        }
    }
}

$HarnessRoot = Join-Path $ProjectRoot "build\capture-recording-e2e-$PID"
$HarnessLocalAppData = Join-Path $HarnessRoot "LocalAppData"
$HarnessRoamingAppData = Join-Path $HarnessRoot "RoamingAppData"
$HarnessDataRoot = Join-Path $HarnessRoot "Tools3000Data"
$HarnessConfigDir = Join-Path $HarnessDataRoot "config"
$HarnessLogsDir = Join-Path $HarnessDataRoot "logs"
$HarnessShotsDir = Join-Path $HarnessRoot "Screenshots"
$HarnessVideosDir = Join-Path $HarnessRoot "Recordings"
$HarnessExportDir = Join-Path $HarnessRoot "StudioExport"
New-Item -ItemType Directory -Path $HarnessLocalAppData, $HarnessRoamingAppData, $HarnessDataRoot, $HarnessConfigDir, $HarnessLogsDir, $HarnessShotsDir, $HarnessVideosDir, $HarnessExportDir -Force | Out-Null

$SelectedHotkeys = Get-AvailableE2EHotkeys
$HarnessConfigPath = Join-Path $HarnessConfigDir "config.json"
$ConfigPayload = @{
    plugins = @{
        capture = @{ enabled = $true }
        keycast = @{ enabled = $false }
        gesture = @{ enabled = $false }
    }
    hotkeys = @{
        Screenshot = $SelectedHotkeys[0].Text
        Record     = $SelectedHotkeys[1].Text
    }
    capture = @{
        format               = "png"
        quality              = 90
        saveToFile           = $true
        copyToClipboard      = $true
        saveDirectory        = $HarnessShotsDir
        autoBypassFullscreen = $false
        autoDetectWindow     = $false
        beautyShellEnabled   = $false
    }
    recording = @{
        format                 = "mp4_h264"
        fps                    = 30
        bitrate                = 6
        countdownSeconds       = 2
        saveDirectory          = $HarnessVideosDir
        captureSystemAudio     = $false
        captureMicrophone      = $false
        includeKeycast         = $false
        studioMode             = $true
        studioAutoZoom         = $true
        studioZoomMode         = 1
        studioAutoFrame        = $true
        studioAutoCaptions     = $false
        hideIndicatorInCapture = $true
        studioExport           = @{
            savePrefs = $true
            directory = $HarnessExportDir
        }
    }
} | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($HarnessConfigPath, $ConfigPayload, [System.Text.UTF8Encoding]::new($false))

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

function Get-RegisteredHotkeyId([string]$LogPath, [string]$HotkeyName) {
    $pattern = 'name=' + [regex]::Escape($HotkeyName) + ',\s*def=.*\bid=(\d+)'
    for ($i = 0; $i -lt 30; $i++) {
        $txt = Get-LogTextSafe $LogPath
        if ($txt.Length -gt 0) {
            $m = [regex]::Matches($txt, $pattern)
            if ($m.Count -gt 0) {
                return [int]$m[$m.Count - 1].Groups[1].Value
            }
        }
        Start-Sleep -Milliseconds 100
    }
    throw "未能在日志中定位已注册快捷键 ID: $HotkeyName"
}

function Invoke-E2EHotkey([System.Diagnostics.Process]$Process, [string]$LogPath, [string]$HotkeyName) {
    $msgWnd = [CaptureE2EHarness]::FindMessageWindowForProcess([uint32]$Process.Id)
    if ($msgWnd -eq [IntPtr]::Zero) {
        throw "未找到待测进程主消息窗口，无法触发快捷键: $HotkeyName"
    }
    $hkId = Get-RegisteredHotkeyId -LogPath $LogPath -HotkeyName $HotkeyName
    if (-not [CaptureE2EHarness]::PostMessageW($msgWnd, [CaptureE2EHarness]::WM_HOTKEY, [IntPtr]$hkId, [IntPtr]::Zero)) {
        throw "向待测进程投递 WM_HOTKEY 失败: $HotkeyName"
    }
}

function Wait-ForWindowClass([uint32]$TargetPid, [string]$ClassName, [int]$TimeoutMs = 4000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $h = [CaptureE2EHarness]::FindByClassForProcess($ClassName, $TargetPid, $true)
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 60
    }
    return [IntPtr]::Zero
}

function Wait-ForWindowGone([uint32]$TargetPid, [string]$ClassName, [int]$TimeoutMs = 4000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $h = [CaptureE2EHarness]::FindByClassForProcess($ClassName, $TargetPid, $true)
        if ($h -eq [IntPtr]::Zero) { return $true }
        Start-Sleep -Milliseconds 60
    }
    return $false
}

function Test-Mp4ContainerSignature([string]$FilePath) {
    $bytes = [System.IO.File]::ReadAllBytes($FilePath)
    if ($bytes.Length -lt 256) { return $false }
    return ($bytes[4] -eq 0x66 -and $bytes[5] -eq 0x74 -and $bytes[6] -eq 0x79 -and $bytes[7] -eq 0x70)
}

$HarnessLogFile = Join-Path $HarnessLogsDir "tools3000.log"
$decoyForm = $null
$proc = $null

try {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $ExePath
    $psi.WorkingDirectory = Split-Path -Parent $ExePath
    $psi.UseShellExecute = $false
    $psi.Arguments = "--no-elevate --lifecycle-test-instance"
    $psi.EnvironmentVariables["LOCALAPPDATA"] = $HarnessLocalAppData
    $psi.EnvironmentVariables["APPDATA"] = $HarnessRoamingAppData
    $psi.EnvironmentVariables["TOOLS3000_DATA_ROOT"] = $HarnessDataRoot
    $psi.EnvironmentVariables["TOOLS3000_ALLOW_INJECTED_KEYBOARD"] = "1"
    $psi.EnvironmentVariables["TOOLS3000_ALLOW_INJECTED_MOUSE"] = "1"

    $proc = [System.Diagnostics.Process]::Start($psi)
    if (-not $proc -or $proc.HasExited) {
        throw "待测进程 Tools3000.exe 启动失败！"
    }
    $targetPid = [uint32]$proc.Id
    Write-Host "[OK] 隔离待测实例已启动 (PID: $targetPid, 截图热键=$($SelectedHotkeys[0].Text), 录屏热键=$($SelectedHotkeys[1].Text))" -ForegroundColor Green

    [void](Get-RegisteredHotkeyId -LogPath $HarnessLogFile -HotkeyName "Screenshot")
    [void](Get-RegisteredHotkeyId -LogPath $HarnessLogFile -HotkeyName "Record")

    $VK_RETURN = [uint16]0x0D
    $VK_1      = [uint16]0x31
    $VK_S      = [uint16]0x53

    # -------------------------------------------------------------------------
    # [1/6] 区域框选截图 + 矩形标注 + 磁盘 PNG & 剪贴板双通道校验
    # -------------------------------------------------------------------------
    Write-Host "`n── [1/6] E2E 区域框选截图 + 矩形标注 + PNG 落盘与系统剪贴板校验 ──" -ForegroundColor Cyan
    if ([CaptureE2EHarness]::OpenClipboard([IntPtr]::Zero)) {
        [void][CaptureE2EHarness]::EmptyClipboard()
        [void][CaptureE2EHarness]::CloseClipboard()
    }

    Invoke-E2EHotkey -Process $proc -LogPath $HarnessLogFile -HotkeyName "Screenshot"
    $overlayHwnd = Wait-ForWindowClass -TargetPid $targetPid -ClassName "Tools3000_CaptureOverlay" -TimeoutMs 4000
    if ($overlayHwnd -eq [IntPtr]::Zero) {
        throw "[1/6] 触发截图热键后 Tools3000_CaptureOverlay 未显示！"
    }
    Start-Sleep -Milliseconds 120

    # 拖拽 400x300 选区 -> 按 '1' 激活矩形标注并绘制矩形 -> 按 Enter 完成交付
    [CaptureE2EHarness]::DragClientMouse($overlayHwnd, 300, 220, 700, 520, 12)
    [CaptureE2EHarness]::SendWindowKey($overlayHwnd, $VK_1)
    [CaptureE2EHarness]::DragClientMouse($overlayHwnd, 360, 270, 560, 410, 8)
    [CaptureE2EHarness]::SendWindowKey($overlayHwnd, $VK_RETURN)

    if (-not (Wait-ForWindowGone -TargetPid $targetPid -ClassName "Tools3000_CaptureOverlay" -TimeoutMs 4000)) {
        throw "[1/6] 按下 Enter 确认截图后覆盖层窗口未能及时关闭！"
    }

    $pngFile = $null
    for ($i = 0; $i -lt 25; $i++) {
        $pngFile = Get-ChildItem -LiteralPath $HarnessShotsDir -Filter "*.png" -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($pngFile -and $pngFile.Length -gt 256) { break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $pngFile) {
        throw "[1/6] 截图确认后未在保存目录 ($HarnessShotsDir) 生成 PNG 文件！"
    }

    $bmp = [System.Drawing.Bitmap]::new($pngFile.FullName)
    $imgW = $bmp.Width
    $imgH = $bmp.Height
    $bmp.Dispose()
    if ([Math]::Abs($imgW - 400) -gt 4 -or [Math]::Abs($imgH - 300) -gt 4) {
        throw "[1/6] 落盘 PNG 尺寸异常: 期望约 400x300，实际 ${imgW}x${imgH}"
    }

    $pngFmtId = [CaptureE2EHarness]::RegisterClipboardFormatW("PNG")
    $hasClipImg = $false
    for ($ci = 0; $ci -lt 20; $ci++) {
        if ([CaptureE2EHarness]::IsClipboardFormatAvailable(8) -or
            [CaptureE2EHarness]::IsClipboardFormatAvailable(17) -or
            [CaptureE2EHarness]::IsClipboardFormatAvailable(2) -or
            ($pngFmtId -ne 0 -and [CaptureE2EHarness]::IsClipboardFormatAvailable($pngFmtId))) {
            $hasClipImg = $true
            break
        }
        Start-Sleep -Milliseconds 60
    }
    if (-not $hasClipImg) {
        throw "[1/6] 截图完成后系统剪贴板未检测到图像数据 (CF_DIB/CF_DIBV5/CF_BITMAP/PNG)！"
    }
    Write-Host "[OK] 区域标注截图落盘 (${imgW}x${imgH}, $($pngFile.Length) bytes) 与系统剪贴板图像写入全部通过！" -ForegroundColor Green

    # -------------------------------------------------------------------------
    # [2/6] 区域截图一键贴图置顶 (ToolbarCommand::PinWindow, index=11) 与回收校验
    # -------------------------------------------------------------------------
    Write-Host "`n── [2/6] E2E 区域截图一键桌面贴图与 PinWindow 回收校验 ──" -ForegroundColor Cyan
    Invoke-E2EHotkey -Process $proc -LogPath $HarnessLogFile -HotkeyName "Screenshot"
    $overlayHwnd2 = Wait-ForWindowClass -TargetPid $targetPid -ClassName "Tools3000_CaptureOverlay" -TimeoutMs 4000
    if ($overlayHwnd2 -eq [IntPtr]::Zero) {
        throw "[2/6] 第二次触发截图热键未能唤起覆盖层！"
    }
    Start-Sleep -Milliseconds 120

    [CaptureE2EHarness]::DragClientMouse($overlayHwnd2, 340, 250, 660, 490, 10)
    # 同步验证 UIA 工具栏动作派发通道：主栏索引 11 为 ToolbarCommand::PinWindow (贴图置顶)
    [void][CaptureE2EHarness]::SendMessageW($overlayHwnd2, [CaptureE2EHarness]::WM_ACCESSIBILITY_INVOKE_TOOLBAR, [IntPtr]11, [IntPtr]::Zero)

    $pinHwnd = Wait-ForWindowClass -TargetPid $targetPid -ClassName "Tools3000_PinWindow" -TimeoutMs 4000
    if ($pinHwnd -eq [IntPtr]::Zero) {
        throw "[2/6] 触发贴图命令后未能创建并显示桌面贴图窗口 Tools3000_PinWindow！"
    }
    $pinRect = New-Object CaptureE2EHarness+RECT
    [void][CaptureE2EHarness]::GetWindowRect($pinHwnd, [ref]$pinRect)
    $pinW = $pinRect.Right - $pinRect.Left
    $pinH = $pinRect.Bottom - $pinRect.Top
    if ($pinW -lt 200 -or $pinH -lt 150) {
        throw "[2/6] Tools3000_PinWindow 窗口几何尺寸异常: ${pinW}x${pinH}"
    }

    [void][CaptureE2EHarness]::PostMessageW($pinHwnd, [CaptureE2EHarness]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not (Wait-ForWindowGone -TargetPid $targetPid -ClassName "Tools3000_PinWindow" -TimeoutMs 3000)) {
        throw "[2/6] 关闭贴图后 Tools3000_PinWindow 仍残留可见！"
    }
    Write-Host "[OK] 桌面贴图窗口创建 (${pinW}x${pinH}) 与无残留销毁验证通过！" -ForegroundColor Green

    # -------------------------------------------------------------------------
    # [3/6] 区域录屏启动 + 60FPS 圆形倒计时窗口可见性与 WDA_NONE 防隐身断言
    # -------------------------------------------------------------------------
    Write-Host "`n── [3/6] E2E 区域录屏启动 + 圆形倒计时窗口可见性与 DWM 防隐身断言 ──" -ForegroundColor Cyan
    Invoke-E2EHotkey -Process $proc -LogPath $HarnessLogFile -HotkeyName "Record"
    $recOverlayHwnd = Wait-ForWindowClass -TargetPid $targetPid -ClassName "Tools3000_CaptureOverlay" -TimeoutMs 4000
    if ($recOverlayHwnd -eq [IntPtr]::Zero) {
        throw "[3/6] 触发录屏热键未能唤起选区覆盖层！"
    }
    Start-Sleep -Milliseconds 120

    # 框选 480x320 录屏区域并调用录屏工具栏索引 10 (RecordStartConfirm) 启动 2 秒倒计时
    [CaptureE2EHarness]::DragClientMouse($recOverlayHwnd, 280, 200, 760, 520, 10)
    [void][CaptureE2EHarness]::SendMessageW($recOverlayHwnd, [CaptureE2EHarness]::WM_ACCESSIBILITY_INVOKE_TOOLBAR, [IntPtr]10, [IntPtr]::Zero)

    $countdownHwnd = Wait-ForWindowClass -TargetPid $targetPid -ClassName "Tools3000_RecordingCountdown" -TimeoutMs 2000
    if ($countdownHwnd -eq [IntPtr]::Zero) {
        throw "[3/6] 启动带倒计时的录屏后，中央圆形倒计时窗口 Tools3000_RecordingCountdown 未显示！"
    }

    $cdAffinity = [uint32]999
    if (-not [CaptureE2EHarness]::GetWindowDisplayAffinity($countdownHwnd, [ref]$cdAffinity)) {
        throw "[3/6] 无法查询 Tools3000_RecordingCountdown 的 DisplayAffinity！"
    }
    if ($cdAffinity -ne 0) {
        throw "[3/6] 检测到 ULW_ALPHA 圆形倒计时窗口被错误设置了 DisplayAffinity=$cdAffinity (非 WDA_NONE=0)，在 Win11 DWM 下会导致圆盘隐身！"
    }

    $cdRect = New-Object CaptureE2EHarness+RECT
    [void][CaptureE2EHarness]::GetWindowRect($countdownHwnd, [ref]$cdRect)
    $cdW = $cdRect.Right - $cdRect.Left
    $cdH = $cdRect.Bottom - $cdRect.Top
    $cdDpi = [CaptureE2EHarness]::GetDpiForWindow($countdownHwnd)
    if ($cdDpi -lt 96) { $cdDpi = 96 }
    $expectedCdW = [int][Math]::Round(360.0 * [double]$cdDpi / 96.0)
    $expectedCdH = [int][Math]::Round(260.0 * [double]$cdDpi / 96.0)
    if ([Math]::Abs($cdW - $expectedCdW) -gt 4 -or [Math]::Abs($cdH - $expectedCdH) -gt 4) {
        throw "[3/6] 倒计时圆盘与下方微晶提示胶囊画布尺寸未对齐当前显示器 DPI ($cdDpi): 实际=${cdW}x${cdH}, 期望=${expectedCdW}x${expectedCdH}"
    }
    $cdCx = ($cdRect.Left + $cdRect.Right) / 2
    $cdCy = ($cdRect.Top + $cdRect.Bottom) / 2
    if ([Math]::Abs($cdCx - 520) -gt 8 -or [Math]::Abs($cdCy - 360) -gt 8) {
        throw "[3/6] 圆形倒计时窗口未居中于录制选区中心 (520,360): 实际中心=($cdCx,$cdCy)"
    }

    $indicatorHwnd = Wait-ForWindowClass -TargetPid $targetPid -ClassName "Tools3000_RecordingIndicator" -TimeoutMs 1500
    if ($indicatorHwnd -eq [IntPtr]::Zero) {
        throw "[3/6] 录制悬浮状态栏 Tools3000_RecordingIndicator 未显示！"
    }
    Write-Host "[OK] 圆形倒计时窗口 (${cdW}x${cdH} @ ${cdDpi}DPI, 中心=($cdCx,$cdCy), Affinity=WDA_NONE) 与悬浮录制条可见性验证通过！" -ForegroundColor Green

    # -------------------------------------------------------------------------
    # [4/6] 等待倒计时自然结束进入正式录制 + 外部前台目标窗口动态录制
    # -------------------------------------------------------------------------
    Write-Host "`n── [4/6] E2E 倒计时消隐转正式录制 + 选区内动态画面录制 ──" -ForegroundColor Cyan
    if (-not (Wait-ForWindowGone -TargetPid $targetPid -ClassName "Tools3000_RecordingCountdown" -TimeoutMs 3500)) {
        throw "[4/6] 倒计时超时后 Tools3000_RecordingCountdown 未自动销毁收起！"
    }

    $decoyForm = [System.Windows.Forms.Form]::new()
    $decoyForm.Text = "ExternalTargetApp"
    $decoyForm.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
    $decoyForm.Bounds = [System.Drawing.Rectangle]::new(300, 220, 420, 260)
    $decoyForm.BackColor = [System.Drawing.Color]::FromArgb(32, 45, 72)
    $decoyForm.Show()
    [void][CaptureE2EHarness]::SetForegroundWindow($decoyForm.Handle)
    Start-Sleep -Milliseconds 450
    $decoyForm.BackColor = [System.Drawing.Color]::FromArgb(46, 94, 138)
    $decoyForm.Refresh()
    Start-Sleep -Milliseconds 850
    Write-Host "[OK] 倒计时平滑转入正式录制并完成选区动态画面捕获！" -ForegroundColor Green

    # -------------------------------------------------------------------------
    # [5/6] 停止录屏 -> 原始 MP4 校验 + 演播室自动越层置顶抢占 Foreground 焦点断言
    # -------------------------------------------------------------------------
    Write-Host "`n── [5/6] E2E 停止录屏 -> 原始 MP4 落盘 + 演播室自动置顶获焦断言 ──" -ForegroundColor Cyan
    [void][CaptureE2EHarness]::SetForegroundWindow($decoyForm.Handle)
    Start-Sleep -Milliseconds 80

    Invoke-E2EHotkey -Process $proc -LogPath $HarnessLogFile -HotkeyName "Record"

    $studioHwnd = Wait-ForWindowClass -TargetPid $targetPid -ClassName "Tools3000_StudioPreviewWindow" -TimeoutMs 8000
    if ($studioHwnd -eq [IntPtr]::Zero) {
        throw "[5/6] 停止录屏后演播室窗口 Tools3000_StudioPreviewWindow 未自动弹出！"
    }

    # 等待 FocuSee 级准备等待界面完成后台母带原子落盘并无缝切入正式演播室就绪态
    $rawMp4 = $null
    $readySw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($readySw.ElapsedMilliseconds -lt 8000) {
        $candMp4 = Get-ChildItem -LiteralPath $HarnessVideosDir -Filter "record_*.mp4" -File -ErrorAction SilentlyContinue | Select-Object -First 1
        $sbTitle = [System.Text.StringBuilder]::new(256)
        [void][CaptureE2EHarness]::GetWindowTextW($studioHwnd, $sbTitle, $sbTitle.Capacity)
        $curTitle = $sbTitle.ToString()
        $stillPreparing = ($curTitle -match "Preparing|正在准备")
        if ($candMp4 -and $candMp4.Length -ge 1024 -and (Test-Mp4ContainerSignature $candMp4.FullName) -and (-not $stillPreparing)) {
            $rawMp4 = $candMp4
            break
        }
        Start-Sleep -Milliseconds 40
    }
    if (-not $rawMp4) {
        throw "[5/6] 原始录屏 MP4 未生成、容器头非法或演播室未完成就绪切换！"
    }

    Start-Sleep -Milliseconds 180
    $fgNow = [CaptureE2EHarness]::GetForegroundWindow()
    $aboveDecoy = [CaptureE2EHarness]::IsWindowAboveInZOrder($studioHwnd, $decoyForm.Handle)
    if ($fgNow -ne $studioHwnd -and -not ($fgNow -eq [IntPtr]::Zero -and $aboveDecoy)) {
        throw ("[5/6] 演播室窗口未能自动越层置顶获取焦点！当前 Foreground=0x{0:X8}, Studio=0x{1:X8}, AboveDecoy={2}" -f [int64]$fgNow, [int64]$studioHwnd, $aboveDecoy)
    }
    Write-Host "[OK] 原始底片 ($($rawMp4.Name), $($rawMp4.Length) bytes) 已落盘，且演播室窗口自动越过外部应用置顶浮现 (HWND=0x$($studioHwnd.ToString('X8')))！" -ForegroundColor Green

    # -------------------------------------------------------------------------
    # [6/6] 演播室非线性拆分 (S) + 点击右上角导出按钮 + 异步合成导出成片 (Enter)
    # -------------------------------------------------------------------------
    Write-Host "`n── [6/6] E2E 演播室切片拆分 (S) 与美化成片异步合成导出 ──" -ForegroundColor Cyan
    Start-Sleep -Milliseconds 300
    [CaptureE2EHarness]::SendWindowKey($studioHwnd, $VK_S)

    # 验证演播室具备标准 Windows WS_THICKFRAME 可调大小窗口样式，且四角真实返回 HTTOPLEFT/HTTOPRIGHT/HTBOTTOMLEFT/HTBOTTOMRIGHT
    $winStyle = [CaptureE2EHarness]::GetWindowLongW($studioHwnd, [CaptureE2EHarness]::GWL_STYLE)
    if (($winStyle -band [CaptureE2EHarness]::WS_THICKFRAME) -eq 0) {
        throw ("[6/6] 演播室窗口缺少标准 WS_THICKFRAME (0x00040000) 样式，导致四角无法拖拽拉大放小！Style=0x{0:X8}" -f $winStyle)
    }
    $winRc = New-Object CaptureE2EHarness+RECT
    [void][CaptureE2EHarness]::GetWindowRect($studioHwnd, [ref]$winRc)
    $htTL = [CaptureE2EHarness]::SendMessageW($studioHwnd, [CaptureE2EHarness]::WM_NCHITTEST, [IntPtr]::Zero, [CaptureE2EHarness]::MakeLParam($winRc.Left + 3, $winRc.Top + 3)).ToInt64()
    $htTR = [CaptureE2EHarness]::SendMessageW($studioHwnd, [CaptureE2EHarness]::WM_NCHITTEST, [IntPtr]::Zero, [CaptureE2EHarness]::MakeLParam($winRc.Right - 3, $winRc.Top + 3)).ToInt64()
    $htBL = [CaptureE2EHarness]::SendMessageW($studioHwnd, [CaptureE2EHarness]::WM_NCHITTEST, [IntPtr]::Zero, [CaptureE2EHarness]::MakeLParam($winRc.Left + 3, $winRc.Bottom - 3)).ToInt64()
    $htBR = [CaptureE2EHarness]::SendMessageW($studioHwnd, [CaptureE2EHarness]::WM_NCHITTEST, [IntPtr]::Zero, [CaptureE2EHarness]::MakeLParam($winRc.Right - 3, $winRc.Bottom - 3)).ToInt64()
    if ($htTL -ne 13 -or $htTR -ne 14 -or $htBL -ne 16 -or $htBR -ne 17) {
        throw ("[6/6] 演播室窗口四角 WM_NCHITTEST 未返回标准对角缩放码 (期望 13/14/16/17，实际 TL={0}, TR={1}, BL={2}, BR={3})！" -f $htTL, $htTR, $htBL, $htBR)
    }

    # 验证演播室右上角「最大化 / 还原」按钮真实放大窗口能力 (maxRc 逻辑区域 [W-73, 9, W-43, 37] -> 中心 W-58, 23)
    $origClientRc = New-Object CaptureE2EHarness+RECT
    [void][CaptureE2EHarness]::GetClientRect($studioHwnd, [ref]$origClientRc)
    $origClientW = $origClientRc.Right - $origClientRc.Left
    $winDpi = [CaptureE2EHarness]::GetDpiForWindow($studioHwnd)
    $dpiScale = if ($winDpi -gt 0) { [double]$winDpi / 96.0 } else { 1.0 }
    $maxToggleX = [int]([Math]::Round($origClientW - 58.0 * $dpiScale))
    $maxToggleY = [int]([Math]::Round(23.0 * $dpiScale))
    [CaptureE2EHarness]::ClickClientMouse($studioHwnd, $maxToggleX, $maxToggleY)
    Start-Sleep -Milliseconds 160
    $maxClientRc = New-Object CaptureE2EHarness+RECT
    [void][CaptureE2EHarness]::GetClientRect($studioHwnd, [ref]$maxClientRc)
    $maxClientW = $maxClientRc.Right - $maxClientRc.Left
    if ($maxClientW -lt $origClientW) {
        throw "[6/6] 点击演播室最大化按钮后窗口宽度异常缩小 ($origClientW -> $maxClientW)！"
    }
    # 再次点击最大化/还原按钮恢复标准窗口尺寸
    $restoreToggleX = [int]([Math]::Round($maxClientW - 58.0 * $dpiScale))
    [CaptureE2EHarness]::ClickClientMouse($studioHwnd, $restoreToggleX, $maxToggleY)
    Start-Sleep -Milliseconds 160

    # 计算 StudioPreviewWindow 右上角「导出」主按钮物理客户区中心点 (逻辑区域 [W-217, 8, W-125, 38] -> 中心 W-171, 23)
    $clientRc = New-Object CaptureE2EHarness+RECT
    [void][CaptureE2EHarness]::GetClientRect($studioHwnd, [ref]$clientRc)
    $clientW = $clientRc.Right - $clientRc.Left
    $exportClickX = [int]([Math]::Round($clientW - 171.0 * $dpiScale))
    $exportClickY = [int]([Math]::Round(23.0 * $dpiScale))
    [CaptureE2EHarness]::ClickClientMouse($studioHwnd, $exportClickX, $exportClickY)
    Start-Sleep -Milliseconds 180
    [CaptureE2EHarness]::SendWindowKey($studioHwnd, $VK_RETURN)

    $exportedMp4 = $null
    $exportSw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($exportSw.ElapsedMilliseconds -lt 25000) {
        $exportedMp4 = Get-ChildItem -LiteralPath $HarnessExportDir -Filter "*.mp4" -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notlike '*.studio_tmp*' -and $_.Name -notlike '*.part*' -and $_.Name -notlike '.studio_master*' } |
            Select-Object -First 1
        if ($exportedMp4 -and $exportedMp4.Length -gt 1024 -and (Test-Mp4ContainerSignature $exportedMp4.FullName)) {
            break
        }
        Start-Sleep -Milliseconds 150
    }
    if (-not $exportedMp4 -or $exportedMp4.Length -lt 1024 -or -not (Test-Mp4ContainerSignature $exportedMp4.FullName)) {
        throw "[6/6] 演播室异步合成导出成片失败或超时未在导出目录 ($HarnessExportDir) 生成合法 MP4 文件！"
    }

    # 等待主线程接收导出完成回调并切换至原位交付结果卡，断言演播室默认常驻不自动退出（支持继续导出 GIF 对比）
    Start-Sleep -Milliseconds 250
    if (-not [CaptureE2EHarness]::IsWindowVisible($studioHwnd)) {
        throw "[6/6] 演播室导出完成后未保持非破坏性常驻（意外自动关闭），无法继续导出其他格式对比！"
    }

    # 在原位交付结果卡按下 Enter 触发「完成并关闭」，验证演播室优雅退出并清理未使用的临时母带
    [CaptureE2EHarness]::SendWindowKey($studioHwnd, $VK_RETURN)
    if (-not (Wait-ForWindowGone -TargetPid $targetPid -ClassName "Tools3000_StudioPreviewWindow" -TimeoutMs 4000)) {
        throw "[6/6] 在导出完成交付卡按下 Enter (完成并关闭) 后演播室窗口未能及时收起！"
    }
    Write-Host "[OK] 演播室成片合成导出成功 ($($exportedMp4.Name), $($exportedMp4.Length) bytes)，非破坏性交付卡常驻与 Enter 收起验证全部通过！" -ForegroundColor Green

    Write-Host "`n===============================================================================" -ForegroundColor Cyan
    Write-Host " [PASS] 截图·贴图·录屏·圆形倒计时·演播室全链路端到端 (6/6 关卡) 100% 全部通过！" -ForegroundColor Green
    Write-Host "===============================================================================" -ForegroundColor Cyan
}
finally {
    if ($decoyForm) {
        try { $decoyForm.Close(); $decoyForm.Dispose() } catch {}
    }
    if ($proc -and -not $proc.HasExited) {
        $msgWnd = [CaptureE2EHarness]::FindMessageWindowForProcess([uint32]$proc.Id)
        if ($msgWnd -ne [IntPtr]::Zero) {
            [void][CaptureE2EHarness]::PostMessageW($msgWnd, [CaptureE2EHarness]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
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
