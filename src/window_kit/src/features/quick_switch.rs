// ─────────────────────────────────────────────────────────────────────────────
// features/quick_switch.rs — 现代原生 Direct2D + DwmRegisterThumbnail Alt+Tab 速切器
//                            对标并超越 AltTab / AltTabio 交互体验：
//                            - 卡片 115ms 缓入 + DWM 实时缩略图 95ms 交叉柔显
//                            - 选中高亮胶囊 144Hz 弹簧阻尼丝滑跟手（收敛即停表 0% CPU）
//                            - 内置中文拼音首字母/关键词 DirectWrite 原生高亮着重 + 自动省略号
//                            - 两段式 Esc / Shift+Tab / Home / End / PgUp / PgDn / Delete 关窗 / M 最小化
//                            - 矢量绘制悬停快捷按钮（最小化 / 关闭 / 强杀）与极细滚动条
//                            - 隐藏立即注销缩略图并释放全部临时位图与渲染目标（零常驻占用）
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{parse_hex_color, WindowKitConfig};
use crate::core::constants::{
    ANIM_QUICK_SWITCH_FADE_MS, ANIM_THUMBNAIL_FADE_MS, QUICK_SWITCH_HEIGHT_DIP,
    QUICK_SWITCH_LIST_WIDTH_DIP, QUICK_SWITCH_MAX_VISIBLE_ROWS, QUICK_SWITCH_ROW_COMFORTABLE_DIP,
    QUICK_SWITCH_ROW_COMPACT_DIP, QUICK_SWITCH_WINDOW_CLASS, QUICK_SWITCH_WIDTH_DIP,
    WM_WK_INTERNAL_QUICK_SWITCH_KEY,
};
use crate::core::pinyin::{find_title_highlight_utf16_ranges, score_window_match};
use crate::core::win_utils::{
    activate_window, apply_win11_card_chrome, close_window_async, enumerate_monitors,
    enumerate_user_windows, get_window_dpi, get_window_icon_safe, kill_window_process_safe,
    minimize_window_async, monitor_for_point, set_window_layered_alpha, to_wide, WindowInfo,
};
use crate::render::d2d::{
    ease_out_cubic, exp_damp_f32, rect_f, rgb_a, rgba, rounded_rect, SharedGraphics,
};
use crate::{wk_debug, wk_info};
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Once;
use std::time::Instant;
use windows::core::{Interface, PCWSTR};
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, SIZE, WPARAM};
use windows::Win32::Graphics::Direct2D::Common::D2D_POINT_2F;
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
    GetAsyncKeyState, VK_BACK, VK_CONTROL, VK_DELETE, VK_DOWN, VK_END, VK_ESCAPE, VK_HOME,
    VK_MENU, VK_NEXT, VK_PRIOR, VK_RETURN, VK_SHIFT, VK_SPACE, VK_TAB, VK_UP,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, GetCursorPos, GetWindowLongPtrW, IsWindow,
    LoadCursorW, PostMessageW, RegisterClassExW, SetCursor, SetWindowLongPtrW, SetWindowPos,
    ShowWindow, CS_HREDRAW, CS_VREDRAW, GWL_EXSTYLE, GWLP_USERDATA, HWND_TOPMOST, IDC_ARROW,
    IDC_HAND, SWP_SHOWWINDOW, SW_HIDE, SW_SHOW, WA_INACTIVE, WM_ACTIVATE, WM_CHAR, WM_KEYDOWN,
    WM_LBUTTONDOWN, WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_PAINT, WM_SETCURSOR, WNDCLASSEXW,
    WS_EX_LAYERED, WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_POPUP,
};

/// 供低级键盘钩子无锁读取的 QuickSwitch 打开状态
pub static QUICK_SWITCH_IS_OPEN: AtomicBool = AtomicBool::new(false);
/// 供 WM_SETCURSOR 无锁读取的交互悬停态（列表行 / 行尾操作按钮显示手型光标，其余显示箭头，绝不滞留系统等待圈）
static QUICK_SWITCH_HOVER_INTERACTIVE: AtomicBool = AtomicBool::new(false);
const HTCLIENT_CODE: usize = 1;

/// QuickSwitch 内部回传控制窗口的事件编码
pub const QS_EVT_PAINT: usize = 1;
pub const QS_EVT_KEYDOWN: usize = 2;
pub const QS_EVT_CHAR: usize = 3;
pub const QS_EVT_MOUSEMOVE: usize = 4;
pub const QS_EVT_LBUTTONDOWN: usize = 5;
pub const QS_EVT_WHEEL: usize = 6;
pub const QS_EVT_DEACTIVATE: usize = 7;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RowActionKind {
    Minimize,
    Close,
    Kill,
}

