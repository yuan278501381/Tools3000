// ─────────────────────────────────────────────────────────────────────────────
// features/multi_monitor.rs — 离屏窗口一键救援、跨屏等比瞬移与最小化到系统托盘
//                             支持 Explorer 重启 TaskbarCreated 自动重建托盘图标、
//                             托盘右键紧凑恢复菜单与无锁崩溃兜底全量还原，永不遗失窗口
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::WindowKitConfig;
use crate::core::constants::{TRAY_ICON_UID_BASE, WM_WK_INTERNAL_TRAY_CALLBACK};
use crate::core::geometry::{
    is_window_offscreen, rescue_rect_into_work_area, transform_rect_between_monitors, RectI,
};
use crate::core::topology::{
    is_window_on_current_virtual_desktop, move_window_to_current_virtual_desktop,
};
use crate::core::win_utils::{
    activate_window, enumerate_monitors, enumerate_user_windows, get_window_icon_safe,
    get_window_process_info, get_window_title_safe, get_window_visual_rect, is_hung_window,
    is_manageable_window, is_shell_taskbar_present, monitor_for_hwnd, monitor_for_point,
    place_window_visual_rect, register_tray_recovery_hwnd, resolve_foreground_or_target_hwnd,
    to_wide, unregister_tray_recovery_hwnd, SafeIcon,
};
use crate::{wk_info, wk_warn};
use std::collections::{HashMap, HashSet};
use std::mem::size_of;
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HWND, LPARAM, POINT, WPARAM};
use windows::Win32::UI::Shell::{
    Shell_NotifyIconW, NIF_ICON, NIF_MESSAGE, NIF_TIP, NIM_ADD, NIM_DELETE, NOTIFYICONDATAW,
};
use windows::Win32::UI::WindowsAndMessaging::{
    AppendMenuW, CreatePopupMenu, DestroyMenu, GetCursorPos, IsIconic, IsWindow, IsZoomed,
    LoadIconW, PostMessageW, SetForegroundWindow, ShowWindow, TrackPopupMenu, IDI_APPLICATION,
    MF_SEPARATOR, MF_STRING, SW_HIDE, SW_MAXIMIZE, SW_RESTORE, SW_SHOW, TPM_BOTTOMALIGN,
    TPM_LEFTALIGN, TPM_RETURNCMD, TPM_RIGHTBUTTON, WM_LBUTTONDBLCLK, WM_LBUTTONUP, WM_NULL,
    WM_RBUTTONUP,
};

const TRAY_MENU_CMD_RESTORE_ONE: u32 = 1001;
const TRAY_MENU_CMD_RESTORE_ALL: u32 = 1002;
/// 兼容旧版 Shell 的 NOTIFYICONDATAW_V3_SIZE 字节尺寸（x64 下去除 hBalloonIcon 后的 952 字节）
const NOTIFYICONDATAW_COMPAT_V3_SIZE: u32 = 952;

struct TrayMinimizedEntry {
    uid: u32,
    target_hwnd: HWND,
    tip: String,
    icon_guard: Option<SafeIcon>,
    was_maximized: bool,
}

pub struct MultiMonitorManager {
    control_hwnd: HWND,
    next_uid: u32,
    tray_entries: HashMap<u32, TrayMinimizedEntry>,
    centered_hwnds: HashSet<isize>,
}

impl MultiMonitorManager {
    pub fn new(control_hwnd: HWND) -> Self {
        Self {
            control_hwnd,
            next_uid: TRAY_ICON_UID_BASE,
            tray_entries: HashMap::new(),
            centered_hwnds: HashSet::new(),
        }
    }

