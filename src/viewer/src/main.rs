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

use crate::assoc_dialog::AssocDialogOutcome;
use crate::i18n::ViewerLanguage;
use std::collections::HashSet;
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
use windows::Win32::UI::WindowsAndMessaging::{
    MessageBoxW, MB_ICONERROR, MB_ICONINFORMATION, MB_OK,
};

fn to_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

fn show_message_box(title: &str, body: &str, is_error: bool) {
    unsafe {
        let wt = to_wide(title);
        let wb = to_wide(body);
        let style = MB_OK | if is_error { MB_ICONERROR } else { MB_ICONINFORMATION };
        let _ = MessageBoxW(
            HWND::default(),
            PCWSTR(wb.as_ptr()),
            PCWSTR(wt.as_ptr()),
            style,
        );
    }
}

/// 由 `constants::SUPPORTED_EXTENSIONS` 单一事实源动态生成文件选择器扩展名过滤器
pub fn build_open_dialog_filter_spec() -> String {
    constants::SUPPORTED_EXTENSIONS
        .iter()
        .map(|ext| format!("*.{ext}"))
        .collect::<Vec<_>>()
        .join(";")
}

/// 弹出原生文件选择器（无命令行参数启动或看图器内按 `O` 键选图，自动跟随系统语言）
pub fn pick_image_via_dialog(owner: HWND, lang: ViewerLanguage) -> Option<PathBuf> {
    let s = lang.strings();
    let filter_spec = build_open_dialog_filter_spec();
    unsafe {
        let com_ok = CoInitializeEx(None, COINIT_APARTMENTTHREADED).is_ok();
        let result = (|| -> Option<PathBuf> {
            let dlg: IFileOpenDialog =
                CoCreateInstance(&FileOpenDialog, None, CLSCTX_INPROC_SERVER).ok()?;
            let name_w = to_wide(s.open_filter_images);
            let spec_w = to_wide(&filter_spec);
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

fn is_known_bool_flag(arg: &str) -> bool {
    arg.eq_ignore_ascii_case("--slideshow")
        || arg.eq_ignore_ascii_case("--hover-close")
        || arg.eq_ignore_ascii_case("--show-info")
        || arg.eq_ignore_ascii_case("--zoom-badge")
        || arg.eq_ignore_ascii_case("--dark")
        || arg.eq_ignore_ascii_case("--config")
        || arg.eq_ignore_ascii_case("-config")
        || arg.eq_ignore_ascii_case("--register")
        || arg.eq_ignore_ascii_case("--register-silent")
        || arg.eq_ignore_ascii_case("--unregister")
        || arg.eq_ignore_ascii_case("--unregister-silent")
}

/// 安全解析命令行中的目标图片路径：
/// 1. 先跳过已知双参/单参 flag；
/// 2. 支持 `--` 选项终止符（后续首个参数无条件视为图片路径）；
/// 3. 对 `-` 开头但非已知 flag 的参数，若磁盘文件真实存在（如 `-photo.png`），直接识别为目标路径！
fn parse_cli_target_path(args: &[String]) -> Option<PathBuf> {
    let mut k = 0;
    let mut after_double_dash = false;
    while k < args.len() {
        let arg = &args[k];
        if after_double_dash {
            return Some(PathBuf::from(arg));
        }
        if arg == "--" {
            after_double_dash = true;
            k += 1;
            continue;
        }
        if arg.eq_ignore_ascii_case("--lang") || arg.eq_ignore_ascii_case("--dpi") {
            k += 2;
            continue;
        }
        if is_known_bool_flag(arg) {
            k += 1;
            continue;
        }
        if !arg.starts_with('-') || Path::new(arg).exists() {
            return Some(PathBuf::from(arg));
        }
        k += 1;
    }
    None
}

/// 无窗口离屏 GPU 帧导出器 (`--dump-ui <img_path> <out_png> [--dpi 192] [--slideshow] [--hover-close] [--zoom-badge]`)
#[allow(clippy::too_many_arguments)]
fn run_offscreen_ui_dump(
    img_path: &Path,
    out_png: &Path,
    lang: ViewerLanguage,
    dpi: f32,
    is_slideshow: bool,
    hover_close: bool,
    show_info: bool,
    show_zoom_badge: bool,
) -> Result<(), String> {
    unsafe {
        let _ = CoInitializeEx(None, COINIT_APARTMENTTHREADED);
    }
    let abs_img = folder::normalize_full_path(img_path);
    let (siblings, current_index) = folder::scan_sibling_images(&abs_img);
    if siblings.is_empty() {
        return Err("No sibling images found".to_string());
    }
    let decoded = Arc::new(
        decode::decode_image_file(&siblings[current_index]).map_err(|e| e.to_string())?,
    );

    let scale = (dpi / 96.0).clamp(1.0, 3.0);
    let view_w = 1280.0f32;
    let view_h = 720.0f32;
    let phys_w = (view_w * scale).round() as u32;
    let phys_h = (view_h * scale).round() as u32;

    let mut renderer = renderer::ViewerRenderer::new_offscreen(phys_w, phys_h, dpi, lang)?;
    let protected: HashSet<&Path> = siblings.iter().take(16).map(|p| p.as_path()).collect();
    for p in siblings.iter().take(16) {
        if let Some(tb) = decode::decode_thumbnail(p, constants::THUMB_TARGET_SIZE) {
            renderer.ensure_thumb_uploaded(p, &tb, &protected);
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
        backdrop_open_progress: 1.0,
        open_anim_armed: true,
        open_progress: 1.0,
        close_progress: 0.0,
        close_snapshot: None,
        slide_progress: 1.0,
        slide_dir: 0.0,
        slide_is_crossfade: false,
        slide_fast_repeat: false,
        cinema_dim: if is_slideshow { 1.0 } else { 0.0 },
        slideshow_portal: None,
        close_hover_anim: if hover_close { 1.0 } else { 0.0 },
        play_hover_anim: 0.0,
        backdrop: types::BackdropMode::PicasaGlass,
        simulated_desktop_bg: Some([0.46, 0.48, 0.52, 1.0]),
        camera: &camera,
        image: Some(&decoded),
        fallback_thumb_path: None,
        frame_index: 0,
        error: None,
        loading_name: None,
        show_loading_box: false,
        current_index,
        sibling_paths: &siblings,
        is_slideshow,
        layout: &layout,
        hovered,
        anim_speed: 1.0,
        selection_box: None,
        toast_text: None,
        show_info,
        zoom_badge_alpha: if show_zoom_badge { 1.0 } else { 0.0 },
    };
    renderer.render(&params).map_err(|e| e.to_string())?;
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
    let mut dump_zoom_badge = false;
    let mut flag_config = false;
    let mut flag_reg_silent = false;
    let mut flag_unreg_silent = false;
    let mut flag_reg = false;
    let mut flag_unreg = false;
    let mut flag_dark = false;

    // 仅扫描 `--` 之前的已知 CLI 选项，避免文件名恰巧与开关同名造成误判
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        if a == "--" {
            break;
        }
        if a.eq_ignore_ascii_case("--lang") && i + 1 < args.len() {
            lang_override = Some(ViewerLanguage::from_tag(&args[i + 1]));
            i += 2;
            continue;
        }
        if a.eq_ignore_ascii_case("--dpi") && i + 1 < args.len() {
            if let Ok(v) = args[i + 1].parse::<f32>() {
                dump_dpi = v.clamp(96.0, 288.0);
            }
            i += 2;
            continue;
        }
        if a.eq_ignore_ascii_case("--slideshow") {
            dump_slideshow = true;
        } else if a.eq_ignore_ascii_case("--hover-close") {
            dump_hover_close = true;
        } else if a.eq_ignore_ascii_case("--show-info") {
            dump_show_info = true;
        } else if a.eq_ignore_ascii_case("--zoom-badge") {
            dump_zoom_badge = true;
        } else if a.eq_ignore_ascii_case("--dark") {
            flag_dark = true;
        } else if a.eq_ignore_ascii_case("--config") || a.eq_ignore_ascii_case("-config") {
            flag_config = true;
        } else if a.eq_ignore_ascii_case("--register-silent") {
            flag_reg_silent = true;
        } else if a.eq_ignore_ascii_case("--unregister-silent") {
            flag_unreg_silent = true;
        } else if a.eq_ignore_ascii_case("--register") {
            flag_reg = true;
        } else if a.eq_ignore_ascii_case("--unregister") {
            flag_unreg = true;
        }
        i += 1;
    }
    let lang = lang_override.unwrap_or_else(ViewerLanguage::detect_system);
    let s = lang.strings();

    if flag_config {
        if let AssocDialogOutcome::Failed(e) =
            assoc_dialog::show_file_association_dialog(HWND::default(), lang)
        {
            show_message_box(s.reg_fail_title, &e, true);
        }
        return;
    }

    if flag_reg_silent {
        match integration::register_user_file_associations() {
            Ok(()) => std::process::exit(0),
            Err(e) => {
                eprintln!("[ERROR] register-silent failed: {e}");
                std::process::exit(1);
            }
        }
    }

    if flag_unreg_silent {
        match integration::unregister_user_file_associations() {
            Ok(()) => std::process::exit(0),
            Err(e) => {
                eprintln!("[ERROR] unregister-silent failed: {e}");
                std::process::exit(1);
            }
        }
    }

    if flag_reg {
        match integration::register_user_file_associations() {
            Ok(()) => show_message_box("Tools3000 Picasa Viewer", s.reg_ok_body, false),
            Err(e) => show_message_box(s.reg_fail_title, &e, true),
        }
        return;
    }

    if flag_unreg {
        match integration::unregister_user_file_associations() {
            Ok(()) => show_message_box("Tools3000 Picasa Viewer", s.unreg_ok_body, false),
            Err(e) => show_message_box(s.unreg_fail_title, &e, true),
        }
        return;
    }

    if args.len() >= 2 && args[0].eq_ignore_ascii_case("--dump-assoc-ui") {
        let out = PathBuf::from(&args[1]);
        match assoc_dialog::dump_assoc_dialog_preview(
            &out,
            lang,
            dump_dpi.round() as u32,
            flag_dark,
        ) {
            Ok(()) => {
                println!("[OK] dumped assoc dialog UI to {}", out.display());
                std::process::exit(0);
            }
            Err(e) => {
                eprintln!("[ERROR] dump-assoc-ui failed: {e}");
                std::process::exit(2);
            }
        }
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
            dump_zoom_badge,
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
        let target = folder::normalize_full_path(Path::new(&args[1]));
        match decode::decode_image_file(&target) {
            Ok(img) => {
                println!(
                    "[OK] decoded={} format={} size={}x{} frames={} duration_ms={} bytes={}",
                    target.display(),
                    img.format_kind.short_tag(),
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

    let picked_path =
        parse_cli_target_path(&args).or_else(|| pick_image_via_dialog(HWND::default(), lang));
    let Some(raw_path) = picked_path else {
        return;
    };
    let abs_path = folder::normalize_full_path(&raw_path);

    // 单实例复用：若已存在打开的看图器窗口，无缝切换目标图片并置前，绝不叠加多层置顶黑幕
    if app::try_forward_to_existing_instance(&abs_path) {
        return;
    }

    if let Err(err) = app::ViewerApp::run(&abs_path, lang_override) {
        show_message_box("Tools3000 Picasa Viewer", &err, true);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn open_dialog_filter_covers_every_supported_extension_ssot() {
        let spec = build_open_dialog_filter_spec();
        for &ext in constants::SUPPORTED_EXTENSIONS {
            let pattern = format!("*.{ext}");
            assert!(
                spec.split(';').any(|p| p == pattern),
                "选图对话框过滤器遗漏扩展名: {pattern}"
            );
        }
    }

    #[test]
    fn cli_parser_supports_double_dash_and_leading_hyphen_existing_file() {
        // 1. `--` 终止符后接 `-` 开头文件名
        let args1 = vec![
            "--lang".to_string(),
            "en-US".to_string(),
            "--".to_string(),
            "-2026-shot.png".to_string(),
        ];
        assert_eq!(
            parse_cli_target_path(&args1),
            Some(PathBuf::from("-2026-shot.png"))
        );

        // 2. 磁盘上真实存在的 `-` 开头文件名（无 `--` 前缀）也能正确识别
        let dir = std::env::temp_dir().join("t3k_picasa_cli_hyphen_test");
        let _ = std::fs::create_dir_all(&dir);
        let dash_file = dir.join("-leading-dash.png");
        std::fs::write(&dash_file, b"stub").unwrap();
        let prev_cwd = std::env::current_dir().unwrap();
        std::env::set_current_dir(&dir).unwrap();
        let args2 = vec!["-leading-dash.png".to_string()];
        let parsed = parse_cli_target_path(&args2);
        std::env::set_current_dir(&prev_cwd).unwrap();
        let _ = std::fs::remove_file(&dash_file);
        assert_eq!(parsed, Some(PathBuf::from("-leading-dash.png")));
    }
}
