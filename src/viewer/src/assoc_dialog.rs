// ─────────────────────────────────────────────────────────────────────────────
// assoc_dialog.rs — 致敬原版 PicasaPhotoViewer (-config) 的文件关联与默认看图器配置面板
//
// 特性：
// - 100% 独立原生 Win32 高分屏自适应模态/独立双用窗口（零 WebView / 零外部依赖）；
// - 左侧实时渲染内嵌资源 ID=2 的「经典 Picasa 拍立得青松金丘相纸文档图标」高清预览；
// - 右侧支持按 8 大格式族自由勾选关联格式，一键刷新资源管理器图标并拉起系统默认应用设置；
// - 自动跟随操作系统 8 国显示语言。
// ─────────────────────────────────────────────────────────────────────────────

use crate::i18n::ViewerLanguage;
use crate::integration::{
    open_windows_default_apps_settings, query_registered_extensions,
    register_selected_file_associations, unregister_user_file_associations,
};
use std::sync::Once;
use windows::core::PCWSTR;
use windows::Win32::Foundation::{COLORREF, HINSTANCE, HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    BeginPaint, CreateFontW, CreatePen, CreateSolidBrush, DeleteObject, EndPaint, FillRect,
    GetMonitorInfoW, MonitorFromPoint, MonitorFromWindow, RoundRect, SelectObject, SetBkMode,
     SetTextColor, CLEARTYPE_QUALITY, CLIP_DEFAULT_PRECIS, DEFAULT_CHARSET, DEFAULT_PITCH,
    DT_CENTER, DT_END_ELLIPSIS, DT_SINGLELINE, DT_VCENTER, DT_WORDBREAK, DrawTextW, FF_DONTCARE,
    FW_MEDIUM, FW_SEMIBOLD, HBRUSH, HFONT, MONITORINFO, MONITOR_DEFAULTTONEAREST, OUT_TT_PRECIS,
    PAINTSTRUCT, PS_SOLID, TRANSPARENT,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::HiDpi::{
    GetDpiForSystem, GetDpiForWindow, SetProcessDpiAwarenessContext,
    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{EnableWindow, SetFocus, VK_ESCAPE};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyIcon, DestroyWindow, DispatchMessageW, DrawIconEx,
    GetClientRect, GetCursorPos, GetMessageW, GetWindowLongPtrW, IsWindow, LoadCursorW,
    LoadImageW, PostMessageW, RegisterClassExW, SendMessageW, SetForegroundWindow,
    SetWindowLongPtrW, ShowWindow, TranslateMessage, BM_GETCHECK, BM_SETCHECK, BS_AUTOCHECKBOX,
    BS_DEFPUSHBUTTON, BS_PUSHBUTTON, DI_NORMAL, GWLP_USERDATA, HICON, ICON_BIG, ICON_SMALL,
    IDC_ARROW, IMAGE_ICON, LR_DEFAULTCOLOR, MSG, SW_SHOW, WM_CLOSE, WM_COMMAND,
    WM_CTLCOLORBTN, WM_CTLCOLORSTATIC, WM_DESTROY, WM_KEYDOWN, WM_PAINT, WM_SETFONT, WM_SETICON,
    WNDCLASSEXW, WS_CAPTION, WS_CHILD, WS_EX_DLGMODALFRAME, WS_EX_TOPMOST, WS_OVERLAPPED,
    WS_SYSMENU, WS_TABSTOP, WS_VISIBLE,
};

const ASSOC_WND_CLASS: &str = "Tools3000_Picasa_Assoc_Dlg";

const ID_CB_BASE: usize = 2100;
const ID_BTN_ALL: usize = 2201;
const ID_BTN_COMMON: usize = 2202;
const ID_BTN_UNREG: usize = 2203;
const ID_BTN_APPLY_ONLY: usize = 2204;
const ID_BTN_APPLY_DEFAULT: usize = 2205;
const ID_BTN_CLOSE: usize = 2206;

const BST_UNCHECKED_VAL: usize = 0;
const BST_CHECKED_VAL: usize = 1;

struct FormatGroup {
    label: &'static str,
    common_default: bool,
    exts: &'static [&'static str],
}

