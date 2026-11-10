<#
.SYNOPSIS
Tools3000 官方 CLI 静默卸载工具 (Official CLI Uninstaller)

.DESCRIPTION
检索系统注册表与标准安装路径，自动优雅停止运行实例，并以 /VERYSILENT 模式彻底清理安装目录与服务。

.EXAMPLE
.\uninstall.ps1
.\uninstall.cmd
.\uninstall.ps1 -KeepPersonalData
#>

param (
    [switch]$KeepPersonalData = $false,   # 保留配置、日志、转储、索引、历史、截图和录屏
    [switch]$Force = $false               # 强制杀死残留进程
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $ScriptDir

$TraceID = [guid]::NewGuid().ToString("N").Substring(0, 8)

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
Write-Host "   Tools3000 Official CLI Uninstaller (2026)           " -ForegroundColor Cyan
Write-Host "   Copyright (c) 2026 Yy1 (@yuan278501381)             " -ForegroundColor DarkGray
Write-Host "=======================================================" -ForegroundColor Cyan

# 1. 毫秒级极速停止运行中的 Tools3000 与服务进程树
Write-CliLog "正在检测运行中的 Tools3000 进程..." "INFO"
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

# 2. 检索卸载程序路径
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
    Write-CliLog "定位到卸载程序: $UninstallerPath" "INFO"
    Write-CliLog "正在执行一键静默无感卸载..." "INFO"
    
    $pinfo = New-Object System.Diagnostics.ProcessStartInfo
    $pinfo.FileName = $UninstallerPath
    $pinfo.Arguments = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"
    if ($KeepPersonalData) {
        $pinfo.Arguments += " /KEEPPERSONALDATA"
    }
    $pinfo.Verb = "runas"
    $pinfo.UseShellExecute = $true

    try {
        $p = [System.Diagnostics.Process]::Start($pinfo)
        $p.WaitForExit()
        if ($p.ExitCode -eq 0) {
            Write-CliLog "Tools3000 主程序与系统服务已成功卸载！" "SUCCESS"
            $InstallDir = Split-Path -Parent $UninstallerPath
            if ($InstallDir -and (Test-Path -LiteralPath $InstallDir)) {
                Remove-Item -LiteralPath $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        } else {
            Write-CliLog "卸载退出代码: $($p.ExitCode)" "WARN"
        }
    } catch {
        Write-CliLog "卸载调用失败: $_" "ERROR"
        exit 1
    }
} else {
    Write-CliLog "未在注册表或默认路径检测到 Tools3000 安装包记录，执行绿色/便携与残留深度清理..." "WARN"
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

# 3. 默认清理全部个人数据；可用 -KeepPersonalData 明确保留
if (-not $KeepPersonalData) {
    $PersonalDataDirs = @(
        (Join-Path $env:LOCALAPPDATA "Tools3000"),
        (Join-Path $env:APPDATA "Tools3000"),
        (Join-Path $env:ProgramData "Tools3000")
    )
    foreach ($dir in $PersonalDataDirs) {
        if (Test-Path -LiteralPath $dir) {
            Write-CliLog "正在清理个人数据: $dir" "INFO"
            Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    Write-CliLog "个人数据已清理完毕！" "SUCCESS"
}

Write-Host "=======================================================" -ForegroundColor Green
Write-Host "Tools3000 CLI 卸载流程执行完毕。" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Green
