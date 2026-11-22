// ─────────────────────────────────────────────────────────────────────────────
// assoc_dialog.rs — 世界级原生 Direct2D + DirectWrite 文件关联与默认看图器面板
//
// 特性：
// - 100% 独立原生硬件加速矢量渲染（复用单支可变色画刷，0 帧内 COM 刷子分配）；
// - 单实例复用（已有配置窗直接激活置前，绝不叠开双窗）；
// - PerMonitorV2 全分辨率多屏精准 DPI（GetDpiForMonitor + WM_DPICHANGED + WM_SIZE）；
// - 完整键盘无障碍焦点环（Tab/Shift+Tab/Up/Down/Space/Enter/Esc，焦点与悬停完全解耦）；
// - 8 组格式 8 语种地道本地化 + Win10/11 UserChoice 锁定检测徽章 + 底部状态反馈横幅；
// - 严守排版红线（0 Emoji、字号 >= 12.0 DIP、字重 >= 500、等宽字体回退检测）。
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{ASSOC_WINDOW_CLASS, SUPPORTED_EXTENSIONS};
use crate::i18n::ViewerLanguage;
use crate::integration::{
    open_windows_default_apps_settings, query_extension_assoc_status, query_registered_extensions,
    register_selected_file_associations, unregister_user_file_associations, ExtAssocStatus,
};
use std::fs::File;
use std::io::BufWriter;
use std::path::Path;
use std::sync::Once;
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows::Win32::Graphics::Direct2D::Common::{
    D2D1_ALPHA_MODE_IGNORE, D2D1_ALPHA_MODE_PREMULTIPLIED, D2D1_COLOR_F, D2D1_PIXEL_FORMAT,
    D2D_POINT_2F, D2D_RECT_F, D2D_SIZE_U,
};
use windows::Win32::Graphics::Direct2D::{
    D2D1CreateFactory, ID2D1Bitmap, ID2D1Factory1, ID2D1HwndRenderTarget, ID2D1RenderTarget,
    ID2D1SolidColorBrush, ID2D1StrokeStyle, ID2D1StrokeStyle1,
    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, D2D1_CAP_STYLE_ROUND, D2D1_DRAW_TEXT_OPTIONS_CLIP,
    D2D1_ELLIPSE, D2D1_FACTORY_OPTIONS, D2D1_FACTORY_TYPE_SINGLE_THREADED,
    D2D1_HWND_RENDER_TARGET_PROPERTIES, D2D1_LINE_JOIN_ROUND, D2D1_PRESENT_OPTIONS_NONE,
    D2D1_RENDER_TARGET_PROPERTIES, D2D1_RENDER_TARGET_TYPE_DEFAULT,
    D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1_ROUNDED_RECT, D2D1_STROKE_STYLE_PROPERTIES1,
    D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE,
};
use windows::Win32::Graphics::DirectWrite::{
    DWriteCreateFactory, IDWriteFactory, IDWriteTextFormat, DWRITE_FACTORY_TYPE_SHARED,
    DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_WEIGHT_BOLD,
    DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_MEASURING_MODE_NATURAL,
    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
    DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_TRIMMING,
    DWRITE_TRIMMING_GRANULARITY_CHARACTER, DWRITE_WORD_WRAPPING_NO_WRAP, DWRITE_WORD_WRAPPING_WRAP,
};
use windows::Win32::Graphics::Dwm::{
    DwmSetWindowAttribute, DWMWA_USE_IMMERSIVE_DARK_MODE, DWMWA_WINDOW_CORNER_PREFERENCE,
    DWMWCP_ROUND,
};
use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_B8G8R8A8_UNORM;
use windows::Win32::Graphics::Gdi::{
    BeginPaint, EndPaint, GetMonitorInfoW, InvalidateRect, MonitorFromPoint, MonitorFromWindow,
    HMONITOR, MONITORINFO, MONITOR_DEFAULTTONEAREST, PAINTSTRUCT,
};
use windows::Win32::Graphics::Imaging::{
    CLSID_WICImagingFactory, GUID_WICPixelFormat32bppPBGRA, IWICImagingFactory,
    WICBitmapCacheOnLoad, WICBitmapDitherTypeNone, WICBitmapLockRead, WICBitmapPaletteTypeCustom,
    WICRect,
};
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CoUninitialize, CLSCTX_INPROC_SERVER,
    COINIT_APARTMENTTHREADED,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::Registry::{
    RegCloseKey, RegOpenKeyExW, RegQueryValueExW, HKEY, HKEY_CURRENT_USER, KEY_READ,
};
use windows::Win32::UI::Controls::WM_MOUSELEAVE;
use windows::Win32::UI::HiDpi::{
    AdjustWindowRectExForDpi, GetDpiForMonitor, GetDpiForSystem, GetDpiForWindow,
    SetProcessDpiAwarenessContext, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, MDT_EFFECTIVE_DPI,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    EnableWindow, GetKeyState, ReleaseCapture, SetCapture, TrackMouseEvent, TME_LEAVE,
    TRACKMOUSEEVENT, VK_DOWN, VK_ESCAPE, VK_RETURN, VK_SHIFT, VK_SPACE, VK_TAB, VK_UP,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyIcon, DestroyWindow, DispatchMessageW, FindWindowW,
    GetClientRect, GetCursorPos, GetMessageW, GetWindowLongPtrW, IsWindow, LoadCursorW, LoadImageW,
    PostMessageW, RegisterClassExW, SendMessageW, SetCursor, SetForegroundWindow,
    SetWindowLongPtrW, SetWindowPos, ShowWindow, TranslateMessage, GWLP_USERDATA, HCURSOR, HICON,
    ICON_BIG, ICON_SMALL, IDC_ARROW, IDC_HAND, IMAGE_ICON, LR_DEFAULTCOLOR, MSG, SWP_NOACTIVATE,
    SWP_NOZORDER, SW_RESTORE, SW_SHOW, WM_CAPTURECHANGED, WM_CLOSE, WM_DESTROY, WM_DPICHANGED,
    WM_ERASEBKGND, WM_KEYDOWN, WM_KILLFOCUS, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE, WM_PAINT,
    WM_SETCURSOR, WM_SETICON, WM_SIZE, WNDCLASSEXW, WS_CAPTION, WS_EX_DLGMODALFRAME, WS_EX_TOPMOST,
    WS_OVERLAPPED, WS_SYSMENU,
};

const DLG_W_DIP: f32 = 824.0;
const DLG_H_DIP: f32 = 590.0;

struct FormatGroup {
    exts_preview: &'static str,
    common_default: bool,
    exts: &'static [&'static str],
}

const FORMAT_GROUPS: [FormatGroup; 8] = [
    FormatGroup {
        exts_preview: ".jpg  .jpeg  .jpe  .jfif  .pjpeg",
        common_default: true,
        exts: &["jpg", "jpeg", "jpe", "jfif", "pjpeg"],
    },
    FormatGroup {
        exts_preview: ".png  .apng",
        common_default: true,
        exts: &["png", "apng"],
    },
    FormatGroup {
        exts_preview: ".webp",
        common_default: true,
        exts: &["webp"],
    },
    FormatGroup {
        exts_preview: ".gif",
        common_default: true,
        exts: &["gif"],
    },
    FormatGroup {
        exts_preview: ".bmp  .dib  .rle  .ico  .cur",
        common_default: true,
        exts: &["bmp", "dib", "rle", "ico", "cur"],
    },
    FormatGroup {
        exts_preview: ".tif  .tiff",
        common_default: true,
        exts: &["tif", "tiff"],
    },
    FormatGroup {
        exts_preview: ".svg  .svgz",
        common_default: false,
        exts: &["svg", "svgz"],
    },
    FormatGroup {
        exts_preview: ".avif  .heic  .dng  .cr2  .nef  .tga  .qoi  .dds ...",
        common_default: false,
        exts: &[
            "avif", "heic", "heif", "hif", "wdp", "hdp", "jxr", "tga", "qoi", "ppm", "pgm", "pbm",
            "dds", "dng", "cr2", "cr3", "nef", "arw", "orf", "rw2", "pef", "raf", "srw",
        ],
    },
];

