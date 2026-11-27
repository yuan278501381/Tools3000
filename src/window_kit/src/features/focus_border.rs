// ─────────────────────────────────────────────────────────────────────────────
// features/focus_border.rs — 焦点窗口自适应主题色圆角高亮边框
//                            严格遵循自动避让全屏、DWMWA_CLOAKED 虚拟桌面隐藏、
//                            切窗 115ms 柔滑淡入、拖窗 0 延迟紧跟与闲置零绘制开销
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{resolve_color_or_accent, WindowKitConfig};
use crate::core::constants::{ANIM_FOCUS_FADE_MS, FOCUS_BORDER_WINDOW_CLASS};
use crate::core::geometry::RectI;
use crate::core::win_utils::{
    get_window_dpi, get_window_visual_rect, is_foreground_fullscreen, is_hung_window,
    is_manageable_window, is_window_cloaked, to_wide,
};
use crate::render::d2d::{
    ease_out_cubic, rect_f, render_layered_window_d2d_alpha, rgb_a, rounded_rect,
    update_layered_window_alpha_only, SharedGraphics,
};
use std::sync::Once;
use std::time::Instant;
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, GetForegroundWindow, IsIconic, IsWindow,
    IsWindowVisible, IsZoomed, LoadCursorW, RegisterClassExW, SetWindowPos, ShowWindow, CS_HREDRAW,
    CS_VREDRAW, HWND_TOPMOST, IDC_ARROW, SWP_NOACTIVATE, SW_HIDE, SW_SHOWNOACTIVATE, WNDCLASSEXW,
    WS_EX_LAYERED, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_EX_TRANSPARENT, WS_POPUP,
};

pub struct FocusBorderManager {
    overlay_hwnd: Option<HWND>,
    last_target: isize,
    last_rect: RectI,
    last_zoomed: bool,
    visible: bool,
    fade_started: Option<Instant>,
}

impl FocusBorderManager {
    pub fn new() -> Self {
        Self {
            overlay_hwnd: None,
            last_target: 0,
            last_rect: RectI::default(),
            last_zoomed: false,
            visible: false,
            fade_started: None,
        }
    }

    pub fn has_active_animation(&self) -> bool {
        self.visible && self.fade_started.is_some()
    }

    pub fn tick_animation(
        &mut self,
        _cfg: &WindowKitConfig,
        _gfx: Option<&SharedGraphics>,
        muted: bool,
        quick_switch_open: bool,
    ) -> bool {
        if !self.has_active_animation() {
            return false;
        }
        if muted || quick_switch_open {
            self.fade_started = None;
            return false;
        }
        if let Some(started) = self.fade_started {
            let elapsed = started.elapsed().as_secs_f32() * 1000.0;
            let t = (elapsed / ANIM_FOCUS_FADE_MS).clamp(0.0, 1.0);
            let alpha = (56.0 + ease_out_cubic(t) * 199.0).round().clamp(56.0, 255.0) as u8;
            if let Some(bh) = self.overlay_hwnd {
                update_layered_window_alpha_only(bh, alpha);
            }
            if t >= 1.0 {
                self.fade_started = None;
            }
        }
        self.has_active_animation()
    }

