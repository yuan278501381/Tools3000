// 独立图标构建小工具（仅通过 `cargo run --example build_icon` 手动运行，不编入正式二进制）
// 负责将 assets/picasa_viewer_icon.svg 渲染为多分辨率 Windows .ico 与深浅双背景预览图

use std::fs::File;
use std::io::BufWriter;
use std::path::Path;

fn render_svg_rgba(tree: &resvg::usvg::Tree, size: u32) -> Vec<u8> {
    let mut pixmap = resvg::tiny_skia::Pixmap::new(size, size).expect("pixmap");
    let sx = size as f32 / tree.size().width();
    let sy = size as f32 / tree.size().height();
    resvg::render(
        tree,
        resvg::tiny_skia::Transform::from_scale(sx, sy),
        &mut pixmap.as_mut(),
    );
    // tiny-skia 输出预乘 RGBA，反预乘回标准直通 RGBA 写入 PNG
    let mut out = pixmap.take();
    for px in out.chunks_exact_mut(4) {
        let a = px[3];
        if a > 0 && a < 255 {
            let a32 = a as u32;
            px[0] = ((px[0] as u32 * 255 + a32 / 2) / a32).min(255) as u8;
            px[1] = ((px[1] as u32 * 255 + a32 / 2) / a32).min(255) as u8;
            px[2] = ((px[2] as u32 * 255 + a32 / 2) / a32).min(255) as u8;
        }
    }
    out
}

fn encode_png_bytes(rgba: &[u8], w: u32, h: u32) -> Vec<u8> {
    let mut buf = Vec::new();
    {
        let mut enc = png::Encoder::new(&mut buf, w, h);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        let mut writer = enc.write_header().expect("png header");
        writer.write_image_data(rgba).expect("png data");
    }
    buf
}

fn save_png(path: &Path, rgba: &[u8], w: u32, h: u32) {
    let file = File::create(path).expect("create png");
    let mut enc = png::Encoder::new(BufWriter::new(file), w, h);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    let mut writer = enc.write_header().expect("png header");
    writer.write_image_data(rgba).expect("png data");
}

fn write_ico(path: &Path, entries: &[(u32, Vec<u8>)]) {
    // 标准 Windows Vista+ 多尺寸 ICO 格式（内嵌 PNG 帧，全高分屏 16~256px 100% 锐利）
    let count = entries.len() as u16;
    let mut out = Vec::new();
    // ICONDIR (6 bytes)
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes()); // 1 = ICON
    out.extend_from_slice(&count.to_le_bytes());

    let mut offset = 6u32 + (entries.len() as u32) * 16;
    for (sz, png_bytes) in entries {
        let dim = if *sz >= 256 { 0u8 } else { *sz as u8 };
        out.push(dim); // width
        out.push(dim); // height
        out.push(0); // color count
        out.push(0); // reserved
        out.extend_from_slice(&1u16.to_le_bytes()); // planes
        out.extend_from_slice(&32u16.to_le_bytes()); // bpp
        out.extend_from_slice(&(png_bytes.len() as u32).to_le_bytes());
        out.extend_from_slice(&offset.to_le_bytes());
        offset += png_bytes.len() as u32;
    }
    for (_, png_bytes) in entries {
        out.extend_from_slice(png_bytes);
    }
    std::fs::write(path, out).expect("write ico");
}

