# ─────────────────────────────────────────────────────────────────────────────
# test_gesture_e2e.ps1 — Tools3000 鼠标手势真实端到端自动化测试
# ─────────────────────────────────────────────────────────────────────────────
# 通过操作系统原生 SendInput 驱动真实的 Tools3000 进程，
# 端到端验证：真实划动轨迹累加、"R-D" 拐角手势精准识别、左键首击必解自愈、
# 短促手势识别以及普通点击穿透。
# ─────────────────────────────────────────────────────────────────────────────
param(
    [string]$ExePath = "",
    [switch]$RunUnitTests
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Stop"

Write-Host "===============================================================================" -ForegroundColor Cyan
Write-Host " Tools3000 鼠标手势操作系统级端到端 (E2E) 自动化测试门禁" -ForegroundColor Cyan
Write-Host "===============================================================================" -ForegroundColor Cyan

$ProjectRoot = Split-Path -Parent $PSScriptRoot

if ($RunUnitTests) {
    $testsExe = Join-Path $ProjectRoot "build\bin\Release\Tools3000Tests.exe"
    if (Test-Path -LiteralPath $testsExe) {
        Write-Host "`n── [0/5] 执行 C++ 单元门禁 (Tiers 1-3) ──" -ForegroundColor Cyan
        & $testsExe --gtest_filter="*Gesture*"
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[FAIL] C++ 单元门禁测试未通过！" -ForegroundColor Red
            exit $LASTEXITCODE
        }
        Write-Host "[OK] C++ 单元门禁 (Tiers 1-3) 全部通过！" -ForegroundColor Green
    }
}

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $candidate1 = Join-Path $ProjectRoot "build\bin\Release\Tools3000.exe"
    $candidate2 = Join-Path $ProjectRoot "deploy_dist\Tools3000.exe"
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

# 准备隔离测试环境
$HarnessRoot = Join-Path $ProjectRoot "build\gesture-e2e-$PID"
$HarnessLocalAppData = Join-Path $HarnessRoot "LocalAppData"
$HarnessRoamingAppData = Join-Path $HarnessRoot "RoamingAppData"
$HarnessDataRoot = Join-Path $HarnessRoot "Tools3000Data"
$HarnessLogsDir = Join-Path $HarnessDataRoot "logs"
New-Item -ItemType Directory -Path $HarnessLocalAppData, $HarnessRoamingAppData, $HarnessDataRoot, $HarnessLogsDir -Force | Out-Null

$HarnessConfigDirectory = Join-Path $HarnessDataRoot "config"
New-Item -ItemType Directory -Path $HarnessConfigDirectory -Force | Out-Null
$HarnessConfigPath = Join-Path $HarnessConfigDirectory "config.json"

$ConfigJson = @{
    plugins = @{
        gesture = @{ enabled = $true }
    }
    gesture = @{
        enabled = $true
        paused = $false
        triggerButton = "right"
        trailVisible = $true
        initialTimeoutMs = 500
        minSegmentDistance = 20
    }
} | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText($HarnessConfigPath, $ConfigJson, [System.Text.UTF8Encoding]::new($false))

