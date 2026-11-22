// ─────────────────────────────────────────────────────────────────────────────
// decode/mod.rs — 多态图像解码分发器
// 策略路由：GIF / WebP / APNG / SVG(Z) / QOI / TGA / PNM / Windows WIC
// ─────────────────────────────────────────────────────────────────────────────

pub mod apng_decoder;
pub mod extra_decoder;
pub mod gif_decoder;
pub mod svg_decoder;
pub mod webp_decoder;
pub mod wic_decoder;

use crate::constants::{MAX_DECODE_DIMENSION, MAX_DECODE_PIXELS};
use crate::types::{DecodeError, DecodedImage, ThumbnailBitmap};
use std::path::Path;

/// 统一校验单帧图像尺寸是否处于安全内存与算术上限内（防御畸形头触发 panic="abort" OOM）
pub fn validate_image_dimensions(
    w: u32,
    h: u32,
    format: &'static str,
) -> Result<usize, DecodeError> {
    if w == 0 || h == 0 {
        return Err(DecodeError::ZeroDimensions { format });
    }
    if w > MAX_DECODE_DIMENSION || h > MAX_DECODE_DIMENSION {
        return Err(DecodeError::DimensionsTooLarge {
            width: w,
            height: h,
        });
    }
    let pixels_u64 = (w as u64)
        .checked_mul(h as u64)
        .ok_or(DecodeError::DimensionsTooLarge {
            width: w,
            height: h,
        })?;
    if pixels_u64 > MAX_DECODE_PIXELS {
        return Err(DecodeError::DimensionsTooLarge {
            width: w,
            height: h,
        });
    }
    usize::try_from(pixels_u64).map_err(|_| DecodeError::DimensionsTooLarge {
        width: w,
        height: h,
    })
}

/// 统一安全分配清零像素缓冲（使用 `Vec::try_reserve_exact` 严防 OOM Abort）
pub fn try_alloc_zeroed_buffer(byte_len: usize) -> Result<Vec<u8>, DecodeError> {
    try_alloc_filled_buffer(byte_len, 0)
}

/// 统一安全分配指定填充值缓冲（使用 `Vec::try_reserve_exact` 严防 OOM Abort）
pub fn try_alloc_filled_buffer(byte_len: usize, fill: u8) -> Result<Vec<u8>, DecodeError> {
    let mut buf = Vec::new();
    buf.try_reserve_exact(byte_len)
        .map_err(|_| DecodeError::OutOfMemory)?;
    buf.resize(byte_len, fill);
    Ok(buf)
}

/// 统一安全克隆大像素缓冲（用于 GIF/APNG `DisposalMethod::Previous` 备份）
pub fn try_clone_buffer(src: &[u8]) -> Result<Vec<u8>, DecodeError> {
    let mut buf = Vec::new();
    buf.try_reserve_exact(src.len())
        .map_err(|_| DecodeError::OutOfMemory)?;
    buf.extend_from_slice(src);
    Ok(buf)
}

/// 图像嗅探类型（优先依据文件头魔数，扩展名兜底）
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum DetectedFormat {
    Gif,
    WebP,
    Apng,
    Svg,
    Qoi,
    Tga,
    Pnm,
    WicNative,
}

fn sniff_format(path: &Path, header: &[u8]) -> DetectedFormat {
    if header.len() >= 6 && (&header[0..6] == b"GIF87a" || &header[0..6] == b"GIF89a") {
        return DetectedFormat::Gif;
    }
    if header.len() >= 12 && &header[0..4] == b"RIFF" && &header[8..12] == b"WEBP" {
        return DetectedFormat::WebP;
    }
    if header.len() >= 4 && &header[0..4] == b"qoif" {
        return DetectedFormat::Qoi;
    }
    if header.len() >= 8 && &header[0..8] == [137, 80, 78, 71, 13, 10, 26, 10] {
        if apng_decoder::is_animated_png_file(path) {
            return DetectedFormat::Apng;
        }
        return DetectedFormat::WicNative;
    }
    if header.len() >= 3
        && header[0] == b'P'
        && matches!(header[1], b'1'..=b'6')
        && header[2].is_ascii_whitespace()
    {
        return DetectedFormat::Pnm;
    }

    let ext = path
        .extension()
        .and_then(|e| e.to_str())
        .map(|s| s.to_ascii_lowercase())
        .unwrap_or_default();
    match ext.as_str() {
        "svg" | "svgz" => DetectedFormat::Svg,
        "gif" => DetectedFormat::Gif,
        "webp" => DetectedFormat::WebP,
        "apng" => DetectedFormat::Apng,
        "qoi" => DetectedFormat::Qoi,
        "tga" => DetectedFormat::Tga,
        "ppm" | "pgm" | "pbm" => DetectedFormat::Pnm,
        _ => DetectedFormat::WicNative,
    }
}

