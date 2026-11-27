// ─────────────────────────────────────────────────────────────────────────────
// features/always_on_top.rs — 窗口置顶与对标 PowerToys AlwaysOnTop 的原生圆角微光边
//                             支持多窗口同时置顶、DWMWA_CLOAKED 虚拟桌面自动隐藏、
//                             最大化贴边直角不溢副屏、全屏避让与置顶瞬间呼吸脉冲反馈
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{resolve_color_or_accent, WindowKitConfig};
use crate::core::constants::{ANIM_AOT_PULSE_MS, PIN_BORDER_WINDOW_CLASS};
use crate::core::geometry::RectI;
use crate::core::win_utils::{
    get_window_dpi, get_window_visual_rect, is_foreground_fullscreen, is_hung_window,
    is_manageable_window, is_window_cloaked, resolve_foreground_or_target_hwnd, to_wide,
};
use crate::features::focus_border::{
    is_invalid_or_minimized_rect, is_layered_zero_alpha, is_mouse_drag_button_down,
    is_rect_visible_on_any_monitor, should_resume_after_drag,
};
use crate::render::d2d::{
    ease_out_cubic, rect_f, render_layered_window_d2d, rgb_a, rounded_rect, SharedGraphics,
};
use crate::wk_info;
use std::collections::HashMap;
use std::sync::Once;
use std::time::Instant;
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, GetForegroundWindow, GetWindowLongPtrW,
    IsIconic, IsWindow, IsWindowVisible, IsZoomed, LoadCursorW, RegisterClassExW, SetWindowPos,
    ShowWindow, CS_HREDRAW, CS_VREDRAW, GWL_EXSTYLE, HWND_NOTOPMOST, HWND_TOPMOST, IDC_ARROW,
    SWP_NOACTIVATE, SWP_NOMOVE, SWP_NOSIZE, SW_HIDE, SW_SHOWNOACTIVATE, WNDCLASSEXW,
    WS_EX_LAYERED, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_EX_TRANSPARENT, WS_POPUP,
};

struct PinnedEntry {
    target: HWND,
    border_hwnd: Option<HWND>,
    last_rect: RectI,
    last_zoomed: bool,
    last_visible: bool,
    last_rendered_size: (i32, i32),
    /// 置顶开启瞬间触发的 230ms 柔和光晕脉冲计时器（收敛后立即清空，静止 0 CPU）
    pulse_started: Option<Instant>,
    /// 拖动/缩放期零滞后隐身态：(is_system_movesize, last_seen_rect, last_motion_at)
    drag_hidden: Option<(bool, RectI, Instant)>,
}

pub struct AlwaysOnTopManager {
    pinned: HashMap<isize, PinnedEntry>,
}

impl AlwaysOnTopManager {
    pub fn new() -> Self {
        Self {
            pinned: HashMap::new(),
        }
    }

    pub fn pinned_count(&self) -> usize {
        self.pinned.len()
    }

    pub fn has_active_animation(&self) -> bool {
        self.pinned
            .values()
            .any(|e| e.pulse_started.is_some() || e.drag_hidden.is_some())
    }

    /// 拖动/缩放已置顶窗口瞬间 0ms 隐藏边框（对标微软 PowerToys AlwaysOnTop，杜绝拖影滞后）
    pub fn on_move_size_start(&mut self, hwnd: HWND) {
        let key = hwnd.0 as isize;
        if let Some(entry) = self.pinned.get_mut(&key) {
            let rect = get_window_visual_rect(hwnd).unwrap_or(entry.last_rect);
            entry.drag_hidden = Some((true, rect, Instant::now()));
            if entry.last_visible {
                if let Some(bh) = entry.border_hwnd {
                    unsafe {
                        let _ = ShowWindow(bh, SW_HIDE);
                    }
                }
                entry.last_visible = false;
            }
        }
    }

