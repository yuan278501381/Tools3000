// ─────────────────────────────────────────────────────────────────────────────
// integration.rs — 独立看图器系统集成模块（100% 与外部宿主解耦）
// 包含：剪贴板(图像含当前旋转+文件/路径)、回收站安全删除、导出当前帧 PNG(含当前旋转)、
//       设为壁纸、原生 8 语种右键菜单、Picasa 拍立得文件关联图标注册与默认应用唤起
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{APP_PROG_ID, SUPPORTED_EXTENSIONS};
use crate::i18n::ViewerLanguage;
use crate::types::{ImageFrame, RotationQuarter, ToolbarAction};
use std::borrow::Cow;
use std::collections::HashSet;
use std::ffi::OsStr;
use std::fs::File;
use std::io::BufWriter;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use windows::core::PCWSTR;
use windows::Win32::Foundation::{GlobalFree, HGLOBAL, HWND};
use windows::Win32::Graphics::Gdi::{BI_BITFIELDS, BITMAPV5HEADER};
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CoTaskMemFree, CoUninitialize, CLSCTX_INPROC_SERVER,
    COINIT_APARTMENTTHREADED,
};
use windows::Win32::System::DataExchange::{
    CloseClipboard, EmptyClipboard, OpenClipboard, SetClipboardData,
};
use windows::Win32::System::Memory::{GlobalAlloc, GlobalLock, GlobalUnlock, GMEM_MOVEABLE};
use windows::Win32::System::Ole::{CF_DIBV5, CF_HDROP, CF_UNICODETEXT};
use windows::Win32::System::Registry::{
    RegCloseKey, RegCreateKeyExW, RegDeleteTreeW, RegDeleteValueW, RegOpenKeyExW,
    RegQueryValueExW, RegSetValueExW, HKEY, HKEY_CURRENT_USER, KEY_READ, KEY_WRITE,
    REG_OPTION_NON_VOLATILE, REG_SZ,
};
use windows::Win32::UI::Shell::Common::COMDLG_FILTERSPEC;
use windows::Win32::UI::Shell::{
    FileSaveDialog, IFileSaveDialog, SHChangeNotify, SHFileOperationW, ShellExecuteW, DROPFILES,
    FOF_ALLOWUNDO, FOF_NOCONFIRMATION, FOF_NOERRORUI, FOF_SILENT, FO_DELETE, SHCNE_ASSOCCHANGED,
    SHCNF_IDLIST, SHFILEOPSTRUCTW, SIGDN_FILESYSPATH,
};
use windows::Win32::UI::WindowsAndMessaging::{
    AppendMenuW, CreatePopupMenu, DestroyMenu, SetForegroundWindow, SystemParametersInfoW,
    TrackPopupMenuEx, MF_CHECKED, MF_SEPARATOR, MF_STRING, MF_UNCHECKED, SPIF_SENDCHANGE,
    SPIF_UPDATEINIFILE, SPI_SETDESKWALLPAPER, SW_SHOWNORMAL, TPM_LEFTALIGN, TPM_RETURNCMD,
    TPM_RIGHTBUTTON,
};

const LCS_SRGB: u32 = 0x7352_4742;

fn to_wide_null(s: &OsStr) -> Vec<u16> {
    s.encode_wide().chain(Some(0)).collect()
}

fn str_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

/// 按当前看图局部镜像翻转 (`flip_x` / `flip_y`) 与旋转方向（0°/90°/180°/270°）无损变换帧像素，
/// 严格与 Direct2D 渲染矩阵 `scale(flip_x, flip_y) * rotation(deg)` 对齐
pub fn apply_transform_to_frame<'a>(
    frame: &'a ImageFrame,
    rotation: RotationQuarter,
    flip_x: bool,
    flip_y: bool,
) -> Cow<'a, ImageFrame> {
    let w = frame.width as usize;
    let h = frame.height as usize;
    if w == 0 || h == 0 || (rotation == RotationQuarter::Deg0 && !flip_x && !flip_y) {
        return Cow::Borrowed(frame);
    }

    let (nw, nh) = if rotation.swaps_axes() { (h, w) } else { (w, h) };
    let mut dst = vec![0u8; nw * nh * 4];
    let src = &frame.bgra_premul;

    for y in 0..h {
        let ly = if flip_y { h - 1 - y } else { y };
        for x in 0..w {
            let lx = if flip_x { w - 1 - x } else { x };
            let (dx, dy) = match rotation {
                RotationQuarter::Deg0 => (lx, ly),
                RotationQuarter::Deg90 => (h - 1 - ly, lx),
                RotationQuarter::Deg180 => (w - 1 - lx, h - 1 - ly),
                RotationQuarter::Deg270 => (ly, w - 1 - lx),
            };
            let s_off = (y * w + x) * 4;
            let d_off = (dy * nw + dx) * 4;
            dst[d_off..d_off + 4].copy_from_slice(&src[s_off..s_off + 4]);
        }
    }

    Cow::Owned(ImageFrame {
        width: nw as u32,
        height: nh as u32,
        delay_ms: frame.delay_ms,
        bgra_premul: dst,
    })
}

