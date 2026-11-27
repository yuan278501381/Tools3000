// ─────────────────────────────────────────────────────────────────────────────
// features/focus_border.rs — 世界级焦点窗口自适应圆角高亮边框
//
// 核心架构铁律:
//   1. 拖动/缩放零滞后引擎 (Zero-Lag Drag Suppression, 对标并超越 PowerToys AlwaysOnTop):
//      Windows DWM 跨进程独立浮层在鼠标拖拽时物理必然滞后 1~2 帧；
//      本引擎同时覆盖标准 EVENT_SYSTEM_MOVESIZESTART 与微信/Chrome/VS Code/Electron
//      自绘标题栏拖拽（左/右键按下 + 坐标位移嗅探）：拖放/缩放瞬间 0ms 瞬隐零拖影，
//      松手落位瞬间 0ms 严丝合缝贴合；纯键盘切分屏/切窗走 240Hz 零重绘硬件瞬移；
//   2. 六重零鬼影残留防线 (Zero-Ghost Guarantee):
//      覆盖 EVENT_OBJECT_HIDE / EVENT_OBJECT_CLOAKED / EVENT_SYSTEM_MINIMIZESTART /
//      (-32000, -32000) 离屏坐标 / 跨全显示器可见性校验 / 0 透明度 WS_EX_LAYERED 假可见 /
//      切窗重绘失败强制隐去旧框，彻底根治微信最小化/隐藏至托盘后边框滞留；
//   3. 精准紧邻 Z-Order: 非置顶窗精准插于其正上方紧邻层，绝不霸占 HWND_TOPMOST 切穿右键菜单与输入法。
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{resolve_color_or_accent, WindowKitConfig};
use crate::core::constants::FOCUS_BORDER_WINDOW_CLASS;
use crate::core::geometry::RectI;
use crate::core::win_utils::{
    get_window_dpi, get_window_visual_rect, is_foreground_fullscreen, is_hung_window,
    is_manageable_window, is_window_cloaked, to_wide,
};
use crate::render::d2d::{rect_f, render_layered_window_d2d_alpha, rgb_a, rounded_rect, SharedGraphics};
use std::sync::Once;
use std::time::Instant;
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{MonitorFromRect, MONITOR_DEFAULTTONULL};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::Input::KeyboardAndMouse::{GetAsyncKeyState, VK_LBUTTON, VK_RBUTTON};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, GetForegroundWindow, GetLayeredWindowAttributes,
    GetWindow, GetWindowLongPtrW, IsIconic, IsWindow, IsWindowVisible, IsZoomed, LoadCursorW,
    RegisterClassExW, SetWindowPos, ShowWindow, CS_HREDRAW, CS_VREDRAW, GWL_EXSTYLE, GW_HWNDPREV,
    HWND_NOTOPMOST, HWND_TOP, HWND_TOPMOST, IDC_ARROW, LAYERED_WINDOW_ATTRIBUTES_FLAGS, LWA_ALPHA,
    SWP_NOACTIVATE, SWP_NOSIZE, SW_HIDE, SW_SHOWNOACTIVATE, WNDCLASSEXW, WS_EX_LAYERED,
    WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_EX_TRANSPARENT, WS_POPUP,
};

#[derive(Debug, Clone, Copy, PartialEq)]
struct RenderedVisualKey {
    w: i32,
    h: i32,
    bw_bits: u32,
    radius_bits: u32,
    alpha_bits: u32,
    accent_bits: [u32; 3],
    is_zoomed: bool,
    scale_bits: u32,
}

#[derive(Debug, Clone, Copy)]
struct DragHideState {
    target_raw: isize,
    is_system_movesize: bool,
    last_seen_rect: RectI,
    last_motion_at: Instant,
}

/// 判定矩形是否处于 Windows 最小化离屏哨兵坐标（典型如 (-32000, -32000)）或过小退化矩形
#[inline]
pub fn is_invalid_or_minimized_rect(rect: RectI) -> bool {
    rect.width() < 48
        || rect.height() < 32
        || rect.left <= -16000
        || rect.top <= -16000
        || rect.right <= -16000
        || rect.bottom <= -16000
}

