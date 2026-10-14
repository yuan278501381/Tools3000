# ─────────────────────────────────────────────────────────────────────────────
# generate_icon.ps1 — 生成 Tools3000 全套 Windows 规范应用图标与纯白托盘图标
#
# 规范:
#   1. app.ico (用于任务栏、任务管理器、Alt+Tab、桌面快捷方式、安装包)
#      • 活力亲民的天空蓝到蓝紫渐变 Squircle 底座 (#4378F0 -> #7C3AED)
#      • 居中纯白极速破空平顶闪电 (带 3D 柔和微投影，大号饱满，极高辨识度)
#   2. tray.ico (用于 Windows 系统托盘区 Notification Area)
#      • 纯透明底色 + 纯白大号平顶极速闪电 (占满 94% 视区，线条饱满，与系统托盘图标完全协调)
#   3. 输出未压缩 32 位 DIB 多分辨率 (16, 20, 24, 32, 40, 48, 64, 128, 256)，100% 兼容 rc.exe / High-DPI
# ─────────────────────────────────────────────────────────────────────────────

Add-Type -AssemblyName System.Drawing

function Build-IcoFile {
    param (
        [string]$OutputPath,
        [scriptblock]$RenderCallback,
        [int[]]$Sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
    )

    $imagesData = @()
    $headerSize = 6 + ($Sizes.Length * 16)
    $currentOffset = $headerSize

    foreach ($sz in $Sizes) {
        $scale = 4
        $canvasSz = $sz * $scale
        $bmpHi = New-Object System.Drawing.Bitmap($canvasSz, $canvasSz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmpHi)
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.Clear([System.Drawing.Color]::Transparent)

        & $RenderCallback $g $canvasSz

        $g.Dispose()

        # 4x 超采样下采样回目标尺寸
        $finalBmp = New-Object System.Drawing.Bitmap($sz, $sz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $gFinal = [System.Drawing.Graphics]::FromImage($finalBmp)
        $gFinal.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $gFinal.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $gFinal.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $gFinal.DrawImage($bmpHi, (New-Object System.Drawing.Rectangle(0, 0, $sz, $sz)), 0, 0, $canvasSz, $canvasSz, [System.Drawing.GraphicsUnit]::Pixel)
        $gFinal.Dispose()
        $bmpHi.Dispose()

        # 提取 32 位 BGRA 像素
        $rect = New-Object System.Drawing.Rectangle(0, 0, $sz, $sz)
        $data = $finalBmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $stride = $data.Stride
        $pixels = New-Object byte[] ($stride * $sz)
        [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
        $finalBmp.UnlockBits($data)
        $finalBmp.Dispose()

        $andRowBytes = [int][Math]::Floor((($sz + 31) / 32)) * 4
        $xorBytes = $sz * $sz * 4
        $andBytes = $sz * $andRowBytes
        $imageBytes = 40 + $xorBytes + $andBytes

        # 序列化单张 DIB 数据块
        $msImg = New-Object System.IO.MemoryStream
        $bwImg = New-Object System.IO.BinaryWriter($msImg)

        # BITMAPINFOHEADER (biHeight = 2*sz)
        $bwImg.Write([uint32]40)
        $bwImg.Write([int32]$sz)
        $bwImg.Write([int32]($sz * 2))
        $bwImg.Write([uint16]1)
        $bwImg.Write([uint16]32)
        $bwImg.Write([uint32]0)
        $bwImg.Write([uint32]$xorBytes)
        $bwImg.Write([int32]0)
        $bwImg.Write([int32]0)
        $bwImg.Write([uint32]0)
        $bwImg.Write([uint32]0)

        # XOR 位图 (自底向上 BGRA)
        for ($y = $sz - 1; $y -ge 0; $y--) {
            $bwImg.Write($pixels, $y * $stride, $sz * 4)
        }

        # AND 掩码 (全 0 透明通道由 alpha 决定)
        $zeroRow = New-Object byte[] $andRowBytes
        for ($y = 0; $y -lt $sz; $y++) {
            $bwImg.Write($zeroRow, 0, $andRowBytes)
        }

        $bwImg.Flush()
        $dibBytes = $msImg.ToArray()
        $msImg.Dispose()

        $imagesData += [PSCustomObject]@{
            Size = $sz
            ImageBytes = $imageBytes
            Offset = $currentOffset
            Data = $dibBytes
        }

        $currentOffset += $imageBytes
    }

    # 组装完整 ICO 文件
    $msFile = New-Object System.IO.MemoryStream
    $bwFile = New-Object System.IO.BinaryWriter($msFile)

    # ICONDIR
    $bwFile.Write([uint16]0)
    $bwFile.Write([uint16]1)
    $bwFile.Write([uint16]$Sizes.Length)

    # ICONDIRENTRY[]
    foreach ($entry in $imagesData) {
        $wByte = if ($entry.Size -eq 256) { [byte]0 } else { [byte]$entry.Size }
        $hByte = if ($entry.Size -eq 256) { [byte]0 } else { [byte]$entry.Size }
        $bwFile.Write($wByte)
        $bwFile.Write($hByte)
        $bwFile.Write([byte]0) # ColorCount
        $bwFile.Write([byte]0) # Reserved
        $bwFile.Write([uint16]1) # Planes
        $bwFile.Write([uint16]32) # BitCount
        $bwFile.Write([uint32]$entry.ImageBytes)
        $bwFile.Write([uint32]$entry.Offset)
    }

    # DIB Blocks
    foreach ($entry in $imagesData) {
        $bwFile.Write($entry.Data, 0, $entry.Data.Length)
    }

    $bwFile.Flush()
    [System.IO.File]::WriteAllBytes($OutputPath, $msFile.ToArray())
    $msFile.Dispose()

    Write-Output "Generated: $OutputPath ($($Sizes.Length) sizes: $($Sizes -join ', ') px)"
}

# 绘制圆角矩形辅助函数
function Add-RoundedRectPath {
    param ($path, $rect, $radius)
    $diameter = $radius * 2
    $arc = New-Object System.Drawing.RectangleF($rect.X, $rect.Y, $diameter, $diameter)
    
    $path.AddArc($arc, 180, 90)
    $arc.X = $rect.Right - $diameter
    $path.AddArc($arc, 270, 90)
    $arc.Y = $rect.Bottom - $diameter
    $path.AddArc($arc, 0, 90)
    $arc.X = $rect.X
    $path.AddArc($arc, 90, 90)
    $path.CloseFigure()
}

# 绘制平顶极速破空闪电多边形
function Get-FlatSpeedLightningPoints {
    param ([float]$w, [float]$h, [float]$padX = 0, [float]$padY = 0)
    $bw = $w - ($padX * 2)
    $bh = $h - ($padY * 2)

    return @(
        (New-Object System.Drawing.PointF ($padX + $bw * 0.44), ($padY + $bh * 0.04)), # 顶平左
        (New-Object System.Drawing.PointF ($padX + $bw * 0.72), ($padY + $bh * 0.04)), # 顶平右
        (New-Object System.Drawing.PointF ($padX + $bw * 0.54), ($padY + $bh * 0.44)), # 右腰内折
        (New-Object System.Drawing.PointF ($padX + $bw * 0.82), ($padY + $bh * 0.44)), # 右侧突尖
        (New-Object System.Drawing.PointF ($padX + $bw * 0.33), ($padY + $bh * 1.00)), # 底部刺针尖
        (New-Object System.Drawing.PointF ($padX + $bw * 0.48), ($padY + $bh * 0.62)), # 左腰内折
        (New-Object System.Drawing.PointF ($padX + $bw * 0.20), ($padY + $bh * 0.62))  # 左侧突尖
    )
}

# ── 1. 生成 app.ico (天空蓝到蓝紫渐变底座 + 纯白大号平顶闪电) ────────
$appIcoPath = Join-Path $PSScriptRoot "app.ico"
Build-IcoFile -OutputPath $appIcoPath -RenderCallback {
    param ($g, $sz)

    $margin = [float]($sz * 0.035)
    $rect = New-Object System.Drawing.RectangleF($margin, $margin, ($sz - $margin * 2), ($sz - $margin * 2))
    $radius = [float]($sz * 0.22)

    $bgPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    Add-RoundedRectPath $bgPath $rect $radius

    # 活力亲民渐变 (#4378F0 -> #7C3AED)
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (
        (New-Object System.Drawing.PointF $rect.Left, $rect.Top),
        (New-Object System.Drawing.PointF $rect.Right, $rect.Bottom),
        [System.Drawing.Color]::FromArgb(255, 67, 120, 240),  # #4378F0
        [System.Drawing.Color]::FromArgb(255, 124, 58, 237)   # #7C3AED
    )
    $g.FillPath($bgBrush, $bgPath)
    $bgBrush.Dispose()

    # 细微高光内描边
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(60, 255, 255, 255), [float]($sz * 0.015))
    $g.DrawPath($pen, $bgPath)
    $pen.Dispose()
    $bgPath.Dispose()

    # 闪电在底座中占 84% 高度
    $padX = [float]($sz * 0.14)
    $padY = [float]($sz * 0.08)
    $bw = $sz - ($padX * 2)
    $bh = $sz - ($padY * 2)

    # 柔和微投影
    $shadowOffset = [float]($sz * 0.015)
    $shadowPts = @(
        (New-Object System.Drawing.PointF ($padX + $bw * 0.44), ($padY + $bh * 0.04 + $shadowOffset)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.72), ($padY + $bh * 0.04 + $shadowOffset)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.54), ($padY + $bh * 0.44 + $shadowOffset)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.82), ($padY + $bh * 0.44 + $shadowOffset)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.33), ($padY + $bh * 1.00 + $shadowOffset)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.48), ($padY + $bh * 0.62 + $shadowOffset)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.20), ($padY + $bh * 0.62 + $shadowOffset))
    )
    $sBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(45, 0, 0, 0))
    $g.FillPolygon($sBrush, $shadowPts)
    $sBrush.Dispose()

    # 纯白闪电本体
    $pts = Get-FlatSpeedLightningPoints $sz $sz $padX $padY
    $whiteBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $g.FillPolygon($whiteBrush, $pts)
    $whiteBrush.Dispose()
}

# ── 2. 生成 tray.ico (纯透明底色 + 纯白大号平顶闪电) ─────────────────
$trayIcoPath = Join-Path $PSScriptRoot "tray.ico"
Build-IcoFile -OutputPath $trayIcoPath -RenderCallback {
    param ($g, $sz)

    # 托盘专属：透明底，闪电占满 94% 区域，浑厚饱满，锋利清晰
    $padX = [float]($sz * 0.03)
    $padY = [float]($sz * 0.03)
    $pts = Get-FlatSpeedLightningPoints $sz $sz $padX $padY
    
    $whiteBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $g.FillPolygon($whiteBrush, $pts)
    $whiteBrush.Dispose()
}
