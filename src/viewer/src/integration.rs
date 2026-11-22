// ─────────────────────────────────────────────────────────────────────────────
// integration.rs — 独立看图器系统集成模块（100% 与外部宿主解耦）
// 包含：剪贴板(图像含当前旋转+文件/路径)、现代 COM 回收站安全删除(兼容长路径)、
//       原生 Shell 定位与打开方式、导出当前帧 PNG(含当前旋转)、设为壁纸、
//       原生 8 语种右键菜单、注册表关联原值备份还原与 Win10/11 UserChoice 状态检测
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{APP_PROG_ID, SUPPORTED_EXTENSIONS};
use crate::folder::normalize_full_path;
use crate::i18n::ViewerLanguage;
use crate::types::{ImageFrame, RotationQuarter, ToolbarAction};
use std::borrow::Cow;
use std::collections::HashSet;
use std::ffi::OsStr;
use std::fs::File;
use std::io::BufWriter;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{GlobalFree, HGLOBAL, HWND};
use windows::Win32::Graphics::Gdi::{BI_BITFIELDS, BITMAPV5HEADER};
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CoTaskMemFree, CoUninitialize, CLSCTX_ALL,
    CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED,
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
    FileOperation, FileSaveDialog, IFileOperation, IFileSaveDialog, ILCreateFromPathW, ILFree,
    IShellItem, SHChangeNotify, SHCreateItemFromParsingName, SHOpenFolderAndSelectItems,
    SHOpenWithDialog, ShellExecuteW, DROPFILES, FOFX_RECYCLEONDELETE, FOF_ALLOWUNDO,
    FOF_NOCONFIRMATION, FOF_NOERRORUI, FOF_SILENT, OAIF_ALLOW_REGISTRATION, OAIF_EXEC, OPENASINFO,
    SHCNE_ASSOCCHANGED, SHCNF_IDLIST, SIGDN_FILESYSPATH,
};
use windows::Win32::UI::WindowsAndMessaging::{
    AppendMenuW, CreatePopupMenu, DestroyMenu, SetForegroundWindow, SystemParametersInfoW,
    TrackPopupMenuEx, MF_CHECKED, MF_SEPARATOR, MF_STRING, MF_UNCHECKED, SPIF_SENDCHANGE,
    SPIF_UPDATEINIFILE, SPI_SETDESKWALLPAPER, SW_SHOWNORMAL, TPM_LEFTALIGN, TPM_RETURNCMD,
    TPM_RIGHTBUTTON,
};

const LCS_SRGB: u32 = 0x7352_4742;
const VIEWER_REG_ROOT: &str = "Software\\Tools3000\\PicasaViewer";
const BACKUP_ASSOC_ROOT: &str = "Software\\Tools3000\\PicasaViewer\\BackupAssoc";

/// 线程安全 COM STA 生命周期 RAII 守卫（自动精确配对 CoInitializeEx / CoUninitialize）
struct ComGuard {
    should_uninit: bool,
}

impl ComGuard {
    fn init_sta() -> Self {
        let should_uninit = unsafe { CoInitializeEx(None, COINIT_APARTMENTTHREADED).is_ok() };
        Self { should_uninit }
    }
}

impl Drop for ComGuard {
    fn drop(&mut self) {
        if self.should_uninit {
            unsafe {
                CoUninitialize();
            }
        }
    }
}

fn to_wide_null(s: &OsStr) -> Vec<u16> {
    s.encode_wide().chain(Some(0)).collect()
}

fn str_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

