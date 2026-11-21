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

use crate::types::{DecodedImage, ThumbnailBitmap};
use std::path::Path;

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
pub fn decode_image_file(path: &Path) -> Result<DecodedImage, String> {
    let meta = std::fs::metadata(path).map_err(|e| format!("无法读取文件元数据: {e}"))?;
    let file_size = meta.len();

    let mut header = [0u8; 32];
    let head_len = {
        use std::io::Read;
        let mut f = std::fs::File::open(path).map_err(|e| format!("无法打开图片文件: {e}"))?;
        f.read(&mut header).unwrap_or(0)
    };

    let format = sniff_format(path, &header[..head_len]);
    match format {
        DetectedFormat::Gif => gif_decoder::decode_gif(path, file_size)
            .or_else(|_| wic_decoder::decode_via_wic(path, file_size)),
        DetectedFormat::WebP => webp_decoder::decode_webp(path, file_size)
            .or_else(|_| wic_decoder::decode_via_wic(path, file_size)),
        DetectedFormat::Apng => apng_decoder::decode_apng(path, file_size)
            .or_else(|_| wic_decoder::decode_via_wic(path, file_size)),
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

/// 高质量双线性盒式缩放生成预乘 BGRA8 缩略图
pub fn downscale_bgra_thumbnail(
    src_bgra: &[u8],
    src_w: u32,
    src_h: u32,
    max_side: u32,
) -> ThumbnailBitmap {
    if src_w == 0 || src_h == 0 || max_side == 0 {
        return ThumbnailBitmap {
            width: 1,
            height: 1,
            orig_width: 1,
            orig_height: 1,
            bgra_premul: vec![0, 0, 0, 0],
        };
    }
    let scale = (max_side as f32 / src_w.max(src_h) as f32).min(1.0);
    let dst_w = ((src_w as f32 * scale).round() as u32).max(1);
    let dst_h = ((src_h as f32 * scale).round() as u32).max(1);
    if dst_w == src_w && dst_h == src_h {
        return ThumbnailBitmap {
            width: src_w,
            height: src_h,
            orig_width: src_w,
            orig_height: src_h,
            bgra_premul: src_bgra.to_vec(),
        };
    }

    let mut dst = vec![0u8; (dst_w * dst_h * 4) as usize];
    for dy in 0..dst_h {
        let sy0 = (dy as u64 * src_h as u64 / dst_h as u64) as u32;
        let sy1 = (((dy + 1) as u64 * src_h as u64 + dst_h as u64 - 1) / dst_h as u64)
            .min(src_h as u64)
            .max((sy0 + 1) as u64) as u32;
        for dx in 0..dst_w {
            let sx0 = (dx as u64 * src_w as u64 / dst_w as u64) as u32;
            let sx1 = (((dx + 1) as u64 * src_w as u64 + dst_w as u64 - 1) / dst_w as u64)
                .min(src_w as u64)
                .max((sx0 + 1) as u64) as u32;

            let mut sum_b = 0u64;
            let mut sum_g = 0u64;
            let mut sum_r = 0u64;
            let mut sum_a = 0u64;
            let mut cnt = 0u64;
            for sy in sy0..sy1 {
                let row = (sy * src_w) as usize;
                for sx in sx0..sx1 {
                    let idx = (row + sx as usize) * 4;
                    sum_b += src_bgra[idx] as u64;
                    sum_g += src_bgra[idx + 1] as u64;
                    sum_r += src_bgra[idx + 2] as u64;
                    sum_a += src_bgra[idx + 3] as u64;
                    cnt += 1;
                }
            }
            let out_idx = ((dy * dst_w + dx) * 4) as usize;
            if cnt > 0 {
                dst[out_idx] = (sum_b / cnt) as u8;
                dst[out_idx + 1] = (sum_g / cnt) as u8;
                dst[out_idx + 2] = (sum_r / cnt) as u8;
                dst[out_idx + 3] = (sum_a / cnt) as u8;
            }
        }
    }

    ThumbnailBitmap {
        width: dst_w,
        height: dst_h,
        orig_width: src_w,
        orig_height: src_h,
        bgra_premul: dst,
    }
}