/// 解码完整图像（自动按魔数分派最优内存安全解码器；失败自动交叉兜底）
pub fn decode_image_file(path: &Path) -> Result<DecodedImage, DecodeError> {
    let meta = std::fs::metadata(path).map_err(|e| DecodeError::ReadMetadata(e.to_string()))?;
    let file_size = meta.len();

    let mut header = [0u8; 32];
    let head_len = {
        use std::io::Read;
        let mut f = std::fs::File::open(path).map_err(|e| DecodeError::OpenFile(e.to_string()))?;
        f.read(&mut header).unwrap_or(0)
    };

    let format = sniff_format(path, &header[..head_len]);
    match format {
        DetectedFormat::Gif => gif_decoder::decode_gif(path, file_size)
            .or_else(|e| wic_decoder::decode_via_wic(path, file_size).map_err(|_| e)),
        DetectedFormat::WebP => webp_decoder::decode_webp(path, file_size)
            .or_else(|e| wic_decoder::decode_via_wic(path, file_size).map_err(|_| e)),
        DetectedFormat::Apng => apng_decoder::decode_apng(path, file_size)
            .or_else(|e| wic_decoder::decode_via_wic(path, file_size).map_err(|_| e)),
        DetectedFormat::Svg => svg_decoder::decode_svg(path, file_size),
        DetectedFormat::Qoi => extra_decoder::decode_qoi(path, file_size),
        DetectedFormat::Tga => extra_decoder::decode_tga(path, file_size),
        DetectedFormat::Pnm => extra_decoder::decode_pnm(path, file_size),
        DetectedFormat::WicNative => wic_decoder::decode_via_wic(path, file_size),
    }
}

/// 解码轻量级胶片缩略图（静态图走 WIC 原生硬件缩放，极速省内存）
pub fn decode_thumbnail(path: &Path, max_side: u32) -> Option<ThumbnailBitmap> {
    let mut header = [0u8; 16];
    let head_len = {
        use std::io::Read;
        let mut f = std::fs::File::open(path).ok()?;
        f.read(&mut header).unwrap_or(0)
    };
    let format = sniff_format(path, &header[..head_len]);
    match format {
        DetectedFormat::Svg => svg_decoder::decode_svg_thumbnail(path, max_side).ok(),
        DetectedFormat::WebP => wic_decoder::decode_thumbnail_via_wic(path, max_side)
            .or_else(|_| webp_decoder::decode_webp_first_frame_thumb(path, max_side))
            .ok(),
        DetectedFormat::Gif => wic_decoder::decode_thumbnail_via_wic(path, max_side)
            .or_else(|_| gif_decoder::decode_gif_first_frame_thumb(path, max_side))
            .ok(),
        DetectedFormat::Qoi | DetectedFormat::Tga | DetectedFormat::Pnm => {
            let img = decode_image_file(path).ok()?;
            let first = img.frames.first()?;
            Some(downscale_bgra_thumbnail(
                &first.bgra_premul,
                first.width,
                first.height,
                max_side,
            ))
        }
        DetectedFormat::Apng | DetectedFormat::WicNative => {
            wic_decoder::decode_thumbnail_via_wic(path, max_side).ok()
        }
    }
}