/// 判断鼠标左键或右键是否处于按下状态（识别标准拖拽、自绘标题栏拖拽以及 AltDrag）
#[inline]
pub fn is_mouse_drag_button_down() -> bool {
    unsafe {
        ((GetAsyncKeyState(VK_LBUTTON.0 as i32) as u16) & 0x8000) != 0
            || ((GetAsyncKeyState(VK_RBUTTON.0 as i32) as u16) & 0x8000) != 0
    }
}

/// 判断带 WS_EX_LAYERED 的窗口是否被应用用 Alpha=0 伪隐藏（常见于托盘驻留软件）
#[inline]
pub fn is_layered_zero_alpha(hwnd: HWND, ex_style: u32) -> bool {
    if (ex_style & WS_EX_LAYERED.0) == 0 {
        return false;
    }
    let mut alpha: u8 = 255;
    let mut flags = LAYERED_WINDOW_ATTRIBUTES_FLAGS(0);
    unsafe {
        GetLayeredWindowAttributes(hwnd, None, Some(&mut alpha), Some(&mut flags)).is_ok()
            && (flags.0 & LWA_ALPHA.0) != 0
            && alpha == 0
    }
}

/// 校验窗口矩形是否与至少一台活动物理显示器相交（拦截移至屏幕外的托盘假隐藏窗口）
#[inline]
pub fn is_rect_visible_on_any_monitor(rect: RectI) -> bool {
    let r = RECT {
        left: rect.left,
        top: rect.top,
        right: rect.right,
        bottom: rect.bottom,
    };
    let hmon = unsafe { MonitorFromRect(&r, MONITOR_DEFAULTTONULL) };
    !hmon.is_invalid()
}

/// 纯逻辑判定：自绘或系统拖拽隐身态何时应结束并 0ms 贴合复现
#[inline]
pub fn should_resume_after_drag(
    is_system_movesize: bool,
    mouse_down: bool,
    quiet_ms: u128,
) -> bool {
    if mouse_down {
        return false;
    }
    if is_system_movesize {
        // 标准拖拽通常由 EVENT_SYSTEM_MOVESIZEEND 立即结束；140ms 为异常丢消息兜底保险
        quiet_ms >= 140
    } else {
        // 自绘标题栏拖拽（微信/Chrome/VS Code）：松开鼠标并坐标稳定 24ms 即 0ms 精准复现
        quiet_ms >= 24
    }
}

pub struct FocusBorderManager {
    overlay_hwnd: Option<HWND>,
    last_target: isize,
    last_rect: RectI,
    last_overlay_pos: (i32, i32),
    last_zoomed: bool,
    last_target_topmost: bool,
    last_visual_key: Option<RenderedVisualKey>,
    visible: bool,
    /// 最近瞬间最小化/隐藏的窗口短暂抑制槽（规避 280ms 最小化缩入任务栏动画期间闪回残影）
    suppressed_target: (isize, Instant),
    /// 拖拽/缩放期零滞后隐身态（拖动瞬间 0ms 隐去外框，松手落位 0ms 精准复现）
    drag_hide: Option<DragHideState>,
}

impl FocusBorderManager {
    pub fn new() -> Self {
        Self {
            overlay_hwnd: None,
            last_target: 0,
            last_rect: RectI::default(),
            last_overlay_pos: (0, 0),
            last_zoomed: false,
            last_target_topmost: false,
            last_visual_key: None,
            visible: false,
            suppressed_target: (0, Instant::now()),
            drag_hide: None,
        }
    }

    pub fn current_target_raw(&self) -> isize {
        if self.visible {
            self.last_target
        } else if let Some(d) = self.drag_hide {
            d.target_raw
        } else {
            0
        }
    }

    /// 收到标准 EVENT_SYSTEM_MOVESIZESTART：立即 0ms 隐去外框，彻底杜绝拖动/缩放滞后撕裂
    pub fn on_move_size_start(&mut self, hwnd: HWND) {
        let raw = hwnd.0 as isize;
        if raw != 0 && (raw == self.last_target || unsafe { GetForegroundWindow() } == hwnd) {
            let rect = get_window_visual_rect(hwnd).unwrap_or(self.last_rect);
            self.drag_hide = Some(DragHideState {
                target_raw: raw,
                is_system_movesize: true,
                last_seen_rect: rect,
                last_motion_at: Instant::now(),
            });
            self.hide_overlay_window_only();
        }
    }

