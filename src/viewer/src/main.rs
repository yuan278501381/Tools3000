// ─────────────────────────────────────────────────────────────────────────────
// Tools3000_Picasa_Viewer — 致敬经典 Google Picasa Photo Viewer 的极速现代原生看图器
// 纯 Rust + Win32 DirectComposition / Direct2D + 原生 WIC + 8 语种自动跟随系统语言
// 高分屏 (High-DPI 100%~250%+) 原生自适应 · 6 大物理动效 · 0% 静止 CPU 占用
// ─────────────────────────────────────────────────────────────────────────────
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod assoc_dialog;
mod cache;
mod constants;
mod decode;
mod folder;
mod i18n;
mod integration;
mod physics;
mod renderer;
mod types;
mod ui_layout;
mod viewport;

use crate::i18n::ViewerLanguage;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};
use windows::core::PCWSTR;
use windows::Win32::Foundation::HWND;
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CoTaskMemFree, CoUninitialize, CLSCTX_INPROC_SERVER,
    COINIT_APARTMENTTHREADED,
};
use windows::Win32::UI::Shell::Common::COMDLG_FILTERSPEC;
use windows::Win32::UI::Shell::{FileOpenDialog, IFileOpenDialog, SIGDN_FILESYSPATH};
use windows::Win32::UI::WindowsAndMessaging::{MessageBoxW, MB_ICONERROR, MB_ICONINFORMATION, MB_OK};

fn to_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

fn show_message_box(title: &str, body: &str, is_error: bool) {
    unsafe {
        let wt = to_wide(title);
        let wb = to_wide(body);
        let style = MB_OK | if is_error { MB_ICONERROR } else { MB_ICONINFORMATION };
        let _ = MessageBoxW(HWND::default(), PCWSTR(wb.as_ptr()), PCWSTR(wt.as_ptr()), style);
    }
}

/// 弹出原生文件选择器（无命令行参数启动或看图器内按 `O` 键选图，自动跟随系统语言）
pub fn pick_image_via_dialog(owner: HWND, lang: ViewerLanguage) -> Option<PathBuf> {
    let s = lang.strings();
    unsafe {
        let com_ok = CoInitializeEx(None, COINIT_APARTMENTTHREADED).is_ok();
        let result = (|| -> Option<PathBuf> {
            let dlg: IFileOpenDialog =
                CoCreateInstance(&FileOpenDialog, None, CLSCTX_INPROC_SERVER).ok()?;
            let name_w = to_wide(s.open_filter_images);
            let spec_w = to_wide("*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.apng;*.webp;*.gif;*.bmp;*.dib;*.ico;*.cur;*.tif;*.tiff;*.svg;*.svgz;*.tga;*.qoi;*.ppm;*.pgm;*.pbm;*.dds;*.avif;*.heic;*.heif;*.jxr;*.dng;*.cr2;*.cr3;*.nef;*.arw");
            let all_name = to_wide(s.open_filter_all);
            let all_spec = to_wide("*.*");
            let filters = [
                COMDLG_FILTERSPEC {
                    pszName: PCWSTR(name_w.as_ptr()),
                    pszSpec: PCWSTR(spec_w.as_ptr()),
                },
                COMDLG_FILTERSPEC {
                    pszName: PCWSTR(all_name.as_ptr()),
                    pszSpec: PCWSTR(all_spec.as_ptr()),
                },
            ];
            let _ = dlg.SetFileTypes(&filters);
            let title_w = to_wide(s.open_dialog_title);
            let _ = dlg.SetTitle(PCWSTR(title_w.as_ptr()));
            dlg.Show(owner).ok()?;
            let item = dlg.GetResult().ok()?;
            let pwstr = item.GetDisplayName(SIGDN_FILESYSPATH).ok()?;
            let p_str = pwstr.to_string().ok();
            CoTaskMemFree(Some(pwstr.0 as *const _));
            p_str.map(PathBuf::from)
        })();
        if com_ok {
            CoUninitialize();
        }
        result
    }
}

