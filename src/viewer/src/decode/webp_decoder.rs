// ─────────────────────────────────────────────────────────────────────────────
// decode/webp_decoder.rs — 纯 Rust 静态 & 动态 WebP 完整解码器 (VP8 / VP8L / ANIM)
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{
    DEFAULT_FRAME_DELAY_MS, MAX_ANIM_FRAMES, MAX_ANIM_TOTAL_BYTES, MIN_FRAME_DELAY_MS,
};
use crate::decode::{
    downscale_bgra_thumbnail, try_alloc_zeroed_buffer, validate_image_dimensions,
};
use crate::types::{
    try_rgba_straight_to_bgra_premul, DecodeError, DecodedImage, ExifField, ImageFormatKind,
    ImageFrame, ThumbnailBitmap,
};
use image_webp::WebPDecoder;
use std::fs::File;
use std::io::BufReader;
use std::path::Path;

pub fn decode_webp(path: &Path, file_size: u64) -> Result<DecodedImage, DecodeError> {
    let file = File::open(path).map_err(|e| DecodeError::OpenFile(e.to_string()))?;
    let mut decoder =
        WebPDecoder::new(BufReader::new(file)).map_err(|e| DecodeError::DecodeFailed {
            format: "WebP",
            detail: e.to_string(),
        })?;

    let (width, height) = decoder.dimensions();
    let total_pixels = validate_image_dimensions(width, height, "WebP")?;

    let has_alpha_stream = decoder.has_alpha();
    let is_animated = decoder.is_animated();
    let channels: usize = if has_alpha_stream { 4 } else { 3 };
    let frame_buf_len = total_pixels
        .checked_mul(channels)
        .ok_or(DecodeError::OutOfMemory)?;

    let mut frames = Vec::new();
    let mut total_bytes = 0usize;
    let mut total_duration_ms = 0u64;
    let mut any_alpha = false;

    if is_animated {
        let count = (decoder.num_frames() as usize).min(MAX_ANIM_FRAMES);
        let mut raw_buf = try_alloc_zeroed_buffer(frame_buf_len)?;
        for _ in 0..count {
            let delay_raw = match decoder.read_frame(&mut raw_buf) {
                Ok(d) => d,
                Err(_) if !frames.is_empty() => break,
                Err(e) => {
                    return Err(DecodeError::DecodeFailed {
                        format: "WebP",
                        detail: e.to_string(),
                    });
                }
            };
            let (bgra, frame_alpha) =
                convert_webp_pixels_to_bgra_premul(&raw_buf, has_alpha_stream)?;
            if frame_alpha {
                any_alpha = true;
            }
            let delay_ms = if delay_raw < MIN_FRAME_DELAY_MS {
                DEFAULT_FRAME_DELAY_MS
            } else {
                delay_raw
            };
            total_duration_ms = total_duration_ms.saturating_add(delay_ms as u64);
            total_bytes = total_bytes.saturating_add(bgra.len());
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
        let mut raw_buf = try_alloc_zeroed_buffer(frame_buf_len)?;
        decoder
            .read_image(&mut raw_buf)
            .map_err(|e| DecodeError::DecodeFailed {
                format: "WebP",
                detail: e.to_string(),
            })?;
        let (bgra, frame_alpha) = convert_webp_pixels_to_bgra_premul(&raw_buf, has_alpha_stream)?;
        any_alpha = frame_alpha;
        frames.push(ImageFrame {
            width,
            height,
            delay_ms: 0,
            bgra_premul: bgra,
        });
    }

    if frames.is_empty() {
        return Err(DecodeError::NoValidFrames { format: "WebP" });
    }

    let anim_real = frames.len() > 1;
    let format_kind = if anim_real {
        ImageFormatKind::AnimatedWebP
    } else {
        ImageFormatKind::Static("WebP".to_string())
    };
    let mut exif = vec![
        ExifField::Format(format_kind.clone()),
        ExifField::Dimensions(width, height),
        ExifField::Alpha(any_alpha),
    ];
    if anim_real {
        exif.push(ExifField::FrameCount {
            count: frames.len(),
            best_selected: false,
            duration_sec: None,
        });
        exif.push(ExifField::DurationSec(total_duration_ms as f64 / 1000.0));
    }

    Ok(DecodedImage {
        path: path.to_path_buf(),
        format_kind,
        file_size_bytes: file_size,
        width,
        height,
        has_alpha: any_alpha,
        frames,
        total_duration_ms,
        exif_summary: exif,
    })
}

pub fn decode_webp_first_frame_thumb(
    path: &Path,
    max_side: u32,
) -> Result<ThumbnailBitmap, DecodeError> {
    let file = File::open(path).map_err(|e| DecodeError::OpenFile(e.to_string()))?;
    let mut decoder =
        WebPDecoder::new(BufReader::new(file)).map_err(|e| DecodeError::DecodeFailed {
            format: "WebP",
            detail: e.to_string(),
        })?;
    let (width, height) = decoder.dimensions();
    let total_pixels = validate_image_dimensions(width, height, "WebP")?;
    let has_alpha = decoder.has_alpha();
    let channels: usize = if has_alpha { 4 } else { 3 };
    let raw_len = total_pixels
        .checked_mul(channels)
        .ok_or(DecodeError::OutOfMemory)?;
    let mut raw = try_alloc_zeroed_buffer(raw_len)?;
    if decoder.is_animated() {
        decoder
            .read_frame(&mut raw)
            .map_err(|e| DecodeError::DecodeFailed {
                format: "WebP",
                detail: e.to_string(),
            })?;
    } else {
        decoder
            .read_image(&mut raw)
            .map_err(|e| DecodeError::DecodeFailed {
                format: "WebP",
                detail: e.to_string(),
            })?;
    }
    let (bgra, _) = convert_webp_pixels_to_bgra_premul(&raw, has_alpha)?;
    Ok(downscale_bgra_thumbnail(&bgra, width, height, max_side))
}

fn convert_webp_pixels_to_bgra_premul(
    raw: &[u8],
    has_alpha: bool,
) -> Result<(Vec<u8>, bool), DecodeError> {
    if has_alpha {
        try_rgba_straight_to_bgra_premul(raw)
    } else {
        let px_count = raw.len() / 3;
        let out_len = px_count.checked_mul(4).ok_or(DecodeError::OutOfMemory)?;
        let mut out = try_alloc_zeroed_buffer(out_len)?;
        for i in 0..px_count {
            let s = i * 3;
            let d = i * 4;
            out[d] = raw[s + 2];     // B
            out[d + 1] = raw[s + 1]; // G
            out[d + 2] = raw[s];     // R
            out[d + 3] = 255;        // A
        }
        Ok((out, false))
    }
}
