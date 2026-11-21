// ─────────────────────────────────────────────────────────────────────────────
// decode/extra_decoder.rs — 零外部依赖纯 Rust 扩展格式解码器（TGA / QOI / PNM）
// 补齐 WIC 未内置的美术/游戏/开发常见图片格式，几乎 0 KB 二进制增量
// ─────────────────────────────────────────────────────────────────────────────

use crate::types::{rgba_straight_to_bgra_premul, DecodedImage, ImageFrame};
use std::path::Path;

fn build_static_image(
    path: &Path,
    file_size: u64,
    format_label: &str,
    w: u32,
    h: u32,
    rgba: &[u8],
) -> DecodedImage {
    let (bgra_premul, has_alpha) = rgba_straight_to_bgra_premul(rgba);
    let exif = vec![
        ("格式".to_string(), format_label.to_string()),
        ("尺寸".to_string(), format!("{} x {}", w, h)),
        (
            "透明通道".to_string(),
            if has_alpha { "含 Alpha" } else { "不透明" }.to_string(),
        ),
    ];
    DecodedImage {
        path: path.to_path_buf(),
        format_label: format_label.to_string(),
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
    }
}

/// 解码 QOI (Quite OK Image) 现代无损格式（完整规范）
pub fn decode_qoi(path: &Path, file_size: u64) -> Result<DecodedImage, String> {
    let data = std::fs::read(path).map_err(|e| format!("读取 QOI 失败: {e}"))?;
    if data.len() < 22 || &data[0..4] != b"qoif" {
        return Err("无效的 QOI 文件头".to_string());
    }
    let w = u32::from_be_bytes([data[4], data[5], data[6], data[7]]);
    let h = u32::from_be_bytes([data[8], data[9], data[10], data[11]]);
    let channels = data[12];
    if w == 0 || h == 0 || (channels != 3 && channels != 4) {
        return Err("不支持的 QOI 尺寸或通道配置".to_string());
    }
    let total_pixels = (w as usize)
        .checked_mul(h as usize)
        .ok_or_else(|| "QOI 尺寸溢出".to_string())?;
    let mut out = vec![0u8; total_pixels * 4];
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
                        px[0] = px[0].wrapping_add(vg.wrapping_sub(8).wrapping_add((b2 >> 4) & 0x0F));
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

    Ok(build_static_image(path, file_size, "QOI", w, h, &out))
}

/// 解码 TGA (Truevision Targa) 常见美术/游戏贴图格式（支持未压缩与 RLE，8/16/24/32-bit）
pub fn decode_tga(path: &Path, file_size: u64) -> Result<DecodedImage, String> {
    let data = std::fs::read(path).map_err(|e| format!("读取 TGA 失败: {e}"))?;
    if data.len() < 18 {
        return Err("无效的 TGA 文件长度".to_string());
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

    if w == 0 || h == 0 {
        return Err("TGA 尺寸为 0".to_string());
    }
    if color_map_type != 0 {
        return Err("暂不支持调色板索引型 TGA".to_string());
    }
    let is_rle = matches!(img_type, 10 | 11);
    let is_gray = matches!(img_type, 3 | 11);
    let is_rgb = matches!(img_type, 2 | 10);
    if !is_gray && !is_rgb {
        return Err(format!("不支持的 TGA 图像类型: {img_type}"));
    }
    let bytes_per_px = match bpp {
        8 if is_gray => 1usize,
        16 if is_rgb => 2,
        24 if is_rgb => 3,
        32 if is_rgb => 4,
        _ => return Err(format!("不支持的 TGA 位深: {bpp}")),
    };

    let mut pos = 18 + id_len + cm_len * ((cm_depth + 7) / 8);
    if pos >= data.len() {
        return Err("TGA 像素数据截断".to_string());
    }
    let total = (w as usize) * (h as usize);
    let mut raw_rgba = vec![0u8; total * 4];

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
        let need = total * bytes_per_px;
        if pos + need > data.len() {
            return Err("TGA 未压缩像素区不足".to_string());
        }
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
        let mut flipped = vec![0u8; total * 4];
        for y in 0..h as usize {
            let sy = if top_origin { y } else { h as usize - 1 - y };
            for x in 0..w as usize {
                let sx = if right_origin { w as usize - 1 - x } else { x };
                let si = (sy * w as usize + sx) * 4;
                let di = (y * w as usize + x) * 4;
                flipped[di..di + 4].copy_from_slice(&raw_rgba[si..si + 4]);
            }
        }
        raw_rgba = flipped;
    }

    Ok(build_static_image(path, file_size, "TGA", w, h, &raw_rgba))
}

