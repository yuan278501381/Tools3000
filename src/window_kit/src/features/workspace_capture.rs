// ─────────────────────────────────────────────────────────────────────────────
// features/workspace_capture.rs — Workspaces v2 纯 Rust 正红 4px 细边摆拍捕获器
//
// 世界级架构亮点（复刻并超越 PowerToys Workspaces 图 2 体验）:
//   1. 每屏 4 条 4px 纯色正红细边窗（RGB(220, 38, 38)，非洋红），纯 GDI 零位图内存，
//      每屏仅占用 ~190 KB（比全屏 32bpp 4K 位图省 99.4%），中间整片桌面 100% 原生穿透，
//      摆拍期间用户可照常拖移窗口、使用 SnapZones 分屏、最小化想放任务栏的应用。
//   2. 居中可拖动轻量 Fluent 捕获卡片：实时展示已检测到窗口数量，一键「捕获」或按 Enter，
//      瞬间拍下当前虚拟桌面全量应用状态（含真实 PNG 图标、PEB CLI、管理员标识、SnapZones 槽位），
//      写盘并异步通知 C++ 宿主拉起设置页工作台精调。
//   3. 多屏热插拔/分辨率变更自动重建每屏 4 条正红细边，零内存与句柄泄漏。
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{parse_hex_color, WindowKitConfig};
use crate::core::constants::{
    FILE_WORKSPACE_CAPTURE_RESULT, HOST_MESSAGE_WINDOW_CLASS,
    HOST_WORKSPACE_CAPTURE_RESULT_MSG_NAME, WM_WK_INTERNAL_CAPTURE_EVENT,
    WORKSPACE_CAPTURE_CARD_CLASS, WORKSPACE_CAPTURE_FRAME_CLASS,
};
use crate::core::geometry::RectI;
use crate::core::topology::{
    apply_cross_os_popup_chrome, ensure_window_rect_on_live_monitors,
    is_window_on_current_virtual_desktop,
};
use crate::core::win_utils::{
    enumerate_monitors, enumerate_user_windows, monitor_for_point, to_wide, write_text_file_atomic,
    MonitorInfo,
};
use crate::features::workspaces::capture_workspace_v2_json;
use crate::render::d2d::{rect_f, rgb_a, rgba, rounded_rect, SharedGraphics};
use crate::{wk_debug, wk_info, wk_warn};
use std::ffi::c_void;
use std::mem::size_of;
use std::path::Path;
use std::sync::atomic::{AtomicI64, AtomicIsize, AtomicU32, Ordering};
use std::sync::Once;
use windows::core::{Interface, PCWSTR};
use windows::Win32::Foundation::{COLORREF, HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Direct2D::Common::{D2D_POINT_2F, D2D_SIZE_U};
use windows::Win32::Graphics::Direct2D::{
    ID2D1HwndRenderTarget, ID2D1RenderTarget, D2D1_DRAW_TEXT_OPTIONS_CLIP,
};
use windows::Win32::Graphics::DirectWrite::DWRITE_MEASURING_MODE_NATURAL;
use windows::Win32::Graphics::Gdi::{
    BeginPaint, CreateSolidBrush, DeleteObject, EndPaint, FillRect, PAINTSTRUCT,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::Input::KeyboardAndMouse::{
    ReleaseCapture, SetCapture, SetFocus, VK_ESCAPE, VK_RETURN,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, FindWindowW, GetClientRect, GetCursorPos,
    GetWindowRect, IsWindow, LoadCursorW, PostMessageW, RegisterClassExW, RegisterWindowMessageW,
    SetCursor, SetForegroundWindow, SetLayeredWindowAttributes, SetWindowPos, ShowWindow,
    CS_HREDRAW, CS_VREDRAW, HCURSOR, HTCLIENT, HWND_TOPMOST, IDC_ARROW, IDC_HAND, IDC_SIZEALL,
    LWA_ALPHA, SWP_NOACTIVATE, SWP_NOSIZE, SWP_NOZORDER, SW_HIDE, SW_SHOW, SW_SHOWNOACTIVATE,
    WM_CLOSE, WM_KEYDOWN, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE, WM_PAINT, WM_SETCURSOR,
    WNDCLASSEXW, WS_EX_LAYERED, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TOPMOST,
    WS_EX_TRANSPARENT, WS_POPUP,
};

/// 正红色边框（RGB(220, 38, 38)，绝非洋红；Win32 COLORREF 为 0x00BBGGRR）
const CAPTURE_FRAME_COLORREF: COLORREF = COLORREF(0x0026_26DC);
const CAPTURE_BORDER_THICKNESS_DIP: f32 = 4.0;

static CAP_CONTROL_HWND: AtomicIsize = AtomicIsize::new(0);
static CAP_CURSOR_KIND: AtomicU32 = AtomicU32::new(0);
static CAP_LATEST_PT: AtomicI64 = AtomicI64::new(0);
static CAP_MOVE_PENDING: AtomicU32 = AtomicU32::new(0);
static CAP_HOST_RESULT_MSG: AtomicU32 = AtomicU32::new(0);

const CURSOR_ARROW: u32 = 0;
const CURSOR_HAND: u32 = 1;
const CURSOR_SIZEALL: u32 = 2;

const CAP_EVT_MOUSEMOVE: usize = 1;
const CAP_EVT_LBUTTONDOWN: usize = 2;
const CAP_EVT_LBUTTONUP: usize = 3;
const CAP_EVT_KEYDOWN: usize = 4;
const CAP_EVT_PAINT: usize = 5;
const CAP_EVT_CLOSE: usize = 6;

fn pack_xy(x: i32, y: i32) -> i64 {
    ((x as u32 as u64) | ((y as u32 as u64) << 32)) as i64
}

fn unpack_xy(v: i64) -> (i32, i32) {
    let u = v as u64;
    ((u & 0xFFFF_FFFF) as u32 as i32, (u >> 32) as u32 as i32)
}

fn unpack_lparam_xy(lp: isize) -> (i32, i32) {
    let x = (lp as u32 & 0xFFFF) as i16 as i32;
    let y = (((lp as u32) >> 16) & 0xFFFF) as i16 as i32;
    (x, y)
}

fn capture_cursor_handle(kind: u32) -> HCURSOR {
    unsafe {
        let id = match kind {
            CURSOR_HAND => IDC_HAND,
            CURSOR_SIZEALL => IDC_SIZEALL,
            _ => IDC_ARROW,
        };
        LoadCursorW(None, id).unwrap_or_default()
    }
}

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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CaptureHit {
    DragHeader,
    BtnClose,
    BtnCapture,
    BtnCancel,
}

pub struct StartCaptureRequest {
    pub workspace_id: Option<String>,
    pub restore_settings_hwnd: Option<isize>,
    pub ui_lang: String,
}

struct CaptureSession {
    frame_hwnds: Vec<HWND>, // 每屏 4 条正红 4px 细边窗
    card_hwnd: HWND,
    rt: Option<ID2D1HwndRenderTarget>,
    monitors: Vec<MonitorInfo>,
    card_rect: RectI,
    scale: f32,
    zh_ui: bool,
    workspace_id: Option<String>,
    restore_settings_hwnd: Option<isize>,
    detected_window_count: usize,
    hovered: Option<CaptureHit>,
    drag_origin: Option<(i32, i32, i32, i32)>,
    btn_close_rect: RectI,
    btn_capture_rect: RectI,
    btn_cancel_rect: RectI,
}

pub struct WorkspaceCaptureManager {
    control_hwnd: HWND,
    session: Option<CaptureSession>,
}

impl WorkspaceCaptureManager {
    pub fn new(control_hwnd: HWND) -> Self {
        Self {
            control_hwnd,
            session: None,
        }
    }

    pub fn set_control_hwnd(&mut self, hwnd: HWND) {
        self.control_hwnd = hwnd;
        CAP_CONTROL_HWND.store(hwnd.0 as isize, Ordering::Release);
    }

    pub fn is_open(&self) -> bool {
        self.session.is_some()
    }

    /// 启动全多屏 4px 正红细边摆拍模式 + 居中捕获控制卡片
    pub fn start(
        &mut self,
        req: StartCaptureRequest,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> bool {
        if let Some(sess) = self.session.as_mut() {
            unsafe {
                let _ = ShowWindow(sess.card_hwnd, SW_SHOW);
                let _ = SetForegroundWindow(sess.card_hwnd);
                let _ = SetFocus(sess.card_hwnd);
            }
            return true;
        }

        let Some(g) = gfx else {
            wk_warn!(trace_id, "workspaceCapture", "SharedGraphics unavailable");
            return false;
        };
        let monitors = enumerate_monitors();
        if monitors.is_empty() {
            return false;
        }

        // 选取光标所在屏或主屏放置居中捕获卡片
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
        let card_w = (368.0 * scale).round() as i32;
        let card_h = (158.0 * scale).round() as i32;
        let work = target_mon.work_rect;
        let card_rect = RectI::from_xywh(
            work.left + (work.width() - card_w) / 2,
            work.top + (work.height() - card_h) / 2,
            card_w,
            card_h,
        );

        let frame_hwnds = create_all_red_frame_strips(&monitors);
        let Some(card_hwnd) = create_capture_card_window(card_rect) else {
            destroy_frame_strips(frame_hwnds);
            return false;
        };

        let dark = cfg.is_dark_theme();
        apply_cross_os_popup_chrome(
            card_hwnd,
            dark,
            card_rect.width(),
            card_rect.height(),
            (10.0 * scale).round() as i32,
        );
        let rt = g
            .create_hwnd_target(
                card_hwnd,
                card_rect.width().max(1) as u32,
                card_rect.height().max(1) as u32,
                96,
            )
            .ok();

        let exclude_hwnd = req
            .restore_settings_hwnd
            .filter(|&raw| raw > 0)
            .map(|raw| HWND(raw as *mut c_void));
        let detected = count_capturable_windows(&cfg.excluded_apps, exclude_hwnd);

        let zh_ui = !req.ui_lang.trim().to_ascii_lowercase().starts_with("en");
        let mut sess = CaptureSession {
            frame_hwnds,
            card_hwnd,
            rt,
            monitors,
            card_rect,
            scale,
            zh_ui,
            workspace_id: req.workspace_id,
            restore_settings_hwnd: req.restore_settings_hwnd,
            detected_window_count: detected,
            hovered: None,
            drag_origin: None,
            btn_close_rect: RectI::default(),
            btn_capture_rect: RectI::default(),
            btn_cancel_rect: RectI::default(),
        };

        sess.render(cfg, g);
        unsafe {
            let _ = ShowWindow(card_hwnd, SW_SHOW);
            let _ = SetForegroundWindow(card_hwnd);
            let _ = SetFocus(card_hwnd);
        }

        wk_info!(
            trace_id,
            "workspaceCapture",
            "started 4px red-frame capture across {} monitor(s) ({} strip windows, detected={} wins)",
            sess.monitors.len(),
            sess.frame_hwnds.len(),
            sess.detected_window_count
        );
        self.session = Some(sess);
        true
    }

    /// 多屏热插拔/分辨率变更时自动重建每屏 4 条正红细边并将卡片拉回在线屏幕
    pub fn on_topology_changed(&mut self, cfg: &WindowKitConfig, gfx: Option<&SharedGraphics>) {
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

        let safe = ensure_window_rect_on_live_monitors(
            "",
            None,
            sess.card_rect.width(),
            sess.card_rect.height(),
            sess.monitors.first(),
            &sess.monitors,
        );
        if safe != sess.card_rect {
            sess.card_rect = safe;
            unsafe {
                let _ = SetWindowPos(
                    sess.card_hwnd,
                    HWND_TOPMOST,
                    safe.left,
                    safe.top,
                    safe.width(),
                    safe.height(),
                    SWP_NOACTIVATE,
                );
            }
        }
        if let Some(g) = gfx {
            sess.render(cfg, g);
        }
    }

    pub fn cancel_and_close(&mut self, trace_id: &str) {
        if let Some(sess) = self.session.take() {
            let restore = sess.restore_settings_hwnd.is_some();
            destroy_capture_session(sess);
            notify_host_capture_result(false, restore, trace_id);
        }
    }

    /// 处理捕获卡交互事件；返回 true 表示捕获会话已结束（触发冷路径内存回收）
    pub fn handle_event(
        &mut self,
        evt: usize,
        lparam: isize,
        cfg: &WindowKitConfig,
        config_dir: &Path,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> bool {
        if self.session.is_none() {
            return false;
        }

        let mut finish_capture: Option<bool> = None; // Some(true)=执行捕获, Some(false)=取消
        let mut need_redraw = false;

        if let Some(sess) = self.session.as_mut() {
            match evt {
                CAP_EVT_CLOSE => finish_capture = Some(false),
                CAP_EVT_PAINT => need_redraw = true,
                CAP_EVT_MOUSEMOVE => {
                    CAP_MOVE_PENDING.store(0, Ordering::Release);
                    let (lx, ly) = unpack_xy(CAP_LATEST_PT.load(Ordering::Acquire));
                    if let Some((sx, sy, ox, oy)) = sess.drag_origin {
                        let mut pt = POINT::default();
                        if unsafe { GetCursorPos(&mut pt) }.is_ok() {
                            let nx = ox + (pt.x - sx);
                            let ny = oy + (pt.y - sy);
                            sess.card_rect =
                                RectI::from_xywh(nx, ny, sess.card_rect.width(), sess.card_rect.height());
                            unsafe {
                                let _ = SetWindowPos(
                                    sess.card_hwnd,
                                    HWND::default(),
                                    nx,
                                    ny,
                                    0,
                                    0,
                                    SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE,
                                );
                            }
                        }
                    } else {
                        let hit = sess.hit_test(lx, ly);
                        let cur = match hit {
                            Some(CaptureHit::DragHeader) => CURSOR_SIZEALL,
                            Some(_) => CURSOR_HAND,
                            None => CURSOR_ARROW,
                        };
                        let prev = CAP_CURSOR_KIND.swap(cur, Ordering::AcqRel);
                        if prev != cur {
                            unsafe {
                                let _ = SetCursor(capture_cursor_handle(cur));
                            }
                        }
                        if hit != sess.hovered {
                            sess.hovered = hit;
                            need_redraw = true;
                        }
                    }
                }
                CAP_EVT_LBUTTONDOWN => {
                    let (lx, ly) = unpack_lparam_xy(lparam);
                    match sess.hit_test(lx, ly) {
                        Some(CaptureHit::DragHeader) => {
                            let mut pt = POINT::default();
                            let mut wr = RECT::default();
                            unsafe {
                                let _ = GetCursorPos(&mut pt);
                                let _ = GetWindowRect(sess.card_hwnd, &mut wr);
                                let _ = SetCapture(sess.card_hwnd);
                            }
                            sess.drag_origin = Some((pt.x, pt.y, wr.left, wr.top));
                        }
                        Some(CaptureHit::BtnClose) | Some(CaptureHit::BtnCancel) => {
                            finish_capture = Some(false);
                        }
                        Some(CaptureHit::BtnCapture) => {
                            finish_capture = Some(true);
                        }
                        None => {}
                    }
                }
                CAP_EVT_LBUTTONUP => {
                    if sess.drag_origin.take().is_some() {
                        unsafe {
                            let _ = ReleaseCapture();
                        }
                    }
                }
                CAP_EVT_KEYDOWN => {
                    let vk = lparam as u16;
                    if vk == VK_ESCAPE.0 {
                        finish_capture = Some(false);
                    } else if vk == VK_RETURN.0 {
                        finish_capture = Some(true);
                    }
                }
                _ => {}
            }

            if finish_capture.is_none() && need_redraw {
                if let Some(g) = gfx {
                    let exclude_hwnd = sess
                        .restore_settings_hwnd
                        .filter(|&raw| raw > 0)
                        .map(|raw| HWND(raw as *mut c_void));
                    sess.detected_window_count =
                        count_capturable_windows(&cfg.excluded_apps, exclude_hwnd);
                    sess.render(cfg, g);
                }
            }
        }

        if let Some(do_capture) = finish_capture {
            if let Some(sess) = self.session.take() {
                let workspace_id = sess.workspace_id.clone();
                let restore_hwnd = sess.restore_settings_hwnd;
                // 先彻底销毁全部正红细边与捕获卡，再采样真实桌面，确保自身 0 污染！
                destroy_capture_session(sess);

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
            }
            return true;
        }

        false
    }
}

impl Drop for WorkspaceCaptureManager {
    fn drop(&mut self) {
        if let Some(sess) = self.session.take() {
            destroy_capture_session(sess);
        }
    }
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

fn destroy_capture_session(sess: CaptureSession) {
    let card_hwnd = sess.card_hwnd;
    let strips = sess.frame_hwnds;
    drop(sess.rt);
    destroy_frame_strips(strips);
    unsafe {
        let _ = ReleaseCapture();
        if IsWindow(card_hwnd).as_bool() {
            let _ = ShowWindow(card_hwnd, SW_HIDE);
            let _ = DestroyWindow(card_hwnd);
        }
    }
    CAP_CURSOR_KIND.store(CURSOR_ARROW, Ordering::Release);
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
                wk_debug!(
                    trace_id,
                    "workspaceCapture",
                    "notified host capture result captured={} restoreSettings={}",
                    captured,
                    restore_settings
                );
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. 捕获卡片 Direct2D 渲染（高分屏锐利、零 Emoji、对标 PowerToys 图 2）
// ─────────────────────────────────────────────────────────────────────────────

impl CaptureSession {
    fn hit_test(&self, lx: i32, ly: i32) -> Option<CaptureHit> {
        if self.btn_close_rect.contains_point(lx, ly) {
            return Some(CaptureHit::BtnClose);
        }
        if self.btn_capture_rect.contains_point(lx, ly) {
            return Some(CaptureHit::BtnCapture);
        }
        if self.btn_cancel_rect.contains_point(lx, ly) {
            return Some(CaptureHit::BtnCancel);
        }
        if ly <= (38.0 * self.scale).round() as i32 {
            return Some(CaptureHit::DragHeader);
        }
        None
    }

    fn render(&mut self, cfg: &WindowKitConfig, gfx: &SharedGraphics) {
        let Some(hwnd_rt) = self.rt.clone() else {
            return;
        };
        let Ok(rt): Result<ID2D1RenderTarget, _> = hwnd_rt.cast() else {
            return;
        };

        let mut rc = RECT::default();
        unsafe {
            let _ = GetClientRect(self.card_hwnd, &mut rc);
        }
        let w = (rc.right - rc.left).max(1) as f32;
        let h = (rc.bottom - rc.top).max(1) as f32;
        let s = self.scale;
        let dark = cfg.is_dark_theme();
        let accent = parse_hex_color(&cfg.accent_color, [0.23, 0.51, 0.96]);
        let zh = self.zh_ui;

        let title_fmt = gfx.ui_text_format(13.0 * s, true, false).ok();
        let body_fmt = gfx.ui_text_format(13.2 * s, false, true).ok();
        let sub_fmt = gfx.mono_text_format(11.8 * s, false, true).ok();
        let btn_fmt = gfx.ui_text_format(13.0 * s, true, true).ok();

        unsafe {
            let sz = D2D_SIZE_U {
                width: w as u32,
                height: h as u32,
            };
            let _ = hwnd_rt.Resize(&sz);

            rt.BeginDraw();
            let bg = if dark { rgba(0.115, 0.13, 0.165, 1.0) } else { rgba(0.99, 0.995, 1.0, 1.0) };
            let surface = if dark { rgba(0.17, 0.195, 0.24, 1.0) } else { rgba(0.945, 0.955, 0.97, 1.0) };
            let border = if dark { rgba(1.0, 1.0, 1.0, 0.14) } else { rgba(0.10, 0.14, 0.22, 0.14) };
            let text_main = if dark { rgba(0.95, 0.97, 1.0, 1.0) } else { rgba(0.10, 0.13, 0.18, 1.0) };
            let text_muted = if dark { rgba(0.70, 0.76, 0.84, 1.0) } else { rgba(0.36, 0.42, 0.50, 1.0) };
            let red_dot = rgba(0.863, 0.149, 0.149, 1.0); // #DC2626 正红，与外框统一
            let accent_col = rgb_a(accent, 1.0);
            let white = rgba(1.0, 1.0, 1.0, 1.0);

            rt.Clear(Some(&bg));

            let (
                Ok(b_surface),
                Ok(b_border),
                Ok(b_text),
                Ok(b_muted),
                Ok(b_red),
                Ok(b_accent),
                Ok(b_white),
            ) = (
                rt.CreateSolidColorBrush(&surface, None),
                rt.CreateSolidColorBrush(&border, None),
                rt.CreateSolidColorBrush(&text_main, None),
                rt.CreateSolidColorBrush(&text_muted, None),
                rt.CreateSolidColorBrush(&red_dot, None),
                rt.CreateSolidColorBrush(&accent_col, None),
                rt.CreateSolidColorBrush(&white, None),
            ) else {
                let _ = rt.EndDraw(None, None);
                return;
            };

            // 顶部标题栏 + 正红录制态圆点
            let dot_r = 4.5 * s;
            let dot_cx = 18.0 * s;
            let dot_cy = 20.0 * s;
            rt.FillRoundedRectangle(
                &rounded_rect(rect_f(dot_cx - dot_r, dot_cy - dot_r, dot_cx + dot_r, dot_cy + dot_r), dot_r),
                &b_red,
            );
            if let Some(fmt) = &title_fmt {
                let title = to_wide(if zh { "捕获工作区" } else { "Capture Workspace" });
                rt.DrawText(
                    &title[..title.len().saturating_sub(1)],
                    fmt,
                    &rect_f(30.0 * s, 11.0 * s, w - 42.0 * s, 32.0 * s),
                    &b_text,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }

            // 右上角关闭按钮
            let cr = rect_f(w - 36.0 * s, 4.0 * s, w - 4.0 * s, 36.0 * s);
            self.btn_close_rect = RectI::new(cr.left as i32, cr.top as i32, cr.right as i32, cr.bottom as i32);
            if self.hovered == Some(CaptureHit::BtnClose) {
                rt.FillRoundedRectangle(&rounded_rect(cr, 6.0 * s), &b_red);
            }
            let cx = (cr.left + cr.right) * 0.5;
            let cy = (cr.top + cr.bottom) * 0.5;
            let cs = 4.5 * s;
            let xb = if self.hovered == Some(CaptureHit::BtnClose) { &b_white } else { &b_muted };
            rt.DrawLine(D2D_POINT_2F { x: cx - cs, y: cy - cs }, D2D_POINT_2F { x: cx + cs, y: cy + cs }, xb, 1.4 * s, None);
            rt.DrawLine(D2D_POINT_2F { x: cx + cs, y: cy - cs }, D2D_POINT_2F { x: cx - cs, y: cy + cs }, xb, 1.4 * s, None);

            // 居中操作提示 + 已识别应用计数
            if let Some(fmt) = &body_fmt {
                let msg = to_wide(if zh {
                    "设置布局，完成后单击「捕获」"
                } else {
                    "Arrange your windows, then click Capture"
                });
                rt.DrawText(
                    &msg[..msg.len().saturating_sub(1)],
                    fmt,
                    &rect_f(18.0 * s, 46.0 * s, w - 18.0 * s, 72.0 * s),
                    &b_text,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }
            if let Some(fmt) = &sub_fmt {
                let sub = if zh {
                    format!("当前虚拟桌面已就绪 {} 个应用窗口（含最小化）", self.detected_window_count)
                } else {
                    format!("{} window(s) ready on current desktop", self.detected_window_count)
                };
                let sw = to_wide(&sub);
                rt.DrawText(
                    &sw[..sw.len().saturating_sub(1)],
                    fmt,
                    &rect_f(18.0 * s, 74.0 * s, w - 18.0 * s, 96.0 * s),
                    &b_muted,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }

            // 底部双按钮：捕获（主操作）+ 取消
            let pad = 18.0 * s;
            let gap = 10.0 * s;
            let btn_h = (36.0 * s).round();
            let btn_w = ((w - pad * 2.0 - gap) * 0.5).floor();
            let by = h - pad - btn_h;
            let cap_r = rect_f(pad, by, pad + btn_w, by + btn_h);
            let can_r = rect_f(pad + btn_w + gap, by, w - pad, by + btn_h);
            self.btn_capture_rect = RectI::new(cap_r.left as i32, cap_r.top as i32, cap_r.right as i32, cap_r.bottom as i32);
            self.btn_cancel_rect = RectI::new(can_r.left as i32, can_r.top as i32, can_r.right as i32, can_r.bottom as i32);

            rt.FillRoundedRectangle(&rounded_rect(cap_r, 6.0 * s), &b_accent);
            if self.hovered == Some(CaptureHit::BtnCapture) {
                rt.DrawRoundedRectangle(&rounded_rect(cap_r, 6.0 * s), &b_white, 1.5 * s, None);
            }
            rt.FillRoundedRectangle(&rounded_rect(can_r, 6.0 * s), &b_surface);
            rt.DrawRoundedRectangle(&rounded_rect(can_r, 6.0 * s), &b_border, 1.0 * s, None);

            if let Some(fmt) = &btn_fmt {
                let l1 = to_wide(if zh { "捕获" } else { "Capture" });
                let l2 = to_wide(if zh { "取消" } else { "Cancel" });
                rt.DrawText(&l1[..l1.len().saturating_sub(1)], fmt, &cap_r, &b_white, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
                rt.DrawText(&l2[..l2.len().saturating_sub(1)], fmt, &can_r, &b_text, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }

            // 外框极细描边
            rt.DrawRectangle(&rect_f(0.5, 0.5, w - 0.5, h - 0.5), &b_border, 1.0, None);
            let _ = rt.EndDraw(None, None);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. 每屏 4 条 4px 纯色正红穿透细边窗创建与 Win32 过程（~190 KB / 屏）
// ─────────────────────────────────────────────────────────────────────────────

fn ensure_capture_classes() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);

        let f_cls = to_wide(WORKSPACE_CAPTURE_FRAME_CLASS);
        let f_wc = WNDCLASSEXW {
            cbSize: size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(capture_frame_strip_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(f_cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&f_wc);

        let c_cls = to_wide(WORKSPACE_CAPTURE_CARD_CLASS);
        let c_wc = WNDCLASSEXW {
            cbSize: size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(capture_card_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(c_cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&c_wc);
    });
}

/// 为所有在线物理显示器生成 4 条正红细边（精确贴合屏幕工作区外框，不遮挡任务栏，中间 100% 穿透）
fn create_all_red_frame_strips(monitors: &[MonitorInfo]) -> Vec<HWND> {
    ensure_capture_classes();
    let mut out = Vec::with_capacity(monitors.len() * 4);
    let hinst = unsafe { HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0) };
    let cls = to_wide(WORKSPACE_CAPTURE_FRAME_CLASS);
    let ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;

    for mon in monitors {
        let scale = (mon.dpi as f32 / 96.0).max(1.0);
        let t = (CAPTURE_BORDER_THICKNESS_DIP * scale).round().max(3.0) as i32;
        let r = mon.work_rect;
        let w = r.width().max(t * 2);
        let h = r.height().max(t * 2);
        let strips = [
            // 上边
            (r.left, r.top, w, t),
            // 下边
            (r.left, r.bottom - t, w, t),
            // 左边（扣除上下重叠角）
            (r.left, r.top + t, t, (h - t * 2).max(1)),
            // 右边
            (r.right - t, r.top + t, t, (h - t * 2).max(1)),
        ];
        for (x, y, sw, sh) in strips {
            if let Ok(hwnd) = unsafe {
                CreateWindowExW(
                    ex,
                    PCWSTR(cls.as_ptr()),
                    PCWSTR::null(),
                    WS_POPUP,
                    x,
                    y,
                    sw,
                    sh,
                    None,
                    None,
                    hinst,
                    None,
                )
            } {
                unsafe {
                    let _ = SetLayeredWindowAttributes(hwnd, COLORREF(0), 240, LWA_ALPHA);
                    let _ = SetWindowPos(hwnd, HWND_TOPMOST, x, y, sw, sh, SWP_NOACTIVATE);
                    let _ = ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                }
                out.push(hwnd);
            }
        }
    }
    out
}

fn create_capture_card_window(rect: RectI) -> Option<HWND> {
    ensure_capture_classes();
    unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(WORKSPACE_CAPTURE_CARD_CLASS);
        let title = to_wide("Tools3000 Workspace Capture");
        CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            PCWSTR(cls.as_ptr()),
            PCWSTR(title.as_ptr()),
            WS_POPUP,
            rect.left,
            rect.top,
            rect.width().max(260),
            rect.height().max(120),
            None,
            None,
            hinst,
            None,
        )
        .ok()
    }
}

unsafe extern "system" fn capture_frame_strip_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if msg == WM_PAINT {
        let mut ps = PAINTSTRUCT::default();
        let hdc = BeginPaint(hwnd, &mut ps);
        let brush = CreateSolidBrush(CAPTURE_FRAME_COLORREF);
        let _ = FillRect(hdc, &ps.rcPaint, brush);
        let _ = DeleteObject(brush);
        let _ = EndPaint(hwnd, &ps);
        return LRESULT(0);
    }
    DefWindowProcW(hwnd, msg, wparam, lparam)
}

unsafe extern "system" fn capture_card_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if msg == WM_SETCURSOR {
        let ht = (lparam.0 as u32) & 0xFFFF;
        if ht == HTCLIENT {
            let k = CAP_CURSOR_KIND.load(Ordering::Acquire);
            let _ = SetCursor(capture_cursor_handle(k));
            return LRESULT(1);
        }
    }
    let ctrl_raw = CAP_CONTROL_HWND.load(Ordering::Acquire);
    if ctrl_raw != 0 {
        let ctrl = HWND(ctrl_raw as *mut c_void);
        match msg {
            WM_MOUSEMOVE => {
                let (x, y) = unpack_lparam_xy(lparam.0);
                CAP_LATEST_PT.store(pack_xy(x, y), Ordering::Release);
                if CAP_MOVE_PENDING.swap(1, Ordering::AcqRel) == 0 {
                    let _ = PostMessageW(ctrl, WM_WK_INTERNAL_CAPTURE_EVENT, WPARAM(CAP_EVT_MOUSEMOVE), LPARAM(0));
                }
                return LRESULT(0);
            }
            WM_LBUTTONDOWN => {
                let _ = PostMessageW(ctrl, WM_WK_INTERNAL_CAPTURE_EVENT, WPARAM(CAP_EVT_LBUTTONDOWN), lparam);
                return LRESULT(0);
            }
            WM_LBUTTONUP => {
                let _ = PostMessageW(ctrl, WM_WK_INTERNAL_CAPTURE_EVENT, WPARAM(CAP_EVT_LBUTTONUP), lparam);
                return LRESULT(0);
            }
            WM_KEYDOWN => {
                let _ = PostMessageW(ctrl, WM_WK_INTERNAL_CAPTURE_EVENT, WPARAM(CAP_EVT_KEYDOWN), LPARAM(wparam.0 as isize));
                return LRESULT(0);
            }
            WM_PAINT => {
                let _ = PostMessageW(ctrl, WM_WK_INTERNAL_CAPTURE_EVENT, WPARAM(CAP_EVT_PAINT), LPARAM(0));
            }
            WM_CLOSE => {
                let _ = PostMessageW(ctrl, WM_WK_INTERNAL_CAPTURE_EVENT, WPARAM(CAP_EVT_CLOSE), LPARAM(0));
                return LRESULT(0);
            }
            _ => {}
        }
    }
    DefWindowProcW(hwnd, msg, wparam, lparam)
}
