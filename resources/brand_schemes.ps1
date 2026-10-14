param (
    [int]$ApplyScheme = 1
)

# ─────────────────────────────────────────────────────────────────────────────
# brand_schemes.ps1 — 统一切换 Tools3000 官方品牌资产 (ICO + UI SVG + Favicon)
# ─────────────────────────────────────────────────────────────────────────────

Add-Type -AssemblyName System.Drawing

function Build-IcoFileFromRenderer {
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
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.Clear([System.Drawing.Color]::Transparent)

        & $RenderCallback $g $canvasSz

        $g.Dispose()

        $finalBmp = New-Object System.Drawing.Bitmap($sz, $sz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $gFinal = [System.Drawing.Graphics]::FromImage($finalBmp)
        $gFinal.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $gFinal.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $gFinal.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $gFinal.DrawImage($bmpHi, (New-Object System.Drawing.Rectangle(0, 0, $sz, $sz)), 0, 0, $canvasSz, $canvasSz, [System.Drawing.GraphicsUnit]::Pixel)
        $gFinal.Dispose()
        $bmpHi.Dispose()

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

        $msImg = New-Object System.IO.MemoryStream
        $bwImg = New-Object System.IO.BinaryWriter($msImg)

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

        for ($y = $sz - 1; $y -ge 0; $y--) {
            $bwImg.Write($pixels, $y * $stride, $sz * 4)
        }

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

    $msFile = New-Object System.IO.MemoryStream
    $bwFile = New-Object System.IO.BinaryWriter($msFile)

    $bwFile.Write([uint16]0)
    $bwFile.Write([uint16]1)
    $bwFile.Write([uint16]$Sizes.Length)

    foreach ($entry in $imagesData) {
        $wByte = if ($entry.Size -eq 256) { [byte]0 } else { [byte]$entry.Size }
        $hByte = if ($entry.Size -eq 256) { [byte]0 } else { [byte]$entry.Size }
        $bwFile.Write($wByte)
        $bwFile.Write($hByte)
        $bwFile.Write([byte]0)
        $bwFile.Write([byte]0)
        $bwFile.Write([uint16]1)
        $bwFile.Write([uint16]32)
        $bwFile.Write([uint32]$entry.ImageBytes)
        $bwFile.Write([uint32]$entry.Offset)
    }

    foreach ($entry in $imagesData) {
        $bwFile.Write($entry.Data, 0, $entry.Data.Length)
    }

    $bwFile.Flush()
    [System.IO.File]::WriteAllBytes($OutputPath, $msFile.ToArray())
    $msFile.Dispose()

    Write-Output "ICO Generated: $OutputPath ($($Sizes.Length) sizes)"
}

function Add-SquircleRect {
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

# ─────────────────────────────────────────────────────────────────────────────
# 方案 1: 【超速滑翔光翼 "E" · Aero-Glide 'E'】
# ─────────────────────────────────────────────────────────────────────────────
function Get-AeroGlidePath {
    param ([float]$sz, [float]$padX = 0, [float]$padY = 0)
    $bw = $sz - ($padX * 2)
    $bh = $sz - ($padY * 2)

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath

    # 1. 顶层滑翔翼 (Top Wing)
    $pTop = New-Object System.Drawing.Drawing2D.GraphicsPath
    [System.Drawing.PointF[]]$topPts = @(
        (New-Object System.Drawing.PointF ($padX + $bw * 0.38), ($padY + $bh * 0.16)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.76), ($padY + $bh * 0.16)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.66), ($padY + $bh * 0.34)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.32), ($padY + $bh * 0.34))
    )
    $pTop.AddPolygon($topPts)

    # 2. 中层穿梭翼 (Middle Wing)
    $pMid = New-Object System.Drawing.Drawing2D.GraphicsPath
    [System.Drawing.PointF[]]$midPts = @(
        (New-Object System.Drawing.PointF ($padX + $bw * 0.30), ($padY + $bh * 0.44)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.70), ($padY + $bh * 0.44)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.62), ($padY + $bh * 0.60)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.24), ($padY + $bh * 0.60))
    )
    $pMid.AddPolygon($midPts)

    # 3. 底层滑行翼 (Bottom Wing)
    $pBot = New-Object System.Drawing.Drawing2D.GraphicsPath
    [System.Drawing.PointF[]]$botPts = @(
        (New-Object System.Drawing.PointF ($padX + $bw * 0.22), ($padY + $bh * 0.70)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.72), ($padY + $bh * 0.70)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.62), ($padY + $bh * 0.86)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.14), ($padY + $bh * 0.86))
    )
    $pBot.AddPolygon($botPts)

    # 4. 倾斜动能主脊梁 (Inclined Spine)
    $pSpine = New-Object System.Drawing.Drawing2D.GraphicsPath
    [System.Drawing.PointF[]]$spinePts = @(
        (New-Object System.Drawing.PointF ($padX + $bw * 0.38), ($padY + $bh * 0.16)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.50), ($padY + $bh * 0.16)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.26), ($padY + $bh * 0.86)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.14), ($padY + $bh * 0.86))
    )
    $pSpine.AddPolygon($spinePts)

    $path.AddPath($pTop, $false)
    $path.AddPath($pMid, $false)
    $path.AddPath($pBot, $false)
    $path.AddPath($pSpine, $false)
    return $path
}