pub enum AssocDialogOutcome {
    Closed,
    Applied,
    Unregistered,
    Failed(String),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum GroupBadge {
    None,
    Active,
    LockedByOs,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum HitItem {
    Row(usize),
    BtnSelectAll,
    BtnCommon,
    BtnUnregister,
    BtnApplyOnly,
    BtnApplyDefault,
    BtnClose,
}

const FOCUS_ORDER: [HitItem; 14] = [
    HitItem::Row(0),
    HitItem::Row(1),
    HitItem::Row(2),
    HitItem::Row(3),
    HitItem::Row(4),
    HitItem::Row(5),
    HitItem::Row(6),
    HitItem::Row(7),
    HitItem::BtnSelectAll,
    HitItem::BtnCommon,
    HitItem::BtnUnregister,
    HitItem::BtnApplyOnly,
    HitItem::BtnApplyDefault,
    HitItem::BtnClose,
];

fn cycle_focus(current: HitItem, backward: bool) -> HitItem {
    let idx = FOCUS_ORDER
        .iter()
        .position(|&x| x == current)
        .unwrap_or(0);
    let next = if backward {
        (idx + FOCUS_ORDER.len() - 1) % FOCUS_ORDER.len()
    } else {
        (idx + 1) % FOCUS_ORDER.len()
    };
    FOCUS_ORDER[next]
}

#[derive(Clone, Copy)]
struct Palette {
    canvas: D2D1_COLOR_F,
    card: D2D1_COLOR_F,
    well: D2D1_COLOR_F,
    row_hover: D2D1_COLOR_F,
    row_checked: D2D1_COLOR_F,
    border: D2D1_COLOR_F,
    border_strong: D2D1_COLOR_F,
    text_primary: D2D1_COLOR_F,
    text_secondary: D2D1_COLOR_F,
    text_muted: D2D1_COLOR_F,
    accent: D2D1_COLOR_F,
    accent_hover: D2D1_COLOR_F,
    accent_pressed: D2D1_COLOR_F,
    accent_soft_bg: D2D1_COLOR_F,
    accent_soft_border: D2D1_COLOR_F,
    focus_ring: D2D1_COLOR_F,
    ok_text: D2D1_COLOR_F,
    ok_bg: D2D1_COLOR_F,
    ok_border: D2D1_COLOR_F,
    warn_text: D2D1_COLOR_F,
    warn_bg: D2D1_COLOR_F,
    warn_border: D2D1_COLOR_F,
    danger_text: D2D1_COLOR_F,
    danger_hover_bg: D2D1_COLOR_F,
    danger_border: D2D1_COLOR_F,
    btn_bg: D2D1_COLOR_F,
    btn_hover: D2D1_COLOR_F,
    btn_pressed: D2D1_COLOR_F,
    on_accent: D2D1_COLOR_F,
    shadow: D2D1_COLOR_F,
}

const fn rgba(r: u8, g: u8, b: u8, a: f32) -> D2D1_COLOR_F {
    D2D1_COLOR_F {
        r: r as f32 / 255.0,
        g: g as f32 / 255.0,
        b: b as f32 / 255.0,
        a,
    }
}

fn palette_for(dark: bool) -> Palette {
    if dark {
        Palette {
            canvas: rgba(20, 21, 25, 1.0),
            card: rgba(29, 31, 38, 1.0),
            well: rgba(38, 41, 50, 1.0),
            row_hover: rgba(46, 51, 63, 1.0),
            row_checked: rgba(59, 130, 246, 0.14),
            border: rgba(255, 255, 255, 0.10),
            border_strong: rgba(255, 255, 255, 0.22),
            text_primary: rgba(243, 244, 246, 1.0),
            text_secondary: rgba(176, 184, 198, 1.0),
            text_muted: rgba(136, 145, 162, 1.0),
            accent: rgba(59, 130, 246, 1.0),
            accent_hover: rgba(96, 165, 250, 1.0),
            accent_pressed: rgba(37, 99, 235, 1.0),
            accent_soft_bg: rgba(59, 130, 246, 0.16),
            accent_soft_border: rgba(96, 165, 250, 0.36),
            focus_ring: rgba(147, 197, 253, 0.96),
            ok_text: rgba(74, 222, 128, 1.0),
            ok_bg: rgba(34, 197, 94, 0.15),
            ok_border: rgba(74, 222, 128, 0.36),
            warn_text: rgba(251, 191, 36, 1.0),
            warn_bg: rgba(245, 158, 11, 0.15),
            warn_border: rgba(251, 191, 36, 0.38),
            danger_text: rgba(248, 113, 113, 1.0),
            danger_hover_bg: rgba(239, 68, 68, 0.16),
            danger_border: rgba(248, 113, 113, 0.36),
            btn_bg: rgba(38, 41, 50, 1.0),
            btn_hover: rgba(50, 54, 66, 1.0),
            btn_pressed: rgba(32, 35, 43, 1.0),
            on_accent: rgba(255, 255, 255, 1.0),
            shadow: rgba(0, 0, 0, 0.34),
        }
    } else {
        Palette {
            canvas: rgba(245, 246, 249, 1.0),
            card: rgba(255, 255, 255, 1.0),
            well: rgba(241, 244, 249, 1.0),
            row_hover: rgba(241, 245, 252, 1.0),
            row_checked: rgba(37, 99, 235, 0.07),
            border: rgba(15, 23, 42, 0.11),
            border_strong: rgba(15, 23, 42, 0.24),
            text_primary: rgba(17, 24, 39, 1.0),
            text_secondary: rgba(71, 85, 105, 1.0),
            text_muted: rgba(100, 116, 139, 1.0),
            accent: rgba(37, 99, 235, 1.0),
            accent_hover: rgba(29, 78, 216, 1.0),
            accent_pressed: rgba(30, 64, 175, 1.0),
            accent_soft_bg: rgba(37, 99, 235, 0.10),
            accent_soft_border: rgba(37, 99, 235, 0.28),
            focus_ring: rgba(29, 78, 216, 0.96),
            ok_text: rgba(21, 128, 61, 1.0),
            ok_bg: rgba(22, 163, 74, 0.10),
            ok_border: rgba(22, 163, 74, 0.32),
            warn_text: rgba(180, 83, 9, 1.0),
            warn_bg: rgba(217, 119, 6, 0.10),
            warn_border: rgba(217, 119, 6, 0.34),
            danger_text: rgba(220, 38, 38, 1.0),
            danger_hover_bg: rgba(220, 38, 38, 0.08),
            danger_border: rgba(220, 38, 38, 0.30),
            btn_bg: rgba(255, 255, 255, 1.0),
            btn_hover: rgba(243, 246, 251, 1.0),
            btn_pressed: rgba(229, 234, 243, 1.0),
            on_accent: rgba(255, 255, 255, 1.0),
            shadow: rgba(15, 23, 42, 0.12),
        }
    }
}

#[derive(Clone, Copy)]
struct DialogLayout {
    left_card: D2D_RECT_F,
    right_card: D2D_RECT_F,
    rows: [D2D_RECT_F; 8],
    btn_all: D2D_RECT_F,
    btn_common: D2D_RECT_F,
    btn_unreg: D2D_RECT_F,
    status_strip: D2D_RECT_F,
    btn_apply_only: D2D_RECT_F,
    btn_apply_default: D2D_RECT_F,
    btn_close: D2D_RECT_F,
    footer_y: f32,
}

struct TextFormats {
    title: IDWriteTextFormat,
    subtitle: IDWriteTextFormat,
    badge: IDWriteTextFormat,
    row_title: IDWriteTextFormat,
    row_ext: IDWriteTextFormat,
    card_caption: IDWriteTextFormat,
    status: IDWriteTextFormat,
    btn: IDWriteTextFormat,
    btn_bold: IDWriteTextFormat,
}

struct AssocDialogState {
    has_parent: bool,
    lang: ViewerLanguage,
    dpi: u32,
    dark: bool,
    checked: [bool; 8],
    group_badges: [GroupBadge; 8],
    locked_group_count: usize,
    status_banner: Option<(String, bool)>,
    focused_item: HitItem,
    focus_visible: bool,
    hovered: Option<HitItem>,
    pressed: Option<HitItem>,
    tracking_mouse: bool,
    layout: DialogLayout,
    d2d_factory: ID2D1Factory1,
    dwrite_factory: IDWriteFactory,
    wic_factory: Option<IWICImagingFactory>,
    rt: Option<ID2D1HwndRenderTarget>,
    solid_brush: Option<ID2D1SolidColorBrush>,
    doc_bmp: Option<ID2D1Bitmap>,
    app_bmp: Option<ID2D1Bitmap>,
    formats: TextFormats,
    check_stroke: Option<ID2D1StrokeStyle1>,
    doc_icon: HICON,
    app_icon: HICON,
    cursor_arrow: HCURSOR,
    cursor_hand: HCURSOR,
    done: bool,
    outcome: AssocDialogOutcome,
}

fn to_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

fn rect_f(left: f32, top: f32, right: f32, bottom: f32) -> D2D_RECT_F {
    D2D_RECT_F {
        left,
        top,
        right,
        bottom,
    }
}

fn pt_in_rect(x: f32, y: f32, r: D2D_RECT_F) -> bool {
    x >= r.left && x <= r.right && y >= r.top && y <= r.bottom
}

pub(crate) fn detect_system_dark_theme() -> bool {
    unsafe {
        let sub = to_wide("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize");
        let mut hkey = HKEY::default();
        if RegOpenKeyExW(
            HKEY_CURRENT_USER,
            PCWSTR(sub.as_ptr()),
            0,
            KEY_READ,
            &mut hkey,
        )
        .is_err()
        {
            return false;
        }
        let val_name = to_wide("AppsUseLightTheme");
        let mut val: u32 = 1;
        let mut sz = std::mem::size_of::<u32>() as u32;
        let ok = RegQueryValueExW(
            hkey,
            PCWSTR(val_name.as_ptr()),
            None,
            None,
            Some(&mut val as *mut u32 as *mut u8),
            Some(&mut sz),
        )
        .is_ok();
        let _ = RegCloseKey(hkey);
        ok && val == 0
    }
}

/// 逐级探测系统已安装的字体家族（如 Cascadia Code -> Consolas -> Segoe UI），杜绝精简系统字体缺失
fn resolve_font_chain<'a>(dw: &IDWriteFactory, candidates: &[&'a str]) -> &'a str {
    unsafe {
        let mut collection = None;
        if dw.GetSystemFontCollection(&mut collection, false).is_ok() {
            if let Some(col) = collection {
                for &fam in candidates {
                    let w = to_wide(fam);
                    let mut idx = 0u32;
                    let mut exists = windows::Win32::Foundation::BOOL(0);
                    if col
                        .FindFamilyName(PCWSTR(w.as_ptr()), &mut idx, &mut exists)
                        .is_ok()
                        && exists.as_bool()
                    {
                        return fam;
                    }
                }
            }
        }
    }
    candidates.last().copied().unwrap_or("Segoe UI")
}

/// 精确探测目标监视器的物理有效 DPI（兼容多屏混合 100%~250%+ 缩放）
fn query_monitor_dpi(mon: HMONITOR) -> u32 {
    unsafe {
        let mut dx = 0u32;
        let mut dy = 0u32;
        if GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &mut dx, &mut dy).is_ok() && dx >= 96 {
            return dx.clamp(96, 384);
        }
        GetDpiForSystem().max(96)
    }
}

/// 查询 8 组格式在当前 Windows 系统下的真实关联/UserChoice 锁定状态
fn refresh_group_assoc_badges() -> ([GroupBadge; 8], usize) {
    let mut badges = [GroupBadge::None; 8];
    let mut locked_groups = 0usize;
    let registered = query_registered_extensions();
    for (i, grp) in FORMAT_GROUPS.iter().enumerate() {
        let mut any_active = false;
        let mut any_locked = false;
        let any_reg = grp.exts.iter().any(|e| registered.contains(*e));
        for &ext in grp.exts {
            match query_extension_assoc_status(ext) {
                ExtAssocStatus::ActiveTools3000 => any_active = true,
                ExtAssocStatus::LockedByUserChoice => any_locked = true,
                ExtAssocStatus::NotAssociated => {}
            }
        }
        if any_locked {
            badges[i] = GroupBadge::LockedByOs;
            locked_groups += 1;
        } else if any_active || any_reg {
            badges[i] = GroupBadge::Active;
        }
    }
    (badges, locked_groups)
}

