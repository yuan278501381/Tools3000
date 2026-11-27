// ─────────────────────────────────────────────────────────────────────────────
// features/quick_switch.rs — Quick Switch v2（Slint 1.19 极简美学 + 双轨道 + DWM 实时预览）
//
// 1:1 吸收 vibeslop/AltTabio 纯净比例美学并深度融合 Tools3000 生态:
//   1. 视觉对标 AltTabio：浅色柔和灰白 (#F3F3F3) / 深色曜石炭黑 (#111216) +
//      动态强调色柔和选中胶囊 + 1px 发丝可拖分割线 + 28px 真实应用彩色图标 +
//      默认隐形打字才浮现的中文拼音搜索胶囊 + 右栏 60FPS DwmRegisterThumbnail 零拷贝预览；
//   2. 双独立切窗轨道：全局增强 Alt+Tab（含托盘窗一键捞回） + 独立同应用 Alt+`；
//   3. 窗口操控中枢：F4(关闭)/F5(最小化)/F6(最大化)/F7(还原)/F8(防误杀强杀)/F9(降权新开) +
//      Ctrl+P(置顶)/Ctrl+Z(下一分区)/Ctrl+T(托盘收纳或捞回)/Ctrl+D(切换原位投影) + 悬停微晶条；
//   4. 跨多屏 8 向自由拖缩与分割线记忆持久化，拔屏自动回退存活屏幕。
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::WindowKitConfig;
use crate::core::constants::{
    FILE_QUICKSWITCH_BOUNDS_RESULT, HOST_MESSAGE_WINDOW_CLASS, HOST_QUICKSWITCH_BOUNDS_MSG_NAME,
};
use crate::core::geometry::RectI;
use crate::core::json::JsonValue;
use crate::core::pinyin::score_window_match;
use crate::core::topology::{
    ensure_window_rect_on_live_monitors, is_window_on_current_virtual_desktop, NormalizedWorkRect,
};
use crate::core::win_utils::{
    activate_window_or_popup, close_window_async, enumerate_monitors, enumerate_user_windows,
    extract_window_icon_rgba8, get_window_aumid, get_window_normal_placement_rect,
    get_window_process_info, kill_window_process_safe, minimize_window_async, monitor_for_hwnd,
    monitor_for_point, query_process_creation_time, spawn_process_with_elevation_policy, to_wide,
    write_text_file_atomic, MonitorInfo, WindowInfo,
};
use crate::render::d2d::SharedGraphics;
use crate::ui_slint::{
    focus_quick_switch_search, hide_quick_switch, show_or_update_quick_switch, QuickSwitchRowData,
    QuickSwitchViewState, SlintThemeSpec, SlintUiEvent,
};
use crate::{wk_debug, wk_info};
use std::collections::HashMap;
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::time::Instant;
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HWND, LPARAM, POINT, WPARAM};
use windows::Win32::UI::WindowsAndMessaging::{
    FindWindowW, GetCursorPos, GetForegroundWindow, GetWindowLongPtrW, GetWindowThreadProcessId,
    IsWindow, PostMessageW, RegisterWindowMessageW, ShowWindowAsync, GWL_EXSTYLE, SW_MAXIMIZE,
    SW_MINIMIZE, SW_RESTORE, WS_EX_TOPMOST,
};

/// 供独立低级键盘钩子线程无锁读取的 QuickSwitch 打开状态与搜索锁定状态
pub static QUICK_SWITCH_IS_OPEN: AtomicBool = AtomicBool::new(false);
pub static QUICK_SWITCH_SEARCH_LOCKED: AtomicBool = AtomicBool::new(false);
pub static QUICK_SWITCH_HAS_QUERY: AtomicBool = AtomicBool::new(false);
static QS_HOST_BOUNDS_MSG: AtomicU32 = AtomicU32::new(0);

const MIN_WIN_W_DIP: f32 = 580.0;
const MIN_WIN_H_DIP: f32 = 340.0;

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
    icon_cache: HashMap<isize, (u32, u32, Vec<u8>)>,
    all_entries: Vec<SwitchEntry>,
    filtered: Vec<SwitchEntry>,
    selected_index: usize,
    search_query: String,
    search_mode: bool,
    opened_via_alt_tab: bool,
    scope: SwitchScope,
    scope_app_label: String,
    open: bool,
    opened_at: Option<Instant>,
    window_rect: RectI,
    card_dpi: u32,
    list_width_ratio: f32,
    preview_desktop_pos_override: Option<bool>,
    monitors_snapshot: Vec<MonitorInfo>,
}