function Render-Scheme1-App {
    param ($g, $sz)
    $margin = [float]($sz * 0.04)
    $rect = New-Object System.Drawing.RectangleF($margin, $margin, ($sz - $margin * 2), ($sz - $margin * 2))
    $radius = [float]($sz * 0.22)

    $bgPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    Add-SquircleRect $bgPath $rect $radius
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (
        (New-Object System.Drawing.PointF $rect.Left, $rect.Top),
        (New-Object System.Drawing.PointF $rect.Right, $rect.Bottom),
        [System.Drawing.Color]::FromArgb(255, 56, 189, 248),  # #38BDF8 天空蓝
        [System.Drawing.Color]::FromArgb(255, 99, 102, 241)   # #6366F1 极光紫
    )
    $g.FillPath($bgBrush, $bgPath)
    $bgBrush.Dispose()

    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(60, 255, 255, 255), [float]($sz * 0.015))
    $g.DrawPath($pen, $bgPath)
    $pen.Dispose()
    $bgPath.Dispose()

    $padX = [float]($sz * 0.14)
    $padY = [float]($sz * 0.10)
    $pE = Get-AeroGlidePath $sz $padX $padY

    $matrix = New-Object System.Drawing.Drawing2D.Matrix
    $matrix.Translate(0, [float]($sz * 0.015))
    $pShadow = $pE.Clone()
    $pShadow.Transform($matrix)
    $sBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(40, 0, 0, 0))
    $g.FillPath($sBrush, $pShadow)
    $sBrush.Dispose()
    $pShadow.Dispose()
    $matrix.Dispose()

    $wBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $g.FillPath($wBrush, $pE)
    $wBrush.Dispose()
    $pE.Dispose()
}

function Render-Scheme1-Tray {
    param ($g, $sz)
    $padX = [float]($sz * 0.03)
    $padY = [float]($sz * 0.03)
    $pE = Get-AeroGlidePath $sz $padX $padY
    $wBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $g.FillPath($wBrush, $pE)
    $wBrush.Dispose()
    $pE.Dispose()
}

# ─────────────────────────────────────────────────────────────────────────────
# 方案 4: 【莫比乌斯流体尾迹 "e" · Sonic Loop 'e'】
# ─────────────────────────────────────────────────────────────────────────────
function Get-SonicLoopPath {
    param ([float]$sz, [float]$padX = 0, [float]$padY = 0)
    $bw = $sz - ($padX * 2)
    $bh = $sz - ($padY * 2)

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath

    [System.Drawing.PointF[]]$pts = @(
        (New-Object System.Drawing.PointF ($padX + $bw * 0.50), ($padY + $bh * 0.12)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.18), ($padY + $bh * 0.32)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.14), ($padY + $bh * 0.56)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.28), ($padY + $bh * 0.84)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.54), ($padY + $bh * 0.88)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.90), ($padY + $bh * 0.30)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.74), ($padY + $bh * 0.65)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.50), ($padY + $bh * 0.72)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.32), ($padY + $bh * 0.58)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.78), ($padY + $bh * 0.44)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.74), ($padY + $bh * 0.26)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.50), ($padY + $bh * 0.12))
    )
    $path.AddClosedCurve($pts, 0.4)

    [System.Drawing.PointF[]]$holePts = @(
        (New-Object System.Drawing.PointF ($padX + $bw * 0.50), ($padY + $bh * 0.24)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.34), ($padY + $bh * 0.34)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.34), ($padY + $bh * 0.44)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.64), ($padY + $bh * 0.44)),
        (New-Object System.Drawing.PointF ($padX + $bw * 0.62), ($padY + $bh * 0.30))
    )
    $holePath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $holePath.AddClosedCurve($holePts, 0.3)
    $path.AddPath($holePath, $false)
    $holePath.Dispose()

    return $path
}

