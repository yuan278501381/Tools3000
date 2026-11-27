// ─────────────────────────────────────────────────────────────────────────────
// features/quick_switch.rs — Quick Switch v2（双独立轨道 + 窗口中枢 + 自由拖缩）
//
// 吸收 AltTabio 纯 Rust 精华并全面超越:
//   1. 双独立切窗轨道：
//      - 轨道 A：全局增强 Alt+Tab（5/8 黄金视野 + 底部浮现中文拼音搜索胶囊 + DWM 实时预览 + 托盘窗捞回）
//      - 轨道 B：独立同应用切窗 Alt+`（不接管原生 Alt+Tab 时也可独立开启，支持含最小化窗、预览、键盘/滚轮）
//   2. 桌面原位预览模式（参考 AltTabio preview.rs）：可按真实桌面坐标与尺寸比例投影 DWM 缩略图，
//      一眼看出窗口位于屏幕何处，并支持右上角一键在「居中放大 / 原位投影」间切换。
//   3. 完整窗口操控中枢：F4(关)/F5(最小化)/F6(最大化)/F7(还原)/F8(防误杀强杀)/F9(新开降权实例) +
//      P(置顶)/Z(下一分区)/T(收托盘或捞回) + 原生右键上下文菜单 + GetLastActivePopup 激活。
//   4. 标准 Windows 窗口自由度：顶栏自由拖动 + 原生 WM_NCHITTEST 8 向边角缩放 + 中央分割线拖拽调宽 +
//      最大化/复位/关闭按钮 + 跨多屏拔插安全位置记忆持久化回写。
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{parse_hex_color, WindowKitConfig};
use crate::core::constants::{
    ANIM_QUICK_SWITCH_FADE_MS, ANIM_THUMBNAIL_FADE_MS, FILE_QUICKSWITCH_BOUNDS_RESULT,
    HOST_MESSAGE_WINDOW_CLASS, HOST_QUICKSWITCH_BOUNDS_MSG_NAME, QUICK_SWITCH_MAX_VISIBLE_ROWS,
    QUICK_SWITCH_ROW_COMFORTABLE_DIP, QUICK_SWITCH_ROW_COMPACT_DIP, QUICK_SWITCH_WINDOW_CLASS,
    WM_WK_INTERNAL_QUICK_SWITCH_KEY,
};
use crate::core::geometry::RectI;
use crate::core::json::JsonValue;
use crate::core::pinyin::{find_title_highlight_utf16_ranges, score_window_match};
use crate::core::topology::{
    apply_cross_os_popup_chrome, ensure_window_rect_on_live_monitors,
    is_window_on_current_virtual_desktop, NormalizedWorkRect,
};
use crate::core::win_utils::{
    activate_window, activate_window_or_popup, close_window_async, enumerate_monitors,
    enumerate_user_windows, get_window_dpi, get_window_icon_safe, get_window_normal_placement_rect,
    get_window_process_info, kill_window_process_safe, minimize_window_async, monitor_for_hwnd,
    monitor_for_point, query_process_creation_time, set_window_layered_alpha,
    spawn_process_with_elevation_policy, to_wide, write_text_file_atomic, MonitorInfo, WindowInfo,
};
use crate::render::d2d::{
    ease_out_cubic, exp_damp_f32, rect_f, rgb_a, rgba, rounded_rect, SharedGraphics,
};
use crate::{wk_debug, wk_info};
use std::collections::HashMap;
use std::ffi::c_void;
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering};
use std::sync::Once;
use std::time::Instant;
use windows::core::{Interface, PCWSTR};
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, SIZE, WPARAM};
use windows::Win32::Graphics::Direct2D::Common::{D2D_POINT_2F, D2D_SIZE_U};
use windows::Win32::Graphics::Direct2D::{
    ID2D1Bitmap, ID2D1HwndRenderTarget, ID2D1RenderTarget, D2D1_DRAW_TEXT_OPTIONS_CLIP,
    D2D1_ELLIPSE,
};
use windows::Win32::Graphics::DirectWrite::{
    DWRITE_FONT_WEIGHT_BOLD, DWRITE_MEASURING_MODE_NATURAL, DWRITE_TEXT_RANGE,
};
use windows::Win32::Graphics::Dwm::{
    DwmQueryThumbnailSourceSize, DwmRegisterThumbnail, DwmUnregisterThumbnail,
    DwmUpdateThumbnailProperties, DWM_THUMBNAIL_PROPERTIES, DWM_TNP_OPACITY,
    DWM_TNP_RECTDESTINATION, DWM_TNP_SOURCECLIENTAREAONLY, DWM_TNP_VISIBLE,
};
use windows::Win32::Graphics::Gdi::{BeginPaint, EndPaint, InvalidateRect, PAINTSTRUCT};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetAsyncKeyState, ReleaseCapture, SetCapture, VK_BACK, VK_CONTROL, VK_DELETE, VK_DOWN, VK_END,
    VK_ESCAPE, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_HOME, VK_MENU, VK_NEXT, VK_PRIOR,
    VK_RETURN, VK_SHIFT, VK_SPACE, VK_TAB, VK_UP,
};
use windows::Win32::UI::WindowsAndMessaging::{
    AppendMenuW, CreatePopupMenu, CreateWindowExW, DefWindowProcW, DestroyMenu, DestroyWindow,
    FindWindowW, GetClientRect, GetCursorPos, GetForegroundWindow, GetWindowLongPtrW,
    GetWindowRect, GetWindowThreadProcessId, IsWindow, LoadCursorW, PostMessageW, RegisterClassExW,
    RegisterWindowMessageW, SetCursor, SetForegroundWindow, SetWindowLongPtrW, SetWindowPos,
    ShowWindow, ShowWindowAsync, TrackPopupMenu, CS_DBLCLKS, CS_HREDRAW, CS_VREDRAW, GWL_EXSTYLE,
    GWLP_USERDATA, HTBOTTOM, HTBOTTOMLEFT, HTBOTTOMRIGHT, HTCAPTION, HTCLIENT, HTLEFT, HTRIGHT,
    HTTOP, HTTOPLEFT, HTTOPRIGHT, HWND_TOPMOST, IDC_ARROW, IDC_HAND, IDC_SIZEWE, MF_SEPARATOR,
    MF_STRING, MINMAXINFO, SWP_NOACTIVATE, SWP_SHOWWINDOW, SW_HIDE, SW_MAXIMIZE, SW_MINIMIZE,
    SW_RESTORE, SW_SHOW, TPM_NONOTIFY, TPM_RETURNCMD, TPM_RIGHTBUTTON, WA_INACTIVE, WM_ACTIVATE,
    WM_CHAR, WM_EXITSIZEMOVE, WM_GETMINMAXINFO, WM_KEYDOWN, WM_LBUTTONDBLCLK, WM_LBUTTONDOWN,
    WM_LBUTTONUP, WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_NCHITTEST, WM_PAINT, WM_RBUTTONDOWN,
    WM_SETCURSOR, WM_SIZE, WNDCLASSEXW, WS_EX_LAYERED, WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_POPUP,
};

/// 供低级键盘钩子无锁读取的 QuickSwitch 打开状态
pub static QUICK_SWITCH_IS_OPEN: AtomicBool = AtomicBool::new(false);
/// 光标形态：0=箭头, 1=手型, 2=左右分割线拖动
static QS_CURSOR_KIND: AtomicU32 = AtomicU32::new(0);
/// 供 WM_NCHITTEST 无锁读取当前可拖动顶栏高度（物理像素）与禁止调整大小状态
static QS_TITLEBAR_H_PX: AtomicI32 = AtomicI32::new(34);
static QS_RIGHT_CTRL_W_PX: AtomicI32 = AtomicI32::new(148);
static QS_IS_MAXIMIZED: AtomicBool = AtomicBool::new(false);
static QS_HOST_BOUNDS_MSG: AtomicU32 = AtomicU32::new(0);

const QS_CUR_ARROW: u32 = 0;
const QS_CUR_HAND: u32 = 1;
const QS_CUR_SIZEWE: u32 = 2;

const HEADER_H_DIP: f32 = 34.0;
const BOTTOM_SEARCH_H_DIP: f32 = 42.0;
const MIN_WIN_W_DIP: f32 = 580.0;
const MIN_WIN_H_DIP: f32 = 360.0;
const RESIZE_EDGE_PX: i32 = 7;

/// QuickSwitch 内部回传控制窗口的事件编码
pub const QS_EVT_PAINT: usize = 1;
pub const QS_EVT_KEYDOWN: usize = 2;
pub const QS_EVT_CHAR: usize = 3;
pub const QS_EVT_MOUSEMOVE: usize = 4;
pub const QS_EVT_LBUTTONDOWN: usize = 5;
pub const QS_EVT_WHEEL: usize = 6;
pub const QS_EVT_DEACTIVATE: usize = 7;
pub const QS_EVT_LBUTTONUP: usize = 8;
pub const QS_EVT_RBUTTONDOWN: usize = 9;
pub const QS_EVT_SIZE: usize = 10;
pub const QS_EVT_EXITSIZEMOVE: usize = 11;
pub const QS_EVT_LBUTTONDBLCLK: usize = 12;

fn host_bounds_msg_id() -> u32 {
    let cached = QS_HOST_BOUNDS_MSG.load(Ordering::Acquire);
    if cached != 0 {
        return cached;
    }
    let wide = to_wide(HOST_QUICKSWITCH_BOUNDS_MSG_NAME);
    let msg = unsafe { RegisterWindowMessageW(PCWSTR(wide.as_ptr())) };
    QS_HOST_BOUNDS_MSG.store(msg, Ordering::Release);
    msg
}

