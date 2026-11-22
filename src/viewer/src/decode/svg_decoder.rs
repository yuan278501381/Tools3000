// ─────────────────────────────────────────────────────────────────────────────
// decode/svg_decoder.rs — 纯 Rust SVG 高清矢量栅格化解码器 (resvg，防 gzip 炸弹与 OOM)
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{MAX_SVG_DECOMPRESSED_BYTES, SVG_TARGET_LONG_SIDE};
use crate::decode::validate_image_dimensions;
use crate::types::{
    DecodeError, DecodedImage, ExifField, ImageFormatKind, ImageFrame, ThumbnailBitmap,
};
use std::io::Read;
use std::path::Path;

fn read_svg_or_svgz_bytes(path: &Path) -> Result<Vec<u8>, DecodeError> {
    let raw = std::fs::read(path).map_err(|e| DecodeError::ReadFile {
        format: "SVG",
        detail: e.to_string(),
    })?;
    // 自动识别 gzip 压缩的 .svgz（魔数 1F 8B），并限制解压上限防御压缩炸弹 OOM
    if raw.len() >= 2 && raw[0] == 0x1F && raw[1] == 0x8B {
        let decoder = flate2::read::GzDecoder::new(&raw[..]);
        let mut limited = decoder.take((MAX_SVG_DECOMPRESSED_BYTES + 1) as u64);
        let mut out = Vec::new();
        limited
            .read_to_end(&mut out)
            .map_err(|e| DecodeError::SvgParseFailure(e.to_string()))?;
        if out.len() > MAX_SVG_DECOMPRESSED_BYTES {
            return Err(DecodeError::SvgTooLarge);
        }
        return Ok(out);
    }
    if raw.len() > MAX_SVG_DECOMPRESSED_BYTES {
        return Err(DecodeError::SvgTooLarge);
    }
    Ok(raw)
}

pub fn decode_svg(path: &Path, file_size: u64) -> Result<DecodedImage, DecodeError> {
    let bytes = read_svg_or_svgz_bytes(path)?;
    let opt = resvg::usvg::Options::default();
    let tree = resvg::usvg::Tree::from_data(&bytes, &opt)
        .map_err(|e| DecodeError::SvgParseFailure(e.to_string()))?;

    let size = tree.size();
    let orig_w = size.width().max(1.0);
    let orig_h = size.height().max(1.0);
    let long_side = orig_w.max(orig_h);
    let scale = (SVG_TARGET_LONG_SIDE as f32 / long_side).clamp(0.25, 16.0);
    let out_w = ((orig_w * scale).round() as u32).clamp(1, SVG_TARGET_LONG_SIDE);
    let out_h = ((orig_h * scale).round() as u32).clamp(1, SVG_TARGET_LONG_SIDE);

    let bgra = render_svg_tree_to_bgra(&tree, out_w, out_h)?;
    let format_kind = ImageFormatKind::SvgVector;
    let exif = vec![
        ExifField::Format(format_kind.clone()),
        ExifField::Artboard {
            orig_w: orig_w.round().max(1.0) as u32,
            orig_h: orig_h.round().max(1.0) as u32,
            out_w,
            out_h,
        },
    ];

    Ok(DecodedImage {
        path: path.to_path_buf(),
        format_kind,
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

pub fn decode_svg_thumbnail(path: &Path, max_side: u32) -> Result<ThumbnailBitmap, DecodeError> {
    let bytes = read_svg_or_svgz_bytes(path)?;
    let opt = resvg::usvg::Options::default();
    let tree = resvg::usvg::Tree::from_data(&bytes, &opt)
        .map_err(|e| DecodeError::SvgParseFailure(e.to_string()))?;
    let size = tree.size();
    let orig_w = size.width().max(1.0);
    let orig_h = size.height().max(1.0);
    let scale = max_side as f32 / orig_w.max(orig_h);
    let out_w = ((orig_w * scale).round() as u32).clamp(1, max_side.max(1));
    let out_h = ((orig_h * scale).round() as u32).clamp(1, max_side.max(1));
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
) -> Result<Vec<u8>, DecodeError> {
    let _ = validate_image_dimensions(out_w, out_h, "SVG")?;
    let mut pixmap = resvg::tiny_skia::Pixmap::new(out_w, out_h).ok_or(DecodeError::OutOfMemory)?;
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
