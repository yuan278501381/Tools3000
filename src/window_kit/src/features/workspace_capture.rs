// ─────────────────────────────────────────────────────────────────────────────
// features/workspace_capture.rs — Workspaces v2 正红 4px 细边摆拍 + Slint Fluent 捕获浮卡
//
// 世界级架构亮点:
//   1. 每屏 4 条 4px 纯色正红细边窗（RGB(220, 38, 38)，非洋红），纯 GDI 零大位图，
//      每屏仅占用 ~190 KB，中间整片桌面 100% 原生穿透；
//   2. 居中捕获浮卡由 Slint Fluent 声明式引擎渲染（1:1 对标 PowerToys Workspaces 居中卡片，
//      自动联动 Tools3000 浅色/深色与动态强调色），一键「捕获」瞬间采下全量应用快照写盘通知宿主；
//   3. 多屏热插拔/分辨率变更自动重建每屏 4 条正红细边并自愈卡片位置。
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::WindowKitConfig;
use crate::core::constants::{
    FILE_WORKSPACE_CAPTURE_RESULT, HOST_MESSAGE_WINDOW_CLASS,
    HOST_WORKSPACE_CAPTURE_RESULT_MSG_NAME, WORKSPACE_CAPTURE_FRAME_CLASS,
};
use crate::core::geometry::RectI;
use crate::core::topology::{
    ensure_window_rect_on_live_monitors, is_window_on_current_virtual_desktop,
};
use crate::core::win_utils::{
    enumerate_monitors, enumerate_user_windows, monitor_for_point, to_wide, write_text_file_atomic,
    MonitorInfo,
};
use crate::features::workspaces::capture_workspace_v2_json;
use crate::render::d2d::SharedGraphics;
use crate::ui_slint::{
    hide_workspace_capture, show_or_update_workspace_capture, SlintThemeSpec,
    WorkspaceCaptureViewState,
};
use crate::{wk_debug, wk_info, wk_warn};
use std::ffi::c_void;
use std::mem::size_of;
use std::path::Path;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Once;
use windows::core::PCWSTR;
use windows::Win32::Foundation::{COLORREF, HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    BeginPaint, CreateSolidBrush, DeleteObject, EndPaint, FillRect, PAINTSTRUCT,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, FindWindowW, GetClientRect, GetCursorPos,
    IsWindow, LoadCursorW, PostMessageW, RegisterClassExW, RegisterWindowMessageW,
    SetLayeredWindowAttributes, ShowWindow, CS_HREDRAW, CS_VREDRAW, IDC_ARROW, LWA_ALPHA, SW_HIDE,
    SW_SHOWNOACTIVATE, WM_PAINT, WNDCLASSEXW, WS_EX_LAYERED, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW,
    WS_EX_TOPMOST, WS_EX_TRANSPARENT, WS_POPUP,
};

/// 正红色边框（RGB(220, 38, 38)，绝非洋红；Win32 COLORREF 为 0x00BBGGRR）
const CAPTURE_FRAME_COLORREF: COLORREF = COLORREF(0x0026_26DC);
const CAPTURE_BORDER_THICKNESS_DIP: f32 = 4.0;

static CAP_HOST_RESULT_MSG: AtomicU32 = AtomicU32::new(0);

fn capture_host_result_msg_id() -> u32 {
    let cached = CAP_HOST_RESULT_MSG.load(Ordering::Acquire);
    if cached != 0 {
        return cached;
    }
    let wide = to_wide(HOST_WORKSPACE_CAPTURE_RESULT_MSG_NAME);
    let msg = unsafe { RegisterWindowMessageW(PCWSTR(wide.as_ptr())) };
    CAP_HOST_RESULT_MSG.store(msg, Ordering::Release);
    msg
}

pub struct StartCaptureRequest {
    pub workspace_id: Option<String>,
    pub restore_settings_hwnd: Option<isize>,
    pub ui_lang: String,
}