/// 复制当前帧像素 (CF_DIBV5 32位含透明与当前旋转/翻转) 与原始文件路径 (CF_HDROP) 至系统剪贴板
pub fn copy_frame_and_file_to_clipboard(
    hwnd: HWND,
    raw_frame: &ImageFrame,
    rotation: RotationQuarter,
    flip_x: bool,
    flip_y: bool,
    path: &Path,
) -> bool {
    let oriented = apply_transform_to_frame(raw_frame, rotation, flip_x, flip_y);
    let frame = oriented.as_ref();
    unsafe {
        if OpenClipboard(hwnd).is_err() {
            return false;
        }
        let _ = EmptyClipboard();
        let mut ok = false;

        let header_size = std::mem::size_of::<BITMAPV5HEADER>();
        let pixel_bytes = (frame.width as usize) * (frame.height as usize) * 4;
        if let Ok(hmem) = GlobalAlloc(GMEM_MOVEABLE, header_size + pixel_bytes) {
            let ptr = GlobalLock(hmem) as *mut u8;
            if !ptr.is_null() {
                let mut hdr = BITMAPV5HEADER::default();
                hdr.bV5Size = header_size as u32;
                hdr.bV5Width = frame.width as i32;
                hdr.bV5Height = -(frame.height as i32);
                hdr.bV5Planes = 1;
                hdr.bV5BitCount = 32;
                hdr.bV5Compression = BI_BITFIELDS;
                hdr.bV5SizeImage = pixel_bytes as u32;
                hdr.bV5RedMask = 0x00FF_0000;
                hdr.bV5GreenMask = 0x0000_FF00;
                hdr.bV5BlueMask = 0x0000_00FF;
                hdr.bV5AlphaMask = 0xFF00_0000;
                hdr.bV5CSType = LCS_SRGB;
                std::ptr::copy_nonoverlapping(
                    &hdr as *const BITMAPV5HEADER as *const u8,
                    ptr,
                    header_size,
                );
                let dst = std::slice::from_raw_parts_mut(ptr.add(header_size), pixel_bytes);
                for (s, d) in frame
                    .bgra_premul
                    .chunks_exact(4)
                    .zip(dst.chunks_exact_mut(4))
                {
                    let a = s[3];
                    if a == 0 {
                        d.copy_from_slice(&[0, 0, 0, 0]);
                    } else if a == 255 {
                        d.copy_from_slice(s);
                    } else {
                        let a32 = a as u32;
                        d[0] = ((s[0] as u32 * 255 + a32 / 2) / a32).min(255) as u8;
                        d[1] = ((s[1] as u32 * 255 + a32 / 2) / a32).min(255) as u8;
                        d[2] = ((s[2] as u32 * 255 + a32 / 2) / a32).min(255) as u8;
                        d[3] = a;
                    }
                }
                let _ = GlobalUnlock(hmem);
                if SetClipboardData(CF_DIBV5.0 as u32, windows::Win32::Foundation::HANDLE(hmem.0))
                    .is_ok()
                {
                    ok = true;
                } else {
                    let _ = GlobalFree(HGLOBAL(hmem.0));
                }
            } else {
                let _ = GlobalFree(HGLOBAL(hmem.0));
            }
        }

        if rotation == RotationQuarter::Deg0 && !flip_x && !flip_y && path.exists() {
            let wide_path: Vec<u16> = path
                .as_os_str()
                .encode_wide()
                .chain([0u16, 0u16])
                .collect();
            let drop_hdr_size = std::mem::size_of::<DROPFILES>();
            let total_drop = drop_hdr_size + wide_path.len() * 2;
            if let Ok(hdrop) = GlobalAlloc(GMEM_MOVEABLE, total_drop) {
                let ptr = GlobalLock(hdrop) as *mut u8;
                if !ptr.is_null() {
                    let df = DROPFILES {
                        pFiles: drop_hdr_size as u32,
                        pt: Default::default(),
                        fNC: false.into(),
                        fWide: true.into(),
                    };
                    std::ptr::copy_nonoverlapping(
                        &df as *const DROPFILES as *const u8,
                        ptr,
                        drop_hdr_size,
                    );
                    std::ptr::copy_nonoverlapping(
                        wide_path.as_ptr() as *const u8,
                        ptr.add(drop_hdr_size),
                        wide_path.len() * 2,
                    );
                    let _ = GlobalUnlock(hdrop);
                    if SetClipboardData(
                        CF_HDROP.0 as u32,
                        windows::Win32::Foundation::HANDLE(hdrop.0),
                    )
                    .is_err()
                    {
                        let _ = GlobalFree(HGLOBAL(hdrop.0));
                    }
                } else {
                    let _ = GlobalFree(HGLOBAL(hdrop.0));
                }
            }
        }

        let _ = CloseClipboard();
        ok
    }
}

