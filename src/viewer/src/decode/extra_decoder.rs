// ─────────────────────────────────────────────────────────────────────────────
// decode/extra_decoder.rs — 零外部依赖纯 Rust 扩展格式解码器（TGA / QOI / PNM）
// 补齐 WIC 未内置的美术/游戏/开发常见图片格式，全链路防畸形图 OOM Abort
// ─────────────────────────────────────────────────────────────────────────────

use crate::decode::{
    try_alloc_filled_buffer, try_alloc_zeroed_buffer, validate_image_dimensions,
};
use crate::types::{
    try_rgba_straight_to_bgra_premul, DecodeError, DecodedImage, ExifField, ImageFormatKind,
    ImageFrame,
};
use std::path::Path;

fn build_static_image(
    path: &Path,
    file_size: u64,
    format_label: &str,
    w: u32,
    h: u32,
    rgba: &[u8],
) -> Result<DecodedImage, DecodeError> {
    let (bgra_premul, has_alpha) = try_rgba_straight_to_bgra_premul(rgba)?;
    let format_kind = ImageFormatKind::Static(format_label.to_string());
    let exif = vec![
        ExifField::Format(format_kind.clone()),
        ExifField::Dimensions(w, h),
        ExifField::Alpha(has_alpha),
    ];
    Ok(DecodedImage {
        path: path.to_path_buf(),
        format_kind,
        file_size_bytes: file_size,
        width: w,
        height: h,
        has_alpha,
        frames: vec![ImageFrame {
            width: w,
            height: h,
            delay_ms: 0,
            bgra_premul,
        }],
        total_duration_ms: 0,
        exif_summary: exif,
    })
}

/// 解码 QOI (Quite OK Image) 现代无损格式（完整规范 + 防 OOM 安全边界）
pub fn decode_qoi(path: &Path, file_size: u64) -> Result<DecodedImage, DecodeError> {
    let data = std::fs::read(path).map_err(|e| DecodeError::ReadFile {
        format: "QOI",
        detail: e.to_string(),
    })?;
    if data.len() < 22 || &data[0..4] != b"qoif" {
        return Err(DecodeError::InvalidHeader { format: "QOI" });
    }
    let w = u32::from_be_bytes([data[4], data[5], data[6], data[7]]);
    let h = u32::from_be_bytes([data[8], data[9], data[10], data[11]]);
    let channels = data[12];
    if channels != 3 && channels != 4 {
        return Err(DecodeError::UnsupportedFormat {
            format: "QOI",
            detail: format!("channels={channels}"),
        });
    }
    let total_pixels = validate_image_dimensions(w, h, "QOI")?;
    // QOI 单个 OP_RUN 字节最多编码 62 个像素，提前校验畸形小文件宣称超大尺寸
    let payload_len = data.len().saturating_sub(22);
    let max_possible_pixels = payload_len.saturating_mul(62);
    if total_pixels > max_possible_pixels {
        return Err(DecodeError::TruncatedData { format: "QOI" });
    }
    let out_bytes = total_pixels
        .checked_mul(4)
        .ok_or(DecodeError::OutOfMemory)?;
    let mut out = try_alloc_zeroed_buffer(out_bytes)?;

    let mut index = [[0u8; 4]; 64];
    let mut px = [0u8, 0, 0, 255];
    let mut p = 14usize;
    let end = data.len().saturating_sub(8);
    let mut run = 0usize;

    for i in 0..total_pixels {
        if run > 0 {
            run -= 1;
        } else if p < end {
            let b1 = data[p];
            p += 1;
            if b1 == 0xFE {
                if p + 3 > data.len() {
                    break;
                }
                px[0] = data[p];
                px[1] = data[p + 1];
                px[2] = data[p + 2];
                p += 3;
            } else if b1 == 0xFF {
                if p + 4 > data.len() {
                    break;
                }
                px = [data[p], data[p + 1], data[p + 2], data[p + 3]];
                p += 4;
            } else {
                match b1 & 0xC0 {
                    0x00 => {
                        px = index[(b1 & 0x3F) as usize];
                    }
                    0x40 => {
                        px[0] = px[0].wrapping_add(((b1 >> 4) & 0x03).wrapping_sub(2));
                        px[1] = px[1].wrapping_add(((b1 >> 2) & 0x03).wrapping_sub(2));
                        px[2] = px[2].wrapping_add((b1 & 0x03).wrapping_sub(2));
                    }
                    0x80 => {
                        if p >= data.len() {
                            break;
                        }
                        let b2 = data[p];
                        p += 1;
                        let vg = (b1 & 0x3F).wrapping_sub(32);
                        px[0] =
                            px[0].wrapping_add(vg.wrapping_sub(8).wrapping_add((b2 >> 4) & 0x0F));
                        px[1] = px[1].wrapping_add(vg);
                        px[2] = px[2].wrapping_add(vg.wrapping_sub(8).wrapping_add(b2 & 0x0F));
                    }
                    _ => {
                        run = (b1 & 0x3F) as usize;
                    }
                }
            }
            let hash = (px[0] as usize * 3
                + px[1] as usize * 5
                + px[2] as usize * 7
                + px[3] as usize * 11)
                % 64;
            index[hash] = px;
        }
        out[i * 4..i * 4 + 4].copy_from_slice(&px);
    }

    build_static_image(path, file_size, "QOI", w, h, &out)
}