#[allow(clippy::too_many_arguments)]
fn make_text_format(
    dw: &IDWriteFactory,
    family: &str,
    locale: &str,
    size: f32,
    weight: windows::Win32::Graphics::DirectWrite::DWRITE_FONT_WEIGHT,
    align: windows::Win32::Graphics::DirectWrite::DWRITE_TEXT_ALIGNMENT,
    valign: windows::Win32::Graphics::DirectWrite::DWRITE_PARAGRAPH_ALIGNMENT,
    wrap: bool,
) -> Result<IDWriteTextFormat, String> {
    unsafe {
        let fam_w = to_wide(family);
        let loc_w = to_wide(locale);
        let fmt = dw
            .CreateTextFormat(
                PCWSTR(fam_w.as_ptr()),
                None,
                weight,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                size.max(12.0),
                PCWSTR(loc_w.as_ptr()),
            )
            .map_err(|e| format!("CreateTextFormat failed: {e}"))?;
        let _ = fmt.SetTextAlignment(align);
        let _ = fmt.SetParagraphAlignment(valign);
        let _ = fmt.SetWordWrapping(if wrap {
            DWRITE_WORD_WRAPPING_WRAP
        } else {
            DWRITE_WORD_WRAPPING_NO_WRAP
        });
        if !wrap {
            let trim = DWRITE_TRIMMING {
                granularity: DWRITE_TRIMMING_GRANULARITY_CHARACTER,
                delimiter: 0,
                delimiterCount: 0,
            };
            if let Ok(ellipsis) = dw.CreateEllipsisTrimmingSign(&fmt) {
                let _ = fmt.SetTrimming(&trim, &ellipsis);
            }
        }
        Ok(fmt)
    }
}

fn build_text_formats(dw: &IDWriteFactory, lang: ViewerLanguage) -> Result<TextFormats, String> {
    let font_face = resolve_font_chain(
        dw,
        &[lang.preferred_cjk_fallback_font(), "Segoe UI Variable Text", "Segoe UI"],
    );
    let mono_face = resolve_font_chain(dw, &["Cascadia Code", "Consolas", "Segoe UI"]);
    let locale = lang.tag();
    // 严格遵守排版红线：全部字号 >= 12.0 DIP，全部字重 >= 500 Medium
    Ok(TextFormats {
        title: make_text_format(
            dw,
            font_face,
            locale,
            16.5,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            false,
        )?,
        subtitle: make_text_format(
            dw,
            font_face,
            locale,
            12.4,
            DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
            true,
        )?,
        badge: make_text_format(
            dw,
            font_face,
            locale,
            12.0,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            false,
        )?,
        row_title: make_text_format(
            dw,
            font_face,
            locale,
            12.8,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            false,
        )?,
        row_ext: make_text_format(
            dw,
            mono_face,
            locale,
            12.0,
            DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            false,
        )?,
        card_caption: make_text_format(
            dw,
            font_face,
            locale,
            12.6,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            false,
        )?,
        status: make_text_format(
            dw,
            font_face,
            locale,
            12.2,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            false,
        )?,
        btn: make_text_format(
            dw,
            font_face,
            locale,
            12.5,
            DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            false,
        )?,
        btn_bold: make_text_format(
            dw,
            font_face,
            locale,
            12.5,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
            false,
        )?,
    })
}

fn measure_text_width(dw: &IDWriteFactory, fmt: &IDWriteTextFormat, text: &str) -> f32 {
    unsafe {
        let wide: Vec<u16> = text.encode_utf16().collect();
        if wide.is_empty() {
            return 0.0;
        }
        if let Ok(layout) = dw.CreateTextLayout(&wide, fmt, 1200.0, 48.0) {
            let mut metrics = Default::default();
            if layout.GetMetrics(&mut metrics).is_ok() {
                return metrics.widthIncludingTrailingWhitespace;
            }
        }
        text.chars().count() as f32 * 8.0
    }
}

fn compute_layout(dw: &IDWriteFactory, formats: &TextFormats, lang: ViewerLanguage) -> DialogLayout {
    let s = lang.strings();
    let pad_x = 24.0;
    let body_top = 96.0;
    let body_h = 376.0;
    let left_w = 228.0;
    let col_gap = 16.0;

    let left_card = rect_f(pad_x, body_top, pad_x + left_w, body_top + body_h);
    let right_left = left_card.right + col_gap;
    let right_card = rect_f(right_left, body_top, DLG_W_DIP - pad_x, body_top + body_h);

    let lbtn_pad = 16.0;
    let lbtn_h = 30.0;
    let lbtn_gap = 8.0;
    let lbtn_l = left_card.left + lbtn_pad;
    let lbtn_r = left_card.right - lbtn_pad;
    let btn_unreg_b = left_card.bottom - 16.0;
    let btn_unreg = rect_f(lbtn_l, btn_unreg_b - lbtn_h, lbtn_r, btn_unreg_b);
    let btn_common = rect_f(
        lbtn_l,
        btn_unreg.top - lbtn_gap - lbtn_h,
        lbtn_r,
        btn_unreg.top - lbtn_gap,
    );
    let btn_all = rect_f(
        lbtn_l,
        btn_common.top - lbtn_gap - lbtn_h,
        lbtn_r,
        btn_common.top - lbtn_gap,
    );

    let mut rows = [rect_f(0.0, 0.0, 0.0, 0.0); 8];
    let row_pad_x = 10.0;
    let row_pad_y = 10.0;
    let row_gap = 3.0;
    let row_h = (body_h - row_pad_y * 2.0 - row_gap * 7.0) / 8.0;
    for (i, slot) in rows.iter_mut().enumerate() {
        let top = right_card.top + row_pad_y + i as f32 * (row_h + row_gap);
        *slot = rect_f(
            right_card.left + row_pad_x,
            top,
            right_card.right - row_pad_x,
            top + row_h,
        );
    }

    // 独立整宽状态反馈横幅槽位（展示操作结果或 UserChoice 锁定摘要，与底栏按钮 100% 物理零重叠）
    let status_strip = rect_f(
        pad_x,
        body_top + body_h + 10.0,
        DLG_W_DIP - pad_x,
        body_top + body_h + 40.0,
    );

    let footer_y = status_strip.bottom + 12.0;
    let btn_h = 36.0;
    let btn_y = footer_y + ((DLG_H_DIP - footer_y) - btn_h) * 0.5;
    let btn_gap = 10.0;

    let mut w_close =
        (measure_text_width(dw, &formats.btn, s.assoc_btn_close) + 34.0).clamp(84.0, 140.0);
    let mut w_def = (measure_text_width(dw, &formats.btn_bold, s.assoc_btn_apply_default) + 36.0)
        .clamp(190.0, 360.0);
    let mut w_only =
        (measure_text_width(dw, &formats.btn, s.assoc_btn_apply_only) + 34.0).clamp(150.0, 310.0);

    let max_footer_w = DLG_W_DIP - pad_x * 2.0;
    let total_w = w_close + w_def + w_only + btn_gap * 2.0;
    if total_w > max_footer_w {
        let scale = (max_footer_w - btn_gap * 2.0) / (w_close + w_def + w_only);
        w_close *= scale;
        w_def *= scale;
        w_only *= scale;
    }

    let mut rx = DLG_W_DIP - pad_x;
    let btn_close = rect_f(rx - w_close, btn_y, rx, btn_y + btn_h);
    rx = btn_close.left - btn_gap;
    let btn_apply_default = rect_f(rx - w_def, btn_y, rx, btn_y + btn_h);
    rx = btn_apply_default.left - btn_gap;
    let btn_apply_only = rect_f(rx - w_only, btn_y, rx, btn_y + btn_h);

    DialogLayout {
        left_card,
        right_card,
        rows,
        btn_all,
        btn_common,
        btn_unreg,
        status_strip,
        btn_apply_only,
        btn_apply_default,
        btn_close,
        footer_y,
    }
}

fn hicon_to_d2d_bitmap(
    wic: &IWICImagingFactory,
    rt: &ID2D1RenderTarget,
    hicon: HICON,
) -> Option<ID2D1Bitmap> {
    if hicon.is_invalid() {
        return None;
    }
    unsafe {
        let bmp = wic.CreateBitmapFromHICON(hicon).ok()?;
        let conv = wic.CreateFormatConverter().ok()?;
        conv.Initialize(
            &bmp,
            &GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            None,
            0.0,
            WICBitmapPaletteTypeCustom,
        )
        .ok()?;
        rt.CreateBitmapFromWicBitmap(&conv, None).ok()
    }
}

unsafe fn draw_text_with(
    rt: &ID2D1RenderTarget,
    brush: &ID2D1SolidColorBrush,
    text: &str,
    fmt: &IDWriteTextFormat,
    bounds: D2D_RECT_F,
    color: D2D1_COLOR_F,
) {
    brush.SetColor(&color);
    let wide: Vec<u16> = text.encode_utf16().collect();
    rt.DrawText(
        &wide,
        fmt,
        &bounds,
        brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP,
        DWRITE_MEASURING_MODE_NATURAL,
    );
}

unsafe fn draw_focus_ring(
    rt: &ID2D1RenderTarget,
    brush: &ID2D1SolidColorBrush,
    rect: D2D_RECT_F,
    radius: f32,
    color: D2D1_COLOR_F,
) {
    let ring_rc = rect_f(
        rect.left - 2.0,
        rect.top - 2.0,
        rect.right + 2.0,
        rect.bottom + 2.0,
    );
    let rr = D2D1_ROUNDED_RECT {
        rect: ring_rc,
        radiusX: radius + 2.0,
        radiusY: radius + 2.0,
    };
    brush.SetColor(&color);
    rt.DrawRoundedRectangle(&rr, brush, 2.0, None);
}