/// 仅复制图片完整路径文本至剪贴板
pub fn copy_path_to_clipboard(hwnd: HWND, path: &Path) -> bool {
    let wide = to_wide_null(path.as_os_str());
    let byte_len = wide.len() * 2;
    unsafe {
        if OpenClipboard(hwnd).is_err() {
            return false;
        }
        let _ = EmptyClipboard();
        let mut ok = false;
        if let Ok(hmem) = GlobalAlloc(GMEM_MOVEABLE, byte_len) {
            let ptr = GlobalLock(hmem) as *mut u8;
            if !ptr.is_null() {
                std::ptr::copy_nonoverlapping(wide.as_ptr() as *const u8, ptr, byte_len);
                let _ = GlobalUnlock(hmem);
                if SetClipboardData(
                    CF_UNICODETEXT.0 as u32,
                    windows::Win32::Foundation::HANDLE(hmem.0),
                )
                .is_ok()
                {
                    ok = true;
                } else {
                    let _ = GlobalFree(HGLOBAL(hmem.0));
                }
            } else {
                let _ = GlobalFree(HGLOBAL(hmem.0));
            }
        }
        let _ = CloseClipboard();
        ok
    }
}

/// 弹出原生保存对话框，将当前画面（含当前旋转角度与翻转镜像）无损保存为 PNG
#[allow(clippy::too_many_arguments)]
pub fn save_frame_as_png_dialog(
    hwnd: HWND,
    raw_frame: &ImageFrame,
    rotation: RotationQuarter,
    flip_x: bool,
    flip_y: bool,
    source_path: &Path,
    frame_idx: usize,
    is_anim: bool,
    lang: ViewerLanguage,
) -> Result<Option<PathBuf>, String> {
    let s = lang.strings();
    let default_stem = source_path
        .file_stem()
        .and_then(|st| st.to_str())
        .unwrap_or("image");
    let default_name = if is_anim {
        format!("{default_stem}_frame{}.png", frame_idx + 1)
    } else {
        format!("{default_stem}.png")
    };

    let target = unsafe {
        let com_ok = CoInitializeEx(None, COINIT_APARTMENTTHREADED).is_ok();
        let picked = (|| -> Option<PathBuf> {
            let dlg: IFileSaveDialog =
                CoCreateInstance(&FileSaveDialog, None, CLSCTX_INPROC_SERVER).ok()?;
            let name_w = str_wide(s.save_png_filter);
            let spec_w = str_wide("*.png");
            let filters = [COMDLG_FILTERSPEC {
                pszName: PCWSTR(name_w.as_ptr()),
                pszSpec: PCWSTR(spec_w.as_ptr()),
            }];
            let _ = dlg.SetFileTypes(&filters);
            let ext_w = str_wide("png");
            let _ = dlg.SetDefaultExtension(PCWSTR(ext_w.as_ptr()));
            let fn_w = str_wide(&default_name);
            let _ = dlg.SetFileName(PCWSTR(fn_w.as_ptr()));
            let title_w = str_wide(s.save_png_title);
            let _ = dlg.SetTitle(PCWSTR(title_w.as_ptr()));
            dlg.Show(hwnd).ok()?;
            let item = dlg.GetResult().ok()?;
            let pwstr = item.GetDisplayName(SIGDN_FILESYSPATH).ok()?;
            let p_str = pwstr.to_string().ok();
            CoTaskMemFree(Some(pwstr.0 as *const _));
            p_str.map(PathBuf::from)
        })();
        if com_ok {
            CoUninitialize();
        }
        picked
    };

    let Some(out_path) = target else {
        return Ok(None);
    };

    let oriented = apply_transform_to_frame(raw_frame, rotation, flip_x, flip_y);
    save_premul_bgra_as_png(&oriented, &out_path)?;
    Ok(Some(out_path))
}

