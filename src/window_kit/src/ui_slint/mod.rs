// ─────────────────────────────────────────────────────────────────────────────
// ui_slint/mod.rs — Tools3000 WindowKit 原生 Slint UI 协调运行时
//
// 架构准则:
//   1. 惰性按需启动：独立 Slint UI 线程（winit with_any_thread(true) + FemtoVG GPU / Software 自动降级）
//      仅在用户首次呼出 QuickSwitch / LayoutHub / WorkspaceCapture / WorkspaceProgress 时启动，
//      平时 0 线程 / 0 GPU 表面 / 0 MB 额外开销；
//   2. 零阻塞双向通信：Win32 钩子主线程通过 slint::invoke_from_event_loop 亚毫秒级推送状态，
//      Slint UI 事件写入无锁队列并 PostMessageW(WM_WK_INTERNAL_SLINT_EVENT) 通知主线程；
//   3. 原生 Win32 深度融合：通过 raw-window-handle-06 取得真实 HWND，施加 Win11 硬件圆角/深浅色阴影、
//      WS_EX_TOOLWINDOW 隐匿任务栏、以及 QuickSwitch 右栏 60FPS DwmRegisterThumbnail 零拷贝实时预览！
// ─────────────────────────────────────────────────────────────────────────────

#![allow(clippy::too_many_arguments)]

mod generated {
    slint::include_modules!();
}

use crate::core::config::parse_hex_color;
use crate::core::constants::WM_WK_INTERNAL_SLINT_EVENT;
use crate::core::geometry::RectI;
use crate::core::topology::apply_cross_os_popup_chrome;
use crate::{wk_info, wk_warn};
use generated::{
    LayoutCardItem, LayoutHubWindow, MonitorCardItem, QuickSwitchRowItem, QuickSwitchWindow,
    ThemeTokens, VirtualDesktopPill, WorkspaceCaptureWindow, WorkspaceProgressWindow,
    ZoneThumbRect,
};
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use slint::winit_030::winit::dpi::{PhysicalPosition, PhysicalSize};
use slint::winit_030::winit::window::ResizeDirection;
use slint::winit_030::WinitWindowAccessor;
use slint::{Color, ComponentHandle, Image, ModelRc, Rgba8Pixel, SharedPixelBuffer, SharedString, VecModel};
use std::cell::RefCell;
use std::collections::VecDeque;
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicIsize, Ordering};
use std::sync::{mpsc, Mutex, OnceLock};
use std::thread::{self, JoinHandle};
use windows::Win32::Foundation::{HWND, LPARAM, RECT, SIZE, WPARAM};
use windows::Win32::Graphics::Dwm::{
    DwmQueryThumbnailSourceSize, DwmRegisterThumbnail, DwmUnregisterThumbnail,
    DwmUpdateThumbnailProperties, DWM_THUMBNAIL_PROPERTIES, DWM_TNP_OPACITY,
    DWM_TNP_RECTDESTINATION, DWM_TNP_SOURCECLIENTAREAONLY, DWM_TNP_VISIBLE,
};
use windows::Win32::System::Threading::{AttachThreadInput, GetCurrentThreadId};
use windows::Win32::UI::WindowsAndMessaging::{
    AllowSetForegroundWindow, BringWindowToTop, GetForegroundWindow, GetWindowLongPtrW,
    GetWindowThreadProcessId, IsWindow, PostMessageW, SetForegroundWindow, SetWindowLongPtrW,
    SetWindowPos, ASFW_ANY, GWL_EXSTYLE, HWND_TOPMOST, SWP_NOACTIVATE, SWP_NOMOVE, SWP_NOSIZE,
    WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TOPMOST,
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. 跨线程公开数据契约（Win32 引擎 -> Slint UI & Slint UI -> Win32 引擎）
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct SlintThemeSpec {
    pub dark: bool,
    pub accent_hex: String,
}

impl SlintThemeSpec {
    fn to_slint_accent(&self) -> Color {
        let rgb = parse_hex_color(&self.accent_hex, [0.231, 0.510, 0.965]);
        Color::from_rgb_u8(
            (rgb[0].clamp(0.0, 1.0) * 255.0).round() as u8,
            (rgb[1].clamp(0.0, 1.0) * 255.0).round() as u8,
            (rgb[2].clamp(0.0, 1.0) * 255.0).round() as u8,
        )
    }
}

#[derive(Debug, Clone)]
pub struct QuickSwitchRowData {
    pub index: i32,
    pub title: String,
    pub app_name: String,
    pub icon_rgba: Option<(u32, u32, Vec<u8>)>,
    pub number_label: String,
    pub is_minimized: bool,
    pub is_pinned: bool,
    pub is_tray: bool,
    pub state_badge: String,
}

#[derive(Debug, Clone)]
pub struct QuickSwitchViewState {
    pub theme: SlintThemeSpec,
    pub window_rect: RectI,
    #[allow(dead_code)]
    pub dpi: u32,
    pub items: Vec<QuickSwitchRowData>,
    pub selected_index: i32,
    pub filter_text: String,
    pub search_visible: bool,
    pub search_locked: bool,
    pub show_numbers: bool,
    pub show_app_names: bool,
    pub compact_list: bool,
    pub same_app_mode: bool,
    pub scope_title: String,
    pub list_width_ratio: f32,
    pub zh_ui: bool,
    pub show_live_preview: bool,
    pub preview_in_desktop_position: bool,
    pub preview_target_hwnd: isize,
    pub preview_target_minimized: bool,
    pub preview_target_norm_rect: Option<(f32, f32, f32, f32)>,
    pub reposition_window: bool,
}

#[derive(Debug, Clone)]
pub struct LayoutThumbData {
    pub rx: f32,
    pub ry: f32,
    pub rw: f32,
    pub rh: f32,
    pub layer: i32,
}

#[derive(Debug, Clone)]
pub struct LayoutMonitorData {
    pub index: i32,
    pub badge_num: String,
    pub resolution: String,
    pub scale_text: String,
    pub friendly_name: String,
    pub selected: bool,
}

#[derive(Debug, Clone)]
pub struct LayoutDesktopData {
    pub id: String,
    pub label: String,
    pub active: bool,
}

#[derive(Debug, Clone)]
pub struct LayoutCardData {
    pub id: String,
    pub name: String,
    pub is_builtin: bool,
    pub is_no_layout: bool,
    pub is_layered: bool,
    pub is_active: bool,
    pub hotkey_badge: String,
    pub thumbs: Vec<LayoutThumbData>,
}

#[derive(Debug, Clone)]
pub struct LayoutHubViewState {
    pub theme: SlintThemeSpec,
    pub window_rect: RectI,
    pub reposition_window: bool,
    pub zh_ui: bool,
    pub monitors: Vec<LayoutMonitorData>,
    pub desktops: Vec<LayoutDesktopData>,
    pub template_layouts: Vec<LayoutCardData>,
    pub custom_layouts: Vec<LayoutCardData>,
    pub modal_mode: i32, // 0=none, 1=edit-template, 2=edit-custom, 3=create-new
    pub modal_title: String,
    pub draft_layout_id: String,
    pub draft_name: String,
    pub draft_zone_count: i32,
    pub draft_show_spacing: bool,
    pub draft_spacing: i32,
    pub draft_highlight_distance: i32,
    pub draft_hotkey_index: i32,
    pub draft_default_horizontal: bool,
    pub draft_default_vertical: bool,
    pub draft_create_canvas: bool,
    pub draft_is_layered: bool,
    pub draft_thumbs: Vec<LayoutThumbData>,
}

#[derive(Debug, Clone)]
pub struct WorkspaceCaptureViewState {
    pub theme: SlintThemeSpec,
    pub window_rect: RectI,
    pub zh_ui: bool,
    pub detected_windows: usize,
    pub busy: bool,
}

#[derive(Debug, Clone)]
pub struct WorkspaceProgressViewState {
    pub theme: SlintThemeSpec,
    pub window_rect: RectI,
    pub workspace_name: String,
    pub status_text: String,
    pub progress: f32,
}

#[derive(Debug, Clone)]
pub enum SlintUiEvent {
    // QuickSwitch 事件
    QsSelectRow(i32),
    QsActivateRow(i32),
    QsCloseRow(i32),
    QsFilterChanged(String),
    QsClearFilter,
    QsSearchEngaged,
    QsAction(String),
    QsKeyCommand {
        key: String,
        shift: bool,
        #[allow(dead_code)]
        ctrl: bool,
    },
    QsDismissed,
    QsBoundsChanged {
        rect: RectI,
        list_width_ratio: f32,
    },

    // LayoutHub 事件
    HubSelectMonitor(i32),
    HubSelectDesktop(String),
    HubSelectLayout(String),
    HubOpenTemplateEditor(String),
    HubCustomMenuAction {
        action: String,
        layout_id: String,
    },
    HubOpenCreateModal,
    HubDraftPreviewChanged {
        zone_count: i32,
        show_spacing: bool,
        spacing: i32,
        highlight_distance: i32,
    },
    HubModalSave {
        name: String,
        zone_count: i32,
        show_spacing: bool,
        spacing: i32,
        highlight_distance: i32,
        hotkey_index: i32,
        default_horizontal: bool,
        default_vertical: bool,
    },
    HubModalCancel,
    HubModalDuplicate,
    HubModalDelete,
    HubModalOpenFullscreenZones,
    HubCreateConfirm {
        name: String,
        is_canvas: bool,
    },
    HubClosed,

    // WorkspaceCapture 事件
    CaptureConfirm,
    CaptureCancel,
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. 全局事件通道与按需惰性 Slint UI 线程宿主
// ─────────────────────────────────────────────────────────────────────────────

static CONTROL_HWND: AtomicIsize = AtomicIsize::new(0);
static SLINT_STARTED: AtomicBool = AtomicBool::new(false);
static SLINT_SHUTTING_DOWN: AtomicBool = AtomicBool::new(false);

fn event_queue() -> &'static Mutex<VecDeque<SlintUiEvent>> {
    static Q: OnceLock<Mutex<VecDeque<SlintUiEvent>>> = OnceLock::new();
    Q.get_or_init(|| Mutex::new(VecDeque::new()))
}

fn runtime_thread() -> &'static Mutex<Option<JoinHandle<()>>> {
    static H: OnceLock<Mutex<Option<JoinHandle<()>>>> = OnceLock::new();
    H.get_or_init(|| Mutex::new(None))
}