    pub fn on_move_size_end(&mut self, hwnd: HWND) {
        let key = hwnd.0 as isize;
        if let Some(entry) = self.pinned.get_mut(&key) {
            entry.drag_hidden = None;
        }
    }

    /// 强制将指定窗口设为置顶状态（用于 Workspaces 场景态置顶叠加）
    pub fn ensure_pinned_hwnd(
        &mut self,
        hwnd: HWND,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> bool {
        if hwnd.0.is_null() || !is_manageable_window(hwnd, false, &cfg.excluded_apps) {
            return false;
        }
        let key = hwnd.0 as isize;
        unsafe {
            let _ = SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
            );
        }
        if !self.pinned.contains_key(&key) {
            let border_hwnd = if cfg.always_on_top.show_border {
                create_border_window()
            } else {
                None
            };
            self.pinned.insert(
                key,
                PinnedEntry {
                    target: hwnd,
                    border_hwnd,
                    last_rect: RectI::default(),
                    last_zoomed: false,
                    last_visible: false,
                    last_rendered_size: (0, 0),
                    pulse_started: Some(Instant::now()),
                    drag_hidden: None,
                },
            );
            wk_info!(trace_id, "alwaysOnTop", "workspace-pinned hwnd=0x{:x}", key);
        }
        self.sync_borders(cfg, gfx, false);
        true
    }