/// 将预乘 BGRA8 缓冲无损编码保存为 PNG 文件（供导出功能与 `--dump-ui` 离线自检共用）
pub fn save_premul_bgra_as_png(frame: &ImageFrame, out_path: &Path) -> Result<(), String> {
    let mut rgba = vec![0u8; (frame.width as usize) * (frame.height as usize) * 4];
    for (s, d) in frame.bgra_premul.chunks_exact(4).zip(rgba.chunks_exact_mut(4)) {
        let a = s[3];
        if a == 0 {
            d.copy_from_slice(&[0, 0, 0, 0]);
        } else if a == 255 {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = 255;
        } else {
            let a32 = a as u32;
            d[0] = ((s[2] as u32 * 255 + a32 / 2) / a32).min(255) as u8;
            d[1] = ((s[1] as u32 * 255 + a32 / 2) / a32).min(255) as u8;
            d[2] = ((s[0] as u32 * 255 + a32 / 2) / a32).min(255) as u8;
            d[3] = a;
        }
    }

    let file = File::create(out_path).map_err(|e| format!("{e}"))?;
    let mut enc = png::Encoder::new(BufWriter::new(file), frame.width, frame.height);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    let mut writer = enc.write_header().map_err(|e| format!("{e}"))?;
    writer
        .write_image_data(&rgba)
        .map_err(|e| format!("{e}"))?;
    Ok(())
}

/// 设为 Windows 桌面壁纸
pub fn set_as_desktop_wallpaper(path: &Path) -> bool {
    let wide = to_wide_null(path.as_os_str());
    unsafe {
        SystemParametersInfoW(
            SPI_SETDESKWALLPAPER,
            0,
            Some(wide.as_ptr() as *mut _),
            SPIF_UPDATEINIFILE | SPIF_SENDCHANGE,
        )
        .is_ok()
    }
}

/// 唤起 Windows 原生「打开方式」对话框
pub fn open_with_system_dialog(path: &Path) {
    let arg = format!("shell32.dll,OpenAs_RunDLL {}", path.display());
    let _ = Command::new("rundll32.exe").arg(arg).spawn();
}

/// 唤起 Windows 10/11 原生「默认应用 -> Tools3000 Picasa Viewer」专属设置页，方便用户一键确认系统默认打开
pub fn open_windows_default_apps_settings(hwnd: HWND) {
    unsafe {
        let op = str_wide("open");
        let uri_specific =
            str_wide("ms-settings:defaultapps?registeredAppUser=Tools3000%20Picasa%20Viewer");
        let res = ShellExecuteW(
            hwnd,
            PCWSTR(op.as_ptr()),
            PCWSTR(uri_specific.as_ptr()),
            PCWSTR::null(),
            PCWSTR::null(),
            SW_SHOWNORMAL,
        );
        if (res.0 as isize) <= 32 {
            let uri_fallback = str_wide("ms-settings:defaultapps");
            let _ = ShellExecuteW(
                hwnd,
                PCWSTR(op.as_ptr()),
                PCWSTR(uri_fallback.as_ptr()),
                PCWSTR::null(),
                PCWSTR::null(),
                SW_SHOWNORMAL,
            );
        }
    }
}

/// 将图片安全移入 Windows 回收站（可撤销恢复）
pub fn move_to_recycle_bin(hwnd: HWND, path: &Path) -> bool {
    let double_null: Vec<u16> = path
        .as_os_str()
        .encode_wide()
        .chain([0u16, 0u16])
        .collect();
    let mut op = SHFILEOPSTRUCTW {
        hwnd,
        wFunc: FO_DELETE,
        pFrom: PCWSTR(double_null.as_ptr()),
        pTo: PCWSTR::null(),
        fFlags: (FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT).0 as u16,
        fAnyOperationsAborted: false.into(),
        hNameMappings: std::ptr::null_mut(),
        lpszProgressTitle: PCWSTR::null(),
    };
    let ret = unsafe { SHFileOperationW(&mut op) };
    ret == 0 && !op.fAnyOperationsAborted.as_bool()
}