pub fn set_control_hwnd(hwnd: HWND) {
    CONTROL_HWND.store(hwnd.0 as isize, Ordering::Release);
}

fn push_ui_event(evt: SlintUiEvent) {
    if let Ok(mut q) = event_queue().lock() {
        q.push_back(evt);
    }
    let raw = CONTROL_HWND.load(Ordering::Acquire);
    if raw != 0 {
        unsafe {
            let _ = PostMessageW(
                HWND(raw as *mut c_void),
                WM_WK_INTERNAL_SLINT_EVENT,
                WPARAM(0),
                LPARAM(0),
            );
        }
    }
}

pub fn drain_slint_events() -> Vec<SlintUiEvent> {
    if let Ok(mut q) = event_queue().lock() {
        q.drain(..).collect()
    } else {
        Vec::new()
    }
}

/// 惰性拉起专用 Slint UI 线程（仅在用户首次实际打开窗口时启动，冷态零开销）
fn ensure_slint_thread() -> bool {
    if SLINT_SHUTTING_DOWN.load(Ordering::Acquire) {
        return false;
    }
    if SLINT_STARTED.load(Ordering::Acquire) {
        return true;
    }

    let mut guard = match runtime_thread().lock() {
        Ok(g) => g,
        Err(_) => return false,
    };
    if SLINT_STARTED.load(Ordering::Acquire) {
        return true;
    }

    let (tx, rx) = mpsc::sync_channel::<bool>(1);
    let handle = match thread::Builder::new()
        .name("Tools3000-SlintUI".to_string())
        .spawn(move || {
            use slint::winit_030::winit::platform::windows::EventLoopBuilderExtWindows;
            let mut el_builder =
                slint::winit_030::winit::event_loop::EventLoop::<slint::winit_030::SlintEvent>::with_user_event();
            el_builder.with_any_thread(true);
            let selector = slint::BackendSelector::new()
                .backend_name("winit".to_string())
                .renderer_name("software".to_string())
                .with_winit_event_loop_builder(el_builder);
            if let Err(e) = selector.select() {
                wk_warn!("slint", "ui_slint", "BackendSelector::select failed: {}", e);
                let _ = tx.send(false);
                return;
            }
            wk_info!("slint", "ui_slint", "Slint winit backend selected on dedicated UI thread");
            // 先排入一个启动完成信号，确保事件循环真正进入运行态后再唤醒主线程
            let tx_ready = tx.clone();
            let _ = slint::invoke_from_event_loop(move || {
                wk_info!("slint", "ui_slint", "Slint event loop is actively running");
                let _ = tx_ready.send(true);
            });
            if let Err(e) = slint::run_event_loop_until_quit() {
                wk_warn!("slint", "ui_slint", "run_event_loop_until_quit returned err: {}", e);
                let _ = tx.send(false);
            }
            // 线程退出前确保释放所有线程本地 Slint 窗口与 DWM 缩略图句柄
            cleanup_all_thread_local_windows();
        }) {
        Ok(h) => h,
        Err(_) => return false,
    };

    match rx.recv_timeout(std::time::Duration::from_secs(5)) {
        Ok(true) => {
            *guard = Some(handle);
            SLINT_STARTED.store(true, Ordering::Release);
            true
        }
        _ => {
            let _ = handle.join();
            false
        }
    }
}