/// 无窗口离屏 GPU 帧导出器 (`--dump-ui <img_path> <out_png> [--dpi 192] [--slideshow] [--hover-close]`)
fn run_offscreen_ui_dump(
    img_path: &Path,
    out_png: &Path,
    lang: ViewerLanguage,
    dpi: f32,
    is_slideshow: bool,
    hover_close: bool,
    show_info: bool,
) -> Result<(), String> {
    unsafe {
        let _ = CoInitializeEx(None, COINIT_APARTMENTTHREADED);
    }
    let (siblings, current_index) = folder::scan_sibling_images(img_path);
    if siblings.is_empty() {
        return Err("No sibling images found".to_string());
    }
    let decoded = Arc::new(decode::decode_image_file(&siblings[current_index])?);

    let scale = (dpi / 96.0).clamp(1.0, 3.0);
    let view_w = 1280.0f32;
    let view_h = 720.0f32;
    let phys_w = (view_w * scale).round() as u32;
    let phys_h = (view_h * scale).round() as u32;

    let mut renderer = renderer::ViewerRenderer::new_offscreen(phys_w, phys_h, dpi, lang)?;
    for p in siblings.iter().take(16) {
        if let Some(tb) = decode::decode_thumbnail(p, constants::THUMB_TARGET_SIZE) {
            renderer.ensure_thumb_uploaded(p, &tb);
        }
    }

    let mut camera = viewport::ViewportCamera::default();
    camera.set_dpi_scale(scale);
    camera.reset_to_fit_with_mode(
        decoded.width as f32,
        decoded.height as f32,
        view_w,
        view_h,
        is_slideshow,
    );

    let layout = ui_layout::ViewerUiLayout::compute(&ui_layout::LayoutInput {
        view_w,
        view_h,
        sibling_count: siblings.len(),
        current_index,
        filmstrip_scroll_offset: 0.0,
        is_animated: decoded.is_animated(),
        anim_playing: false,
        is_slideshow,
        slideshow_paused: false,
        is_actual_zoom: false,
        is_flipped_h: false,
        is_flipped_v: false,
        show_info,
        show_minimap: false,
        img_eff_w: decoded.width as f32,
        img_eff_h: decoded.height as f32,
        exif_line_count: decoded.exif_summary.len() + 2,
    });

    let open_started_at = Instant::now();
    let now = open_started_at + Duration::from_secs(2);
    let hovered = if hover_close {
        ui_layout::HitTarget::ToolbarButton(types::ToolbarAction::CloseViewer)
    } else {
        ui_layout::HitTarget::None
    };

    let params = renderer::RenderFrameParams {
        now,
        open_started_at,
        lang,
        view_w,
        view_h,
        dpi,
        open_progress: 1.0,
        close_progress: 0.0,
        close_snapshot: None,
        slide_progress: 1.0,
        slide_dir: 0.0,
        slide_is_crossfade: false,
        slide_fast_repeat: false,
        cinema_dim: if is_slideshow { 1.0 } else { 0.0 },
        ken_burns: if is_slideshow {
            Some(physics::KenBurnsTrack::for_slide_index(current_index, 4.4))
        } else {
            None
        },
        close_hover_anim: if hover_close { 1.0 } else { 0.0 },
        play_hover_anim: 0.0,
        backdrop: types::BackdropMode::PicasaGlass,
        simulated_desktop_bg: Some([0.46, 0.48, 0.52, 1.0]),
        camera: &camera,
        image: Some(&decoded),
        fallback_thumb_path: None,
        frame_index: 0,
        error_text: None,
        loading_name: None,
        show_loading_box: false,
        current_index,
        sibling_paths: &siblings,
        is_slideshow,
        slideshow_paused: false,
        layout: &layout,
        ui_opacity: 1.0,
        hovered,
        anim_speed: 1.0,
        selection_box: None,
        toast_text: None,
        show_info,
    };
    renderer.render(&params)?;
    let frame = renderer.readback_offscreen_frame()?;
    integration::save_premul_bgra_as_png(&frame, out_png)?;
    Ok(())
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();

    let mut lang_override: Option<ViewerLanguage> = None;
    let mut dump_dpi: f32 = 96.0;
    let mut dump_slideshow = false;
    let mut dump_hover_close = false;
    let mut dump_show_info = false;
    let mut i = 0;
    while i < args.len() {
        if args[i].eq_ignore_ascii_case("--lang") && i + 1 < args.len() {
            lang_override = Some(ViewerLanguage::from_tag(&args[i + 1]));
            i += 2;
            continue;
        }
        if args[i].eq_ignore_ascii_case("--dpi") && i + 1 < args.len() {
            if let Ok(v) = args[i + 1].parse::<f32>() {
                dump_dpi = v.clamp(96.0, 288.0);
            }
            i += 2;
            continue;
        }
        if args[i].eq_ignore_ascii_case("--slideshow") {
            dump_slideshow = true;
        }
        if args[i].eq_ignore_ascii_case("--hover-close") {
            dump_hover_close = true;
        }
        if args[i].eq_ignore_ascii_case("--show-info") {
            dump_show_info = true;
        }
        i += 1;
    }
    let lang = lang_override.unwrap_or_else(ViewerLanguage::detect_system);
    let s = lang.strings();

    if args
        .iter()
        .any(|a| a.eq_ignore_ascii_case("--config") || a.eq_ignore_ascii_case("-config"))
    {
        let _ = assoc_dialog::show_file_association_dialog(HWND::default(), lang);
        return;
    }

    if args.iter().any(|a| a.eq_ignore_ascii_case("--register")) {
        match integration::register_user_file_associations() {
            Ok(()) => show_message_box("Tools3000 Picasa Viewer", s.reg_ok_body, false),
            Err(e) => show_message_box(s.reg_fail_title, &e, true),
        }
        return;
    }

    if args.iter().any(|a| a.eq_ignore_ascii_case("--unregister")) {
        match integration::unregister_user_file_associations() {
            Ok(()) => show_message_box("Tools3000 Picasa Viewer", s.unreg_ok_body, false),
            Err(e) => show_message_box(s.unreg_fail_title, &e, true),
        }
        return;
    }

    if args.len() >= 3 && args[0].eq_ignore_ascii_case("--dump-ui") {
        let img = PathBuf::from(&args[1]);
        let out = PathBuf::from(&args[2]);
        match run_offscreen_ui_dump(
            &img,
            &out,
            lang,
            dump_dpi,
            dump_slideshow,
            dump_hover_close,
            dump_show_info,
        ) {
            Ok(()) => {
                println!("[OK] dumped UI frame to {}", out.display());
                std::process::exit(0);
            }
            Err(e) => {
                eprintln!("[ERROR] dump-ui failed: {e}");
                std::process::exit(2);
            }
        }
    }

    if args.len() >= 2 && args[0].eq_ignore_ascii_case("--self-test-decode") {
        let target = PathBuf::from(&args[1]);
        match decode::decode_image_file(&target) {
            Ok(img) => {
                println!(
                    "[OK] decoded={} format={} size={}x{} frames={} duration_ms={} bytes={}",
                    target.display(),
                    img.format_label,
                    img.width,
                    img.height,
                    img.frames.len(),
                    img.total_duration_ms,
                    img.memory_bytes()
                );
                std::process::exit(0);
            }
            Err(e) => {
                eprintln!("[ERROR] decode failed: {e}");
                std::process::exit(2);
            }
        }
    }

    let mut non_flag_path: Option<PathBuf> = None;
    let mut k = 0;
    while k < args.len() {
        if args[k].eq_ignore_ascii_case("--lang") || args[k].eq_ignore_ascii_case("--dpi") {
            k += 2;
            continue;
        }
        if !args[k].starts_with('-') {
            non_flag_path = Some(PathBuf::from(&args[k]));
            break;
        }
        k += 1;
    }

    let Some(path) = non_flag_path.or_else(|| pick_image_via_dialog(HWND::default(), lang)) else {
        return;
    };

    if let Err(err) = app::ViewerApp::run(&path, lang_override) {
        show_message_box("Tools3000 Picasa Viewer", &err, true);
    }
}