#[allow(clippy::too_many_arguments)]
unsafe fn draw_button_with(
    rt: &ID2D1RenderTarget,
    brush: &ID2D1SolidColorBrush,
    rect: D2D_RECT_F,
    label: &str,
    fmt: &IDWriteTextFormat,
    bg: D2D1_COLOR_F,
    border: Option<D2D1_COLOR_F>,
    text_color: D2D1_COLOR_F,
    radius: f32,
    focused: bool,
    focus_color: D2D1_COLOR_F,
) {
    let rr = D2D1_ROUNDED_RECT {
        rect,
        radiusX: radius,
        radiusY: radius,
    };
    brush.SetColor(&bg);
    rt.FillRoundedRectangle(&rr, brush);
    if let Some(bc) = border {
        brush.SetColor(&bc);
        rt.DrawRoundedRectangle(&rr, brush, 1.0, None);
    }
    if focused {
        draw_focus_ring(rt, brush, rect, radius, focus_color);
    }
    let text_rect = rect_f(rect.left + 8.0, rect.top, rect.right - 8.0, rect.bottom);
    draw_text_with(rt, brush, label, fmt, text_rect, text_color);
}

struct PaintAssocArgs<'a> {
    rt: &'a ID2D1RenderTarget,
    brush: &'a ID2D1SolidColorBrush,
    dw: &'a IDWriteFactory,
    formats: &'a TextFormats,
    check_stroke: Option<&'a ID2D1StrokeStyle>,
    layout: &'a DialogLayout,
    lang: ViewerLanguage,
    dark: bool,
    checked: &'a [bool; 8],
    group_badges: &'a [GroupBadge; 8],
    locked_group_count: usize,
    status_banner: Option<(&'a str, bool)>,
    focused_item: HitItem,
    focus_visible: bool,
    hovered: Option<HitItem>,
    pressed: Option<HitItem>,
    doc_bmp: Option<&'a ID2D1Bitmap>,
    app_bmp: Option<&'a ID2D1Bitmap>,
}