/// 进程优雅退出时干净结束 Slint 事件循环并回收 UI 线程
pub fn shutdown_slint_runtime() {
    SLINT_SHUTTING_DOWN.store(true, Ordering::Release);
    if !SLINT_STARTED.load(Ordering::Acquire) {
        return;
    }
    let _ = slint::invoke_from_event_loop(|| {
        cleanup_all_thread_local_windows();
        let _ = slint::quit_event_loop();
    });
    if let Ok(mut guard) = runtime_thread().lock() {
        if let Some(h) = guard.take() {
            let _ = h.join();
        }
    }
    SLINT_STARTED.store(false, Ordering::Release);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. 线程本地 Slint 窗口池 + 原生 Win32 HWND 与 DWM 缩略图封装
//    关键并发铁律：窗口池 (SLINT_WIN_POOL) 与缩略图状态 (QS_THUMB_STATE) 物理分离，
//    且仅在短临界区克隆 ComponentHandle (底层廉价 Rc)，绝不在持有 RefCell 借用期间
//    调用任何 Slint setter / show / invoke，彻底杜绝同步回调重入 RefCell BorrowError！
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Default)]
struct SlintWinPool {
    quick_switch: Option<QuickSwitchWindow>,
    layout_hub: Option<LayoutHubWindow>,
    workspace_capture: Option<WorkspaceCaptureWindow>,
    workspace_progress: Option<WorkspaceProgressWindow>,
}

struct QsThumbState {
    thumb_handle: isize,
    thumb_target: isize,
    preview_slot_logical: (f32, f32, f32, f32),
    show_live_preview: bool,
    preview_in_desktop_pos: bool,
    preview_target_minimized: bool,
    preview_norm_rect: Option<(f32, f32, f32, f32)>,
    list_ratio: f32,
}

impl Default for QsThumbState {
    fn default() -> Self {
        Self {
            thumb_handle: 0,
            thumb_target: 0,
            preview_slot_logical: (320.0, 0.0, 640.0, 560.0),
            show_live_preview: true,
            preview_in_desktop_pos: false,
            preview_target_minimized: false,
            preview_norm_rect: None,
            list_ratio: 0.30,
        }
    }
}

impl QsThumbState {
    fn unregister_thumb(&mut self) {
        if self.thumb_handle != 0 {
            unsafe {
                let _ = DwmUnregisterThumbnail(self.thumb_handle);
            }
            self.thumb_handle = 0;
            self.thumb_target = 0;
        }
    }

    fn update_thumbnail(&mut self, qs_win: &QuickSwitchWindow) -> bool {
        let target_raw = self.thumb_target;
        if !self.show_live_preview
            || self.preview_target_minimized
            || target_raw == 0
            || !unsafe { IsWindow(HWND(target_raw as *mut c_void)).as_bool() }
        {
            self.unregister_thumb();
            return false;
        }

        let Some(host_hwnd) = extract_win32_hwnd(qs_win.window()) else {
            return false;
        };

        if self.thumb_handle == 0 {
            let reg = unsafe { DwmRegisterThumbnail(host_hwnd, HWND(target_raw as *mut c_void)) };
            match reg {
                Ok(h) if h != 0 => {
                    self.thumb_handle = h;
                }
                _ => {
                    return false;
                }
            }
        }

        let scale = qs_win.window().scale_factor().max(1.0);
        let (sx, sy, sw, sh) = self.preview_slot_logical;
        let pad_x = 26.0 * scale;
        let pad_top = 24.0 * scale;
        let pad_bottom = 52.0 * scale;
        let slot_left = sx * scale + pad_x;
        let slot_top = sy * scale + pad_top;
        let slot_w = (sw * scale - pad_x * 2.0).max(40.0);
        let slot_h = (sh * scale - pad_top - pad_bottom).max(40.0);

        let dest_rect = if self.preview_in_desktop_pos {
            if let Some((nx, ny, nw, nh)) = self.preview_norm_rect {
                let l = (slot_left + nx.clamp(0.0, 0.95) * slot_w).round() as i32;
                let t = (slot_top + ny.clamp(0.0, 0.95) * slot_h).round() as i32;
                let r = (l as f32 + (nw.clamp(0.08, 1.0) * slot_w)).round() as i32;
                let b = (t as f32 + (nh.clamp(0.08, 1.0) * slot_h)).round() as i32;
                RECT {
                    left: l,
                    top: t,
                    right: r.max(l + 32),
                    bottom: b.max(t + 24),
                }
            } else {
                fit_thumbnail_centered(self.thumb_handle, slot_left, slot_top, slot_w, slot_h)
            }
        } else {
            fit_thumbnail_centered(self.thumb_handle, slot_left, slot_top, slot_w, slot_h)
        };

        let props = DWM_THUMBNAIL_PROPERTIES {
            dwFlags: DWM_TNP_RECTDESTINATION
                | DWM_TNP_VISIBLE
                | DWM_TNP_OPACITY
                | DWM_TNP_SOURCECLIENTAREAONLY,
            rcDestination: dest_rect,
            opacity: 255,
            fVisible: true.into(),
            fSourceClientAreaOnly: false.into(),
            ..Default::default()
        };

        unsafe { DwmUpdateThumbnailProperties(self.thumb_handle, &props).is_ok() }
    }
}

thread_local! {
    static SLINT_WIN_POOL: RefCell<SlintWinPool> = RefCell::new(SlintWinPool::default());
    static QS_THUMB_STATE: RefCell<QsThumbState> = RefCell::new(QsThumbState::default());
}

fn cleanup_all_thread_local_windows() {
    QS_THUMB_STATE.with(|c| {
        if let Ok(mut ts) = c.try_borrow_mut() {
            ts.unregister_thumb();
        }
    });
    let pool = SLINT_WIN_POOL.with(|c| std::mem::take(&mut *c.borrow_mut()));
    if let Some(w) = pool.quick_switch {
        let _ = w.hide();
    }
    if let Some(w) = pool.layout_hub {
        let _ = w.hide();
    }
    if let Some(w) = pool.workspace_capture {
        let _ = w.hide();
    }
    if let Some(w) = pool.workspace_progress {
        let _ = w.hide();
    }
}

fn refresh_qs_thumbnail_unlocked(qs_win: &QuickSwitchWindow) {
    let ok = QS_THUMB_STATE.with(|c| {
        if let Ok(mut ts) = c.try_borrow_mut() {
            ts.update_thumbnail(qs_win)
        } else {
            false
        }
    });
    qs_win.set_preview_has_live_thumb(ok);
}

fn fit_thumbnail_centered(
    thumb: isize,
    slot_left: f32,
    slot_top: f32,
    slot_w: f32,
    slot_h: f32,
) -> RECT {
    let src_sz = unsafe { DwmQueryThumbnailSourceSize(thumb) }.unwrap_or(SIZE {
        cx: 1280,
        cy: 800,
    });
    let sw = (src_sz.cx.max(1)) as f32;
    let sh = (src_sz.cy.max(1)) as f32;
    let fit = (slot_w / sw).min(slot_h / sh).min(1.25);
    let dw = (sw * fit).max(48.0);
    let dh = (sh * fit).max(36.0);
    let l = (slot_left + (slot_w - dw) * 0.5).round() as i32;
    let t = (slot_top + (slot_h - dh) * 0.5).round() as i32;
    RECT {
        left: l,
        top: t,
        right: l + dw.round() as i32,
        bottom: t + dh.round() as i32,
    }
}