    /// 响应 EVENT_OBJECT_SHOW：若开启 centerNewWindows，将新打开的普通窗口居中至当前光标所在显示器工作区
    pub fn maybe_center_new_window(
        &mut self,
        hwnd: HWND,
        cfg: &WindowKitConfig,
        muted: bool,
        trace_id: &str,
    ) -> bool {
        if muted
            || !cfg.features.multi_monitor
            || !cfg.multi_monitor.center_new_windows
            || hwnd.0.is_null()
        {
            return false;
        }
        let key = hwnd.0 as isize;
        if self.centered_hwnds.contains(&key) {
            return false;
        }
        if unsafe { IsIconic(hwnd).as_bool() || IsZoomed(hwnd).as_bool() } {
            return false;
        }
        if !is_manageable_window(hwnd, false, &cfg.excluded_apps) {
            return false;
        }
        let Some(vis) = get_window_visual_rect(hwnd) else {
            return false;
        };
        let monitors = enumerate_monitors();
        if monitors.is_empty() {
            return false;
        }
        let mut pt = POINT::default();
        let target_mon = if unsafe { GetCursorPos(&mut pt) }.is_ok() {
            monitor_for_point(pt.x, pt.y, &monitors)
        } else {
            monitor_for_hwnd(hwnd, &monitors)
        };
        let Some(mon) = target_mon else {
            return false;
        };
        let work = mon.work_rect;
        // 若窗口尺寸已接近全屏工作区（>= 92% 宽且 >= 92% 高）则不干预，防误伤全屏/伪最大化窗口
        if vis.width() * 100 >= work.width().max(1) * 92
            && vis.height() * 100 >= work.height().max(1) * 92
        {
            self.centered_hwnds.insert(key);
            return false;
        }

        let target_w = vis.width().min(work.width());
        let target_h = vis.height().min(work.height());
        let target_left = work.left + (work.width() - target_w) / 2;
        let target_top = work.top + (work.height() - target_h) / 2;
        let target_rect = RectI::from_xywh(target_left, target_top, target_w, target_h);

        if self.centered_hwnds.len() > 256 {
            self.centered_hwnds
                .retain(|&raw| unsafe { IsWindow(HWND(raw as *mut std::ffi::c_void)).as_bool() });
        }
        self.centered_hwnds.insert(key);

        if (vis.left - target_rect.left).abs() <= 4 && (vis.top - target_rect.top).abs() <= 4 {
            return false;
        }
        if place_window_visual_rect(hwnd, target_rect) {
            let (_, proc_name, _) = get_window_process_info(hwnd);
            wk_info!(
                trace_id,
                "multiMonitor",
                "centered new window hwnd=0x{:x} proc={} on {} ({},{},{},{})",
                key,
                proc_name,
                mon.friendly_name,
                target_rect.left,
                target_rect.top,
                target_rect.right,
                target_rect.bottom
            );
            return true;
        }
        false
    }

    pub fn on_window_destroyed(&mut self, hwnd: HWND) {
        self.centered_hwnds.remove(&(hwnd.0 as isize));
    }

    pub fn set_control_hwnd(&mut self, hwnd: HWND) {
        self.control_hwnd = hwnd;
    }

    pub fn tray_minimized_count(&self) -> usize {
        self.tray_entries.len()
    }

    /// Action 9: 将全部掉出可见屏幕或残留在已断开显示器上的正常窗口拉回当前/主显示器居中
    pub fn rescue_offscreen_windows(&self, cfg: &WindowKitConfig, trace_id: &str) -> usize {
        let monitors = enumerate_monitors();
        if monitors.is_empty() {
            return 0;
        }
        let work_areas: Vec<RectI> = monitors.iter().map(|m| m.work_rect).collect();
        let primary_work = monitors
            .iter()
            .find(|m| m.is_primary)
            .unwrap_or(&monitors[0])
            .work_rect;

        let windows = enumerate_user_windows(false, &cfg.excluded_apps);
        let mut rescued = 0usize;
        for win in windows {
            if win.is_minimized || win.is_maximized {
                continue;
            }
            let vis = get_window_visual_rect(win.hwnd).unwrap_or(win.rect);
            if is_window_offscreen(vis, &work_areas) {
                let target_rect = rescue_rect_into_work_area(vis, primary_work);
                if place_window_visual_rect(win.hwnd, target_rect) {
                    rescued += 1;
                    wk_info!(
                        trace_id,
                        "multiMonitor",
                        "rescued offscreen hwnd=0x{:x} proc={} to ({},{},{},{})",
                        win.hwnd.0 as isize,
                        win.process_name,
                        target_rect.left,
                        target_rect.top,
                        target_rect.right,
                        target_rect.bottom
                    );
                }
            }
        }
        rescued
    }