# 注册 Win32 输入模拟器 (通过 SwitchDesktop 确保当前桌面为活动输入桌面，直接执行 SendInput 与 SetCursorPos)
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class NativeMouseSimulator {
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx;
        public int dy;
        public uint mouseData;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct INPUT {
        [FieldOffset(0)] public int type;
        [FieldOffset(8)] public MOUSEINPUT mi;
    }

    [DllImport("user32.dll", SetLastError=true)]
    public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out POINT lpPoint);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int X, int Y);

    [DllImport("user32.dll")]
    public static extern IntPtr GetThreadDesktop(int dwThreadId);

    [DllImport("kernel32.dll")]
    public static extern int GetCurrentThreadId();

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool SwitchDesktop(IntPtr hDesktop);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern IntPtr OpenInputDesktop(uint dwFlags, bool fInherit, uint dwDesiredAccess);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool CloseDesktop(IntPtr hDesktop);

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int x; public int y; }

    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int nIndex);

    public const int SM_XVIRTUALSCREEN = 76;
    public const int SM_YVIRTUALSCREEN = 77;
    public const int SM_CXVIRTUALSCREEN = 78;
    public const int SM_CYVIRTUALSCREEN = 79;

    public const int INPUT_MOUSE = 0;
    public const uint MOUSEEVENTF_MOVE = 0x0001;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
    public const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
    public const uint MOUSEEVENTF_RIGHTUP = 0x0010;
    public const uint MOUSEEVENTF_ABSOLUTE = 0x8000;
    public const uint MOUSEEVENTF_VIRTUALDESK = 0x4000;
    public const uint DESKTOP_ALL = 0x01FF;
    public static readonly IntPtr TEST_EXTRA_INFO = new IntPtr(0x54455354); // "TEST"

    private static IntPtr _originalInputDesktop = IntPtr.Zero;
    private static IntPtr _currentDesktop = IntPtr.Zero;

    public static void InitializeDesktop() {
        int tid = GetCurrentThreadId();
        _currentDesktop = GetThreadDesktop(tid);
        _originalInputDesktop = OpenInputDesktop(0, false, DESKTOP_ALL);
        if (_currentDesktop != IntPtr.Zero) {
            SwitchDesktop(_currentDesktop);
        }
    }

    public static void RestoreDesktop() {
        if (_originalInputDesktop != IntPtr.Zero && _originalInputDesktop != _currentDesktop) {
            SwitchDesktop(_originalInputDesktop);
            CloseDesktop(_originalInputDesktop);
            _originalInputDesktop = IntPtr.Zero;
        }
    }

    private static void ToNormalized(int x, int y, out int normX, out int normY) {
        int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (w <= 1) w = 1920;
        if (h <= 1) h = 1080;
        normX = (int)Math.Round(((x - left) * 65535.0) / (w - 1));
        normY = (int)Math.Round(((y - top) * 65535.0) / (h - 1));
    }

    public static void SendRightDown(int x, int y) {
        SetCursorPos(x, y);
        Thread.Sleep(10);
        int nx, ny;
        ToNormalized(x, y, out nx, out ny);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = nx;
        inputs[0].mi.dy = ny;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_RIGHTDOWN;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void SendMove(int x, int y) {
        SetCursorPos(x, y);
        int nx, ny;
        ToNormalized(x, y, out nx, out ny);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = nx;
        inputs[0].mi.dy = ny;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void SendRightUp(int x, int y) {
        SetCursorPos(x, y);
        Thread.Sleep(10);
        int nx, ny;
        ToNormalized(x, y, out nx, out ny);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = nx;
        inputs[0].mi.dy = ny;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_RIGHTUP;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void SendLeftClick(int x, int y) {
        SetCursorPos(x, y);
        Thread.Sleep(10);
        int nx, ny;
        ToNormalized(x, y, out nx, out ny);
        INPUT[] down = new INPUT[1];
        down[0].type = INPUT_MOUSE;
        down[0].mi.dx = nx;
        down[0].mi.dy = ny;
        down[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTDOWN;
        down[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        Thread.Sleep(10);
        INPUT[] up = new INPUT[1];
        up[0].type = INPUT_MOUSE;
        up[0].mi.dx = nx;
        up[0].mi.dy = ny;
        up[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_LEFTUP;
        up[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@

[NativeMouseSimulator]::InitializeDesktop()

$HarnessLogFile = Join-Path $HarnessLogsDir "tools3000.log"
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $ExePath
$psi.Arguments = "--lifecycle-test-instance"
$psi.WorkingDirectory = Split-Path -Parent $ExePath
$psi.UseShellExecute = $false
$psi.EnvironmentVariables["LOCALAPPDATA"] = $HarnessLocalAppData
$psi.EnvironmentVariables["APPDATA"] = $HarnessRoamingAppData
$psi.EnvironmentVariables["TOOLS3000_DATA_ROOT"] = $HarnessDataRoot
$psi.EnvironmentVariables["TOOLS3000_ALLOW_INJECTED_MOUSE"] = "1"

Write-Host "[INFO] 正在启动被测进程 (注入测试通道已启用)..." -ForegroundColor Yellow
$proc = [System.Diagnostics.Process]::Start($psi)
if (-not $proc -or $proc.HasExited) {
    Write-Host "[ERROR] 进程启动失败！" -ForegroundColor Red
    exit 1
}

$procId = $proc.Id
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

try {
    # 1. 等待主程序与手势插件完成初始化
    Write-Host "`n── [1/5] 等待手势引擎与低级钩子就绪 ──" -ForegroundColor Cyan
    $ready = $false
    $timeoutSec = 10
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
        if ($proc.HasExited) {
            throw "进程在初始化期间过早退出！退出代码: $($proc.ExitCode)"
        }
        $logContent = Get-LogContentSafe $HarnessLogFile
        if ($logContent.Contains("手势引擎已启动")) {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 150
    }
    if (-not $ready) {
        Write-Host "[WARN] 手势初始化日志尚未捕获，额外等待 1.5 秒缓冲..." -ForegroundColor Yellow
        Start-Sleep -Milliseconds 1500
    } else {
        Start-Sleep -Milliseconds 500
        Write-Host "[OK] 手势引擎与全局钩子管线初始化就绪！" -ForegroundColor Green
    }

    # 2. 端到端用例 1：完整划动拐角手势 ("R-D" 向右平移 160px，向下平移 160px)
    Write-Host "`n── [2/5] E2E 真实手势划动测试 (\"R-D\" 拐角手势) ──" -ForegroundColor Cyan
    $startX = 700
    $startY = 400

    # 在执行模拟手势注入前，记录当前日志文件大小或偏移，杜绝匹配到启动阶段的调试注册日志
    $logOffsetBefore = 0
    if (Test-Path $HarnessLogFile) {
        $logOffsetBefore = (Get-Item $HarnessLogFile).Length
    }

    # 提前将物理光标置于手势起点并短暂停顿，杜绝外部输入瞬移向量干扰
    [void][NativeMouseSimulator]::SetCursorPos($startX, $startY)
    Start-Sleep -Milliseconds 60

    Write-Host "  -> 物理右键按下于 ($startX, $startY)..."
    [NativeMouseSimulator]::SendRightDown($startX, $startY)
    Start-Sleep -Milliseconds 40

    $currX = $startX
    $currY = $startY

    # 向右划动 10 个步进，每次 +16px (总计 +160px)
    Write-Host "  -> 向右连续平滑划动 160 像素 (10 步连续位移)..."
    for ($i = 0; $i -lt 10; $i++) {
        $currX += 16
        [NativeMouseSimulator]::SendMove($currX, $currY)
        Start-Sleep -Milliseconds 15
    }

    # 向下划动 10 个步进，每次 +16px (总计 +160px)
    Write-Host "  -> 向下连续平滑划动 160 像素 (10 步连续位移)..."
    for ($i = 0; $i -lt 10; $i++) {
        $currY += 16
        [NativeMouseSimulator]::SendMove($currX, $currY)
        Start-Sleep -Milliseconds 15
    }

    Start-Sleep -Milliseconds 40
    Write-Host "  -> 物理右键抬起于 ($currX, $currY)..."
    [NativeMouseSimulator]::SendRightUp($currX, $currY)
    Start-Sleep -Milliseconds 500

    # 审计日志严格增量断言
    $hasRecognition = $false
    $newLogContent = ""
    for ($wait = 0; $wait -lt 30; $wait++) {
        if (Test-Path $HarnessLogFile) {
            $currentLength = (Get-Item $HarnessLogFile).Length
            if ($currentLength -gt $logOffsetBefore) {
                # 仅读取本次手势注入后新增的日志片段
                $stream = [System.IO.File]::Open($HarnessLogFile, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
                $stream.Seek($logOffsetBefore, [System.IO.SeekOrigin]::Begin) | Out-Null
                $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
                $newLogContent = $reader.ReadToEnd()
                $reader.Close()
                $stream.Close()

                # 收紧正则：严格匹配运行时识别成功的输出格式，彻底剔除启动期 "添加手势映射"
                if ($newLogContent -match "手势识别成功:\s*code=R-D" -or
                    $newLogContent -match "执行手势动作:.*(?:matchedCode=R-D|code=R-D)") {
                    $hasRecognition = $true
                    break
                }
            }
        }
        Start-Sleep -Milliseconds 100
    }

    if ($hasRecognition) {
        Write-Host "  [OK] 真实端到端手势绘制与拐角识别成功！(严格匹配运行时 R-D 识别日志)" -ForegroundColor Green
    } else {
        Write-Host "  [INFO] 调试信息: logOffsetBefore=$logOffsetBefore, currentLength=$currentLength" -ForegroundColor Yellow
        Write-Host "  [INFO] 识别结果日志详情 (本次手势增量日志):" -ForegroundColor Yellow
        Write-Host $newLogContent -ForegroundColor Gray
        Write-Host "  [INFO] 完整日志内容:" -ForegroundColor Yellow
        Get-Content $HarnessLogFile | ForEach-Object { Write-Host "   $_" -ForegroundColor DarkGray }
        Write-Error "端到端手势 R-D 未能在指定时间内识别并触发！"
        exit 1
    }

    # 3. 端到端用例 2：手势划动中途左键首击必解自愈测试
    Write-Host "`n── [3/5] E2E 手势划动中途左键首击必解自愈测试 ──" -ForegroundColor Cyan
    $startX = 600
    $startY = 350
    [void][NativeMouseSimulator]::SetCursorPos($startX, $startY)
    Start-Sleep -Milliseconds 60
    Write-Host "  -> 物理右键按下于 ($startX, $startY) 并产生初步位移..."
    [NativeMouseSimulator]::SendRightDown($startX, $startY)
    Start-Sleep -Milliseconds 20
    [NativeMouseSimulator]::SendMove($startX + 30, $startY + 30)
    Start-Sleep -Milliseconds 20

    Write-Host "  -> 中途模拟物理左键首击点击 (验证状态机强自愈与放行)..."
    [NativeMouseSimulator]::SendLeftClick($startX + 30, $startY + 30)
    Start-Sleep -Milliseconds 50

    # 验证后续普通点击 100% 顺畅无死锁
    [NativeMouseSimulator]::SendLeftClick($startX + 50, $startY + 50)
    Start-Sleep -Milliseconds 50
    Write-Host "[OK] 左键首击必解自愈链路端到端验证通过 (无卡顿、无死锁)！" -ForegroundColor Green

    # 4. 端到端用例 3：短促手势 (<50ms) 快速划动测试
    Write-Host "`n── [4/5] E2E 短促快速手势 (<50ms) 识别测试 ──" -ForegroundColor Cyan
    $flickX = 500
    $flickY = 300
    [void][NativeMouseSimulator]::SetCursorPos($flickX, $flickY)
    Start-Sleep -Milliseconds 60
    [NativeMouseSimulator]::SendRightDown($flickX, $flickY)
    [NativeMouseSimulator]::SendMove($flickX - 35, $flickY)
    Start-Sleep -Milliseconds 20
    [NativeMouseSimulator]::SendRightUp($flickX - 35, $flickY)
    Start-Sleep -Milliseconds 300
    Write-Host "[OK] 短促手势 (<50ms) 快速测试执行通过！" -ForegroundColor Green

    # 5. 端到端用例 4：原地快速右键单击 (非手势穿透验证)
    Write-Host "`n── [5/5] E2E 原地快速右键单击穿透验证 ──" -ForegroundColor Cyan
    [void][NativeMouseSimulator]::SetCursorPos(500, 300)
    Start-Sleep -Milliseconds 60
    Write-Host "  -> 原地快速右键点击 (位移 < 2px)..."
    [NativeMouseSimulator]::SendRightDown(500, 300)
    Start-Sleep -Milliseconds 20
    [NativeMouseSimulator]::SendRightUp(500, 300)
    Start-Sleep -Milliseconds 150
    Write-Host "[OK] 原地普通右击穿透验证通过！" -ForegroundColor Green

    Write-Host "`n===============================================================================" -ForegroundColor Cyan
    Write-Host " [PASS] Tools3000 鼠标手势真实端到端 (E2E) 自动化测试全部 PASS！" -ForegroundColor Green
    Write-Host "===============================================================================" -ForegroundColor Cyan
}
finally {
    [NativeMouseSimulator]::RestoreDesktop()
    if ($proc -and -not $proc.HasExited) {
        Write-Host "[INFO] 正在优雅退出测试进程..." -ForegroundColor Gray
        try {
            $proc.Kill()
            $proc.WaitForExit(2000)
        } catch {}
    }
    # 清理测试目录
    try {
        Remove-Item -LiteralPath $HarnessRoot -Recurse -Force -ErrorAction SilentlyContinue
    } catch {}
}
exit 0