fn blit_alpha(
    canvas: &mut [u8],
    cw: u32,
    ch: u32,
    src: &[u8],
    sw: u32,
    sh: u32,
    ox: u32,
    oy: u32,
) {
    for y in 0..sh {
        let dy = oy + y;
        if dy >= ch {
            continue;
        }
        for x in 0..sw {
            let dx = ox + x;
            if dx >= cw {
                continue;
            }
            let si = ((y * sw + x) * 4) as usize;
            let di = ((dy * cw + dx) * 4) as usize;
            let sa = src[si + 3] as f32 / 255.0;
            let inv = 1.0 - sa;
            canvas[di] = (src[si] as f32 * sa + canvas[di] as f32 * inv).round() as u8;
            canvas[di + 1] = (src[si + 1] as f32 * sa + canvas[di + 1] as f32 * inv).round() as u8;
            canvas[di + 2] = (src[si + 2] as f32 * sa + canvas[di + 2] as f32 * inv).round() as u8;
            canvas[di + 3] = 255;
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() >= 5 && args[1] == "--compare" {
        // 用法: --compare <out.png> <svg1> <svg2> <svg3>
        let out_png = Path::new(&args[2]);
        let svgs = &args[3..];
        let cols = svgs.len() as u32;
        let col_w = 340u32;
        let cw = cols * col_w;
        let ch = 640u32;
        let mut board = vec![0u8; (cw * ch * 4) as usize];
        for y in 0..ch {
            for x in 0..cw {
                let idx = ((y * cw + x) * 4) as usize;
                let (r, g, b) = if y < ch / 2 {
                    (244u8, 246u8, 250u8) // 上半：浅色任务栏/桌面
                } else {
                    (22u8, 25u8, 34u8) // 下半：深色任务栏/桌面
                };
                board[idx] = r;
                board[idx + 1] = g;
                board[idx + 2] = b;
                board[idx + 3] = 255;
            }
        }
        for (i, svg_p) in svgs.iter().enumerate() {
            let bytes = std::fs::read(svg_p).expect("read compare svg");
            let tree = resvg::usvg::Tree::from_data(&bytes, &resvg::usvg::Options::default())
                .expect("parse compare svg");
            let i200 = render_svg_rgba(&tree, 200);
            let i48 = render_svg_rgba(&tree, 48);
            let i24 = render_svg_rgba(&tree, 24);
            let i16 = render_svg_rgba(&tree, 16);
            let bx = (i as u32) * col_w;
            for row in 0..2u32 {
                let by = row * (ch / 2);
                blit_alpha(&mut board, cw, ch, &i200, 200, 200, bx + 70, by + 24);
                blit_alpha(&mut board, cw, ch, &i48, 48, 48, bx + 78, by + 242);
                blit_alpha(&mut board, cw, ch, &i24, 24, 24, bx + 158, by + 254);
                blit_alpha(&mut board, cw, ch, &i16, 16, 16, bx + 210, by + 258);
            }
        }
        save_png(out_png, &board, cw, ch);
        println!("[OK] Wrote compare board: {}", out_png.display());
        return;
    }

    let manifest_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
    let svg_path = manifest_dir.join("assets").join("picasa_viewer_icon.svg");
    let ico_path = manifest_dir.join("assets").join("picasa_viewer.ico");
    let svg_bytes = std::fs::read(&svg_path).expect("read svg");
    let tree = resvg::usvg::Tree::from_data(&svg_bytes, &resvg::usvg::Options::default())
        .expect("parse svg");

    // 1. 生成 7 档全高分屏 Windows ICO（16/24/32/48/64/128/256px）
    let sizes = [16u32, 24, 32, 48, 64, 128, 256];
    let mut ico_entries = Vec::new();
    for sz in sizes {
        let rgba = render_svg_rgba(&tree, sz);
        ico_entries.push((sz, encode_png_bytes(&rgba, sz, sz)));
    }
    write_ico(&ico_path, &ico_entries);
    println!("[OK] Wrote multi-DPI ICO: {}", ico_path.display());

    // 2. 生成 512px 透明高清主图与深浅双主题全尺寸对比预览板（若传入输出目录）
    if let Some(out_dir_arg) = std::env::args().nth(1) {
        let out_dir = Path::new(&out_dir_arg);
        let _ = std::fs::create_dir_all(out_dir);

        let rgba512 = render_svg_rgba(&tree, 512);
        save_png(&out_dir.join("picasa_viewer_icon_512.png"), &rgba512, 512, 512);

        // 拼接展示图：左深色任务栏/壁纸底，右浅色任务栏/壁纸底，展示 256 / 64 / 32 / 16px 实效
        let cw = 920u32;
        let ch = 460u32;
        let mut board = vec![0u8; (cw * ch * 4) as usize];
        for y in 0..ch {
            for x in 0..cw {
                let idx = ((y * cw + x) * 4) as usize;
                let (r, g, b) = if x < cw / 2 {
                    (20u8, 23u8, 31u8) // 深色系统底
                } else {
                    (242u8, 244u8, 248u8) // 浅色系统底
                };
                board[idx] = r;
                board[idx + 1] = g;
                board[idx + 2] = b;
                board[idx + 3] = 255;
            }
        }
        let icon256 = render_svg_rgba(&tree, 256);
        let icon64 = render_svg_rgba(&tree, 64);
        let icon32 = render_svg_rgba(&tree, 32);
        let icon24 = render_svg_rgba(&tree, 24);
        let icon16 = render_svg_rgba(&tree, 16);

        for half in 0..2u32 {
            let base_x = half * (cw / 2);
            blit_alpha(&mut board, cw, ch, &icon256, 256, 256, base_x + 102, 52);
            blit_alpha(&mut board, cw, ch, &icon64, 64, 64, base_x + 76, 350);
            blit_alpha(&mut board, cw, ch, &icon32, 32, 32, base_x + 178, 366);
            blit_alpha(&mut board, cw, ch, &icon24, 24, 24, base_x + 244, 370);
            blit_alpha(&mut board, cw, ch, &icon16, 16, 16, base_x + 298, 374);
        }
        let preview_path = out_dir.join("picasa_viewer_icon_showcase.png");
        save_png(&preview_path, &board, cw, ch);
        println!("[OK] Wrote icon showcase: {}", preview_path.display());
    }
}
