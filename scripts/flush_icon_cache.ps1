# scripts/flush_icon_cache.ps1 — Windows 系统与任务栏图标缓存深度冲刷工具
param(
    [switch]$RestartExplorer = $false
)

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host " [Tools3000] 正在执行 Windows Shell 图标缓存深度刷新..." -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

# 1. 注入快捷方式 AUMID 并通过原生 COM IPersistFile::Save 通知 Shell 属性存储
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ExeCandidates = @(
    "C:\Program Files\Tools3000\Tools3000.exe",
    "$RepoRoot\deploy_dist\Tools3000.exe",
    "$RepoRoot\build\Release\Tools3000.exe"
)
foreach ($exe in $ExeCandidates) {
    if (Test-Path $exe) {
        Start-Process -FilePath $exe -ArgumentList "--update-shortcuts" -Wait -WindowStyle Hidden -ErrorAction SilentlyContinue
        Write-Host "已通过 Tools3000 原生 COM 注入快捷方式 AUMID (Yy1.Tools3000): $exe" -ForegroundColor Green
        break
    }
}

# 3. 广播 SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST)
try {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class ShellIconFlusher {
    [DllImport("shell32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    public static extern void SHChangeNotify(uint wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);
}
"@ -ErrorAction SilentlyContinue
    [ShellIconFlusher]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    Write-Host "已向 Windows Shell 广播 SHCNE_ASSOCCHANGED 全局刷新通知。" -ForegroundColor Green
} catch {
    Write-Host "SHChangeNotify 广播失败: $_" -ForegroundColor Yellow
}

# 4. 可选：重启 Explorer 并清空物理 iconcache_*.db 缓存文件
if ($RestartExplorer) {
    Write-Host "正在重启 Windows Explorer 并重置图标数据库..." -ForegroundColor Cyan
    Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600

    $ExplorerCacheDir = "$env:LOCALAPPDATA\Microsoft\Windows\Explorer"
    if (Test-Path $ExplorerCacheDir) {
        Get-ChildItem -Path $ExplorerCacheDir -Filter "iconcache_*.db" -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
        Get-ChildItem -Path $ExplorerCacheDir -Filter "thumbcache_*.db" -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    }
    $OldIconCache = "$env:LOCALAPPDATA\IconCache.db"
    if (Test-Path $OldIconCache) {
        Remove-Item -Force $OldIconCache -ErrorAction SilentlyContinue
    }

    Start-Process explorer.exe
    Start-Sleep -Seconds 1
    Write-Host "Windows Explorer 已重新启动，图标缓存已彻底重置！" -ForegroundColor Green
} else {
    # 软刷新：调用 ie4uinit
    try {
        Start-Process ie4uinit.exe -ArgumentList "-show" -Wait -ErrorAction SilentlyContinue
    } catch { }
}

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host " 图标缓存刷新流程执行完毕！" -ForegroundColor Green
Write-Host "=======================================================" -ForegroundColor Cyan
