// ─────────────────────────────────────────────────────────────────────────────
// decode/webp_decoder.rs — 纯 Rust 静态 & 动态 WebP 完整解码器 (VP8 / VP8L / ANIM)
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{
    DEFAULT_FRAME_DELAY_MS, MAX_ANIM_FRAMES, MAX_ANIM_TOTAL_BYTES, MIN_FRAME_DELAY_MS,
};
use crate::decode::downscale_bgra_thumbnail;
use crate::types::{
    rgba_straight_to_bgra_premul, DecodedImage, ImageFrame, ThumbnailBitmap,
};
use image_webp::WebPDecoder;
use std::fs::File;
use std::io::BufReader;
use std::path::Path;

pub fn decode_webp(path: &Path, file_size: u64) -> Result<DecodedImage, String> {
    let file = File::open(path).map_err(|e| format!("打开 WebP 失败: {e}"))?;
    let mut decoder =
        WebPDecoder::new(BufReader::new(file)).map_err(|e| format!("解析 WebP 失败: {e}"))?;

    let (width, height) = decoder.dimensions();
    if width == 0 || height == 0 {
        return Err("WebP 尺寸为 0".to_string());
    }

    let has_alpha_stream = decoder.has_alpha();
    let is_animated = decoder.is_animated();
    let channels: usize = if has_alpha_stream { 4 } else { 3 };
    let frame_buf_len = (width as usize)
        .checked_mul(height as usize)
        .and_then(|n| n.checked_mul(channels))
        .ok_or_else(|| "WebP 尺寸溢出".to_string())?;

    let mut frames = Vec::new();
    let mut total_bytes = 0usize;
    let mut total_duration_ms = 0u64;
    let mut any_alpha = false;

    if is_animated {
        let count = (decoder.num_frames() as usize).min(MAX_ANIM_FRAMES);
        let mut raw_buf = vec![0u8; frame_buf_len];
        for _ in 0..count {
            let delay_raw = match decoder.read_frame(&mut raw_buf) {
                Ok(d) => d,
                Err(_) if !frames.is_empty() => break,
                Err(e) => return Err(format!("读取 WebP 动画帧失败: {e}")),
            };
            let (bgra, frame_alpha) = convert_webp_pixels_to_bgra_premul(&raw_buf, has_alpha_stream);
            if frame_alpha {
                any_alpha = true;
            }
            let delay_ms = if delay_raw < MIN_FRAME_DELAY_MS {
                DEFAULT_FRAME_DELAY_MS
            } else {
                delay_raw
            };
            total_duration_ms += delay_ms as u64;
            total_bytes += bgra.len();
            frames.push(ImageFrame {
                width,
                height,
                delay_ms,
                bgra_premul: bgra,
            });
            if total_bytes >= MAX_ANIM_TOTAL_BYTES {
                break;
            }
        }
    } else {
        let mut raw_buf = vec![0u8; frame_buf_len];
        decoder
            .read_image(&mut raw_buf)
            .map_err(|e| format!("解码静态 WebP 失败: {e}"))?;
        let (bgra, frame_alpha) = convert_webp_pixels_to_bgra_premul(&raw_buf, has_alpha_stream);
        any_alpha = frame_alpha;
        frames.push(ImageFrame {
            width,
            height,
            delay_ms: 0,
            bgra_premul: bgra,
        });
    }

    if frames.is_empty() {
        return Err("WebP 未包含有效图像帧".to_string());
    }

    let anim_real = frames.len() > 1;
    let label = if anim_real { "WebP 动图" } else { "WebP" };
    let mut exif = vec![
        ("格式".to_string(), label.to_string()),
        ("尺寸".to_string(), format!("{} x {}", width, height)),
        (
            "透明通道".to_string(),
            if any_alpha { "含 Alpha" } else { "不透明" }.to_string(),
        ),
    ];
    if anim_real {
        exif.push(("帧数".to_string(), format!("{} 帧", frames.len())));
        exif.push((
            "总时长".to_string(),
            format!("{:.2} s", total_duration_ms as f64 / 1000.0),
        ));
    }

    Ok(DecodedImage {
        path: path.to_path_buf(),
        format_label: label.to_string(),
        file_size_bytes: file_size,
        width,
        height,
        has_alpha: any_alpha,
        frames,
        total_duration_ms,
        exif_summary: exif,
    })
}

pub fn decode_webp_first_frame_thumb(path: &Path, max_side: u32) -> Result<ThumbnailBitmap, String> {
    let file = File::open(path).map_err(|e| format!("{e}"))?;
    let mut decoder = WebPDecoder::new(BufReader::new(file)).map_err(|e| format!("{e}"))?;
    let (width, height) = decoder.dimensions();
    let has_alpha = decoder.has_alpha();
    let channels: usize = if has_alpha { 4 } else { 3 };
    let mut raw = vec![0u8; (width as usize) * (height as usize) * channels];
    if decoder.is_animated() {
        decoder.read_frame(&mut raw).map_err(|e| format!("{e}"))?;
    } else {
        decoder.read_image(&mut raw).map_err(|e| format!("{e}"))?;
    }
    let (bgra, _) = convert_webp_pixels_to_bgra_premul(&raw, has_alpha);
    Ok(downscale_bgra_thumbnail(&bgra, width, height, max_side))
}

fn convert_webp_pixels_to_bgra_premul(raw: &[u8], has_alpha: bool) -> (Vec<u8>, bool) {
    if has_alpha {
        rgba_straight_to_bgra_premul(raw)
    } else {
        let px_count = raw.len() / 3;
        let mut out = vec![0u8; px_count * 4];
        for i in 0..px_count {
            let s = i * 3;
            let d = i * 4;
            out[d] = raw[s + 2];     // B
            out[d + 1] = raw[s + 1]; // G
            out[d + 2] = raw[s];     // R
            out[d + 3] = 255;        // A
        }
        (out, false)
    }
}