fn paint_assoc_ui(a: &PaintAssocArgs<'_>) {
    let rt = a.rt;
    let br = a.brush;
    let dw = a.dw;
    let formats = a.formats;
    let layout = a.layout;
    let pal = palette_for(a.dark);
    let s = a.lang.strings();

    let mut selected_ext_count = 0usize;
    for (i, grp) in FORMAT_GROUPS.iter().enumerate() {
        if a.checked[i] {
            selected_ext_count += grp.exts.len();
        }
    }
    let total_ext_count = SUPPORTED_EXTENSIONS.len();

    unsafe {
        rt.Clear(Some(&pal.canvas));

        // 1. 顶部 Header（图标 + 主标题 + Rust 复刻边界徽章 + 副标题）
        let header_x = 24.0;
        let icon_y = 18.0;
        let icon_sz = 32.0;
        if let Some(bmp) = a.app_bmp {
            let dst = rect_f(header_x, icon_y, header_x + icon_sz, icon_y + icon_sz);
            rt.DrawBitmap(
                bmp,
                Some(&dst),
                1.0,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                None,
            );
        }

        let title_x = header_x + icon_sz + 12.0;
        let short_title = "Tools3000 Picasa Viewer";
        let title_w = measure_text_width(dw, &formats.title, short_title) + 8.0;
        draw_text_with(
            rt,
            br,
            short_title,
            &formats.title,
            rect_f(title_x, 16.0, title_x + title_w, 42.0),
            pal.text_primary,
        );

        let badge_w = (measure_text_width(dw, &formats.badge, s.assoc_scope_badge) + 22.0)
            .min(DLG_W_DIP - 24.0 - (title_x + title_w + 10.0));
        if badge_w > 60.0 {
            let badge_l = title_x + title_w + 10.0;
            let badge_rc = rect_f(badge_l, 18.0, badge_l + badge_w, 41.0);
            let rr = D2D1_ROUNDED_RECT {
                rect: badge_rc,
                radiusX: 11.5,
                radiusY: 11.5,
            };
            br.SetColor(&pal.accent_soft_bg);
            rt.FillRoundedRectangle(&rr, br);
            br.SetColor(&pal.accent_soft_border);
            rt.DrawRoundedRectangle(&rr, br, 1.0, None);
            draw_text_with(rt, br, s.assoc_scope_badge, &formats.badge, badge_rc, pal.accent);
        }

        draw_text_with(
            rt,
            br,
            s.assoc_dlg_subtitle,
            &formats.subtitle,
            rect_f(title_x, 45.0, DLG_W_DIP - 24.0, 90.0),
            pal.text_secondary,
        );

        // 2. 左侧预览卡片与快捷操作
        let left_rr = D2D1_ROUNDED_RECT {
            rect: layout.left_card,
            radiusX: 12.0,
            radiusY: 12.0,
        };
        br.SetColor(&pal.card);
        rt.FillRoundedRectangle(&left_rr, br);
        br.SetColor(&pal.border);
        rt.DrawRoundedRectangle(&left_rr, br, 1.0, None);

        let card_cx = (layout.left_card.left + layout.left_card.right) * 0.5;
        let doc_sz = 114.0;
        let doc_l = card_cx - doc_sz * 0.5;
        let doc_t = layout.left_card.top + 22.0;

        br.SetColor(&pal.shadow);
        let ell = D2D1_ELLIPSE {
            point: D2D_POINT_2F {
                x: card_cx,
                y: doc_t + doc_sz - 2.0,
            },
            radiusX: 44.0,
            radiusY: 7.5,
        };
        rt.FillEllipse(&ell, br);

        if let Some(bmp) = a.doc_bmp {
            let dst = rect_f(doc_l, doc_t, doc_l + doc_sz, doc_t + doc_sz);
            rt.DrawBitmap(
                bmp,
                Some(&dst),
                1.0,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                None,
            );
        }
        if let Some(bmp) = a.app_bmp {
            let badge_sz = 34.0;
            let bx = doc_l + doc_sz - 24.0;
            let by = doc_t + doc_sz - 26.0;
            let dst = rect_f(bx, by, bx + badge_sz, by + badge_sz);
            rt.DrawBitmap(
                bmp,
                Some(&dst),
                1.0,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                None,
            );
        }

        draw_text_with(
            rt,
            br,
            s.assoc_icon_caption,
            &formats.card_caption,
            rect_f(
                layout.left_card.left + 12.0,
                doc_t + doc_sz + 14.0,
                layout.left_card.right - 12.0,
                doc_t + doc_sz + 36.0,
            ),
            pal.text_primary,
        );

        let count_text = format!("{selected_ext_count} / {total_ext_count}");
        let pill_w = (measure_text_width(dw, &formats.badge, &count_text) + 26.0).max(88.0);
        let pill_t = doc_t + doc_sz + 44.0;
        let pill_rc = rect_f(
            card_cx - pill_w * 0.5,
            pill_t,
            card_cx + pill_w * 0.5,
            pill_t + 24.0,
        );
        let pill_rr = D2D1_ROUNDED_RECT {
            rect: pill_rc,
            radiusX: 12.0,
            radiusY: 12.0,
        };
        br.SetColor(&pal.accent_soft_bg);
        rt.FillRoundedRectangle(&pill_rr, br);
        br.SetColor(&pal.accent_soft_border);
        rt.DrawRoundedRectangle(&pill_rr, br, 1.0, None);
        draw_text_with(rt, br, &count_text, &formats.badge, pill_rc, pal.accent);

        let draw_side_btn = |item: HitItem, r: D2D_RECT_F, text: &str, danger: bool| {
            let is_press = a.pressed == Some(item);
            let is_hover = a.hovered == Some(item);
            let is_focus = a.focus_visible && a.focused_item == item;
            let (bg, border, fg) = if danger {
                let bg = if is_press || is_hover {
                    pal.danger_hover_bg
                } else {
                    pal.well
                };
                (bg, Some(pal.danger_border), pal.danger_text)
            } else {
                let bg = if is_press {
                    pal.btn_pressed
                } else if is_hover {
                    pal.btn_hover
                } else {
                    pal.well
                };
                (bg, Some(pal.border), pal.text_primary)
            };
            draw_button_with(
                rt,
                br,
                r,
                text,
                &formats.btn,
                bg,
                border,
                fg,
                7.0,
                is_focus,
                pal.focus_ring,
            );
        };

        draw_side_btn(HitItem::BtnSelectAll, layout.btn_all, s.assoc_btn_select_all, false);
        draw_side_btn(HitItem::BtnCommon, layout.btn_common, s.assoc_btn_common, false);
        draw_side_btn(HitItem::BtnUnregister, layout.btn_unreg, s.assoc_btn_remove_all, true);

        // 3. 右侧格式族卡片容器
        let right_rr = D2D1_ROUNDED_RECT {
            rect: layout.right_card,
            radiusX: 12.0,
            radiusY: 12.0,
        };
        br.SetColor(&pal.card);
        rt.FillRoundedRectangle(&right_rr, br);
        br.SetColor(&pal.border);
        rt.DrawRoundedRectangle(&right_rr, br, 1.0, None);

        // 8 行复选框 + 8 语种标题 + 扩展名清单 + UserChoice 状态微晶胶囊 + 独立键盘焦点环
        for (i, grp) in FORMAT_GROUPS.iter().enumerate() {
            let r = layout.rows[i];
            let is_checked = a.checked[i];
            let is_hover = a.hovered == Some(HitItem::Row(i));
            let is_press = a.pressed == Some(HitItem::Row(i));
            let is_focus = a.focus_visible && a.focused_item == HitItem::Row(i);

            let row_rr = D2D1_ROUNDED_RECT {
                rect: r,
                radiusX: 8.0,
                radiusY: 8.0,
            };
            if is_press || is_hover {
                br.SetColor(&pal.row_hover);
                rt.FillRoundedRectangle(&row_rr, br);
            } else if is_checked {
                br.SetColor(&pal.row_checked);
                rt.FillRoundedRectangle(&row_rr, br);
            }

            if is_checked {
                br.SetColor(&pal.accent_soft_border);
                rt.DrawRoundedRectangle(&row_rr, br, 1.0, None);
            } else if is_hover {
                br.SetColor(&pal.border);
                rt.DrawRoundedRectangle(&row_rr, br, 1.0, None);
            }
            if is_focus {
                draw_focus_ring(rt, br, r, 8.0, pal.focus_ring);
            }

            // 18x18 抗锯齿圆角复选框
            let cb_sz = 18.0;
            let cb_l = r.left + 12.0;
            let cb_t = (r.top + r.bottom - cb_sz) * 0.5;
            let cb_rc = rect_f(cb_l, cb_t, cb_l + cb_sz, cb_t + cb_sz);
            let cb_rr = D2D1_ROUNDED_RECT {
                rect: cb_rc,
                radiusX: 5.0,
                radiusY: 5.0,
            };
            if is_checked {
                br.SetColor(&pal.accent);
                rt.FillRoundedRectangle(&cb_rr, br);
                br.SetColor(&pal.on_accent);
                let p1 = D2D_POINT_2F {
                    x: cb_l + 4.4,
                    y: cb_t + 9.2,
                };
                let p2 = D2D_POINT_2F {
                    x: cb_l + 7.6,
                    y: cb_t + 12.6,
                };
                let p3 = D2D_POINT_2F {
                    x: cb_l + 13.8,
                    y: cb_t + 5.6,
                };
                rt.DrawLine(p1, p2, br, 2.0, a.check_stroke);
                rt.DrawLine(p2, p3, br, 2.0, a.check_stroke);
            } else {
                br.SetColor(&pal.card);
                rt.FillRoundedRectangle(&cb_rr, br);
                let stroke_c = if is_hover { pal.accent } else { pal.border_strong };
                br.SetColor(&stroke_c);
                rt.DrawRoundedRectangle(&cb_rr, br, 1.4, None);
            }

            // 右侧系统关联 / UserChoice 锁定状态徽章
            let mut ext_right = r.right - 12.0;
            let badge_spec = match a.group_badges[i] {
                GroupBadge::Active => Some((s.assoc_badge_default, pal.ok_bg, pal.ok_border, pal.ok_text)),
                GroupBadge::LockedByOs => Some((
                    s.assoc_badge_locked,
                    pal.warn_bg,
                    pal.warn_border,
                    pal.warn_text,
                )),
                GroupBadge::None => None,
            };
            if let Some((b_text, b_bg, b_bd, b_fg)) = badge_spec {
                let bw = (measure_text_width(dw, &formats.badge, b_text) + 16.0).clamp(58.0, 146.0);
                let bh = 22.0;
                let bt = (r.top + r.bottom - bh) * 0.5;
                let bl = ext_right - bw;
                let b_rc = rect_f(bl, bt, ext_right, bt + bh);
                let b_rr = D2D1_ROUNDED_RECT {
                    rect: b_rc,
                    radiusX: 11.0,
                    radiusY: 11.0,
                };
                br.SetColor(&b_bg);
                rt.FillRoundedRectangle(&b_rr, br);
                br.SetColor(&b_bd);
                rt.DrawRoundedRectangle(&b_rr, br, 1.0, None);
                draw_text_with(rt, br, b_text, &formats.badge, b_rc, b_fg);
                ext_right = bl - 10.0;
            }

            // 8 语种本地化格式名称 + 等宽扩展名列表
            let grp_title = s.assoc_grp_titles[i];
            let text_l = cb_rc.right + 12.0;
            let title_w = measure_text_width(dw, &formats.row_title, grp_title).min(176.0);
            draw_text_with(
                rt,
                br,
                grp_title,
                &formats.row_title,
                rect_f(text_l, r.top, text_l + title_w + 6.0, r.bottom),
                pal.text_primary,
            );

            let ext_l = (text_l + title_w + 12.0).max(r.left + 188.0);
            if ext_l + 36.0 < ext_right {
                draw_text_with(
                    rt,
                    br,
                    grp.exts_preview,
                    &formats.row_ext,
                    rect_f(ext_l, r.top, ext_right, r.bottom),
                    pal.text_muted,
                );
            }
        }

        // 4. 整宽状态反馈横幅（优先展示最新操作结果；否则当存在 Win10/11 UserChoice 锁定时提示用户）
        let banner_data: Option<(String, D2D1_COLOR_F, D2D1_COLOR_F, D2D1_COLOR_F)> =
            if let Some((msg, is_err)) = a.status_banner {
                if is_err {
                    Some((
                        msg.to_string(),
                        pal.danger_hover_bg,
                        pal.danger_border,
                        pal.danger_text,
                    ))
                } else {
                    Some((msg.to_string(), pal.ok_bg, pal.ok_border, pal.ok_text))
                }
            } else if a.locked_group_count > 0 {
                Some((
                    format!(
                        "{}{} / {}",
                        s.assoc_locked_summary_prefix,
                        a.locked_group_count,
                        FORMAT_GROUPS.len()
                    ),
                    pal.warn_bg,
                    pal.warn_border,
                    pal.warn_text,
                ))
            } else {
                None
            };
        if let Some((txt, bg, bd, fg)) = banner_data {
            let sr = layout.status_strip;
            let srr = D2D1_ROUNDED_RECT {
                rect: sr,
                radiusX: 8.0,
                radiusY: 8.0,
            };
            br.SetColor(&bg);
            rt.FillRoundedRectangle(&srr, br);
            br.SetColor(&bd);
            rt.DrawRoundedRectangle(&srr, br, 1.0, None);
            draw_text_with(
                rt,
                br,
                &txt,
                &formats.status,
                rect_f(sr.left + 12.0, sr.top, sr.right - 12.0, sr.bottom),
                fg,
            );
        }

        // 5. 底部分隔线与 3 个主操作按钮
        br.SetColor(&pal.border);
        rt.DrawLine(
            D2D_POINT_2F {
                x: 24.0,
                y: layout.footer_y,
            },
            D2D_POINT_2F {
                x: DLG_W_DIP - 24.0,
                y: layout.footer_y,
            },
            br,
            1.0,
            None,
        );

        {
            let item = HitItem::BtnApplyOnly;
            let bg = match (a.pressed == Some(item), a.hovered == Some(item)) {
                (true, _) => pal.btn_pressed,
                (false, true) => pal.btn_hover,
                _ => pal.btn_bg,
            };
            draw_button_with(
                rt,
                br,
                layout.btn_apply_only,
                s.assoc_btn_apply_only,
                &formats.btn,
                bg,
                Some(pal.border_strong),
                pal.text_primary,
                8.0,
                a.focus_visible && a.focused_item == item,
                pal.focus_ring,
            );
        }

        {
            let item = HitItem::BtnApplyDefault;
            let bg = match (a.pressed == Some(item), a.hovered == Some(item)) {
                (true, _) => pal.accent_pressed,
                (false, true) => pal.accent_hover,
                _ => pal.accent,
            };
            draw_button_with(
                rt,
                br,
                layout.btn_apply_default,
                s.assoc_btn_apply_default,
                &formats.btn_bold,
                bg,
                None,
                pal.on_accent,
                8.0,
                a.focus_visible && a.focused_item == item,
                pal.focus_ring,
            );
        }

        {
            let item = HitItem::BtnClose;
            let bg = match (a.pressed == Some(item), a.hovered == Some(item)) {
                (true, _) => pal.btn_pressed,
                (false, true) => pal.btn_hover,
                _ => pal.btn_bg,
            };
            draw_button_with(
                rt,
                br,
                layout.btn_close,
                s.assoc_btn_close,
                &formats.btn,
                bg,
                Some(pal.border),
                pal.text_secondary,
                8.0,
                a.focus_visible && a.focused_item == item,
                pal.focus_ring,
            );
        }
    }
}

fn hit_test(layout: &DialogLayout, x: f32, y: f32) -> Option<HitItem> {
    for (i, &r) in layout.rows.iter().enumerate() {
        if pt_in_rect(x, y, r) {
            return Some(HitItem::Row(i));
        }
    }
    if pt_in_rect(x, y, layout.btn_all) {
        return Some(HitItem::BtnSelectAll);
    }
    if pt_in_rect(x, y, layout.btn_common) {
        return Some(HitItem::BtnCommon);
    }
    if pt_in_rect(x, y, layout.btn_unreg) {
        return Some(HitItem::BtnUnregister);
    }
    if pt_in_rect(x, y, layout.btn_apply_only) {
        return Some(HitItem::BtnApplyOnly);
    }
    if pt_in_rect(x, y, layout.btn_apply_default) {
        return Some(HitItem::BtnApplyDefault);
    }
    if pt_in_rect(x, y, layout.btn_close) {
        return Some(HitItem::BtnClose);
    }
    None
}

