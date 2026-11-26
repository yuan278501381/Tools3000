// ─────────────────────────────────────────────────────────────────────────────
// features/alt_drag.rs — Linux/macOS 风格 Alt+左键任意处拖动 & Alt+右键就近象限缩放
//                        支持最大化窗按光标比例原地还原不跳窗、支持与 SnapZones 联动吸附、
//                        所有实际窗口位置变更均在控制消息循环异步执行，绝不阻塞鼠标钩子
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::WindowKitConfig;
use crate::core::constants::{ALT_DRAG_MIN_WINDOW_HEIGHT, ALT_DRAG_MIN_WINDOW_WIDTH};
use crate::core::geometry::RectI;
use crate::core::win_utils::{is_hung_window, is_manageable_window, rect_from_win32};
use crate::wk_debug;
use windows::Win32::Foundation::{HWND, POINT, RECT};
use windows::Win32::UI::WindowsAndMessaging::{
    GetAncestor, GetWindowLongPtrW, GetWindowRect, IsWindow, IsZoomed, SetForegroundWindow,
    SetWindowPos, ShowWindow, WindowFromPoint, GA_ROOT, GWL_STYLE, HWND_TOP, SWP_NOACTIVATE,
    SWP_NOSIZE, SWP_NOZORDER, SW_RESTORE, WS_THICKFRAME,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AltDragMode {
    Move,
    Resize {
        left_edge: bool,
        top_edge: bool,
    },
}

struct ActiveDragSession {
    target: HWND,
    mode: AltDragMode,
    start_cursor: (i32, i32),
    start_rect: RectI,
}

pub struct AltDragManager {
    session: Option<ActiveDragSession>,
}

impl AltDragManager {
    pub fn new() -> Self {
        Self { session: None }
    }

    pub fn is_active(&self) -> bool {
        self.session.is_some()
    }

    /// 返回当前正在执行移动拖动的目标 HWND（用于与 SnapZones 实时联动吸附）
    pub fn active_move_hwnd(&self) -> Option<HWND> {
        self.session.as_ref().and_then(|s| {
            if matches!(s.mode, AltDragMode::Move) {
                Some(s.target)
            } else {
                None
            }
        })
    }

    /// 响应低级钩子转发的 Alt+左键 / Alt+右键 按下事件
    pub fn begin(
        &mut self,
        is_resize: bool,
        cursor_x: i32,
        cursor_y: i32,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> bool {
        self.session = None;
        if (!is_resize && !cfg.alt_drag.enable_move) || (is_resize && !cfg.alt_drag.enable_resize) {
            return false;
        }
        let child = unsafe {
            WindowFromPoint(POINT {
                x: cursor_x,
                y: cursor_y,
            })
        };
        if child.0.is_null() {
            return false;
        }
        let root = unsafe { GetAncestor(child, GA_ROOT) };
        let target = if root.0.is_null() { child } else { root };

        let excluded = cfg.combined_excluded_apps(&cfg.alt_drag.excluded_apps);
        if !is_manageable_window(target, false, &excluded) {
            return false;
        }

        // 若目标窗口处于最大化态，按光标在最大化宽度中的横向占比锚定还原，避免窗口瞬间跳离手下
        let mut pre_max_rect = RECT::default();
        let was_zoomed = unsafe { IsZoomed(target).as_bool() };
        if was_zoomed {
            unsafe {
                let _ = GetWindowRect(target, &mut pre_max_rect);
                let _ = ShowWindow(target, SW_RESTORE);
            }
        }
        unsafe {
            if cfg.alt_drag.bring_to_front {
                let _ = SetForegroundWindow(target);
            }
        }

        let mut rc = RECT::default();
        if unsafe { GetWindowRect(target, &mut rc) }.is_err() {
            return false;
        }
        let mut start_rect = rect_from_win32(rc);
        if !start_rect.is_valid() {
            return false;
        }

        if was_zoomed && !is_resize {
            let max_w = (pre_max_rect.right - pre_max_rect.left).max(1) as f64;
            let ratio_x = ((cursor_x - pre_max_rect.left) as f64 / max_w).clamp(0.15, 0.85);
            let new_left = cursor_x - ((start_rect.width() as f64) * ratio_x).round() as i32;
            let new_top = (cursor_y - 24).max(pre_max_rect.top);
            let shifted = RectI::from_xywh(new_left, new_top, start_rect.width(), start_rect.height());
            unsafe {
                let _ = SetWindowPos(
                    target,
                    HWND_TOP,
                    shifted.left,
                    shifted.top,
                    0,
                    0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE,
                );
            }
            start_rect = shifted;
        }

        let mode = if is_resize {
            // 仅对支持可调边框的窗口执行缩放
            let style = unsafe { GetWindowLongPtrW(target, GWL_STYLE) as u32 };
            if (style & WS_THICKFRAME.0) == 0 {
                return false;
            }
            let cx = start_rect.left + start_rect.width() / 2;
            let cy = start_rect.top + start_rect.height() / 2;
            AltDragMode::Resize {
                left_edge: cursor_x < cx,
                top_edge: cursor_y < cy,
            }
        } else {
            AltDragMode::Move
        };

        wk_debug!(
            trace_id,
            "altDrag",
            "begin mode={:?} hwnd=0x{:x} at ({},{})",
            mode,
            target.0 as isize,
            cursor_x,
            cursor_y
        );
        self.session = Some(ActiveDragSession {
            target,
            mode,
            start_cursor: (cursor_x, cursor_y),
            start_rect,
        });
        true
    }

    /// 处理合并后的鼠标移动位置
    pub fn update(&mut self, cursor_x: i32, cursor_y: i32) {
        let Some(sess) = self.session.as_ref() else {
            return;
        };
        if unsafe { !IsWindow(sess.target).as_bool() } || is_hung_window(sess.target) {
            self.session = None;
            return;
        }

        let dx = cursor_x - sess.start_cursor.0;
        let dy = cursor_y - sess.start_cursor.1;

        match sess.mode {
            AltDragMode::Move => {
                let nx = sess.start_rect.left + dx;
                let ny = sess.start_rect.top + dy;
                unsafe {
                    let _ = SetWindowPos(
                        sess.target,
                        HWND_TOP,
                        nx,
                        ny,
                        0,
                        0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE,
                    );
                }
            }
            AltDragMode::Resize {
                left_edge,
                top_edge,
            } => {
                let mut left = sess.start_rect.left;
                let mut right = sess.start_rect.right;
                let mut top = sess.start_rect.top;
                let mut bottom = sess.start_rect.bottom;

                if left_edge {
                    left = (sess.start_rect.left + dx)
                        .min(sess.start_rect.right - ALT_DRAG_MIN_WINDOW_WIDTH);
                } else {
                    right = (sess.start_rect.right + dx)
                        .max(sess.start_rect.left + ALT_DRAG_MIN_WINDOW_WIDTH);
                }
                if top_edge {
                    top = (sess.start_rect.top + dy)
                        .min(sess.start_rect.bottom - ALT_DRAG_MIN_WINDOW_HEIGHT);
                } else {
                    bottom = (sess.start_rect.bottom + dy)
                        .max(sess.start_rect.top + ALT_DRAG_MIN_WINDOW_HEIGHT);
                }

                unsafe {
                    let _ = SetWindowPos(
                        sess.target,
                        HWND_TOP,
                        left,
                        top,
                        right - left,
                        bottom - top,
                        SWP_NOZORDER | SWP_NOACTIVATE,
                    );
                }
            }
        }
    }

    /// 结束拖动并返回被移动的窗口句柄（供 SnapZones 联动完成吸附）
    pub fn end(&mut self) -> Option<HWND> {
        self.session.take().and_then(|s| {
            if matches!(s.mode, AltDragMode::Move) {
                Some(s.target)
            } else {
                None
            }
        })
    }
}