function Render-Scheme4-App {
    param ($g, $sz)
    $margin = [float]($sz * 0.04)
    $rect = New-Object System.Drawing.RectangleF($margin, $margin, ($sz - $margin * 2), ($sz - $margin * 2))
    $radius = [float]($sz * 0.22)

    $bgPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    Add-SquircleRect $bgPath $rect $radius
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (
        (New-Object System.Drawing.PointF $rect.Left, $rect.Top),
        (New-Object System.Drawing.PointF $rect.Right, $rect.Bottom),
        [System.Drawing.Color]::FromArgb(255, 56, 189, 248),  # #38BDF8 天空蓝
        [System.Drawing.Color]::FromArgb(255, 124, 58, 237)  # #7C3AED 极光紫
    )
    $g.FillPath($bgBrush, $bgPath)
    $bgBrush.Dispose()

    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(60, 255, 255, 255), [float]($sz * 0.015))
    $g.DrawPath($pen, $bgPath)
    $pen.Dispose()
    $bgPath.Dispose()

    $padX = [float]($sz * 0.12)
    $padY = [float]($sz * 0.10)
    $pe = Get-SonicLoopPath $sz $padX $padY

    $matrix = New-Object System.Drawing.Drawing2D.Matrix
    $matrix.Translate(0, [float]($sz * 0.015))
    $pShadow = $pe.Clone()
    $pShadow.Transform($matrix)
    $sBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(40, 0, 0, 0))
    $g.FillPath($sBrush, $pShadow)
    $sBrush.Dispose()
    $pShadow.Dispose()
    $matrix.Dispose()

    $wBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $g.FillPath($wBrush, $pe)
    $wBrush.Dispose()
    $pe.Dispose()
}

function Render-Scheme4-Tray {
    param ($g, $sz)
    $padX = [float]($sz * 0.02)
    $padY = [float]($sz * 0.02)
    $pe = Get-SonicLoopPath $sz $padX $padY
    $wBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $g.FillPath($wBrush, $pe)
    $wBrush.Dispose()
    $pe.Dispose()
}

# ─────────────────────────────────────────────────────────────────────────────
# 同步更新前端 UI 组件与 Favicon
# ─────────────────────────────────────────────────────────────────────────────
$repoRoot = Split-Path $PSScriptRoot -Parent
$boltComponentPath = Join-Path $repoRoot "ui\src\components\Tools3000Bolt.tsx"
$faviconPath = Join-Path $repoRoot "ui\public\favicon.svg"

