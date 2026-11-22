// ─────────────────────────────────────────────────────────────────────────────
// decode/gif_decoder.rs — 纯 Rust GIF89a 动图完整画布合成解码器（安全防 OOM）
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{
    DEFAULT_FRAME_DELAY_MS, MAX_ANIM_FRAMES, MAX_ANIM_TOTAL_BYTES, MIN_FRAME_DELAY_MS,
};
use crate::decode::{
    downscale_bgra_thumbnail, try_alloc_zeroed_buffer, try_clone_buffer, validate_image_dimensions,
};
use crate::types::{
    try_rgba_straight_to_bgra_premul, DecodeError, DecodedImage, ExifField, ImageFormatKind,
    ImageFrame, ThumbnailBitmap,
};
use gif::{ColorOutput, DecodeOptions, DisposalMethod};
use std::fs::File;
use std::io::BufReader;
use std::path::Path;

pub fn decode_gif(path: &Path, file_size: u64) -> Result<DecodedImage, DecodeError> {
    let file = File::open(path).map_err(|e| DecodeError::OpenFile(e.to_string()))?;
    let mut opts = DecodeOptions::new();
    opts.set_color_output(ColorOutput::RGBA);
    let mut decoder = opts
        .read_info(BufReader::new(file))
        .map_err(|e| DecodeError::DecodeFailed {
            format: "GIF",
            detail: e.to_string(),
        })?;

    let canvas_w = decoder.width() as u32;
    let canvas_h = decoder.height() as u32;
    let total_pixels = validate_image_dimensions(canvas_w, canvas_h, "GIF")?;
    let pixel_bytes = total_pixels.checked_mul(4).ok_or(DecodeError::OutOfMemory)?;

    let mut canvas = try_alloc_zeroed_buffer(pixel_bytes)?;
    let mut frames = Vec::new();
    let mut total_bytes = 0usize;
    let mut total_duration_ms = 0u64;
    let mut any_alpha = false;

    while let Some(frame) = decoder
        .read_next_frame()
        .map_err(|e| DecodeError::DecodeFailed {
            format: "GIF",
            detail: e.to_string(),
        })?
    {
        let prev_backup = if frame.dispose == DisposalMethod::Previous {
            Some(try_clone_buffer(&canvas)?)
        } else {
            None
        };

        let left = frame.left as u32;
        let top = frame.top as u32;
        let fw = frame.width as u32;
        let fh = frame.height as u32;
        let buf = frame.buffer.as_ref();

        for fy in 0..fh {
            let cy = top.saturating_add(fy);
            if cy >= canvas_h {
                break;
            }
            for fx in 0..fw {
                let cx = left.saturating_add(fx);
                if cx >= canvas_w {
                    break;
                }
                let si = ((fy as usize) * (fw as usize) + (fx as usize)) * 4;
                if si + 3 >= buf.len() {
                    continue;
                }
                let a = buf[si + 3];
                if a > 0 {
                    let di = ((cy as usize) * (canvas_w as usize) + (cx as usize)) * 4;
                    canvas[di] = buf[si];
                    canvas[di + 1] = buf[si + 1];
                    canvas[di + 2] = buf[si + 2];
                    canvas[di + 3] = a;
                }
            }
        }

        let (bgra, has_alpha) = try_rgba_straight_to_bgra_premul(&canvas)?;
        if has_alpha {
            any_alpha = true;
        }
        // GIF 标准 delay 单位为 10ms，0 或 1 通常按浏览器标准回退至 100ms
        let raw_ms = (frame.delay as u32) * 10;
        let delay_ms = if raw_ms < MIN_FRAME_DELAY_MS {
            DEFAULT_FRAME_DELAY_MS
        } else {
            raw_ms
        };
        total_duration_ms = total_duration_ms.saturating_add(delay_ms as u64);
        total_bytes = total_bytes.saturating_add(bgra.len());

        frames.push(ImageFrame {
            width: canvas_w,
            height: canvas_h,
            delay_ms,
            bgra_premul: bgra,
        });

        match frame.dispose {
            DisposalMethod::Background => {
                for fy in 0..fh {
                    let cy = top.saturating_add(fy);
                    if cy >= canvas_h {
                        break;
                    }
                    for fx in 0..fw {
                        let cx = left.saturating_add(fx);
                        if cx >= canvas_w {
                            break;
                        }
                        let di = ((cy as usize) * (canvas_w as usize) + (cx as usize)) * 4;
                        canvas[di] = 0;
                        canvas[di + 1] = 0;
                        canvas[di + 2] = 0;
                        canvas[di + 3] = 0;
                    }
                }
            }
            DisposalMethod::Previous => {
                if let Some(prev) = prev_backup {
                    canvas = prev;
                }
            }
            DisposalMethod::Any | DisposalMethod::Keep => {}
        }

        if frames.len() >= MAX_ANIM_FRAMES || total_bytes >= MAX_ANIM_TOTAL_BYTES {
            break;
        }
    }

    if frames.is_empty() {
        return Err(DecodeError::NoValidFrames { format: "GIF" });
    }

    let is_anim = frames.len() > 1;
    let format_kind = if is_anim {
        ImageFormatKind::AnimatedGif
    } else {
        ImageFormatKind::Static("GIF".to_string())
    };
    let mut exif = vec![
        ExifField::Format(format_kind.clone()),
        ExifField::Dimensions(canvas_w, canvas_h),
    ];
    if is_anim {
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
        width: canvas_w,
        height: canvas_h,
        has_alpha: any_alpha,
        frames,
        total_duration_ms,
        exif_summary: exif,
    })
}

