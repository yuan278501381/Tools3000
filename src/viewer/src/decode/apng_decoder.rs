// ─────────────────────────────────────────────────────────────────────────────
// decode/apng_decoder.rs — 纯 Rust 动态 APNG 多帧合成解码器（零新增依赖，复用 png）
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{
    DEFAULT_FRAME_DELAY_MS, MAX_ANIM_FRAMES, MAX_ANIM_TOTAL_BYTES, MIN_FRAME_DELAY_MS,
};
use crate::types::{rgba_straight_to_bgra_premul, DecodedImage, ImageFrame};
use std::fs::File;
use std::io::{BufReader, Read};
use std::path::Path;

/// 快速扫描 PNG 文件头部块，判断是否包含 `acTL` 动图控制块（静态 PNG < 0.05ms 直接跳过走 WIC）
pub fn is_animated_png_file(path: &Path) -> bool {
    let Ok(mut f) = File::open(path) else {
        return false;
    };
    let mut sig = [0u8; 8];
    if f.read_exact(&mut sig).is_err() || sig != [137, 80, 78, 71, 13, 10, 26, 10] {
        return false;
    }
    let mut hdr = [0u8; 8];
    for _ in 0..24 {
        if f.read_exact(&mut hdr).is_err() {
            break;
        }
        let chunk_len = u32::from_be_bytes([hdr[0], hdr[1], hdr[2], hdr[3]]) as i64;
        let chunk_type = &hdr[4..8];
        if chunk_type == b"acTL" {
            return true;
        }
        if chunk_type == b"IDAT" || chunk_type == b"IEND" {
            return false;
        }
        use std::io::{Seek, SeekFrom};
        if f.seek(SeekFrom::Current(chunk_len + 4)).is_err() {
            break;
        }
    }
    false
}

fn alpha_over_rgba(dst: &mut [u8], src: &[u8]) {
    let sa = src[3] as u32;
    if sa == 0 {
        return;
    }
    if sa == 255 || dst[3] == 0 {
        dst.copy_from_slice(src);
        return;
    }
    let da = dst[3] as u32;
    let inv_sa = 255 - sa;
    let out_a = sa * 255 + da * inv_sa;
    if out_a == 0 {
        dst.copy_from_slice(&[0, 0, 0, 0]);
        return;
    }
    for c in 0..3 {
        let num = src[c] as u32 * sa * 255 + dst[c] as u32 * da * inv_sa;
        dst[c] = ((num + out_a / 2) / out_a).min(255) as u8;
    }
    dst[3] = ((out_a + 127) / 255).min(255) as u8;
}