fn extract_win32_hwnd(win: &slint::Window) -> Option<HWND> {
    let handle = win.window_handle();
    let wh = handle.window_handle().ok()?;
    match wh.as_raw() {
        RawWindowHandle::Win32(h) => Some(HWND(h.hwnd.get() as *mut c_void)),
        _ => None,
    }
}

fn apply_slint_win32_chrome(
    win: &slint::Window,
    dark: bool,
    tool_window: bool,
    no_activate: bool,
    topmost: bool,
) {
    let Some(hwnd) = extract_win32_hwnd(win) else {
        return;
    };
    unsafe {
        let mut ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
        if tool_window {
            ex |= WS_EX_TOOLWINDOW.0;
        }
        if no_activate {
            ex |= WS_EX_NOACTIVATE.0;
        }
        if topmost {
            ex |= WS_EX_TOPMOST.0;
        }
        let _ = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex as isize);
        let sz = win.size();
        apply_cross_os_popup_chrome(
            hwnd,
            dark,
            sz.width as i32,
            sz.height as i32,
            8,
        );
        if topmost {
            let flags = SWP_NOMOVE | SWP_NOSIZE | if no_activate { SWP_NOACTIVATE } else { windows::Win32::UI::WindowsAndMessaging::SET_WINDOW_POS_FLAGS(0) };
            let _ = SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
        }
    }
}

fn apply_theme_to_component<C>(comp: &C, theme: &SlintThemeSpec)
where
    C: ComponentHandle,
    for<'a> ThemeTokens<'a>: slint::Global<'a, C>,
{
    let tokens = comp.global::<ThemeTokens>();
    tokens.invoke_sync_palette(theme.dark);
    tokens.set_accent(theme.to_slint_accent());
}

fn rgba_to_slint_image(rgba: &Option<(u32, u32, Vec<u8>)>) -> (Image, bool) {
    if let Some((w, h, bytes)) = rgba {
        if *w > 0 && *h > 0 && bytes.len() == (*w as usize) * (*h as usize) * 4 {
            let buf = SharedPixelBuffer::<Rgba8Pixel>::clone_from_slice(bytes, *w, *h);
            return (Image::from_rgba8(buf), true);
        }
    }
    (Image::default(), false)
}

fn make_thumbs_model(thumbs: &[LayoutThumbData]) -> ModelRc<ZoneThumbRect> {
    let items: Vec<ZoneThumbRect> = thumbs
        .iter()
        .map(|z| ZoneThumbRect {
            rx: z.rx,
            ry: z.ry,
            rw: z.rw,
            rh: z.rh,
            layer: z.layer,
        })
        .collect();
    ModelRc::new(VecModel::from(items))
}

/// 强前台激活（通过短暂 AttachThreadInput 突破后台进程 SetForegroundWindow 限制，确保搜索框 100% 接住键盘输入）
fn force_foreground_window(hwnd: HWND) {
    unsafe {
        if hwnd.0.is_null() || !IsWindow(hwnd).as_bool() {
            return;
        }
        let _ = AllowSetForegroundWindow(ASFW_ANY);
        let fg = GetForegroundWindow();
        let cur_tid = GetCurrentThreadId();
        let fg_tid = if !fg.0.is_null() {
            GetWindowThreadProcessId(fg, None)
        } else {
            0
        };
        let attached = fg_tid != 0 && fg_tid != cur_tid && AttachThreadInput(cur_tid, fg_tid, true).as_bool();
        let _ = BringWindowToTop(hwnd);
        let _ = SetForegroundWindow(hwnd);
        if attached {
            let _ = AttachThreadInput(cur_tid, fg_tid, false);
        }
    }
}

/// 在 QuickSwitch 已打开时主动将前台焦点与 Slint 键盘焦点拉至搜索输入框（供钩子捕获首字母入搜调用）
pub fn focus_quick_switch_search() {
    if !SLINT_STARTED.load(Ordering::Acquire) {
        return;
    }
    let _ = slint::invoke_from_event_loop(|| {
        let win = SLINT_WIN_POOL.with(|c| c.borrow().quick_switch.as_ref().map(|w| w.clone_strong()));
        if let Some(w) = win {
            if let Some(hwnd) = extract_win32_hwnd(w.window()) {
                force_foreground_window(hwnd);
            }
            w.invoke_focus_input();
        }
    });
}