/// 在 Windows 资源管理器中打开并精准选中该图片文件
pub fn reveal_in_explorer(path: &Path) {
    let arg = format!("/select,\"{}\"", path.display());
    let _ = Command::new("explorer.exe").arg(arg).spawn();
}

/// 原生 Win32 上下文菜单状态快照
pub struct ContextMenuState {
    pub is_slideshow: bool,
    pub is_fullscreen: bool,
    pub show_info: bool,
    pub is_animated: bool,
    pub flipped_h: bool,
    pub flipped_v: bool,
    pub lang: ViewerLanguage,
}

/// 在鼠标物理屏幕坐标 (screen_x, screen_y) 弹出自动跟随系统语言的纯粹看图上下文菜单
pub fn show_native_context_menu(
    hwnd: HWND,
    screen_x: i32,
    screen_y: i32,
    st: &ContextMenuState,
) -> Option<ToolbarAction> {
    const CMD_OPEN_FILE: usize = 1020;
    const CMD_ACTUAL: usize = 1001;
    const CMD_FIT_WIDTH: usize = 1018;
    const CMD_SLIDESHOW: usize = 1002;
    const CMD_FULLSCREEN: usize = 1003;
    const CMD_ROT_CW: usize = 1004;
    const CMD_ROT_CCW: usize = 1005;
    const CMD_FLIP_H: usize = 1021;
    const CMD_FLIP_V: usize = 1022;
    const CMD_COPY_IMG: usize = 1007;
    const CMD_COPY_PATH: usize = 1008;
    const CMD_SAVE_PNG: usize = 1009;
    const CMD_WALLPAPER: usize = 1010;
    const CMD_BACKDROP: usize = 1011;
    const CMD_INFO: usize = 1012;
    const CMD_REVEAL: usize = 1013;
    const CMD_OPEN_WITH: usize = 1014;
    const CMD_REGISTER: usize = 1015;
    const CMD_DELETE: usize = 1016;
    const CMD_CLOSE: usize = 1017;

    let s = st.lang.strings();

    unsafe {
        let Ok(hmenu) = CreatePopupMenu() else {
            return None;
        };
        let add_item = |id: usize, text: &str, checked: bool| {
            let w = str_wide(text);
            let flags = MF_STRING | if checked { MF_CHECKED } else { MF_UNCHECKED };
            let _ = AppendMenuW(hmenu, flags, id, PCWSTR(w.as_ptr()));
        };
        let add_sep = || {
            let _ = AppendMenuW(hmenu, MF_SEPARATOR, 0, PCWSTR::null());
        };

        add_item(CMD_OPEN_FILE, s.menu_open_file, false);
        add_sep();
        add_item(CMD_ACTUAL, s.menu_actual_or_fit, false);
        add_item(CMD_FIT_WIDTH, s.menu_fit_width, false);
        add_item(
            CMD_SLIDESHOW,
            if st.is_slideshow {
                s.menu_stop_slideshow
            } else {
                s.menu_play_slideshow
            },
            st.is_slideshow,
        );
        add_item(CMD_FULLSCREEN, s.menu_fullscreen_overlay, st.is_fullscreen);
        add_sep();
        add_item(CMD_ROT_CW, s.menu_rotate_cw, false);
        add_item(CMD_ROT_CCW, s.menu_rotate_ccw, false);
        add_item(CMD_FLIP_H, s.menu_flip_h, st.flipped_h);
        add_item(CMD_FLIP_V, s.menu_flip_v, st.flipped_v);
        add_sep();
        add_item(CMD_COPY_IMG, s.menu_copy_image, false);
        add_item(CMD_COPY_PATH, s.menu_copy_path, false);
        add_item(
            CMD_SAVE_PNG,
            if st.is_animated {
                s.menu_save_frame_png
            } else {
                s.menu_save_image_png
            },
            false,
        );
        add_item(CMD_WALLPAPER, s.menu_set_wallpaper, false);
        add_sep();
        add_item(CMD_BACKDROP, s.menu_cycle_backdrop, false);
        add_item(CMD_INFO, s.menu_image_info, st.show_info);
        add_item(CMD_REVEAL, s.menu_reveal_explorer, false);
        add_item(CMD_OPEN_WITH, s.menu_open_with, false);
        add_item(CMD_REGISTER, s.menu_register_assoc, false);
        add_sep();
        add_item(CMD_DELETE, s.menu_delete_recycle, false);
        add_item(CMD_CLOSE, s.menu_close, false);

        let _ = SetForegroundWindow(hwnd);
        let cmd = TrackPopupMenuEx(
            hmenu,
            (TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD).0,
            screen_x,
            screen_y,
            hwnd,
            None,
        );
        let _ = DestroyMenu(hmenu);

        match cmd.0 as usize {
            CMD_OPEN_FILE => Some(ToolbarAction::OpenImageFile),
            CMD_ACTUAL => Some(ToolbarAction::ToggleActualSize),
            CMD_FIT_WIDTH => Some(ToolbarAction::FitToWidth),
            CMD_SLIDESHOW => Some(ToolbarAction::ToggleSlideshow),
            CMD_FULLSCREEN => Some(ToolbarAction::ToggleFullscreen),
            CMD_ROT_CW => Some(ToolbarAction::RotateCw),
            CMD_ROT_CCW => Some(ToolbarAction::RotateCcw),
            CMD_FLIP_H => Some(ToolbarAction::FlipHorizontal),
            CMD_FLIP_V => Some(ToolbarAction::FlipVertical),
            CMD_COPY_IMG => Some(ToolbarAction::CopyImage),
            CMD_COPY_PATH => Some(ToolbarAction::CopyPath),
            CMD_SAVE_PNG => Some(ToolbarAction::SaveCurrentFramePng),
            CMD_WALLPAPER => Some(ToolbarAction::SetAsWallpaper),
            CMD_BACKDROP => Some(ToolbarAction::CycleBackdrop),
            CMD_INFO => Some(ToolbarAction::ToggleInfo),
            CMD_REVEAL => Some(ToolbarAction::RevealInExplorer),
            CMD_OPEN_WITH => Some(ToolbarAction::OpenWithDialog),
            CMD_REGISTER => Some(ToolbarAction::RegisterDefaultApp),
            CMD_DELETE => Some(ToolbarAction::DeleteToRecycleBin),
            CMD_CLOSE => Some(ToolbarAction::CloseViewer),
            _ => None,
        }
    }
}