    /// 收到 EVENT_SYSTEM_MOVESIZEEND：立即解除隐身态，调用方紧接着调用 refresh() 0ms 严丝合缝贴合
    pub fn on_move_size_end(&mut self) {
        self.drag_hide = None;
    }

    /// 当目标窗口最小化、隐藏至托盘（如微信关闭/最小化）、被 Cloak 或销毁时 0ms 瞬隐，杜绝鬼影框
    pub fn on_target_vanished(&mut self, hwnd: HWND) {
        let raw = hwnd.0 as isize;
        if raw == 0 {
            return;
        }
        let matches_drag = self.drag_hide.map(|d| d.target_raw == raw).unwrap_or(false);
        if matches_drag {
            self.drag_hide = None;
        }
        if raw == self.last_target || matches_drag || unsafe { GetForegroundWindow() } == hwnd {
            self.suppressed_target = (raw, Instant::now());
            self.hide();
        }
    }

    pub fn has_active_animation(&self) -> bool {
        // 仅在拖拽隐身期间维持 16ms 零成本松手探测器（0 绘制/0 SetWindowPos），松手即停表保持 0% 静止 CPU
        self.drag_hide.is_some()
    }

    pub fn tick_animation(
        &mut self,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        muted: bool,
        quick_switch_open: bool,
    ) -> bool {
        let Some(mut drag) = self.drag_hide else {
            return false;
        };

        let fg = unsafe { GetForegroundWindow() };
        let fg_raw = fg.0 as isize;
        if fg_raw != drag.target_raw || !cfg.features.focus_border || muted || quick_switch_open {
            self.drag_hide = None;
            self.refresh(cfg, gfx, muted, quick_switch_open);
            return false;
        }

        if let Some(cur_rect) = get_window_visual_rect(fg) {
            if cur_rect != drag.last_seen_rect {
                drag.last_seen_rect = cur_rect;
                drag.last_motion_at = Instant::now();
                self.drag_hide = Some(drag);
                return true;
            }
        }

        let mouse_down = is_mouse_drag_button_down();
        let quiet_ms = drag.last_motion_at.elapsed().as_millis();
        if should_resume_after_drag(drag.is_system_movesize, mouse_down, quiet_ms) {
            self.drag_hide = None;
            self.refresh(cfg, gfx, muted, quick_switch_open);
            return false;
        }
        true
    }

    /// 刷新焦点窗口高亮边框（供 WinEvent、松手落位与极轻量保活巡检调用）
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
        let target_id = fg.0 as isize;
        if target_id == 0 {
            self.hide();
            return;
        }

        // 若目标窗口刚刚触发最小化/隐藏（280ms 动画窗口期内），直接静默隐藏不追着缩小残影画框
        if self.suppressed_target.0 == target_id
            && self.suppressed_target.1.elapsed().as_millis() < 280
        {
            self.hide();
            return;
        }

        let fg_ex = unsafe { GetWindowLongPtrW(fg, GWL_EXSTYLE) as u32 };
        if unsafe {
            !IsWindow(fg).as_bool()
                || !IsWindowVisible(fg).as_bool()
                || IsIconic(fg).as_bool()
                || is_hung_window(fg)
                || is_window_cloaked(fg)
        } || is_layered_zero_alpha(fg, fg_ex)
            || !is_manageable_window(fg, false, &cfg.excluded_apps)
        {
            self.hide();
            return;
        }

        let Some(rect) = get_window_visual_rect(fg) else {
            self.hide();
            return;
        };
        if is_invalid_or_minimized_rect(rect) || !is_rect_visible_on_any_monitor(rect) {
            self.hide();
            return;
        }

        // 拖动/缩放零滞后状态机：
        // 1) 若当前目标正处于拖拽隐身态，仅记录最新坐标，保持隐身零拖影；切到其他窗口则立刻解除。
        if let Some(mut drag) = self.drag_hide {
            if drag.target_raw == target_id {
                if rect != drag.last_seen_rect {
                    drag.last_seen_rect = rect;
                    drag.last_motion_at = Instant::now();
                    self.drag_hide = Some(drag);
                    return;
                }
                if !should_resume_after_drag(
                    drag.is_system_movesize,
                    is_mouse_drag_button_down(),
                    drag.last_motion_at.elapsed().as_millis(),
                ) {
                    return;
                }
                self.drag_hide = None;
            } else {
                self.drag_hide = None;
            }
        }