if ($ApplyScheme -eq 1) {
    Write-Output ">> 正在装配 方案 1 (超速滑翔光翼 Aero-Glide E)..."
    Build-IcoFileFromRenderer (Join-Path $PSScriptRoot "app.ico") ${function:Render-Scheme1-App}
    Build-IcoFileFromRenderer (Join-Path $PSScriptRoot "tray.ico") ${function:Render-Scheme1-Tray}

    # 写入前端 React 组件 (方案 1 矢量)
    $scheme1SvgCode = @'
import React from 'react';

interface Tools3000BoltProps {
  size?: number;
  className?: string;
  fill?: string;
}

/**
 * 官方标准方案 1: 超速滑翔光翼 E (The Aero-Glide 'E')
 */
export const Tools3000Bolt: React.FC<Tools3000BoltProps> = ({
  size = 24,
  className = '',
  fill = 'var(--primary)',
}) => {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 100 100"
      fill="none"
      xmlns="http://www.w3.org/2000/svg"
      className={className}
      style={{ display: 'inline-block', verticalAlign: 'middle', flexShrink: 0 }}
      aria-hidden="true"
    >
      {/* 顶翼 */}
      <polygon points="38,16 76,16 66,34 32,34" fill={fill} />
      {/* 中翼 */}
      <polygon points="30,44 70,44 62,60 24,60" fill={fill} />
      {/* 底翼 */}
      <polygon points="22,70 72,70 62,86 14,86" fill={fill} />
      {/* 倾斜主脊柱 */}
      <polygon points="38,16 50,16 26,86 14,86" fill={fill} />
    </svg>
  );
};
'@
    [System.IO.File]::WriteAllText($boltComponentPath, $scheme1SvgCode, [System.Text.Encoding]::UTF8)

    # 写入 Favicon
    $scheme1Favicon = @'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" width="128" height="128">
  <defs>
    <linearGradient id="tools3000-grad" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="#38BDF8" />
      <stop offset="100%" stop-color="#6366F1" />
    </linearGradient>
  </defs>
  <rect x="4" y="4" width="120" height="120" rx="28" fill="url(#tools3000-grad)" stroke="rgba(255,255,255,0.25)" stroke-width="1.5" />
  <g fill="#FFFFFF">
    <polygon points="46,24 88,24 78,44 40,44" />
    <polygon points="38,54 82,54 74,72 32,72" />
    <polygon points="30,82 86,82 76,102 20,102" />
    <polygon points="46,24 60,24 34,102 20,102" />
  </g>
</svg>
'@
    [System.IO.File]::WriteAllText($faviconPath, $scheme1Favicon, [System.Text.Encoding]::UTF8)

} elseif ($ApplyScheme -eq 4) {
    Write-Output ">> 正在装配 方案 4 (莫比乌斯流体尾迹 Sonic Loop e)..."
    Build-IcoFileFromRenderer (Join-Path $PSScriptRoot "app.ico") ${function:Render-Scheme4-App}
    Build-IcoFileFromRenderer (Join-Path $PSScriptRoot "tray.ico") ${function:Render-Scheme4-Tray}

    # 写入前端 React 组件 (方案 4 矢量)
    $scheme4SvgCode = @'
import React from 'react';

interface Tools3000BoltProps {
  size?: number;
  className?: string;
  fill?: string;
}

/**
 * 官方标准方案 4: 莫比乌斯流体尾迹 e (The Sonic Loop 'e')
 */
export const Tools3000Bolt: React.FC<Tools3000BoltProps> = ({
  size = 24,
  className = '',
  fill = 'var(--primary)',
}) => {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 100 100"
      fill="none"
      xmlns="http://www.w3.org/2000/svg"
      className={className}
      style={{ display: 'inline-block', verticalAlign: 'middle', flexShrink: 0 }}
      aria-hidden="true"
    >
      <path
        fillRule="evenodd"
        clipRule="evenodd"
        d="M50,12 C30,12 16,30 16,55 C16,75 32,88 55,88 C68,88 78,80 88,32 C78,60 62,68 50,68 C36,68 28,58 28,45 L76,45 C78,45 78,35 72,26 C66,16 58,12 50,12 Z M32,36 C34,26 42,22 50,22 C58,22 64,26 64,36 L32,36 Z"
        fill={fill}
      />
    </svg>
  );
};
'@
    [System.IO.File]::WriteAllText($boltComponentPath, $scheme4SvgCode, [System.Text.Encoding]::UTF8)

    # 写入 Favicon
    $scheme4Favicon = @'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" width="128" height="128">
  <defs>
    <linearGradient id="tools3000-grad" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="#38BDF8" />
      <stop offset="100%" stop-color="#7C3AED" />
    </linearGradient>
  </defs>
  <rect x="4" y="4" width="120" height="120" rx="28" fill="url(#tools3000-grad)" stroke="rgba(255,255,255,0.25)" stroke-width="1.5" />
  <path
    fillRule="evenodd"
    clipRule="evenodd"
    d="M64,18 C40,18 24,38 24,68 C24,92 42,108 70,108 C86,108 98,98 110,40 C98,75 78,85 64,85 C46,85 36,73 36,57 L96,57 C98,57 98,45 90,34 C82,22 72,18 64,18 Z M42,46 C44,34 54,29 64,29 C74,29 82,34 82,46 L42,46 Z"
    fill="#FFFFFF"
  />
</svg>
'@
    [System.IO.File]::WriteAllText($faviconPath, $scheme4Favicon, [System.Text.Encoding]::UTF8)
}