fn reg_write_sz(subkey: &str, value_name: Option<&str>, data: &str) -> Result<(), String> {
    unsafe {
        let wide_sub = to_wide_null(OsStr::new(subkey));
        let mut hkey = HKEY::default();
        let status = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            PCWSTR(wide_sub.as_ptr()),
            0,
            PCWSTR::null(),
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE,
            None,
            &mut hkey,
            None,
        );
        if status.is_err() {
            return Err(format!("RegCreateKeyExW failed: {subkey}"));
        }
        let wide_val = to_wide_null(OsStr::new(data));
        let bytes =
            std::slice::from_raw_parts(wide_val.as_ptr() as *const u8, wide_val.len() * 2);
        let wide_name = value_name.map(|n| to_wide_null(OsStr::new(n)));
        let name_ptr = wide_name
            .as_ref()
            .map(|v| PCWSTR(v.as_ptr()))
            .unwrap_or(PCWSTR::null());
        let res = RegSetValueExW(hkey, name_ptr, 0, REG_SZ, Some(bytes));
        let _ = RegCloseKey(hkey);
        if res.is_err() {
            return Err(format!("RegSetValueExW failed: {subkey}"));
        }
        Ok(())
    }
}

fn reg_read_sz(subkey: &str, value_name: Option<&str>) -> Option<String> {
    unsafe {
        let wide_sub = to_wide_null(OsStr::new(subkey));
        let mut hkey = HKEY::default();
        if RegOpenKeyExW(
            HKEY_CURRENT_USER,
            PCWSTR(wide_sub.as_ptr()),
            0,
            KEY_READ,
            &mut hkey,
        )
        .is_err()
        {
            return None;
        }
        let wide_name = value_name.map(|n| to_wide_null(OsStr::new(n)));
        let name_ptr = wide_name
            .as_ref()
            .map(|v| PCWSTR(v.as_ptr()))
            .unwrap_or(PCWSTR::null());
        let mut buf = [0u16; 260];
        let mut byte_len = (buf.len() * 2) as u32;
        let ok = RegQueryValueExW(
            hkey,
            name_ptr,
            None,
            None,
            Some(buf.as_mut_ptr() as *mut u8),
            Some(&mut byte_len),
        )
        .is_ok();
        let _ = RegCloseKey(hkey);
        if !ok || byte_len < 2 {
            return None;
        }
        let u16_len = (byte_len as usize / 2).min(buf.len());
        let end = buf[..u16_len]
            .iter()
            .position(|&c| c == 0)
            .unwrap_or(u16_len);
        Some(String::from_utf16_lossy(&buf[..end]))
    }
}