/// 需要委托主运行期其他模块执行的组合动作
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum QuickSwitchDelegatedAction {
    None,
    Closed,
    TogglePinWindow(isize),
    MoveWindowToNextZone(isize),
    MinimizeWindowToTray(isize),
    RestoreTrayWindowAndClose(isize),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SwitchScope {
    GlobalAll,
    SameApp,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RowActionKind {
    Minimize,
    Close,
    Kill,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum HeaderBtnKind {
    ToggleDesktopPreview,
    ResetBounds,
    ToggleMaximize,
    CloseCard,
}

/// 进程强身份标识（参考 AltTabio ProcessIdentity，防止 PID 复用误杀无辜进程）
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct ProcessIdentity {
    pid: u32,
    started_at: u64,
}

impl ProcessIdentity {
    fn capture(pid: u32) -> Self {
        Self {
            pid,
            started_at: query_process_creation_time(pid),
        }
    }

    fn verify_live(self, hwnd: HWND) -> bool {
        if self.pid <= 4 {
            return false;
        }
        let mut cur_pid = 0u32;
        unsafe {
            let _ = GetWindowThreadProcessId(hwnd, Some(&mut cur_pid));
        }
        if cur_pid != self.pid {
            return false;
        }
        if self.started_at == 0 {
            return true;
        }
        query_process_creation_time(self.pid) == self.started_at
    }
}

#[derive(Debug, Clone)]
struct SwitchEntry {
    info: WindowInfo,
    identity: ProcessIdentity,
    is_tray_hidden: bool,
    score: i32,
}

pub struct QuickSwitchManager {
    control_hwnd: HWND,
    card_hwnd: Option<HWND>,
    render_target: Option<ID2D1HwndRenderTarget>,
    icon_cache: HashMap<isize, ID2D1Bitmap>,
    thumbnail_handle: isize,
    thumbnail_source: isize,
    thumbnail_fade_started: Option<Instant>,
    all_entries: Vec<SwitchEntry>,
    filtered: Vec<SwitchEntry>,
    selected_index: usize,
    scroll_offset: usize,
    hovered_row: Option<usize>,
    hovered_action: Option<(usize, RowActionKind)>,
    hovered_header_btn: Option<HeaderBtnKind>,
    search_query: String,
    search_mode: bool,
    opened_via_alt_tab: bool,
    scope: SwitchScope,
    scope_app_label: String,
    open: bool,
    opened_at: Option<Instant>,
    card_fade_done: bool,
    highlight_y_dip: f32,
    highlight_target_y_dip: f32,
    last_anim_tick: Option<Instant>,
    card_dpi: u32,
    card_width_px: i32,
    card_height_px: i32,
    list_width_ratio: f32,
    dragging_splitter: bool,
    in_context_menu: bool,
    is_maximized: bool,
    restore_rect: Option<RectI>,
    preview_desktop_pos_override: Option<bool>,
    monitors_snapshot: Vec<MonitorInfo>,
}

impl QuickSwitchManager {
    pub fn new(control_hwnd: HWND) -> Self {
        Self {
            control_hwnd,
            card_hwnd: None,
            render_target: None,
            icon_cache: HashMap::new(),
            thumbnail_handle: 0,
            thumbnail_source: 0,
            thumbnail_fade_started: None,
            all_entries: Vec::new(),
            filtered: Vec::new(),
            selected_index: 0,
            scroll_offset: 0,
            hovered_row: None,
            hovered_action: None,
            hovered_header_btn: None,
            search_query: String::new(),
            search_mode: false,
            opened_via_alt_tab: false,
            scope: SwitchScope::GlobalAll,
            scope_app_label: String::new(),
            open: false,
            opened_at: None,
            card_fade_done: true,
            highlight_y_dip: HEADER_H_DIP + 8.0,
            highlight_target_y_dip: HEADER_H_DIP + 8.0,
            last_anim_tick: None,
            card_dpi: 96,
            card_width_px: 980,
            card_height_px: 580,
            list_width_ratio: 0.34,
            dragging_splitter: false,
            in_context_menu: false,
            is_maximized: false,
            restore_rect: None,
            preview_desktop_pos_override: None,
            monitors_snapshot: Vec::new(),
        }
    }

    pub fn set_control_hwnd(&mut self, hwnd: HWND) {
        self.control_hwnd = hwnd;
    }

    pub fn is_open(&self) -> bool {
        self.open
    }

    pub fn has_active_animation(&self) -> bool {
        if !self.open {
            return false;
        }
        !self.card_fade_done
            || self.thumbnail_fade_started.is_some()
            || (self.highlight_y_dip - self.highlight_target_y_dip).abs() > 0.35
    }

    pub fn tick_animation(
        &mut self,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
    ) -> bool {
        if !self.open {
            return false;
        }
        let now = Instant::now();
        let dt_ms = self
            .last_anim_tick
            .map(|t| (now.duration_since(t).as_secs_f32() * 1000.0).clamp(1.0, 50.0))
            .unwrap_or(12.0);
        self.last_anim_tick = Some(now);

        let mut need_repaint = false;

        // 1. 卡片 115ms 缓入
        if !self.card_fade_done {
            if let (Some(hwnd), Some(started)) = (self.card_hwnd, self.opened_at) {
                let t = (started.elapsed().as_secs_f32() * 1000.0) / ANIM_QUICK_SWITCH_FADE_MS;
                if t >= 1.0 {
                    set_window_layered_alpha(hwnd, 255);
                    self.card_fade_done = true;
                } else {
                    let alpha = (68.0 + ease_out_cubic(t) * 187.0).round().clamp(68.0, 255.0) as u8;
                    set_window_layered_alpha(hwnd, alpha);
                }
            } else {
                self.card_fade_done = true;
            }
        }

        // 2. 高亮胶囊弹簧阻尼滑行
        if (self.highlight_y_dip - self.highlight_target_y_dip).abs() > 0.35 {
            self.highlight_y_dip =
                exp_damp_f32(self.highlight_y_dip, self.highlight_target_y_dip, dt_ms, 26.0);
            if (self.highlight_y_dip - self.highlight_target_y_dip).abs() <= 0.35 {
                self.highlight_y_dip = self.highlight_target_y_dip;
            }
            need_repaint = true;
        }

        // 3. DWM 实时缩略图淡入
        if self.thumbnail_fade_started.is_some() {
            self.update_thumbnail(cfg);
        }

        if need_repaint {
            if let Some(g) = gfx {
                self.render(g, cfg);
            } else {
                self.request_repaint();
            }
        }

        self.has_active_animation()
    }

    fn scale(&self) -> f32 {
        (self.card_dpi as f32 / 96.0).max(1.0)
    }

    fn width_dip(&self) -> f32 {
        self.card_width_px as f32 / self.scale()
    }

    fn height_dip(&self) -> f32 {
        self.card_height_px as f32 / self.scale()
    }

    fn list_pane_right_dip(&self) -> f32 {
        let w = self.width_dip();
        let ratio = self.list_width_ratio.clamp(0.22, 0.58);
        (w * ratio).clamp(210.0, (w - 220.0).max(210.0))
    }

    fn list_top_dip(&self) -> f32 {
        HEADER_H_DIP + 6.0
    }

    fn list_bottom_dip(&self) -> f32 {
        let reserve = if self.show_bottom_search_bar() {
            BOTTOM_SEARCH_H_DIP + 10.0
        } else {
            14.0
        };
        (self.height_dip() - reserve).max(self.list_top_dip() + 80.0)
    }

    fn show_bottom_search_bar(&self) -> bool {
        !self.search_query.is_empty() || self.search_mode
    }

    fn effective_desktop_preview(&self, cfg: &WindowKitConfig) -> bool {
        self.preview_desktop_pos_override
            .unwrap_or(cfg.quick_switch.preview_in_desktop_position)
    }

    fn row_height_dip(&self, cfg: &WindowKitConfig) -> f32 {
        if cfg.quick_switch.compact_density {
            QUICK_SWITCH_ROW_COMPACT_DIP
        } else {
            QUICK_SWITCH_ROW_COMFORTABLE_DIP
        }
    }

    fn max_visible_rows(&self, cfg: &WindowKitConfig) -> usize {
        let usable = (self.list_bottom_dip() - self.list_top_dip()).max(80.0);
        let fit = (usable / self.row_height_dip(cfg)).floor() as usize;
        fit.clamp(3, QUICK_SWITCH_MAX_VISIBLE_ROWS + 10)
    }

    fn sync_highlight_target(&mut self, cfg: &WindowKitConfig, snap_immediately: bool) {
        let slot = self.selected_index.saturating_sub(self.scroll_offset);
        let target_y = self.list_top_dip() + (slot as f32) * self.row_height_dip(cfg);
        self.highlight_target_y_dip = target_y;
        if snap_immediately {
            self.highlight_y_dip = target_y;
        }
    }

    /// 轨道 A：响应全局 Alt+Tab / Alt+Shift+Tab
    pub fn on_alt_tab_step(
        &mut self,
        reverse: bool,
        tray_windows: &[(HWND, String)],
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) {
        if !self.open {
            self.open_card(
                true,
                reverse,
                SwitchScope::GlobalAll,
                tray_windows,
                cfg,
                gfx,
                trace_id,
            );
        } else {
            self.step_selection(if reverse { -1 } else { 1 }, cfg, gfx);
        }
    }

    /// 轨道 B：响应同应用切窗 Alt+` / Alt+Shift+`（对标并超越 PowerToys Window Hopper）
    pub fn on_same_app_step(
        &mut self,
        reverse: bool,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) {
        if !self.open {
            self.open_card(true, reverse, SwitchScope::SameApp, &[], cfg, gfx, trace_id);
        } else {
            self.step_selection(if reverse { -1 } else { 1 }, cfg, gfx);
        }
    }

    /// Action 13: 程序化开关 QuickSwitch（直接锁定浏览与拼音搜索态）
    pub fn toggle_programmatic(
        &mut self,
        tray_windows: &[(HWND, String)],
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> bool {
        if self.open {
            self.close_and_release(trace_id);
            true
        } else {
            self.open_card(
                false,
                false,
                SwitchScope::GlobalAll,
                tray_windows,
                cfg,
                gfx,
                trace_id,
            );
            false
        }
    }

    /// 多屏拔插/分辨率改变自愈
    pub fn on_topology_changed(&mut self, cfg: &WindowKitConfig, gfx: Option<&SharedGraphics>) {
        if !self.open {
            return;
        }
        self.monitors_snapshot = enumerate_monitors();
        if let Some(hwnd) = self.card_hwnd {
            let safe = ensure_window_rect_on_live_monitors(
                &cfg.quick_switch.custom_monitor_id,
                cfg.quick_switch.custom_rel_rect,
                self.card_width_px,
                self.card_height_px,
                self.monitors_snapshot.first(),
                &self.monitors_snapshot,
            );
            self.card_width_px = safe.width();
            self.card_height_px = safe.height();
            unsafe {
                let _ = SetWindowPos(
                    hwnd,
                    HWND_TOPMOST,
                    safe.left,
                    safe.top,
                    safe.width(),
                    safe.height(),
                    SWP_NOACTIVATE,
                );
            }
            self.resize_render_target(gfx);
            self.refresh_thumbnail_and_repaint(cfg, gfx);
        }
    }

    /// 响应 Alt 键抬起：若未锁定浏览/搜索/拖拽态且由 Alt+Tab 或 Alt+` 打开，立即切换目标窗口
    pub fn on_alt_released(&mut self, trace_id: &str) -> QuickSwitchDelegatedAction {
        if !self.open
            || !self.opened_via_alt_tab
            || self.search_mode
            || self.dragging_splitter
            || self.in_context_menu
        {
            return QuickSwitchDelegatedAction::None;
        }
        self.commit_selected_and_close(trace_id)
    }

    /// 处理 QuickSwitch 卡片转发来的全量键盘/鼠标/拖缩/右键中枢事件
    pub fn handle_card_event(
        &mut self,
        evt: usize,
        param: isize,
        cfg: &mut WindowKitConfig,
        config_dir: &Path,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> QuickSwitchDelegatedAction {
        if !self.open {
            return QuickSwitchDelegatedAction::None;
        }
        match evt {
            QS_EVT_PAINT => {
                if let Some(g) = gfx {
                    self.render(g, cfg);
                }
                QuickSwitchDelegatedAction::None
            }
            QS_EVT_SIZE => {
                self.search_mode = true; // 缩放期间锁定浏览态防松 Alt 闪退
                self.sync_client_size_and_resize(cfg, gfx);
                QuickSwitchDelegatedAction::None
            }
            QS_EVT_EXITSIZEMOVE => {
                self.search_mode = true;
                self.sync_client_size_and_resize(cfg, gfx);
                if cfg.quick_switch.remember_bounds && !self.is_maximized {
                    self.persist_window_bounds(cfg, config_dir, trace_id);
                }
                QuickSwitchDelegatedAction::None
            }
            QS_EVT_LBUTTONDBLCLK => {
                let (x_dip, y_dip) = self.unpack_dip_coords(param);
                if y_dip <= HEADER_H_DIP && x_dip < self.width_dip() - 130.0 {
                    self.search_mode = true;
                    self.toggle_maximize(cfg, gfx);
                }
                QuickSwitchDelegatedAction::None
            }
            QS_EVT_KEYDOWN => {
                let vk = param as u16;
                let ctrl = async_down(VK_CONTROL.0);
                let shift = async_down(VK_SHIFT.0);
                let alt = async_down(VK_MENU.0);

                // 两段式 Esc：有搜索词先清空恢复列表，否则退出
                if vk == VK_ESCAPE.0 {
                    if !self.search_query.is_empty() {
                        self.search_query.clear();
                        self.apply_filter(cfg);
                        self.sync_highlight_target(cfg, false);
                        self.refresh_thumbnail_and_repaint(cfg, gfx);
                        return QuickSwitchDelegatedAction::None;
                    }
                    self.close_and_release(trace_id);
                    return QuickSwitchDelegatedAction::Closed;
                }
                if vk == VK_RETURN.0 {
                    return self.commit_selected_and_close(trace_id);
                }
                if vk == VK_DOWN.0 {
                    self.step_selection(1, cfg, gfx);
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_UP.0 {
                    self.step_selection(-1, cfg, gfx);
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_TAB.0 {
                    self.step_selection(if shift { -1 } else { 1 }, cfg, gfx);
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_HOME.0 {
                    if !self.filtered.is_empty() {
                        self.selected_index = 0;
                        self.ensure_selected_visible(cfg);
                        self.sync_highlight_target(cfg, false);
                        self.refresh_thumbnail_and_repaint(cfg, gfx);
                    }
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_END.0 {
                    if !self.filtered.is_empty() {
                        self.selected_index = self.filtered.len() - 1;
                        self.ensure_selected_visible(cfg);
                        self.sync_highlight_target(cfg, false);
                        self.refresh_thumbnail_and_repaint(cfg, gfx);
                    }
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_PRIOR.0 {
                    self.step_selection(-(self.max_visible_rows(cfg) as i32), cfg, gfx);
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_NEXT.0 {
                    self.step_selection(self.max_visible_rows(cfg) as i32, cfg, gfx);
                    return QuickSwitchDelegatedAction::None;
                }

                // AltTabio F4-F9 完整窗口命令热键
                if vk == VK_F4.0 || vk == VK_DELETE.0 || (ctrl && vk == b'W' as u16) {
                    self.search_mode = true;
                    self.execute_row_action_on_index(self.selected_index, RowActionKind::Close, cfg, gfx, trace_id);
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_F5.0 {
                    self.search_mode = true;
                    self.execute_row_action_on_index(self.selected_index, RowActionKind::Minimize, cfg, gfx, trace_id);
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_F6.0 {
                    self.search_mode = true;
                    self.show_cmd_selected(SW_MAXIMIZE, cfg, gfx);
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_F7.0 {
                    self.search_mode = true;
                    self.show_cmd_selected(SW_RESTORE, cfg, gfx);
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_F8.0 {
                    self.search_mode = true;
                    self.execute_row_action_on_index(self.selected_index, RowActionKind::Kill, cfg, gfx, trace_id);
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_F9.0 {
                    self.search_mode = true;
                    self.launch_new_instance_selected(trace_id);
                    return QuickSwitchDelegatedAction::None;
                }

                if (ctrl && (vk == b'A' as u16 || vk == VK_BACK.0)) && !self.search_query.is_empty() {
                    self.search_query.clear();
                    self.apply_filter(cfg);
                    self.sync_highlight_target(cfg, false);
                    self.refresh_thumbnail_and_repaint(cfg, gfx);
                    return QuickSwitchDelegatedAction::None;
                }
                if vk == VK_BACK.0 {
                    self.search_mode = true;
                    self.search_query.pop();
                    self.apply_filter(cfg);
                    self.sync_highlight_target(cfg, false);
                    self.refresh_thumbnail_and_repaint(cfg, gfx);
                    return QuickSwitchDelegatedAction::None;
                }
                if (alt || ctrl) && (b'1' as u16..=b'9' as u16).contains(&vk) {
                    let idx = (vk as u8 - b'1') as usize + self.scroll_offset;
                    if idx < self.filtered.len() {
                        self.selected_index = idx;
                        return self.commit_selected_and_close(trace_id);
                    }
                }
                if vk == VK_SPACE.0 && self.search_query.is_empty() && !self.search_mode {
                    self.search_mode = true;
                    self.refresh_thumbnail_and_repaint(cfg, gfx);
                    return QuickSwitchDelegatedAction::None;
                }
                QuickSwitchDelegatedAction::None
            }
            QS_EVT_CHAR => {
                let Some(ch) = char::from_u32(param as u32) else {
                    return QuickSwitchDelegatedAction::None;
                };
                if ch.is_control() {
                    return QuickSwitchDelegatedAction::None;
                }
                // 非搜索输入态下支持单键窗口操控中枢快捷键：1-9 快切 / M 最小化 / P 置顶 / Z 下一分区 / T 托盘收纳或捞回
                if self.search_query.is_empty() && !self.search_mode {
                    if ('1'..='9').contains(&ch) {
                        let idx = (ch as u8 - b'1') as usize + self.scroll_offset;
                        if idx < self.filtered.len() {
                            self.selected_index = idx;
                            return self.commit_selected_and_close(trace_id);
                        }
                    }
                    if ch == 'm' || ch == 'M' {
                        self.search_mode = true;
                        self.execute_row_action_on_index(self.selected_index, RowActionKind::Minimize, cfg, gfx, trace_id);
                        return QuickSwitchDelegatedAction::None;
                    }
                    if ch == 'p' || ch == 'P' {
                        if let Some(entry) = self.filtered.get(self.selected_index) {
                            if !entry.is_tray_hidden {
                                self.search_mode = true;
                                return QuickSwitchDelegatedAction::TogglePinWindow(entry.info.hwnd.0 as isize);
                            }
                        }
                    }
                    if ch == 'z' || ch == 'Z' {
                        if let Some(entry) = self.filtered.get(self.selected_index) {
                            if !entry.is_tray_hidden {
                                self.search_mode = true;
                                return QuickSwitchDelegatedAction::MoveWindowToNextZone(entry.info.hwnd.0 as isize);
                            }
                        }
                    }
                    if ch == 't' || ch == 'T' {
                        if let Some(entry) = self.filtered.get(self.selected_index).cloned() {
                            if entry.is_tray_hidden {
                                self.close_and_release(trace_id);
                                return QuickSwitchDelegatedAction::RestoreTrayWindowAndClose(entry.info.hwnd.0 as isize);
                            } else {
                                self.search_mode = true;
                                let hwnd_raw = entry.info.hwnd.0 as isize;
                                self.all_entries.retain(|e| e.info.hwnd.0 as isize != hwnd_raw);
                                self.apply_filter(cfg);
                                self.refresh_thumbnail_and_repaint(cfg, gfx);
                                return QuickSwitchDelegatedAction::MinimizeWindowToTray(hwnd_raw);
                            }
                        }
                    }
                    if ch == ' ' {
                        self.search_mode = true;
                        self.refresh_thumbnail_and_repaint(cfg, gfx);
                        return QuickSwitchDelegatedAction::None;
                    }
                }
                self.search_mode = true;
                if self.search_query.chars().count() < 48 {
                    self.search_query.push(ch);
                }
                self.apply_filter(cfg);
                self.sync_highlight_target(cfg, false);
                self.refresh_thumbnail_and_repaint(cfg, gfx);
                QuickSwitchDelegatedAction::None
            }
            QS_EVT_MOUSEMOVE => {
                let (x_dip, y_dip) = self.unpack_dip_coords(param);
                if self.dragging_splitter {
                    let w = self.width_dip().max(400.0);
                    let next_ratio = (x_dip / w).clamp(0.22, 0.58);
                    if (next_ratio - self.list_width_ratio).abs() > 0.002 {
                        self.list_width_ratio = next_ratio;
                        cfg.quick_switch.list_width_ratio = next_ratio;
                        self.refresh_thumbnail_and_repaint(cfg, gfx);
                    }
                    self.set_cursor(QS_CUR_SIZEWE);
                    return QuickSwitchDelegatedAction::None;
                }

                // 检测中央分割线拖拽区（左右各 5 DIP）
                let split_x = self.list_pane_right_dip() + 6.0;
                if y_dip > HEADER_H_DIP && (x_dip - split_x).abs() <= 5.5 {
                    self.set_cursor(QS_CUR_SIZEWE);
                    if self.hovered_row.is_some() || self.hovered_header_btn.is_some() {
                        self.hovered_row = None;
                        self.hovered_action = None;
                        self.hovered_header_btn = None;
                        self.request_repaint();
                    }
                    return QuickSwitchDelegatedAction::None;
                }

                let hbtn = self.hit_test_header_btn(x_dip, y_dip);
                let (row, act) = if hbtn.is_some() {
                    (None, None)
                } else {
                    self.hit_test_list(x_dip, y_dip, cfg)
                };
                let cur = if hbtn.is_some() || row.is_some() || act.is_some() {
                    QS_CUR_HAND
                } else {
                    QS_CUR_ARROW
                };
                self.set_cursor(cur);

                if hbtn != self.hovered_header_btn
                    || row != self.hovered_row
                    || act != self.hovered_action
                {
                    self.hovered_header_btn = hbtn;
                    self.hovered_row = row;
                    self.hovered_action = act;
                    if let Some(r) = row {
                        if r != self.selected_index && r < self.filtered.len() {
                            self.selected_index = r;
                            self.sync_highlight_target(cfg, false);
                            self.update_thumbnail(cfg);
                        }
                    }
                    self.request_repaint();
                }
                QuickSwitchDelegatedAction::None
            }
            QS_EVT_LBUTTONUP => {
                if self.dragging_splitter {
                    self.dragging_splitter = false;
                    unsafe {
                        let _ = ReleaseCapture();
                    }
                    if cfg.quick_switch.remember_bounds {
                        self.persist_window_bounds(cfg, config_dir, trace_id);
                    }
                }
                QuickSwitchDelegatedAction::None
            }
            QS_EVT_LBUTTONDOWN => {
                let (x_dip, y_dip) = self.unpack_dip_coords(param);

                // 1. 顶栏 4 颗微控按钮（原位预览切换 / 复位居中 / 最大化 / 关闭）
                if let Some(hbtn) = self.hit_test_header_btn(x_dip, y_dip) {
                    self.search_mode = true;
                    match hbtn {
                        HeaderBtnKind::ToggleDesktopPreview => {
                            let next = !self.effective_desktop_preview(cfg);
                            self.preview_desktop_pos_override = Some(next);
                            cfg.quick_switch.preview_in_desktop_position = next;
                            self.refresh_thumbnail_and_repaint(cfg, gfx);
                        }
                        HeaderBtnKind::ResetBounds => {
                            self.reset_bounds_to_default(cfg, config_dir, gfx, trace_id);
                        }
                        HeaderBtnKind::ToggleMaximize => {
                            self.toggle_maximize(cfg, gfx);
                        }
                        HeaderBtnKind::CloseCard => {
                            self.close_and_release(trace_id);
                            return QuickSwitchDelegatedAction::Closed;
                        }
                    }
                    return QuickSwitchDelegatedAction::None;
                }

                // 2. 拖动中央双栏分割线
                let split_x = self.list_pane_right_dip() + 6.0;
                if y_dip > HEADER_H_DIP && (x_dip - split_x).abs() <= 5.5 {
                    self.search_mode = true;
                    self.dragging_splitter = true;
                    if let Some(hwnd) = self.card_hwnd {
                        unsafe {
                            let _ = SetCapture(hwnd);
                        }
                    }
                    return QuickSwitchDelegatedAction::None;
                }

                // 3. 列表行与行尾操作按钮
                let (row, act) = self.hit_test_list(x_dip, y_dip, cfg);
                if let Some((r, kind)) = act {
                    self.search_mode = true;
                    self.execute_row_action_on_index(r, kind, cfg, gfx, trace_id);
                    return QuickSwitchDelegatedAction::None;
                }
                if let Some(r) = row {
                    if r < self.filtered.len() {
                        self.selected_index = r;
                        return self.commit_selected_and_close(trace_id);
                    }
                }
                QuickSwitchDelegatedAction::None
            }
            QS_EVT_RBUTTONDOWN => {
                let (x_dip, y_dip) = self.unpack_dip_coords(param);
                let (row, _) = self.hit_test_list(x_dip, y_dip, cfg);
                if let Some(r) = row {
                    if r < self.filtered.len() {
                        self.selected_index = r;
                        self.search_mode = true;
                        self.sync_highlight_target(cfg, true);
                        self.refresh_thumbnail_and_repaint(cfg, gfx);
                        return self.show_row_context_menu(r, cfg, gfx, trace_id);
                    }
                }
                QuickSwitchDelegatedAction::None
            }
            QS_EVT_WHEEL => {
                // 分区滚轮：鼠标仅在卡片内滚动列表，绝不误传给后台软件
                let delta = param as i16;
                self.step_selection(if delta < 0 { 1 } else { -1 }, cfg, gfx);
                QuickSwitchDelegatedAction::None
            }
            QS_EVT_DEACTIVATE => {
                if self.in_context_menu || self.dragging_splitter {
                    return QuickSwitchDelegatedAction::None;
                }
                if self
                    .opened_at
                    .map(|t| t.elapsed().as_millis() < 280)
                    .unwrap_or(false)
                {
                    return QuickSwitchDelegatedAction::None;
                }
                self.close_and_release(trace_id);
                QuickSwitchDelegatedAction::Closed
            }
            _ => QuickSwitchDelegatedAction::None,
        }
    }

    /// 原生 Win32 右键窗口中枢菜单（复刻并超越 AltTabio 右键菜单）
    fn show_row_context_menu(
        &mut self,
        idx: usize,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> QuickSwitchDelegatedAction {
        let (Some(owner), Some(entry)) = (self.card_hwnd, self.filtered.get(idx).cloned()) else {
            return QuickSwitchDelegatedAction::None;
        };
        self.in_context_menu = true;
        let selected_cmd = unsafe {
            let Ok(menu) = CreatePopupMenu() else {
                self.in_context_menu = false;
                return QuickSwitchDelegatedAction::None;
            };
            let append = |id: usize, text: &str| {
                let w = to_wide(text);
                let _ = AppendMenuW(menu, MF_STRING, id, PCWSTR(w.as_ptr()));
            };
            append(1, "切换并激活\tEnter");
            if entry.is_tray_hidden {
                append(11, "从系统托盘恢复\tT");
            } else {
                let _ = AppendMenuW(menu, MF_SEPARATOR, 0, PCWSTR::null());
                append(2, "最小化\tF5");
                append(3, "最大化\tF6");
                append(4, "还原\tF7");
                let _ = AppendMenuW(menu, MF_SEPARATOR, 0, PCWSTR::null());
                append(8, "置顶 / 取消置顶\tP");
                append(9, "移至下一分屏区\tZ");
                append(10, "收纳到系统托盘\tT");
                let _ = AppendMenuW(menu, MF_SEPARATOR, 0, PCWSTR::null());
                append(7, "新开一个实例\tF9");
                append(5, "关闭窗口\tF4");
                append(6, "强制结束进程\tF8");
            }
            let mut pt = POINT::default();
            let _ = GetCursorPos(&mut pt);
            let _ = SetForegroundWindow(owner);
            let cmd = TrackPopupMenu(
                menu,
                TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                pt.x,
                pt.y,
                0,
                owner,
                None,
            );
            let _ = DestroyMenu(menu);
            cmd.0 as usize
        };
        self.in_context_menu = false;

        match selected_cmd {
            1 => self.commit_selected_and_close(trace_id),
            2 => {
                self.execute_row_action_on_index(idx, RowActionKind::Minimize, cfg, gfx, trace_id);
                QuickSwitchDelegatedAction::None
            }
            3 => {
                self.show_cmd_selected(SW_MAXIMIZE, cfg, gfx);
                QuickSwitchDelegatedAction::None
            }
            4 => {
                self.show_cmd_selected(SW_RESTORE, cfg, gfx);
                QuickSwitchDelegatedAction::None
            }
            5 => {
                self.execute_row_action_on_index(idx, RowActionKind::Close, cfg, gfx, trace_id);
                QuickSwitchDelegatedAction::None
            }
            6 => {
                self.execute_row_action_on_index(idx, RowActionKind::Kill, cfg, gfx, trace_id);
                QuickSwitchDelegatedAction::None
            }
            7 => {
                self.launch_new_instance_selected(trace_id);
                QuickSwitchDelegatedAction::None
            }
            8 => QuickSwitchDelegatedAction::TogglePinWindow(entry.info.hwnd.0 as isize),
            9 => QuickSwitchDelegatedAction::MoveWindowToNextZone(entry.info.hwnd.0 as isize),
            10 => {
                let raw = entry.info.hwnd.0 as isize;
                self.all_entries.retain(|e| e.info.hwnd.0 as isize != raw);
                self.apply_filter(cfg);
                self.refresh_thumbnail_and_repaint(cfg, gfx);
                QuickSwitchDelegatedAction::MinimizeWindowToTray(raw)
            }
            11 => {
                self.close_and_release(trace_id);
                QuickSwitchDelegatedAction::RestoreTrayWindowAndClose(entry.info.hwnd.0 as isize)
            }
            _ => QuickSwitchDelegatedAction::None,
        }
    }

    fn show_cmd_selected(
        &mut self,
        cmd: windows::Win32::UI::WindowsAndMessaging::SHOW_WINDOW_CMD,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
    ) {
        let Some(entry) = self.filtered.get(self.selected_index).cloned() else {
            return;
        };
        if entry.is_tray_hidden || !entry.identity.verify_live(entry.info.hwnd) {
            return;
        }
        unsafe {
            let _ = ShowWindowAsync(entry.info.hwnd, cmd);
        }
        for e in &mut self.all_entries {
            if e.info.hwnd == entry.info.hwnd {
                if cmd == SW_MAXIMIZE {
                    e.info.is_maximized = true;
                    e.info.is_minimized = false;
                } else if cmd == SW_RESTORE {
                    e.info.is_maximized = false;
                    e.info.is_minimized = false;
                } else if cmd == SW_MINIMIZE {
                    e.info.is_minimized = true;
                }
            }
        }
        self.apply_filter(cfg);
        self.refresh_thumbnail_and_repaint(cfg, gfx);
    }

    fn launch_new_instance_selected(&self, trace_id: &str) {
        let Some(entry) = self.filtered.get(self.selected_index) else {
            return;
        };
        if entry.info.exe_path.trim().is_empty() {
            return;
        }
        let ok = spawn_process_with_elevation_policy(&entry.info.exe_path, "", "", false);
        wk_info!(
            trace_id,
            "quickSwitch",
            "F9 launch new instance exe='{}' ok={}",
            entry.info.exe_path,
            ok
        );
    }

    fn execute_row_action_on_index(
        &mut self,
        idx: usize,
        kind: RowActionKind,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) {
        let Some(entry) = self.filtered.get(idx).cloned() else {
            return;
        };
        if entry.is_tray_hidden {
            return;
        }
        // 执行危险强杀前严格进行 ProcessIdentity 双重校验，防 PID 复用误杀！
        if kind == RowActionKind::Kill && !entry.identity.verify_live(entry.info.hwnd) {
            return;
        }
        match kind {
            RowActionKind::Minimize => minimize_window_async(entry.info.hwnd),
            RowActionKind::Close => close_window_async(entry.info.hwnd),
            RowActionKind::Kill => {
                let _ = kill_window_process_safe(entry.info.hwnd);
            }
        }
        if matches!(kind, RowActionKind::Close | RowActionKind::Kill) {
            self.all_entries.retain(|e| e.info.hwnd != entry.info.hwnd);
        } else {
            for e in &mut self.all_entries {
                if e.info.hwnd == entry.info.hwnd {
                    e.info.is_minimized = true;
                }
            }
            if !cfg.quick_switch.include_minimized {
                self.all_entries.retain(|e| e.info.hwnd != entry.info.hwnd);
            }
        }
        self.apply_filter(cfg);
        self.sync_highlight_target(cfg, false);
        self.refresh_thumbnail_and_repaint(cfg, gfx);
        wk_info!(
            trace_id,
            "quickSwitch",
            "executed row action {:?} on hwnd=0x{:x}",
            kind,
            entry.info.hwnd.0 as isize
        );
    }

    #[allow(clippy::too_many_arguments)]
    fn open_card(
        &mut self,
        via_alt_tab: bool,
        reverse: bool,
        scope: SwitchScope,
        tray_windows: &[(HWND, String)],
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) {
        let monitors = enumerate_monitors();
        self.monitors_snapshot = monitors.clone();
        let mut cursor = POINT::default();
        let _ = unsafe { GetCursorPos(&mut cursor) };
        let active_mon = monitor_for_point(cursor.x, cursor.y, &monitors)
            .or_else(|| monitors.first().cloned());

        let include_min = match scope {
            SwitchScope::SameApp => true, // 同应用切换强制包含最小化窗，全面超越 Window Hopper
            SwitchScope::GlobalAll => cfg.quick_switch.include_minimized,
        };
        let mut raw_windows = enumerate_user_windows(include_min, &cfg.excluded_apps);

        // 严格尊重「仅当前虚拟桌面」选项
        if cfg.quick_switch.current_desktop_only {
            raw_windows.retain(|w| is_window_on_current_virtual_desktop(w.hwnd));
        }

        // 轨道 B（同应用切换 Alt+`）：按当前前台应用进程名过滤
        self.scope = scope;
        self.scope_app_label.clear();
        if scope == SwitchScope::SameApp {
            let fg = unsafe { GetForegroundWindow() };
            let (_, fg_proc, fg_exe) = get_window_process_info(fg);
            let target_proc = fg_proc.to_ascii_lowercase();
            let target_exe = fg_exe.to_ascii_lowercase();
            if target_proc.is_empty() && target_exe.is_empty() {
                return;
            }
            self.scope_app_label = fg_proc.clone();
            raw_windows.retain(|w| {
                (!target_exe.is_empty() && w.exe_path.to_ascii_lowercase() == target_exe)
                    || (!target_proc.is_empty()
                        && w.process_name.to_ascii_lowercase() == target_proc)
            });
            // 同应用窗口少于 2 个时不弹出打扰
            if raw_windows.len() < 2 {
                wk_debug!(
                    trace_id,
                    "quickSwitch",
                    "same-app switch skipped for '{}' (count={})",
                    self.scope_app_label,
                    raw_windows.len()
                );
                return;
            }
        } else if !cfg.quick_switch.show_all_monitors {
            if let Some(m) = &active_mon {
                raw_windows.retain(|w| w.monitor_id == m.id);
            }
        }

        self.all_entries = raw_windows
            .into_iter()
            .map(|info| {
                let identity = ProcessIdentity::capture(info.pid);
                SwitchEntry {
                    info,
                    identity,
                    is_tray_hidden: false,
                    score: 0,
                }
            })
            .collect();

        // 轨道 A 融合 Tools3000 已收纳到系统托盘的窗口，支持一键捞回！
        if scope == SwitchScope::GlobalAll {
            for (thwnd, title) in tray_windows {
                if unsafe { !IsWindow(*thwnd).as_bool() } {
                    continue;
                }
                let (pid, proc_name, exe_path) = get_window_process_info(*thwnd);
                self.all_entries.push(SwitchEntry {
                    info: WindowInfo {
                        hwnd: *thwnd,
                        pid,
                        title: title.clone(),
                        class_name: String::new(),
                        process_name: proc_name,
                        exe_path,
                        rect: RectI::default(),
                        monitor_id: String::new(),
                        is_maximized: false,
                        is_minimized: true,
                    },
                    identity: ProcessIdentity::capture(pid),
                    is_tray_hidden: true,
                    score: 0,
                });
            }
        }

        self.search_query.clear();
        self.search_mode = !via_alt_tab;
        self.opened_via_alt_tab = via_alt_tab;
        self.hovered_row = None;
        self.hovered_action = None;
        self.hovered_header_btn = None;
        self.dragging_splitter = false;
        self.in_context_menu = false;
        self.is_maximized = false;
        QS_IS_MAXIMIZED.store(false, Ordering::Release);
        self.restore_rect = None;
        self.scroll_offset = 0;
        self.list_width_ratio = cfg.quick_switch.list_width_ratio.clamp(0.22, 0.58);
        self.apply_filter(cfg);

        if via_alt_tab && self.filtered.len() >= 2 {
            self.selected_index = if reverse {
                self.filtered.len() - 1
            } else {
                1
            };
        } else {
            self.selected_index = 0;
        }

        if self.card_hwnd.is_none() {
            self.card_hwnd = create_quick_switch_window(self.control_hwnd);
        }
        let Some(hwnd) = self.card_hwnd else {
            return;
        };

        let work = active_mon
            .as_ref()
            .map(|m| m.work_rect)
            .unwrap_or(RectI::new(0, 0, 1920, 1040));
        let dpi = active_mon.as_ref().map(|m| m.dpi).unwrap_or(96).max(96);
        self.card_dpi = dpi;
        let scale = dpi as f32 / 96.0;

        // 默认 5/8 × 5/8 黄金视野（对标 AltTabio），若用户自定义拖缩过且开启记忆则按跨屏安全记忆恢复！
        let default_w = ((work.width() as f32) * (5.0 / 8.0)).round() as i32;
        let default_h = ((work.height() as f32) * (5.0 / 8.0)).round() as i32;
        let desired_w = if cfg.quick_switch.remember_bounds {
            cfg.quick_switch
                .custom_width_dip
                .map(|w| (w * scale).round() as i32)
                .unwrap_or(default_w)
        } else {
            default_w
        };
        let desired_h = if cfg.quick_switch.remember_bounds {
            cfg.quick_switch
                .custom_height_dip
                .map(|h| (h * scale).round() as i32)
                .unwrap_or(default_h)
        } else {
            default_h
        };
        let min_w = (MIN_WIN_W_DIP * scale).round() as i32;
        let min_h = (MIN_WIN_H_DIP * scale).round() as i32;

        let target_rect = if cfg.quick_switch.remember_bounds && cfg.quick_switch.custom_rel_rect.is_some()
        {
            ensure_window_rect_on_live_monitors(
                &cfg.quick_switch.custom_monitor_id,
                cfg.quick_switch.custom_rel_rect,
                desired_w.max(min_w),
                desired_h.max(min_h),
                active_mon.as_ref(),
                &monitors,
            )
        } else {
            let cw = desired_w.clamp(min_w.min(work.width()), (work.width() - 32).max(min_w));
            let ch = desired_h.clamp(min_h.min(work.height()), (work.height() - 32).max(min_h));
            RectI::from_xywh(
                work.left + (work.width() - cw) / 2,
                work.top + (work.height() - ch) / 2,
                cw,
                ch,
            )
        };

        self.card_width_px = target_rect.width();
        self.card_height_px = target_rect.height();
        QS_TITLEBAR_H_PX.store((HEADER_H_DIP * scale).round() as i32, Ordering::Release);
        QS_RIGHT_CTRL_W_PX.store((150.0 * scale).round() as i32, Ordering::Release);

        self.ensure_selected_visible(cfg);
        self.sync_highlight_target(cfg, true);

        apply_cross_os_popup_chrome(
            hwnd,
            cfg.is_dark_theme(),
            self.card_width_px,
            self.card_height_px,
            (10.0 * scale).round() as i32,
        );
        set_window_layered_alpha(hwnd, 72);
        unsafe {
            let _ = SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                target_rect.left,
                target_rect.top,
                self.card_width_px,
                self.card_height_px,
                SWP_SHOWWINDOW,
            );
            let _ = ShowWindow(hwnd, SW_SHOW);
        }
        let _ = activate_window(hwnd);

        self.open = true;
        self.opened_at = Some(Instant::now());
        self.card_fade_done = false;
        self.last_anim_tick = Some(Instant::now());
        QUICK_SWITCH_IS_OPEN.store(true, Ordering::Release);

        self.icon_cache.clear();
        self.render_target = None;
        if let Some(g) = gfx {
            let actual_dpi = get_window_dpi(hwnd).max(96);
            self.card_dpi = actual_dpi;
            self.render_target = g
                .create_hwnd_target(
                    hwnd,
                    self.card_width_px as u32,
                    self.card_height_px as u32,
                    actual_dpi,
                )
                .ok();
            self.preload_icons(g, cfg.quick_switch.large_icons);
            self.update_thumbnail(cfg);
            self.render(g, cfg);
        }

        wk_info!(
            trace_id,
            "quickSwitch",
            "opened v2 card scope={:?} items={} size={}x{} dpi={}",
            scope,
            self.filtered.len(),
            self.card_width_px,
            self.card_height_px,
            self.card_dpi
        );
    }

    fn sync_client_size_and_resize(&mut self, cfg: &WindowKitConfig, gfx: Option<&SharedGraphics>) {
        let Some(hwnd) = self.card_hwnd else {
            return;
        };
        let mut rc = RECT::default();
        unsafe {
            let _ = GetClientRect(hwnd, &mut rc);
        }
        let w = (rc.right - rc.left).max(320);
        let h = (rc.bottom - rc.top).max(240);
        let dpi = get_window_dpi(hwnd).max(96);
        let scale = dpi as f32 / 96.0;
        self.card_dpi = dpi;
        self.card_width_px = w;
        self.card_height_px = h;
        QS_TITLEBAR_H_PX.store((HEADER_H_DIP * scale).round() as i32, Ordering::Release);
        QS_RIGHT_CTRL_W_PX.store((150.0 * scale).round() as i32, Ordering::Release);
        self.resize_render_target(gfx);
        self.ensure_selected_visible(cfg);
        self.sync_highlight_target(cfg, true);
        self.refresh_thumbnail_and_repaint(cfg, gfx);
    }

    fn resize_render_target(&mut self, gfx: Option<&SharedGraphics>) {
        let Some(hwnd) = self.card_hwnd else {
            return;
        };
        let w = self.card_width_px.max(1) as u32;
        let h = self.card_height_px.max(1) as u32;
        if let Some(rt) = &self.render_target {
            let sz = D2D_SIZE_U { width: w, height: h };
            if unsafe { rt.Resize(&sz) }.is_err() {
                self.render_target = gfx.and_then(|g| g.create_hwnd_target(hwnd, w, h, self.card_dpi).ok());
                self.icon_cache.clear();
                if let Some(g) = gfx {
                    self.preload_icons(g, true);
                }
            }
        } else if let Some(g) = gfx {
            self.render_target = g.create_hwnd_target(hwnd, w, h, self.card_dpi).ok();
        }
    }

    fn toggle_maximize(&mut self, cfg: &WindowKitConfig, gfx: Option<&SharedGraphics>) {
        let Some(hwnd) = self.card_hwnd else {
            return;
        };
        let monitors = enumerate_monitors();
        let mon = monitor_for_hwnd(hwnd, &monitors)
            .or_else(|| monitors.first().cloned())
            .unwrap_or_else(|| MonitorInfo {
                hmonitor: 0,
                id: String::new(),
                device_name: String::new(),
                friendly_name: String::new(),
                physical_rect: RectI::new(0, 0, 1920, 1080),
                work_rect: RectI::new(0, 0, 1920, 1040),
                dpi: 96,
                scale_percent: 100,
                is_primary: true,
            });
        if self.is_maximized {
            let target = self.restore_rect.take().unwrap_or_else(|| {
                let w = ((mon.work_rect.width() as f32) * (5.0 / 8.0)).round() as i32;
                let h = ((mon.work_rect.height() as f32) * (5.0 / 8.0)).round() as i32;
                RectI::from_xywh(
                    mon.work_rect.left + (mon.work_rect.width() - w) / 2,
                    mon.work_rect.top + (mon.work_rect.height() - h) / 2,
                    w,
                    h,
                )
            });
            self.is_maximized = false;
            QS_IS_MAXIMIZED.store(false, Ordering::Release);
            unsafe {
                let _ = SetWindowPos(
                    hwnd,
                    HWND_TOPMOST,
                    target.left,
                    target.top,
                    target.width(),
                    target.height(),
                    SWP_NOACTIVATE,
                );
            }
        } else {
            let mut wr = RECT::default();
            if unsafe { GetWindowRect(hwnd, &mut wr) }.is_ok() {
                self.restore_rect = Some(RectI::new(wr.left, wr.top, wr.right, wr.bottom));
            }
            self.is_maximized = true;
            QS_IS_MAXIMIZED.store(true, Ordering::Release);
            let w = mon.work_rect;
            unsafe {
                let _ = SetWindowPos(
                    hwnd,
                    HWND_TOPMOST,
                    w.left,
                    w.top,
                    w.width(),
                    w.height(),
                    SWP_NOACTIVATE,
                );
            }
        }
        apply_cross_os_popup_chrome(
            hwnd,
            cfg.is_dark_theme(),
            self.card_width_px,
            self.card_height_px,
            if self.is_maximized { 0 } else { (10.0 * self.scale()).round() as i32 },
        );
        self.sync_client_size_and_resize(cfg, gfx);
    }

    fn reset_bounds_to_default(
        &mut self,
        cfg: &mut WindowKitConfig,
        config_dir: &Path,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) {
        let Some(hwnd) = self.card_hwnd else {
            return;
        };
        let monitors = enumerate_monitors();
        let Some(mon) = monitor_for_hwnd(hwnd, &monitors).or_else(|| monitors.first().cloned()) else {
            return;
        };
        self.is_maximized = false;
        QS_IS_MAXIMIZED.store(false, Ordering::Release);
        self.restore_rect = None;
        self.list_width_ratio = 0.34;
        let w = ((mon.work_rect.width() as f32) * (5.0 / 8.0)).round() as i32;
        let h = ((mon.work_rect.height() as f32) * (5.0 / 8.0)).round() as i32;
        let x = mon.work_rect.left + (mon.work_rect.width() - w) / 2;
        let y = mon.work_rect.top + (mon.work_rect.height() - h) / 2;
        unsafe {
            let _ = SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        }
        cfg.quick_switch.custom_width_dip = None;
        cfg.quick_switch.custom_height_dip = None;
        cfg.quick_switch.custom_monitor_id.clear();
        cfg.quick_switch.custom_rel_rect = None;
        cfg.quick_switch.list_width_ratio = 0.34;
        self.write_bounds_payload_and_notify(cfg, config_dir, trace_id);
        self.sync_client_size_and_resize(cfg, gfx);
    }

    fn persist_window_bounds(
        &mut self,
        cfg: &mut WindowKitConfig,
        config_dir: &Path,
        trace_id: &str,
    ) {
        let Some(hwnd) = self.card_hwnd else {
            return;
        };
        let mut wr = RECT::default();
        if unsafe { GetWindowRect(hwnd, &mut wr) }.is_err() {
            return;
        }
        let rect = RectI::new(wr.left, wr.top, wr.right, wr.bottom);
        let monitors = enumerate_monitors();
        let Some(mon) = monitor_for_hwnd(hwnd, &monitors) else {
            return;
        };
        let scale = (mon.dpi as f32 / 96.0).max(1.0);
        cfg.quick_switch.custom_width_dip = Some((rect.width() as f32 / scale).round());
        cfg.quick_switch.custom_height_dip = Some((rect.height() as f32 / scale).round());
        cfg.quick_switch.custom_monitor_id = mon.id.clone();
        cfg.quick_switch.custom_rel_rect =
            NormalizedWorkRect::from_rect_in_work_area(rect, mon.work_rect);
        cfg.quick_switch.list_width_ratio = self.list_width_ratio;
        self.write_bounds_payload_and_notify(cfg, config_dir, trace_id);
    }

    fn write_bounds_payload_and_notify(
        &self,
        cfg: &WindowKitConfig,
        config_dir: &Path,
        trace_id: &str,
    ) {
        let mut root = JsonValue::object();
        if let Some(w) = cfg.quick_switch.custom_width_dip {
            root.insert("customWidthDip", JsonValue::Number(w as f64));
        }
        if let Some(h) = cfg.quick_switch.custom_height_dip {
            root.insert("customHeightDip", JsonValue::Number(h as f64));
        }
        root.insert(
            "listWidthRatio",
            JsonValue::Number(((cfg.quick_switch.list_width_ratio as f64) * 1000.0).round() / 1000.0),
        );
        root.insert(
            "customMonitorId",
            JsonValue::String(cfg.quick_switch.custom_monitor_id.clone()),
        );
        if let Some(rel) = cfg.quick_switch.custom_rel_rect {
            root.insert("customRelRect", rel.to_json());
        }
        let out_path = config_dir.join(FILE_QUICKSWITCH_BOUNDS_RESULT);
        if write_text_file_atomic(&out_path, &root.to_string_pretty()).is_ok() {
            let cls = to_wide(HOST_MESSAGE_WINDOW_CLASS);
            if let Ok(host_hwnd) = unsafe { FindWindowW(PCWSTR(cls.as_ptr()), PCWSTR::null()) } {
                if !host_hwnd.0.is_null() {
                    let msg_id = host_bounds_msg_id();
                    if msg_id != 0 {
                        unsafe {
                            let _ = PostMessageW(host_hwnd, msg_id, WPARAM(1), LPARAM(0));
                        }
                    }
                }
            }
            wk_debug!(trace_id, "quickSwitch", "persisted custom bounds -> {}", out_path.display());
        }
    }

    fn commit_selected_and_close(&mut self, trace_id: &str) -> QuickSwitchDelegatedAction {
        let target = self.filtered.get(self.selected_index).cloned();
        self.open = false;
        QUICK_SWITCH_IS_OPEN.store(false, Ordering::Release);
        QS_CURSOR_KIND.store(QS_CUR_ARROW, Ordering::Release);

        if let Some(entry) = target {
            if entry.is_tray_hidden {
                let raw = entry.info.hwnd.0 as isize;
                self.close_and_release(trace_id);
                return QuickSwitchDelegatedAction::RestoreTrayWindowAndClose(raw);
            }
            let ok = activate_window_or_popup(entry.info.hwnd);
            wk_info!(
                trace_id,
                "quickSwitch",
                "switched to hwnd=0x{:x} proc={} ok={}",
                entry.info.hwnd.0 as isize,
                entry.info.process_name,
                ok
            );
        }
        self.close_and_release(trace_id);
        QuickSwitchDelegatedAction::Closed
    }

    pub fn close_and_release(&mut self, trace_id: &str) {
        self.unregister_thumbnail();
        self.icon_cache.clear();
        self.render_target = None;
        self.all_entries.clear();
        self.filtered.clear();
        self.search_query.clear();
        self.search_mode = false;
        self.hovered_row = None;
        self.hovered_action = None;
        self.hovered_header_btn = None;
        self.dragging_splitter = false;
        self.in_context_menu = false;
        self.card_fade_done = true;
        self.last_anim_tick = None;
        self.open = false;
        QUICK_SWITCH_IS_OPEN.store(false, Ordering::Release);
        QS_CURSOR_KIND.store(QS_CUR_ARROW, Ordering::Release);

        if let Some(hwnd) = self.card_hwnd {
            unsafe {
                let _ = ReleaseCapture();
                if IsWindow(hwnd).as_bool() {
                    let _ = ShowWindow(hwnd, SW_HIDE);
                }
            }
        }
        wk_debug!(trace_id, "quickSwitch", "closed v2 card and released resources");
    }

    pub fn destroy(&mut self) {
        self.close_and_release("destroy");
        if let Some(hwnd) = self.card_hwnd.take() {
            unsafe {
                if IsWindow(hwnd).as_bool() {
                    let _ = DestroyWindow(hwnd);
                }
            }
        }
    }

    fn apply_filter(&mut self, cfg: &WindowKitConfig) {
        let mut matched: Vec<SwitchEntry> = Vec::new();
        for e in &self.all_entries {
            if let Some(score) = score_window_match(
                &self.search_query,
                &e.info.title,
                &e.info.process_name,
                cfg.quick_switch.enable_pinyin_search,
            ) {
                let mut cloned = e.clone();
                cloned.score = score;
                matched.push(cloned);
            }
        }
        if !self.search_query.trim().is_empty() {
            matched.sort_by(|a, b| b.score.cmp(&a.score));
        }
        self.filtered = matched;
        if self.filtered.is_empty() {
            self.selected_index = 0;
            self.scroll_offset = 0;
        } else {
            self.selected_index = self.selected_index.min(self.filtered.len() - 1);
            self.ensure_selected_visible(cfg);
        }
    }

    fn step_selection(&mut self, delta: i32, cfg: &WindowKitConfig, gfx: Option<&SharedGraphics>) {
        if self.filtered.is_empty() {
            return;
        }
        let len = self.filtered.len() as i32;
        let prev_scroll = self.scroll_offset;
        self.selected_index = ((self.selected_index as i32 + delta).rem_euclid(len)) as usize;
        self.ensure_selected_visible(cfg);
        let jumped_far = (self.scroll_offset as isize - prev_scroll as isize).abs() > 2;
        self.sync_highlight_target(cfg, jumped_far);
        self.refresh_thumbnail_and_repaint(cfg, gfx);
    }

    fn ensure_selected_visible(&mut self, cfg: &WindowKitConfig) {
        let max_rows = self.max_visible_rows(cfg);
        if self.selected_index < self.scroll_offset {
            self.scroll_offset = self.selected_index;
        } else if self.selected_index >= self.scroll_offset + max_rows {
            self.scroll_offset = self.selected_index + 1 - max_rows;
        }
    }

    fn refresh_thumbnail_and_repaint(&mut self, cfg: &WindowKitConfig, gfx: Option<&SharedGraphics>) {
        self.update_thumbnail(cfg);
        if let Some(g) = gfx {
            self.render(g, cfg);
        } else {
            self.request_repaint();
        }
    }

    fn request_repaint(&self) {
        if let Some(hwnd) = self.card_hwnd {
            unsafe {
                let _ = InvalidateRect(hwnd, None, false);
            }
        }
    }

    fn set_cursor(&self, kind: u32) {
        let prev = QS_CURSOR_KIND.swap(kind, Ordering::AcqRel);
        if prev != kind {
            unsafe {
                let id = match kind {
                    QS_CUR_HAND => IDC_HAND,
                    QS_CUR_SIZEWE => IDC_SIZEWE,
                    _ => IDC_ARROW,
                };
                if let Ok(c) = LoadCursorW(None, id) {
                    SetCursor(c);
                }
            }
        }
    }

    fn preload_icons(&mut self, gfx: &SharedGraphics, large: bool) {
        let Some(rt_hwnd) = self.render_target.as_ref() else {
            return;
        };
        let Ok(rt): Result<ID2D1RenderTarget, _> = rt_hwnd.cast() else {
            return;
        };
        for entry in self.all_entries.iter().take(36) {
            let key = entry.info.hwnd.0 as isize;
            if self.icon_cache.contains_key(&key) {
                continue;
            }
            if let Some(icon) = get_window_icon_safe(entry.info.hwnd, &entry.info.exe_path, large) {
                if let Some(bmp) = gfx.hicon_to_d2d_bitmap(&rt, icon.hicon) {
                    self.icon_cache.insert(key, bmp);
                }
            }
        }
    }

    fn unregister_thumbnail(&mut self) {
        if self.thumbnail_handle != 0 {
            unsafe {
                let _ = DwmUnregisterThumbnail(self.thumbnail_handle);
            }
            self.thumbnail_handle = 0;
            self.thumbnail_source = 0;
        }
        self.thumbnail_fade_started = None;
    }

    /// 计算右侧 DWM 缩略图放置区域（支持「居中放大预览」与 AltTabio 招牌「桌面原位投影预览」）
    fn compute_thumbnail_dest_rect(
        &self,
        src_hwnd: HWND,
        src_size: SIZE,
        cfg: &WindowKitConfig,
    ) -> RECT {
        let scale = self.scale();
        let stage_left = ((self.list_pane_right_dip() + 22.0) * scale).round() as i32;
        let stage_top = ((HEADER_H_DIP + 42.0) * scale).round() as i32;
        let stage_right = self.card_width_px - (22.0 * scale).round() as i32;
        let stage_bottom = self.card_height_px - (46.0 * scale).round() as i32;
        let stage_w = (stage_right - stage_left).max(64);
        let stage_h = (stage_bottom - stage_top).max(64);

        // 模式 2：桌面原位投影（复刻 AltTabio preview.rs：把真实显示器工作区等比缩放映射到右侧画框内）
        if self.effective_desktop_preview(cfg) {
            if let Some(mon) = monitor_for_hwnd(src_hwnd, &self.monitors_snapshot)
                .or_else(|| self.monitors_snapshot.first().cloned())
            {
                let work = mon.work_rect;
                let ww = work.width().max(1) as f64;
                let wh = work.height().max(1) as f64;
                let fit_ratio = (stage_w as f64 / ww).min(stage_h as f64 / wh);
                let frame_w = (ww * fit_ratio).round() as i32;
                let frame_h = (wh * fit_ratio).round() as i32;
                let frame_l = stage_left + (stage_w - frame_w) / 2;
                let frame_t = stage_top + (stage_h - frame_h) / 2;

                let win_rect = self
                    .filtered
                    .get(self.selected_index)
                    .map(|e| {
                        if e.info.is_minimized {
                            get_window_normal_placement_rect(src_hwnd).unwrap_or(e.info.rect)
                        } else {
                            e.info.rect
                        }
                    })
                    .unwrap_or(work);
                let rel_l = ((win_rect.left - work.left) as f64 / ww).clamp(0.0, 0.92);
                let rel_t = ((win_rect.top - work.top) as f64 / wh).clamp(0.0, 0.92);
                let rel_w = (win_rect.width() as f64 / ww).clamp(0.10, 1.0 - rel_l);
                let rel_h = (win_rect.height() as f64 / wh).clamp(0.10, 1.0 - rel_t);

                let dl = frame_l + (rel_l * frame_w as f64).round() as i32;
                let dt = frame_t + (rel_t * frame_h as f64).round() as i32;
                let dw = (rel_w * frame_w as f64).round().max(32.0) as i32;
                let dh = (rel_h * frame_h as f64).round().max(24.0) as i32;
                return RECT {
                    left: dl,
                    top: dt,
                    right: dl + dw,
                    bottom: dt + dh,
                };
            }
        }

        // 模式 1：居中最大化高清预览
        let sw = src_size.cx.max(1) as f64;
        let sh = src_size.cy.max(1) as f64;
        let ratio = (stage_w as f64 / sw).min(stage_h as f64 / sh).min(1.0);
        let dst_w = (sw * ratio).round().max(32.0) as i32;
        let dst_h = (sh * ratio).round().max(24.0) as i32;
        let dst_left = stage_left + (stage_w - dst_w) / 2;
        let dst_top = stage_top + (stage_h - dst_h) / 2;
        RECT {
            left: dst_left,
            top: dst_top,
            right: dst_left + dst_w,
            bottom: dst_top + dst_h,
        }
    }

    fn update_thumbnail(&mut self, cfg: &WindowKitConfig) {
        if !cfg.quick_switch.show_live_preview || !self.open {
            self.unregister_thumbnail();
            return;
        }
        let (Some(dst_hwnd), Some(entry)) = (self.card_hwnd, self.filtered.get(self.selected_index))
        else {
            self.unregister_thumbnail();
            return;
        };
        if entry.is_tray_hidden {
            self.unregister_thumbnail();
            return;
        }

        let src_hwnd = entry.info.hwnd;
        let src_raw = src_hwnd.0 as isize;
        if src_raw == 0 || src_hwnd == dst_hwnd {
            self.unregister_thumbnail();
            return;
        }

        if self.thumbnail_source != src_raw {
            self.unregister_thumbnail();
            if let Ok(handle) = unsafe { DwmRegisterThumbnail(dst_hwnd, src_hwnd) } {
                self.thumbnail_handle = handle;
                self.thumbnail_source = src_raw;
                self.thumbnail_fade_started = Some(Instant::now());
            } else {
                return;
            }
        }
        if self.thumbnail_handle == 0 {
            return;
        }

        let thumb_opacity = if let Some(started) = self.thumbnail_fade_started {
            let t = (started.elapsed().as_secs_f32() * 1000.0) / ANIM_THUMBNAIL_FADE_MS;
            if t >= 1.0 {
                self.thumbnail_fade_started = None;
                255u8
            } else {
                (90.0 + ease_out_cubic(t) * 165.0).round().clamp(90.0, 255.0) as u8
            }
        } else {
            255u8
        };

        let src_size = unsafe { DwmQueryThumbnailSourceSize(self.thumbnail_handle) }.unwrap_or(SIZE {
            cx: 800,
            cy: 500,
        });
        let dst = self.compute_thumbnail_dest_rect(src_hwnd, src_size, cfg);

        let props = DWM_THUMBNAIL_PROPERTIES {
            dwFlags: DWM_TNP_RECTDESTINATION
                | DWM_TNP_VISIBLE
                | DWM_TNP_OPACITY
                | DWM_TNP_SOURCECLIENTAREAONLY,
            rcDestination: dst,
            rcSource: RECT::default(),
            opacity: thumb_opacity,
            fVisible: windows::Win32::Foundation::TRUE,
            fSourceClientAreaOnly: windows::Win32::Foundation::FALSE,
        };
        unsafe {
            let _ = DwmUpdateThumbnailProperties(self.thumbnail_handle, &props);
        }
    }

    fn unpack_dip_coords(&self, lparam: isize) -> (f32, f32) {
        let px_x = (lparam & 0xFFFF) as i16 as f32;
        let px_y = ((lparam >> 16) & 0xFFFF) as i16 as f32;
        let s = self.scale();
        (px_x / s, px_y / s)
    }

    fn header_btn_rects_dip(&self) -> [(HeaderBtnKind, [f32; 4]); 4] {
        let w = self.width_dip();
        let btn_w = 32.0;
        let btn_h = 24.0;
        let y0 = (HEADER_H_DIP - btn_h) * 0.5;
        let x_close = w - 10.0 - btn_w;
        let x_max = x_close - 4.0 - btn_w;
        let x_reset = x_max - 4.0 - btn_w;
        let x_pos = x_reset - 4.0 - btn_w;
        [
            (HeaderBtnKind::ToggleDesktopPreview, [x_pos, y0, x_pos + btn_w, y0 + btn_h]),
            (HeaderBtnKind::ResetBounds, [x_reset, y0, x_reset + btn_w, y0 + btn_h]),
            (HeaderBtnKind::ToggleMaximize, [x_max, y0, x_max + btn_w, y0 + btn_h]),
            (HeaderBtnKind::CloseCard, [x_close, y0, x_close + btn_w, y0 + btn_h]),
        ]
    }

    fn hit_test_header_btn(&self, x_dip: f32, y_dip: f32) -> Option<HeaderBtnKind> {
        if y_dip > HEADER_H_DIP {
            return None;
        }
        for (kind, r) in self.header_btn_rects_dip() {
            if x_dip >= r[0] && x_dip <= r[2] && y_dip >= r[1] && y_dip <= r[3] {
                return Some(kind);
            }
        }
        None
    }

    fn hit_test_list(
        &self,
        x_dip: f32,
        y_dip: f32,
        cfg: &WindowKitConfig,
    ) -> (Option<usize>, Option<(usize, RowActionKind)>) {
        let list_left = 12.0;
        let list_right = self.list_pane_right_dip();
        let list_top = self.list_top_dip();
        let list_bottom = self.list_bottom_dip();
        let row_h = self.row_height_dip(cfg);
        let max_rows = self.max_visible_rows(cfg);
        if x_dip < list_left || x_dip > list_right || y_dip < list_top || y_dip > list_bottom {
            return (None, None);
        }
        let rel_row = ((y_dip - list_top) / row_h).floor() as usize;
        let actual_idx = self.scroll_offset + rel_row;
        if rel_row >= max_rows || actual_idx >= self.filtered.len() {
            return (None, None);
        }

        if let Some(entry) = self.filtered.get(actual_idx) {
            if !entry.is_tray_hidden {
                let btn_size = 24.0;
                let btn_gap = 4.0;
                let kill_x = list_right - 8.0 - btn_size;
                let close_x = kill_x - btn_gap - btn_size;
                let min_x = close_x - btn_gap - btn_size;
                if x_dip >= min_x && x_dip <= min_x + btn_size {
                    return (Some(actual_idx), Some((actual_idx, RowActionKind::Minimize)));
                }
                if x_dip >= close_x && x_dip <= close_x + btn_size {
                    return (Some(actual_idx), Some((actual_idx, RowActionKind::Close)));
                }
                if x_dip >= kill_x && x_dip <= kill_x + btn_size {
                    return (Some(actual_idx), Some((actual_idx, RowActionKind::Kill)));
                }
            }
        }
        (Some(actual_idx), None)
    }

    fn render(&mut self, gfx: &SharedGraphics, cfg: &WindowKitConfig) {
        let Some(rt) = self.render_target.as_ref() else {
            return;
        };

        let w_dip = self.width_dip();
        let h_dip = self.height_dip();
        let dark = cfg.is_dark_theme();
        let accent = parse_hex_color(&cfg.accent_color, [0.23, 0.51, 0.96]);
        let row_h = self.row_height_dip(cfg);
        let max_rows = self.max_visible_rows(cfg);
        let list_left = 12.0;
        let list_right = self.list_pane_right_dip();
        let list_top = self.list_top_dip();
        let desktop_pos_mode = self.effective_desktop_preview(cfg);

        let title_fmt = gfx.ui_text_format(13.5, false, false).ok();
        let stage_title_fmt = gfx.ui_text_format(13.5, true, false).ok();
        let sub_fmt = gfx.mono_text_format(12.0, false, false).ok();
        let badge_fmt = gfx.mono_text_format(12.0, true, true).ok();
        let search_fmt = gfx.ui_text_format(13.5, false, false).ok();
        let footer_fmt = gfx.ui_text_format(12.0, false, false).ok();
        let empty_fmt = gfx.ui_text_format(13.5, false, true).ok();

        unsafe {
            rt.BeginDraw();

            let bg_col = if dark { rgba(0.078, 0.090, 0.118, 1.0) } else { rgba(0.962, 0.970, 0.985, 1.0) };
            let surface_col = if dark { rgba(0.118, 0.136, 0.174, 1.0) } else { rgba(1.0, 1.0, 1.0, 1.0) };
            let hover_col = if dark { rgba(1.0, 1.0, 1.0, 0.07) } else { rgba(0.08, 0.12, 0.22, 0.06) };
            let border_col = if dark { rgba(1.0, 1.0, 1.0, 0.11) } else { rgba(0.08, 0.12, 0.20, 0.13) };
            let rim_highlight = if dark { rgba(1.0, 1.0, 1.0, 0.20) } else { rgba(0.08, 0.12, 0.20, 0.18) };
            let text_primary = if dark { rgba(0.955, 0.972, 1.0, 1.0) } else { rgba(0.085, 0.105, 0.155, 1.0) };
            let text_muted = if dark { rgba(0.64, 0.71, 0.81, 1.0) } else { rgba(0.38, 0.44, 0.54, 1.0) };
            let white = rgba(1.0, 1.0, 1.0, 0.98);

            rt.Clear(Some(&bg_col));

            let (
                Ok(b_surface),
                Ok(b_hover),
                Ok(b_border),
                Ok(b_rim),
                Ok(b_accent_fill),
                Ok(b_accent_stroke),
                Ok(b_text),
                Ok(b_muted),
                Ok(b_white),
                Ok(b_danger),
                Ok(b_warn),
            ) = (
                rt.CreateSolidColorBrush(&surface_col, None),
                rt.CreateSolidColorBrush(&hover_col, None),
                rt.CreateSolidColorBrush(&border_col, None),
                rt.CreateSolidColorBrush(&rim_highlight, None),
                rt.CreateSolidColorBrush(&rgb_a(accent, if dark { 0.23 } else { 0.14 }), None),
                rt.CreateSolidColorBrush(&rgb_a(accent, 0.96), None),
                rt.CreateSolidColorBrush(&text_primary, None),
                rt.CreateSolidColorBrush(&text_muted, None),
                rt.CreateSolidColorBrush(&white, None),
                rt.CreateSolidColorBrush(&rgba(0.90, 0.27, 0.31, 0.94), None),
                rt.CreateSolidColorBrush(&rgba(0.95, 0.62, 0.08, 0.94), None),
            ) else {
                let _ = rt.EndDraw(None, None);
                return;
            };

            if cfg.quick_switch.visible_borders {
                let card_outline = rect_f(0.5, 0.5, w_dip - 0.5, h_dip - 0.5);
                rt.DrawRoundedRectangle(&rounded_rect(card_outline, 10.0), &b_rim, 1.0, None);
            }

            // ── 1. 极简 34 DIP 顶部可拖动微控栏（无膨胀顶栏，直接最大化双栏视野） ──
            if let Some(fmt) = &footer_fmt {
                let header_label = match self.scope {
                    SwitchScope::SameApp => format!(
                        "同应用切换 · {} ({})  ·  拖动顶栏移位 / 边缘可自由缩放",
                        self.scope_app_label,
                        self.filtered.len()
                    ),
                    SwitchScope::GlobalAll => format!(
                        "Quick Switch ({})  ·  直接打字拼音筛选  ·  右键 / F4-F9 操控窗口",
                        self.filtered.len()
                    ),
                };
                if let Some(layout) =
                    gfx.create_trimmed_text_layout(&header_label, fmt, (w_dip - 172.0).max(80.0), 20.0)
                {
                    rt.DrawTextLayout(
                        D2D_POINT_2F { x: 14.0, y: 9.0 },
                        &layout,
                        &b_muted,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    );
                }
            }

            // 右上角 4 颗纯矢量微控按钮：桌面原位预览 / 复位居中 / 最大化 / 关闭
            for (kind, r_arr) in self.header_btn_rects_dip() {
                let r = rect_f(r_arr[0], r_arr[1], r_arr[2], r_arr[3]);
                let hov = self.hovered_header_btn == Some(kind);
                let active_toggle = kind == HeaderBtnKind::ToggleDesktopPreview && desktop_pos_mode;
                if hov && kind == HeaderBtnKind::CloseCard {
                    rt.FillRoundedRectangle(&rounded_rect(r, 5.0), &b_danger);
                } else if active_toggle {
                    rt.FillRoundedRectangle(&rounded_rect(r, 5.0), &b_accent_fill);
                    rt.DrawRoundedRectangle(&rounded_rect(r, 5.0), &b_accent_stroke, 1.0, None);
                } else if hov {
                    rt.FillRoundedRectangle(&rounded_rect(r, 5.0), &b_hover);
                }
                let cx = (r.left + r.right) * 0.5;
                let cy = (r.top + r.bottom) * 0.5;
                let ink = if hov && kind == HeaderBtnKind::CloseCard {
                    &b_white
                } else if active_toggle {
                    &b_accent_stroke
                } else {
                    &b_muted
                };
                match kind {
                    HeaderBtnKind::ToggleDesktopPreview => {
                        // 屏幕外框 + 内部窗口小方框（象征桌面原位投影）
                        rt.DrawRectangle(&rect_f(cx - 6.0, cy - 4.5, cx + 6.0, cy + 4.5), ink, 1.2, None);
                        rt.FillRectangle(&rect_f(cx - 4.0, cy - 2.5, cx + 1.0, cy + 2.0), ink);
                    }
                    HeaderBtnKind::ResetBounds => {
                        // 居中复位标靶（十字准星 + 小框）
                        rt.DrawRectangle(&rect_f(cx - 4.5, cy - 4.5, cx + 4.5, cy + 4.5), ink, 1.2, None);
                        rt.DrawLine(D2D_POINT_2F { x: cx - 7.0, y: cy }, D2D_POINT_2F { x: cx - 4.5, y: cy }, ink, 1.2, None);
                        rt.DrawLine(D2D_POINT_2F { x: cx + 4.5, y: cy }, D2D_POINT_2F { x: cx + 7.0, y: cy }, ink, 1.2, None);
                    }
                    HeaderBtnKind::ToggleMaximize => {
                        rt.DrawRectangle(&rect_f(cx - 4.5, cy - 4.5, cx + 4.5, cy + 4.5), ink, 1.3, None);
                    }
                    HeaderBtnKind::CloseCard => {
                        rt.DrawLine(D2D_POINT_2F { x: cx - 4.2, y: cy - 4.2 }, D2D_POINT_2F { x: cx + 4.2, y: cy + 4.2 }, ink, 1.4, None);
                        rt.DrawLine(D2D_POINT_2F { x: cx + 4.2, y: cy - 4.2 }, D2D_POINT_2F { x: cx - 4.2, y: cy + 4.2 }, ink, 1.4, None);
                    }
                }
            }

            // ── 2. 左侧窗口列表（弹性宽度 + 144Hz 高亮胶囊 + 拼音命中高亮 + 悬停按钮） ──
            let visible_end = (self.scroll_offset + max_rows).min(self.filtered.len());
            if !self.filtered.is_empty()
                && self.selected_index >= self.scroll_offset
                && self.selected_index < visible_end
            {
                let hy0 = self.highlight_y_dip;
                let hi_rect = rect_f(list_left, hy0 + 2.0, list_right, hy0 + row_h - 2.0);
                rt.FillRoundedRectangle(&rounded_rect(hi_rect, 8.0), &b_accent_fill);
                rt.DrawRoundedRectangle(&rounded_rect(hi_rect, 8.0), &b_accent_stroke, 1.25, None);
                let pill_bar = rect_f(list_left + 3.0, hy0 + 9.0, list_left + 6.0, hy0 + row_h - 9.0);
                rt.FillRoundedRectangle(&rounded_rect(pill_bar, 1.5), &b_accent_stroke);
            }

            if self.filtered.is_empty() {
                let empty_box = rect_f(list_left, list_top + 40.0, list_right, list_top + 120.0);
                if let Some(fmt) = &empty_fmt {
                    let msg = to_wide("未匹配到窗口 (按 Esc 重置)");
                    rt.DrawText(
                        &msg[..msg.len().saturating_sub(1)],
                        fmt,
                        &empty_box,
                        &b_muted,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        DWRITE_MEASURING_MODE_NATURAL,
                    );
                }
            }

            let hl_ranges_enabled = !self.search_query.trim().is_empty();
            let compact = cfg.quick_switch.compact_density;
            let icon_sz = if cfg.quick_switch.large_icons && !compact {
                26.0
            } else if compact {
                20.0
            } else {
                22.0
            };

            for (slot, idx) in (self.scroll_offset..visible_end).enumerate() {
                let entry = &self.filtered[idx];
                let y0 = list_top + (slot as f32) * row_h;
                let row_rect = rect_f(list_left, y0 + 2.0, list_right, y0 + row_h - 2.0);
                let is_sel = idx == self.selected_index;
                let is_hov = self.hovered_row == Some(idx);

                if is_hov && !is_sel {
                    rt.FillRoundedRectangle(&rounded_rect(row_rect, 8.0), &b_hover);
                }

                let icon_y = y0 + (row_h - icon_sz) * 0.5;
                let icon_rect = rect_f(list_left + 12.0, icon_y, list_left + 12.0 + icon_sz, icon_y + icon_sz);
                if let Some(bmp) = self.icon_cache.get(&(entry.info.hwnd.0 as isize)) {
                    rt.DrawBitmap(
                        bmp,
                        Some(&icon_rect),
                        if entry.info.is_minimized { 0.60 } else { 1.0 },
                        windows::Win32::Graphics::Direct2D::D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                        None,
                    );
                } else {
                    rt.FillRoundedRectangle(&rounded_rect(icon_rect, 5.0), &b_border);
                }

                let text_left = list_left + 12.0 + icon_sz + 10.0;
                let text_right = if is_hov && !entry.is_tray_hidden {
                    list_right - 92.0
                } else if cfg.quick_switch.show_numbers && slot < 9 {
                    list_right - 36.0
                } else {
                    list_right - 10.0
                };
                let text_w = (text_right - text_left).max(40.0);

                let show_sub = cfg.quick_switch.show_process_badge || cfg.quick_switch.show_app_names || entry.is_tray_hidden;
                if let Some(fmt) = &title_fmt {
                    let title_y = if !show_sub {
                        y0 + (row_h - 18.0) * 0.5
                    } else if compact {
                        y0 + 3.0
                    } else {
                        y0 + 6.0
                    };
                    if let Some(layout) =
                        gfx.create_trimmed_text_layout(&entry.info.title, fmt, text_w, 20.0)
                    {
                        if hl_ranges_enabled {
                            let ranges = find_title_highlight_utf16_ranges(
                                &self.search_query,
                                &entry.info.title,
                                cfg.quick_switch.enable_pinyin_search,
                            );
                            for (start, len) in ranges {
                                let tr = DWRITE_TEXT_RANGE {
                                    startPosition: start,
                                    length: len,
                                };
                                let _ = layout.SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, tr);
                                let _ = layout.SetDrawingEffect(&b_accent_stroke, tr);
                            }
                        }
                        rt.DrawTextLayout(
                            D2D_POINT_2F { x: text_left, y: title_y },
                            &layout,
                            &b_text,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        );
                    }
                }

                if show_sub {
                    if let Some(fmt) = &sub_fmt {
                        let sub = if entry.is_tray_hidden {
                            format!("{}  [TRAY]", entry.info.process_name)
                        } else if entry.info.is_minimized {
                            format!("{}  [MIN]", entry.info.process_name)
                        } else if entry.info.is_maximized {
                            format!("{}  [MAX]", entry.info.process_name)
                        } else {
                            entry.info.process_name.clone()
                        };
                        let sub_y = if compact { y0 + 20.0 } else { y0 + 25.0 };
                        if let Some(layout) = gfx.create_trimmed_text_layout(&sub, fmt, text_w, 16.0) {
                            rt.DrawTextLayout(
                                D2D_POINT_2F { x: text_left, y: sub_y },
                                &layout,
                                if entry.is_tray_hidden { &b_accent_stroke } else { &b_muted },
                                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                            );
                        }
                    }
                }

                // 悬停快捷操作按钮 / 1-9 快选胶囊
                if is_hov && !entry.is_tray_hidden {
                    let btn_size = 24.0;
                    let btn_gap = 4.0;
                    let kill_x = list_right - 8.0 - btn_size;
                    let close_x = kill_x - btn_gap - btn_size;
                    let min_x = close_x - btn_gap - btn_size;
                    let by = y0 + (row_h - btn_size) * 0.5;
                    for (bx, kind) in [
                        (min_x, RowActionKind::Minimize),
                        (close_x, RowActionKind::Close),
                        (kill_x, RowActionKind::Kill),
                    ] {
                        let br = rect_f(bx, by, bx + btn_size, by + btn_size);
                        let active_btn = self.hovered_action == Some((idx, kind));
                        let bg = match (active_btn, kind) {
                            (true, RowActionKind::Minimize) => &b_accent_stroke,
                            (true, RowActionKind::Close) => &b_danger,
                            (true, RowActionKind::Kill) => &b_warn,
                            (false, _) => &b_hover,
                        };
                        rt.FillRoundedRectangle(&rounded_rect(br, 6.0), bg);
                        if !active_btn {
                            rt.DrawRoundedRectangle(&rounded_rect(br, 6.0), &b_border, 1.0, None);
                        }
                        let fg_brush = if active_btn { &b_white } else { &b_text };
                        let cx = bx + btn_size * 0.5;
                        let cy = by + btn_size * 0.5;
                        match kind {
                            RowActionKind::Minimize => {
                                rt.DrawLine(D2D_POINT_2F { x: cx - 4.8, y: cy + 2.0 }, D2D_POINT_2F { x: cx + 4.8, y: cy + 2.0 }, fg_brush, 1.6, None);
                            }
                            RowActionKind::Close => {
                                rt.DrawLine(D2D_POINT_2F { x: cx - 4.0, y: cy - 4.0 }, D2D_POINT_2F { x: cx + 4.0, y: cy + 4.0 }, fg_brush, 1.6, None);
                                rt.DrawLine(D2D_POINT_2F { x: cx + 4.0, y: cy - 4.0 }, D2D_POINT_2F { x: cx - 4.0, y: cy + 4.0 }, fg_brush, 1.6, None);
                            }
                            RowActionKind::Kill => {
                                let ring = D2D1_ELLIPSE {
                                    point: D2D_POINT_2F { x: cx, y: cy },
                                    radiusX: 4.8,
                                    radiusY: 4.8,
                                };
                                rt.DrawEllipse(&ring, fg_brush, 1.5, None);
                                rt.DrawLine(D2D_POINT_2F { x: cx, y: cy - 3.0 }, D2D_POINT_2F { x: cx, y: cy + 1.0 }, fg_brush, 1.5, None);
                            }
                        }
                    }
                } else if cfg.quick_switch.show_numbers && slot < 9 {
                    let kr = rect_f(
                        list_right - 28.0,
                        y0 + (row_h - 21.0) * 0.5,
                        list_right - 7.0,
                        y0 + (row_h + 21.0) * 0.5,
                    );
                    rt.FillRoundedRectangle(&rounded_rect(kr, 5.0), &b_surface);
                    rt.DrawRoundedRectangle(&rounded_rect(kr, 5.0), if is_sel { &b_accent_stroke } else { &b_border }, 1.0, None);
                    if let Some(fmt) = &badge_fmt {
                        let num = to_wide(&(slot + 1).to_string());
                        rt.DrawText(
                            &num[..num.len().saturating_sub(1)],
                            fmt,
                            &kr,
                            if is_sel { &b_accent_stroke } else { &b_muted },
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                            DWRITE_MEASURING_MODE_NATURAL,
                        );
                    }
                }
            }

            // 极细滚动条
            if self.filtered.len() > max_rows {
                let track_top = list_top + 4.0;
                let track_h = (max_rows as f32) * row_h - 8.0;
                let ratio = (max_rows as f32) / (self.filtered.len() as f32);
                let thumb_h = (track_h * ratio).max(24.0);
                let max_scroll = (self.filtered.len() - max_rows) as f32;
                let scroll_t = (self.scroll_offset as f32 / max_scroll).clamp(0.0, 1.0);
                let thumb_y = track_top + (track_h - thumb_h) * scroll_t;
                rt.FillRoundedRectangle(
                    &rounded_rect(rect_f(list_right + 2.0, thumb_y, list_right + 4.5, thumb_y + thumb_h), 1.25),
                    &b_border,
                );
            }

            // ── 2b. 底部零跳动浮现中文拼音搜索胶囊（复刻 AltTabio 底部搜索不挤压列表顶部！） ──
            if self.show_bottom_search_bar() {
                let sb_top = h_dip - BOTTOM_SEARCH_H_DIP - 8.0;
                let sb_rect = rect_f(list_left, sb_top, list_right, h_dip - 10.0);
                rt.FillRoundedRectangle(&rounded_rect(sb_rect, 8.0), &b_surface);
                rt.DrawRoundedRectangle(&rounded_rect(sb_rect, 8.0), &b_accent_stroke, 1.4, None);
                // 放大镜矢量图标
                let lens = D2D1_ELLIPSE {
                    point: D2D_POINT_2F { x: sb_rect.left + 15.0, y: (sb_rect.top + sb_rect.bottom) * 0.5 - 1.0 },
                    radiusX: 4.8,
                    radiusY: 4.8,
                };
                rt.DrawEllipse(&lens, &b_accent_stroke, 1.5, None);
                rt.DrawLine(
                    D2D_POINT_2F { x: lens.point.x + 3.5, y: lens.point.y + 3.5 },
                    D2D_POINT_2F { x: lens.point.x + 7.0, y: lens.point.y + 7.0 },
                    &b_accent_stroke,
                    1.6,
                    None,
                );
                if let Some(fmt) = &search_fmt {
                    let display = if self.search_query.is_empty() {
                        "输入标题 / 进程 / 拼音首字母 (wx, vsc)...".to_string()
                    } else {
                        format!("{}|", self.search_query)
                    };
                    if let Some(layout) = gfx.create_trimmed_text_layout(
                        &display,
                        fmt,
                        (sb_rect.right - sb_rect.left - 38.0).max(40.0),
                        24.0,
                    ) {
                        rt.DrawTextLayout(
                            D2D_POINT_2F { x: sb_rect.left + 29.0, y: sb_rect.top + 8.0 },
                            &layout,
                            if self.search_query.is_empty() { &b_muted } else { &b_text },
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        );
                    }
                }
            }

            // ── 3. 可拖动中央双栏分割线（中间带微妙抓手竖条） ──
            let split_x = list_right + 6.0;
            let split_cy = (list_top + h_dip - 12.0) * 0.5;
            rt.FillRoundedRectangle(
                &rounded_rect(rect_f(split_x - 1.5, split_cy - 18.0, split_x + 1.5, split_cy + 18.0), 1.5),
                if self.dragging_splitter { &b_accent_stroke } else { &b_border },
            );

            // ── 4. 右侧 DWM 实时预览舞台（支持「桌面原位投影」画框轮廓 + 底部操控条） ──
            let stage_left = list_right + 12.0;
            let stage_outer = rect_f(stage_left, list_top, w_dip - 12.0, h_dip - 12.0);
            rt.FillRoundedRectangle(&rounded_rect(stage_outer, 10.0), &b_surface);
            rt.DrawRoundedRectangle(&rounded_rect(stage_outer, 10.0), &b_border, 1.0, None);

            if let Some(sel) = self.filtered.get(self.selected_index) {
                let header_left = stage_left + 14.0;
                let header_w = (w_dip - 26.0 - header_left).max(80.0);
                if let Some(fmt) = &stage_title_fmt {
                    if let Some(layout) =
                        gfx.create_trimmed_text_layout(&sel.info.title, fmt, header_w, 22.0)
                    {
                        rt.DrawTextLayout(
                            D2D_POINT_2F { x: header_left, y: list_top + 8.0 },
                            &layout,
                            &b_text,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        );
                    }
                }
                rt.DrawLine(
                    D2D_POINT_2F { x: stage_left + 12.0, y: list_top + 32.0 },
                    D2D_POINT_2F { x: w_dip - 24.0, y: list_top + 32.0 },
                    &b_border,
                    1.0,
                    None,
                );

                // 若处于「桌面原位预览」模式，在预览区背景绘制等比虚拟显示器边框，清晰凸显窗口相对位置！
                if desktop_pos_mode && !sel.is_tray_hidden {
                    if let Some(mon) = monitor_for_hwnd(sel.info.hwnd, &self.monitors_snapshot)
                        .or_else(|| self.monitors_snapshot.first().cloned())
                    {
                        let sl = stage_left + 10.0;
                        let st = list_top + 38.0;
                        let sw = (w_dip - 22.0 - sl).max(60.0);
                        let sh = (h_dip - 46.0 - st).max(60.0);
                        let ww = mon.work_rect.width().max(1) as f32;
                        let wh = mon.work_rect.height().max(1) as f32;
                        let ratio = (sw / ww).min(sh / wh);
                        let fw = ww * ratio;
                        let fh = wh * ratio;
                        let fl = sl + (sw - fw) * 0.5;
                        let ft = st + (sh - fh) * 0.5;
                        let desktop_box = rect_f(fl, ft, fl + fw, ft + fh);
                        rt.FillRoundedRectangle(&rounded_rect(desktop_box, 6.0), &b_hover);
                        rt.DrawRoundedRectangle(&rounded_rect(desktop_box, 6.0), &b_border, 1.0, None);
                    }
                } else if sel.is_tray_hidden {
                    if let Some(fmt) = &empty_fmt {
                        let msg = to_wide("此窗口已收纳至系统托盘 · 回车或点击一键恢复并激活");
                        rt.DrawText(
                            &msg[..msg.len().saturating_sub(1)],
                            fmt,
                            &rect_f(stage_left + 20.0, list_top + 60.0, w_dip - 20.0, h_dip - 60.0),
                            &b_muted,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                            DWRITE_MEASURING_MODE_NATURAL,
                        );
                    }
                }

                // 底部状态与快捷键提示条
                if let Some(fmt) = &footer_fmt {
                    let tag = if sel.is_tray_hidden {
                        " [已收纳托盘]"
                    } else if sel.info.is_minimized {
                        " [最小化]"
                    } else if sel.info.is_maximized {
                        " [最大化]"
                    } else {
                        ""
                    };
                    let mode_tag = if desktop_pos_mode { "原位预览" } else { "居中预览" };
                    let meta = format!(
                        "{}{}  ·  {}  ·  P 置顶  Z 分区  T 托盘  F4 关窗  F9 新开",
                        sel.info.process_name, tag, mode_tag
                    );
                    if let Some(layout) = gfx.create_trimmed_text_layout(&meta, fmt, header_w, 20.0) {
                        rt.DrawTextLayout(
                            D2D_POINT_2F { x: header_left, y: h_dip - 36.0 },
                            &layout,
                            &b_muted,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        );
                    }
                }
            }

            let _ = rt.EndDraw(None, None);
        }
    }
}

impl Drop for QuickSwitchManager {
    fn drop(&mut self) {
        self.destroy();
    }
}

#[inline]
fn async_down(vk: u16) -> bool {
    unsafe { (GetAsyncKeyState(vk as i32) as u16 & 0x8000) != 0 }
}

fn ensure_quick_switch_class() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(QUICK_SWITCH_WINDOW_CLASS);
        let arrow = LoadCursorW(None, IDC_ARROW).unwrap_or_default();
        let wc = WNDCLASSEXW {
            cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
            lpfnWndProc: Some(quick_switch_wnd_proc),
            hInstance: hinst,
            hCursor: arrow,
            lpszClassName: PCWSTR(cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&wc);
    });
}

fn create_quick_switch_window(control_hwnd: HWND) -> Option<HWND> {
    ensure_quick_switch_class();
    unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(QUICK_SWITCH_WINDOW_CLASS);
        let title = to_wide("Tools3000 Quick Switch");
        let hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            PCWSTR(cls.as_ptr()),
            PCWSTR(title.as_ptr()),
            WS_POPUP,
            0,
            0,
            960,
            580,
            None,
            None,
            hinst,
            None,
        )
        .ok()?;
        let _ = SetWindowLongPtrW(hwnd, GWLP_USERDATA, control_hwnd.0 as isize);
        let _ = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        Some(hwnd)
    }
}

unsafe extern "system" fn quick_switch_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    let ctrl_raw = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    let post_ctrl = |evt: usize, param: isize| {
        if ctrl_raw != 0 {
            let _ = PostMessageW(
                HWND(ctrl_raw as *mut c_void),
                WM_WK_INTERNAL_QUICK_SWITCH_KEY,
                WPARAM(evt),
                LPARAM(param),
            );
        }
    };

    match msg {
        // 标准 Windows 窗口原生 8 向边角缩放 + 顶栏拖移（HTCAPTION）
        WM_NCHITTEST => {
            if !QS_IS_MAXIMIZED.load(Ordering::Acquire) {
                let sx = (lparam.0 & 0xFFFF) as i16 as i32;
                let sy = ((lparam.0 >> 16) & 0xFFFF) as i16 as i32;
                let mut wr = RECT::default();
                if GetWindowRect(hwnd, &mut wr).is_ok() {
                    let left = sx - wr.left < RESIZE_EDGE_PX;
                    let right = wr.right - sx < RESIZE_EDGE_PX;
                    let top = sy - wr.top < RESIZE_EDGE_PX;
                    let bottom = wr.bottom - sy < RESIZE_EDGE_PX;
                    if top && left {
                        return LRESULT(HTTOPLEFT as isize);
                    }
                    if top && right {
                        return LRESULT(HTTOPRIGHT as isize);
                    }
                    if bottom && left {
                        return LRESULT(HTBOTTOMLEFT as isize);
                    }
                    if bottom && right {
                        return LRESULT(HTBOTTOMRIGHT as isize);
                    }
                    if left {
                        return LRESULT(HTLEFT as isize);
                    }
                    if right {
                        return LRESULT(HTRIGHT as isize);
                    }
                    if top {
                        return LRESULT(HTTOP as isize);
                    }
                    if bottom {
                        return LRESULT(HTBOTTOM as isize);
                    }
                    let tb_h = QS_TITLEBAR_H_PX.load(Ordering::Acquire).max(28);
                    let r_ctl = QS_RIGHT_CTRL_W_PX.load(Ordering::Acquire).max(120);
                    if sy - wr.top < tb_h && sx < wr.right - r_ctl {
                        return LRESULT(HTCAPTION as isize);
                    }
                }
            }
            return LRESULT(HTCLIENT as isize);
        }
        WM_GETMINMAXINFO => {
            if lparam.0 != 0 {
                let mmi = &mut *(lparam.0 as *mut MINMAXINFO);
                let scale = (get_window_dpi(hwnd).max(96) as f32) / 96.0;
                mmi.ptMinTrackSize.x = (MIN_WIN_W_DIP * scale).round() as i32;
                mmi.ptMinTrackSize.y = (MIN_WIN_H_DIP * scale).round() as i32;
                return LRESULT(0);
            }
        }
        WM_SIZE => {
            post_ctrl(QS_EVT_SIZE, lparam.0);
            return LRESULT(0);
        }
        WM_EXITSIZEMOVE => {
            post_ctrl(QS_EVT_EXITSIZEMOVE, 0);
            return LRESULT(0);
        }
        WM_PAINT => {
            let mut ps = PAINTSTRUCT::default();
            let _ = BeginPaint(hwnd, &mut ps);
            let _ = EndPaint(hwnd, &ps);
            post_ctrl(QS_EVT_PAINT, 0);
            return LRESULT(0);
        }
        WM_KEYDOWN => {
            post_ctrl(QS_EVT_KEYDOWN, wparam.0 as isize);
            return LRESULT(0);
        }
        WM_CHAR => {
            post_ctrl(QS_EVT_CHAR, wparam.0 as isize);
            return LRESULT(0);
        }
        WM_MOUSEMOVE => {
            post_ctrl(QS_EVT_MOUSEMOVE, lparam.0);
            return LRESULT(0);
        }
        WM_LBUTTONDOWN => {
            post_ctrl(QS_EVT_LBUTTONDOWN, lparam.0);
            return LRESULT(0);
        }
        WM_LBUTTONUP => {
            post_ctrl(QS_EVT_LBUTTONUP, lparam.0);
            return LRESULT(0);
        }
        WM_LBUTTONDBLCLK => {
            post_ctrl(QS_EVT_LBUTTONDBLCLK, lparam.0);
            return LRESULT(0);
        }
        WM_RBUTTONDOWN => {
            post_ctrl(QS_EVT_RBUTTONDOWN, lparam.0);
            return LRESULT(0);
        }
        WM_MOUSEWHEEL => {
            let delta = ((wparam.0 >> 16) & 0xFFFF) as i16 as isize;
            post_ctrl(QS_EVT_WHEEL, delta);
            return LRESULT(0);
        }
        WM_SETCURSOR => {
            let hit_test = (lparam.0 as u32) & 0xFFFF;
            if hit_test == HTCLIENT {
                let id = match QS_CURSOR_KIND.load(Ordering::Acquire) {
                    QS_CUR_HAND => IDC_HAND,
                    QS_CUR_SIZEWE => IDC_SIZEWE,
                    _ => IDC_ARROW,
                };
                if let Ok(cur) = LoadCursorW(None, id) {
                    SetCursor(cur);
                    return LRESULT(1);
                }
            }
        }
        WM_ACTIVATE => {
            if (wparam.0 & 0xFFFF) as u32 == WA_INACTIVE {
                post_ctrl(QS_EVT_DEACTIVATE, 0);
            }
        }
        _ => {}
    }
    DefWindowProcW(hwnd, msg, wparam, lparam)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::core::config::WindowKitConfig;

    #[test]
    fn quick_switch_v2_defaults_and_bounds_config_parse_cleanly() {
        let raw = r#"{
            "schemaVersion": 3,
            "enabled": true,
            "features": { "quickSwitch": true },
            "quickSwitch": {
                "replaceAltTab": false,
                "sameAppEnabled": true,
                "previewInDesktopPosition": true,
                "currentDesktopOnly": true,
                "rememberBounds": true,
                "customWidthDip": 1120,
                "customHeightDip": 680,
                "listWidthRatio": 0.42,
                "customMonitorId": "MON-A",
                "customRelRect": { "x": 0.2, "y": 0.15, "w": 0.5, "h": 0.6 }
            }
        }"#;
        let cfg = WindowKitConfig::from_json_str(raw).expect("parse v3 quickSwitch");
        assert!(!cfg.quick_switch.replace_alt_tab);
        assert!(cfg.quick_switch.same_app_enabled);
        assert!(cfg.quick_switch.preview_in_desktop_position);
        assert!(cfg.quick_switch.current_desktop_only);
        assert!(cfg.quick_switch.remember_bounds);
        assert_eq!(cfg.quick_switch.custom_width_dip, Some(1120.0));
        assert_eq!(cfg.quick_switch.custom_height_dip, Some(680.0));
        assert!((cfg.quick_switch.list_width_ratio - 0.42).abs() < 0.001);
        assert_eq!(cfg.quick_switch.custom_monitor_id, "MON-A");
        assert!(cfg.quick_switch.custom_rel_rect.is_some());
    }

    #[test]
    fn quick_switch_process_identity_rejects_null_hwnd() {
        let id = ProcessIdentity {
            pid: 999_999,
            started_at: 12345,
        };
        assert!(!id.verify_live(HWND::default()));
    }
}