    /// 将当前前台窗口等比瞬移到下一台 (+1) 或上一台 (-1) 显示器（兼容混合 DPI、负坐标副屏与最大化状态）
    pub fn move_foreground_to_monitor(
        &self,
        delta: i32,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> bool {
        let monitors = enumerate_monitors();
        if monitors.len() < 2 {
            return false;
        }
        let fg = resolve_foreground_or_target_hwnd();
        if fg.0.is_null() || !is_manageable_window(fg, false, &cfg.excluded_apps) {
            return false;
        }
        let Some(src_mon) = monitor_for_hwnd(fg, &monitors) else {
            return false;
        };
        let cur_idx = monitors
            .iter()
            .position(|m| m.hmonitor == src_mon.hmonitor)
            .unwrap_or(0) as i32;
        let next_idx = (cur_idx + delta).rem_euclid(monitors.len() as i32) as usize;
        let dst_mon = &monitors[next_idx];

        let was_maximized = unsafe { IsZoomed(fg).as_bool() };
        let src_rect = get_window_visual_rect(fg).unwrap_or(src_mon.work_rect);
        let dst_rect = transform_rect_between_monitors(
            src_rect,
            src_mon.work_rect,
            dst_mon.work_rect,
            cfg.multi_monitor.keep_proportional_size,
        );

        let moved = place_window_visual_rect(fg, dst_rect);
        if moved && was_maximized {
            unsafe {
                let _ = ShowWindow(fg, SW_MAXIMIZE);
            }
        }
        if moved {
            wk_info!(
                trace_id,
                "multiMonitor",
                "moved hwnd=0x{:x} from {} to {} (max={})",
                fg.0 as isize,
                src_mon.friendly_name,
                dst_mon.friendly_name,
                was_maximized
            );
        }
        moved
    }

    /// Action 11: 将当前前台正常窗口隐藏并收纳到 Windows 系统托盘（使用真实应用图标与标题提示）
    pub fn minimize_foreground_to_tray(&mut self, cfg: &WindowKitConfig, trace_id: &str) -> bool {
        if !cfg.multi_monitor.enable_tray_minimize {
            return false;
        }
        let fg = resolve_foreground_or_target_hwnd();
        self.minimize_hwnd_to_tray(fg, cfg, trace_id)
    }

    /// 将指定窗口收纳到系统托盘（供快捷键、Workspaces 三态清场与 Quick Switch 原地操作共用）
    pub fn minimize_hwnd_to_tray(
        &mut self,
        target: HWND,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> bool {
        if target.0.is_null()
            || self.control_hwnd.0.is_null()
            || !is_manageable_window(target, false, &cfg.excluded_apps)
        {
            return false;
        }
        if self.tray_entries.values().any(|e| e.target_hwnd == target) {
            return false;
        }
        // MECE 安全门禁：若当前桌面不存在 Shell_TrayWnd（如 Explorer 重启或无外壳隔离桌面），
        // 严禁贸然隐藏用户窗口，防止窗口失踪！
        if !is_shell_taskbar_present() {
            wk_warn!(
                trace_id,
                "multiMonitor",
                "skipped minimize-to-tray for hwnd=0x{:x}: Shell_TrayWnd not present on current desktop (protecting user window from being hidden without tray access)",
                target.0 as isize
            );
            return false;
        }

        let title = get_window_title_safe(target);
        let (_, proc_name, exe_path) = get_window_process_info(target);
        let tip = if title.is_empty() {
            format!("Tools3000 Tray - {proc_name}")
        } else {
            title
        };

        let icon_guard = get_window_icon_safe(target, &exe_path, false);
        let uid = self.next_uid;
        self.next_uid = self.next_uid.wrapping_add(1).max(TRAY_ICON_UID_BASE);

        if !self.register_tray_icon(uid, &tip, icon_guard.as_ref(), trace_id, target) {
            return false;
        }

        let was_maximized = unsafe { IsZoomed(target).as_bool() };
        register_tray_recovery_hwnd(target);
        unsafe {
            let _ = ShowWindow(target, SW_HIDE);
        }

        self.tray_entries.insert(
            uid,
            TrayMinimizedEntry {
                uid,
                target_hwnd: target,
                tip,
                icon_guard,
                was_maximized,
            },
        );
        wk_info!(
            trace_id,
            "multiMonitor",
            "minimized hwnd=0x{:x} proc={} to tray uid={}",
            target.0 as isize,
            proc_name,
            uid
        );
        true
    }

    /// 根据句柄恢复某个已收纳到托盘的窗口（供 Quick Switch 直接一键捞回托盘窗）
    pub fn restore_by_hwnd(&mut self, hwnd: HWND, trace_id: &str) -> bool {
        let uid = self
            .tray_entries
            .iter()
            .find_map(|(&u, e)| (e.target_hwnd == hwnd).then_some(u));
        match uid {
            Some(u) => self.restore_by_uid(u, trace_id),
            None => false,
        }
    }

    /// 列出所有当前收纳在系统托盘的窗口摘要（hwnd, tip），供 Quick Switch 窗口中枢联动展示
    pub fn list_tray_windows(&self) -> Vec<(HWND, String)> {
        self.tray_entries
            .values()
            .filter(|e| unsafe { IsWindow(e.target_hwnd).as_bool() })
            .map(|e| (e.target_hwnd, e.tip.clone()))
            .collect()
    }

    /// 响应 Explorer 重启广播的 TaskbarCreated 消息：自动重建所有已收纳窗口的托盘图标
    pub fn on_taskbar_created(&mut self, trace_id: &str) {
        if self.tray_entries.is_empty() || self.control_hwnd.0.is_null() {
            return;
        }
        let mut rebuilt = 0usize;
        let keys: Vec<u32> = self.tray_entries.keys().copied().collect();
        for uid in keys {
            if let Some(entry) = self.tray_entries.get(&uid) {
                if unsafe { IsWindow(entry.target_hwnd).as_bool() }
                    && self.register_tray_icon(
                        entry.uid,
                        &entry.tip,
                        entry.icon_guard.as_ref(),
                        trace_id,
                        entry.target_hwnd,
                    )
                {
                    rebuilt += 1;
                }
            }
        }
        if rebuilt > 0 {
            wk_info!(
                trace_id,
                "multiMonitor",
                "rebuilt {} tray-minimized icon(s) after TaskbarCreated",
                rebuilt
            );
        }
    }

    fn register_tray_icon(
        &self,
        uid: u32,
        tip: &str,
        icon_guard: Option<&SafeIcon>,
        trace_id: &str,
        target_hwnd: HWND,
    ) -> bool {
        let fallback_icon = unsafe { LoadIconW(None, IDI_APPLICATION).unwrap_or_default() };
        let hicon = icon_guard.map(|i| i.hicon).unwrap_or(fallback_icon);

        let mut nid = NOTIFYICONDATAW {
            cbSize: size_of::<NOTIFYICONDATAW>() as u32,
            hWnd: self.control_hwnd,
            uID: uid,
            uFlags: NIF_ICON | NIF_MESSAGE | NIF_TIP,
            uCallbackMessage: WM_WK_INTERNAL_TRAY_CALLBACK,
            hIcon: hicon,
            ..Default::default()
        };
        let tip_utf16: Vec<u16> = tip.encode_utf16().take(127).collect();
        for (i, ch) in tip_utf16.into_iter().enumerate() {
            nid.szTip[i] = ch;
        }

        // 先尝试清理同 UID 可能遗留的旧图标，再执行三级降级注册（V4 -> V3 兼容尺寸 -> 系统默认图标）
        unsafe {
            let _ = Shell_NotifyIconW(NIM_DELETE, &nid);
        }
        let mut added = unsafe { Shell_NotifyIconW(NIM_ADD, &nid).as_bool() };
        if !added && (size_of::<NOTIFYICONDATAW>() as u32) > NOTIFYICONDATAW_COMPAT_V3_SIZE {
            nid.cbSize = NOTIFYICONDATAW_COMPAT_V3_SIZE;
            added = unsafe { Shell_NotifyIconW(NIM_ADD, &nid).as_bool() };
            nid.cbSize = size_of::<NOTIFYICONDATAW>() as u32;
        }
        if !added && nid.hIcon != fallback_icon {
            nid.hIcon = fallback_icon;
            added = unsafe { Shell_NotifyIconW(NIM_ADD, &nid).as_bool() };
        }
        if !added {
            let err = unsafe { windows::Win32::Foundation::GetLastError() };
            wk_warn!(
                trace_id,
                "multiMonitor",
                "Shell_NotifyIconW(NIM_ADD) failed for hwnd=0x{:x} uid={} err={:?}",
                target_hwnd.0 as isize,
                uid,
                err
            );
        }
        added
    }

    /// 响应系统托盘图标点击回调：左键单击/双击直接瞬时恢复，右键弹出原生紧凑恢复菜单
    pub fn handle_tray_callback(&mut self, uid: u32, mouse_msg: u32, trace_id: &str) {
        match mouse_msg {
            WM_LBUTTONUP | WM_LBUTTONDBLCLK => {
                self.restore_by_uid(uid, trace_id);
            }
            WM_RBUTTONUP => {
                self.show_tray_context_menu(uid, trace_id);
            }
            _ => {}
        }
    }

    fn show_tray_context_menu(&mut self, uid: u32, trace_id: &str) {
        let Some(entry) = self.tray_entries.get(&uid) else {
            return;
        };
        let total = self.tray_entries.len();
        let label_one = format!("Restore: {}", truncate_chars(&entry.tip, 36));
        let label_all = format!("Restore All ({total})");

        unsafe {
            let Ok(hmenu) = CreatePopupMenu() else {
                self.restore_by_uid(uid, trace_id);
                return;
            };
            let w_one = to_wide(&label_one);
            let _ = AppendMenuW(hmenu, MF_STRING, TRAY_MENU_CMD_RESTORE_ONE as usize, PCWSTR(w_one.as_ptr()));
            if total > 1 {
                let _ = AppendMenuW(hmenu, MF_SEPARATOR, 0, PCWSTR::null());
                let w_all = to_wide(&label_all);
                let _ = AppendMenuW(hmenu, MF_STRING, TRAY_MENU_CMD_RESTORE_ALL as usize, PCWSTR(w_all.as_ptr()));
            }
            let mut pt = POINT::default();
            let _ = GetCursorPos(&mut pt);
            let _ = SetForegroundWindow(self.control_hwnd);
            let cmd = TrackPopupMenu(
                hmenu,
                TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
                pt.x,
                pt.y,
                0,
                self.control_hwnd,
                None,
            );
            let _ = PostMessageW(self.control_hwnd, WM_NULL, WPARAM(0), LPARAM(0));
            let _ = DestroyMenu(hmenu);

            match cmd.0 as u32 {
                TRAY_MENU_CMD_RESTORE_ONE => {
                    self.restore_by_uid(uid, trace_id);
                }
                TRAY_MENU_CMD_RESTORE_ALL => {
                    self.restore_all_tray(trace_id);
                }
                _ => {}
            }
        }
    }

    pub fn restore_by_uid(&mut self, uid: u32, trace_id: &str) -> bool {
        let Some(entry) = self.tray_entries.remove(&uid) else {
            return false;
        };
        self.delete_tray_icon(entry.uid);
        unregister_tray_recovery_hwnd(entry.target_hwnd);
        restore_single_window(&entry);
        wk_info!(
            trace_id,
            "multiMonitor",
            "restored hwnd=0x{:x} from tray uid={}",
            entry.target_hwnd.0 as isize,
            uid
        );
        true
    }

    /// Action 12: 立即恢复所有托盘隐藏窗口并清空托盘图标
    pub fn restore_all_tray(&mut self, trace_id: &str) -> usize {
        let entries: Vec<TrayMinimizedEntry> = self.tray_entries.drain().map(|(_, v)| v).collect();
        let mut restored = 0usize;
        for entry in entries {
            self.delete_tray_icon(entry.uid);
            unregister_tray_recovery_hwnd(entry.target_hwnd);
            if restore_single_window(&entry) {
                restored += 1;
            }
        }
        if restored > 0 {
            wk_info!(
                trace_id,
                "multiMonitor",
                "restored {} tray-minimized window(s)",
                restored
            );
        }
        restored
    }

    /// 清理用户已在外部被关闭的失效托盘图标
    pub fn prune_closed_tray_windows(&mut self) {
        let dead: Vec<u32> = self
            .tray_entries
            .iter()
            .filter_map(|(&uid, e)| {
                if unsafe { !IsWindow(e.target_hwnd).as_bool() } {
                    Some(uid)
                } else {
                    None
                }
            })
            .collect();
        for uid in dead {
            if let Some(entry) = self.tray_entries.remove(&uid) {
                self.delete_tray_icon(entry.uid);
                unregister_tray_recovery_hwnd(entry.target_hwnd);
            }
        }
    }

    fn delete_tray_icon(&self, uid: u32) {
        if self.control_hwnd.0.is_null() {
            return;
        }
        let nid = NOTIFYICONDATAW {
            cbSize: size_of::<NOTIFYICONDATAW>() as u32,
            hWnd: self.control_hwnd,
            uID: uid,
            ..Default::default()
        };
        unsafe {
            let _ = Shell_NotifyIconW(NIM_DELETE, &nid);
        }
    }
}

impl Drop for MultiMonitorManager {
    fn drop(&mut self) {
        self.restore_all_tray("drop");
    }
}

fn restore_single_window(entry: &TrayMinimizedEntry) -> bool {
    unsafe {
        if !IsWindow(entry.target_hwnd).as_bool() || is_hung_window(entry.target_hwnd) {
            return false;
        }
        if !is_window_on_current_virtual_desktop(entry.target_hwnd) {
            let _ = move_window_to_current_virtual_desktop(entry.target_hwnd);
        }
        let _ = ShowWindow(entry.target_hwnd, SW_SHOW);
        if entry.was_maximized {
            let _ = ShowWindow(entry.target_hwnd, SW_MAXIMIZE);
        } else if IsIconic(entry.target_hwnd).as_bool() {
            let _ = ShowWindow(entry.target_hwnd, SW_RESTORE);
        }
        activate_window(entry.target_hwnd);
        true
    }
}

fn truncate_chars(s: &str, max_chars: usize) -> String {
    if s.chars().count() <= max_chars {
        s.to_string()
    } else {
        let mut out: String = s.chars().take(max_chars.saturating_sub(1)).collect();
        out.push('…');
        out
    }
}