fn reg_delete_value(subkey: &str, value_name: &str) {
    unsafe {
        let wide_sub = to_wide_null(OsStr::new(subkey));
        let mut hkey = HKEY::default();
        if RegOpenKeyExW(
            HKEY_CURRENT_USER,
            PCWSTR(wide_sub.as_ptr()),
            0,
            KEY_WRITE,
            &mut hkey,
        )
        .is_ok()
        {
            let wide_name = to_wide_null(OsStr::new(value_name));
            let _ = RegDeleteValueW(hkey, PCWSTR(wide_name.as_ptr()));
            let _ = RegCloseKey(hkey);
        }
    }
}

/// 查询当前用户已经在本软件注册关联的扩展名集合（不含前导点，小写）
pub fn query_registered_extensions() -> HashSet<String> {
    let mut set = HashSet::new();
    let assoc_key = "Software\\Tools3000\\PicasaViewer\\Capabilities\\FileAssociations";
    for &ext in SUPPORTED_EXTENSIONS {
        let dot_ext = format!(".{ext}");
        if let Some(val) = reg_read_sz(assoc_key, Some(&dot_ext)) {
            if val == APP_PROG_ID {
                set.insert(ext.to_string());
            }
        }
    }
    set
}

/// 注册全部支持的图片格式关联
pub fn register_user_file_associations() -> Result<(), String> {
    register_selected_file_associations(SUPPORTED_EXTENSIONS)
}

/// 为用户勾选的图片扩展名写入关联，并将关联后的图片文件图标设为 **Picasa 经典拍立得相纸图标 (`"{exe}",1`)**
pub fn register_selected_file_associations(selected_exts: &[&str]) -> Result<(), String> {
    let exe = std::env::current_exe().map_err(|e| format!("{e}"))?;
    let exe_str = exe.display().to_string();
    let open_cmd = format!("\"{}\" \"%1\"", exe_str);
    // 索引 0 = 应用主程序图标 (雕塑玻璃彩色快门圆轮)
    let app_icon_val = format!("\"{}\",0", exe_str);
    // 索引 1 = 图片文件关联文档图标 (经典 Picasa 拍立得青松金丘相纸图标！对应 viewer.rc ID = 2)
    let doc_icon_val = format!("\"{}\",1", exe_str);

    // 1. 图片文档 ProgID：使所有关联的图片文件显示经典 Picasa 拍立得相纸图标！
    let prog_root = format!("Software\\Classes\\{}", APP_PROG_ID);
    reg_write_sz(&prog_root, None, "Picasa Photo Viewer Image")?;
    reg_write_sz(&format!("{prog_root}\\DefaultIcon"), None, &doc_icon_val)?;
    reg_write_sz(&format!("{prog_root}\\shell\\open\\command"), None, &open_cmd)?;

    // 2. 应用程序实体：在「打开方式」与「默认应用」列表中展示软件主程序圆轮图标
    let app_key = "Software\\Classes\\Applications\\Tools3000_Picasa_Viewer.exe";
    reg_write_sz(app_key, Some("FriendlyAppName"), "Tools3000 Picasa Viewer")?;
    reg_write_sz(&format!("{app_key}\\DefaultIcon"), None, &app_icon_val)?;
    reg_write_sz(&format!("{app_key}\\shell\\open\\command"), None, &open_cmd)?;

    // 3. 系统默认应用 Capabilities 清单
    let cap_key = "Software\\Tools3000\\PicasaViewer\\Capabilities";
    reg_write_sz(cap_key, Some("ApplicationName"), "Tools3000 Picasa Viewer")?;
    reg_write_sz(
        cap_key,
        Some("ApplicationDescription"),
        "Ultra-fast native image viewer inspired by Google Picasa Photo Viewer",
    )?;
    reg_write_sz(&format!("{cap_key}\\ApplicationIcon"), None, &app_icon_val)?;
    reg_write_sz(
        "Software\\RegisteredApplications",
        Some("Tools3000 Picasa Viewer"),
        cap_key,
    )?;

    let selected_set: HashSet<&str> = selected_exts.iter().copied().collect();
    let fa_key = format!("{cap_key}\\FileAssociations");

    for &ext in SUPPORTED_EXTENSIONS {
        let dot_ext = format!(".{ext}");
        let ext_class_key = format!("Software\\Classes\\{dot_ext}");
        let owp_key = format!("{ext_class_key}\\OpenWithProgids");
        if selected_set.contains(ext) {
            // 写入 Capabilities 与 OpenWithProgids
            reg_write_sz(&fa_key, Some(&dot_ext), APP_PROG_ID)?;
            reg_write_sz(&owp_key, Some(APP_PROG_ID), "")?;
            // 同步将 HKCU\Software\Classes\.ext 默认类指向 APP_PROG_ID（未被 UWP UserChoice 锁定的后缀立即生效并切换为拍立得图标！）
            let _ = reg_write_sz(&ext_class_key, None, APP_PROG_ID);
            let _ = reg_write_sz(&ext_class_key, Some("PerceivedType"), "image");
        } else {
            // 取消未勾选扩展名在本软件下的关联
            reg_delete_value(&fa_key, &dot_ext);
            reg_delete_value(&owp_key, APP_PROG_ID);
            if reg_read_sz(&ext_class_key, None).as_deref() == Some(APP_PROG_ID) {
                let _ = reg_write_sz(&ext_class_key, None, "");
            }
        }
    }

    // 4. 通知 Windows Explorer 立即刷新全系统文件类型关联与拍立得相纸图标缓存！
    unsafe {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, None, None);
    }
    Ok(())
}