fn ensure_class_registered() {
    static REG: Once = Once::new();
    REG.call_once(|| unsafe {
        let Ok(hmod) = GetModuleHandleW(None) else {
            return;
        };
        let hinst: HINSTANCE = hmod.into();
        let cls_w = to_wide(ASSOC_WINDOW_CLASS);
        let wc = WNDCLASSEXW {
            cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
            lpfnWndProc: Some(assoc_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(cls_w.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&wc);
    });
}

fn load_embedded_icons(hinst: HINSTANCE) -> (HICON, HICON) {
    unsafe {
        let doc_icon = LoadImageW(
            hinst,
            PCWSTR(2usize as *const u16),
            IMAGE_ICON,
            256,
            256,
            LR_DEFAULTCOLOR,
        )
        .map(|h| HICON(h.0))
        .unwrap_or_default();
        let app_icon = LoadImageW(
            hinst,
            PCWSTR(1usize as *const u16),
            IMAGE_ICON,
            128,
            128,
            LR_DEFAULTCOLOR,
        )
        .map(|h| HICON(h.0))
        .unwrap_or_default();
        (doc_icon, app_icon)
    }
}

/// 离屏渲染文件关联面板至高清 PNG（供自动化高分屏视觉与深浅主题验收，无窗口零依赖）
pub fn dump_assoc_dialog_preview(
    out_png: &Path,
    lang: ViewerLanguage,
    dpi: u32,
    dark: bool,
) -> Result<(), String> {
    unsafe {
        let com_ok = CoInitializeEx(None, COINIT_APARTMENTTHREADED).is_ok();
        let dpi = dpi.clamp(96, 288);
        let phys_w = ((DLG_W_DIP * dpi as f32 / 96.0).round() as u32).max(1);
        let phys_h = ((DLG_H_DIP * dpi as f32 / 96.0).round() as u32).max(1);

        let d2d_factory: ID2D1Factory1 = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            Some(&D2D1_FACTORY_OPTIONS::default()),
        )
        .map_err(|e| format!("D2D1CreateFactory failed: {e}"))?;
        let dw: IDWriteFactory = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED)
            .map_err(|e| format!("DWriteCreateFactory failed: {e}"))?;
        let wic: IWICImagingFactory =
            CoCreateInstance(&CLSID_WICImagingFactory, None, CLSCTX_INPROC_SERVER)
                .map_err(|e| format!("WIC factory failed: {e}"))?;

        let formats = build_text_formats(&dw, lang)?;
        let layout = compute_layout(&dw, &formats, lang);

        let wic_bmp = wic
            .CreateBitmap(
                phys_w,
                phys_h,
                &GUID_WICPixelFormat32bppPBGRA,
                WICBitmapCacheOnLoad,
            )
            .map_err(|e| format!("CreateBitmap failed: {e}"))?;

        let rt_props = D2D1_RENDER_TARGET_PROPERTIES {
            r#type: D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            pixelFormat: D2D1_PIXEL_FORMAT {
                format: DXGI_FORMAT_B8G8R8A8_UNORM,
                alphaMode: D2D1_ALPHA_MODE_PREMULTIPLIED,
            },
            dpiX: dpi as f32,
            dpiY: dpi as f32,
            ..Default::default()
        };
        let rt: ID2D1RenderTarget = d2d_factory
            .CreateWicBitmapRenderTarget(&wic_bmp, &rt_props)
            .map_err(|e| format!("CreateWicBitmapRenderTarget failed: {e}"))?;
        rt.SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        let brush = rt
            .CreateSolidColorBrush(&rgba(255, 255, 255, 1.0), None)
            .map_err(|e| format!("CreateSolidColorBrush failed: {e}"))?;

        let hinst: HINSTANCE = GetModuleHandleW(None).map(|h| h.into()).unwrap_or_default();
        let (doc_icon, app_icon) = load_embedded_icons(hinst);
        let doc_bmp = hicon_to_d2d_bitmap(&wic, &rt, doc_icon);
        let app_bmp = hicon_to_d2d_bitmap(&wic, &rt, app_icon);

        let stroke_props = D2D1_STROKE_STYLE_PROPERTIES1 {
            startCap: D2D1_CAP_STYLE_ROUND,
            endCap: D2D1_CAP_STYLE_ROUND,
            lineJoin: D2D1_LINE_JOIN_ROUND,
            ..Default::default()
        };
        let check_stroke = d2d_factory.CreateStrokeStyle(&stroke_props, None).ok();
        let (group_badges, locked_group_count) = refresh_group_assoc_badges();

        let checked = [true, true, true, true, true, true, true, true];
        rt.BeginDraw();
        paint_assoc_ui(&PaintAssocArgs {
            rt: &rt,
            brush: &brush,
            dw: &dw,
            formats: &formats,
            check_stroke: check_stroke.as_ref().map(|v| &**v),
            layout: &layout,
            lang,
            dark,
            checked: &checked,
            group_badges: &group_badges,
            locked_group_count,
            status_banner: None,
            focused_item: HitItem::Row(0),
            focus_visible: true,
            hovered: Some(HitItem::Row(2)),
            pressed: None,
            doc_bmp: doc_bmp.as_ref(),
            app_bmp: app_bmp.as_ref(),
        });
        rt.EndDraw(None, None)
            .map_err(|e| format!("EndDraw failed: {e}"))?;

        if !doc_icon.is_invalid() {
            let _ = DestroyIcon(doc_icon);
        }
        if !app_icon.is_invalid() {
            let _ = DestroyIcon(app_icon);
        }

        let lock_rc = WICRect {
            X: 0,
            Y: 0,
            Width: phys_w as i32,
            Height: phys_h as i32,
        };
        let lock = wic_bmp
            .Lock(&lock_rc, WICBitmapLockRead.0 as u32)
            .map_err(|e| format!("WIC Lock failed: {e}"))?;
        let stride = lock
            .GetStride()
            .map_err(|e| format!("GetStride failed: {e}"))?;
        let mut buf_sz = 0u32;
        let mut ptr: *mut u8 = std::ptr::null_mut();
        lock.GetDataPointer(&mut buf_sz, &mut ptr)
            .map_err(|e| format!("GetDataPointer failed: {e}"))?;
        let raw = std::slice::from_raw_parts(ptr, buf_sz as usize);
        let mut rgba_out = vec![0u8; (phys_w * phys_h * 4) as usize];
        for y in 0..phys_h as usize {
            let src_row = &raw[y * stride as usize..y * stride as usize + phys_w as usize * 4];
            let dst_row =
                &mut rgba_out[y * phys_w as usize * 4..(y + 1) * phys_w as usize * 4];
            for x in 0..phys_w as usize {
                let s = x * 4;
                let a = src_row[s + 3];
                let (r, g, b) = if a == 0 {
                    (0, 0, 0)
                } else if a == 255 {
                    (src_row[s + 2], src_row[s + 1], src_row[s])
                } else {
                    let af = a as f32 / 255.0;
                    (
                        ((src_row[s + 2] as f32 / af).min(255.0)) as u8,
                        ((src_row[s + 1] as f32 / af).min(255.0)) as u8,
                        ((src_row[s] as f32 / af).min(255.0)) as u8,
                    )
                };
                dst_row[s] = r;
                dst_row[s + 1] = g;
                dst_row[s + 2] = b;
                dst_row[s + 3] = 255;
            }
        }
        drop(lock);
        if com_ok {
            CoUninitialize();
        }

        if let Some(p) = out_png.parent() {
            let _ = std::fs::create_dir_all(p);
        }
        let file = File::create(out_png).map_err(|e| format!("create png failed: {e}"))?;
        let mut enc = png::Encoder::new(BufWriter::new(file), phys_w, phys_h);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        let mut writer = enc
            .write_header()
            .map_err(|e| format!("png header failed: {e}"))?;
        writer
            .write_image_data(&rgba_out)
            .map_err(|e| format!("png write failed: {e}"))?;
        Ok(())
    }
}