/// 解码 TGA (Truevision Targa) 美术/游戏贴图格式（支持未压缩与 RLE，分配前严格校验剩余字节）
pub fn decode_tga(path: &Path, file_size: u64) -> Result<DecodedImage, DecodeError> {
    let data = std::fs::read(path).map_err(|e| DecodeError::ReadFile {
        format: "TGA",
        detail: e.to_string(),
    })?;
    if data.len() < 18 {
        return Err(DecodeError::InvalidHeader { format: "TGA" });
    }
    let id_len = data[0] as usize;
    let color_map_type = data[1];
    let img_type = data[2];
    let cm_len = u16::from_le_bytes([data[5], data[6]]) as usize;
    let cm_depth = data[7] as usize;
    let w = u16::from_le_bytes([data[12], data[13]]) as u32;
    let h = u16::from_le_bytes([data[14], data[15]]) as u32;
    let bpp = data[16];
    let desc = data[17];

    let total = validate_image_dimensions(w, h, "TGA")?;
    if color_map_type != 0 {
        return Err(DecodeError::UnsupportedFormat {
            format: "TGA",
            detail: "color-mapped".to_string(),
        });
    }
    let is_rle = matches!(img_type, 10 | 11);
    let is_gray = matches!(img_type, 3 | 11);
    let is_rgb = matches!(img_type, 2 | 10);
    if !is_gray && !is_rgb {
        return Err(DecodeError::UnsupportedFormat {
            format: "TGA",
            detail: format!("type={img_type}"),
        });
    }
    let bytes_per_px = match bpp {
        8 if is_gray => 1usize,
        16 if is_rgb => 2,
        24 if is_rgb => 3,
        32 if is_rgb => 4,
        _ => {
            return Err(DecodeError::UnsupportedFormat {
                format: "TGA",
                detail: format!("bpp={bpp}"),
            });
        }
    };

    let cm_bytes = cm_len
        .checked_mul(cm_depth.div_ceil(8))
        .ok_or(DecodeError::InvalidHeader { format: "TGA" })?;
    let mut pos = 18usize
        .checked_add(id_len)
        .and_then(|v| v.checked_add(cm_bytes))
        .ok_or(DecodeError::InvalidHeader { format: "TGA" })?;
    if pos >= data.len() {
        return Err(DecodeError::TruncatedData { format: "TGA" });
    }

    let remaining = data.len() - pos;
    if !is_rle {
        let need = total
            .checked_mul(bytes_per_px)
            .ok_or(DecodeError::OutOfMemory)?;
        if need > remaining {
            return Err(DecodeError::TruncatedData { format: "TGA" });
        }
    } else {
        // RLE 单包至少耗费 1 + bytes_per_px 字节、最多编码 128 个像素
        let min_pkt_bytes = 1 + bytes_per_px;
        let max_possible = (remaining / min_pkt_bytes).saturating_mul(128);
        if total > max_possible {
            return Err(DecodeError::TruncatedData { format: "TGA" });
        }
    }

    let rgba_bytes = total.checked_mul(4).ok_or(DecodeError::OutOfMemory)?;
    let mut raw_rgba = try_alloc_zeroed_buffer(rgba_bytes)?;

    let read_px = |src: &[u8]| -> [u8; 4] {
        match bytes_per_px {
            1 => [src[0], src[0], src[0], 255],
            2 => {
                let v = u16::from_le_bytes([src[0], src[1]]);
                let b = ((v & 0x1F) * 255 / 31) as u8;
                let g = (((v >> 5) & 0x1F) * 255 / 31) as u8;
                let r = (((v >> 10) & 0x1F) * 255 / 31) as u8;
                [r, g, b, 255]
            }
            3 => [src[2], src[1], src[0], 255],
            _ => [src[2], src[1], src[0], src[3]],
        }
    };

    if !is_rle {
        for i in 0..total {
            let px = read_px(&data[pos + i * bytes_per_px..]);
            raw_rgba[i * 4..i * 4 + 4].copy_from_slice(&px);
        }
    } else {
        let mut out_idx = 0usize;
        while out_idx < total && pos < data.len() {
            let hdr = data[pos];
            pos += 1;
            let count = ((hdr & 0x7F) as usize + 1).min(total - out_idx);
            if (hdr & 0x80) != 0 {
                if pos + bytes_per_px > data.len() {
                    break;
                }
                let px = read_px(&data[pos..]);
                pos += bytes_per_px;
                for _ in 0..count {
                    raw_rgba[out_idx * 4..out_idx * 4 + 4].copy_from_slice(&px);
                    out_idx += 1;
                }
            } else {
                for _ in 0..count {
                    if pos + bytes_per_px > data.len() {
                        break;
                    }
                    let px = read_px(&data[pos..]);
                    pos += bytes_per_px;
                    raw_rgba[out_idx * 4..out_idx * 4 + 4].copy_from_slice(&px);
                    out_idx += 1;
                }
            }
        }
    }

    // 处理 TGA 原点翻转标记（bit 5 = 0 表示自底向上，bit 4 = 1 表示自右向左）
    let top_origin = (desc & 0x20) != 0;
    let right_origin = (desc & 0x10) != 0;
    if !top_origin || right_origin {
        let mut flipped = try_alloc_zeroed_buffer(rgba_bytes)?;
        for y in 0..h as usize {
            let sy = if top_origin { y } else { h as usize - 1 - y };
            for x in 0..w as usize {
                let sx = if right_origin {
                    w as usize - 1 - x
                } else {
                    x
                };
                let si = (sy * w as usize + sx) * 4;
                let di = (y * w as usize + x) * 4;
                flipped[di..di + 4].copy_from_slice(&raw_rgba[si..si + 4]);
            }
        }
        raw_rgba = flipped;
    }

    build_static_image(path, file_size, "TGA", w, h, &raw_rgba)
}