/// 解码 Netpbm PNM 家族（PPM P3/P6、PGM P2/P5、PBM P1/P4）
pub fn decode_pnm(path: &Path, file_size: u64) -> Result<DecodedImage, String> {
    let data = std::fs::read(path).map_err(|e| format!("读取 PNM 失败: {e}"))?;
    if data.len() < 3 || data[0] != b'P' {
        return Err("无效的 PNM 魔数".to_string());
    }
    let kind = data[1];
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
        .ok_or_else(|| "PNM 宽度解析失败".to_string())?;
    let h: u32 = next_token(&mut pos)
        .and_then(|s| s.parse().ok())
        .ok_or_else(|| "PNM 高度解析失败".to_string())?;
    if w == 0 || h == 0 {
        return Err("PNM 尺寸为 0".to_string());
    }
    let maxval: u32 = if matches!(kind, b'1' | b'4') {
        1
    } else {
        next_token(&mut pos)
            .and_then(|s| s.parse().ok())
            .ok_or_else(|| "PNM maxval 解析失败".to_string())?
    };
    if maxval == 0 {
        return Err("PNM maxval 为 0".to_string());
    }
    // 二进制模式跳过紧随头部的单个空白字节
    if matches!(kind, b'4' | b'5' | b'6') && pos < data.len() && data[pos].is_ascii_whitespace() {
        pos += 1;
    }

    let total = (w as usize) * (h as usize);
    let mut rgba = vec![255u8; total * 4];
    let scale = |v: u32| -> u8 { ((v.min(maxval) * 255 + maxval / 2) / maxval) as u8 };

    match kind {
        b'6' => {
            let bpc = if maxval > 255 { 2 } else { 1 };
            let need = total * 3 * bpc;
            if pos + need > data.len() {
                return Err("PPM(P6) 像素数据不足".to_string());
            }
            for i in 0..total {
                let base = pos + i * 3 * bpc;
                let (r, g, b) = if bpc == 1 {
                    (data[base] as u32, data[base + 1] as u32, data[base + 2] as u32)
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
            let bpc = if maxval > 255 { 2 } else { 1 };
            let need = total * bpc;
            if pos + need > data.len() {
                return Err("PGM(P5) 像素数据不足".to_string());
            }
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
            let row_bytes = (w as usize + 7) / 8;
            if pos + row_bytes * h as usize > data.len() {
                return Err("PBM(P4) 像素数据不足".to_string());
            }
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
                let r: u32 = next_token(&mut pos).and_then(|s| s.parse().ok()).unwrap_or(0);
                let g: u32 = next_token(&mut pos).and_then(|s| s.parse().ok()).unwrap_or(0);
                let b: u32 = next_token(&mut pos).and_then(|s| s.parse().ok()).unwrap_or(0);
                rgba[i * 4] = scale(r);
                rgba[i * 4 + 1] = scale(g);
                rgba[i * 4 + 2] = scale(b);
            }
        }
        b'2' => {
            for i in 0..total {
                let v: u32 = next_token(&mut pos).and_then(|s| s.parse().ok()).unwrap_or(0);
                let g = scale(v);
                rgba[i * 4] = g;
                rgba[i * 4 + 1] = g;
                rgba[i * 4 + 2] = g;
            }
        }
        b'1' => {
            for i in 0..total {
                let v: u32 = next_token(&mut pos).and_then(|s| s.parse().ok()).unwrap_or(0);
                let c = if v != 0 { 0u8 } else { 255u8 };
                rgba[i * 4] = c;
                rgba[i * 4 + 1] = c;
                rgba[i * 4 + 2] = c;
            }
        }
        _ => return Err(format!("不支持的 PNM 子格式: P{}", kind as char)),
    }

    let label = match kind {
        b'1' | b'4' => "PBM",
        b'2' | b'5' => "PGM",
        _ => "PPM",
    };
    Ok(build_static_image(path, file_size, label, w, h, &rgba))
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
}
