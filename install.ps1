<#
.SYNOPSIS
Tools3000 官方 CLI 安装管理工具 (Official CLI Installer & Lifecycle Manager)

.DESCRIPTION
支持一键静默安装、自定义路径安装、开机自启配置、便携版运行与静默卸载。

.EXAMPLE
.\install.ps1                          # 默认极速静默安装并自动启动
.\install.ps1 -Silent                  # 静默安装
.\install.ps1 -Dir "D:\Apps\Tools3000" # 安装到指定目录
.\install.ps1 -Uninstall               # 静默卸载
.\install.ps1 -Uninstall -KeepPersonalData # 静默卸载并保留个人数据
.\install.ps1 -Portable                # 直接以绿色便携版运行
.\install.ps1 -Rebuild                 # 重新编译后立即静默安装并启动
#>

param (
    [switch]$Silent = $true,              # 静默安装模式 (默认开启)
    [switch]$VerySilent = $true,          # 完全无感静默安装 (无弹窗打扰)
    [switch]$Launch = $true,              # 安装完成后自动启动应用 (默认开启)
    [string]$Dir = "",                    # 自定义安装路径 (留空默认: C:\Program Files\Tools3000)
    [switch]$DesktopIcon = $false,        # 创建桌面图标
    [switch]$AutoStart = $false,          # 显式配置开机自启动
    [switch]$Uninstall = $false,          # 执行静默卸载流程
    [switch]$KeepPersonalData = $false,   # 卸载时保留全部个人数据
    [switch]$Portable = $false,           # 运行绿色便携版
    [switch]$Rebuild = $false,            # 先执行增量编译打包再安装
    [switch]$FlushIconCache = $false      # 深度刷新 Windows Explorer 图标缓存并重启资源管理器
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $ScriptDir

$TraceID = [guid]::NewGuid().ToString("N").Substring(0, 8)
$SetupExe = Join-Path $ScriptDir "Output\Tools3000-Setup.exe"
if (-not (Test-Path $SetupExe) -and (Test-Path (Join-Path $ScriptDir "Output\Tools3000-Setup.exe"))) {
    $SetupExe = Join-Path $ScriptDir "Output\Tools3000-Setup.exe"
}
$DeployDistExe = Join-Path $ScriptDir "deploy_dist\Tools3000.exe"

function Write-CliLog ($Message, $Level = "INFO") {
    $TimeStamp = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    $Color = switch ($Level) {
        "INFO" { "Cyan" }
        "WARN" { "Yellow" }
        "ERROR" { "Red" }
        "SUCCESS" { "Green" }
        default { "White" }
    }
    Write-Host "[$TimeStamp] [$TraceID] [$Level] $Message" -ForegroundColor $Color
}

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   Tools3000 Official CLI Installer & Manager (2026)   " -ForegroundColor Cyan
Write-Host "   Copyright (c) 2026 Yy1 (@yuan278501381)             " -ForegroundColor DarkGray
Write-Host "=======================================================" -ForegroundColor Cyan

# 1. 便携版模式
if ($Portable) {
    if (-not (Test-Path $DeployDistExe)) {
        Write-CliLog "便携版未就绪，正在快速构建..." "WARN"
        & pwsh.exe -File (Join-Path $ScriptDir "deploy.ps1") -Quick -SkipInstaller
    }
    Write-CliLog "正在以绿色便携版启动 Tools3000..." "INFO"
    Start-Process -FilePath $DeployDistExe
    Write-CliLog "Tools3000 便携版已启动！" "SUCCESS"
    exit 0
}

# 2. 卸载模式
if ($Uninstall) {
    Write-CliLog "正在检索系统中的 Tools3000 安装实例..." "INFO"
    $ActiveProcs = Get-Process -Name "Tools3000", "Tools3000_Service" -ErrorAction SilentlyContinue
    if ($ActiveProcs) {
        Write-CliLog "检测到运行中的实例，正在极速终止进程树..." "INFO"
        taskkill.exe /F /T /IM Tools3000.exe 2>$null | Out-Null
        taskkill.exe /F /T /IM Tools3000_Service.exe 2>$null | Out-Null
    }
    $svc = Get-Service -Name "Tools3000_SearchService" -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
        Stop-Service -Name "Tools3000_SearchService" -Force -NoWait -ErrorAction SilentlyContinue
    }
    $UninstallKeys = @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Tools3000_is1",
        "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Tools3000_is1",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Tools3000_is1"
    )
    $UninstallerPath = ""
    foreach ($k in $UninstallKeys) {
        if (Test-Path $k) {
            $val = (Get-ItemProperty -Path $k -ErrorAction SilentlyContinue).UninstallString
            if ($val) {
                $UninstallerPath = $val.Trim('"')
                break
            }
        }
    }

    if (-not $UninstallerPath -or -not (Test-Path $UninstallerPath)) {
        $DefaultUninstaller = "C:\Program Files\Tools3000\unins000.exe"
        if (Test-Path $DefaultUninstaller) {
            $UninstallerPath = $DefaultUninstaller
        }
    }

    if ($UninstallerPath -and (Test-Path $UninstallerPath)) {
        Write-CliLog "发现卸载程序: $UninstallerPath" "INFO"
        Write-CliLog "正在执行静默卸载并清理后台服务..." "INFO"
        $UninstArgs = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"
        if ($KeepPersonalData) {
            $UninstArgs += " /KEEPPERSONALDATA"
        }
        $proc = Start-Process -FilePath $UninstallerPath -ArgumentList $UninstArgs -Wait -PassThru
        if ($proc.ExitCode -eq 0) {
            Write-CliLog "Tools3000 已成功卸载并清理完毕！" "SUCCESS"
            $InstallDir = Split-Path -Parent $UninstallerPath
            if ($InstallDir -and (Test-Path -LiteralPath $InstallDir)) {
                Remove-Item -LiteralPath $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        } else {
            Write-CliLog "卸载退出代码: $($proc.ExitCode)" "WARN"
        }
    } else {
        Write-CliLog "未在系统中检测到 Tools3000 安装程序，执行绿色/便携与残留深度清理..." "WARN"
        $svc = Get-Service -Name "Tools3000_SearchService" -ErrorAction SilentlyContinue
        if ($svc) {
            sc.exe delete Tools3000_SearchService 2>$null | Out-Null
        }
        try {
            $ts = New-Object -ComObject "Schedule.Service"
            $ts.Connect()
            $folder = $ts.GetFolder("\Tools3000")
            $tasks = $folder.GetTasks(0)
            for ($i = $tasks.Count; $i -ge 1; $i--) {
                $folder.DeleteTask($tasks.Item($i).Name, 0)
            }
            $root = $ts.GetFolder("\")
            $root.DeleteFolder("Tools3000", 0)
        } catch {
            schtasks.exe /delete /tn "Tools3000\Autorun for $env:USERNAME" /f 2>$null | Out-Null
        }
        Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "Tools3000" -ErrorAction SilentlyContinue
    }
    if (-not $KeepPersonalData) {
        @(
            (Join-Path $env:LOCALAPPDATA "Tools3000"),
            (Join-Path $env:APPDATA "Tools3000"),
            (Join-Path $env:ProgramData "Tools3000")
        ) | ForEach-Object {
            if (Test-Path -LiteralPath $_) {
                Remove-Item -LiteralPath $_ -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
        Write-CliLog "Tools3000 个人数据已全部清理。" "SUCCESS"
    }
    exit 0
}

# 3. 重新构建请求
if ($Rebuild -or -not (Test-Path $SetupExe)) {
    Write-CliLog "安装包不存在或指定了 -Rebuild，开始执行增量编译与打包..." "INFO"
    & pwsh.exe -File (Join-Path $ScriptDir "deploy.ps1") -Quick
}

if (-not (Test-Path $SetupExe)) {
    Write-CliLog "未找到安装包文件: $SetupExe" "ERROR"
    exit 1
}

# 4. 执行 CLI 静默安装
Write-CliLog "正在执行 Tools3000 CLI 自动化安装..." "INFO"
$InstallArgs = @()
if ($VerySilent) {
    $InstallArgs += "/VERYSILENT"
    $InstallArgs += "/SUPPRESSMSGBOXES"
} elseif ($Silent) {
    $InstallArgs += "/SILENT"
    $InstallArgs += "/SUPPRESSMSGBOXES"
}

$InstallArgs += "/NORESTART"
$InstallArgs += "/CLOSEAPPLICATIONS"
$InstallArgs += "/FORCECLOSEAPPLICATIONS"

if ($Dir) {
    $InstallArgs += "/DIR=""$Dir"""
    Write-CliLog "自定义安装目录: $Dir" "INFO"
}

$tasks = @()
if ($DesktopIcon) {
    $tasks += "desktopicon"
}

$hasExistingAutoStart = (Get-ScheduledTask -TaskPath "\Tools3000\" -TaskName "Autorun for $env:USERNAME" -ErrorAction SilentlyContinue) -or
                        (Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "Tools3000" -ErrorAction SilentlyContinue)

if ($AutoStart -or ($hasExistingAutoStart -and $DesktopIcon)) {
    $tasks += "autostart"
}

if ($tasks.Count -gt 0) {
    $InstallArgs += "/TASKS=""$($tasks -join ',')"""
}

$LogFilePath = Join-Path $ScriptDir "deploy_logs\install_cli_$TraceID.log"
$InstallArgs += "/LOG=""$LogFilePath"""

Write-CliLog "启动安装进程: $SetupExe" "INFO"
Write-CliLog "参数: $($InstallArgs -join ' ')" "INFO"

# 执行安装前预先安全终止正在运行的旧实例，杜绝安装包文件锁定
sc.exe stop Tools3000_SearchService 2>$null | Out-Null
taskkill /F /T /IM Tools3000.exe 2>$null | Out-Null
taskkill /F /T /IM Tools3000_Service.exe 2>$null | Out-Null
Start-Sleep -Milliseconds 400


# 执行安装
$pinfo = New-Object System.Diagnostics.ProcessStartInfo
$pinfo.FileName = $SetupExe
$pinfo.Arguments = $InstallArgs -join " "
$pinfo.Verb = "runas"
$pinfo.UseShellExecute = $true

try {
    $p = [System.Diagnostics.Process]::Start($pinfo)
    $p.WaitForExit()
    if ($p.ExitCode -eq 0) {
        Write-CliLog "=======================================================" "SUCCESS"
        Write-CliLog "Tools3000 CLI 安装成功！" "SUCCESS"
        Write-CliLog "安装日志: $LogFilePath" "INFO"

        # 强制通知 Windows Shell 刷新图标与关联缓存，确保桌面与任务栏即刻呈现纯透明立体 T 徽标
        try {
            Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class ShellIconCache {
    [DllImport("shell32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern void SHChangeNotify(uint wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);
}
"@ -ErrorAction SilentlyContinue
            [ShellIconCache]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
        } catch { }

        $Shortcuts = @(
            "$env:ALLUSERSPROFILE\Microsoft\Windows\Start Menu\Programs\Tools3000\Tools3000.lnk",
            "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Tools3000\Tools3000.lnk",
            "$env:USERPROFILE\Desktop\Tools3000.lnk",
            "$env:PUBLIC\Desktop\Tools3000.lnk"
        )
        foreach ($sc in $Shortcuts) {
            if (Test-Path $sc) { (Get-Item $sc).LastWriteTime = Get-Date }
        }

        if ($FlushIconCache) {
            $FlushScript = Join-Path $ScriptDir "scripts\flush_icon_cache.ps1"
            if (Test-Path $FlushScript) {
                Write-CliLog "正在执行图标缓存深度冲刷并重置资源管理器..." "INFO"
                & $FlushScript -RestartExplorer
            }
        }

        if ($Launch) {
            $TargetExe = if ($Dir) { Join-Path $Dir "Tools3000.exe" } else { "C:\Program Files\Tools3000\Tools3000.exe" }
            if (Test-Path $TargetExe) {
                Write-CliLog "正在启动应用: $TargetExe" "INFO"
                $launched = $false
                $currentUserName = $env:USERNAME
                $taskCandidates = @(
                    "\Tools3000\Autorun for $currentUserName"
                )

                # 优先通道：通过用户交互式计划任务穿透拉起，确保 100% 注入当前用户的 winsta0\default 物理桌面
                foreach ($taskName in $taskCandidates) {
                    try {
                        $null = schtasks /query /tn "$taskName" 2>&1
                        if ($LASTEXITCODE -eq 0) {
                            $null = schtasks /run /tn "$taskName" 2>&1
                            if ($LASTEXITCODE -eq 0) {
                                $launched = $true
                                Write-CliLog "已通过交互式计划任务 ($taskName) 穿透拉起应用至物理桌面！" "SUCCESS"
                                break
                            }
                        }
                    } catch {
                        # 计划任务不可用时安全回退
                    }
                }

                if (-not $launched) {
                    $appInfo = New-Object System.Diagnostics.ProcessStartInfo
                    $appInfo.FileName = $TargetExe
                    $appInfo.Verb = "runas"
                    $appInfo.UseShellExecute = $true
                    try {
                        [System.Diagnostics.Process]::Start($appInfo) | Out-Null
                    } catch {
                        Start-Process -FilePath $TargetExe
                    }
                    Write-CliLog "Tools3000 已启动并就绪！" "SUCCESS"
                }
            }
        }
        Write-Host "=======================================================" -ForegroundColor Green
    } else {
        Write-CliLog "安装程序退出，错误码: $($p.ExitCode)" "ERROR"
        exit $p.ExitCode
    }
} catch {
    Write-CliLog "安装执行失败: $_" "ERROR"
    exit 1
}