    /// 刷新焦点窗口高亮边框（供 WinEvent 前台/位置变化事件、淡入帧与轻量巡检调用）
    pub fn refresh(
        &mut self,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        muted: bool,
        quick_switch_open: bool,
    ) {
        if !cfg.features.focus_border || muted || quick_switch_open || is_foreground_fullscreen() {
            self.hide();
            return;
        }

        let fg = unsafe { GetForegroundWindow() };
        if fg.0.is_null()
            || !is_manageable_window(fg, false, &cfg.excluded_apps)
            || unsafe {
                !IsWindowVisible(fg).as_bool()
                    || IsIconic(fg).as_bool()
                    || is_hung_window(fg)
                    || is_window_cloaked(fg)
            }
        {
            self.hide();
            return;
        }

        let Some(rect) = get_window_visual_rect(fg) else {
            self.hide();
            return;
        };

        let target_id = fg.0 as isize;
        let is_zoomed = unsafe { IsZoomed(fg).as_bool() };
        let switched_window = target_id != self.last_target || !self.visible;
        if switched_window {
            self.fade_started = Some(Instant::now());
        } else if self.last_rect == rect && self.last_zoomed == is_zoomed {
            return;
        }

        let source_alpha = if switched_window { 56u8 } else { 255u8 };
        if !switched_window {
            self.fade_started = None;
        }

        if self.overlay_hwnd.is_none() {
            self.overlay_hwnd = create_focus_border_window();
        }
        let (Some(bh), Some(g)) = (self.overlay_hwnd, gfx) else {
            return;
        };

        let accent = resolve_color_or_accent(
            &cfg.focus_border.border_color,
            &cfg.accent_color,
            [0.23, 0.51, 0.96],
        );
        let dpi = get_window_dpi(fg);
        let scale = (dpi as f32 / 96.0).max(1.0);
        let bw = (cfg.focus_border.border_width.clamp(1, 8) as f32 * scale).round();
        let radius = if is_zoomed {
            0.0
        } else {
            cfg.focus_border.corner_radius.clamp(0, 24) as f32 * scale
        };

        // 边框紧贴可视外沿向外延伸，最大化时则贴内缘避免裁切或溢屏
        let pad = if is_zoomed { 0 } else { bw.ceil() as i32 + (3.0 * scale).round() as i32 };
        let bx = rect.left - pad;
        let by = rect.top - pad;
        let w = rect.width() + pad * 2;
        let h = rect.height() + pad * 2;
        let alpha = cfg.focus_border.opacity.clamp(0.15, 1.0);

        let _ = render_layered_window_d2d_alpha(g, bh, bx, by, w, h, source_alpha, |rt| unsafe {
            let inset = if is_zoomed {
                bw * 0.5
            } else {
                pad as f32 - bw * 0.5
            };
            let r = rect_f(inset, inset, w as f32 - inset, h as f32 - inset);
            let rr = rounded_rect(r, radius);
            if !is_zoomed {
                if let Ok(glow) = rt.CreateSolidColorBrush(&rgb_a(accent, alpha * 0.24), None) {
                    rt.DrawRoundedRectangle(&rr, &glow, bw + 2.5 * scale, None);
                }
            }
            if let Ok(brush) = rt.CreateSolidColorBrush(&rgb_a(accent, alpha), None) {
                rt.DrawRoundedRectangle(&rr, &brush, bw, None);
            }
        });

        unsafe {
            let _ = SetWindowPos(bh, HWND_TOPMOST, bx, by, w, h, SWP_NOACTIVATE);
            let _ = ShowWindow(bh, SW_SHOWNOACTIVATE);
        }
        self.last_target = target_id;
        self.last_rect = rect;
        self.last_zoomed = is_zoomed;
        self.visible = true;
    }