struct CaptureSession {
    frame_hwnds: Vec<HWND>, // 每屏 4 条正红 4px 细边窗
    monitors: Vec<MonitorInfo>,
    card_rect: RectI,
    zh_ui: bool,
    workspace_id: Option<String>,
    restore_settings_hwnd: Option<isize>,
    detected_window_count: usize,
}

pub struct WorkspaceCaptureManager {
    session: Option<CaptureSession>,
}

impl WorkspaceCaptureManager {
    pub fn new(_control_hwnd: HWND) -> Self {
        Self { session: None }
    }

    pub fn set_control_hwnd(&mut self, _hwnd: HWND) {}

    pub fn is_open(&self) -> bool {
        self.session.is_some()
    }

    /// 启动全多屏 4px 正红细边摆拍模式 + Slint Fluent 居中捕获控制卡片
    pub fn start(
        &mut self,
        req: StartCaptureRequest,
        cfg: &WindowKitConfig,
        _gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> bool {
        if let Some(sess) = self.session.as_mut() {
            let exclude_hwnd = sess
                .restore_settings_hwnd
                .filter(|&raw| raw > 0)
                .map(|raw| HWND(raw as *mut c_void));
            sess.detected_window_count = count_capturable_windows(&cfg.excluded_apps, exclude_hwnd);
            push_capture_card_to_slint(sess, cfg, false);
            return true;
        }

        let monitors = enumerate_monitors();
        if monitors.is_empty() {
            return false;
        }

        let target_mon = {
            let mut pt = POINT::default();
            if unsafe { GetCursorPos(&mut pt) }.is_ok() {
                monitor_for_point(pt.x, pt.y, &monitors)
            } else {
                None
            }
        }
        .unwrap_or_else(|| monitors[0].clone());

        let scale = (target_mon.dpi as f32 / 96.0).max(1.0);
        let card_w = (396.0 * scale).round() as i32;
        let card_h = (158.0 * scale).round() as i32;
        let work = target_mon.work_rect;
        let card_rect = RectI::from_xywh(
            work.left + (work.width() - card_w) / 2,
            work.top + (work.height() - card_h) / 2,
            card_w,
            card_h,
        );

        let frame_hwnds = create_all_red_frame_strips(&monitors);
        let exclude_hwnd = req
            .restore_settings_hwnd
            .filter(|&raw| raw > 0)
            .map(|raw| HWND(raw as *mut c_void));
        let detected = count_capturable_windows(&cfg.excluded_apps, exclude_hwnd);
        let zh_ui = !req.ui_lang.trim().to_ascii_lowercase().starts_with("en");

        let sess = CaptureSession {
            frame_hwnds,
            monitors,
            card_rect,
            zh_ui,
            workspace_id: req.workspace_id,
            restore_settings_hwnd: req.restore_settings_hwnd,
            detected_window_count: detected,
        };

        if !push_capture_card_to_slint(&sess, cfg, false) {
            destroy_frame_strips(sess.frame_hwnds);
            return false;
        }

        wk_info!(
            trace_id,
            "workspaceCapture",
            "started 4px red-frame Slint capture across {} monitor(s) ({} strip windows, detected={} wins)",
            sess.monitors.len(),
            sess.frame_hwnds.len(),
            sess.detected_window_count
        );
        self.session = Some(sess);
        true
    }

    /// 多屏热插拔/分辨率变更时自动重建每屏 4 条正红细边并将 Slint 卡片拉回在线屏幕
    pub fn on_topology_changed(&mut self, cfg: &WindowKitConfig, _gfx: Option<&SharedGraphics>) {
        let Some(sess) = self.session.as_mut() else {
            return;
        };
        let new_monitors = enumerate_monitors();
        if new_monitors.is_empty() {
            return;
        }
        let old_strips = std::mem::take(&mut sess.frame_hwnds);
        destroy_frame_strips(old_strips);
        sess.frame_hwnds = create_all_red_frame_strips(&new_monitors);
        sess.monitors = new_monitors;

        sess.card_rect = ensure_window_rect_on_live_monitors(
            "",
            None,
            sess.card_rect.width(),
            sess.card_rect.height(),
            sess.monitors.first(),
            &sess.monitors,
        );
        push_capture_card_to_slint(sess, cfg, false);
    }

    pub fn cancel_and_close(&mut self, trace_id: &str) {
        if let Some(sess) = self.session.take() {
            let restore = sess.restore_settings_hwnd.is_some();
            hide_workspace_capture();
            destroy_frame_strips(sess.frame_hwnds);
            notify_host_capture_result(false, restore, trace_id);
        }
    }

    /// 处理 Slint 捕获卡片回传的确认/取消决策；返回 true 表示捕获会话已结束
    pub fn handle_slint_decision(
        &mut self,
        do_capture: bool,
        cfg: &WindowKitConfig,
        config_dir: &Path,
        trace_id: &str,
    ) -> bool {
        let Some(sess) = self.session.take() else {
            return false;
        };
        let workspace_id = sess.workspace_id.clone();
        let restore_hwnd = sess.restore_settings_hwnd;

        // 先彻底隐藏 Slint 浮卡并销毁全部正红细边，再采样桌面确保自身 0 污染！
        hide_workspace_capture();
        destroy_frame_strips(sess.frame_hwnds);

        let mut captured_ok = false;
        if do_capture {
            let exclude_hwnd = restore_hwnd
                .filter(|&raw| raw > 0)
                .map(|raw| HWND(raw as *mut c_void));
            let mut payload = capture_workspace_v2_json(cfg, exclude_hwnd);
            if let Some(ws_id) = workspace_id {
                if !ws_id.trim().is_empty() {
                    payload.insert(
                        "targetWorkspaceId",
                        crate::core::json::JsonValue::String(ws_id),
                    );
                }
            }
            let out_path = config_dir.join(FILE_WORKSPACE_CAPTURE_RESULT);
            match write_text_file_atomic(&out_path, &payload.to_string_pretty()) {
                Ok(()) => {
                    captured_ok = true;
                    wk_info!(
                        trace_id,
                        "workspaceCapture",
                        "captured workspace snapshot -> {}",
                        out_path.display()
                    );
                }
                Err(e) => {
                    wk_warn!(
                        trace_id,
                        "workspaceCapture",
                        "failed to write capture snapshot: {}",
                        e
                    );
                }
            }
        } else {
            wk_debug!(trace_id, "workspaceCapture", "capture cancelled by user");
        }
        notify_host_capture_result(captured_ok, restore_hwnd.is_some(), trace_id);
        true
    }
}

impl Drop for WorkspaceCaptureManager {
    fn drop(&mut self) {
        if let Some(sess) = self.session.take() {
            hide_workspace_capture();
            destroy_frame_strips(sess.frame_hwnds);
        }
    }
}

fn push_capture_card_to_slint(sess: &CaptureSession, cfg: &WindowKitConfig, busy: bool) -> bool {
    show_or_update_workspace_capture(WorkspaceCaptureViewState {
        theme: SlintThemeSpec {
            dark: cfg.is_dark_theme(),
            accent_hex: cfg.accent_color.clone(),
        },
        window_rect: sess.card_rect,
        zh_ui: sess.zh_ui,
        detected_windows: sess.detected_window_count,
        busy,
    })
}

fn count_capturable_windows(excluded_apps: &[String], exclude_hwnd: Option<HWND>) -> usize {
    enumerate_user_windows(true, excluded_apps)
        .into_iter()
        .filter(|w| {
            if let Some(ex) = exclude_hwnd {
                if w.hwnd == ex {
                    return false;
                }
            }
            is_window_on_current_virtual_desktop(w.hwnd)
        })
        .count()
}

fn destroy_frame_strips(strips: Vec<HWND>) {
    unsafe {
        for h in strips {
            if IsWindow(h).as_bool() {
                let _ = ShowWindow(h, SW_HIDE);
                let _ = DestroyWindow(h);
            }
        }
    }
}

fn notify_host_capture_result(captured: bool, restore_settings: bool, trace_id: &str) {
    let cls = to_wide(HOST_MESSAGE_WINDOW_CLASS);
    if let Ok(host_hwnd) = unsafe { FindWindowW(PCWSTR(cls.as_ptr()), PCWSTR::null()) } {
        if !host_hwnd.0.is_null() {
            let msg_id = capture_host_result_msg_id();
            if msg_id != 0 {
                unsafe {
                    let _ = PostMessageW(
                        host_hwnd,
                        msg_id,
                        WPARAM(if captured { 1 } else { 0 }),
                        LPARAM(if restore_settings { 1 } else { 0 }),
                    );
                }
            }
        }
    }
    wk_debug!(
        trace_id,
        "workspaceCapture",
        "notified host captured={} restore={}",
        captured,
        restore_settings
    );
}

// ─────────────────────────────────────────────────────────────────────────────
// 每屏 4 条纯 GDI 4px 正红穿透细边窗（0 全屏位图开销）
// ─────────────────────────────────────────────────────────────────────────────

fn create_all_red_frame_strips(monitors: &[MonitorInfo]) -> Vec<HWND> {
    static REG: Once = Once::new();
    let cls = to_wide(WORKSPACE_CAPTURE_FRAME_CLASS);
    let hinst = unsafe { HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0) };