pub fn decode_gif_first_frame_thumb(
    path: &Path,
    max_side: u32,
) -> Result<ThumbnailBitmap, DecodeError> {
    let file = File::open(path).map_err(|e| DecodeError::OpenFile(e.to_string()))?;
    let mut opts = DecodeOptions::new();
    opts.set_color_output(ColorOutput::RGBA);
    let mut decoder = opts
        .read_info(BufReader::new(file))
        .map_err(|e| DecodeError::DecodeFailed {
            format: "GIF",
            detail: e.to_string(),
        })?;
    let cw = decoder.width() as u32;
    let ch = decoder.height() as u32;
    let total_pixels = validate_image_dimensions(cw, ch, "GIF")?;
    let pixel_bytes = total_pixels.checked_mul(4).ok_or(DecodeError::OutOfMemory)?;

    let frame = decoder
        .read_next_frame()
        .map_err(|e| DecodeError::DecodeFailed {
            format: "GIF",
            detail: e.to_string(),
        })?
        .ok_or(DecodeError::NoValidFrames { format: "GIF" })?;
    let mut canvas = try_alloc_zeroed_buffer(pixel_bytes)?;
    let left = frame.left as u32;
    let top = frame.top as u32;
    let fw = frame.width as u32;
    let fh = frame.height as u32;
    let buf = frame.buffer.as_ref();
    for fy in 0..fh {
        let cy = top.saturating_add(fy);
        if cy >= ch {
            break;
        }
        for fx in 0..fw {
            let cx = left.saturating_add(fx);
            if cx >= cw {
                break;
            }
            let si = ((fy as usize) * (fw as usize) + (fx as usize)) * 4;
            let di = ((cy as usize) * (cw as usize) + (cx as usize)) * 4;
            if si + 3 < buf.len() && di + 3 < canvas.len() {
                canvas[di..di + 4].copy_from_slice(&buf[si..si + 4]);
            }
        }
    }
    let (bgra, _) = try_rgba_straight_to_bgra_premul(&canvas)?;
    Ok(downscale_bgra_thumbnail(&bgra, cw, ch, max_side))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_multi_frame_animated_gif() {
        let dir = std::env::temp_dir().join("t3k_picasa_viewer_gif_test");
        let _ = std::fs::create_dir_all(&dir);
        let gif_path = dir.join("anim3.gif");
        {
            let file = File::create(&gif_path).expect("create test gif");
            let palette = [255, 60, 40, 40, 210, 90, 40, 140, 250, 255, 255, 255];
            let mut enc = gif::Encoder::new(file, 16, 12, &palette).expect("gif enc");
            enc.set_repeat(gif::Repeat::Infinite).expect("repeat");
            for color_idx in 0u8..3u8 {
                let mut frame = gif::Frame::default();
                frame.width = 16;
                frame.height = 12;
                frame.delay = 8; // 80ms
                frame.buffer = std::borrow::Cow::Owned(vec![color_idx; 16 * 12]);
                enc.write_frame(&frame).expect("write frame");
            }
        }
        let meta = std::fs::metadata(&gif_path).unwrap();
        let img = decode_gif(&gif_path, meta.len()).expect("decode animated gif");
        assert_eq!(img.width, 16);
        assert_eq!(img.height, 12);
        assert_eq!(img.frames.len(), 3);
        assert_eq!(img.total_duration_ms, 240);
        assert!(img.is_animated());
        assert_eq!(img.format_kind, ImageFormatKind::AnimatedGif);
        let _ = std::fs::remove_file(&gif_path);
    }
}