    /// Action 23: 在目标窗口（默认前台设置窗）外沿即时亮框预览焦点边框或置顶边框参数（持续 1.2s 自动收起）
    pub fn preview_style_on_foreground(
        &mut self,
        color_hex: &str,
        width_px: i32,
        corner_radius: i32,
        opacity: f32,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
    ) -> bool {
        let fg = unsafe { GetForegroundWindow() };
        if fg.0.is_null() || unsafe { !IsWindow(fg).as_bool() || IsIconic(fg).as_bool() } {
            return false;
        }
        let Some(rect) = get_window_visual_rect(fg) else {
            return false;
        };
        if self.overlay_hwnd.is_none() {
            self.overlay_hwnd = create_focus_border_window();
        }
        let (Some(bh), Some(g)) = (self.overlay_hwnd, gfx) else {
            return false;
        };

        let accent = resolve_color_or_accent(color_hex, &cfg.accent_color, [0.23, 0.51, 0.96]);
        let dpi = get_window_dpi(fg);
        let scale = (dpi as f32 / 96.0).max(1.0);
        let is_zoomed = unsafe { IsZoomed(fg).as_bool() };
        let bw = (width_px.clamp(1, 8) as f32 * scale).round();
        let radius = if is_zoomed {
            0.0
        } else {
            corner_radius.clamp(0, 24) as f32 * scale
        };
        let pad = if is_zoomed {
            0
        } else {
            bw.ceil() as i32 + (4.0 * scale).round() as i32
        };
        let bx = rect.left - pad;
        let by = rect.top - pad;
        let w = rect.width() + pad * 2;
        let h = rect.height() + pad * 2;
        let alpha = opacity.clamp(0.25, 1.0);

        let res = render_layered_window_d2d_alpha(g, bh, bx, by, w, h, 255, |rt| unsafe {
            let inset = if is_zoomed {
                bw * 0.5
            } else {
                pad as f32 - bw * 0.5
            };
            let r = rect_f(inset, inset, w as f32 - inset, h as f32 - inset);
            let rr = rounded_rect(r, radius);
            if !is_zoomed {
                if let Ok(glow) = rt.CreateSolidColorBrush(&rgb_a(accent, alpha * 0.30), None) {
                    rt.DrawRoundedRectangle(&rr, &glow, bw + 3.0 * scale, None);
                }
            }
            if let Ok(brush) = rt.CreateSolidColorBrush(&rgb_a(accent, alpha), None) {
                rt.DrawRoundedRectangle(&rr, &brush, bw, None);
            }
        });
        if res.is_err() {
            return false;
        }
        unsafe {
            let _ = SetWindowPos(bh, HWND_TOPMOST, bx, by, w, h, SWP_NOACTIVATE);
            let _ = ShowWindow(bh, SW_SHOWNOACTIVATE);
        }
        self.visible = true;
        self.last_target = 0; // 预览结束后刷新时重新评估
        self.fade_started = None;
        true
    }

    pub fn hide(&mut self) {
        self.fade_started = None;
        if self.visible {
            if let Some(bh) = self.overlay_hwnd {
                unsafe {
                    if IsWindow(bh).as_bool() {
                        let _ = ShowWindow(bh, SW_HIDE);
                    }
                }
            }
            self.visible = false;
            self.last_target = 0;
            self.last_rect = RectI::default();
            self.last_zoomed = false;
        }
    }

    pub fn destroy(&mut self) {
        self.fade_started = None;
        if let Some(bh) = self.overlay_hwnd.take() {
            unsafe {
                if IsWindow(bh).as_bool() {
                    let _ = DestroyWindow(bh);
                }
            }
        }
        self.visible = false;
        self.last_target = 0;
        self.last_rect = RectI::default();
        self.last_zoomed = false;
    }
}

impl Drop for FocusBorderManager {
    fn drop(&mut self) {
        self.destroy();
    }
}

fn ensure_focus_border_class() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(FOCUS_BORDER_WINDOW_CLASS);
        let wc = WNDCLASSEXW {
            cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(focus_border_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&wc);
    });
}

fn create_focus_border_window() -> Option<HWND> {
    ensure_focus_border_class();
    unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(FOCUS_BORDER_WINDOW_CLASS);
        let ex_style = WS_EX_LAYERED
            | WS_EX_TRANSPARENT
            | WS_EX_TOOLWINDOW
            | WS_EX_TOPMOST
            | WS_EX_NOACTIVATE;
        CreateWindowExW(
            ex_style,
            PCWSTR(cls.as_ptr()),
            PCWSTR::null(),
            WS_POPUP,
            0,
            0,
            1,
            1,
            None,
            None,
            hinst,
            None,
        )
        .ok()
    }
}

unsafe extern "system" fn focus_border_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    DefWindowProcW(hwnd, msg, wparam, lparam)
}