/// 高质量盒式降采样（安全防 OOM，供超长截图 GPU 纹理边长适配与缩略图生成共用）
pub fn downscale_bgra_box(
    src_bgra: &[u8],
    src_w: u32,
    src_h: u32,
    max_side: u32,
) -> Result<(u32, u32, Vec<u8>), DecodeError> {
    if src_w == 0 || src_h == 0 || max_side == 0 {
        return Err(DecodeError::ZeroDimensions { format: "BGRA" });
    }
    let expected_src = (src_w as usize)
        .checked_mul(src_h as usize)
        .and_then(|n| n.checked_mul(4))
        .ok_or(DecodeError::OutOfMemory)?;
    if src_bgra.len() < expected_src {
        return Err(DecodeError::TruncatedData { format: "BGRA" });
    }

    let long_side = src_w.max(src_h);
    if long_side <= max_side {
        let copy = try_clone_buffer(&src_bgra[..expected_src])?;
        return Ok((src_w, src_h, copy));
    }

    let scale = max_side as f64 / long_side as f64;
    let dst_w = ((src_w as f64 * scale).floor() as u32).clamp(1, max_side);
    let dst_h = ((src_h as f64 * scale).floor() as u32).clamp(1, max_side);
    let dst_len = (dst_w as usize)
        .checked_mul(dst_h as usize)
        .and_then(|n| n.checked_mul(4))
        .ok_or(DecodeError::OutOfMemory)?;
    let mut dst = try_alloc_zeroed_buffer(dst_len)?;

    let src_w_usize = src_w as usize;
    let dst_w_usize = dst_w as usize;
    for dy in 0..dst_h {
        let sy0 = ((dy as u64 * src_h as u64) / dst_h as u64) as usize;
        let sy1 = (((dy + 1) as u64 * src_h as u64 + dst_h as u64 - 1) / dst_h as u64)
            .min(src_h as u64)
            .max((sy0 + 1) as u64) as usize;
        let dst_row = dy as usize * dst_w_usize;
        for dx in 0..dst_w {
            let sx0 = ((dx as u64 * src_w as u64) / dst_w as u64) as usize;
            let sx1 = (((dx + 1) as u64 * src_w as u64 + dst_w as u64 - 1) / dst_w as u64)
                .min(src_w as u64)
                .max((sx0 + 1) as u64) as usize;

            let mut sum_b = 0u64;
            let mut sum_g = 0u64;
            let mut sum_r = 0u64;
            let mut sum_a = 0u64;
            let mut cnt = 0u64;
            for sy in sy0..sy1 {
                let row = sy * src_w_usize;
                for sx in sx0..sx1 {
                    let idx = (row + sx) * 4;
                    sum_b += src_bgra[idx] as u64;
                    sum_g += src_bgra[idx + 1] as u64;
                    sum_r += src_bgra[idx + 2] as u64;
                    sum_a += src_bgra[idx + 3] as u64;
                    cnt += 1;
                }
            }
            let out_idx = (dst_row + dx as usize) * 4;
            if cnt > 0 {
                dst[out_idx] = ((sum_b + cnt / 2) / cnt) as u8;
                dst[out_idx + 1] = ((sum_g + cnt / 2) / cnt) as u8;
                dst[out_idx + 2] = ((sum_r + cnt / 2) / cnt) as u8;
                dst[out_idx + 3] = ((sum_a + cnt / 2) / cnt) as u8;
            }
        }
    }

    Ok((dst_w, dst_h, dst))
}

/// 高质量双线性盒式缩放生成预乘 BGRA8 缩略图
pub fn downscale_bgra_thumbnail(
    src_bgra: &[u8],
    src_w: u32,
    src_h: u32,
    max_side: u32,
) -> ThumbnailBitmap {
    match downscale_bgra_box(src_bgra, src_w, src_h, max_side) {
        Ok((dst_w, dst_h, dst)) => ThumbnailBitmap {
            width: dst_w,
            height: dst_h,
            orig_width: src_w,
            orig_height: src_h,
            bgra_premul: dst,
        },
        Err(_) => ThumbnailBitmap {
            width: 1,
            height: 1,
            orig_width: src_w.max(1),
            orig_height: src_h.max(1),
            bgra_premul: vec![0, 0, 0, 0],
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dimension_validator_blocks_zero_and_oversized_images() {
        assert!(matches!(
            validate_image_dimensions(0, 100, "PNG"),
            Err(DecodeError::ZeroDimensions { .. })
        ));
        assert!(matches!(
            validate_image_dimensions(200_000, 10, "PNG"),
            Err(DecodeError::DimensionsTooLarge { .. })
        ));
        assert!(matches!(
            validate_image_dimensions(20_000, 20_000, "PNG"),
            Err(DecodeError::DimensionsTooLarge { .. })
        ));
        // 合法超长截图 (1080 x 30000 = 3240 万像素 < 1.34 亿上限) 应放行
        assert_eq!(validate_image_dimensions(1080, 30_000, "PNG"), Ok(32_400_000));
    }

    #[test]
    fn downscale_bgra_box_safely_clamps_tall_screenshot_to_gpu_limit() {
        let src_w = 16u32;
        let src_h = 200u32;
        let mut src = vec![0u8; (src_w * src_h * 4) as usize];
        for px in src.chunks_exact_mut(4) {
            px.copy_from_slice(&[20, 100, 220, 255]);
        }
        let (dw, dh, out) = downscale_bgra_box(&src, src_w, src_h, 64).expect("downscale");
        assert!(dw <= 64 && dh <= 64);
        assert_eq!(dh, 64);
        assert_eq!(out.len(), (dw * dh * 4) as usize);
        assert_eq!(&out[0..4], &[20, 100, 220, 255]);
    }
}