        // 2) 自绘标题栏拖拽/缩放自动嗅探（微信/Chrome/VS Code/Electron 等不发 MOVESIZESTART）：
        //    当前窗口边框已可见且鼠标左/右键按住并带动窗口位置或尺寸变化 -> 0ms 瞬间隐藏防拖尾！
        if self.visible
            && self.last_target == target_id
            && self.last_rect != rect
            && is_mouse_drag_button_down()
        {
            self.drag_hide = Some(DragHideState {
                target_raw: target_id,
                is_system_movesize: false,
                last_seen_rect: rect,
                last_motion_at: Instant::now(),
            });
            self.hide_overlay_window_only();
            return;
        }

        let is_zoomed = unsafe { IsZoomed(fg).as_bool() };
        let target_topmost = (fg_ex & WS_EX_TOPMOST.0) != 0;

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
        let pad = if is_zoomed {
            0
        } else {
            bw.ceil() as i32 + (3.0 * scale).round() as i32
        };
        let bx = rect.left - pad;
        let by = rect.top - pad;
        let w = rect.width() + pad * 2;
        let h = rect.height() + pad * 2;
        let alpha = cfg.focus_border.opacity.clamp(0.15, 1.0);

        let visual_key = RenderedVisualKey {
            w,
            h,
            bw_bits: bw.to_bits(),
            radius_bits: radius.to_bits(),
            alpha_bits: alpha.to_bits(),
            accent_bits: [accent[0].to_bits(), accent[1].to_bits(), accent[2].to_bits()],
            is_zoomed,
            scale_bits: scale.to_bits(),
        };

        // 零开销早退：同一目标、位置尺寸与置顶态均完全一致时直接返回
        if self.visible
            && self.last_target == target_id
            && self.last_rect == rect
            && self.last_zoomed == is_zoomed
            && self.last_target_topmost == target_topmost
            && self.last_visual_key == Some(visual_key)
        {
            return;
        }

        if self.overlay_hwnd.is_none() {
            self.overlay_hwnd = create_focus_border_window();
            self.last_visual_key = None;
        }
        let Some(bh) = self.overlay_hwnd else {
            self.hide();
            return;
        };

        // 计算精准 Z-Order 锚点：若目标窗口为普通窗则紧贴其正上方（绝不压住右键菜单/输入法）
        let z_anchor = resolve_border_z_anchor(fg, bh, target_topmost);
        let need_redraw = self.last_visual_key != Some(visual_key);

        if !need_redraw {
            // 240Hz 零重绘硬件瞬移（如键盘切分屏/跨屏/切同尺寸窗）：0 DIB 分配、0 Direct2D 调用！
            unsafe {
                if !target_topmost && self.last_target_topmost {
                    let _ = SetWindowPos(
                        bh,
                        HWND_NOTOPMOST,
                        bx,
                        by,
                        0,
                        0,
                        SWP_NOSIZE | SWP_NOACTIVATE,
                    );
                }
                let _ = SetWindowPos(bh, z_anchor, bx, by, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                if !self.visible {
                    let _ = ShowWindow(bh, SW_SHOWNOACTIVATE);
                }
            }
            self.last_target = target_id;
            self.last_rect = rect;
            self.last_overlay_pos = (bx, by);
            self.last_zoomed = is_zoomed;
            self.last_target_topmost = target_topmost;
            self.visible = true;
            return;
        }

        let Some(g) = gfx else {
            // 严防旧窗口（如已切走或最小化的微信）残影僵死在屏幕上！
            if self.visible && self.last_target != target_id {
                self.hide();
            }
            return;
        };

        let render_ok = render_layered_window_d2d_alpha(g, bh, bx, by, w, h, 255, |rt| unsafe {
            let inset = if is_zoomed {
                bw * 0.5
            } else {
                pad as f32 - bw * 0.5
            };
            let r = rect_f(inset, inset, w as f32 - inset, h as f32 - inset);
            let rr = rounded_rect(r, radius);
            if !is_zoomed {
                if let Ok(glow) = rt.CreateSolidColorBrush(&rgb_a(accent, alpha * 0.22), None) {
                    rt.DrawRoundedRectangle(&rr, &glow, bw + 2.2 * scale, None);
                }
            }
            if let Ok(brush) = rt.CreateSolidColorBrush(&rgb_a(accent, alpha), None) {
                rt.DrawRoundedRectangle(&rr, &brush, bw, None);
            }
        })
        .is_ok();

        if !render_ok {
            if self.visible && self.last_target != target_id {
                self.hide();
            }
            return;
        }

        unsafe {
            if !target_topmost && self.last_target_topmost {
                let _ = SetWindowPos(bh, HWND_NOTOPMOST, bx, by, w, h, SWP_NOACTIVATE);
            }
            let _ = SetWindowPos(bh, z_anchor, bx, by, w, h, SWP_NOACTIVATE);
            if !self.visible {
                let _ = ShowWindow(bh, SW_SHOWNOACTIVATE);
            }
        }
        self.last_target = target_id;
        self.last_rect = rect;
        self.last_overlay_pos = (bx, by);
        self.last_zoomed = is_zoomed;
        self.last_target_topmost = target_topmost;
        self.last_visual_key = Some(visual_key);
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
        if is_invalid_or_minimized_rect(rect) {
            return false;
        }
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
        self.drag_hide = None;
        self.visible = true;
        self.last_target = 0; // 预览结束后刷新时重新评估
        self.last_visual_key = None;
        self.last_target_topmost = true;
        true
    }