fn resize_direction_from_code(code: i32) -> ResizeDirection {
    match code {
        1 => ResizeDirection::North,
        2 => ResizeDirection::South,
        3 => ResizeDirection::West,
        4 => ResizeDirection::East,
        5 => ResizeDirection::NorthWest,
        6 => ResizeDirection::NorthEast,
        7 => ResizeDirection::SouthWest,
        _ => ResizeDirection::SouthEast,
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. 公开控制 API：QuickSwitch / LayoutHub / WorkspaceCapture / Progress
// ─────────────────────────────────────────────────────────────────────────────

pub fn show_or_update_quick_switch(state: QuickSwitchViewState) -> bool {
    if !ensure_slint_thread() {
        wk_warn!(
            "slint",
            "ui_slint",
            "show_or_update_quick_switch aborted: Slint thread failed to start"
        );
        return false;
    }
    if let Err(e) = slint::invoke_from_event_loop(move || {
        // 1. 短临界区取出或创建 QuickSwitchWindow，绝不在持有 RefCell 借用期间调任何 Slint 方法！
        let existing = SLINT_WIN_POOL.with(|c| c.borrow().quick_switch.as_ref().map(|w| w.clone_strong()));
        let w = match existing {
            Some(win) => win,
            None => match QuickSwitchWindow::new() {
                Ok(w) => {
                    let w_weak_move = w.as_weak();
                    w.on_drag_move_requested(move || {
                        if let Some(win) = w_weak_move.upgrade() {
                            win.window().with_winit_window(|ww| {
                                let _ = ww.drag_window();
                            });
                            report_qs_bounds(&win);
                        }
                    });

                    let w_weak_resize = w.as_weak();
                    w.on_drag_resize_requested(move |code| {
                        if let Some(win) = w_weak_resize.upgrade() {
                            let dir = resize_direction_from_code(code);
                            win.window().with_winit_window(|ww| {
                                let _ = ww.drag_resize_window(dir);
                            });
                            report_qs_bounds(&win);
                        }
                    });

                    let w_weak_ratio = w.as_weak();
                    w.on_list_ratio_changed(move |ratio| {
                        QS_THUMB_STATE.with(|c| {
                            if let Ok(mut ts) = c.try_borrow_mut() {
                                ts.list_ratio = ratio;
                            }
                        });
                        if let Some(win) = w_weak_ratio.upgrade() {
                            refresh_qs_thumbnail_unlocked(&win);
                            report_qs_bounds(&win);
                        }
                    });

                    let w_weak_slot = w.as_weak();
                    w.on_preview_slot_changed(move |x, y, pw, ph| {
                        QS_THUMB_STATE.with(|c| {
                            if let Ok(mut ts) = c.try_borrow_mut() {
                                ts.preview_slot_logical = (x, y, pw, ph);
                            }
                        });
                        if let Some(win) = w_weak_slot.upgrade() {
                            refresh_qs_thumbnail_unlocked(&win);
                        }
                    });

                    w.on_select_row(|idx| push_ui_event(SlintUiEvent::QsSelectRow(idx)));
                    w.on_activate_row(|idx| push_ui_event(SlintUiEvent::QsActivateRow(idx)));
                    w.on_close_row(|idx| push_ui_event(SlintUiEvent::QsCloseRow(idx)));
                    w.on_filter_changed(|txt| {
                        push_ui_event(SlintUiEvent::QsFilterChanged(txt.to_string()));
                    });
                    w.on_clear_filter(|| push_ui_event(SlintUiEvent::QsClearFilter));
                    w.on_search_engaged(|| push_ui_event(SlintUiEvent::QsSearchEngaged));
                    w.on_action_requested(|act| {
                        push_ui_event(SlintUiEvent::QsAction(act.to_string()));
                    });
                    w.on_key_command(|key, shift, ctrl| {
                        push_ui_event(SlintUiEvent::QsKeyCommand {
                            key: key.to_string(),
                            shift,
                            ctrl,
                        });
                    });
                    w.on_dismiss_requested(|| push_ui_event(SlintUiEvent::QsDismissed));
                    w.window().on_close_requested(|| {
                        push_ui_event(SlintUiEvent::QsDismissed);
                        slint::CloseRequestResponse::HideWindow
                    });
                    w.window().on_winit_window_event(|_win, ev| {
                        if let slint::winit_030::winit::event::WindowEvent::Focused(false) = ev {
                            push_ui_event(SlintUiEvent::QsDismissed);
                        }
                        slint::winit_030::EventResult::Propagate
                    });

                    SLINT_WIN_POOL.with(|c| {
                        c.borrow_mut().quick_switch = Some(w.clone_strong());
                    });
                    w
                }
                Err(e) => {
                    wk_warn!("slint", "ui_slint", "QuickSwitchWindow::new failed: {}", e);
                    return;
                }
            },
        };

        // 2. 脱离任何 RefCell 借用安全更新全部 Slint 属性与原生 Win32 壳
        apply_theme_to_component(&w, &state.theme);

        let mut rows = Vec::with_capacity(state.items.len());
        let mut sel_title = String::new();
        let mut sel_app = String::new();
        let mut sel_icon = Image::default();
        let mut sel_has_icon = false;
        let mut sel_pinned = false;

        for item in &state.items {
            let (img, has_icon) = rgba_to_slint_image(&item.icon_rgba);
            if item.index == state.selected_index {
                sel_title = item.title.clone();
                sel_app = item.app_name.clone();
                sel_icon = img.clone();
                sel_has_icon = has_icon;
                sel_pinned = item.is_pinned;
            }
            rows.push(QuickSwitchRowItem {
                index: item.index,
                title: SharedString::from(item.title.as_str()),
                app_name: SharedString::from(item.app_name.as_str()),
                icon: img,
                has_icon,
                number_label: SharedString::from(item.number_label.as_str()),
                is_minimized: item.is_minimized,
                is_pinned: item.is_pinned,
                is_tray: item.is_tray,
                state_badge: SharedString::from(item.state_badge.as_str()),
            });
        }

        w.set_items(ModelRc::new(VecModel::from(rows)));
        w.set_selected_index(state.selected_index);
        if w.get_filter_text().as_str() != state.filter_text.as_str() {
            w.set_filter_text(SharedString::from(state.filter_text.as_str()));
        }
        w.set_search_visible(state.search_visible);
        w.set_search_locked(state.search_locked);
        w.set_show_numbers(state.show_numbers);
        w.set_show_app_names(state.show_app_names);
        w.set_compact_list(state.compact_list);
        w.set_same_app_mode(state.same_app_mode);
        w.set_scope_title(SharedString::from(state.scope_title.as_str()));
        w.set_list_width_ratio(state.list_width_ratio);
        w.set_selected_title(SharedString::from(sel_title.as_str()));
        w.set_selected_app_name(SharedString::from(sel_app.as_str()));
        w.set_selected_icon(sel_icon);
        w.set_selected_has_icon(sel_has_icon);
        w.set_selected_is_pinned(sel_pinned);
        if state.zh_ui {
            w.set_search_placeholder(SharedString::from("搜索窗口 / 拼音首字母 · 1-9 直达"));
            w.set_empty_label(SharedString::from("没有匹配的窗口"));
            w.set_preview_unavailable_hint(SharedString::from(
                "窗口已最小化或收纳至托盘，按 Enter 或双击立即恢复",
            ));
        } else {
            w.set_search_placeholder(SharedString::from("Search windows / Pinyin · 1-9 jump"));
            w.set_empty_label(SharedString::from("No matching windows"));
            w.set_preview_unavailable_hint(SharedString::from(
                "Window is minimized or in tray. Press Enter or double-click to restore.",
            ));
        }

        if state.reposition_window {
            let r = state.window_rect;
            let rw = r.width().max(580) as u32;
            let rh = r.height().max(340) as u32;
            w.window().set_size(slint::PhysicalSize::new(rw, rh));
            w.window()
                .set_position(slint::PhysicalPosition::new(r.left, r.top));
            if let Err(e) = w.show() {
                wk_warn!("slint", "ui_slint", "QuickSwitchWindow::show failed: {}", e);
            }
            w.window().set_size(slint::PhysicalSize::new(rw, rh));
            w.window()
                .set_position(slint::PhysicalPosition::new(r.left, r.top));
            w.window().with_winit_window(|ww| {
                let _ = ww.request_inner_size(PhysicalSize::new(rw, rh));
                ww.set_outer_position(PhysicalPosition::new(r.left, r.top));
            });
            apply_slint_win32_chrome(w.window(), state.theme.dark, true, false, true);
            if let Some(hwnd) = extract_win32_hwnd(w.window()) {
                unsafe {
                    let _ = SetWindowPos(
                        hwnd,
                        HWND_TOPMOST,
                        r.left,
                        r.top,
                        rw as i32,
                        rh as i32,
                        windows::Win32::UI::WindowsAndMessaging::SET_WINDOW_POS_FLAGS(0),
                    );
                }
                force_foreground_window(hwnd);
                wk_info!(
                    "slint",
                    "ui_slint",
                    "QuickSwitchWindow shown hwnd=0x{:x} rect={}x{} @ ({},{})",
                    hwnd.0 as isize,
                    rw,
                    rh,
                    r.left,
                    r.top
                );
            }
            w.invoke_focus_input();
            w.invoke_notify_preview_slot();
        } else {
            apply_slint_win32_chrome(w.window(), state.theme.dark, true, false, true);
        }

        // 3. 更新 DWM 缩略图状态并刷新
        QS_THUMB_STATE.with(|c| {
            if let Ok(mut ts) = c.try_borrow_mut() {
                if ts.thumb_target != state.preview_target_hwnd {
                    ts.unregister_thumb();
                    ts.thumb_target = state.preview_target_hwnd;
                }
                ts.show_live_preview = state.show_live_preview;
                ts.preview_in_desktop_pos = state.preview_in_desktop_position;
                ts.preview_target_minimized = state.preview_target_minimized;
                ts.preview_norm_rect = state.preview_target_norm_rect;
                ts.list_ratio = state.list_width_ratio;
            }
        });
        refresh_qs_thumbnail_unlocked(&w);
    }) {
        wk_warn!(
            "slint",
            "ui_slint",
            "invoke_from_event_loop failed for QuickSwitch: {}",
            e
        );
        return false;
    }
    true
}

fn report_qs_bounds(win: &QuickSwitchWindow) {
    let mut rect = None;
    win.window().with_winit_window(|ww| {
        if let Ok(pos) = ww.outer_position() {
            let sz = ww.inner_size();
            if sz.width >= 320 && sz.height >= 240 {
                rect = Some(RectI::from_xywh(pos.x, pos.y, sz.width as i32, sz.height as i32));
            }
        }
    });
    if let Some(r) = rect {
        push_ui_event(SlintUiEvent::QsBoundsChanged {
            rect: r,
            list_width_ratio: win.get_list_width_ratio(),
        });
    }
}

pub fn hide_quick_switch() {
    if !SLINT_STARTED.load(Ordering::Acquire) {
        return;
    }
    let _ = slint::invoke_from_event_loop(|| {
        QS_THUMB_STATE.with(|c| {
            if let Ok(mut ts) = c.try_borrow_mut() {
                ts.unregister_thumb();
            }
        });
        let win = SLINT_WIN_POOL.with(|c| c.borrow().quick_switch.as_ref().map(|w| w.clone_strong()));
        if let Some(w) = win {
            let _ = w.hide();
        }
    });
}

pub fn show_or_update_layout_hub(state: LayoutHubViewState) -> bool {
    if !ensure_slint_thread() {
        return false;
    }
    let _ = slint::invoke_from_event_loop(move || {
        let existing = SLINT_WIN_POOL.with(|c| c.borrow().layout_hub.as_ref().map(|w| w.clone_strong()));
        let w = match existing {
            Some(win) => win,
            None => match LayoutHubWindow::new() {
                Ok(w) => {
                    let w_drag = w.as_weak();
                    w.on_drag_window_requested(move || {
                        if let Some(win) = w_drag.upgrade() {
                            win.window().with_winit_window(|ww| {
                                let _ = ww.drag_window();
                            });
                        }
                    });
                    let w_min = w.as_weak();
                    w.on_minimize_window(move || {
                        if let Some(win) = w_min.upgrade() {
                            win.window().with_winit_window(|ww| {
                                ww.set_minimized(true);
                            });
                        }
                    });
                    let w_max = w.as_weak();
                    w.on_toggle_maximize_window(move || {
                        if let Some(win) = w_max.upgrade() {
                            win.window().with_winit_window(|ww| {
                                ww.set_maximized(!ww.is_maximized());
                            });
                        }
                    });

                    w.on_select_monitor(|idx| push_ui_event(SlintUiEvent::HubSelectMonitor(idx)));
                    w.on_select_desktop(|id| {
                        push_ui_event(SlintUiEvent::HubSelectDesktop(id.to_string()));
                    });
                    w.on_select_layout(|id| {
                        push_ui_event(SlintUiEvent::HubSelectLayout(id.to_string()));
                    });
                    w.on_open_template_editor(|id| {
                        push_ui_event(SlintUiEvent::HubOpenTemplateEditor(id.to_string()));
                    });
                    let w_ctx = w.as_weak();
                    w.on_open_custom_menu(move |id, card_idx| {
                        if let Some(win) = w_ctx.upgrade() {
                            win.set_context_menu_layout_id(id);
                            win.set_context_menu_card_index(card_idx);
                            win.set_context_menu_open(true);
                        }
                    });
                    w.on_custom_menu_action(|action, id| {
                        push_ui_event(SlintUiEvent::HubCustomMenuAction {
                            action: action.to_string(),
                            layout_id: id.to_string(),
                        });
                    });
                    w.on_open_create_modal(|| push_ui_event(SlintUiEvent::HubOpenCreateModal));
                    w.on_draft_preview_changed(|zc, ss, sp, hd| {
                        push_ui_event(SlintUiEvent::HubDraftPreviewChanged {
                            zone_count: zc,
                            show_spacing: ss,
                            spacing: sp,
                            highlight_distance: hd,
                        });
                    });

                    let w_save = w.as_weak();
                    w.on_modal_save(move || {
                        if let Some(win) = w_save.upgrade() {
                            push_ui_event(SlintUiEvent::HubModalSave {
                                name: win.get_draft_name().to_string(),
                                zone_count: win.get_draft_zone_count(),
                                show_spacing: win.get_draft_show_spacing(),
                                spacing: win.get_draft_spacing(),
                                highlight_distance: win.get_draft_highlight_distance(),
                                hotkey_index: win.get_draft_hotkey_index(),
                                default_horizontal: win.get_draft_default_horizontal(),
                                default_vertical: win.get_draft_default_vertical(),
                            });
                        }
                    });
                    w.on_modal_cancel(|| push_ui_event(SlintUiEvent::HubModalCancel));
                    w.on_modal_duplicate(|| push_ui_event(SlintUiEvent::HubModalDuplicate));
                    w.on_modal_delete(|| push_ui_event(SlintUiEvent::HubModalDelete));
                    w.on_modal_open_fullscreen_zones(|| {
                        push_ui_event(SlintUiEvent::HubModalOpenFullscreenZones);
                    });
                    w.on_create_confirm(|name, is_canvas| {
                        push_ui_event(SlintUiEvent::HubCreateConfirm {
                            name: name.to_string(),
                            is_canvas,
                        });
                    });
                    let w_cls = w.as_weak();
                    w.on_close_window(move || {
                        if let Some(win) = w_cls.upgrade() {
                            let _ = win.hide();
                        }
                        push_ui_event(SlintUiEvent::HubClosed);
                    });
                    w.window().on_close_requested(|| {
                        push_ui_event(SlintUiEvent::HubClosed);
                        slint::CloseRequestResponse::HideWindow
                    });

                    SLINT_WIN_POOL.with(|c| {
                        c.borrow_mut().layout_hub = Some(w.clone_strong());
                    });
                    w
                }
                Err(e) => {
                    wk_warn!("slint", "ui_slint", "LayoutHubWindow::new failed: {}", e);
                    return;
                }
            },
        };

        apply_theme_to_component(&w, &state.theme);

            // 双语文案
            if state.zh_ui {
                w.set_txt_title(SharedString::from("SnapZones 布局编辑器"));
                w.set_txt_templates(SharedString::from("模板"));
                w.set_txt_custom(SharedString::from("自定义"));
                w.set_txt_new_layout(SharedString::from("新建布局"));
                w.set_txt_save(SharedString::from("保存"));
                w.set_txt_cancel(SharedString::from("取消"));
                w.set_txt_create(SharedString::from("创建"));
                w.set_txt_edit(SharedString::from("编辑"));
                w.set_txt_edit_zones(SharedString::from("编辑区域"));
                w.set_txt_duplicate(SharedString::from("复制"));
                w.set_txt_delete(SharedString::from("删除"));
                w.set_txt_zone_count(SharedString::from("区域数"));
                w.set_txt_zone_count_unit(SharedString::from("区域"));
                w.set_txt_spacing(SharedString::from("区域周围的空间"));
                w.set_txt_pixels(SharedString::from("像素"));
                w.set_txt_highlight_dist(SharedString::from("突出显示距离"));
                w.set_txt_default_horiz(SharedString::from("水平监视器方向的默认布局"));
                w.set_txt_default_vert(SharedString::from("垂直监视器方向的默认布局"));
                w.set_txt_layout_name(SharedString::from("名称"));
                w.set_txt_layout_hotkey(SharedString::from("布局快捷键"));
                w.set_txt_grid_label(SharedString::from("网格 (Grid)"));
                w.set_txt_grid_desc(SharedString::from("通过分割线和合并单元格精准切分"));
                w.set_txt_canvas_label(SharedString::from("画布 (Canvas)"));
                w.set_txt_canvas_desc(SharedString::from("自由拖放和重叠任意尺寸分区"));
                w.set_txt_no_custom(SharedString::from(
                    "暂无自定义布局，点击右下角“新建布局”或复制模板开始创建",
                ));
                w.set_hotkey_options(ModelRc::new(VecModel::from(
                    ["无", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]
                        .into_iter()
                        .map(SharedString::from)
                        .collect::<Vec<_>>(),
                )));
            } else {
                w.set_txt_title(SharedString::from("SnapZones Layout Editor"));
                w.set_txt_templates(SharedString::from("Templates"));
                w.set_txt_custom(SharedString::from("Custom"));
                w.set_txt_new_layout(SharedString::from("Create new"));
                w.set_txt_save(SharedString::from("Save"));
                w.set_txt_cancel(SharedString::from("Cancel"));
                w.set_txt_create(SharedString::from("Create"));
                w.set_txt_edit(SharedString::from("Edit"));
                w.set_txt_edit_zones(SharedString::from("Edit zones"));
                w.set_txt_duplicate(SharedString::from("Duplicate"));
                w.set_txt_delete(SharedString::from("Delete"));
                w.set_txt_zone_count(SharedString::from("Number of zones"));
                w.set_txt_zone_count_unit(SharedString::from("zones"));
                w.set_txt_spacing(SharedString::from("Space around zones"));
                w.set_txt_pixels(SharedString::from("px"));
                w.set_txt_highlight_dist(SharedString::from("Distance to highlight adjacent zones"));
                w.set_txt_default_horiz(SharedString::from("Default layout for horizontal monitors"));
                w.set_txt_default_vert(SharedString::from("Default layout for vertical monitors"));
                w.set_txt_layout_name(SharedString::from("Name"));
                w.set_txt_layout_hotkey(SharedString::from("Shortcut key"));
                w.set_txt_grid_label(SharedString::from("Grid"));
                w.set_txt_grid_desc(SharedString::from("Split and merge zones cleanly on a grid"));
                w.set_txt_canvas_label(SharedString::from("Canvas"));
                w.set_txt_canvas_desc(SharedString::from("Freeform resizable and overlapping zones"));
                w.set_txt_no_custom(SharedString::from(
                    "No custom layouts yet. Click Create new or duplicate a template.",
                ));
                w.set_hotkey_options(ModelRc::new(VecModel::from(
                    ["None", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]
                        .into_iter()
                        .map(SharedString::from)
                        .collect::<Vec<_>>(),
                )));
            }

            let mon_models: Vec<MonitorCardItem> = state
                .monitors
                .iter()
                .map(|m| MonitorCardItem {
                    index: m.index,
                    badge_num: SharedString::from(m.badge_num.as_str()),
                    resolution: SharedString::from(m.resolution.as_str()),
                    scale_text: SharedString::from(m.scale_text.as_str()),
                    friendly_name: SharedString::from(m.friendly_name.as_str()),
                    selected: m.selected,
                })
                .collect();
            w.set_monitors(ModelRc::new(VecModel::from(mon_models)));

            let vd_models: Vec<VirtualDesktopPill> = state
                .desktops
                .iter()
                .map(|d| VirtualDesktopPill {
                    id: SharedString::from(d.id.as_str()),
                    label: SharedString::from(d.label.as_str()),
                    active: d.active,
                })
                .collect();
            w.set_desktops(ModelRc::new(VecModel::from(vd_models)));

            let map_card = |c: &LayoutCardData| LayoutCardItem {
                id: SharedString::from(c.id.as_str()),
                name: SharedString::from(c.name.as_str()),
                is_builtin: c.is_builtin,
                is_no_layout: c.is_no_layout,
                is_layered: c.is_layered,
                is_active: c.is_active,
                hotkey_badge: SharedString::from(c.hotkey_badge.as_str()),
                thumbs: make_thumbs_model(&c.thumbs),
            };
            w.set_template_layouts(ModelRc::new(VecModel::from(
                state.template_layouts.iter().map(map_card).collect::<Vec<_>>(),
            )));
            w.set_custom_layouts(ModelRc::new(VecModel::from(
                state.custom_layouts.iter().map(map_card).collect::<Vec<_>>(),
            )));

            w.set_modal_mode(state.modal_mode);
            w.set_modal_title(SharedString::from(state.modal_title.as_str()));
            w.set_draft_layout_id(SharedString::from(state.draft_layout_id.as_str()));
            w.set_draft_name(SharedString::from(state.draft_name.as_str()));
            w.set_draft_zone_count(state.draft_zone_count);
            w.set_draft_show_spacing(state.draft_show_spacing);
            w.set_draft_spacing(state.draft_spacing);
            w.set_draft_highlight_distance(state.draft_highlight_distance);
            w.set_draft_hotkey_index(state.draft_hotkey_index);
            w.set_draft_default_horizontal(state.draft_default_horizontal);
            w.set_draft_default_vertical(state.draft_default_vertical);
            w.set_draft_create_canvas(state.draft_create_canvas);
            w.set_draft_is_layered(state.draft_is_layered);
            w.set_draft_thumbs(make_thumbs_model(&state.draft_thumbs));

            if state.reposition_window {
                let r = state.window_rect;
                let rw = r.width().max(760) as u32;
                let rh = r.height().max(480) as u32;
                w.window().set_size(slint::PhysicalSize::new(rw, rh));
                w.window()
                    .set_position(slint::PhysicalPosition::new(r.left, r.top));
                let _ = w.show();
                w.window().with_winit_window(|ww| {
                    let _ = ww.request_inner_size(PhysicalSize::new(rw, rh));
                    ww.set_outer_position(PhysicalPosition::new(r.left, r.top));
                });
                apply_slint_win32_chrome(w.window(), state.theme.dark, false, false, true);
                if let Some(hwnd) = extract_win32_hwnd(w.window()) {
                    unsafe {
                        let _ = SetWindowPos(
                            hwnd,
                            HWND_TOPMOST,
                            r.left,
                            r.top,
                            rw as i32,
                            rh as i32,
                            windows::Win32::UI::WindowsAndMessaging::SET_WINDOW_POS_FLAGS(0),
                        );
                        let _ = SetForegroundWindow(hwnd);
                    }
                }
            } else {
                apply_slint_win32_chrome(w.window(), state.theme.dark, false, false, true);
            }
    });
    true
}

pub fn hide_layout_hub() {
    if !SLINT_STARTED.load(Ordering::Acquire) {
        return;
    }
    let _ = slint::invoke_from_event_loop(|| {
        let win = SLINT_WIN_POOL.with(|c| c.borrow().layout_hub.as_ref().map(|w| w.clone_strong()));
        if let Some(w) = win {
            w.set_context_menu_open(false);
            w.set_modal_mode(0);
            let _ = w.hide();
        }
    });
}

pub fn raise_layout_hub_above_preview() {
    if !SLINT_STARTED.load(Ordering::Acquire) {
        return;
    }
    let _ = slint::invoke_from_event_loop(|| {
        let win = SLINT_WIN_POOL.with(|c| c.borrow().layout_hub.as_ref().map(|w| w.clone_strong()));
        if let Some(w) = win {
            if let Some(hwnd) = extract_win32_hwnd(w.window()) {
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
            }
        }
    });
}

pub fn show_or_update_workspace_capture(state: WorkspaceCaptureViewState) -> bool {
    if !ensure_slint_thread() {
        return false;
    }
    let _ = slint::invoke_from_event_loop(move || {
        let existing = SLINT_WIN_POOL.with(|c| c.borrow().workspace_capture.as_ref().map(|w| w.clone_strong()));
        let w = match existing {
            Some(win) => win,
            None => match WorkspaceCaptureWindow::new() {
                Ok(w) => {
                    let w_drag = w.as_weak();
                    w.on_drag_window_requested(move || {
                        if let Some(win) = w_drag.upgrade() {
                            win.window().with_winit_window(|ww| {
                                let _ = ww.drag_window();
                            });
                        }
                    });
                    w.on_capture_clicked(|| push_ui_event(SlintUiEvent::CaptureConfirm));
                    w.on_cancel_clicked(|| push_ui_event(SlintUiEvent::CaptureCancel));
                    w.window().on_close_requested(|| {
                        push_ui_event(SlintUiEvent::CaptureCancel);
                        slint::CloseRequestResponse::HideWindow
                    });
                    SLINT_WIN_POOL.with(|c| {
                        c.borrow_mut().workspace_capture = Some(w.clone_strong());
                    });
                    w
                }
                Err(_) => return,
            },
        };
        apply_theme_to_component(&w, &state.theme);
        w.set_busy(state.busy);
        if state.zh_ui {
            w.set_txt_title(SharedString::from("快速创建工作区"));
            w.set_txt_hint(SharedString::from(format!(
                "排列好窗口布局，完成后单击“捕获”（当前检测到 {} 个窗口）。",
                state.detected_windows
            )));
            w.set_txt_capture(SharedString::from("捕获"));
            w.set_txt_cancel(SharedString::from("取消"));
        } else {
            w.set_txt_title(SharedString::from("Create Workspace"));
            w.set_txt_hint(SharedString::from(format!(
                "Arrange your windows, then click Capture ({} windows detected).",
                state.detected_windows
            )));
            w.set_txt_capture(SharedString::from("Capture"));
            w.set_txt_cancel(SharedString::from("Cancel"));
        }

        let _ = w.show();
        let r = state.window_rect;
        w.window().with_winit_window(|ww| {
            let _ = ww.request_inner_size(PhysicalSize::new(
                r.width().max(340) as u32,
                r.height().max(140) as u32,
            ));
            ww.set_outer_position(PhysicalPosition::new(r.left, r.top));
        });
        apply_slint_win32_chrome(w.window(), state.theme.dark, true, false, true);
        if let Some(hwnd) = extract_win32_hwnd(w.window()) {
            unsafe {
                let _ = SetForegroundWindow(hwnd);
            }
        }
    });
    true
}

pub fn hide_workspace_capture() {
    if !SLINT_STARTED.load(Ordering::Acquire) {
        return;
    }
    let _ = slint::invoke_from_event_loop(|| {
        let win = SLINT_WIN_POOL.with(|c| c.borrow().workspace_capture.as_ref().map(|w| w.clone_strong()));
        if let Some(w) = win {
            let _ = w.hide();
        }
    });
}

pub fn show_or_update_workspace_progress(state: WorkspaceProgressViewState) {
    if !ensure_slint_thread() {
        return;
    }
    let _ = slint::invoke_from_event_loop(move || {
        let existing = SLINT_WIN_POOL.with(|c| c.borrow().workspace_progress.as_ref().map(|w| w.clone_strong()));
        let w = match existing {
            Some(win) => win,
            None => match WorkspaceProgressWindow::new() {
                Ok(w) => {
                    SLINT_WIN_POOL.with(|c| {
                        c.borrow_mut().workspace_progress = Some(w.clone_strong());
                    });
                    w
                }
                Err(_) => return,
            },
        };
        apply_theme_to_component(&w, &state.theme);
        w.set_workspace_name(SharedString::from(state.workspace_name.as_str()));
        w.set_status_text(SharedString::from(state.status_text.as_str()));
        w.set_progress(state.progress.clamp(0.0, 1.0));
        let _ = w.show();
        let r = state.window_rect;
        w.window().with_winit_window(|ww| {
            let _ = ww.request_inner_size(PhysicalSize::new(
                r.width().max(300) as u32,
                r.height().max(76) as u32,
            ));
            ww.set_outer_position(PhysicalPosition::new(r.left, r.top));
        });
        apply_slint_win32_chrome(w.window(), state.theme.dark, true, true, true);
    });
}

pub fn hide_workspace_progress() {
    if !SLINT_STARTED.load(Ordering::Acquire) {
        return;
    }
    let _ = slint::invoke_from_event_loop(|| {
        let win = SLINT_WIN_POOL.with(|c| c.borrow().workspace_progress.as_ref().map(|w| w.clone_strong()));
        if let Some(w) = win {
            let _ = w.hide();
        }
    });
}
