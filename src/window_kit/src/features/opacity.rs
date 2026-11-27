// ─────────────────────────────────────────────────────────────────────────────
// features/opacity.rs — 窗口无级透明度调节（快捷键步进 + Alt+滚轮微调）
//                       精准维护 WS_EX_LAYERED 归属权：回到 100% 时自动移除附加标志
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::WindowKitConfig;
use crate::core::constants::{OPACITY_MAX_PERCENT, OPACITY_MIN_PERCENT_FLOOR};
use crate::core::win_utils::{
    is_hung_window, is_manageable_window, register_opacity_recovery_hwnd,
    resolve_foreground_or_target_hwnd, unregister_opacity_recovery_hwnd,
};
use crate::wk_info;
use std::collections::HashMap;
use windows::Win32::Foundation::{COLORREF, HWND};
use windows::Win32::UI::WindowsAndMessaging::{
    GetLayeredWindowAttributes, GetWindowLongPtrW, IsWindow,
    SetLayeredWindowAttributes, SetWindowLongPtrW, WindowFromPoint, GWL_EXSTYLE, LWA_ALPHA,
    WS_EX_LAYERED,
};
use windows::Win32::Foundation::POINT;
use windows::Win32::UI::WindowsAndMessaging::{GetAncestor, GetCursorPos, GA_ROOT};

struct OpacityState {
    percent: u8,
    added_layered_style: bool,
}

pub struct OpacityManager {
    tracked: HashMap<isize, OpacityState>,
}

impl OpacityManager {
    pub fn new() -> Self {
        Self {
            tracked: HashMap::new(),
        }
    }

    /// 调整当前前台窗口透明度（delta_steps > 0 增透更不透明，< 0 降低不透明度）
    pub fn adjust_foreground(
        &mut self,
        delta_steps: i32,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> bool {
        let fg = resolve_foreground_or_target_hwnd();
        self.adjust_window(fg, delta_steps, cfg, trace_id)
    }

    /// 调整鼠标光标下方根窗口透明度（用于 Alt + 鼠标滚轮）
    pub fn adjust_under_cursor(
        &mut self,
        wheel_delta: i16,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> bool {
        let mut pt = POINT::default();
        if unsafe { GetCursorPos(&mut pt) }.is_err() {
            return false;
        }
        let child = unsafe { WindowFromPoint(pt) };
        if child.0.is_null() {
            return false;
        }
        let root = unsafe { GetAncestor(child, GA_ROOT) };
        let target = if root.0.is_null() { child } else { root };
        let steps = if wheel_delta >= 0 { 1 } else { -1 };
        self.adjust_window(target, steps, cfg, trace_id)
    }

    /// 重置当前前台窗口为 100% 不透明
    pub fn reset_foreground(&mut self, cfg: &WindowKitConfig, trace_id: &str) -> bool {
        let fg = resolve_foreground_or_target_hwnd();
        self.set_window_percent(fg, OPACITY_MAX_PERCENT, cfg, trace_id)
    }

    fn adjust_window(
        &mut self,
        hwnd: HWND,
        delta_steps: i32,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> bool {
        if hwnd.0.is_null() || !is_manageable_window(hwnd, false, &cfg.excluded_apps) {
            return false;
        }
        let current = self.read_current_percent(hwnd);
        let step = cfg.opacity.step_percent.max(1) as i32;
        let min_p = cfg.opacity.min_percent.max(OPACITY_MIN_PERCENT_FLOOR) as i32;
        let next = (current as i32 + delta_steps * step).clamp(min_p, OPACITY_MAX_PERCENT as i32) as u8;
        self.set_window_percent(hwnd, next, cfg, trace_id)
    }

    pub fn set_window_percent(
        &mut self,
        hwnd: HWND,
        percent: u8,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> bool {
        if hwnd.0.is_null()
            || is_hung_window(hwnd)
            || !is_manageable_window(hwnd, false, &cfg.excluded_apps)
        {
            return false;
        }
        let min_p = cfg.opacity.min_percent.max(OPACITY_MIN_PERCENT_FLOOR);
        let clamped = percent.clamp(min_p, OPACITY_MAX_PERCENT);
        let key = hwnd.0 as isize;

        unsafe {
            let ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
            let has_layered = (ex_style & WS_EX_LAYERED.0) != 0;

            if clamped >= OPACITY_MAX_PERCENT {
                let _ = SetLayeredWindowAttributes(hwnd, COLORREF(0), 255, LWA_ALPHA);
                unregister_opacity_recovery_hwnd(hwnd);
                if let Some(st) = self.tracked.remove(&key) {
                    if st.added_layered_style && has_layered {
                        let new_style = (ex_style & !WS_EX_LAYERED.0) as isize;
                        let _ = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new_style);
                    }
                }
                wk_info!(trace_id, "opacity", "reset hwnd=0x{:x} to 100%", key);
                return true;
            }

            let entry = self.tracked.entry(key).or_insert(OpacityState {
                percent: OPACITY_MAX_PERCENT,
                added_layered_style: !has_layered,
            });
            if !has_layered {
                let new_style = (ex_style | WS_EX_LAYERED.0) as isize;
                let _ = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new_style);
            }
            let alpha = ((clamped as f32 / 100.0) * 255.0).round().clamp(1.0, 255.0) as u8;
            if SetLayeredWindowAttributes(hwnd, COLORREF(0), alpha, LWA_ALPHA).is_ok() {
                register_opacity_recovery_hwnd(hwnd);
                entry.percent = clamped;
                wk_info!(
                    trace_id,
                    "opacity",
                    "set hwnd=0x{:x} opacity={}% (alpha={})",
                    key,
                    clamped,
                    alpha
                );
                return true;
            }
        }
        false
    }

    fn read_current_percent(&mut self, hwnd: HWND) -> u8 {
        let key = hwnd.0 as isize;
        if let Some(st) = self.tracked.get(&key) {
            return st.percent;
        }
        unsafe {
            let ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
            if (ex_style & WS_EX_LAYERED.0) != 0 {
                let mut alpha = 255u8;
                let mut flags = windows::Win32::UI::WindowsAndMessaging::LAYERED_WINDOW_ATTRIBUTES_FLAGS(0);
                if GetLayeredWindowAttributes(hwnd, None, Some(&mut alpha), Some(&mut flags)).is_ok()
                    && (flags.0 & LWA_ALPHA.0) != 0
                {
                    return ((alpha as f32 / 255.0) * 100.0).round() as u8;
                }
            }
        }
        OPACITY_MAX_PERCENT
    }

    /// 退出或关闭特性时清理已失效句柄并恢复所有由我们附加的透明度窗口
    pub fn restore_all(&mut self) {
        for (key, st) in self.tracked.drain() {
            let hwnd = HWND(key as *mut std::ffi::c_void);
            unregister_opacity_recovery_hwnd(hwnd);
            unsafe {
                if IsWindow(hwnd).as_bool() && !is_hung_window(hwnd) {
                    let _ = SetLayeredWindowAttributes(hwnd, COLORREF(0), 255, LWA_ALPHA);
                    if st.added_layered_style {
                        let ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
                        if (ex_style & WS_EX_LAYERED.0) != 0 {
                            let _ = SetWindowLongPtrW(
                                hwnd,
                                GWL_EXSTYLE,
                                (ex_style & !WS_EX_LAYERED.0) as isize,
                            );
                        }
                    }
                }
            }
        }
    }
}

impl Drop for OpacityManager {
    fn drop(&mut self) {
        self.restore_all();
    }
}
