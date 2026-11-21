// ─────────────────────────────────────────────────────────────────────────────
// decode/svg_decoder.rs — 纯 Rust SVG 高清矢量栅格化解码器 (resvg)
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::SVG_TARGET_LONG_SIDE;
use crate::types::{DecodedImage, ImageFrame, ThumbnailBitmap};
use std::io::Read;
use std::path::Path;

fn read_svg_or_svgz_bytes(path: &Path) -> Result<Vec<u8>, String> {
    let raw = std::fs::read(path).map_err(|e| format!("读取 SVG 失败: {e}"))?;
    // 自动识别 gzip 压缩的 .svgz（魔数 1F 8B）
    if raw.len() >= 2 && raw[0] == 0x1F && raw[1] == 0x8B {
        let mut decoder = flate2::read::GzDecoder::new(&raw[..]);
        let mut out = Vec::new();
        decoder
            .read_to_end(&mut out)
            .map_err(|e| format!("解压 SVGZ 失败: {e}"))?;
        return Ok(out);
    }
    Ok(raw)
}

pub fn decode_svg(path: &Path, file_size: u64) -> Result<DecodedImage, String> {
    let bytes = read_svg_or_svgz_bytes(path)?;
    let opt = resvg::usvg::Options::default();
    let tree = resvg::usvg::Tree::from_data(&bytes, &opt)
        .map_err(|e| format!("解析 SVG 矢量树失败: {e}"))?;

    let size = tree.size();
    let orig_w = size.width().max(1.0);
    let orig_h = size.height().max(1.0);
    let long_side = orig_w.max(orig_h);
    let scale = (SVG_TARGET_LONG_SIDE as f32 / long_side).clamp(0.25, 16.0);
    let out_w = ((orig_w * scale).round() as u32).clamp(1, SVG_TARGET_LONG_SIDE);
    let out_h = ((orig_h * scale).round() as u32).clamp(1, SVG_TARGET_LONG_SIDE);

    let bgra = render_svg_tree_to_bgra(&tree, out_w, out_h)?;
    let exif = vec![
        ("格式".to_string(), "SVG 矢量图".to_string()),
        (
            "画板尺寸".to_string(),
            format!("{:.0} x {:.0} -> {} x {}", orig_w, orig_h, out_w, out_h),
        ),
    ];

    Ok(DecodedImage {
        path: path.to_path_buf(),
        format_label: "SVG".to_string(),
        file_size_bytes: file_size,
        width: out_w,
        height: out_h,
        has_alpha: true,
        frames: vec![ImageFrame {
            width: out_w,
            height: out_h,
            delay_ms: 0,
            bgra_premul: bgra,
        }],
        total_duration_ms: 0,
        exif_summary: exif,
    })
}

pub fn decode_svg_thumbnail(path: &Path, max_side: u32) -> Result<ThumbnailBitmap, String> {
    let bytes = read_svg_or_svgz_bytes(path)?;
    let opt = resvg::usvg::Options::default();
    let tree = resvg::usvg::Tree::from_data(&bytes, &opt).map_err(|e| format!("{e}"))?;
    let size = tree.size();
    let orig_w = size.width().max(1.0);
    let orig_h = size.height().max(1.0);
    let scale = max_side as f32 / orig_w.max(orig_h);
    let out_w = ((orig_w * scale).round() as u32).max(1);
    let out_h = ((orig_h * scale).round() as u32).max(1);
    let bgra = render_svg_tree_to_bgra(&tree, out_w, out_h)?;
    Ok(ThumbnailBitmap {
        width: out_w,
        height: out_h,
        orig_width: orig_w.round().max(1.0) as u32,
        orig_height: orig_h.round().max(1.0) as u32,
        bgra_premul: bgra,
    })
}

fn render_svg_tree_to_bgra(
    tree: &resvg::usvg::Tree,
    out_w: u32,
    out_h: u32,
) -> Result<Vec<u8>, String> {
    let mut pixmap = resvg::tiny_skia::Pixmap::new(out_w, out_h)
        .ok_or_else(|| "分配 SVG 栅格画布失败".to_string())?;
    let sx = out_w as f32 / tree.size().width().max(1.0);
    let sy = out_h as f32 / tree.size().height().max(1.0);
    let transform = resvg::tiny_skia::Transform::from_scale(sx, sy);
    resvg::render(tree, transform, &mut pixmap.as_mut());

    // tiny-skia 输出的是预乘 RGBA8，只需原地交换 R/B 即得 Direct2D 预乘 BGRA8
    let mut data = pixmap.take();
    for chunk in data.chunks_exact_mut(4) {
        chunk.swap(0, 2);
    }
    Ok(data)
}