fn current_exe_file_name(exe: &Path) -> String {
    exe.file_name()
        .and_then(|n| n.to_str())
        .filter(|s| !s.is_empty())
        .unwrap_or("Tools3000_Picasa_Viewer.exe")
        .to_string()
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

    let target = {
        let _com = ComGuard::init_sta();
        unsafe {
            (|| -> Option<PathBuf> {
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
            })()
        }
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
    let abs = normalize_full_path(path);
    let wide = to_wide_null(abs.as_os_str());
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

/// 唤起 Windows 原生「打开方式」对话框（SHOpenWithDialog，彻底兼容空格/中文/逗号/长路径）
pub fn open_with_system_dialog(hwnd: HWND, path: &Path) {
    let _com = ComGuard::init_sta();
    let abs = normalize_full_path(path);
    let wide = to_wide_null(abs.as_os_str());
    let info = OPENASINFO {
        pcszFile: PCWSTR(wide.as_ptr()),
        pcszClass: PCWSTR::null(),
        oaifInFlags: OAIF_ALLOW_REGISTRATION | OAIF_EXEC,
    };
    unsafe {
        let _ = SHOpenWithDialog(hwnd, &info);
    }
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

/// 将图片通过现代 COM `IFileOperation` 安全移入 Windows 回收站（原生支持 longPathAware > 260 字符与可撤销恢复）
pub fn move_to_recycle_bin(hwnd: HWND, path: &Path) -> bool {
    let _com = ComGuard::init_sta();
    let abs = normalize_full_path(path);
    let wide = to_wide_null(abs.as_os_str());
    unsafe {
        let item: IShellItem = match SHCreateItemFromParsingName(PCWSTR(wide.as_ptr()), None) {
            Ok(it) => it,
            Err(_) => return false,
        };
        let fop: IFileOperation = match CoCreateInstance(&FileOperation, None, CLSCTX_ALL) {
            Ok(op) => op,
            Err(_) => return false,
        };
        let flags = FOF_ALLOWUNDO
            | FOF_NOCONFIRMATION
            | FOF_SILENT
            | FOF_NOERRORUI
            | FOFX_RECYCLEONDELETE;
        if fop.SetOperationFlags(flags).is_err() {
            return false;
        }
        if !hwnd.is_invalid() {
            let _ = fop.SetOwnerWindow(hwnd);
        }
        if fop.DeleteItem(&item, None).is_err() {
            return false;
        }
        if fop.PerformOperations().is_err() {
            return false;
        }
        !fop.GetAnyOperationsAborted()
            .map(|b| b.as_bool())
            .unwrap_or(false)
    }
}

/// 使用原生 Shell API `SHOpenFolderAndSelectItems` 在资源管理器中打开并精准选中文件（兼容空格/中文/逗号/长路径）
pub fn reveal_in_explorer(path: &Path) {
    let _com = ComGuard::init_sta();
    let abs = normalize_full_path(path);
    let wide = to_wide_null(abs.as_os_str());
    unsafe {
        let pidl = ILCreateFromPathW(PCWSTR(wide.as_ptr()));
        if !pidl.is_null() {
            let _ = SHOpenFolderAndSelectItems(pidl, None, 0);
            ILFree(Some(pidl as *const _));
        }
    }
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
        let mut byte_len: u32 = 0;
        let query_sz_ok =
            RegQueryValueExW(hkey, name_ptr, None, None, None, Some(&mut byte_len)).is_ok();
        if !query_sz_ok || byte_len < 2 || byte_len > 65536 {
            let _ = RegCloseKey(hkey);
            return None;
        }
        let mut buf = vec![0u16; (byte_len as usize).div_ceil(2)];
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

fn reg_delete_value(subkey: &str, value_name: Option<&str>) {
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
            let wide_name = value_name.map(|n| to_wide_null(OsStr::new(n)));
            let name_ptr = wide_name
                .as_ref()
                .map(|v| PCWSTR(v.as_ptr()))
                .unwrap_or(PCWSTR::null());
            let _ = RegDeleteValueW(hkey, name_ptr);
            let _ = RegCloseKey(hkey);
        }
    }
}

fn reg_delete_tree(subkey: &str) {
    unsafe {
        let wide = to_wide_null(OsStr::new(subkey));
        let _ = RegDeleteTreeW(HKEY_CURRENT_USER, PCWSTR(wide.as_ptr()));
    }
}

/// 单扩展名在当前 Windows 用户环境下的真实默认关联状态
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExtAssocStatus {
    /// 当前由本看图器默认打开（UserChoice 或 HKCU Classes 指向 APP_PROG_ID）
    ActiveTools3000,
    /// 受 Win10/11 UserChoice 锁定在其他默认程序（需到系统「默认应用」确认）
    LockedByUserChoice,
    /// 未由本程序接管
    NotAssociated,
}

/// 查询 Win10/11 `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.ext\UserChoice\ProgId`
pub fn query_user_choice_prog_id(ext: &str) -> Option<String> {
    let clean = ext.trim_start_matches('.');
    let key = format!(
        "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.{clean}\\UserChoice"
    );
    reg_read_sz(&key, Some("ProgId")).filter(|s| !s.is_empty())
}

/// 检测指定扩展名是否被 Win10/11 UserChoice 锁定在其他应用（非本看图器）
pub fn is_ext_locked_by_other_user_choice(ext: &str) -> bool {
    match query_user_choice_prog_id(ext) {
        Some(prog_id) => !prog_id.eq_ignore_ascii_case(APP_PROG_ID),
        None => false,
    }
}

/// 查询单个扩展名的真实系统默认打开状态（结合 UserChoice 与 HKCU\Software\Classes）
pub fn query_extension_assoc_status(ext: &str) -> ExtAssocStatus {
    let clean = ext.trim_start_matches('.');
    if is_ext_locked_by_other_user_choice(clean) {
        return ExtAssocStatus::LockedByUserChoice;
    }
    if query_user_choice_prog_id(clean).is_some() {
        return ExtAssocStatus::ActiveTools3000;
    }
    let ext_class_key = format!("Software\\Classes\\.{clean}");
    if reg_read_sz(&ext_class_key, None)
        .as_deref()
        .map(|v| v.eq_ignore_ascii_case(APP_PROG_ID))
        .unwrap_or(false)
    {
        ExtAssocStatus::ActiveTools3000
    } else {
        ExtAssocStatus::NotAssociated
    }
}

/// 若扩展名当前默认类为本程序，优先从 BackupAssoc 还原原值；无备份时真正删除 `(Default)` 绝不写空串遮蔽 HKLM
fn restore_or_delete_ext_default(ext: &str) {
    let dot_ext = format!(".{ext}");
    let ext_class_key = format!("Software\\Classes\\{dot_ext}");
    let backup_key = format!("{BACKUP_ASSOC_ROOT}\\{dot_ext}");
    if reg_read_sz(&ext_class_key, None)
        .as_deref()
        .map(|v| v.eq_ignore_ascii_case(APP_PROG_ID))
        .unwrap_or(false)
    {
        if let Some(orig) = reg_read_sz(&backup_key, None).filter(|s| !s.is_empty()) {
            let _ = reg_write_sz(&ext_class_key, None, &orig);
        } else {
            // 真正删除 (Default) 值，恢复回退至 HKLM\Software\Classes\.ext 系统默认类
            reg_delete_value(&ext_class_key, None);
        }
    }
    reg_delete_tree(&backup_key);
}

/// 查询当前用户已经在本软件注册关联的扩展名集合（不含前导点，小写）
pub fn query_registered_extensions() -> HashSet<String> {
    let mut set = HashSet::new();
    let assoc_key = format!("{VIEWER_REG_ROOT}\\Capabilities\\FileAssociations");
    for &ext in SUPPORTED_EXTENSIONS {
        let dot_ext = format!(".{ext}");
        if let Some(val) = reg_read_sz(&assoc_key, Some(&dot_ext)) {
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

/// 为用户勾选的图片扩展名写入关联，备份原有默认值，并将关联图片图标设为经典 Picasa 拍立得相纸图标 (`"{exe}",1`)
pub fn register_selected_file_associations(selected_exts: &[&str]) -> Result<(), String> {
    let exe = std::env::current_exe().map_err(|e| format!("{e}"))?;
    let exe_str = exe.display().to_string();
    let exe_name = current_exe_file_name(&exe);
    let open_cmd = format!("\"{}\" \"%1\"", exe_str);
    // 索引 0 = 应用主程序图标 (雕塑玻璃彩色快门圆轮)
    let app_icon_val = format!("\"{}\",0", exe_str);
    // 索引 1 = 图片文件关联文档图标 (经典 Picasa 拍立得青松金丘相纸图标，对应 viewer.rc ID = 2)
    let doc_icon_val = format!("\"{}\",1", exe_str);

    // 1. 图片文档 ProgID：使所有关联的图片文件显示经典 Picasa 拍立得相纸图标
    let prog_root = format!("Software\\Classes\\{}", APP_PROG_ID);
    reg_write_sz(&prog_root, None, "Picasa Photo Viewer Image")?;
    reg_write_sz(&format!("{prog_root}\\DefaultIcon"), None, &doc_icon_val)?;
    reg_write_sz(&format!("{prog_root}\\shell\\open\\command"), None, &open_cmd)?;

    // 2. 应用程序实体（动态使用当前 EXE 文件名，100% 兼容独立改名运行）
    let prev_exe_name = reg_read_sz(VIEWER_REG_ROOT, Some("RegisteredExeName"));
    if let Some(old_name) = prev_exe_name.as_deref() {
        if !old_name.eq_ignore_ascii_case(&exe_name) && !old_name.is_empty() {
            reg_delete_tree(&format!("Software\\Classes\\Applications\\{old_name}"));
        }
    }
    let app_key = format!("Software\\Classes\\Applications\\{exe_name}");
    reg_write_sz(&app_key, Some("FriendlyAppName"), "Tools3000 Picasa Viewer")?;
    reg_write_sz(&format!("{app_key}\\DefaultIcon"), None, &app_icon_val)?;
    reg_write_sz(&format!("{app_key}\\shell\\open\\command"), None, &open_cmd)?;
    let _ = reg_write_sz(VIEWER_REG_ROOT, Some("RegisteredExeName"), &exe_name);

    // 3. 系统默认应用 Capabilities 清单
    let cap_key = format!("{VIEWER_REG_ROOT}\\Capabilities");
    reg_write_sz(&cap_key, Some("ApplicationName"), "Tools3000 Picasa Viewer")?;
    reg_write_sz(
        &cap_key,
        Some("ApplicationDescription"),
        "Rust-recreated lightweight Picasa Photo Viewer (image viewer only, not full Google Picasa suite)",
    )?;
    reg_write_sz(&format!("{cap_key}\\ApplicationIcon"), None, &app_icon_val)?;
    reg_write_sz(
        "Software\\RegisteredApplications",
        Some("Tools3000 Picasa Viewer"),
        &cap_key,
    )?;

    let selected_set: HashSet<&str> = selected_exts.iter().copied().collect();
    let fa_key = format!("{cap_key}\\FileAssociations");

    for &ext in SUPPORTED_EXTENSIONS {
        let dot_ext = format!(".{ext}");
        let ext_class_key = format!("Software\\Classes\\{dot_ext}");
        let owp_key = format!("{ext_class_key}\\OpenWithProgids");
        let backup_key = format!("{BACKUP_ASSOC_ROOT}\\{dot_ext}");
        if selected_set.contains(ext) {
            // 注册前若 HKCU\Software\Classes\.ext 已存在非本程序的默认值，先无损备份到 BackupAssoc\.ext
            if let Some(existing) = reg_read_sz(&ext_class_key, None) {
                if !existing.is_empty()
                    && !existing.eq_ignore_ascii_case(APP_PROG_ID)
                    && reg_read_sz(&backup_key, None).is_none()
                {
                    let _ = reg_write_sz(&backup_key, None, &existing);
                }
            }
            reg_write_sz(&fa_key, Some(&dot_ext), APP_PROG_ID)?;
            reg_write_sz(&owp_key, Some(APP_PROG_ID), "")?;
            let _ = reg_write_sz(&ext_class_key, None, APP_PROG_ID);
            let _ = reg_write_sz(&ext_class_key, Some("PerceivedType"), "image");
        } else {
            // 取消未勾选扩展名在本软件下的关联，并还原备份原值或真正删除默认值
            reg_delete_value(&fa_key, Some(&dot_ext));
            reg_delete_value(&owp_key, Some(APP_PROG_ID));
            restore_or_delete_ext_default(ext);
        }
    }

    // 4. 通知 Windows Explorer 立即刷新全系统文件类型关联与拍立得相纸图标缓存
    unsafe {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, None, None);
    }
    Ok(())
}

/// 清理用户级 HKCU 文件关联注册，还原原值或真正删除默认值并刷新资源管理器图标缓存
pub fn unregister_user_file_associations() -> Result<(), String> {
    // 顺序关键：必须先循环读取 BackupAssoc 还原/删除各扩展名默认值，最后再删除 VIEWER_REG_ROOT 整树
    for &ext in SUPPORTED_EXTENSIONS {
        let dot_ext = format!(".{ext}");
        let ext_class_key = format!("Software\\Classes\\{dot_ext}");
        let owp_key = format!("{ext_class_key}\\OpenWithProgids");
        reg_delete_value(&owp_key, Some(APP_PROG_ID));
        restore_or_delete_ext_default(ext);
    }
    reg_delete_value(
        "Software\\RegisteredApplications",
        Some("Tools3000 Picasa Viewer"),
    );

    let mut app_keys: Vec<String> = vec![
        format!("Software\\Classes\\{}", APP_PROG_ID),
        "Software\\Classes\\Applications\\Tools3000_Picasa_Viewer.exe".to_string(),
    ];
    if let Ok(exe) = std::env::current_exe() {
        let cur_name = current_exe_file_name(&exe);
        app_keys.push(format!("Software\\Classes\\Applications\\{cur_name}"));
    }
    if let Some(reg_name) = reg_read_sz(VIEWER_REG_ROOT, Some("RegisteredExeName")) {
        if !reg_name.is_empty() {
            app_keys.push(format!("Software\\Classes\\Applications\\{reg_name}"));
        }
    }
    app_keys.push(VIEWER_REG_ROOT.to_string());

    for sub in app_keys {
        reg_delete_tree(&sub);
    }
    unsafe {
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

    #[test]
    fn supported_extensions_are_unique_lowercase_without_dots() {
        assert!(SUPPORTED_EXTENSIONS.len() >= 40);
        let mut seen = HashSet::new();
        for &ext in SUPPORTED_EXTENSIONS {
            assert!(!ext.is_empty());
            assert!(
                !ext.starts_with('.'),
                "extension should not start with dot: {ext}"
            );
            assert_eq!(
                ext,
                ext.to_ascii_lowercase(),
                "extension must be lowercase: {ext}"
            );
            assert!(seen.insert(ext), "duplicate extension: {ext}");
        }
    }

    #[test]
    fn dynamic_exe_name_supports_renamed_standalone_binary() {
        assert_eq!(
            current_exe_file_name(Path::new(r"D:\Portable\MyPhotoViewer.exe")),
            "MyPhotoViewer.exe"
        );
    }

    #[test]
    fn reg_default_value_delete_removes_value_instead_of_empty_string() {
        // 使用隔离临时子键实测 RegDeleteValueW(NULL) 真正删除 (Default) 值
        let test_key = "Software\\Tools3000\\PicasaViewer_UnitTest_RegDefault";
        reg_write_sz(test_key, None, "Custom.TestProgId").expect("write default");
        assert_eq!(reg_read_sz(test_key, None).as_deref(), Some("Custom.TestProgId"));
        reg_delete_value(test_key, None);
        assert_eq!(reg_read_sz(test_key, None), None);
        reg_delete_tree(test_key);
    }
}