/// 清理用户级 HKCU 文件关联注册并还原资源管理器图标缓存
pub fn unregister_user_file_associations() -> Result<(), String> {
    for &ext in SUPPORTED_EXTENSIONS {
        let dot_ext = format!(".{ext}");
        let ext_class_key = format!("Software\\Classes\\{dot_ext}");
        let owp_key = format!("{ext_class_key}\\OpenWithProgids");
        reg_delete_value(&owp_key, APP_PROG_ID);
        if reg_read_sz(&ext_class_key, None).as_deref() == Some(APP_PROG_ID) {
            let _ = reg_write_sz(&ext_class_key, None, "");
        }
    }
    reg_delete_value(
        "Software\\RegisteredApplications",
        "Tools3000 Picasa Viewer",
    );

    unsafe {
        for sub in [
            format!("Software\\Classes\\{}", APP_PROG_ID),
            "Software\\Classes\\Applications\\Tools3000_Picasa_Viewer.exe".to_string(),
            "Software\\Tools3000\\PicasaViewer".to_string(),
        ] {
            let wide = to_wide_null(OsStr::new(&sub));
            let _ = RegDeleteTreeW(HKEY_CURRENT_USER, PCWSTR(wide.as_ptr()));
        }
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, None, None);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rotate_and_flip_frame_preserves_pixels_correctly() {
        let f = ImageFrame {
            width: 2,
            height: 1,
            delay_ms: 0,
            bgra_premul: vec![10, 20, 30, 255, 40, 50, 60, 255],
        };
        let r90 =
            apply_transform_to_frame(&f, RotationQuarter::Deg90, false, false).into_owned();
        assert_eq!(r90.width, 1);
        assert_eq!(r90.height, 2);
        assert_eq!(&r90.bgra_premul[0..4], &[10, 20, 30, 255]);
        assert_eq!(&r90.bgra_premul[4..8], &[40, 50, 60, 255]);

        let r360 =
            apply_transform_to_frame(&r90, RotationQuarter::Deg270, false, false).into_owned();
        assert_eq!(r360.width, 2);
        assert_eq!(r360.height, 1);
        assert_eq!(r360.bgra_premul, f.bgra_premul);

        let fh = apply_transform_to_frame(&f, RotationQuarter::Deg0, true, false).into_owned();
        assert_eq!(&fh.bgra_premul[0..4], &[40, 50, 60, 255]);
        assert_eq!(&fh.bgra_premul[4..8], &[10, 20, 30, 255]);
    }
}