#[derive(Debug, Clone)]
struct FilteredItem {
    info: WindowInfo,
    #[allow(dead_code)]
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
    all_windows: Vec<WindowInfo>,
    filtered: Vec<FilteredItem>,
    selected_index: usize,
    scroll_offset: usize,
    hovered_row: Option<usize>,
    hovered_action: Option<(usize, RowActionKind)>,
    search_query: String,
    search_mode: bool,
    opened_via_alt_tab: bool,
    open: bool,
    opened_at: Option<Instant>,
    card_fade_done: bool,
    /// 高亮胶囊平滑插值坐标（DIP，收敛后与目标行严格重合，停表 0% CPU）
    highlight_y_dip: f32,
    highlight_target_y_dip: f32,
    last_anim_tick: Option<Instant>,
    card_dpi: u32,
    card_width_px: i32,
    card_height_px: i32,
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
            all_windows: Vec::new(),
            filtered: Vec::new(),
            selected_index: 0,
            scroll_offset: 0,
            hovered_row: None,
            hovered_action: None,
            search_query: String::new(),
            search_mode: false,
            opened_via_alt_tab: false,
            open: false,
            opened_at: None,
            card_fade_done: true,
            highlight_y_dip: 66.0,
            highlight_target_y_dip: 66.0,
            last_anim_tick: None,
            card_dpi: 96,
            card_width_px: QUICK_SWITCH_WIDTH_DIP as i32,
            card_height_px: QUICK_SWITCH_HEIGHT_DIP as i32,
        }
    }

    pub fn set_control_hwnd(&mut self, hwnd: HWND) {
        self.control_hwnd = hwnd;
    }

    pub fn is_open(&self) -> bool {
        self.open
    }

    /// 是否存在尚未收敛的淡入或胶囊滑动动画（用于统一按需驱动 144Hz 定时器）
    pub fn has_active_animation(&self) -> bool {
        if !self.open {
            return false;
        }
        !self.card_fade_done
            || self.thumbnail_fade_started.is_some()
            || (self.highlight_y_dip - self.highlight_target_y_dip).abs() > 0.35
    }

    /// 动画帧驱动：推进卡片淡入、高亮胶囊弹性阻尼滑动与 DWM 缩略图淡入，全部收敛时返回 false
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

        // 1. 卡片 115ms EaseOutCubic 淡入
        if !self.card_fade_done {
            if let (Some(hwnd), Some(started)) = (self.card_hwnd, self.opened_at) {
                let t = (started.elapsed().as_secs_f32() * 1000.0) / ANIM_QUICK_SWITCH_FADE_MS;
                if t >= 1.0 {
                    set_window_layered_alpha(hwnd, 255);
                    self.card_fade_done = true;
                } else {
                    let eased = ease_out_cubic(t);
                    let alpha = (60.0 + eased * 195.0).round().clamp(60.0, 255.0) as u8;
                    set_window_layered_alpha(hwnd, alpha);
                }
            } else {
                self.card_fade_done = true;
            }
        }

        // 2. 选中高亮胶囊平滑阻尼滑动（半衰期 28ms，丝滑跟手不拖泥带水）
        if (self.highlight_y_dip - self.highlight_target_y_dip).abs() > 0.35 {
            self.highlight_y_dip =
                exp_damp_f32(self.highlight_y_dip, self.highlight_target_y_dip, dt_ms, 28.0);
            if (self.highlight_y_dip - self.highlight_target_y_dip).abs() <= 0.35 {
                self.highlight_y_dip = self.highlight_target_y_dip;
            }
            need_repaint = true;
        }

        // 3. DWM 实时缩略图 95ms 柔顺淡入
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

    fn row_height_dip(&self, cfg: &WindowKitConfig) -> f32 {
        if cfg.quick_switch.compact_density {
            QUICK_SWITCH_ROW_COMPACT_DIP
        } else {
            QUICK_SWITCH_ROW_COMFORTABLE_DIP
        }
    }

    fn max_visible_rows(&self, cfg: &WindowKitConfig) -> usize {
        let scale = (self.card_dpi as f32 / 96.0).max(1.0);
        let h_dip = self.card_height_px as f32 / scale;
        let usable = (h_dip - 84.0).max(120.0);
        let fit = (usable / self.row_height_dip(cfg)).floor() as usize;
        fit.clamp(5, QUICK_SWITCH_MAX_VISIBLE_ROWS + 2)
    }

    fn sync_highlight_target(&mut self, cfg: &WindowKitConfig, snap_immediately: bool) {
        let slot = self.selected_index.saturating_sub(self.scroll_offset);
        let target_y = 66.0 + (slot as f32) * self.row_height_dip(cfg);
        self.highlight_target_y_dip = target_y;
        if snap_immediately {
            self.highlight_y_dip = target_y;
        }
    }

    /// 响应 Alt+Tab / Alt+Shift+Tab（首次按下时打开并默认选中第 2 个窗口；已打开时步进切换）
    pub fn on_alt_tab_step(
        &mut self,
        reverse: bool,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) {
        if !self.open {
            self.open_card(true, reverse, cfg, gfx, trace_id);
        } else {
            self.step_selection(if reverse { -1 } else { 1 }, cfg, gfx);
        }
    }

    /// Action 13: 程序化开关 QuickSwitch（适合热键呼出与 E2E 实机测试；进入搜索常驻模式）
    pub fn toggle_programmatic(
        &mut self,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> bool {
        if self.open {
            self.close_and_release(trace_id);
            true
        } else {
            self.open_card(false, false, cfg, gfx, trace_id);
            false
        }
    }

    /// 响应 Alt 键抬起：若未进入搜索输入态且通过 Alt+Tab 打开，立即切换目标窗口并释放资源
    /// 返回 true 表示已关闭且应调度 2000ms 延迟冷路径内存修剪
    pub fn on_alt_released(&mut self, trace_id: &str) -> bool {
        if !self.open || !self.opened_via_alt_tab || self.search_mode {
            return false;
        }
        self.commit_selected_and_close(trace_id);
        true
    }

    /// 处理 QuickSwitch 卡片转发来的键盘/鼠标/绘制事件
    /// 返回 true 表示窗口刚刚关闭，调度器应安排冷路径修剪
    pub fn handle_card_event(
        &mut self,
        evt: usize,
        param: isize,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> bool {
        if !self.open {
            return false;
        }
        match evt {
            QS_EVT_PAINT => {
                if let Some(g) = gfx {
                    self.render(g, cfg);
                }
                false
            }
            QS_EVT_KEYDOWN => {
                let vk = param as u16;
                let ctrl = async_down(VK_CONTROL.0);
                let shift = async_down(VK_SHIFT.0);
                let alt = async_down(VK_MENU.0);

                // 两段式 Esc（对标 AltTab / PowerToys Run）：若有搜索词先清空恢复全量列表，再按 Esc 关闭卡片
                if vk == VK_ESCAPE.0 {
                    if !self.search_query.is_empty() {
                        self.search_query.clear();
                        self.apply_filter(cfg);
                        self.sync_highlight_target(cfg, false);
                        self.refresh_thumbnail_and_repaint(cfg, gfx);
                        return false;
                    }
                    self.close_and_release(trace_id);
                    return true;
                }
                if vk == VK_RETURN.0 {
                    self.commit_selected_and_close(trace_id);
                    return true;
                }
                if vk == VK_DOWN.0 {
                    self.step_selection(1, cfg, gfx);
                    return false;
                }
                if vk == VK_UP.0 {
                    self.step_selection(-1, cfg, gfx);
                    return false;
                }
                if vk == VK_TAB.0 {
                    self.step_selection(if shift { -1 } else { 1 }, cfg, gfx);
                    return false;
                }
                if vk == VK_HOME.0 {
                    if !self.filtered.is_empty() {
                        self.selected_index = 0;
                        self.ensure_selected_visible(cfg);
                        self.sync_highlight_target(cfg, false);
                        self.refresh_thumbnail_and_repaint(cfg, gfx);
                    }
                    return false;
                }
                if vk == VK_END.0 {
                    if !self.filtered.is_empty() {
                        self.selected_index = self.filtered.len() - 1;
                        self.ensure_selected_visible(cfg);
                        self.sync_highlight_target(cfg, false);
                        self.refresh_thumbnail_and_repaint(cfg, gfx);
                    }
                    return false;
                }
                if vk == VK_PRIOR.0 {
                    self.step_selection(-(self.max_visible_rows(cfg) as i32), cfg, gfx);
                    return false;
                }
                if vk == VK_NEXT.0 {
                    self.step_selection(self.max_visible_rows(cfg) as i32, cfg, gfx);
                    return false;
                }
                // Ctrl+A / Ctrl+Backspace 一键清空搜索词
                if (ctrl && (vk == b'A' as u16 || vk == VK_BACK.0)) && !self.search_query.is_empty() {
                    self.search_query.clear();
                    self.apply_filter(cfg);
                    self.sync_highlight_target(cfg, false);
                    self.refresh_thumbnail_and_repaint(cfg, gfx);
                    return false;
                }
                if vk == VK_BACK.0 {
                    self.search_mode = true;
                    self.search_query.pop();
                    self.apply_filter(cfg);
                    self.sync_highlight_target(cfg, false);
                    self.refresh_thumbnail_and_repaint(cfg, gfx);
                    return false;
                }
                // Delete 或 Ctrl+W 直接关闭当前选中窗口（不退出速切卡片，对标 AltTab 高效清窗！）
                if vk == VK_DELETE.0 || (ctrl && vk == b'W' as u16) {
                    self.execute_row_action_on_index(
                        self.selected_index,
                        RowActionKind::Close,
                        cfg,
                        gfx,
                        trace_id,
                    );
                    return false;
                }
                // Alt+1..9 或 Ctrl+1..9 随时一键直达可见列表第 1-9 行
                if (alt || ctrl) && (b'1' as u16..=b'9' as u16).contains(&vk) {
                    let idx = (vk as u8 - b'1') as usize + self.scroll_offset;
                    if idx < self.filtered.len() {
                        self.selected_index = idx;
                        self.commit_selected_and_close(trace_id);
                        return true;
                    }
                }
                // 按 Space 锁定为浏览搜索模式（防松开 Alt 误关）
                if vk == VK_SPACE.0 && self.search_query.is_empty() && !self.search_mode {
                    self.search_mode = true;
                    self.request_repaint();
                    return false;
                }
                false
            }
            QS_EVT_CHAR => {
                let ch_u32 = param as u32;
                let Some(ch) = char::from_u32(ch_u32) else {
                    return false;
                };
                if ch.is_control() {
                    return false;
                }
                // 非搜索态下：按 M 快速最小化当前选中窗口，按 1-9 一键快切
                if self.search_query.is_empty() && !self.search_mode {
                    if ('1'..='9').contains(&ch) {
                        let idx = (ch as u8 - b'1') as usize + self.scroll_offset;
                        if idx < self.filtered.len() {
                            self.selected_index = idx;
                            self.commit_selected_and_close(trace_id);
                            return true;
                        }
                    }
                    if ch == 'm' || ch == 'M' {
                        self.execute_row_action_on_index(
                            self.selected_index,
                            RowActionKind::Minimize,
                            cfg,
                            gfx,
                            trace_id,
                        );
                        return false;
                    }
                    if ch == ' ' {
                        self.search_mode = true;
                        self.request_repaint();
                        return false;
                    }
                }
                self.search_mode = true;
                if self.search_query.chars().count() < 48 {
                    self.search_query.push(ch);
                }
                self.apply_filter(cfg);
                self.sync_highlight_target(cfg, false);
                self.refresh_thumbnail_and_repaint(cfg, gfx);
                false
            }
            QS_EVT_MOUSEMOVE => {
                let (x_dip, y_dip) = self.unpack_dip_coords(param);
                let (row, act) = self.hit_test_list(x_dip, y_dip, cfg);
                let interactive = row.is_some() || act.is_some();
                let prev_interactive =
                    QUICK_SWITCH_HOVER_INTERACTIVE.swap(interactive, Ordering::AcqRel);
                if prev_interactive != interactive {
                    unsafe {
                        let cursor_id = if interactive { IDC_HAND } else { IDC_ARROW };
                        if let Ok(cur) = LoadCursorW(None, cursor_id) {
                            SetCursor(cur);
                        }
                    }
                }
                if row != self.hovered_row || act != self.hovered_action {
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
                false
            }
            QS_EVT_LBUTTONDOWN => {
                let (x_dip, y_dip) = self.unpack_dip_coords(param);
                let (row, act) = self.hit_test_list(x_dip, y_dip, cfg);
                if let Some((r, kind)) = act {
                    self.search_mode = true; // 点击行按钮后锁定浏览态，避免松开 Alt 误切窗
                    self.execute_row_action_on_index(r, kind, cfg, gfx, trace_id);
                    return false;
                }
                if let Some(r) = row {
                    if r < self.filtered.len() {
                        self.selected_index = r;
                        self.commit_selected_and_close(trace_id);
                        return true;
                    }
                }
                false
            }
            QS_EVT_WHEEL => {
                let delta = param as i16;
                self.step_selection(if delta < 0 { 1 } else { -1 }, cfg, gfx);
                false
            }
            QS_EVT_DEACTIVATE => {
                // 打开后 280ms 内忽略跨进程切换焦点时的瞬态失焦抖动，防止从设置页呼出瞬间误关
                if self
                    .opened_at
                    .map(|t| t.elapsed().as_millis() < 280)
                    .unwrap_or(false)
                {
                    return false;
                }
                self.close_and_release(trace_id);
                true
            }
            _ => false,
        }
    }

    fn execute_row_action_on_index(
        &mut self,
        idx: usize,
        kind: RowActionKind,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) {
        let Some(item) = self.filtered.get(idx).cloned() else {
            return;
        };
        match kind {
            RowActionKind::Minimize => minimize_window_async(item.info.hwnd),
            RowActionKind::Close => close_window_async(item.info.hwnd),
            RowActionKind::Kill => {
                let _ = kill_window_process_safe(item.info.hwnd);
            }
        }
        if matches!(kind, RowActionKind::Close | RowActionKind::Kill) {
            self.all_windows.retain(|w| w.hwnd != item.info.hwnd);
        } else {
            for w in &mut self.all_windows {
                if w.hwnd == item.info.hwnd {
                    w.is_minimized = true;
                }
            }
            if !cfg.quick_switch.include_minimized {
                self.all_windows.retain(|w| w.hwnd != item.info.hwnd);
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
            item.info.hwnd.0 as isize
        );
    }

    fn open_card(
        &mut self,
        via_alt_tab: bool,
        reverse: bool,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) {
        self.all_windows =
            enumerate_user_windows(cfg.quick_switch.include_minimized, &cfg.excluded_apps);

        let monitors = enumerate_monitors();
        let mut cursor = POINT::default();
        let _ = unsafe { GetCursorPos(&mut cursor) };
        let active_mon = monitor_for_point(cursor.x, cursor.y, &monitors)
            .or_else(|| monitors.first().cloned());

        if !cfg.quick_switch.show_all_monitors {
            if let Some(m) = &active_mon {
                self.all_windows.retain(|w| w.monitor_id == m.id);
            }
        }

        self.search_query.clear();
        self.search_mode = !via_alt_tab;
        self.opened_via_alt_tab = via_alt_tab;
        self.hovered_row = None;
        self.hovered_action = None;
        self.scroll_offset = 0;
        self.apply_filter(cfg);

        // Alt+Tab 首次打开默认选中第二个窗口（即上一个活跃窗口），反向则选最后一个
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
            crate::wk_error!(
                trace_id,
                "quickSwitch",
                "create_quick_switch_window failed err={:?}",
                unsafe { windows::Win32::Foundation::GetLastError() }
            );
            return;
        };

        let work = active_mon
            .as_ref()
            .map(|m| m.work_rect)
            .unwrap_or(crate::core::geometry::RectI::new(0, 0, 1920, 1040));
        let dpi = active_mon.as_ref().map(|m| m.dpi).unwrap_or(96).max(96);
        self.card_dpi = dpi;
        let scale = dpi as f32 / 96.0;

        let card_w = ((QUICK_SWITCH_WIDTH_DIP * scale).round() as i32).min(work.width() - 32);
        let card_h = ((QUICK_SWITCH_HEIGHT_DIP * scale).round() as i32).min(work.height() - 32);
        self.card_width_px = card_w.max(460);
        self.card_height_px = card_h.max(320);

        let left = work.left + (work.width() - self.card_width_px) / 2;
        let top = work.top + (work.height() - self.card_height_px) / 2;

        self.ensure_selected_visible(cfg);
        self.sync_highlight_target(cfg, true);

        apply_win11_card_chrome(hwnd, cfg.is_dark_theme(), false);
        set_window_layered_alpha(hwnd, 64);
        unsafe {
            let _ = SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                left,
                top,
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

        // 重建匹配当前窗口尺寸和 DPI 的 HwndRenderTarget 并加载可见图标
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
            self.preload_icons(g);
            self.update_thumbnail(cfg);
            self.render(g, cfg);
        }

        wk_info!(
            trace_id,
            "quickSwitch",
            "opened card hwnd=0x{:x} windows={} via_alt_tab={} dpi={}",
            hwnd.0 as isize,
            self.filtered.len(),
            via_alt_tab,
            self.card_dpi
        );
    }

    fn commit_selected_and_close(&mut self, trace_id: &str) {
        let target = self
            .filtered
            .get(self.selected_index)
            .map(|item| (item.info.hwnd, item.info.process_name.clone()));
        // 关键顺序：先标记关闭态防止激活目标窗触发 WA_INACTIVE 二次重入，
        // 再趁速切卡片仍持有前台焦点时直接把前台权交给目标窗口（避免先 SW_HIDE 丢失前台锁导致 ok=false），
        // 最后立即隐藏卡片并释放全部 D2D/缩略图资源。
        self.open = false;
        QUICK_SWITCH_IS_OPEN.store(false, Ordering::Release);
        QUICK_SWITCH_HOVER_INTERACTIVE.store(false, Ordering::Release);
        if let Some((hwnd, proc_name)) = target {
            let ok = activate_window(hwnd);
            wk_info!(
                trace_id,
                "quickSwitch",
                "switched to hwnd=0x{:x} proc={} ok={}",
                hwnd.0 as isize,
                proc_name,
                ok
            );
        }
        self.close_and_release(trace_id);
    }

    /// 隐藏卡片并立即释放全部 DWM 缩略图、图标位图与 Direct2D 渲染目标（零内存常驻）
    pub fn close_and_release(&mut self, trace_id: &str) {
        self.unregister_thumbnail();
        self.icon_cache.clear();
        self.render_target = None;
        self.all_windows.clear();
        self.filtered.clear();
        self.search_query.clear();
        self.search_mode = false;
        self.hovered_row = None;
        self.hovered_action = None;
        self.card_fade_done = true;
        self.last_anim_tick = None;
        self.open = false;
        QUICK_SWITCH_IS_OPEN.store(false, Ordering::Release);
        QUICK_SWITCH_HOVER_INTERACTIVE.store(false, Ordering::Release);

        if let Some(hwnd) = self.card_hwnd {
            unsafe {
                if IsWindow(hwnd).as_bool() {
                    let _ = ShowWindow(hwnd, SW_HIDE);
                }
            }
        }
        wk_debug!(trace_id, "quickSwitch", "closed and released all preview/icon resources");
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
        let mut matched: Vec<FilteredItem> = Vec::new();
        for w in &self.all_windows {
            if let Some(score) = score_window_match(
                &self.search_query,
                &w.title,
                &w.process_name,
                cfg.quick_switch.enable_pinyin_search,
            ) {
                matched.push(FilteredItem {
                    info: w.clone(),
                    score,
                });
            }
        }
        // 搜索词非空时按相关性排序；搜索词为空时严格保留系统 Z-Order 切换顺序
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
        // 发生列表滚屏或回绕跳变时缩短跨度避免过度甩动，常规相邻切换走丝滑阻尼滑动
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

    fn preload_icons(&mut self, gfx: &SharedGraphics) {
        let Some(rt_hwnd) = self.render_target.as_ref() else {
            return;
        };
        let Ok(rt): Result<ID2D1RenderTarget, _> = rt_hwnd.cast() else {
            return;
        };
        for win in self.all_windows.iter().take(28) {
            let key = win.hwnd.0 as isize;
            if self.icon_cache.contains_key(&key) {
                continue;
            }
            if let Some(icon) = get_window_icon_safe(win.hwnd, &win.exe_path, false) {
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

    /// 绑定并更新右侧预览区的实时 GPU 零拷贝 DWM 缩略图（带 95ms 切窗柔滑淡入）
    fn update_thumbnail(&mut self, cfg: &WindowKitConfig) {
        if !cfg.quick_switch.show_live_preview || !self.open {
            self.unregister_thumbnail();
            return;
        }
        let (Some(dst_hwnd), Some(item)) = (self.card_hwnd, self.filtered.get(self.selected_index))
        else {
            self.unregister_thumbnail();
            return;
        };

        let src_hwnd = item.info.hwnd;
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
                (85.0 + ease_out_cubic(t) * 170.0).round().clamp(85.0, 255.0) as u8
            }
        } else {
            255u8
        };

        let scale = (self.card_dpi as f32 / 96.0).max(1.0);
        let stage_left = ((QUICK_SWITCH_LIST_WIDTH_DIP + 22.0) * scale).round() as i32;
        let stage_top = (106.0 * scale).round() as i32;
        let stage_right = self.card_width_px - (24.0 * scale).round() as i32;
        let stage_bottom = self.card_height_px - (60.0 * scale).round() as i32;
        let stage_w = (stage_right - stage_left).max(64);
        let stage_h = (stage_bottom - stage_top).max(64);

        let src_size = unsafe { DwmQueryThumbnailSourceSize(self.thumbnail_handle) }
            .unwrap_or(SIZE {
                cx: stage_w,
                cy: stage_h,
            });
        let sw = src_size.cx.max(1) as f64;
        let sh = src_size.cy.max(1) as f64;
        let ratio = (stage_w as f64 / sw).min(stage_h as f64 / sh).min(1.0);
        let dst_w = (sw * ratio).round().max(32.0) as i32;
        let dst_h = (sh * ratio).round().max(24.0) as i32;
        let dst_left = stage_left + (stage_w - dst_w) / 2;
        let dst_top = stage_top + (stage_h - dst_h) / 2;

        let props = DWM_THUMBNAIL_PROPERTIES {
            dwFlags: DWM_TNP_RECTDESTINATION
                | DWM_TNP_VISIBLE
                | DWM_TNP_OPACITY
                | DWM_TNP_SOURCECLIENTAREAONLY,
            rcDestination: RECT {
                left: dst_left,
                top: dst_top,
                right: dst_left + dst_w,
                bottom: dst_top + dst_h,
            },
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
        let scale = (self.card_dpi as f32 / 96.0).max(1.0);
        (px_x / scale, px_y / scale)
    }

    fn hit_test_list(
        &self,
        x_dip: f32,
        y_dip: f32,
        cfg: &WindowKitConfig,
    ) -> (Option<usize>, Option<(usize, RowActionKind)>) {
        let list_left = 14.0;
        let list_right = QUICK_SWITCH_LIST_WIDTH_DIP - 10.0;
        let list_top = 66.0;
        let row_h = self.row_height_dip(cfg);
        let max_rows = self.max_visible_rows(cfg);
        if x_dip < list_left || x_dip > list_right || y_dip < list_top {
            return (None, None);
        }
        let rel_row = ((y_dip - list_top) / row_h).floor() as usize;
        let actual_idx = self.scroll_offset + rel_row;
        if rel_row >= max_rows || actual_idx >= self.filtered.len() {
            return (None, None);
        }

        // 检查行尾 3 个矢量快捷操作按钮（最小化 / 关闭 / 强制结束）
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

        (Some(actual_idx), None)
    }

    fn render(&mut self, gfx: &SharedGraphics, cfg: &WindowKitConfig) {
        let Some(rt) = self.render_target.as_ref() else {
            return;
        };

        let scale = (self.card_dpi as f32 / 96.0).max(1.0);
        let w_dip = self.card_width_px as f32 / scale;
        let h_dip = self.card_height_px as f32 / scale;
        let dark = cfg.is_dark_theme();
        let accent = parse_hex_color(&cfg.accent_color, [0.23, 0.51, 0.96]);
        let row_h = self.row_height_dip(cfg);
        let max_rows = self.max_visible_rows(cfg);

        let title_fmt = gfx.ui_text_format(13.5, false, false).ok();
        let stage_title_fmt = gfx.ui_text_format(13.5, true, false).ok();
        let sub_fmt = gfx.mono_text_format(12.0, false, false).ok();
        let badge_fmt = gfx.mono_text_format(12.0, true, true).ok();
        let search_fmt = gfx.ui_text_format(14.0, false, false).ok();
        let footer_fmt = gfx.ui_text_format(12.0, false, false).ok();
        let empty_fmt = gfx.ui_text_format(13.5, false, true).ok();

        unsafe {
            rt.BeginDraw();

            let bg_col = if dark {
                rgba(0.075, 0.088, 0.116, 1.0)
            } else {
                rgba(0.960, 0.968, 0.984, 1.0)
            };
            let surface_col = if dark {
                rgba(0.118, 0.136, 0.174, 1.0)
            } else {
                rgba(1.0, 1.0, 1.0, 1.0)
            };
            let hover_col = if dark {
                rgba(1.0, 1.0, 1.0, 0.065)
            } else {
                rgba(0.08, 0.12, 0.22, 0.06)
            };
            let border_col = if dark {
                rgba(1.0, 1.0, 1.0, 0.11)
            } else {
                rgba(0.08, 0.12, 0.20, 0.13)
            };
            let rim_highlight = if dark {
                rgba(1.0, 1.0, 1.0, 0.18)
            } else {
                rgba(0.08, 0.12, 0.20, 0.18)
            };
            let text_primary = if dark {
                rgba(0.955, 0.972, 1.0, 1.0)
            } else {
                rgba(0.085, 0.105, 0.155, 1.0)
            };
            let text_muted = if dark {
                rgba(0.62, 0.69, 0.79, 1.0)
            } else {
                rgba(0.38, 0.44, 0.54, 1.0)
            };
            let text_on_accent = rgba(1.0, 1.0, 1.0, 0.98);

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
                rt.CreateSolidColorBrush(&rgb_a(accent, if dark { 0.22 } else { 0.14 }), None),
                rt.CreateSolidColorBrush(&rgb_a(accent, 0.95), None),
                rt.CreateSolidColorBrush(&text_primary, None),
                rt.CreateSolidColorBrush(&text_muted, None),
                rt.CreateSolidColorBrush(&text_on_accent, None),
                rt.CreateSolidColorBrush(&rgba(0.90, 0.27, 0.31, 0.94), None),
                rt.CreateSolidColorBrush(&rgba(0.95, 0.62, 0.08, 0.94), None),
            ) else {
                let _ = rt.EndDraw(None, None);
                return;
            };

            // 顶层微晶卡片 1px 外框描边
            let card_outline = rect_f(0.5, 0.5, w_dip - 0.5, h_dip - 0.5);
            rt.DrawRoundedRectangle(&rounded_rect(card_outline, 10.0), &b_rim, 1.0, None);

            // 1. 顶部搜索框 + 原生矢量放大镜图标 + 计数胶囊
            let search_rect = rect_f(14.0, 14.0, w_dip - 14.0, 52.0);
            rt.FillRoundedRectangle(&rounded_rect(search_rect, 10.0), &b_surface);
            rt.DrawRoundedRectangle(
                &rounded_rect(search_rect, 10.0),
                if self.search_mode { &b_accent_stroke } else { &b_border },
                if self.search_mode { 1.6 } else { 1.0 },
                None,
            );

            // 矢量放大镜图标（左起 28 DIP，16x16 栅格精准抗锯齿绘制）
            let lens_brush = if self.search_mode { &b_accent_stroke } else { &b_muted };
            let lens = D2D1_ELLIPSE {
                point: D2D_POINT_2F { x: 33.0, y: 31.5 },
                radiusX: 5.5,
                radiusY: 5.5,
            };
            rt.DrawEllipse(&lens, lens_brush, 1.6, None);
            rt.DrawLine(
                D2D_POINT_2F { x: 37.0, y: 35.5 },
                D2D_POINT_2F { x: 41.0, y: 39.5 },
                lens_brush,
                1.8,
                None,
            );

            // 搜索文本与右端计数胶囊
            let count_text = format!("{}", self.filtered.len());
            let count_pill = rect_f(w_dip - 62.0, 22.0, w_dip - 24.0, 44.0);
            rt.FillRoundedRectangle(&rounded_rect(count_pill, 6.0), &b_hover);
            rt.DrawRoundedRectangle(&rounded_rect(count_pill, 6.0), &b_border, 1.0, None);
            if let Some(fmt) = &badge_fmt {
                let wc = to_wide(&count_text);
                rt.DrawText(
                    &wc[..wc.len().saturating_sub(1)],
                    fmt,
                    &count_pill,
                    &b_muted,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }

            if let Some(fmt) = &search_fmt {
                let display = if self.search_query.is_empty() {
                    if self.search_mode {
                        "Type title, process or Pinyin initials (wx / sz / vsc) ...".to_string()
                    } else {
                        "Release Alt to switch  |  Type to filter  |  Del: Close  |  M: Minimize".to_string()
                    }
                } else {
                    format!("{}|", self.search_query)
                };
                if let Some(layout) =
                    gfx.create_trimmed_text_layout(&display, fmt, w_dip - 130.0, 26.0)
                {
                    rt.DrawTextLayout(
                        D2D_POINT_2F { x: 49.0, y: 23.0 },
                        &layout,
                        if self.search_query.is_empty() { &b_muted } else { &b_text },
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    );
                }
            }

            // 2. 左侧窗口列表（含 144Hz 弹簧阻尼滑行高亮胶囊 + 拼音关键词原生局部高亮 + 矢量悬停按钮）
            let list_left = 14.0;
            let list_right = QUICK_SWITCH_LIST_WIDTH_DIP - 10.0;
            let list_top = 66.0;
            let visible_end = (self.scroll_offset + max_rows).min(self.filtered.len());

            // 2a. 先画丝滑移动的选中高亮胶囊 + 左侧 3px 主题指示竖条（在所有文字下层）
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
                    let msg = to_wide("No matching windows  (Esc to reset)");
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

            for (slot, idx) in (self.scroll_offset..visible_end).enumerate() {
                let item = &self.filtered[idx];
                let y0 = list_top + (slot as f32) * row_h;
                let row_rect = rect_f(list_left, y0 + 2.0, list_right, y0 + row_h - 2.0);
                let is_sel = idx == self.selected_index;
                let is_hov = self.hovered_row == Some(idx);

                if is_hov && !is_sel {
                    rt.FillRoundedRectangle(&rounded_rect(row_rect, 8.0), &b_hover);
                }

                // 真实应用图标（紧凑 20px / 舒适 24px）
                let icon_sz = if compact { 20.0 } else { 24.0 };
                let icon_y = y0 + (row_h - icon_sz) * 0.5;
                let icon_rect = rect_f(list_left + 13.0, icon_y, list_left + 13.0 + icon_sz, icon_y + icon_sz);
                if let Some(bmp) = self.icon_cache.get(&(item.info.hwnd.0 as isize)) {
                    rt.DrawBitmap(
                        bmp,
                        Some(&icon_rect),
                        if item.info.is_minimized { 0.62 } else { 1.0 },
                        windows::Win32::Graphics::Direct2D::D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                        None,
                    );
                } else {
                    rt.FillRoundedRectangle(&rounded_rect(icon_rect, 5.0), &b_border);
                }

                // 标题与进程副标（支持 DirectWrite 省略号截断 + 拼音/关键词命中高亮）
                let text_left = list_left + 13.0 + icon_sz + 10.0;
                let text_right = if is_hov {
                    list_right - 94.0
                } else if cfg.quick_switch.show_numbers && slot < 9 {
                    list_right - 38.0
                } else {
                    list_right - 12.0
                };
                let text_w = (text_right - text_left).max(40.0);

                if let Some(fmt) = &title_fmt {
                    let title_y = if compact && !cfg.quick_switch.show_process_badge {
                        y0 + (row_h - 18.0) * 0.5
                    } else if compact {
                        y0 + 3.0
                    } else {
                        y0 + 6.0
                    };
                    if let Some(layout) =
                        gfx.create_trimmed_text_layout(&item.info.title, fmt, text_w, 20.0)
                    {
                        if hl_ranges_enabled {
                            let ranges = find_title_highlight_utf16_ranges(
                                &self.search_query,
                                &item.info.title,
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

                if cfg.quick_switch.show_process_badge {
                    if let Some(fmt) = &sub_fmt {
                        let sub = if item.info.is_minimized {
                            format!("{}  [MIN]", item.info.process_name)
                        } else {
                            item.info.process_name.clone()
                        };
                        let sub_y = if compact { y0 + 20.0 } else { y0 + 25.0 };
                        if let Some(layout) = gfx.create_trimmed_text_layout(&sub, fmt, text_w, 16.0) {
                            rt.DrawTextLayout(
                                D2D_POINT_2F { x: text_left, y: sub_y },
                                &layout,
                                &b_muted,
                                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                            );
                        }
                    }
                }

                // 右侧：悬停时绘制 3 个高保真矢量操作按钮（最小化 / 关闭 / 强杀），常态按配置显示 1-9 快选胶囊
                if is_hov {
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
                                // 水平最小化短杠
                                rt.DrawLine(
                                    D2D_POINT_2F { x: cx - 5.0, y: cy + 2.0 },
                                    D2D_POINT_2F { x: cx + 5.0, y: cy + 2.0 },
                                    fg_brush,
                                    1.7,
                                    None,
                                );
                            }
                            RowActionKind::Close => {
                                // 抗锯齿矢量斜叉 X
                                rt.DrawLine(
                                    D2D_POINT_2F { x: cx - 4.2, y: cy - 4.2 },
                                    D2D_POINT_2F { x: cx + 4.2, y: cy + 4.2 },
                                    fg_brush,
                                    1.7,
                                    None,
                                );
                                rt.DrawLine(
                                    D2D_POINT_2F { x: cx + 4.2, y: cy - 4.2 },
                                    D2D_POINT_2F { x: cx - 4.2, y: cy + 4.2 },
                                    fg_brush,
                                    1.7,
                                    None,
                                );
                            }
                            RowActionKind::Kill => {
                                // 矢量终止图标（外环 + 竖杠）
                                let ring = D2D1_ELLIPSE {
                                    point: D2D_POINT_2F { x: cx, y: cy },
                                    radiusX: 5.0,
                                    radiusY: 5.0,
                                };
                                rt.DrawEllipse(&ring, fg_brush, 1.5, None);
                                rt.DrawLine(
                                    D2D_POINT_2F { x: cx, y: cy - 3.2 },
                                    D2D_POINT_2F { x: cx, y: cy + 1.0 },
                                    fg_brush,
                                    1.6,
                                    None,
                                );
                                rt.DrawLine(
                                    D2D_POINT_2F { x: cx, y: cy + 2.6 },
                                    D2D_POINT_2F { x: cx, y: cy + 3.4 },
                                    fg_brush,
                                    1.6,
                                    None,
                                );
                            }
                        }
                    }
                } else if cfg.quick_switch.show_numbers && slot < 9 {
                    let kr = rect_f(
                        list_right - 30.0,
                        y0 + (row_h - 22.0) * 0.5,
                        list_right - 8.0,
                        y0 + (row_h + 22.0) * 0.5,
                    );
                    rt.FillRoundedRectangle(&rounded_rect(kr, 6.0), &b_surface);
                    rt.DrawRoundedRectangle(
                        &rounded_rect(kr, 6.0),
                        if is_sel { &b_accent_stroke } else { &b_border },
                        1.0,
                        None,
                    );
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

            // 2b. 列表超长时绘制极细 3px 滚动指示条
            if self.filtered.len() > max_rows {
                let track_top = list_top + 4.0;
                let track_h = (max_rows as f32) * row_h - 8.0;
                let ratio = (max_rows as f32) / (self.filtered.len() as f32);
                let thumb_h = (track_h * ratio).max(24.0);
                let max_scroll = (self.filtered.len() - max_rows) as f32;
                let scroll_t = (self.scroll_offset as f32 / max_scroll).clamp(0.0, 1.0);
                let thumb_y = track_top + (track_h - thumb_h) * scroll_t;
                let thumb_r = rect_f(list_right + 3.0, thumb_y, list_right + 6.0, thumb_y + thumb_h);
                rt.FillRoundedRectangle(&rounded_rect(thumb_r, 1.5), &b_border);
            }

            // 3. 右侧实时预览舞台（顶部展示完整窗口标题 + 状态徽标，中间零拷贝 DWM 缩略图，底部元数据）
            let stage_outer = rect_f(
                QUICK_SWITCH_LIST_WIDTH_DIP + 10.0,
                66.0,
                w_dip - 14.0,
                h_dip - 14.0,
            );
            rt.FillRoundedRectangle(&rounded_rect(stage_outer, 12.0), &b_surface);
            rt.DrawRoundedRectangle(&rounded_rect(stage_outer, 12.0), &b_border, 1.0, None);

            if let Some(sel) = self.filtered.get(self.selected_index) {
                let header_left = QUICK_SWITCH_LIST_WIDTH_DIP + 22.0;
                let header_w = (w_dip - 26.0 - header_left).max(80.0);
                if let Some(fmt) = &stage_title_fmt {
                    if let Some(layout) =
                        gfx.create_trimmed_text_layout(&sel.info.title, fmt, header_w, 22.0)
                    {
                        rt.DrawTextLayout(
                            D2D_POINT_2F {
                                x: header_left,
                                y: 76.0,
                            },
                            &layout,
                            &b_text,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        );
                    }
                }
                // 标题与预览区之间 1px 柔和分割线
                rt.DrawLine(
                    D2D_POINT_2F {
                        x: QUICK_SWITCH_LIST_WIDTH_DIP + 20.0,
                        y: 100.0,
                    },
                    D2D_POINT_2F {
                        x: w_dip - 24.0,
                        y: 100.0,
                    },
                    &b_border,
                    1.0,
                    None,
                );

                // 底部状态与操作提示条
                if let Some(fmt) = &footer_fmt {
                    let state_tag = if sel.info.is_minimized {
                        " [Minimized]"
                    } else if sel.info.is_maximized {
                        " [Maximized]"
                    } else {
                        ""
                    };
                    let meta = format!(
                        "{}{}  |  {}x{}  |  Enter: Switch  Del: Close  Esc: Back",
                        sel.info.process_name,
                        state_tag,
                        sel.info.rect.width().max(0),
                        sel.info.rect.height().max(0)
                    );
                    if let Some(layout) =
                        gfx.create_trimmed_text_layout(&meta, fmt, header_w, 20.0)
                    {
                        rt.DrawTextLayout(
                            D2D_POINT_2F {
                                x: header_left,
                                y: h_dip - 42.0,
                            },
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
            style: CS_HREDRAW | CS_VREDRAW,
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
            QUICK_SWITCH_WIDTH_DIP as i32,
            QUICK_SWITCH_HEIGHT_DIP as i32,
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
                HWND(ctrl_raw as *mut std::ffi::c_void),
                WM_WK_INTERNAL_QUICK_SWITCH_KEY,
                WPARAM(evt),
                LPARAM(param),
            );
        }
    };

    match msg {
        WM_PAINT => {
            let mut ps = PAINTSTRUCT::default();
            let _ = BeginPaint(hwnd, &mut ps);
            let _ = EndPaint(hwnd, &ps);
            post_ctrl(QS_EVT_PAINT, 0);
            LRESULT(0)
        }
        WM_KEYDOWN => {
            post_ctrl(QS_EVT_KEYDOWN, wparam.0 as isize);
            LRESULT(0)
        }
        WM_CHAR => {
            post_ctrl(QS_EVT_CHAR, wparam.0 as isize);
            LRESULT(0)
        }
        WM_MOUSEMOVE => {
            post_ctrl(QS_EVT_MOUSEMOVE, lparam.0);
            LRESULT(0)
        }
        WM_LBUTTONDOWN => {
            post_ctrl(QS_EVT_LBUTTONDOWN, lparam.0);
            LRESULT(0)
        }
        WM_MOUSEWHEEL => {
            let delta = ((wparam.0 >> 16) & 0xFFFF) as i16 as isize;
            post_ctrl(QS_EVT_WHEEL, delta);
            LRESULT(0)
        }
        WM_SETCURSOR => {
            let hit_test = (lparam.0 as usize) & 0xFFFF;
            if hit_test == HTCLIENT_CODE {
                let cursor_id = if QUICK_SWITCH_HOVER_INTERACTIVE.load(Ordering::Acquire) {
                    IDC_HAND
                } else {
                    IDC_ARROW
                };
                if let Ok(cur) = LoadCursorW(None, cursor_id) {
                    SetCursor(cur);
                    return LRESULT(1);
                }
            }
            DefWindowProcW(hwnd, msg, wparam, lparam)
        }
        WM_ACTIVATE => {
            if (wparam.0 & 0xFFFF) as u32 == WA_INACTIVE {
                post_ctrl(QS_EVT_DEACTIVATE, 0);
            }
            LRESULT(0)
        }
        _ => DefWindowProcW(hwnd, msg, wparam, lparam),
    }
}
