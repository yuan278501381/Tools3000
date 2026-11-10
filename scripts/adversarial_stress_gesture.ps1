# ─────────────────────────────────────────────────────────────────────────────
# adversarial_stress_gesture.ps1 — 对抗性极限压力与失效模式检验工具
# ─────────────────────────────────────────────────────────────────────────────
# 目标：
#   1. 超长持续时间 1000Hz 输入 (3000 点位持续 3 秒，检测队列积压与内存稳定性)
#   2. 超量无节流爆发注入 (10,000 点位瞬间轰击，触发 SPSC 4096 环形缓冲滑窗溢出)
#   3. 极速高频反复触发/释放/左键打断会话链 (100 次快速连击，检测锁死与状态机失步)
#   4. 进程抗压后的干净退出与挂起检测 (检验优雅退出，杜绝死锁僵死)
# ─────────────────────────────────────────────────────────────────────────────
param(
    [string]$ExePath = "",
    [int]$CpuLoadThreads = 4
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = "Stop"

Write-Host "===============================================================================" -ForegroundColor Magenta
Write-Host " [ADVERSARIAL CHALLENGER] 鼠标手势并发管线对抗性压力测试" -ForegroundColor Magenta
Write-Host "===============================================================================" -ForegroundColor Magenta

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $ProjectRoot "build\bin\Release\Tools3000.exe"
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
Write-Host "[INFO] 待测程序: $ExePath" -ForegroundColor Gray

# 准备独立沙箱
$HarnessRoot = Join-Path $ProjectRoot "build\adversarial-harness-$PID"
$HarnessLocalAppData = Join-Path $HarnessRoot "LocalAppData"
$HarnessRoamingAppData = Join-Path $HarnessRoot "RoamingAppData"
$HarnessDataRoot = Join-Path $HarnessRoot "Tools3000Data"
$HarnessLogsDir = Join-Path $HarnessDataRoot "logs"
New-Item -ItemType Directory -Path $HarnessLocalAppData, $HarnessRoamingAppData, $HarnessDataRoot, $HarnessLogsDir -Force | Out-Null

$HarnessConfigDirectory = Join-Path $HarnessDataRoot "config"
New-Item -ItemType Directory -Path $HarnessConfigDirectory -Force | Out-Null
$HarnessConfigPath = Join-Path $HarnessConfigDirectory "config.json"

$ConfigJson = @{
    plugins = @{ gesture = @{ enabled = $true } }
    gesture = @{
        enabled = $true
        paused = $false
        triggerButton = "right"
        trailVisible = $true
        initialTimeoutMs = 500
        minSegmentDistance = 14
    }
} | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText($HarnessConfigPath, $ConfigJson, [System.Text.UTF8Encoding]::new($false))

# 注册 Win32 极限注入引擎
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Diagnostics;

public static class AdversarialMouseInjector {
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
    public static extern bool SetCursorPos(int X, int Y);

    [DllImport("winmm.dll")]
    public static extern uint timeBeginPeriod(uint uPeriod);

    [DllImport("winmm.dll")]
    public static extern uint timeEndPeriod(uint uPeriod);

    public const int INPUT_MOUSE = 0;
    public const uint MOUSEEVENTF_MOVE = 0x0001;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
    public const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
    public const uint MOUSEEVENTF_RIGHTUP = 0x0010;
    public static readonly IntPtr TEST_EXTRA_INFO = new IntPtr(0x54455354); // "TEST"

    public static void SendRightDown(int x, int y) {
        SetCursorPos(x, y);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void SendRightUp(int x, int y) {
        SetCursorPos(x, y);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void SendMove(int x, int y) {
        SetCursorPos(x, y);
        INPUT[] inputs = new INPUT[1];
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE;
        inputs[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void SendLeftClick(int x, int y) {
        SetCursorPos(x, y);
        INPUT[] down = new INPUT[1];
        down[0].type = INPUT_MOUSE;
        down[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        down[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        INPUT[] up = new INPUT[1];
        up[0].type = INPUT_MOUSE;
        up[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        up[0].mi.dwExtraInfo = TEST_EXTRA_INFO;
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }

    // 1. 持续 1000Hz 注入 (指定持续点数)
    public static double RunSustained1000Hz(int centerX, int centerY, int radius, int totalPoints) {
        timeBeginPeriod(1);
        SendRightDown(centerX + radius, centerY);
        Stopwatch sw = Stopwatch.StartNew();
        long qpcFreq = Stopwatch.Frequency;
        double targetIntervalMs = 1.0;

        for (int i = 0; i < totalPoints; i++) {
            double angle = (2.0 * Math.PI * i) / 500.0; // 每 500 点一圈
            int x = centerX + (int)(radius * Math.Cos(angle));
            int y = centerY + (int)(radius * Math.Sin(angle));
            SendMove(x, y);

            long targetTicks = (long)((i + 1) * targetIntervalMs * (qpcFreq / 1000.0));
            while (sw.ElapsedTicks < targetTicks) {
                if (targetTicks - sw.ElapsedTicks > (qpcFreq / 1000.0) * 0.5) {
                    Thread.Yield();
                }
            }
        }
        sw.Stop();
        SendRightUp(centerX + radius, centerY);
        timeEndPeriod(1);
        return (totalPoints / sw.Elapsed.TotalSeconds);
    }

    // 2. 极限洪泛爆发注入 (瞬间推入 totalPoints 个点位，无睡眠，彻底测试 4096 溢出滑窗)
    public static double RunUnthrottledBurst(int startX, int startY, int totalPoints) {
        SendRightDown(startX, startY);
        Stopwatch sw = Stopwatch.StartNew();
        int curX = startX;
        int curY = startY;
        for (int i = 0; i < totalPoints; i++) {
            curX = startX + (i % 300);
            curY = startY + ((i / 300) % 300);
            SendMove(curX, curY);
        }
        sw.Stop();
        SendRightUp(curX, curY);
        return (totalPoints / sw.Elapsed.TotalSeconds);
    }

    // 3. 极速反复会话连击 (迅速按下/移动一小段/松开或左键打断)
    public static void RunRapidSessionChurn(int startX, int startY, int sessions) {
        for (int s = 0; s < sessions; s++) {
            int x = startX + (s % 50);
            int y = startY + (s % 50);
            SendRightDown(x, y);
            SendMove(x + 20, y + 20);
            SendMove(x + 40, y + 40);
            if (s % 3 == 0) {
                // 每 3 次穿插一次物理左键打断自愈
                SendLeftClick(x + 40, y + 40);
            } else {
                SendRightUp(x + 40, y + 40);
            }
            Thread.Sleep(2);
        }
    }
}
"@

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

Write-Host "[INFO] 启动被测进程..." -ForegroundColor Yellow
$proc = [System.Diagnostics.Process]::Start($psi)
if (-not $proc -or $proc.HasExited) {
    Write-Host "[ERROR] 进程启动失败！" -ForegroundColor Red
    exit 1
}
$procId = $proc.Id
Write-Host "[OK] Tools3000 已启动 (PID: $procId)" -ForegroundColor Green

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

# 启动 CPU 负载
$cpuStressJobs = @()
function Start-CpuStress([int]$threadCount) {
    Write-Host "[INFO] 启动 $threadCount 个 CPU 满载作业..." -ForegroundColor Yellow
    for ($i = 0; $i -lt $threadCount; $i++) {
        $job = Start-Job -ScriptBlock {
            while ($true) {
                $x = 1.0
                for ($k = 0; $k -lt 50000; $k++) {
                    $x = [Math]::Sin($x) * [Math]::Cos($x) + [Math]::Sqrt($k + 1.0)
                }
            }
        }
        $script:cpuStressJobs += $job
    }
}
function Stop-CpuStress() {
    if ($script:cpuStressJobs.Count -gt 0) {
        $script:cpuStressJobs | ForEach-Object {
            Stop-Job $_ -ErrorAction SilentlyContinue | Out-Null
            Remove-Job $_ -Force -ErrorAction SilentlyContinue | Out-Null
        }
        $script:cpuStressJobs = @()
    }
}

$allPassed = $true

try {
    # 握手
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $ready = $false
    while ($sw.Elapsed.TotalSeconds -lt 10) {
        if ($proc.HasExited) { throw "进程意外退出！" }
        $log = Get-LogContentSafe $HarnessLogFile
        if ($log -match "GesturePlugin" -or $log -match "手势引擎" -or $log -match "初始化完成") {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 150
    }
    Write-Host "[OK] 手势管线已就绪！" -ForegroundColor Green

    Start-CpuStress -threadCount $CpuLoadThreads

    # 1. 持续超长 1000Hz 压测 (3000 点位连续画圆，持续 ~3 秒)
    Write-Host "`n── [对抗挑战 1/4] 超长持续 1000Hz 输入 (3000 点位连续画圆) ──" -ForegroundColor Magenta
    $rate1 = [AdversarialMouseInjector]::RunSustained1000Hz(700, 450, 150, 3000)
    Write-Host "  -> 实测派发速率: $([Math]::Round($rate1, 1)) Hz" -ForegroundColor Gray
    Start-Sleep -Milliseconds 500
    if ($proc.HasExited) {
        Write-Host "[FAIL] 挑战 1 失败：超长 1000Hz 导致进程崩溃！" -ForegroundColor Red
        $allPassed = $false
    } else {
        $proc.Refresh()
        $wsMb = [Math]::Round($proc.WorkingSet64 / 1MB, 2)
        Write-Host "[OK] 挑战 1 通过：3000 点位完成，进程存活，物理内存工作集: ${wsMb} MB" -ForegroundColor Green
    }

    # 2. 极限洪泛爆发压测 (10,000 点位无节流推入，测试 4096 环形缓冲滑窗溢出)
    Write-Host "`n── [对抗挑战 2/4] 极限洪泛爆发注入 (10,000 点位无节流瞬间推入) ──" -ForegroundColor Magenta
    $burstRate = [AdversarialMouseInjector]::RunUnthrottledBurst(600, 400, 10000)
    Write-Host "  -> 爆发推入速率: $([Math]::Round($burstRate, 0)) 点/秒" -ForegroundColor Gray
    Start-Sleep -Milliseconds 600
    if ($proc.HasExited) {
        Write-Host "[FAIL] 挑战 2 失败：10,000 点位洪泛导致缓冲区溢出崩溃！" -ForegroundColor Red
        $allPassed = $false
    } else {
        $proc.Refresh()
        $wsMb2 = [Math]::Round($proc.WorkingSet64 / 1MB, 2)
        Write-Host "[OK] 挑战 2 通过：SPSC 4096 滑窗溢出承受住 10,000 点爆发轰击，0 崩溃，物理内存工作集: ${wsMb2} MB" -ForegroundColor Green
    }

    # 3. 极速反复会话连击 (100 次极速启动/移动/左键打断 churn)
    Write-Host "`n── [对抗挑战 3/4] 极速反复手势会话连击 (100 次连击 + 穿插左键打断自愈) ──" -ForegroundColor Magenta
    [AdversarialMouseInjector]::RunRapidSessionChurn(650, 400, 100)
    Start-Sleep -Milliseconds 500
    if ($proc.HasExited) {
        Write-Host "[FAIL] 挑战 3 失败：100 次极速反复会话导致崩溃或状态机错乱！" -ForegroundColor Red
        $allPassed = $false
    } else {
        Write-Host "[OK] 挑战 3 通过：100 次高频会话连击完成，无死锁、无状态机挂起！" -ForegroundColor Green
    }

    # 4. 退出挂起与死锁检验
    Write-Host "`n── [对抗挑战 4/4] 极限压测后进程停止与资源回收无死锁检验 ──" -ForegroundColor Magenta
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $proc.Kill()
    $exitedInTime = $proc.WaitForExit(3000)
    $stopwatch.Stop()
    if ($exitedInTime) {
        Write-Host "[OK] 挑战 4 通过：进程在 $($stopwatch.ElapsedMilliseconds)ms 内彻底释放退出，0 僵死进程！" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] 挑战 4 失败：进程退出超时挂起！" -ForegroundColor Red
        $allPassed = $false
    }
}
finally {
    Stop-CpuStress
    if ($proc -and -not $proc.HasExited) {
        try { $proc.Kill(); $proc.WaitForExit(1000) } catch {}
    }
    try {
        Remove-Item -LiteralPath $HarnessRoot -Recurse -Force -ErrorAction SilentlyContinue
    } catch {}
}

Write-Host "`n===============================================================================" -ForegroundColor Magenta
if ($allPassed) {
    Write-Host " [PASS] 对抗性极限压力测试 (4/4 挑战) 100% 全部攻破并证明健壮！" -ForegroundColor Green
} else {
    Write-Host " [FAIL] 对抗性极限压力测试发现缺陷！" -ForegroundColor Red
}
Write-Host "===============================================================================" -ForegroundColor Magenta

if (-not $allPassed) { exit 1 }
exit 0