/// 弹出文件关联与默认看图器配置窗口（单实例复用；`parent` 为空时作为 `--config` 独立窗口运行）
pub fn show_file_association_dialog(parent: HWND, lang: ViewerLanguage) -> AssocDialogOutcome {
    unsafe {
        let _ = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        // 单实例复用：若已存在打开的文件关联配置窗，直接激活置前，绝不叠开多窗
        let cls_w = to_wide(ASSOC_WINDOW_CLASS);
        if let Ok(existing) = FindWindowW(PCWSTR(cls_w.as_ptr()), PCWSTR::null()) {
            if !existing.0.is_null() && IsWindow(existing).as_bool() {
                let _ = ShowWindow(existing, SW_RESTORE);
                let _ = SetForegroundWindow(existing);
                return AssocDialogOutcome::Closed;
            }
        }

        let com_ok = CoInitializeEx(None, COINIT_APARTMENTTHREADED).is_ok();
        ensure_class_registered();

        let Ok(hmod) = GetModuleHandleW(None) else {
            if com_ok {
                CoUninitialize();
            }
            return AssocDialogOutcome::Closed;
        };
        let hinst: HINSTANCE = hmod.into();
        let has_parent = !parent.0.is_null() && IsWindow(parent).as_bool();

        let Ok(d2d_factory) = D2D1CreateFactory::<ID2D1Factory1>(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            Some(&D2D1_FACTORY_OPTIONS::default()),
        ) else {
            if com_ok {
                CoUninitialize();
            }
            return AssocDialogOutcome::Closed;
        };
        let Ok(dwrite_factory) = DWriteCreateFactory::<IDWriteFactory>(DWRITE_FACTORY_TYPE_SHARED)
        else {
            if com_ok {
                CoUninitialize();
            }
            return AssocDialogOutcome::Closed;
        };
        let Ok(formats) = build_text_formats(&dwrite_factory, lang) else {
            if com_ok {
                CoUninitialize();
            }
            return AssocDialogOutcome::Closed;
        };
        let wic_factory: Option<IWICImagingFactory> =
            CoCreateInstance(&CLSID_WICImagingFactory, None, CLSCTX_INPROC_SERVER).ok();

        let mon = if has_parent {
            MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST)
        } else {
            let mut pt = Default::default();
            let _ = GetCursorPos(&mut pt);
            MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)
        };
        let mut mi = MONITORINFO {
            cbSize: std::mem::size_of::<MONITORINFO>() as u32,
            ..Default::default()
        };
        let work_rc = if GetMonitorInfoW(mon, &mut mi).as_bool() {
            mi.rcWork
        } else {
            RECT {
                left: 0,
                top: 0,
                right: 1920,
                bottom: 1080,
            }
        };

        // 无父窗首开通过 GetDpiForMonitor 精准获取目标屏幕有效 DPI，彻底修复副屏缩放出界
        let dpi = if has_parent {
            GetDpiForWindow(parent).max(96)
        } else {
            query_monitor_dpi(mon)
        };

        let style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        let ex_style = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME;
        let mut wnd_rc = RECT {
            left: 0,
            top: 0,
            right: (DLG_W_DIP * dpi as f32 / 96.0).round() as i32,
            bottom: (DLG_H_DIP * dpi as f32 / 96.0).round() as i32,
        };
        let _ = AdjustWindowRectExForDpi(&mut wnd_rc, style, false, ex_style, dpi);
        let dlg_w = wnd_rc.right - wnd_rc.left;
        let dlg_h = wnd_rc.bottom - wnd_rc.top;
        let x = work_rc.left + ((work_rc.right - work_rc.left) - dlg_w) / 2;
        let y = work_rc.top + ((work_rc.bottom - work_rc.top) - dlg_h) / 2;

        let (doc_icon, app_icon) = load_embedded_icons(hinst);
        let dark = detect_system_dark_theme();
        let layout = compute_layout(&dwrite_factory, &formats, lang);

        let existing = query_registered_extensions();
        let first_time = existing.is_empty();
        let mut checked = [false; 8];
        for (i, grp) in FORMAT_GROUPS.iter().enumerate() {
            checked[i] = if first_time {
                true
            } else {
                grp.exts.iter().any(|e| existing.contains(*e))
            };
        }
        let (group_badges, locked_group_count) = refresh_group_assoc_badges();

        let stroke_props = D2D1_STROKE_STYLE_PROPERTIES1 {
            startCap: D2D1_CAP_STYLE_ROUND,
            endCap: D2D1_CAP_STYLE_ROUND,
            lineJoin: D2D1_LINE_JOIN_ROUND,
            ..Default::default()
        };
        let check_stroke = d2d_factory.CreateStrokeStyle(&stroke_props, None).ok();

        let mut state = Box::new(AssocDialogState {
            has_parent,
            lang,
            dpi,
            dark,
            checked,
            group_badges,
            locked_group_count,
            status_banner: None,
            focused_item: HitItem::Row(0),
            focus_visible: false,
            hovered: None,
            pressed: None,
            tracking_mouse: false,
            layout,
            d2d_factory,
            dwrite_factory,
            wic_factory,
            rt: None,
            solid_brush: None,
            doc_bmp: None,
            app_bmp: None,
            formats,
            check_stroke,
            doc_icon,
            app_icon,
            cursor_arrow: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            cursor_hand: LoadCursorW(None, IDC_HAND).unwrap_or_default(),
            done: false,
            outcome: AssocDialogOutcome::Closed,
        });

        let s = lang.strings();
        let title_w = to_wide(s.assoc_dlg_title);

        let Ok(hwnd) = CreateWindowExW(
            ex_style,
            PCWSTR(cls_w.as_ptr()),
            PCWSTR(title_w.as_ptr()),
            style,
            x,
            y,
            dlg_w,
            dlg_h,
            parent,
            None,
            hinst,
            None,
        ) else {
            if !state.doc_icon.is_invalid() {
                let _ = DestroyIcon(state.doc_icon);
            }
            if !state.app_icon.is_invalid() {
                let _ = DestroyIcon(state.app_icon);
            }
            if com_ok {
                CoUninitialize();
            }
            return AssocDialogOutcome::Closed;
        };

        let dark_val: i32 = if dark { 1 } else { 0 };
        let _ = DwmSetWindowAttribute(
            hwnd,
            DWMWA_USE_IMMERSIVE_DARK_MODE,
            &dark_val as *const i32 as *const _,
            std::mem::size_of::<i32>() as u32,
        );
        let corner_pref = DWMWCP_ROUND;
        let _ = DwmSetWindowAttribute(
            hwnd,
            DWMWA_WINDOW_CORNER_PREFERENCE,
            &corner_pref as *const _ as *const _,
            std::mem::size_of_val(&corner_pref) as u32,
        );

        let actual_dpi = GetDpiForWindow(hwnd).max(96);
        state.dpi = actual_dpi;

        SetWindowLongPtrW(
            hwnd,
            GWLP_USERDATA,
            (&mut *state as *mut AssocDialogState) as isize,
        );
        if !state.app_icon.is_invalid() {
            let _ = SendMessageW(
                hwnd,
                WM_SETICON,
                WPARAM(ICON_SMALL as usize),
                LPARAM(state.app_icon.0 as isize),
            );
            let _ = SendMessageW(
                hwnd,
                WM_SETICON,
                WPARAM(ICON_BIG as usize),
                LPARAM(state.app_icon.0 as isize),
            );
        }

        if has_parent {
            let _ = EnableWindow(parent, false);
        }
        let _ = ShowWindow(hwnd, SW_SHOW);
        let _ = SetForegroundWindow(hwnd);
        let _ = InvalidateRect(hwnd, None, false);

        let mut msg = MSG::default();
        while !state.done && IsWindow(hwnd).as_bool() {
            let ret = GetMessageW(&mut msg, None, 0, 0);
            if ret.0 <= 0 {
                break;
            }
            let _ = TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if has_parent {
            let _ = EnableWindow(parent, true);
            let _ = SetForegroundWindow(parent);
        }
        if IsWindow(hwnd).as_bool() {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            let _ = DestroyWindow(hwnd);
        }
        state.solid_brush = None;
        state.doc_bmp = None;
        state.app_bmp = None;
        state.rt = None;
        if !state.doc_icon.is_invalid() {
            let _ = DestroyIcon(state.doc_icon);
        }
        if !state.app_icon.is_invalid() {
            let _ = DestroyIcon(state.app_icon);
        }
        if com_ok {
            CoUninitialize();
        }

        state.outcome
    }
}

impl AssocDialogState {
    fn collect_checked_extensions(&self) -> Vec<&'static str> {
        let mut out = Vec::new();
        for (i, grp) in FORMAT_GROUPS.iter().enumerate() {
            if self.checked[i] {
                out.extend_from_slice(grp.exts);
            }
        }
        out
    }

    fn refresh_badges(&mut self) {
        let (badges, locked) = refresh_group_assoc_badges();
        self.group_badges = badges;
        self.locked_group_count = locked;
    }

    unsafe fn ensure_render_target(&mut self, hwnd: HWND) -> bool {
        if self.rt.is_some() && self.solid_brush.is_some() {
            return true;
        }
        let mut cli = RECT::default();
        let _ = GetClientRect(hwnd, &mut cli);
        let pw = (cli.right - cli.left).max(1) as u32;
        let ph = (cli.bottom - cli.top).max(1) as u32;
        let rt_props = D2D1_RENDER_TARGET_PROPERTIES {
            r#type: D2D1_RENDER_TARGET_TYPE_DEFAULT,
            pixelFormat: D2D1_PIXEL_FORMAT {
                format: DXGI_FORMAT_B8G8R8A8_UNORM,
                alphaMode: D2D1_ALPHA_MODE_IGNORE,
            },
            dpiX: self.dpi as f32,
            dpiY: self.dpi as f32,
            ..Default::default()
        };
        let hwnd_props = D2D1_HWND_RENDER_TARGET_PROPERTIES {
            hwnd,
            pixelSize: D2D_SIZE_U {
                width: pw,
                height: ph,
            },
            presentOptions: D2D1_PRESENT_OPTIONS_NONE,
        };
        let Ok(rt) = self.d2d_factory.CreateHwndRenderTarget(&rt_props, &hwnd_props) else {
            return false;
        };
        rt.SetDpi(self.dpi as f32, self.dpi as f32);
        rt.SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

        let base_rt: ID2D1RenderTarget = rt.clone().into();
        let Ok(brush) = base_rt.CreateSolidColorBrush(&rgba(255, 255, 255, 1.0), None) else {
            return false;
        };
        if let Some(wic) = self.wic_factory.as_ref() {
            self.doc_bmp = hicon_to_d2d_bitmap(wic, &base_rt, self.doc_icon);
            self.app_bmp = hicon_to_d2d_bitmap(wic, &base_rt, self.app_icon);
        }
        self.solid_brush = Some(brush);
        self.rt = Some(rt);
        true
    }

    unsafe fn activate_item(&mut self, hwnd: HWND, item: HitItem) {
        let s = self.lang.strings();
        match item {
            HitItem::Row(i) => {
                self.checked[i] = !self.checked[i];
                self.focused_item = HitItem::Row(i);
                let _ = InvalidateRect(hwnd, None, false);
            }
            HitItem::BtnSelectAll => {
                self.checked = [true; 8];
                self.focused_item = item;
                let _ = InvalidateRect(hwnd, None, false);
            }
            HitItem::BtnCommon => {
                for (i, grp) in FORMAT_GROUPS.iter().enumerate() {
                    self.checked[i] = grp.common_default;
                }
                self.focused_item = item;
                let _ = InvalidateRect(hwnd, None, false);
            }
            HitItem::BtnUnregister => {
                match unregister_user_file_associations() {
                    Ok(()) => {
                        self.outcome = AssocDialogOutcome::Unregistered;
                        self.checked = [false; 8];
                        self.refresh_badges();
                        self.status_banner = Some((s.assoc_banner_unregistered.to_string(), false));
                    }
                    Err(e) => {
                        self.status_banner =
                            Some((format!("{}{e}", s.assoc_banner_failed_prefix), true));
                        self.outcome = AssocDialogOutcome::Failed(e);
                    }
                }
                if self.has_parent {
                    self.done = true;
                    let _ = PostMessageW(hwnd, WM_CLOSE, WPARAM(0), LPARAM(0));
                } else {
                    let _ = InvalidateRect(hwnd, None, false);
                }
            }
            HitItem::BtnApplyOnly | HitItem::BtnApplyDefault => {
                let exts = self.collect_checked_extensions();
                if exts.is_empty() {
                    match unregister_user_file_associations() {
                        Ok(()) => {
                            self.outcome = AssocDialogOutcome::Unregistered;
                            self.refresh_badges();
                            self.status_banner =
                                Some((s.assoc_banner_unregistered.to_string(), false));
                        }
                        Err(e) => {
                            self.status_banner =
                                Some((format!("{}{e}", s.assoc_banner_failed_prefix), true));
                            self.outcome = AssocDialogOutcome::Failed(e);
                        }
                    }
                } else {
                    match register_selected_file_associations(&exts) {
                        Ok(()) => {
                            self.refresh_badges();
                            if item == HitItem::BtnApplyDefault {
                                open_windows_default_apps_settings(hwnd);
                                self.status_banner = Some((
                                    s.assoc_banner_applied_open_settings.to_string(),
                                    false,
                                ));
                            } else {
                                self.status_banner =
                                    Some((s.assoc_banner_applied.to_string(), false));
                            }
                            self.outcome = AssocDialogOutcome::Applied;
                        }
                        Err(e) => {
                            self.status_banner =
                                Some((format!("{}{e}", s.assoc_banner_failed_prefix), true));
                            self.outcome = AssocDialogOutcome::Failed(e);
                        }
                    }
                }
                if self.has_parent {
                    self.done = true;
                    let _ = PostMessageW(hwnd, WM_CLOSE, WPARAM(0), LPARAM(0));
                } else {
                    let _ = InvalidateRect(hwnd, None, false);
                }
            }
            HitItem::BtnClose => {
                self.done = true;
                let _ = PostMessageW(hwnd, WM_CLOSE, WPARAM(0), LPARAM(0));
            }
        }
    }
}