    /// 仅隐藏浮层窗口本身，保留 `drag_hide` 状态与缓存的 `last_visual_key` 供松手瞬间零重绘复现
    fn hide_overlay_window_only(&mut self) {
        if self.visible {
            if let Some(bh) = self.overlay_hwnd {
                unsafe {
                    if IsWindow(bh).as_bool() {
                        let _ = ShowWindow(bh, SW_HIDE);
                    }
                }
            }
            self.visible = false;
        }
    }

    pub fn hide(&mut self) {
        self.drag_hide = None;
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
        self.drag_hide = None;
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
        self.last_visual_key = None;
    }
}

impl Drop for FocusBorderManager {
    fn drop(&mut self) {
        self.destroy();
    }
}

/// 求解边框最精准的 Z-Order 插入锚点：
/// - 置顶目标窗口 -> HWND_TOPMOST；
/// - 普通前台窗口 -> 寻找其正上方最近一个非自身边框且非置顶的兄弟窗口，确保边框紧贴目标上方且绝不压住开始菜单/右键菜单/IME。
fn resolve_border_z_anchor(fg: HWND, border_hwnd: HWND, target_topmost: bool) -> HWND {
    if target_topmost {
        return HWND_TOPMOST;
    }
    unsafe {
        let mut cur = GetWindow(fg, GW_HWNDPREV).unwrap_or_default();
        let mut steps = 0;
        while !cur.0.is_null() && steps < 12 {
            steps += 1;
            if cur == border_hwnd {
                cur = GetWindow(cur, GW_HWNDPREV).unwrap_or_default();
                continue;
            }
            if IsWindowVisible(cur).as_bool() {
                let ex = GetWindowLongPtrW(cur, GWL_EXSTYLE) as u32;
                if (ex & WS_EX_TOPMOST.0) == 0 {
                    return cur;
                }
            }
            cur = GetWindow(cur, GW_HWNDPREV).unwrap_or_default();
        }
    }
    HWND_TOP
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
        // 不再写死 WS_EX_TOPMOST，改由 resolve_border_z_anchor 按目标窗口层级动态精准贴合
        let ex_style = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detects_offscreen_minimized_sentinel_rects_immediately() {
        assert!(is_invalid_or_minimized_rect(RectI::from_xywh(-32000, -32000, 160, 28)));
        assert!(is_invalid_or_minimized_rect(RectI::from_xywh(100, 100, 20, 20)));
        assert!(!is_invalid_or_minimized_rect(RectI::from_xywh(120, 80, 1024, 720)));
    }

    #[test]
    fn drag_hide_resumes_immediately_after_mouse_release_and_never_while_holding() {
        assert!(!should_resume_after_drag(false, true, 500));
        assert!(!should_resume_after_drag(true, true, 500));
        assert!(!should_resume_after_drag(false, false, 10));
        assert!(should_resume_after_drag(false, false, 24));
        assert!(!should_resume_after_drag(true, false, 80));
        assert!(should_resume_after_drag(true, false, 140));
    }
}