impl QuickSwitchManager {
    pub fn new(_control_hwnd: HWND) -> Self {
        Self {
            icon_cache: HashMap::new(),
            all_entries: Vec::new(),
            filtered: Vec::new(),
            selected_index: 0,
            search_query: String::new(),
            search_mode: false,
            opened_via_alt_tab: false,
            scope: SwitchScope::GlobalAll,
            scope_app_label: String::new(),
            open: false,
            opened_at: None,
            window_rect: RectI::from_xywh(200, 140, 1080, 620),
            card_dpi: 96,
            list_width_ratio: 0.30,
            preview_desktop_pos_override: None,
            monitors_snapshot: Vec::new(),
        }
    }

    pub fn set_control_hwnd(&mut self, _hwnd: HWND) {}

    pub fn is_open(&self) -> bool {
        self.open
    }

    pub fn has_active_animation(&self) -> bool {
        false // 由 Slint 原生硬件加速管线平滑驱动，主线程 0 定时轮询
    }

    pub fn tick_animation(
        &mut self,
        _cfg: &WindowKitConfig,
        _gfx: Option<&SharedGraphics>,
    ) -> bool {
        false
    }

    fn effective_desktop_preview(&self, cfg: &WindowKitConfig) -> bool {
        self.preview_desktop_pos_override
            .unwrap_or(cfg.quick_switch.preview_in_desktop_position)
    }

    /// 轨道 A：响应全局 Alt+Tab / Alt+Shift+Tab
    pub fn on_alt_tab_step(
        &mut self,
        reverse: bool,
        tray_windows: &[(HWND, String)],
        cfg: &WindowKitConfig,
        _gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) {
        if !self.open {
            self.open_card(
                true,
                reverse,
                SwitchScope::GlobalAll,
                tray_windows,
                cfg,
                trace_id,
            );
        } else {
            self.step_selection(if reverse { -1 } else { 1 }, cfg);
        }
    }

    /// 轨道 B：响应同应用切窗 Alt+` / Alt+Shift+`
    pub fn on_same_app_step(
        &mut self,
        reverse: bool,
        cfg: &WindowKitConfig,
        _gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) {
        if !self.open {
            self.open_card(true, reverse, SwitchScope::SameApp, &[], cfg, trace_id);
        } else {
            self.step_selection(if reverse { -1 } else { 1 }, cfg);
        }
    }