pub fn decode_apng(path: &Path, file_size: u64) -> Result<DecodedImage, String> {
    let file = File::open(path).map_err(|e| format!("打开 APNG 失败: {e}"))?;
    let mut decoder = png::Decoder::new(BufReader::new(file));
    decoder.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
    let mut reader = decoder
        .read_info()
        .map_err(|e| format!("解析 APNG 头失败: {e}"))?;

    let info = reader.info();
    let canvas_w = info.width;
    let canvas_h = info.height;
    if canvas_w == 0 || canvas_h == 0 {
        return Err("APNG 尺寸为 0".to_string());
    }
    let total_expected = info
        .animation_control()
        .map(|ac| ac.num_frames as usize)
        .unwrap_or(1)
        .max(1);

    let frame_bytes = (canvas_w as usize)
        .checked_mul(canvas_h as usize)
        .and_then(|v| v.checked_mul(4))
        .ok_or_else(|| "APNG 画布尺寸溢出".to_string())?;
    let mut canvas = vec![0u8; frame_bytes];
    let mut frames = Vec::new();
    let mut total_bytes = 0usize;
    let mut total_duration_ms = 0u64;
    let mut has_alpha = false;
    let mut scratch = vec![0u8; reader.output_buffer_size()];

    for _ in 0..total_expected.min(MAX_ANIM_FRAMES) {
        let fc = reader.info().frame_control().cloned();
        let out_info = match reader.next_frame(&mut scratch) {
            Ok(i) => i,
            Err(_) => break,
        };
        let fw = out_info.width;
        let fh = out_info.height;
        let (x_off, y_off, delay_ms, dispose_op, blend_op) = if let Some(fc) = fc {
            let den = if fc.delay_den == 0 { 100 } else { fc.delay_den } as u32;
            let raw_ms = (fc.delay_num as u32) * 1000 / den;
            let ms = if raw_ms == 0 {
                DEFAULT_FRAME_DELAY_MS
            } else {
                raw_ms.max(MIN_FRAME_DELAY_MS)
            };
            (fc.x_offset, fc.y_offset, ms, fc.dispose_op, fc.blend_op)
        } else {
            (
                0,
                0,
                DEFAULT_FRAME_DELAY_MS,
                png::DisposeOp::None,
                png::BlendOp::Source,
            )
        };

        let prev_canvas = if dispose_op == png::DisposeOp::Previous {
            Some(canvas.clone())
        } else {
            None
        };

        let raw_frame = &scratch[..out_info.buffer_size()];
        let bpp = match out_info.color_type {
            png::ColorType::Rgba => 4,
            png::ColorType::Rgb => 3,
            png::ColorType::GrayscaleAlpha => 2,
            png::ColorType::Grayscale => 1,
            _ => 4,
        };

        for y in 0..fh {
            let cy = y_off.saturating_add(y);
            if cy >= canvas_h {
                continue;
            }
            for x in 0..fw {
                let cx = x_off.saturating_add(x);
                if cx >= canvas_w {
                    continue;
                }
                let si = ((y * fw + x) as usize) * bpp;
                let rgba = match bpp {
                    4 => [raw_frame[si], raw_frame[si + 1], raw_frame[si + 2], raw_frame[si + 3]],
                    3 => [raw_frame[si], raw_frame[si + 1], raw_frame[si + 2], 255],
                    2 => [raw_frame[si], raw_frame[si], raw_frame[si], raw_frame[si + 1]],
                    _ => [raw_frame[si], raw_frame[si], raw_frame[si], 255],
                };
                let di = ((cy * canvas_w + cx) as usize) * 4;
                if blend_op == png::BlendOp::Source {
                    canvas[di..di + 4].copy_from_slice(&rgba);
                } else {
                    alpha_over_rgba(&mut canvas[di..di + 4], &rgba);
                }
            }
        }

        let (bgra_premul, frame_alpha) = rgba_straight_to_bgra_premul(&canvas);
        if frame_alpha {
            has_alpha = true;
        }
        total_bytes = total_bytes.saturating_add(bgra_premul.len());
        total_duration_ms = total_duration_ms.saturating_add(delay_ms as u64);
        frames.push(ImageFrame {
            width: canvas_w,
            height: canvas_h,
            delay_ms,
            bgra_premul,
        });

        match dispose_op {
            png::DisposeOp::Background => {
                for y in 0..fh {
                    let cy = y_off.saturating_add(y);
                    if cy >= canvas_h {
                        continue;
                    }
                    for x in 0..fw {
                        let cx = x_off.saturating_add(x);
                        if cx >= canvas_w {
                            continue;
                        }
                        let di = ((cy * canvas_w + cx) as usize) * 4;
                        canvas[di..di + 4].copy_from_slice(&[0, 0, 0, 0]);
                    }
                }
            }
            png::DisposeOp::Previous => {
                if let Some(prev) = prev_canvas {
                    canvas = prev;
                }
            }
            png::DisposeOp::None => {}
        }

        if total_bytes >= MAX_ANIM_TOTAL_BYTES {
            break;
        }
    }

    if frames.is_empty() {
        return Err("APNG 未解码出任何有效帧".to_string());
    }

    let is_anim = frames.len() > 1;
    let mut exif = vec![
        (
            "格式".to_string(),
            if is_anim { "APNG 动图" } else { "PNG" }.to_string(),
        ),
        ("尺寸".to_string(), format!("{} x {}", canvas_w, canvas_h)),
    ];
    if is_anim {
        exif.push((
            "动画帧数".to_string(),
            format!("{} 帧（{:.2}s）", frames.len(), total_duration_ms as f64 / 1000.0),
        ));
    }

    Ok(DecodedImage {
        path: path.to_path_buf(),
        format_label: if is_anim { "APNG" } else { "PNG" }.to_string(),
        file_size_bytes: file_size,
        width: canvas_w,
        height: canvas_h,
        has_alpha,
        frames,
        total_duration_ms: if is_anim { total_duration_ms } else { 0 },
        exif_summary: exif,
    })
}
