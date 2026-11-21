# -----------------------------------------------------------------------------
# sync-delayed-dlls.ps1 — 递归解析并补齐 PE 常规导入与 /DELAYLOAD 延迟导入依赖
# -----------------------------------------------------------------------------
# 解决 vcpkg applocal 仅扫描常规导入表而漏拷贝 /DELAYLOAD 延迟加载 DLL 及其传递
# 依赖的问题。纯 .NET PE 二进制解析，零外部工具依赖，兼容 x64 与 ARM64 PE32+。
# -----------------------------------------------------------------------------
param(
    [Parameter(Mandatory = $true)]
    [string]$TargetDir,
    [Parameter(Mandatory = $true)]
    [string]$SearchDirs
)

$ErrorActionPreference = 'Stop'
$SearchDirList = @($SearchDirs -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })

if (-not (Test-Path -LiteralPath $TargetDir -PathType Container)) {
    throw "TargetDir 不存在: $TargetDir"
}

if (-not ('Tools3000PeImportInspector' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class Tools3000PeImportInspector {
    private struct Section {
        public uint VirtualAddress;
        public uint VirtualSize;
        public uint RawPtr;
        public uint RawSize;
    }

    private static int RvaToOffset(uint rva, List<Section> sections) {
        foreach (var s in sections) {
            uint span = Math.Max(s.VirtualSize, s.RawSize);
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + span) {
                return (int)(s.RawPtr + (rva - s.VirtualAddress));
            }
        }
        return -1;
    }

    private static string ReadCString(byte[] data, int offset) {
        if (offset < 0 || offset >= data.Length) return string.Empty;
        int end = offset;
        while (end < data.Length && data[end] != 0) end++;
        return Encoding.ASCII.GetString(data, offset, end - offset);
    }

    public static string[] GetAllImportedDlls(string pePath) {
        byte[] data = File.ReadAllBytes(pePath);
        if (data.Length < 0x100 || data[0] != 'M' || data[1] != 'Z') return Array.Empty<string>();
        int peOff = BitConverter.ToInt32(data, 0x3C);
        if (peOff <= 0 || peOff + 24 >= data.Length) return Array.Empty<string>();
        if (data[peOff] != 'P' || data[peOff + 1] != 'E') return Array.Empty<string>();

        ushort numSections = BitConverter.ToUInt16(data, peOff + 6);
        ushort optSize = BitConverter.ToUInt16(data, peOff + 20);
        int optOff = peOff + 24;
        ushort magic = BitConverter.ToUInt16(data, optOff);
        bool isPe32Plus = (magic == 0x20B);
        int ddOff = optOff + (isPe32Plus ? 112 : 96);
        int secOff = optOff + optSize;

        var sections = new List<Section>(numSections);
        for (int i = 0; i < numSections; i++) {
            int o = secOff + i * 40;
            if (o + 40 > data.Length) break;
            sections.Add(new Section {
                VirtualSize = BitConverter.ToUInt32(data, o + 8),
                VirtualAddress = BitConverter.ToUInt32(data, o + 12),
                RawSize = BitConverter.ToUInt32(data, o + 16),
                RawPtr = BitConverter.ToUInt32(data, o + 20)
            });
        }

        var result = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        // Directory[1]: Standard Import Table (20-byte descriptors)
        uint impRva = BitConverter.ToUInt32(data, ddOff + 1 * 8);
        if (impRva != 0) {
            int off = RvaToOffset(impRva, sections);
            while (off >= 0 && off + 20 <= data.Length) {
                uint nameRva = BitConverter.ToUInt32(data, off + 12);
                uint firstThunk = BitConverter.ToUInt32(data, off + 16);
                if (nameRva == 0 && firstThunk == 0) break;
                if (nameRva != 0) {
                    string name = ReadCString(data, RvaToOffset(nameRva, sections));
                    if (!string.IsNullOrEmpty(name)) result.Add(name);
                }
                off += 20;
            }
        }

        // Directory[13]: Delay Import Table (32-byte descriptors)
        uint delayRva = BitConverter.ToUInt32(data, ddOff + 13 * 8);
        if (delayRva != 0) {
            int off = RvaToOffset(delayRva, sections);
            while (off >= 0 && off + 32 <= data.Length) {
                uint nameRva = BitConverter.ToUInt32(data, off + 4);
                if (nameRva == 0) break;
                string name = ReadCString(data, RvaToOffset(nameRva, sections));
                if (!string.IsNullOrEmpty(name)) result.Add(name);
                off += 32;
            }
        }

        var list = new string[result.Count];
        result.CopyTo(list);
        return list;
    }
}
'@
}

$validSearchDirs = @($SearchDirList | Where-Object { -not [string]::IsNullOrWhiteSpace($_) -and (Test-Path -LiteralPath $_ -PathType Container) } | Select-Object -Unique)
$availableByName = @{}
foreach ($dir in $validSearchDirs) {
    foreach ($f in Get-ChildItem -LiteralPath $dir -Filter '*.dll' -File -ErrorAction SilentlyContinue) {
        $key = $f.Name.ToLowerInvariant()
        if (-not $availableByName.ContainsKey($key)) {
            $availableByName[$key] = $f.FullName
        }
    }
}

$queue = [System.Collections.Generic.Queue[string]]::new()
$visited = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)

foreach ($f in Get-ChildItem -LiteralPath $TargetDir -File -ErrorAction SilentlyContinue) {
    if ($f.Extension -ieq '.exe' -or $f.Extension -ieq '.dll') {
        if ($visited.Add($f.FullName)) { $queue.Enqueue($f.FullName) }
    }
}
$pluginsDir = Join-Path $TargetDir 'plugins'
if (Test-Path -LiteralPath $pluginsDir -PathType Container) {
    foreach ($f in Get-ChildItem -LiteralPath $pluginsDir -Filter 'Plugin_*.dll' -File -ErrorAction SilentlyContinue) {
        if ($visited.Add($f.FullName)) { $queue.Enqueue($f.FullName) }
    }
}

$copied = @()
while ($queue.Count -gt 0) {
    $current = $queue.Dequeue()
    $imports = [Tools3000PeImportInspector]::GetAllImportedDlls($current)
    foreach ($dllName in $imports) {
        $key = $dllName.ToLowerInvariant()
        if ($key.StartsWith('api-ms-win-') -or $key.StartsWith('ext-ms-win-')) { continue }
        $destPath = Join-Path $TargetDir $dllName
        if (Test-Path -LiteralPath $destPath -PathType Leaf) {
            if ($visited.Add($destPath)) { $queue.Enqueue($destPath) }
            continue
        }
        if ($availableByName.ContainsKey($key)) {
            Copy-Item -LiteralPath $availableByName[$key] -Destination $destPath -Force
            $copied += $dllName
            if ($visited.Add($destPath)) { $queue.Enqueue($destPath) }
        }
    }
}

if ($copied.Count -gt 0) {
    Write-Host ("[INFO] sync-delayed-dlls: 补齐 {0} 个延迟/传递依赖运行库: {1}" -f $copied.Count, (($copied | Sort-Object) -join ', '))
} else {
    Write-Host "[OK] sync-delayed-dlls: 所有常规与 /DELAYLOAD 依赖运行库已 100% 齐全。"
}