/// 解码 Netpbm PNM 家族（PPM P3/P6、PGM P2/P5、PBM P1/P4，分配前严格校验剩余字节）
pub fn decode_pnm(path: &Path, file_size: u64) -> Result<DecodedImage, DecodeError> {
    let data = std::fs::read(path).map_err(|e| DecodeError::ReadFile {
        format: "PNM",
        detail: e.to_string(),
    })?;
    if data.len() < 3 || data[0] != b'P' {
        return Err(DecodeError::InvalidHeader { format: "PNM" });
    }
    let kind = data[1];
    if !matches!(kind, b'1'..=b'6') {
        return Err(DecodeError::UnsupportedFormat {
            format: "PNM",
            detail: format!("P{}", kind as char),
        });
    }
    let mut pos = 2usize;

    let next_token = |pos: &mut usize| -> Option<String> {
        while *pos < data.len() {
            let b = data[*pos];
            if b == b'#' {
                while *pos < data.len() && data[*pos] != b'\n' {
                    *pos += 1;
                }
            } else if b.is_ascii_whitespace() {
                *pos += 1;
            } else {
                break;
            }
        }
        let start = *pos;
        while *pos < data.len() && !data[*pos].is_ascii_whitespace() && data[*pos] != b'#' {
            *pos += 1;
        }
        if *pos > start {
            String::from_utf8(data[start..*pos].to_vec()).ok()
        } else {
            None
        }
    };

    let w: u32 = next_token(&mut pos)
        .and_then(|s| s.parse().ok())
        .ok_or(DecodeError::InvalidHeader { format: "PNM" })?;
    let h: u32 = next_token(&mut pos)
        .and_then(|s| s.parse().ok())
        .ok_or(DecodeError::InvalidHeader { format: "PNM" })?;
    let total = validate_image_dimensions(w, h, "PNM")?;

    let maxval: u32 = if matches!(kind, b'1' | b'4') {
        1
    } else {
        next_token(&mut pos)
            .and_then(|s| s.parse().ok())
            .ok_or(DecodeError::InvalidHeader { format: "PNM" })?
    };
    if maxval == 0 || maxval > 65535 {
        return Err(DecodeError::InvalidHeader { format: "PNM" });
    }
    // 二进制模式跳过紧随头部的单个空白字节
    if matches!(kind, b'4' | b'5' | b'6') && pos < data.len() && data[pos].is_ascii_whitespace() {
        pos += 1;
    }

    // 分配像素缓冲区之前严格校验剩余字节下界，彻底防御畸形小文件声明超大分辨率触发 OOM
    let remaining = data.len().saturating_sub(pos);
    let bpc = if maxval > 255 { 2usize } else { 1usize };
    let min_required = match kind {
        b'6' => total
            .checked_mul(3)
            .and_then(|v| v.checked_mul(bpc))
            .ok_or(DecodeError::OutOfMemory)?,
        b'5' => total.checked_mul(bpc).ok_or(DecodeError::OutOfMemory)?,
        b'4' => ((w as usize).div_ceil(8))
            .checked_mul(h as usize)
            .ok_or(DecodeError::OutOfMemory)?,
        b'3' => total
            .checked_mul(6)
            .map(|v| v.saturating_sub(1))
            .ok_or(DecodeError::OutOfMemory)?,
        b'2' => total
            .checked_mul(2)
            .map(|v| v.saturating_sub(1))
            .ok_or(DecodeError::OutOfMemory)?,
        b'1' => total,
        _ => 0,
    };
    if remaining < min_required {
        return Err(DecodeError::TruncatedData { format: "PNM" });
    }

    let rgba_bytes = total.checked_mul(4).ok_or(DecodeError::OutOfMemory)?;
    let mut rgba = try_alloc_filled_buffer(rgba_bytes, 255)?;
    let scale = |v: u32| -> u8 { ((v.min(maxval) * 255 + maxval / 2) / maxval) as u8 };

    match kind {
        b'6' => {
            for i in 0..total {
                let base = pos + i * 3 * bpc;
                let (r, g, b) = if bpc == 1 {
                    (
                        data[base] as u32,
                        data[base + 1] as u32,
                        data[base + 2] as u32,
                    )
                } else {
                    (
                        u16::from_be_bytes([data[base], data[base + 1]]) as u32,
                        u16::from_be_bytes([data[base + 2], data[base + 3]]) as u32,
                        u16::from_be_bytes([data[base + 4], data[base + 5]]) as u32,
                    )
                };
                rgba[i * 4] = scale(r);
                rgba[i * 4 + 1] = scale(g);
                rgba[i * 4 + 2] = scale(b);
            }
        }
        b'5' => {
            for i in 0..total {
                let base = pos + i * bpc;
                let v = if bpc == 1 {
                    data[base] as u32
                } else {
                    u16::from_be_bytes([data[base], data[base + 1]]) as u32
                };
                let g = scale(v);
                rgba[i * 4] = g;
                rgba[i * 4 + 1] = g;
                rgba[i * 4 + 2] = g;
            }
        }
        b'4' => {
            let row_bytes = (w as usize).div_ceil(8);
            for y in 0..h as usize {
                for x in 0..w as usize {
                    let byte = data[pos + y * row_bytes + x / 8];
                    let bit = (byte >> (7 - (x % 8))) & 1;
                    let c = if bit == 1 { 0u8 } else { 255u8 };
                    let idx = (y * w as usize + x) * 4;
                    rgba[idx] = c;
                    rgba[idx + 1] = c;
                    rgba[idx + 2] = c;
                }
            }
        }
        b'3' => {
            for i in 0..total {
                let r: u32 = next_token(&mut pos)
                    .and_then(|s| s.parse().ok())
                    .unwrap_or(0);
                let g: u32 = next_token(&mut pos)
                    .and_then(|s| s.parse().ok())
                    .unwrap_or(0);
                let b: u32 = next_token(&mut pos)
                    .and_then(|s| s.parse().ok())
                    .unwrap_or(0);
                rgba[i * 4] = scale(r);
                rgba[i * 4 + 1] = scale(g);
                rgba[i * 4 + 2] = scale(b);
            }
        }
        b'2' => {
            for i in 0..total {
                let v: u32 = next_token(&mut pos)
                    .and_then(|s| s.parse().ok())
                    .unwrap_or(0);
                let g = scale(v);
                rgba[i * 4] = g;
                rgba[i * 4 + 1] = g;
                rgba[i * 4 + 2] = g;
            }
        }
        b'1' => {
            for i in 0..total {
                let v: u32 = next_token(&mut pos)
                    .and_then(|s| s.parse().ok())
                    .unwrap_or(0);
                let c = if v != 0 { 0u8 } else { 255u8 };
                rgba[i * 4] = c;
                rgba[i * 4 + 1] = c;
                rgba[i * 4 + 2] = c;
            }
        }
        _ => unreachable!(),
    }

    let label = match kind {
        b'1' | b'4' => "PBM",
        b'2' | b'5' => "PGM",
        _ => "PPM",
    };
    build_static_image(path, file_size, label, w, h, &rgba)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_qoi_tga_and_pnm() {
        let dir = std::env::temp_dir().join("t3k_picasa_viewer_extra_test");
        let _ = std::fs::create_dir_all(&dir);

        // 1. QOI 2x2 RGBA
        let qoi_path = dir.join("tiny.qoi");
        let mut qoi = Vec::new();
        qoi.extend_from_slice(b"qoif");
        qoi.extend_from_slice(&2u32.to_be_bytes());
        qoi.extend_from_slice(&2u32.to_be_bytes());
        qoi.push(4);
        qoi.push(0);
        qoi.extend_from_slice(&[0xFF, 240, 100, 50, 200, 0xC2]); // 1 full + 3 run
        qoi.extend_from_slice(&[0, 0, 0, 0, 0, 0, 0, 1]);
        std::fs::write(&qoi_path, &qoi).unwrap();
        let img = decode_qoi(&qoi_path, qoi.len() as u64).expect("decode qoi");
        assert_eq!((img.width, img.height), (2, 2));
        assert!(img.has_alpha);

        // 2. TGA 2x2 uncompressed 32-bit
        let tga_path = dir.join("tiny.tga");
        let mut tga = vec![0u8; 18];
        tga[2] = 2; // uncompressed RGB
        tga[12..14].copy_from_slice(&2u16.to_le_bytes());
        tga[14..16].copy_from_slice(&2u16.to_le_bytes());
        tga[16] = 32;
        tga[17] = 0x28;
        for _ in 0..4 {
            tga.extend_from_slice(&[20, 120, 250, 255]);
        }
        std::fs::write(&tga_path, &tga).unwrap();
        let img = decode_tga(&tga_path, tga.len() as u64).expect("decode tga");
        assert_eq!((img.width, img.height), (2, 2));
        assert!(!img.has_alpha);

        // 3. PPM P6 2x2
        let ppm_path = dir.join("tiny.ppm");
        let mut ppm = b"P6\n2 2\n255\n".to_vec();
        ppm.extend_from_slice(&[255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255]);
        std::fs::write(&ppm_path, &ppm).unwrap();
        let img = decode_pnm(&ppm_path, ppm.len() as u64).expect("decode ppm");
        assert_eq!((img.width, img.height), (2, 2));

        let _ = std::fs::remove_file(&qoi_path);
        let _ = std::fs::remove_file(&tga_path);
        let _ = std::fs::remove_file(&ppm_path);
    }

    #[test]
    fn rejects_malformed_tga_pnm_qoi_headers_without_oom() {
        let dir = std::env::temp_dir().join("t3k_picasa_viewer_malformed_test");
        let _ = std::fs::create_dir_all(&dir);

        // 畸形 TGA：仅 24 字节却声明 10000x10000 未压缩像素
        let bad_tga = dir.join("bad.tga");
        let mut tga = vec![0u8; 24];
        tga[2] = 2;
        tga[12..14].copy_from_slice(&10_000u16.to_le_bytes());
        tga[14..16].copy_from_slice(&10_000u16.to_le_bytes());
        tga[16] = 32;
        std::fs::write(&bad_tga, &tga).unwrap();
        assert!(matches!(
            decode_tga(&bad_tga, tga.len() as u64),
            Err(DecodeError::TruncatedData { format: "TGA" })
        ));

        // 畸形 RLE TGA：仅 24 字节却声明 10000x10000 RLE 像素
        tga[2] = 10;
        std::fs::write(&bad_tga, &tga).unwrap();
        assert!(matches!(
            decode_tga(&bad_tga, tga.len() as u64),
            Err(DecodeError::TruncatedData { format: "TGA" })
        ));

        // 畸形 PPM：仅 16 字节却声明 50000x50000
        let bad_ppm = dir.join("bad.ppm");
        let ppm = b"P6\n50000 50000\n255\nABC";
        std::fs::write(&bad_ppm, ppm).unwrap();
        assert!(matches!(
            decode_pnm(&bad_ppm, ppm.len() as u64),
            Err(DecodeError::DimensionsTooLarge { .. })
        ));

        // 畸形截断 PPM：合法像素数内但文件被截断
        let trunc_ppm = dir.join("trunc.ppm");
        let ppm2 = b"P6\n4000 4000\n255\nABC";
        std::fs::write(&trunc_ppm, ppm2).unwrap();
        assert!(matches!(
            decode_pnm(&trunc_ppm, ppm2.len() as u64),
            Err(DecodeError::TruncatedData { format: "PNM" })
        ));

        let _ = std::fs::remove_file(&bad_tga);
        let _ = std::fs::remove_file(&bad_ppm);
        let _ = std::fs::remove_file(&trunc_ppm);
    }
}