unsafe extern "system" fn assoc_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    let ptr = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *mut AssocDialogState;
    if !ptr.is_null() {
        let st = &mut *ptr;
        match msg {
            WM_ERASEBKGND => return LRESULT(1),
            WM_DPICHANGED => {
                let new_dpi = (((wparam.0 >> 16) & 0xFFFF) as u32).clamp(96, 384);
                st.dpi = new_dpi;
                if lparam.0 != 0 {
                    let suggested = *(lparam.0 as *const RECT);
                    let _ = SetWindowPos(
                        hwnd,
                        None,
                        suggested.left,
                        suggested.top,
                        (suggested.right - suggested.left).max(320),
                        (suggested.bottom - suggested.top).max(240),
                        SWP_NOZORDER | SWP_NOACTIVATE,
                    );
                }
                if let Some(rt) = st.rt.as_ref() {
                    rt.SetDpi(new_dpi as f32, new_dpi as f32);
                    let mut cli = RECT::default();
                    let _ = GetClientRect(hwnd, &mut cli);
                    let pw = (cli.right - cli.left).max(1) as u32;
                    let ph = (cli.bottom - cli.top).max(1) as u32;
                    let _ = rt.Resize(&D2D_SIZE_U {
                        width: pw,
                        height: ph,
                    });
                }
                let _ = InvalidateRect(hwnd, None, false);
                return LRESULT(0);
            }
            WM_SIZE => {
                let pw = (lparam.0 & 0xFFFF) as u32;
                let ph = ((lparam.0 >> 16) & 0xFFFF) as u32;
                if pw > 0 && ph > 0 {
                    if let Some(rt) = st.rt.as_ref() {
                        let _ = rt.Resize(&D2D_SIZE_U {
                            width: pw,
                            height: ph,
                        });
                    }
                    let _ = InvalidateRect(hwnd, None, false);
                }
                return LRESULT(0);
            }
            WM_SETCURSOR => {
                if (lparam.0 & 0xFFFF) == 1 {
                    let cur = if st.hovered.is_some() {
                        st.cursor_hand
                    } else {
                        st.cursor_arrow
                    };
                    if !cur.is_invalid() {
                        SetCursor(cur);
                        return LRESULT(1);
                    }
                }
            }
            WM_MOUSEMOVE => {
                if !st.tracking_mouse {
                    let mut tme = TRACKMOUSEEVENT {
                        cbSize: std::mem::size_of::<TRACKMOUSEEVENT>() as u32,
                        dwFlags: TME_LEAVE,
                        hwndTrack: hwnd,
                        dwHoverTime: 0,
                    };
                    let _ = TrackMouseEvent(&mut tme);
                    st.tracking_mouse = true;
                }
                let px = (lparam.0 & 0xFFFF) as i16 as f32;
                let py = ((lparam.0 >> 16) & 0xFFFF) as i16 as f32;
                let dip_scale = 96.0 / st.dpi.max(96) as f32;
                let hit = hit_test(&st.layout, px * dip_scale, py * dip_scale);
                if hit != st.hovered {
                    st.hovered = hit;
                    let _ = InvalidateRect(hwnd, None, false);
                }
                return LRESULT(0);
            }
            WM_MOUSELEAVE => {
                st.tracking_mouse = false;
                if st.hovered.is_some() || st.pressed.is_some() {
                    st.hovered = None;
                    st.pressed = None;
                    let _ = InvalidateRect(hwnd, None, false);
                }
                return LRESULT(0);
            }
            WM_CAPTURECHANGED | WM_KILLFOCUS => {
                if st.pressed.is_some() {
                    st.pressed = None;
                    let _ = InvalidateRect(hwnd, None, false);
                }
                return LRESULT(0);
            }
            WM_LBUTTONDOWN => {
                let px = (lparam.0 & 0xFFFF) as i16 as f32;
                let py = ((lparam.0 >> 16) & 0xFFFF) as i16 as f32;
                let dip_scale = 96.0 / st.dpi.max(96) as f32;
                let hit = hit_test(&st.layout, px * dip_scale, py * dip_scale);
                st.pressed = hit;
                st.focus_visible = false;
                if let Some(h) = hit {
                    st.focused_item = h;
                    SetCapture(hwnd);
                }
                let _ = InvalidateRect(hwnd, None, false);
                return LRESULT(0);
            }
            WM_LBUTTONUP => {
                let _ = ReleaseCapture();
                let px = (lparam.0 & 0xFFFF) as i16 as f32;
                let py = ((lparam.0 >> 16) & 0xFFFF) as i16 as f32;
                let dip_scale = 96.0 / st.dpi.max(96) as f32;
                let hit = hit_test(&st.layout, px * dip_scale, py * dip_scale);
                let prev_pressed = st.pressed.take();
                st.hovered = hit;
                let _ = InvalidateRect(hwnd, None, false);
                if let (Some(p), Some(h)) = (prev_pressed, hit) {
                    if p == h {
                        st.activate_item(hwnd, h);
                    }
                }
                return LRESULT(0);
            }
            WM_KEYDOWN => {
                let vk = wparam.0 as u16;
                let shift = (GetKeyState(VK_SHIFT.0 as i32) as u16 & 0x8000) != 0;
                if vk == VK_ESCAPE.0 {
                    st.done = true;
                    let _ = PostMessageW(hwnd, WM_CLOSE, WPARAM(0), LPARAM(0));
                    return LRESULT(0);
                }
                if vk == VK_TAB.0 {
                    st.focused_item = cycle_focus(st.focused_item, shift);
                    st.focus_visible = true;
                    let _ = InvalidateRect(hwnd, None, false);
                    return LRESULT(0);
                }
                if vk == VK_UP.0 {
                    let next_row = match st.focused_item {
                        HitItem::Row(r) => r.saturating_sub(1),
                        _ => FORMAT_GROUPS.len() - 1,
                    };
                    st.focused_item = HitItem::Row(next_row);
                    st.focus_visible = true;
                    let _ = InvalidateRect(hwnd, None, false);
                    return LRESULT(0);
                }
                if vk == VK_DOWN.0 {
                    let next_row = match st.focused_item {
                        HitItem::Row(r) => (r + 1).min(FORMAT_GROUPS.len() - 1),
                        _ => 0,
                    };
                    st.focused_item = HitItem::Row(next_row);
                    st.focus_visible = true;
                    let _ = InvalidateRect(hwnd, None, false);
                    return LRESULT(0);
                }
                if vk == VK_SPACE.0 {
                    st.focus_visible = true;
                    st.activate_item(hwnd, st.focused_item);
                    return LRESULT(0);
                }
                if vk == VK_RETURN.0 {
                    st.focus_visible = true;
                    let target = match st.focused_item {
                        HitItem::Row(_) => HitItem::BtnApplyDefault,
                        btn => btn,
                    };
                    st.activate_item(hwnd, target);
                    return LRESULT(0);
                }
            }
            WM_PAINT => {
                let mut ps = PAINTSTRUCT::default();
                let _ = BeginPaint(hwnd, &mut ps);
                if st.ensure_render_target(hwnd) {
                    if let (Some(rt), Some(brush)) = (st.rt.clone(), st.solid_brush.clone()) {
                        let base_rt: ID2D1RenderTarget = rt.into();
                        base_rt.BeginDraw();
                        paint_assoc_ui(&PaintAssocArgs {
                            rt: &base_rt,
                            brush: &brush,
                            dw: &st.dwrite_factory,
                            formats: &st.formats,
                            check_stroke: st.check_stroke.as_ref().map(|v| &**v),
                            layout: &st.layout,
                            lang: st.lang,
                            dark: st.dark,
                            checked: &st.checked,
                            group_badges: &st.group_badges,
                            locked_group_count: st.locked_group_count,
                            status_banner: st
                                .status_banner
                                .as_ref()
                                .map(|(msg, err)| (msg.as_str(), *err)),
                            focused_item: st.focused_item,
                            focus_visible: st.focus_visible,
                            hovered: st.hovered,
                            pressed: st.pressed,
                            doc_bmp: st.doc_bmp.as_ref(),
                            app_bmp: st.app_bmp.as_ref(),
                        });
                        if base_rt.EndDraw(None, None).is_err() {
                            st.solid_brush = None;
                            st.doc_bmp = None;
                            st.app_bmp = None;
                            st.rt = None;
                        }
                    }
                }
                let _ = EndPaint(hwnd, &ps);
                return LRESULT(0);
            }
            WM_CLOSE | WM_DESTROY => {
                st.done = true;
                return LRESULT(0);
            }
            _ => {}
        }
    }
    DefWindowProcW(hwnd, msg, wparam, lparam)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tab_focus_cycles_forward_and_backward_across_all_controls() {
        let mut cur = HitItem::Row(0);
        for _ in 0..FOCUS_ORDER.len() {
            cur = cycle_focus(cur, false);
        }
        assert_eq!(cur, HitItem::Row(0));
        assert_eq!(cycle_focus(HitItem::Row(0), true), HitItem::BtnClose);
        assert_eq!(cycle_focus(HitItem::BtnClose, false), HitItem::Row(0));
    }
}