    /// Action 13: 程序化开关 QuickSwitch（默认锁定浏览态，方便鼠标点击与拼音筛选）
    pub fn toggle_programmatic(
        &mut self,
        tray_windows: &[(HWND, String)],
        cfg: &WindowKitConfig,
        _gfx: Option<&SharedGraphics>,
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
                trace_id,
            );
            false
        }
    }

    /// 主题或强调色热变更时实时同步已打开的卡片
    pub fn refresh_theme_if_open(&self, cfg: &WindowKitConfig) {
        if self.open {
            self.push_to_slint(cfg, false);
        }
    }

    /// 多屏拔插/分辨率改变自愈
    pub fn on_topology_changed(&mut self, cfg: &WindowKitConfig, _gfx: Option<&SharedGraphics>) {
        if !self.open {
            return;
        }
        self.monitors_snapshot = enumerate_monitors();
        let safe = ensure_window_rect_on_live_monitors(
            &cfg.quick_switch.custom_monitor_id,
            cfg.quick_switch.custom_rel_rect,
            self.window_rect.width(),
            self.window_rect.height(),
            self.monitors_snapshot.first(),
            &self.monitors_snapshot,
        );
        self.window_rect = safe;
        self.push_to_slint(cfg, true);
    }

    /// 同步搜索锁定态与查询非空态至原子变量，供独立低级钩子线程无锁判别 1-9 直达与字母入搜
    fn sync_hook_search_flags(&self) {
        QUICK_SWITCH_SEARCH_LOCKED.store(self.open && self.search_mode, Ordering::Release);
        QUICK_SWITCH_HAS_QUERY.store(
            self.open && !self.search_query.is_empty(),
            Ordering::Release,
        );
    }

    /// 响应 Alt 键抬起：若未进入搜索/浏览锁定态且由 Alt+Tab 或 Alt+` 打开，立即切换目标窗口
    pub fn on_alt_released(&mut self, trace_id: &str) -> QuickSwitchDelegatedAction {
        if !self.open || !self.opened_via_alt_tab || self.search_mode {
            return QuickSwitchDelegatedAction::None;
        }
        self.commit_selected_and_close(trace_id)
    }

    /// 接收来自独立低级键盘钩子线程的直通快捷按键（0 焦点竞争，彻底消灭 1-9 与按住 Alt 搜索失灵）
    pub fn handle_hook_key(
        &mut self,
        vk: u16,
        mod_mask: u32,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> QuickSwitchDelegatedAction {
        if !self.open {
            return QuickSwitchDelegatedAction::None;
        }
        let alt = (mod_mask & 1) != 0;
        let ctrl = (mod_mask & 2) != 0;
        let shift = (mod_mask & 4) != 0;

        // 1. 编号 1-9 直达（主键盘 0x31..=0x39 与小键盘 0x61..=0x69）
        let digit = if (0x31..=0x39).contains(&vk) {
            Some((vk - 0x30) as usize)
        } else if (0x61..=0x69).contains(&vk) {
            Some((vk - 0x60) as usize)
        } else {
            None
        };
        if let Some(d) = digit {
            if alt || ctrl || self.search_query.is_empty() {
                if d >= 1 && d - 1 < self.filtered.len() {
                    self.selected_index = d - 1;
                    return self.commit_selected_and_close(trace_id);
                }
            }
            return QuickSwitchDelegatedAction::None;
        }

        // 2. 导航 / 确认 / 取消
        match vk {
            0x26 /* VK_UP */ => {
                self.step_selection(-1, cfg);
                return QuickSwitchDelegatedAction::None;
            }
            0x28 /* VK_DOWN */ => {
                self.step_selection(1, cfg);
                return QuickSwitchDelegatedAction::None;
            }
            0x0D /* VK_RETURN */ => {
                return self.commit_selected_and_close(trace_id);
            }
            0x1B /* VK_ESCAPE */ => {
                if !self.search_query.is_empty() {
                    self.search_query.clear();
                    self.apply_filter(cfg);
                    self.sync_hook_search_flags();
                    self.push_to_slint(cfg, false);
                    return QuickSwitchDelegatedAction::None;
                }
                self.close_and_release(trace_id);
                return QuickSwitchDelegatedAction::Closed;
            }
            0x08 /* VK_BACK */ => {
                if !self.search_query.is_empty() {
                    self.search_query.pop();
                    self.search_mode = true;
                    self.apply_filter(cfg);
                    self.sync_hook_search_flags();
                    self.push_to_slint(cfg, false);
                    focus_quick_switch_search();
                }
                return QuickSwitchDelegatedAction::None;
            }
            0x20 /* VK_SPACE */ | 0xBF /* VK_OEM_2 '/' */ => {
                // 按住 Alt 时敲 Space 或 /：立即锁定搜索模式并强聚焦输入框，松手继续打中文/拼音！
                self.search_mode = true;
                self.sync_hook_search_flags();
                self.push_to_slint(cfg, false);
                focus_quick_switch_search();
                return QuickSwitchDelegatedAction::None;
            }
            _ => {}
        }

        // 3. 按住 Alt 或未聚焦搜索框时直接敲字母 A-Z：无缝进入锁定搜索态并写入首字母立即过滤！
        if !ctrl && (0x41..=0x5A).contains(&vk) {
            let ch = if shift {
                (vk as u8) as char
            } else {
                ((vk as u8) + 32) as char
            };
            self.search_mode = true;
            if self.search_query.chars().count() < 48 {
                self.search_query.push(ch);
            }
            self.apply_filter(cfg);
            self.sync_hook_search_flags();
            self.push_to_slint(cfg, false);
            focus_quick_switch_search();
        }
        QuickSwitchDelegatedAction::None
    }

    /// 处理 Slint QuickSwitchWindow 回传的全部键盘/鼠标/拖缩事件
    pub fn handle_slint_event(
        &mut self,
        ev: &SlintUiEvent,
        cfg: &mut WindowKitConfig,
        config_dir: &Path,
        trace_id: &str,
    ) -> QuickSwitchDelegatedAction {
        if !self.open {
            return QuickSwitchDelegatedAction::None;
        }
        match ev {
            SlintUiEvent::QsSelectRow(idx) => {
                let uidx = (*idx).max(0) as usize;
                if uidx < self.filtered.len() && uidx != self.selected_index {
                    self.selected_index = uidx;
                    self.push_to_slint(cfg, false);
                }
                QuickSwitchDelegatedAction::None
            }
            SlintUiEvent::QsActivateRow(idx) => {
                let uidx = (*idx).max(0) as usize;
                if uidx < self.filtered.len() {
                    self.selected_index = uidx;
                    return self.commit_selected_and_close(trace_id);
                }
                QuickSwitchDelegatedAction::None
            }
            SlintUiEvent::QsCloseRow(idx) => {
                let uidx = (*idx).max(0) as usize;
                self.search_mode = true;
                self.execute_row_action_on_index(uidx, RowActionKind::Close, cfg, trace_id);
                QuickSwitchDelegatedAction::None
            }
            SlintUiEvent::QsFilterChanged(text) => {
                self.search_mode = true;
                self.search_query = text.chars().take(48).collect();
                self.apply_filter(cfg);
                self.sync_hook_search_flags();
                self.push_to_slint(cfg, false);
                QuickSwitchDelegatedAction::None
            }
            SlintUiEvent::QsClearFilter => {
                self.search_query.clear();
                self.apply_filter(cfg);
                self.sync_hook_search_flags();
                self.push_to_slint(cfg, false);
                QuickSwitchDelegatedAction::None
            }
            SlintUiEvent::QsSearchEngaged => {
                self.search_mode = true;
                self.sync_hook_search_flags();
                self.push_to_slint(cfg, false);
                QuickSwitchDelegatedAction::None
            }
            SlintUiEvent::QsAction(act) => {
                self.search_mode = true;
                match act.as_str() {
                    "close" => {
                        self.execute_row_action_on_index(
                            self.selected_index,
                            RowActionKind::Close,
                            cfg,
                            trace_id,
                        );
                        QuickSwitchDelegatedAction::None
                    }
                    "minimize" => {
                        self.execute_row_action_on_index(
                            self.selected_index,
                            RowActionKind::Minimize,
                            cfg,
                            trace_id,
                        );
                        QuickSwitchDelegatedAction::None
                    }
                    "maximize" => {
                        self.show_cmd_selected(SW_MAXIMIZE, cfg);
                        QuickSwitchDelegatedAction::None
                    }
                    "restore" => {
                        self.show_cmd_selected(SW_RESTORE, cfg);
                        QuickSwitchDelegatedAction::None
                    }
                    "kill" => {
                        self.execute_row_action_on_index(
                            self.selected_index,
                            RowActionKind::Kill,
                            cfg,
                            trace_id,
                        );
                        QuickSwitchDelegatedAction::None
                    }
                    "new" => {
                        self.launch_new_instance_selected(trace_id);
                        QuickSwitchDelegatedAction::None
                    }
                    "pin" => {
                        if let Some(entry) = self.filtered.get(self.selected_index) {
                            if !entry.is_tray_hidden {
                                return QuickSwitchDelegatedAction::TogglePinWindow(
                                    entry.info.hwnd.0 as isize,
                                );
                            }
                        }
                        QuickSwitchDelegatedAction::None
                    }
                    "zone" => {
                        if let Some(entry) = self.filtered.get(self.selected_index) {
                            if !entry.is_tray_hidden {
                                return QuickSwitchDelegatedAction::MoveWindowToNextZone(
                                    entry.info.hwnd.0 as isize,
                                );
                            }
                        }
                        QuickSwitchDelegatedAction::None
                    }
                    "tray" => {
                        if let Some(entry) = self.filtered.get(self.selected_index).cloned() {
                            if entry.is_tray_hidden {
                                let raw = entry.info.hwnd.0 as isize;
                                self.close_and_release(trace_id);
                                return QuickSwitchDelegatedAction::RestoreTrayWindowAndClose(raw);
                            } else {
                                let hwnd_raw = entry.info.hwnd.0 as isize;
                                self.all_entries
                                    .retain(|e| e.info.hwnd.0 as isize != hwnd_raw);
                                self.apply_filter(cfg);
                                self.push_to_slint(cfg, false);
                                return QuickSwitchDelegatedAction::MinimizeWindowToTray(hwnd_raw);
                            }
                        }
                        QuickSwitchDelegatedAction::None
                    }
                    "toggle_desktop_preview" => {
                        let next = !self.effective_desktop_preview(cfg);
                        self.preview_desktop_pos_override = Some(next);
                        cfg.quick_switch.preview_in_desktop_position = next;
                        self.push_to_slint(cfg, false);
                        QuickSwitchDelegatedAction::None
                    }
                    _ => QuickSwitchDelegatedAction::None,
                }
            }
            SlintUiEvent::QsKeyCommand { key, shift, .. } => match key.as_str() {
                "up" => {
                    self.step_selection(-1, cfg);
                    QuickSwitchDelegatedAction::None
                }
                "down" => {
                    self.step_selection(1, cfg);
                    QuickSwitchDelegatedAction::None
                }
                "tab" => {
                    self.step_selection(if *shift { -1 } else { 1 }, cfg);
                    QuickSwitchDelegatedAction::None
                }
                "enter" => self.commit_selected_and_close(trace_id),
                "escape" => {
                    if !self.search_query.is_empty() {
                        self.search_query.clear();
                        self.apply_filter(cfg);
                        self.push_to_slint(cfg, false);
                        QuickSwitchDelegatedAction::None
                    } else {
                        self.close_and_release(trace_id);
                        QuickSwitchDelegatedAction::Closed
                    }
                }
                k if k.starts_with("digit_") => {
                    if let Ok(d) = k[6..].parse::<usize>() {
                        if (1..=9).contains(&d) && d - 1 < self.filtered.len() {
                            self.selected_index = d - 1;
                            return self.commit_selected_and_close(trace_id);
                        }
                    }
                    QuickSwitchDelegatedAction::None
                }
                _ => QuickSwitchDelegatedAction::None,
            },
            SlintUiEvent::QsDismissed => {
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
            SlintUiEvent::QsBoundsChanged {
                rect,
                list_width_ratio,
            } => {
                self.search_mode = true;
                self.window_rect = *rect;
                self.list_width_ratio = list_width_ratio.clamp(0.20, 0.52);
                cfg.quick_switch.list_width_ratio = self.list_width_ratio;
                if cfg.quick_switch.remember_bounds {
                    self.persist_window_bounds(*rect, cfg, config_dir, trace_id);
                }
                QuickSwitchDelegatedAction::None
            }
            _ => QuickSwitchDelegatedAction::None,
        }
    }

    fn show_cmd_selected(
        &mut self,
        cmd: windows::Win32::UI::WindowsAndMessaging::SHOW_WINDOW_CMD,
        cfg: &WindowKitConfig,
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
        self.push_to_slint(cfg, false);
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
        trace_id: &str,
    ) {
        let Some(entry) = self.filtered.get(idx).cloned() else {
            return;
        };
        if entry.is_tray_hidden {
            return;
        }
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
        self.push_to_slint(cfg, false);
        wk_info!(
            trace_id,
            "quickSwitch",
            "executed row action {:?} on hwnd=0x{:x}",
            kind,
            entry.info.hwnd.0 as isize
        );
    }

    fn open_card(
        &mut self,
        via_alt_tab: bool,
        reverse: bool,
        scope: SwitchScope,
        tray_windows: &[(HWND, String)],
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) {
        let monitors = enumerate_monitors();
        self.monitors_snapshot = monitors.clone();
        let mut cursor = POINT::default();
        let _ = unsafe { GetCursorPos(&mut cursor) };
        let active_mon = monitor_for_point(cursor.x, cursor.y, &monitors)
            .or_else(|| monitors.first().cloned());

        let include_min = match scope {
            SwitchScope::SameApp => true,
            SwitchScope::GlobalAll => cfg.quick_switch.include_minimized,
        };
        let mut raw_windows = enumerate_user_windows(include_min, &cfg.excluded_apps);

        if cfg.quick_switch.current_desktop_only {
            raw_windows.retain(|w| is_window_on_current_virtual_desktop(w.hwnd));
        }

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
            self.scope_app_label = fg_proc;
            raw_windows.retain(|w| {
                (!target_exe.is_empty() && w.exe_path.to_ascii_lowercase() == target_exe)
                    || (!target_proc.is_empty()
                        && w.process_name.to_ascii_lowercase() == target_proc)
            });
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

        // 一次性提取 32px 真彩色 RGBA8 图标至会话缓存（含 UWP AUMID 兜底，关闭即清空归零）
        self.icon_cache.clear();
        for entry in self.all_entries.iter().take(48) {
            let key = entry.info.hwnd.0 as isize;
            let aumid = get_window_aumid(entry.info.hwnd).unwrap_or_default();
            if let Some(rgba) =
                extract_window_icon_rgba8(entry.info.hwnd, &entry.info.exe_path, &aumid, 32)
            {
                self.icon_cache.insert(key, rgba);
            }
        }

        self.search_query.clear();
        self.search_mode = !via_alt_tab;
        self.opened_via_alt_tab = via_alt_tab;
        self.list_width_ratio = cfg.quick_switch.list_width_ratio.clamp(0.20, 0.52);
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

        let work = active_mon
            .as_ref()
            .map(|m| m.work_rect)
            .unwrap_or(RectI::new(0, 0, 1920, 1040));
        let dpi = active_mon.as_ref().map(|m| m.dpi).unwrap_or(96).max(96);
        self.card_dpi = dpi;
        let scale = dpi as f32 / 96.0;

        // 5/8 × 5/8 黄金比例视野（1:1 对标 AltTabio），记住拖缩后跨屏安全恢复
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

        let target_rect =
            if cfg.quick_switch.remember_bounds && cfg.quick_switch.custom_rel_rect.is_some() {
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

        self.window_rect = target_rect;
        self.open = true;
        self.opened_at = Some(Instant::now());
        QUICK_SWITCH_IS_OPEN.store(true, Ordering::Release);
        self.sync_hook_search_flags();

        self.push_to_slint(cfg, true);

        wk_info!(
            trace_id,
            "quickSwitch",
            "opened Slint QuickSwitch scope={:?} items={} size={}x{} dpi={}",
            scope,
            self.filtered.len(),
            self.window_rect.width(),
            self.window_rect.height(),
            self.card_dpi
        );
    }

    fn push_to_slint(&self, cfg: &WindowKitConfig, reposition_window: bool) {
        let mut rows = Vec::with_capacity(self.filtered.len());
        for (idx, entry) in self.filtered.iter().enumerate() {
            let hwnd_raw = entry.info.hwnd.0 as isize;
            let is_pinned = if !entry.is_tray_hidden {
                let ex = unsafe { GetWindowLongPtrW(entry.info.hwnd, GWL_EXSTYLE) as u32 };
                (ex & WS_EX_TOPMOST.0) != 0
            } else {
                false
            };
            let state_badge = if entry.is_tray_hidden {
                "TRAY".to_string()
            } else if is_pinned {
                "PIN".to_string()
            } else if entry.info.is_minimized {
                "MIN".to_string()
            } else {
                String::new()
            };
            rows.push(QuickSwitchRowData {
                index: idx as i32,
                title: entry.info.title.clone(),
                app_name: entry.info.process_name.clone(),
                icon_rgba: self.icon_cache.get(&hwnd_raw).cloned(),
                number_label: if idx < 9 {
                    (idx + 1).to_string()
                } else {
                    String::new()
                },
                is_minimized: entry.info.is_minimized,
                is_pinned,
                is_tray: entry.is_tray_hidden,
                state_badge,
            });
        }

        let (preview_target_hwnd, preview_target_minimized, preview_target_norm_rect) =
            if let Some(sel) = self.filtered.get(self.selected_index) {
                if sel.is_tray_hidden {
                    (0, true, None)
                } else {
                    let norm = monitor_for_hwnd(sel.info.hwnd, &self.monitors_snapshot)
                        .or_else(|| self.monitors_snapshot.first().cloned())
                        .map(|mon| {
                            let work = mon.work_rect;
                            let ww = work.width().max(1) as f32;
                            let wh = work.height().max(1) as f32;
                            let wr = if sel.info.is_minimized {
                                get_window_normal_placement_rect(sel.info.hwnd)
                                    .unwrap_or(sel.info.rect)
                            } else {
                                sel.info.rect
                            };
                            let nx = ((wr.left - work.left) as f32 / ww).clamp(0.0, 0.92);
                            let ny = ((wr.top - work.top) as f32 / wh).clamp(0.0, 0.92);
                            let nw = (wr.width() as f32 / ww).clamp(0.10, 1.0 - nx);
                            let nh = (wr.height() as f32 / wh).clamp(0.10, 1.0 - ny);
                            (nx, ny, nw, nh)
                        });
                    (sel.info.hwnd.0 as isize, sel.info.is_minimized, norm)
                }
            } else {
                (0, true, None)
            };

        let _ = show_or_update_quick_switch(QuickSwitchViewState {
            theme: SlintThemeSpec {
                dark: cfg.is_dark_theme(),
                accent_hex: cfg.accent_color.clone(),
            },
            window_rect: self.window_rect,
            dpi: self.card_dpi,
            items: rows,
            selected_index: self.selected_index as i32,
            filter_text: self.search_query.clone(),
            search_visible: true,
            search_locked: self.search_mode,
            show_numbers: cfg.quick_switch.show_numbers,
            show_app_names: cfg.quick_switch.show_app_names || cfg.quick_switch.show_process_badge,
            compact_list: cfg.quick_switch.compact_density,
            same_app_mode: self.scope == SwitchScope::SameApp,
            scope_title: self.scope_app_label.clone(),
            list_width_ratio: self.list_width_ratio,
            zh_ui: true,
            show_live_preview: cfg.quick_switch.show_live_preview,
            preview_in_desktop_position: self.effective_desktop_preview(cfg),
            preview_target_hwnd,
            preview_target_minimized,
            preview_target_norm_rect,
            reposition_window,
        });
    }

    fn persist_window_bounds(
        &mut self,
        rect: RectI,
        cfg: &mut WindowKitConfig,
        config_dir: &Path,
        trace_id: &str,
    ) {
        let monitors = enumerate_monitors();
        let cx = (rect.left + rect.right) / 2;
        let cy = (rect.top + rect.bottom) / 2;
        let Some(mon) = monitor_for_point(cx, cy, &monitors).or_else(|| monitors.first().cloned())
        else {
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
            wk_debug!(
                trace_id,
                "quickSwitch",
                "persisted custom bounds -> {}",
                out_path.display()
            );
        }
    }

    fn commit_selected_and_close(&mut self, trace_id: &str) -> QuickSwitchDelegatedAction {
        let target = self.filtered.get(self.selected_index).cloned();
        self.open = false;
        QUICK_SWITCH_IS_OPEN.store(false, Ordering::Release);

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
        self.icon_cache.clear();
        self.all_entries.clear();
        self.filtered.clear();
        self.search_query.clear();
        self.search_mode = false;
        self.open = false;
        QUICK_SWITCH_IS_OPEN.store(false, Ordering::Release);
        QUICK_SWITCH_SEARCH_LOCKED.store(false, Ordering::Release);
        QUICK_SWITCH_HAS_QUERY.store(false, Ordering::Release);
        hide_quick_switch();
        wk_debug!(
            trace_id,
            "quickSwitch",
            "closed Slint QuickSwitch and released icon cache"
        );
    }

    pub fn destroy(&mut self) {
        self.close_and_release("destroy");
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
        } else {
            self.selected_index = self.selected_index.min(self.filtered.len() - 1);
        }
    }

    fn step_selection(&mut self, delta: i32, cfg: &WindowKitConfig) {
        if self.filtered.is_empty() {
            return;
        }
        let len = self.filtered.len() as i32;
        self.selected_index = ((self.selected_index as i32 + delta).rem_euclid(len)) as usize;
        self.push_to_slint(cfg, false);
    }
}

impl Drop for QuickSwitchManager {
    fn drop(&mut self) {
        self.destroy();
    }
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