    /// 切换当前前台合法用户窗口的置顶状态
    pub fn toggle_foreground(
        &mut self,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> bool {
        let fg = resolve_foreground_or_target_hwnd();
        if fg.0.is_null() || !is_manageable_window(fg, false, &cfg.excluded_apps) {
            return false;
        }
        let key = fg.0 as isize;
        let ex_style = unsafe { GetWindowLongPtrW(fg, GWL_EXSTYLE) as u32 };
        let is_currently_topmost = (ex_style & WS_EX_TOPMOST.0) != 0;

        if is_currently_topmost || self.pinned.contains_key(&key) {
            unsafe {
                let _ = SetWindowPos(
                    fg,
                    HWND_NOTOPMOST,
                    0,
                    0,
                    0,
                    0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
                );
            }
            if let Some(entry) = self.pinned.remove(&key) {
                destroy_border_hwnd(entry.border_hwnd);
            }
            wk_info!(trace_id, "alwaysOnTop", "unpinned hwnd=0x{:x}", key);
            true
        } else {
            unsafe {
                let _ = SetWindowPos(
                    fg,
                    HWND_TOPMOST,
                    0,
                    0,
                    0,
                    0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
                );
            }
            let border_hwnd = if cfg.always_on_top.show_border {
                create_border_window()
            } else {
                None
            };
            self.pinned.insert(
                key,
                PinnedEntry {
                    target: fg,
                    border_hwnd,
                    last_rect: RectI::default(),
                    last_zoomed: false,
                    last_visible: false,
                    last_rendered_size: (0, 0),
                    pulse_started: Some(Instant::now()),
                    drag_hidden: None,
                },
            );
            wk_info!(trace_id, "alwaysOnTop", "pinned hwnd=0x{:x}", key);
            self.sync_borders(cfg, gfx, false);
            true
        }
    }

    /// 动画帧驱动：仅在刚置顶后的 230ms 内执行光晕收束，收敛后自动结束返回 false
    pub fn tick_animation(
        &mut self,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        muted: bool,
    ) -> bool {
        if !self.has_active_animation() {
            return false;
        }
        self.sync_borders(cfg, gfx, muted);
        self.has_active_animation()
    }

    /// 同步所有已置顶窗口的边框位置与可见性（在 WinEvent 位置变更、脉冲帧或周期巡检时调用）
    pub fn sync_borders(
        &mut self,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        muted: bool,
    ) {
        if self.pinned.is_empty() {
            return;
        }
        let mut stale_keys = Vec::new();
        let border_color = resolve_color_or_accent(
            &cfg.always_on_top.border_color,
            &cfg.accent_color,
            [0.96, 0.62, 0.04],
        );
        let fg_now = unsafe { GetForegroundWindow() };
        let fg_is_fullscreen = is_foreground_fullscreen();
        let show_border = cfg.always_on_top.show_border && cfg.features.always_on_top && !muted;

        for (&key, entry) in self.pinned.iter_mut() {
            let valid = unsafe { IsWindow(entry.target).as_bool() };
            if !valid {
                stale_keys.push(key);
                continue;
            }
            let ex_style = unsafe { GetWindowLongPtrW(entry.target, GWL_EXSTYLE) as u32 };
            if (ex_style & WS_EX_TOPMOST.0) == 0 {
                // 用户通过外部工具取消了置顶
                stale_keys.push(key);
                continue;
            }

            // 对标 PowerToys AlwaysOnTop：检测 DWMWA_CLOAKED（切虚拟桌面自动隐去）、最小化、零透明与独占全屏避让
            let occluded_by_fullscreen = fg_is_fullscreen && fg_now != entry.target;
            let visible_target = unsafe {
                IsWindowVisible(entry.target).as_bool()
                    && !IsIconic(entry.target).as_bool()
                    && !is_hung_window(entry.target)
                    && !is_window_cloaked(entry.target)
                    && !is_layered_zero_alpha(entry.target, ex_style)
                    && !occluded_by_fullscreen
            };

            if !show_border || !visible_target {
                entry.pulse_started = None;
                entry.drag_hidden = None;
                if entry.last_visible {
                    if let Some(bh) = entry.border_hwnd {
                        unsafe {
                            let _ = ShowWindow(bh, SW_HIDE);
                        }
                    }
                    entry.last_visible = false;
                }
                continue;
            }

            let Some(rect) = get_window_visual_rect(entry.target) else {
                continue;
            };
            if is_invalid_or_minimized_rect(rect) || !is_rect_visible_on_any_monitor(rect) {
                if entry.last_visible {
                    if let Some(bh) = entry.border_hwnd {
                        unsafe {
                            let _ = ShowWindow(bh, SW_HIDE);
                        }
                    }
                    entry.last_visible = false;
                }
                continue;
            }

            // 拖动/缩放零滞后抑制（支持标准 MOVESIZE 及自绘标题栏拖拽）
            let mouse_down = is_mouse_drag_button_down();
            if let Some((is_sys, last_seen, last_at)) = entry.drag_hidden {
                if rect != last_seen {
                    entry.drag_hidden = Some((is_sys, rect, Instant::now()));
                    continue;
                }
                if !should_resume_after_drag(is_sys, mouse_down, last_at.elapsed().as_millis()) {
                    continue;
                }
                entry.drag_hidden = None;
            } else if entry.last_visible && rect != entry.last_rect && mouse_down {
                entry.drag_hidden = Some((false, rect, Instant::now()));
                if let Some(bh) = entry.border_hwnd {
                    unsafe {
                        let _ = ShowWindow(bh, SW_HIDE);
                    }
                }
                entry.last_visible = false;
                continue;
            }

            if entry.border_hwnd.is_none() {
                entry.border_hwnd = create_border_window();
                entry.last_rendered_size = (0, 0);
            }
            let (Some(bh), Some(g)) = (entry.border_hwnd, gfx) else {
                continue;
            };

            let is_zoomed = unsafe { IsZoomed(entry.target).as_bool() };
            let pulse_factor = if let Some(started) = entry.pulse_started {
                let elapsed = started.elapsed().as_secs_f32() * 1000.0;
                if elapsed >= ANIM_AOT_PULSE_MS {
                    entry.pulse_started = None;
                    0.0
                } else {
                    1.0 - ease_out_cubic(elapsed / ANIM_AOT_PULSE_MS)
                }
            } else {
                0.0
            };

            if rect == entry.last_rect
                && is_zoomed == entry.last_zoomed
                && entry.last_visible
                && entry.pulse_started.is_none()
            {
                continue;
            }

            let dpi = get_window_dpi(entry.target);
            let scale = (dpi as f32 / 96.0).max(1.0);
            let base_bw = (cfg.always_on_top.border_width.clamp(1, 8) as f32 * scale).round();
            let bw = base_bw + pulse_factor * 1.8 * scale;
            // 最大化时切 0px 直角并紧贴可视内缘，绝不溢出到相邻副屏
            let pad = if is_zoomed {
                0
            } else {
                (base_bw.ceil() as i32) + (4.0 * scale).round() as i32
            };
            let bx = rect.left - pad;
            let by = rect.top - pad;
            let w = rect.width() + pad * 2;
            let h = rect.height() + pad * 2;
            let radius = if is_zoomed { 0.0 } else { 8.0 * scale };

            let need_redraw = entry.pulse_started.is_some()
                || entry.last_rendered_size != (w, h)
                || entry.last_zoomed != is_zoomed;

            if need_redraw {
                let _ = render_layered_window_d2d(g, bh, bx, by, w, h, |rt| unsafe {
                    let inset = if is_zoomed {
                        bw * 0.5
                    } else {
                        pad as f32 - base_bw * 0.5
                    };
                    let r = rect_f(inset, inset, w as f32 - inset, h as f32 - inset);
                    let rr = rounded_rect(r, radius);
                    // 柔和外光晕（置顶开启瞬间 230ms 由 0.48 收束至 0.16 轻盈反馈）
                    if !is_zoomed {
                        let glow_alpha = 0.16 + pulse_factor * 0.34;
                        let glow_width = bw + (2.5 + pulse_factor * 3.0) * scale;
                        if let Ok(glow) = rt.CreateSolidColorBrush(&rgb_a(border_color, glow_alpha), None) {
                            rt.DrawRoundedRectangle(&rr, &glow, glow_width, None);
                        }
                    }
                    if let Ok(brush) = rt.CreateSolidColorBrush(&rgb_a(border_color, 0.94), None) {
                        rt.DrawRoundedRectangle(&rr, &brush, bw, None);
                    }
                });
                entry.last_rendered_size = (w, h);
            }

            unsafe {
                let _ = SetWindowPos(
                    bh,
                    HWND_TOPMOST,
                    bx,
                    by,
                    w,
                    h,
                    if need_redraw {
                        SWP_NOACTIVATE
                    } else {
                        SWP_NOSIZE | SWP_NOACTIVATE
                    },
                );
                if !entry.last_visible {
                    let _ = ShowWindow(bh, SW_SHOWNOACTIVATE);
                }
            }
            entry.last_rect = rect;
            entry.last_zoomed = is_zoomed;
            entry.last_visible = true;
        }

        for k in stale_keys {
            if let Some(entry) = self.pinned.remove(&k) {
                destroy_border_hwnd(entry.border_hwnd);
            }
        }
    }

    /// 隐藏并释放所有边框窗口（不撤销用户主动设置的置顶，防止打断用户工作流；仅在关闭边框时清理覆盖层）
    pub fn cleanup_overlays(&mut self) {
        for (_, entry) in self.pinned.drain() {
            destroy_border_hwnd(entry.border_hwnd);
        }
    }
}

impl Drop for AlwaysOnTopManager {
    fn drop(&mut self) {
        self.cleanup_overlays();
    }
}

fn destroy_border_hwnd(hwnd: Option<HWND>) {
    if let Some(h) = hwnd {
        unsafe {
            if IsWindow(h).as_bool() {
                let _ = DestroyWindow(h);
            }
        }
    }
}

fn ensure_pin_border_class() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(PIN_BORDER_WINDOW_CLASS);
        let wc = WNDCLASSEXW {
            cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(pin_border_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&wc);
    });
}

fn create_border_window() -> Option<HWND> {
    ensure_pin_border_class();
    unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(PIN_BORDER_WINDOW_CLASS);
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

unsafe extern "system" fn pin_border_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    DefWindowProcW(hwnd, msg, wparam, lparam)
}