    REG.call_once(|| unsafe {
        let wc = WNDCLASSEXW {
            cbSize: size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(red_strip_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&wc);
    });

    let mut out = Vec::with_capacity(monitors.len() * 4);
    for mon in monitors {
        let s = (mon.dpi as f32 / 96.0).max(1.0);
        let t = (CAPTURE_BORDER_THICKNESS_DIP * s).round().max(3.0) as i32;
        let r = mon.physical_rect;
        let strips = [
            // 上边
            RectI::from_xywh(r.left, r.top, r.width(), t),
            // 下边
            RectI::from_xywh(r.left, r.bottom - t, r.width(), t),
            // 左边
            RectI::from_xywh(r.left, r.top + t, t, (r.height() - t * 2).max(1)),
            // 右边
            RectI::from_xywh(r.right - t, r.top + t, t, (r.height() - t * 2).max(1)),
        ];
        for sr in strips {
            if let Ok(h) = unsafe {
                CreateWindowExW(
                    WS_EX_TOPMOST
                        | WS_EX_TOOLWINDOW
                        | WS_EX_NOACTIVATE
                        | WS_EX_LAYERED
                        | WS_EX_TRANSPARENT,
                    PCWSTR(cls.as_ptr()),
                    PCWSTR::null(),
                    WS_POPUP,
                    sr.left,
                    sr.top,
                    sr.width().max(1),
                    sr.height().max(1),
                    None,
                    None,
                    hinst,
                    None,
                )
            } {
                unsafe {
                    let _ = SetLayeredWindowAttributes(h, COLORREF(0), 245, LWA_ALPHA);
                    let _ = ShowWindow(h, SW_SHOWNOACTIVATE);
                }
                out.push(h);
            }
        }
    }
    out
}

unsafe extern "system" fn red_strip_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if msg == WM_PAINT {
        let mut ps = PAINTSTRUCT::default();
        let hdc = BeginPaint(hwnd, &mut ps);
        let mut rc = RECT::default();
        let _ = GetClientRect(hwnd, &mut rc);
        let brush = CreateSolidBrush(CAPTURE_FRAME_COLORREF);
        let _ = FillRect(hdc, &rc, brush);
        let _ = DeleteObject(brush);
        let _ = EndPaint(hwnd, &ps);
        return LRESULT(0);
    }
    DefWindowProcW(hwnd, msg, wparam, lparam)
}