const FORMAT_GROUPS: [FormatGroup; 8] = [
    FormatGroup {
        label: "JPEG  (.jpg, .jpeg, .jpe, .jfif, .pjpeg)",
        common_default: true,
        exts: &["jpg", "jpeg", "jpe", "jfif", "pjpeg"],
    },
    FormatGroup {
        label: "PNG / APNG  (.png, .apng)",
        common_default: true,
        exts: &["png", "apng"],
    },
    FormatGroup {
        label: "WebP  (.webp)",
        common_default: true,
        exts: &["webp"],
    },
    FormatGroup {
        label: "GIF  (.gif)",
        common_default: true,
        exts: &["gif"],
    },
    FormatGroup {
        label: "BMP / ICO / CUR  (.bmp, .dib, .rle, .ico, .cur)",
        common_default: true,
        exts: &["bmp", "dib", "rle", "ico", "cur"],
    },
    FormatGroup {
        label: "TIFF  (.tif, .tiff)",
        common_default: true,
        exts: &["tif", "tiff"],
    },
    FormatGroup {
        label: "SVG  (.svg, .svgz)",
        common_default: false,
        exts: &["svg", "svgz"],
    },
    FormatGroup {
        label: "AVIF / HEIC / RAW / TGA / QOI  (.avif, .heic, .dng, .cr2, .nef, .arw, .tga, .qoi...)",
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

struct AssocDialogState {
    lang: ViewerLanguage,
    dpi: i32,
    checkboxes: [HWND; 8],
    bg_brush: HBRUSH,
    card_brush: HBRUSH,
    font_normal: HFONT,
    font_bold: HFONT,
    doc_icon: HICON,
    app_icon: HICON,
    done: bool,
    outcome: AssocDialogOutcome,
}

fn to_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

#[inline]
fn scale_px(dip: i32, dpi: i32) -> i32 {
    (dip * dpi + 48) / 96
}

fn ensure_class_registered() {
    static REG: Once = Once::new();
    REG.call_once(|| unsafe {
        let Ok(hmod) = GetModuleHandleW(None) else {
            return;
        };
        let hinst: HINSTANCE = hmod.into();
        let cls_w = to_wide(ASSOC_WND_CLASS);
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

/// 弹出文件关联与默认看图器配置窗口（`parent` 为空时可作为 `--config` 独立窗口运行）
pub fn show_file_association_dialog(parent: HWND, lang: ViewerLanguage) -> AssocDialogOutcome {
    unsafe {
        let _ = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ensure_class_registered();

        let Ok(hmod) = GetModuleHandleW(None) else {
            return AssocDialogOutcome::Closed;
        };
        let hinst: HINSTANCE = hmod.into();
        let has_parent = !parent.0.is_null() && IsWindow(parent).as_bool();

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

        let dpi = if has_parent {
            GetDpiForWindow(parent).max(96) as i32
        } else {
            GetDpiForSystem().max(96) as i32
        };

        let dlg_w = scale_px(680, dpi);
        let dlg_h = scale_px(468, dpi);
        let x = work_rc.left + ((work_rc.right - work_rc.left) - dlg_w) / 2;
        let y = work_rc.top + ((work_rc.bottom - work_rc.top) - dlg_h) / 2;

        let bg_brush = CreateSolidBrush(COLORREF(0x00F9_F7F6));
        let card_brush = CreateSolidBrush(COLORREF(0x00FF_FFFF));

        let face = to_wide(lang.preferred_cjk_fallback_font());
        let mut lf_face = [0u16; 32];
        for (i, &ch) in face.iter().take(31).enumerate() {
            lf_face[i] = ch;
        }
        let font_normal = CreateFontW(
            -scale_px(13, dpi),
            0,
            0,
            0,
            FW_MEDIUM.0 as i32,
            0,
            0,
            0,
            DEFAULT_CHARSET.0 as u32,
            OUT_TT_PRECIS.0 as u32,
            CLIP_DEFAULT_PRECIS.0 as u32,
            CLEARTYPE_QUALITY.0 as u32,
            (DEFAULT_PITCH.0 | FF_DONTCARE.0) as u32,
            PCWSTR(lf_face.as_ptr()),
        );
        let font_bold = CreateFontW(
            -scale_px(13, dpi),
            0,
            0,
            0,
            FW_SEMIBOLD.0 as i32,
            0,
            0,
            0,
            DEFAULT_CHARSET.0 as u32,
            OUT_TT_PRECIS.0 as u32,
            CLIP_DEFAULT_PRECIS.0 as u32,
            CLEARTYPE_QUALITY.0 as u32,
            (DEFAULT_PITCH.0 | FF_DONTCARE.0) as u32,
            PCWSTR(lf_face.as_ptr()),
        );

        let doc_icon_sz = scale_px(112, dpi);
        let app_icon_sz = scale_px(36, dpi);
        // 资源 ID=2: 经典 Picasa 拍立得青松金丘相纸文档图标
        let doc_icon = LoadImageW(
            hinst,
            PCWSTR(2usize as *const u16),
            IMAGE_ICON,
            doc_icon_sz,
            doc_icon_sz,
            LR_DEFAULTCOLOR,
        )
        .map(|h| HICON(h.0))
        .unwrap_or_default();
        // 资源 ID=1: 主程序彩色玻璃快门圆轮图标
        let app_icon = LoadImageW(
            hinst,
            PCWSTR(1usize as *const u16),
            IMAGE_ICON,
            app_icon_sz,
            app_icon_sz,
            LR_DEFAULTCOLOR,
        )
        .map(|h| HICON(h.0))
        .unwrap_or_default();

        let mut state = Box::new(AssocDialogState {
            lang,
            dpi,
            checkboxes: [HWND::default(); 8],
            bg_brush,
            card_brush,
            font_normal,
            font_bold,
            doc_icon,
            app_icon,
            done: false,
            outcome: AssocDialogOutcome::Closed,
        });

        let s = lang.strings();
        let cls_w = to_wide(ASSOC_WND_CLASS);
        let title_w = to_wide(s.assoc_dlg_title);

        let Ok(hwnd) = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
            PCWSTR(cls_w.as_ptr()),
            PCWSTR(title_w.as_ptr()),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            x,
            y,
            dlg_w,
            dlg_h,
            parent,
            None,
            hinst,
            None,
        ) else {
            return AssocDialogOutcome::Closed;
        };

        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (&mut *state as *mut AssocDialogState) as isize);
        if !app_icon.is_invalid() {
            let _ = SendMessageW(
                hwnd,
                WM_SETICON,
                WPARAM(ICON_SMALL as usize),
                LPARAM(app_icon.0 as isize),
            );
            let _ = SendMessageW(
                hwnd,
                WM_SETICON,
                WPARAM(ICON_BIG as usize),
                LPARAM(app_icon.0 as isize),
            );
        }

        // 读取当前已注册后缀：若尚未注册任何后缀则默认全勾选，方便用户一键设置
        let existing = query_registered_extensions();
        let first_time = existing.is_empty();

        let btn_cls = to_wide("BUTTON");
        let cb_left = scale_px(214, dpi);
        let cb_w = scale_px(406, dpi);
        let cb_h = scale_px(26, dpi);
        let mut cb_y = scale_px(68, dpi);

        for (i, grp) in FORMAT_GROUPS.iter().enumerate() {
            let lbl_w = to_wide(grp.label);
            if let Ok(cb) = CreateWindowExW(
                Default::default(),
                PCWSTR(btn_cls.as_ptr()),
                PCWSTR(lbl_w.as_ptr()),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | windows::Win32::UI::WindowsAndMessaging::WINDOW_STYLE(BS_AUTOCHECKBOX as u32),
                cb_left,
                cb_y,
                cb_w,
                cb_h,
                hwnd,
                windows::Win32::UI::WindowsAndMessaging::HMENU((ID_CB_BASE + i) as *mut _),
                hinst,
                None,
            ) {
                let _ = SendMessageW(cb, WM_SETFONT, WPARAM(font_normal.0 as usize), LPARAM(1));
                let checked = if first_time {
                    true
                } else {
                    grp.exts.iter().any(|e| existing.contains(*e))
                };
                let _ = SendMessageW(
                    cb,
                    BM_SETCHECK,
                    WPARAM(if checked { BST_CHECKED_VAL } else { BST_UNCHECKED_VAL }),
                    LPARAM(0),
                );
                state.checkboxes[i] = cb;
            }
            cb_y += scale_px(30, dpi);
        }

        // 创建快捷选择行（位于复选框正下方，避免与底栏长多语言主按钮拥挤）
        let mut cli = RECT::default();
        let _ = GetClientRect(hwnd, &mut cli);
        let quick_y = cb_y + scale_px(6, dpi);
        let quick_h = scale_px(27, dpi);
        let btn_y = cli.bottom - scale_px(44, dpi);
        let btn_h = scale_px(31, dpi);

        let make_btn = |id: usize, text: &str, bx: i32, by: i32, bw: i32, bh: i32, is_def: bool| -> HWND {
            let tw = to_wide(text);
            let bstyle = if is_def { BS_DEFPUSHBUTTON } else { BS_PUSHBUTTON } as u32;
            let b = CreateWindowExW(
                Default::default(),
                PCWSTR(btn_cls.as_ptr()),
                PCWSTR(tw.as_ptr()),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | windows::Win32::UI::WindowsAndMessaging::WINDOW_STYLE(bstyle),
                bx,
                by,
                bw,
                bh,
                hwnd,
                windows::Win32::UI::WindowsAndMessaging::HMENU(id as *mut _),
                hinst,
                None,
            )
            .unwrap_or_default();
            let f = if is_def { font_bold } else { font_normal };
            let _ = SendMessageW(b, WM_SETFONT, WPARAM(f.0 as usize), LPARAM(1));
            b
        };

        let gap = scale_px(8, dpi);
        let mut qx = cb_left;
        let w_q1 = scale_px(116, dpi);
        let w_q2 = scale_px(126, dpi);
        let w_q3 = scale_px(126, dpi);
        let _ = make_btn(ID_BTN_ALL, s.assoc_btn_select_all, qx, quick_y, w_q1, quick_h, false);
        qx += w_q1 + gap;
        let _ = make_btn(ID_BTN_COMMON, s.assoc_btn_common, qx, quick_y, w_q2, quick_h, false);
        qx += w_q2 + gap;
        let _ = make_btn(ID_BTN_UNREG, s.assoc_btn_remove_all, qx, quick_y, w_q3, quick_h, false);

        // 最底排宽敞主操作区（完美兼容俄语/西语/葡语长文案）
        let mut rx = cli.right - scale_px(20, dpi);
        let w_close = scale_px(88, dpi);
        rx -= w_close;
        let _ = make_btn(ID_BTN_CLOSE, s.assoc_btn_close, rx, btn_y, w_close, btn_h, false);

        let w_def = scale_px(292, dpi);
        rx -= w_def + gap;
        let def_btn = make_btn(ID_BTN_APPLY_DEFAULT, s.assoc_btn_apply_default, rx, btn_y, w_def, btn_h, true);

        let w_only = scale_px(220, dpi);
        rx -= w_only + gap;
        let _ = make_btn(ID_BTN_APPLY_ONLY, s.assoc_btn_apply_only, rx, btn_y, w_only, btn_h, false);

        if has_parent {
            let _ = EnableWindow(parent, false);
        }
        let _ = ShowWindow(hwnd, SW_SHOW);
        let _ = SetForegroundWindow(hwnd);
        if !def_btn.0.is_null() {
            let _ = SetFocus(def_btn);
        }

        // 局部模态消息泵（兼容看图器内调用与 `--config` 独立调用，不触发主程序 PostQuitMessage）
        let mut msg = MSG::default();
        while !state.done && IsWindow(hwnd).as_bool() {
            let ret = GetMessageW(&mut msg, None, 0, 0);
            if ret.0 <= 0 {
                break;
            }
            if msg.message == WM_KEYDOWN && msg.wParam.0 as u16 == VK_ESCAPE.0 {
                state.done = true;
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
        if !doc_icon.is_invalid() {
            let _ = DestroyIcon(doc_icon);
        }
        if !app_icon.is_invalid() {
            let _ = DestroyIcon(app_icon);
        }
        let _ = DeleteObject(font_normal);
        let _ = DeleteObject(font_bold);
        let _ = DeleteObject(bg_brush);
        let _ = DeleteObject(card_brush);

        state.outcome
    }
}

impl AssocDialogState {
    unsafe fn collect_checked_extensions(&self) -> Vec<&'static str> {
        let mut out = Vec::new();
        for (i, grp) in FORMAT_GROUPS.iter().enumerate() {
            let cb = self.checkboxes[i];
            if !cb.0.is_null() {
                let st = SendMessageW(cb, BM_GETCHECK, WPARAM(0), LPARAM(0));
                if st.0 as usize == BST_CHECKED_VAL {
                    out.extend_from_slice(grp.exts);
                }
            }
        }
        out
    }

    unsafe fn set_checks(&self, only_common: bool) {
        for (i, grp) in FORMAT_GROUPS.iter().enumerate() {
            let cb = self.checkboxes[i];
            if !cb.0.is_null() {
                let on = if only_common { grp.common_default } else { true };
                let _ = SendMessageW(
                    cb,
                    BM_SETCHECK,
                    WPARAM(if on { BST_CHECKED_VAL } else { BST_UNCHECKED_VAL }),
                    LPARAM(0),
                );
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
            WM_CTLCOLORSTATIC | WM_CTLCOLORBTN => {
                let hdc = windows::Win32::Graphics::Gdi::HDC(wparam.0 as *mut _);
                let _ = SetBkMode(hdc, TRANSPARENT);
                let _ = SetTextColor(hdc, COLORREF(0x0024_2220));
                return LRESULT(st.bg_brush.0 as isize);
            }
            WM_PAINT => {
                let mut ps = PAINTSTRUCT::default();
                let hdc = BeginPaint(hwnd, &mut ps);
                let mut rc = RECT::default();
                let _ = GetClientRect(hwnd, &mut rc);
                let _ = FillRect(hdc, &rc, st.bg_brush);
                let _ = SetBkMode(hdc, TRANSPARENT);

                let s = st.lang.strings();
                let dpi = st.dpi;

                // 顶部说明横幅文字
                let old_font = SelectObject(hdc, st.font_normal);
                let _ = SetTextColor(hdc, COLORREF(0x0038_3430));
                let mut sub_rc = RECT {
                    left: scale_px(20, dpi),
                    top: scale_px(14, dpi),
                    right: rc.right - scale_px(20, dpi),
                    bottom: scale_px(58, dpi),
                };
                let mut sub_w = to_wide(s.assoc_dlg_subtitle);
                let sub_len = sub_w.len().saturating_sub(1);
                let _ = DrawTextW(hdc, &mut sub_w[..sub_len], &mut sub_rc, DT_WORDBREAK);

                // 左侧 Picasa 经典拍立得青松金丘文件图标高清预览白卡片
                let card_l = scale_px(20, dpi);
                let card_t = scale_px(68, dpi);
                let card_r = scale_px(196, dpi);
                let card_b = scale_px(304, dpi);
                let pen = CreatePen(PS_SOLID, 1, COLORREF(0x00DF_DBD6));
                let old_pen = SelectObject(hdc, pen);
                let old_br = SelectObject(hdc, st.card_brush);
                let _ = RoundRect(
                    hdc,
                    card_l,
                    card_t,
                    card_r,
                    card_b,
                    scale_px(12, dpi),
                    scale_px(12, dpi),
                );
                let _ = SelectObject(hdc, old_br);
                let _ = SelectObject(hdc, old_pen);
                let _ = DeleteObject(pen);

                let doc_sz = scale_px(112, dpi);
                let icon_x = card_l + ((card_r - card_l) - doc_sz) / 2;
                let icon_y = card_t + scale_px(24, dpi);
                if !st.doc_icon.is_invalid() {
                    let _ = DrawIconEx(
                        hdc,
                        icon_x,
                        icon_y,
                        st.doc_icon,
                        doc_sz,
                        doc_sz,
                        0,
                        None,
                        DI_NORMAL,
                    );
                }
                if !st.app_icon.is_invalid() {
                    let app_sz = scale_px(36, dpi);
                    let _ = DrawIconEx(
                        hdc,
                        icon_x + doc_sz - scale_px(22, dpi),
                        icon_y + doc_sz - scale_px(24, dpi),
                        st.app_icon,
                        app_sz,
                        app_sz,
                        0,
                        None,
                        DI_NORMAL,
                    );
                }

                let _ = SelectObject(hdc, st.font_bold);
                let _ = SetTextColor(hdc, COLORREF(0x0028_2522));
                let mut cap_rc = RECT {
                    left: card_l + scale_px(8, dpi),
                    top: icon_y + doc_sz + scale_px(16, dpi),
                    right: card_r - scale_px(8, dpi),
                    bottom: card_b - scale_px(12, dpi),
                };
                let mut cap_w = to_wide(s.assoc_icon_caption);
                let cap_len = cap_w.len().saturating_sub(1);
                let _ = DrawTextW(
                    hdc,
                    &mut cap_w[..cap_len],
                    &mut cap_rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                );

                // 底部按钮上方的细分隔线
                let line_y = rc.bottom - scale_px(56, dpi);
                let sep_rc = RECT {
                    left: scale_px(18, dpi),
                    top: line_y,
                    right: rc.right - scale_px(18, dpi),
                    bottom: line_y + 1,
                };
                let sep_br = CreateSolidBrush(COLORREF(0x00E2_DED9));
                let _ = FillRect(hdc, &sep_rc, sep_br);
                let _ = DeleteObject(sep_br);

                let _ = SelectObject(hdc, old_font);
                let _ = EndPaint(hwnd, &ps);
                return LRESULT(0);
            }
            WM_COMMAND => {
                let id = wparam.0 & 0xFFFF;
                match id {
                    ID_BTN_ALL => {
                        st.set_checks(false);
                        return LRESULT(0);
                    }
                    ID_BTN_COMMON => {
                        st.set_checks(true);
                        return LRESULT(0);
                    }
                    ID_BTN_UNREG => {
                        match unregister_user_file_associations() {
                            Ok(()) => st.outcome = AssocDialogOutcome::Unregistered,
                            Err(e) => st.outcome = AssocDialogOutcome::Failed(e),
                        }
                        st.done = true;
                        let _ = PostMessageW(hwnd, WM_CLOSE, WPARAM(0), LPARAM(0));
                        return LRESULT(0);
                    }
                    ID_BTN_APPLY_ONLY | ID_BTN_APPLY_DEFAULT => {
                        let exts = st.collect_checked_extensions();
                        if exts.is_empty() {
                            match unregister_user_file_associations() {
                                Ok(()) => st.outcome = AssocDialogOutcome::Unregistered,
                                Err(e) => st.outcome = AssocDialogOutcome::Failed(e),
                            }
                        } else {
                            match register_selected_file_associations(&exts) {
                                Ok(()) => {
                                    if id == ID_BTN_APPLY_DEFAULT {
                                        open_windows_default_apps_settings(hwnd);
                                    }
                                    st.outcome = AssocDialogOutcome::Applied;
                                }
                                Err(e) => st.outcome = AssocDialogOutcome::Failed(e),
                            }
                        }
                        st.done = true;
                        let _ = PostMessageW(hwnd, WM_CLOSE, WPARAM(0), LPARAM(0));
                        return LRESULT(0);
                    }
                    ID_BTN_CLOSE => {
                        st.done = true;
                        let _ = PostMessageW(hwnd, WM_CLOSE, WPARAM(0), LPARAM(0));
                        return LRESULT(0);
                    }
                    _ => {}
                }
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
