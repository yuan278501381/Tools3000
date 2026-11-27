// ─────────────────────────────────────────────────────────────────────────────
// core/win_utils.rs — 安全无阻塞 Win32 系统原语封装
//                     包含：崩溃兜底托盘恢复阵列、非阻塞跨进程窗口探测、
//                     显示器 EDID 枚举、DWM 两阶段精准定位、内存工作集冷路径修剪
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::constants::{FOREIGN_SEND_TIMEOUT_MS, MAX_TRAY_RECOVERY_SLOTS};
use crate::core::filter::{is_eligible_user_window, WindowCandidateMeta};
use crate::core::geometry::{compensate_dwm_frame_bounds, RectI};
use crate::core::json::JsonValue;
use std::collections::HashMap;
use std::ffi::c_void;
use std::mem::size_of;
use std::path::Path;
use std::sync::atomic::{AtomicIsize, Ordering};
use windows::core::{GUID, PCWSTR, PWSTR};
use windows::Win32::Devices::Display::{
    DisplayConfigGetDeviceInfo, GetDisplayConfigBufferSizes, QueryDisplayConfig,
    DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
    DISPLAYCONFIG_DEVICE_INFO_HEADER, DISPLAYCONFIG_MODE_INFO, DISPLAYCONFIG_PATH_INFO,
    DISPLAYCONFIG_SOURCE_DEVICE_NAME, DISPLAYCONFIG_TARGET_DEVICE_NAME, QDC_ONLY_ACTIVE_PATHS,
};
use windows::Win32::Foundation::{CloseHandle, BOOL, HWND, LPARAM, LRESULT, POINT, RECT, TRUE, WPARAM};
use windows::Win32::Graphics::Dwm::{
    DwmGetWindowAttribute, DwmSetWindowAttribute, DWMWA_CLOAKED, DWMWA_EXTENDED_FRAME_BOUNDS,
    DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND, DWMWCP_ROUNDSMALL,
};
use windows::Win32::Graphics::Gdi::{
    EnumDisplayMonitors, GetMonitorInfoW, MonitorFromPoint, MonitorFromWindow, HDC, HMONITOR,
    MONITORINFOEXW, MONITOR_DEFAULTTONEAREST,
};
use windows::Win32::System::Com::{CoCreateInstance, CLSCTX_ALL};
use windows::Win32::System::Diagnostics::Debug::{
    SetUnhandledExceptionFilter, EXCEPTION_CONTINUE_SEARCH, EXCEPTION_POINTERS,
};
use windows::Win32::System::Memory::{GetProcessHeap, HeapCompact, HEAP_FLAGS};
use windows::Win32::System::ProcessStatus::{
    GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS, PROCESS_MEMORY_COUNTERS_EX,
};
use windows::Win32::System::Registry::{
    RegCloseKey, RegOpenKeyExW, RegQueryValueExW, HKEY, HKEY_CURRENT_USER, KEY_READ, REG_SZ,
    REG_VALUE_TYPE,
};
use windows::Win32::System::Threading::{
    AttachThreadInput, GetCurrentProcess, GetCurrentProcessId, GetCurrentThreadId, GetGuiResources,
    GetProcessHandleCount, OpenProcess, QueryFullProcessImageNameW, SetProcessWorkingSetSize,
    TerminateProcess, GR_GDIOBJECTS, GR_USEROBJECTS, PROCESS_NAME_FORMAT,
    PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_TERMINATE,
};
use windows::Win32::UI::HiDpi::{GetDpiForMonitor, GetDpiForWindow, MDT_EFFECTIVE_DPI};
use windows::Win32::UI::Shell::{
    IVirtualDesktopManager, SHGetFileInfoW, VirtualDesktopManager, SHFILEINFOW, SHGFI_ICON,
    SHGFI_LARGEICON, SHGFI_SMALLICON,
};
use windows::Win32::UI::WindowsAndMessaging::{
    AllowSetForegroundWindow, BringWindowToTop, DestroyIcon, EnumWindows, GetClassLongPtrW,
    GetClassNameW, GetForegroundWindow, GetWindow, GetWindowLongPtrW, GetWindowRect,
    GetWindowTextW, GetWindowThreadProcessId, IsHungAppWindow, IsIconic, IsWindow,
    IsWindowVisible, IsZoomed, PostMessageW, SendMessageTimeoutW, SetForegroundWindow,
    SetWindowPos, ShowWindow, ASFW_ANY, GCLP_HICON, GCLP_HICONSM, GWL_EXSTYLE, GW_OWNER, HICON,
    HWND_TOP, ICON_BIG, ICON_SMALL, ICON_SMALL2, SC_MINIMIZE, SMTO_ABORTIFHUNG, SMTO_NORMAL,
    SWP_NOACTIVATE, SWP_NOMOVE, SWP_NOSIZE, SWP_NOZORDER, SW_RESTORE, SW_SHOW, WM_CLOSE,
    WM_GETICON, WM_GETTEXT, WM_SYSCOMMAND, WS_EX_APPWINDOW, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW,
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. 崩溃安全托盘恢复阵列（无锁静态 AtomicIsize，极端崩溃时保证用户窗口不丢失）
// ─────────────────────────────────────────────────────────────────────────────

static TRAY_RECOVERY_HWNDS: [AtomicIsize; MAX_TRAY_RECOVERY_SLOTS] =
    [const { AtomicIsize::new(0) }; MAX_TRAY_RECOVERY_SLOTS];

/// 崩溃安全透明度恢复阵列（确保极端崩溃时已调透明的用户窗口不残留半透明态）
static OPACITY_RECOVERY_HWNDS: [AtomicIsize; MAX_TRAY_RECOVERY_SLOTS] =
    [const { AtomicIsize::new(0) }; MAX_TRAY_RECOVERY_SLOTS];

/// 显式动作目标句柄覆写槽（支持宿主/Lua/CLI/隔离端到端测试精准指定目标 HWND，默认 0 时严格回落 GetForegroundWindow）
static ACTION_TARGET_HWND_OVERRIDE: AtomicIsize = AtomicIsize::new(0);

pub struct ActionTargetOverrideGuard;

impl ActionTargetOverrideGuard {
    pub fn enter(hwnd: Option<HWND>) -> Self {
        ACTION_TARGET_HWND_OVERRIDE.store(
            hwnd.map(|h| h.0 as isize).unwrap_or(0),
            Ordering::Release,
        );
        Self
    }
}

impl Drop for ActionTargetOverrideGuard {
    fn drop(&mut self) {
        ACTION_TARGET_HWND_OVERRIDE.store(0, Ordering::Release);
    }
}

pub fn resolve_foreground_or_target_hwnd() -> HWND {
    let raw = ACTION_TARGET_HWND_OVERRIDE.load(Ordering::Acquire);
    if raw != 0 {
        let h = HWND(raw as *mut std::ffi::c_void);
        if unsafe { IsWindow(h).as_bool() } {
            return h;
        }
    }
    unsafe { GetForegroundWindow() }
}

/// 登记一个最小化到托盘的窗口句柄（无锁 O(64)）
pub fn register_tray_recovery_hwnd(hwnd: HWND) {
    let raw = hwnd.0 as isize;
    if raw == 0 {
        return;
    }
    for slot in &TRAY_RECOVERY_HWNDS {
        if slot.load(Ordering::Acquire) == raw {
            return;
        }
    }
    for slot in &TRAY_RECOVERY_HWNDS {
        if slot
            .compare_exchange(0, raw, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            return;
        }
    }
}

/// 注销已恢复的托盘窗口句柄（无锁）
pub fn unregister_tray_recovery_hwnd(hwnd: HWND) {
    let raw = hwnd.0 as isize;
    if raw == 0 {
        return;
    }
    for slot in &TRAY_RECOVERY_HWNDS {
        let _ = slot.compare_exchange(raw, 0, Ordering::AcqRel, Ordering::Acquire);
    }
}

/// 登记被调整透明度的窗口句柄（无锁）
pub fn register_opacity_recovery_hwnd(hwnd: HWND) {
    let raw = hwnd.0 as isize;
    if raw == 0 {
        return;
    }
    for slot in &OPACITY_RECOVERY_HWNDS {
        if slot.load(Ordering::Acquire) == raw {
            return;
        }
    }
    for slot in &OPACITY_RECOVERY_HWNDS {
        if slot
            .compare_exchange(0, raw, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            return;
        }
    }
}

/// 注销已恢复 100% 不透明的窗口句柄（无锁）
pub fn unregister_opacity_recovery_hwnd(hwnd: HWND) {
    let raw = hwnd.0 as isize;
    if raw == 0 {
        return;
    }
    for slot in &OPACITY_RECOVERY_HWNDS {
        let _ = slot.compare_exchange(raw, 0, Ordering::AcqRel, Ordering::Acquire);
    }
}

/// 紧急无锁恢复所有托管隐藏窗口与透明度（可供 SEH 异常过滤器、panic hook 与退出清理安全调用）
pub fn emergency_restore_all_tray_windows() {
    for slot in &TRAY_RECOVERY_HWNDS {
        let raw = slot.swap(0, Ordering::AcqRel);
        if raw != 0 {
            let hwnd = HWND(raw as *mut c_void);
            unsafe {
                if IsWindow(hwnd).as_bool() {
                    let _ = ShowWindow(hwnd, SW_SHOW);
                    if IsIconic(hwnd).as_bool() {
                        let _ = ShowWindow(hwnd, SW_RESTORE);
                    }
                }
            }
        }
    }
    for slot in &OPACITY_RECOVERY_HWNDS {
        let raw = slot.swap(0, Ordering::AcqRel);
        if raw != 0 {
            let hwnd = HWND(raw as *mut c_void);
            unsafe {
                if IsWindow(hwnd).as_bool() && !IsHungAppWindow(hwnd).as_bool() {
                    let _ = windows::Win32::UI::WindowsAndMessaging::SetLayeredWindowAttributes(
                        hwnd,
                        windows::Win32::Foundation::COLORREF(0),
                        255,
                        windows::Win32::UI::WindowsAndMessaging::LWA_ALPHA,
                    );
                }
            }
        }
    }
}

unsafe extern "system" fn unhandled_exception_restore_filter(
    _info: *const EXCEPTION_POINTERS,
) -> i32 {
    emergency_restore_all_tray_windows();
    EXCEPTION_CONTINUE_SEARCH
}

/// 安装进程级 SEH 异常兜底与 Rust panic hook，确保任何异常都不会遗失托盘隐藏窗口
pub fn install_crash_recovery_guards() {
    unsafe {
        SetUnhandledExceptionFilter(Some(unhandled_exception_restore_filter));
    }
    let prev_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        emergency_restore_all_tray_windows();
        prev_hook(info);
    }));
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. 基础宽字符串与安全 RAII 图标句柄
// ─────────────────────────────────────────────────────────────────────────────

pub fn to_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

pub fn from_wide_nul(buf: &[u16]) -> String {
    let len = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    String::from_utf16_lossy(&buf[..len])
}

pub fn rect_from_win32(r: RECT) -> RectI {
    RectI::new(r.left, r.top, r.right, r.bottom)
}

/// 自动释放拥有权的 HICON 包装（防止 GDI/USER 对象泄漏）
pub struct SafeIcon {
    pub hicon: HICON,
    owned: bool,
}

impl SafeIcon {
    pub fn borrowed(hicon: HICON) -> Self {
        Self {
            hicon,
            owned: false,
        }
    }

    pub fn owned(hicon: HICON) -> Self {
        Self { hicon, owned: true }
    }
}

impl Drop for SafeIcon {
    fn drop(&mut self) {
        if self.owned && !self.hicon.is_invalid() {
            unsafe {
                let _ = DestroyIcon(self.hicon);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. 显示器枚举（含 EDID 稳定硬件键、真实友好名称、工作区、物理区与 DPI）
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct MonitorInfo {
    pub hmonitor: isize,
    pub id: String,
    pub device_name: String,
    pub friendly_name: String,
    pub physical_rect: RectI,
    pub work_rect: RectI,
    pub dpi: u32,
    pub scale_percent: u32,
    pub is_primary: bool,
}

impl MonitorInfo {
    pub fn to_json(&self) -> JsonValue {
        let mut obj = JsonValue::object();
        obj.insert("id", JsonValue::String(self.id.clone()));
        obj.insert("name", JsonValue::String(self.friendly_name.clone()));
        obj.insert("deviceName", JsonValue::String(self.device_name.clone()));
        obj.insert("friendlyName", JsonValue::String(self.friendly_name.clone()));
        obj.insert("width", JsonValue::from_i32(self.physical_rect.width()));
        obj.insert("height", JsonValue::from_i32(self.physical_rect.height()));
        obj.insert("physicalRect", self.physical_rect.to_json());
        obj.insert("workRect", self.work_rect.to_json());
        obj.insert("dpi", JsonValue::from_u64(self.dpi as u64));
        obj.insert(
            "scalePercent",
            JsonValue::from_u64(self.scale_percent as u64),
        );
        obj.insert("isPrimary", JsonValue::Bool(self.is_primary));
        obj
    }
}

/// 查询 DisplayConfig 获取每个 GDI 显示设备名对应的真实 EDID 键与硬件友好名
fn query_display_friendly_map() -> HashMap<String, (String, String)> {
    let mut map = HashMap::new();
    unsafe {
        let mut path_count = 0u32;
        let mut mode_count = 0u32;
        if GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &mut path_count, &mut mode_count)
            .is_err()
            || path_count == 0
        {
            return map;
        }
        let mut paths = vec![DISPLAYCONFIG_PATH_INFO::default(); path_count as usize];
        let mut modes = vec![DISPLAYCONFIG_MODE_INFO::default(); mode_count as usize];
        if QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &mut path_count,
            paths.as_mut_ptr(),
            &mut mode_count,
            modes.as_mut_ptr(),
            None,
        )
        .is_err()
        {
            return map;
        }

        for path in paths.iter().take(path_count as usize) {
            let mut src_name = DISPLAYCONFIG_SOURCE_DEVICE_NAME {
                header: DISPLAYCONFIG_DEVICE_INFO_HEADER {
                    r#type: DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                    size: size_of::<DISPLAYCONFIG_SOURCE_DEVICE_NAME>() as u32,
                    adapterId: path.sourceInfo.adapterId,
                    id: path.sourceInfo.id,
                },
                ..Default::default()
            };
            if DisplayConfigGetDeviceInfo(&mut src_name.header) != 0 {
                continue;
            }
            let gdi_name = from_wide_nul(&src_name.viewGdiDeviceName);
            if gdi_name.is_empty() {
                continue;
            }

            let mut target_name = DISPLAYCONFIG_TARGET_DEVICE_NAME {
                header: DISPLAYCONFIG_DEVICE_INFO_HEADER {
                    r#type: DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
                    size: size_of::<DISPLAYCONFIG_TARGET_DEVICE_NAME>() as u32,
                    adapterId: path.targetInfo.adapterId,
                    id: path.targetInfo.id,
                },
                ..Default::default()
            };
            if DisplayConfigGetDeviceInfo(&mut target_name.header) != 0 {
                continue;
            }

            let friendly = from_wide_nul(&target_name.monitorFriendlyDeviceName);
            let dev_path = from_wide_nul(&target_name.monitorDevicePath);
            let stable_id = if !dev_path.is_empty() {
                // 提取稳定硬件设备路径键（去除易变前缀）
                dev_path
                    .trim_start_matches(r"\\?\")
                    .to_ascii_uppercase()
            } else if target_name.edidManufactureId != 0 || target_name.edidProductCodeId != 0 {
                format!(
                    "EDID_{:04X}_{:04X}_{}",
                    target_name.edidManufactureId, target_name.edidProductCodeId, path.targetInfo.id
                )
            } else {
                gdi_name.clone()
            };
            let display_label = if friendly.trim().is_empty() {
                gdi_name.clone()
            } else {
                friendly
            };
            map.insert(gdi_name.to_ascii_uppercase(), (stable_id, display_label));
        }
    }
    map
}

/// 枚举当前全部激活显示器（主屏排首位，其余按从左到右、从上到下稳定排序）
pub fn enumerate_monitors() -> Vec<MonitorInfo> {
    struct EnumCtx {
        list: Vec<MonitorInfo>,
        friendly_map: HashMap<String, (String, String)>,
    }

    unsafe extern "system" fn callback(
        hmon: HMONITOR,
        _hdc: HDC,
        _rc: *mut RECT,
        lparam: LPARAM,
    ) -> BOOL {
        let ctx = &mut *(lparam.0 as *mut EnumCtx);
        let mut mi = MONITORINFOEXW::default();
        mi.monitorInfo.cbSize = size_of::<MONITORINFOEXW>() as u32;
        if !GetMonitorInfoW(hmon, &mut mi.monitorInfo).as_bool() {
            return TRUE;
        }
        let device_name = from_wide_nul(&mi.szDevice);
        let (stable_id, friendly_name) = ctx
            .friendly_map
            .get(&device_name.to_ascii_uppercase())
            .cloned()
            .unwrap_or_else(|| (device_name.clone(), device_name.clone()));

        let mut dpi_x = 96u32;
        let mut dpi_y = 96u32;
        let _ = GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &mut dpi_x, &mut dpi_y);
        if dpi_x == 0 {
            dpi_x = 96;
        }
        let scale_percent = ((dpi_x as f64 / 96.0) * 100.0).round() as u32;

        ctx.list.push(MonitorInfo {
            hmonitor: hmon.0 as isize,
            id: stable_id,
            device_name,
            friendly_name,
            physical_rect: rect_from_win32(mi.monitorInfo.rcMonitor),
            work_rect: rect_from_win32(mi.monitorInfo.rcWork),
            dpi: dpi_x,
            scale_percent,
            is_primary: (mi.monitorInfo.dwFlags & 1) != 0,
        });
        TRUE
    }

    let mut ctx = EnumCtx {
        list: Vec::new(),
        friendly_map: query_display_friendly_map(),
    };
    unsafe {
        let _ = EnumDisplayMonitors(
            HDC::default(),
            None,
            Some(callback),
            LPARAM(&mut ctx as *mut EnumCtx as isize),
        );
    }

    ctx.list.sort_by(|a, b| {
        b.is_primary
            .cmp(&a.is_primary)
            .then_with(|| a.physical_rect.left.cmp(&b.physical_rect.left))
            .then_with(|| a.physical_rect.top.cmp(&b.physical_rect.top))
    });
    // 美化未提供 EDID 型号名的内置屏/通用显示器，杜绝直接裸露 `\\.\DISPLAY1` 原始设备串
    for (idx, mon) in ctx.list.iter_mut().enumerate() {
        let trimmed = mon.friendly_name.trim();
        if trimmed.is_empty() || trimmed.starts_with(r"\\.\") {
            mon.friendly_name = format!("Display {}", idx + 1);
        }
    }
    ctx.list
}

pub fn monitors_to_json(monitors: &[MonitorInfo]) -> JsonValue {
    let mut root = JsonValue::object();
    root.insert("count", JsonValue::from_usize(monitors.len()));
    let mut arr = JsonValue::array();
    for m in monitors {
        arr.push(m.to_json());
    }
    root.insert("monitors", arr);
    root
}

pub fn monitor_for_hwnd(hwnd: HWND, monitors: &[MonitorInfo]) -> Option<MonitorInfo> {
    if monitors.is_empty() {
        return None;
    }
    let hmon = unsafe { MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) };
    let raw = hmon.0 as isize;
    monitors
        .iter()
        .find(|m| m.hmonitor == raw)
        .or_else(|| monitors.iter().find(|m| m.is_primary))
        .or_else(|| monitors.first())
        .cloned()
}

pub fn monitor_for_point(x: i32, y: i32, monitors: &[MonitorInfo]) -> Option<MonitorInfo> {
    if monitors.is_empty() {
        return None;
    }
    let hmon = unsafe { MonitorFromPoint(POINT { x, y }, MONITOR_DEFAULTTONEAREST) };
    let raw = hmon.0 as isize;
    monitors
        .iter()
        .find(|m| m.hmonitor == raw)
        .or_else(|| monitors.iter().find(|m| m.is_primary))
        .or_else(|| monitors.first())
        .cloned()
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. 非阻塞跨进程窗口属性探测与安全枚举（严禁阻塞 SendMessageW）
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct WindowInfo {
    pub hwnd: HWND,
    pub pid: u32,
    pub title: String,
    #[allow(dead_code)]
    pub class_name: String,
    pub process_name: String,
    pub exe_path: String,
    pub rect: RectI,
    pub monitor_id: String,
    pub is_maximized: bool,
    pub is_minimized: bool,
}

impl WindowInfo {
    #[allow(dead_code)]
    pub fn to_workspace_json(&self) -> JsonValue {
        let mut obj = JsonValue::object();
        obj.insert("hwnd", JsonValue::from_i64(self.hwnd.0 as isize as i64));
        obj.insert("pid", JsonValue::from_u64(self.pid as u64));
        obj.insert("title", JsonValue::String(self.title.clone()));
        obj.insert("className", JsonValue::String(self.class_name.clone()));
        obj.insert("processName", JsonValue::String(self.process_name.clone()));
        obj.insert("exePath", JsonValue::String(self.exe_path.clone()));
        obj.insert("monitorId", JsonValue::String(self.monitor_id.clone()));
        obj.insert("rect", self.rect.to_json());
        obj.insert("maximized", JsonValue::Bool(self.is_maximized));
        obj.insert("minimized", JsonValue::Bool(self.is_minimized));
        obj
    }
}

pub fn is_hung_window(hwnd: HWND) -> bool {
    unsafe { !IsWindow(hwnd).as_bool() || IsHungAppWindow(hwnd).as_bool() }
}

pub fn is_window_cloaked(hwnd: HWND) -> bool {
    let mut cloaked: u32 = 0;
    let hr = unsafe {
        DwmGetWindowAttribute(
            hwnd,
            DWMWA_CLOAKED,
            &mut cloaked as *mut u32 as *mut c_void,
            size_of::<u32>() as u32,
        )
    };
    hr.is_ok() && cloaked != 0
}

pub fn get_window_class_name(hwnd: HWND) -> String {
    let mut buf = [0u16; 256];
    let len = unsafe { GetClassNameW(hwnd, &mut buf) };
    if len <= 0 {
        return String::new();
    }
    String::from_utf16_lossy(&buf[..len as usize])
}

/// 读取窗口标题（先 IsHungAppWindow 检查，再使用 SendMessageTimeoutW + SMTO_ABORTIFHUNG，
/// 若遇 UIPI 提权窗口被拒则回退至跨进程直读内核缓存的 GetWindowTextW，绝不阻塞假死进程）
pub fn get_window_title_safe(hwnd: HWND) -> String {
    if is_hung_window(hwnd) {
        return String::new();
    }
    let mut buf = [0u16; 512];
    let mut copied: usize = 0;
    let ok = unsafe {
        SendMessageTimeoutW(
            hwnd,
            WM_GETTEXT,
            WPARAM(buf.len()),
            LPARAM(buf.as_mut_ptr() as isize),
            SMTO_ABORTIFHUNG | SMTO_NORMAL,
            FOREIGN_SEND_TIMEOUT_MS,
            Some(&mut copied),
        )
    };
    if ok != LRESULT(0) && copied > 0 {
        let len = copied.min(buf.len().saturating_sub(1));
        return from_wide_nul(&buf[..len]);
    }
    // UIPI 拒绝跨完整性级 WM_GETTEXT 时回退直读内核窗口标题缓存（Windows 跨进程不发送消息）
    let mut pid = 0u32;
    unsafe {
        let _ = GetWindowThreadProcessId(hwnd, Some(&mut pid));
        if pid != GetCurrentProcessId() {
            let len = GetWindowTextW(hwnd, &mut buf);
            if len > 0 {
                return String::from_utf16_lossy(&buf[..len as usize]);
            }
        }
    }
    String::new()
}

/// 查询窗口所属进程完整路径与可执行文件名
pub fn get_window_process_info(hwnd: HWND) -> (u32, String, String) {
    let mut pid = 0u32;
    unsafe {
        let _ = GetWindowThreadProcessId(hwnd, Some(&mut pid));
    }
    if pid == 0 {
        return (0, String::new(), String::new());
    }
    let exe_path = query_process_exe_path(pid);
    let process_name = Path::new(&exe_path)
        .file_name()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_string();
    (pid, process_name, exe_path)
}

pub fn query_process_exe_path(pid: u32) -> String {
    if pid == 0 {
        return String::new();
    }
    unsafe {
        let Ok(handle) = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid) else {
            return String::new();
        };
        let mut buf = [0u16; 1024];
        let mut size = buf.len() as u32;
        let ok = QueryFullProcessImageNameW(
            handle,
            PROCESS_NAME_FORMAT(0),
            PWSTR(buf.as_mut_ptr()),
            &mut size,
        )
        .is_ok();
        let _ = CloseHandle(handle);
        if ok && size > 0 {
            String::from_utf16_lossy(&buf[..size as usize])
        } else {
            String::new()
        }
    }
}

/// 严格采用 SendMessageTimeoutW (60ms, SMTO_ABORTIFHUNG) 获取目标窗口图标，绝不阻塞卡死
pub fn get_window_icon_safe(hwnd: HWND, exe_path: &str, large: bool) -> Option<SafeIcon> {
    if is_hung_window(hwnd) {
        return get_exe_shell_icon(exe_path, large);
    }

    let try_msg = |icon_type: usize| -> Option<HICON> {
        let mut res: usize = 0;
        let ok = unsafe {
            SendMessageTimeoutW(
                hwnd,
                WM_GETICON,
                WPARAM(icon_type),
                LPARAM(0),
                SMTO_ABORTIFHUNG | SMTO_NORMAL,
                FOREIGN_SEND_TIMEOUT_MS,
                Some(&mut res),
            )
        };
        if ok != LRESULT(0) && res != 0 {
            Some(HICON(res as *mut c_void))
        } else {
            None
        }
    };

    let order = if large {
        [ICON_BIG as usize, ICON_SMALL2 as usize, ICON_SMALL as usize]
    } else {
        [ICON_SMALL2 as usize, ICON_SMALL as usize, ICON_BIG as usize]
    };

    for kind in order {
        if let Some(h) = try_msg(kind) {
            return Some(SafeIcon::borrowed(h));
        }
    }

    unsafe {
        let primary_cls = if large { GCLP_HICON } else { GCLP_HICONSM };
        let secondary_cls = if large { GCLP_HICONSM } else { GCLP_HICON };
        let h1 = GetClassLongPtrW(hwnd, primary_cls);
        if h1 != 0 {
            return Some(SafeIcon::borrowed(HICON(h1 as *mut c_void)));
        }
        let h2 = GetClassLongPtrW(hwnd, secondary_cls);
        if h2 != 0 {
            return Some(SafeIcon::borrowed(HICON(h2 as *mut c_void)));
        }
    }

    get_exe_shell_icon(exe_path, large).or_else(|| {
        get_window_aumid(hwnd).and_then(|aumid| get_uwp_shell_icon(&aumid))
    })
}

/// 通过 Windows PropertyStore 获取 UWP / 打包应用的 System.AppUserModel.ID
pub fn get_window_aumid(hwnd: HWND) -> Option<String> {
    use windows::core::BSTR;
    use windows::Win32::UI::Shell::PropertiesSystem::{
        IPropertyStore, PSGetPropertyKeyFromName, SHGetPropertyStoreForWindow, PROPERTYKEY,
    };
    if hwnd.0.is_null() {
        return None;
    }
    unsafe {
        let store: IPropertyStore = SHGetPropertyStoreForWindow(hwnd).ok()?;
        let mut key = PROPERTYKEY::default();
        let name = to_wide("System.AppUserModel.ID");
        PSGetPropertyKeyFromName(PCWSTR(name.as_ptr()), &mut key).ok()?;
        let val = store.GetValue(&key).ok()?;
        let s = BSTR::try_from(&val).ok()?.to_string();
        let trimmed = s.trim();
        if trimmed.is_empty() {
            None
        } else {
            Some(trimmed.to_string())
        }
    }
}

/// 参考 AltTabio：通过 `shell:AppsFolder\{AUMID}` 与 `IExtractIconW` 提取 48px 原生 UWP 图标
pub fn get_uwp_shell_icon(aumid: &str) -> Option<SafeIcon> {
    use windows::Win32::UI::Shell::{
        BHID_SFUIObject, IExtractIconW, IShellItem, SHCreateItemFromParsingName,
    };
    let clean = aumid.trim();
    if clean.is_empty() || clean.contains('\0') {
        return None;
    }
    let path = to_wide(&format!("shell:AppsFolder\\{clean}"));
    unsafe {
        let item: IShellItem =
            SHCreateItemFromParsingName(PCWSTR(path.as_ptr()), None).ok()?;
        let extractor: IExtractIconW = item.BindToHandler(None, &BHID_SFUIObject).ok()?;
        let mut location = vec![0u16; 1024];
        let mut index = 0i32;
        let mut flags = 0u32;
        extractor
            .GetIconLocation(0, &mut location, &mut index, &mut flags)
            .ok()?;
        let mut icon = HICON::default();
        let res = extractor.Extract(
            PCWSTR(location.as_ptr()),
            index as u32,
            Some(&mut icon),
            None,
            48,
        );
        if res.is_ok() && !icon.is_invalid() {
            Some(SafeIcon::owned(icon))
        } else {
            if !icon.is_invalid() {
                let _ = DestroyIcon(icon);
            }
            None
        }
    }
}

/// 将任意 Win32 HICON 安全光栅化为 32bpp Top-Down BGRA 像素缓冲（零裸 DIB 指针解引用）
///
/// 全程仅通过 Win32 安全 API（`CreateDIBSection` + `PatBlt` + `DrawIconEx` + `GetDIBits`）
/// 写入托管 `Vec<u8>`，绝不直接解引用 `CreateDIBSection` 输出指针，彻底消除 CWE-476/825
/// 静态告警，同时确保跨进程 / 高 DPI / MSIX / 旧式无 Alpha 图标 100% 稳定提取。
fn rasterize_hicon_to_bgra(hicon: HICON, target_px: u32) -> Option<(u32, Vec<u8>)> {
    use windows::Win32::Graphics::Gdi::{
        CreateCompatibleDC, CreateDIBSection, DeleteDC, DeleteObject, GetDIBits, PatBlt,
        SelectObject, BITMAPINFO, BITMAPINFOHEADER, BI_RGB, BLACKNESS, DIB_RGB_COLORS,
    };
    use windows::Win32::UI::WindowsAndMessaging::{DrawIconEx, DI_NORMAL};

    if hicon.is_invalid() {
        return None;
    }
    let sz = target_px.clamp(16, 64);
    let stride = sz * 4;
    let byte_len = (stride * sz) as usize;

    unsafe {
        let hdc = CreateCompatibleDC(None);
        if hdc.is_invalid() {
            return None;
        }
        let mut bmi = BITMAPINFO {
            bmiHeader: BITMAPINFOHEADER {
                biSize: size_of::<BITMAPINFOHEADER>() as u32,
                biWidth: sz as i32,
                biHeight: -(sz as i32), // top-down
                biPlanes: 1,
                biBitCount: 32,
                biCompression: BI_RGB.0,
                ..Default::default()
            },
            ..Default::default()
        };
        let mut unused_bits: *mut c_void = std::ptr::null_mut();
        let hbmp = match CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &mut unused_bits, None, 0) {
            Ok(b) if !b.is_invalid() => b,
            _ => {
                let _ = DeleteDC(hdc);
                return None;
            }
        };
        let _ = unused_bits;

        let old = SelectObject(hdc, hbmp);
        // 显式清空为全零 BGRA（透明底）后再光栅化 HICON
        let _ = PatBlt(hdc, 0, 0, sz as i32, sz as i32, BLACKNESS);
        let drawn = DrawIconEx(
            hdc,
            0,
            0,
            hicon,
            sz as i32,
            sz as i32,
            0,
            None,
            DI_NORMAL,
        )
        .is_ok();
        // 遵循 Win32 GetDIBits 契约：读取像素前必须先将位图反选出 DC
        SelectObject(hdc, old);

        let mut pixels = vec![0u8; byte_len];
        let copied_lines = if drawn {
            GetDIBits(
                hdc,
                hbmp,
                0,
                sz,
                Some(pixels.as_mut_ptr().cast()),
                &mut bmi,
                DIB_RGB_COLORS,
            )
        } else {
            0
        };
        let _ = DeleteObject(hbmp);
        let _ = DeleteDC(hdc);

        if !drawn || copied_lines != sz as i32 {
            return None;
        }

        // 若旧式图标仅绘制了 RGB 未写入 Alpha 通道（全部 alpha==0 但存在非零像素），补全不透明 Alpha=255
        let any_alpha = pixels.chunks_exact(4).any(|px| px[3] != 0);
        if !any_alpha {
            let mut any_rgb = false;
            for px in pixels.chunks_exact_mut(4) {
                if px[0] != 0 || px[1] != 0 || px[2] != 0 {
                    px[3] = 255;
                    any_rgb = true;
                }
            }
            if !any_rgb {
                return None;
            }
        }

        Some((sz, pixels))
    }
}

/// 将任意 Win32 HICON（含 1/4/8/32bpp、高 DPI、MSIX/UWP）光栅化为直通 RGBA8 像素缓冲区（供 Slint 零拷贝渲染真实彩色应用图标）
pub fn hicon_to_rgba8(hicon: HICON, target_px: u32) -> Option<(u32, u32, Vec<u8>)> {
    let (sz, mut pixels) = rasterize_hicon_to_bgra(hicon, target_px)?;
    for px in pixels.chunks_exact_mut(4) {
        px.swap(0, 2); // BGRA -> RGBA
    }
    Some((sz, sz, pixels))
}

/// 抽取指定窗口的高清 RGBA8 彩色图标（三级兜底：窗口 HICON -> UWP/MSIX Shell -> EXE Shell）
pub fn extract_window_icon_rgba8(
    hwnd: HWND,
    exe_path: &str,
    aumid: &str,
    target_px: u32,
) -> Option<(u32, u32, Vec<u8>)> {
    if let Some(ico) = get_window_icon_safe(hwnd, exe_path, true) {
        if let Some(rgba) = hicon_to_rgba8(ico.hicon, target_px) {
            return Some(rgba);
        }
    }
    if !aumid.trim().is_empty() {
        if let Some(ico) = get_uwp_shell_icon(aumid) {
            if let Some(rgba) = hicon_to_rgba8(ico.hicon, target_px) {
                return Some(rgba);
            }
        }
    }
    if !exe_path.trim().is_empty() {
        if let Some(ico) = get_exe_shell_icon(exe_path, true) {
            if let Some(rgba) = hicon_to_rgba8(ico.hicon, target_px) {
                return Some(rgba);
            }
        }
    }
    None
}

/// 将任意 Win32 HICON（含 1/4/8/32bpp、高 DPI、MSIX/UWP）通过安全 BGRA 光栅化 + WIC WritePixels
/// 100% 无损编码为紧凑 `data:image/png;base64,...`（供前端工作区微缩舞台展示真实彩色图标）
pub fn hicon_to_png_data_url(hicon: HICON, target_px: u32) -> Option<String> {
    use windows::Win32::Graphics::Imaging::{
        CLSID_WICImagingFactory, GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA,
        IWICBitmapEncoder, IWICImagingFactory, WICBitmapEncoderNoCache,
    };
    use windows::Win32::System::Com::StructuredStorage::{
        CreateStreamOnHGlobal, GetHGlobalFromStream,
    };
    use windows::Win32::System::Com::{
        CoInitializeEx, CoUninitialize, CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED,
        STATFLAG_NONAME, STATSTG,
    };
    use windows::Win32::System::Memory::{GlobalLock, GlobalUnlock};

    struct ComScope(bool);
    impl Drop for ComScope {
        fn drop(&mut self) {
            if self.0 {
                unsafe {
                    CoUninitialize();
                }
            }
        }
    }

    let (sz, pixels) = rasterize_hicon_to_bgra(hicon, target_px)?;
    let stride = sz * 4;

    unsafe {
        // 使用 WIC PNG Container Format (GUID_ContainerFormatPng) + WritePixels 编码为内存流
        // 自动兜底线程 COM 初始化（S_OK/S_FALSE 成对卸载；RPC_E_CHANGED_MODE 沿用已有 MTA）
        let _com = ComScope(CoInitializeEx(None, COINIT_APARTMENTTHREADED).is_ok());
        let wic: IWICImagingFactory =
            CoCreateInstance(&CLSID_WICImagingFactory, None, CLSCTX_INPROC_SERVER).ok()?;
        let stream = CreateStreamOnHGlobal(
            windows::Win32::Foundation::HGLOBAL(std::ptr::null_mut()),
            true,
        )
        .ok()?;
        let encoder: IWICBitmapEncoder = wic
            .CreateEncoder(&GUID_ContainerFormatPng, std::ptr::null())
            .ok()?;
        encoder.Initialize(&stream, WICBitmapEncoderNoCache).ok()?;
        let mut frame = None;
        encoder.CreateNewFrame(&mut frame, std::ptr::null_mut()).ok()?;
        let frame = frame?;
        frame.Initialize(None).ok()?;
        frame.SetSize(sz, sz).ok()?;
        let mut fmt = GUID_WICPixelFormat32bppBGRA;
        frame.SetPixelFormat(&mut fmt).ok()?;
        frame.WritePixels(sz, stride, &pixels).ok()?;
        frame.Commit().ok()?;
        encoder.Commit().ok()?;

        let mut stat = STATSTG::default();
        stream.Stat(&mut stat, STATFLAG_NONAME).ok()?;
        let len = (stat.cbSize as usize).min(64 * 1024);
        if len == 0 {
            return None;
        }
        let hglobal = GetHGlobalFromStream(&stream).ok()?;
        let non_null = std::ptr::NonNull::new(GlobalLock(hglobal).cast::<u8>())?;
        let bytes = std::slice::from_raw_parts(non_null.as_ptr(), len).to_vec();
        let _ = GlobalUnlock(hglobal);

        Some(format!("data:image/png;base64,{}", encode_base64(&bytes)))
    }
}

/// 窗口图标 PNG 三级兜底抽取链：窗口自身 HICON -> UWP/MSIX Shell AppsFolder -> EXE Shell Icon
pub fn extract_window_icon_png_data_url(
    hwnd: HWND,
    exe_path: &str,
    aumid: &str,
    target_px: u32,
) -> Option<String> {
    if let Some(ico) = get_window_icon_safe(hwnd, exe_path, true) {
        if let Some(png) = hicon_to_png_data_url(ico.hicon, target_px) {
            return Some(png);
        }
    }
    if !aumid.trim().is_empty() {
        if let Some(ico) = get_uwp_shell_icon(aumid) {
            if let Some(png) = hicon_to_png_data_url(ico.hicon, target_px) {
                return Some(png);
            }
        }
    }
    if !exe_path.trim().is_empty() {
        if let Some(ico) = get_exe_shell_icon(exe_path, true) {
            if let Some(png) = hicon_to_png_data_url(ico.hicon, target_px) {
                return Some(png);
            }
        }
    }
    None
}

/// 零依赖标准 RFC4648 Base64 编码器
pub fn encode_base64(input: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(((input.len() + 2) / 3) * 4);
    let mut i = 0;
    while i + 3 <= input.len() {
        let n = ((input[i] as u32) << 16) | ((input[i + 1] as u32) << 8) | (input[i + 2] as u32);
        out.push(TABLE[((n >> 18) & 63) as usize] as char);
        out.push(TABLE[((n >> 12) & 63) as usize] as char);
        out.push(TABLE[((n >> 6) & 63) as usize] as char);
        out.push(TABLE[(n & 63) as usize] as char);
        i += 3;
    }
    let rem = input.len() - i;
    if rem == 1 {
        let n = (input[i] as u32) << 16;
        out.push(TABLE[((n >> 18) & 63) as usize] as char);
        out.push(TABLE[((n >> 12) & 63) as usize] as char);
        out.push('=');
        out.push('=');
    } else if rem == 2 {
        let n = ((input[i] as u32) << 16) | ((input[i + 1] as u32) << 8);
        out.push(TABLE[((n >> 18) & 63) as usize] as char);
        out.push(TABLE[((n >> 12) & 63) as usize] as char);
        out.push(TABLE[((n >> 6) & 63) as usize] as char);
        out.push('=');
    }
    out
}

/// 探测指定 PID 进程是否以管理员权限运行（若因高完整性级别 UIPI 拒绝访问则视为管理员）
pub fn is_process_elevated(pid: u32) -> bool {
    use windows::Win32::Foundation::{GetLastError, ERROR_ACCESS_DENIED, HANDLE};
    use windows::Win32::Security::{GetTokenInformation, TokenElevation, TOKEN_ELEVATION, TOKEN_QUERY};
    use windows::Win32::System::Threading::OpenProcessToken;

    if pid <= 4 {
        return true;
    }
    unsafe {
        let proc = match OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid) {
            Ok(h) => h,
            Err(_) => {
                return GetLastError() == ERROR_ACCESS_DENIED;
            }
        };
        let mut token = HANDLE::default();
        let opened = OpenProcessToken(proc, TOKEN_QUERY, &mut token).is_ok();
        let _ = CloseHandle(proc);
        if !opened {
            return GetLastError() == ERROR_ACCESS_DENIED;
        }
        let mut elev = TOKEN_ELEVATION::default();
        let mut ret_len = 0u32;
        let ok = GetTokenInformation(
            token,
            TokenElevation,
            Some(&mut elev as *mut TOKEN_ELEVATION as *mut c_void),
            size_of::<TOKEN_ELEVATION>() as u32,
            &mut ret_len,
        )
        .is_ok();
        let _ = CloseHandle(token);
        ok && elev.TokenIsElevated != 0
    }
}

/// 安全读取同位宽可读进程 PEB 命令行并剥离首个可执行文件路径，提取启动 CLI 参数
/// 对受保护进程/提权跨权进程/异常结构安全降级返回空串，绝不崩溃
pub fn query_process_command_line_args(pid: u32) -> String {
    use windows::Win32::System::Diagnostics::Debug::ReadProcessMemory;
    use windows::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};
    use windows::Win32::System::Threading::PROCESS_VM_READ;

    if pid <= 4 {
        return String::new();
    }

    #[repr(C)]
    struct ProcessBasicInfo {
        reserved1: usize,
        peb_base_address: usize,
        reserved2: [usize; 2],
        unique_process_id: usize,
        reserved3: usize,
    }
    #[repr(C)]
    struct UnicodeString {
        length: u16,
        max_length: u16,
        #[cfg(target_pointer_width = "64")]
        _pad: u32,
        buffer: usize,
    }

    type NtQueryInfoFn = unsafe extern "system" fn(
        windows::Win32::Foundation::HANDLE,
        u32,
        *mut c_void,
        u32,
        *mut u32,
    ) -> i32;

    unsafe {
        let Ok(proc) = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, false, pid)
        else {
            return String::new();
        };
        let ntdll = to_wide("ntdll.dll");
        let query_fn: Option<NtQueryInfoFn> = GetModuleHandleW(PCWSTR(ntdll.as_ptr()))
            .ok()
            .and_then(|m| GetProcAddress(m, windows::core::s!("NtQueryInformationProcess")))
            .map(|p| std::mem::transmute(p));
        let Some(nt_query) = query_fn else {
            let _ = CloseHandle(proc);
            return String::new();
        };

        let mut pbi = ProcessBasicInfo {
            reserved1: 0,
            peb_base_address: 0,
            reserved2: [0; 2],
            unique_process_id: 0,
            reserved3: 0,
        };
        let mut ret = 0u32;
        let status = nt_query(
            proc,
            0, // ProcessBasicInformation
            &mut pbi as *mut ProcessBasicInfo as *mut c_void,
            size_of::<ProcessBasicInfo>() as u32,
            &mut ret,
        );
        if status != 0 || pbi.peb_base_address == 0 {
            let _ = CloseHandle(proc);
            return String::new();
        }

        // 64 位 Windows (x64 & ARM64) PEB->ProcessParameters 偏移恒为 0x20；RTL_USER_PROCESS_PARAMETERS->CommandLine 偏移恒为 0x70
        #[cfg(target_pointer_width = "64")]
        const PEB_PARAMS_OFFSET: usize = 0x20;
        #[cfg(target_pointer_width = "64")]
        const PARAMS_CMDLINE_OFFSET: usize = 0x70;
        #[cfg(target_pointer_width = "32")]
        const PEB_PARAMS_OFFSET: usize = 0x10;
        #[cfg(target_pointer_width = "32")]
        const PARAMS_CMDLINE_OFFSET: usize = 0x40;

        let mut params_addr: usize = 0;
        let ok1 = ReadProcessMemory(
            proc,
            (pbi.peb_base_address + PEB_PARAMS_OFFSET) as *const c_void,
            &mut params_addr as *mut usize as *mut c_void,
            size_of::<usize>(),
            None,
        )
        .is_ok();
        if !ok1 || params_addr == 0 {
            let _ = CloseHandle(proc);
            return String::new();
        }

        let mut cmd_us = UnicodeString {
            length: 0,
            max_length: 0,
            #[cfg(target_pointer_width = "64")]
            _pad: 0,
            buffer: 0,
        };
        let ok2 = ReadProcessMemory(
            proc,
            (params_addr + PARAMS_CMDLINE_OFFSET) as *const c_void,
            &mut cmd_us as *mut UnicodeString as *mut c_void,
            size_of::<UnicodeString>(),
            None,
        )
        .is_ok();
        if !ok2 || cmd_us.length == 0 || cmd_us.buffer == 0 {
            let _ = CloseHandle(proc);
            return String::new();
        }

        let char_count = ((cmd_us.length as usize) / 2).min(2048);
        let mut buf = vec![0u16; char_count];
        let ok3 = ReadProcessMemory(
            proc,
            cmd_us.buffer as *const c_void,
            buf.as_mut_ptr() as *mut c_void,
            char_count * 2,
            None,
        )
        .is_ok();
        let _ = CloseHandle(proc);
        if !ok3 {
            return String::new();
        }
        let full = String::from_utf16_lossy(&buf);
        strip_executable_from_cmdline(&full)
    }
}

/// 从完整命令行字符串剥离首段程序路径，保留纯净参数
pub fn strip_executable_from_cmdline(cmdline: &str) -> String {
    let trimmed = cmdline.trim();
    if trimmed.is_empty() {
        return String::new();
    }
    if let Some(rest) = trimmed.strip_prefix('"') {
        if let Some(end_q) = rest.find('"') {
            return rest[end_q + 1..].trim().to_string();
        }
        return String::new();
    }
    if let Some(sp) = trimmed.find(char::is_whitespace) {
        trimmed[sp + 1..].trim().to_string()
    } else {
        String::new()
    }
}

/// 读取窗口的还原位置矩形（支持最小化窗真实桌面位置捕获与原位预览）
pub fn get_window_normal_placement_rect(hwnd: HWND) -> Option<RectI> {
    use windows::Win32::UI::WindowsAndMessaging::{GetWindowPlacement, WINDOWPLACEMENT};
    if hwnd.0.is_null() {
        return None;
    }
    unsafe {
        let mut wp = WINDOWPLACEMENT {
            length: size_of::<WINDOWPLACEMENT>() as u32,
            ..Default::default()
        };
        if GetWindowPlacement(hwnd, &mut wp).is_ok() {
            let r = rect_from_win32(wp.rcNormalPosition);
            if r.is_valid() {
                return Some(r);
            }
        }
    }
    None
}

/// 参考 AltTabio：优先经未提权 Windows Explorer 进程 Token 降权启动普通应用；仅当 run_as_admin=true 时提权运行
pub fn spawn_process_with_elevation_policy(
    exe_path: &str,
    args: &str,
    aumid: &str,
    run_as_admin: bool,
) -> bool {
    use windows::Win32::Foundation::HANDLE;
    use windows::Win32::Security::{
        DuplicateTokenEx, GetTokenInformation, SecurityImpersonation, TokenElevation, TokenPrimary,
        TOKEN_ASSIGN_PRIMARY, TOKEN_DUPLICATE, TOKEN_ELEVATION, TOKEN_QUERY,
    };
    use windows::Win32::System::Threading::{
        CreateProcessWithTokenW, OpenProcessToken, LOGON_WITH_PROFILE, PROCESS_CREATION_FLAGS,
        PROCESS_INFORMATION, STARTUPINFOW,
    };
    use windows::Win32::UI::Shell::ShellExecuteW;
    use windows::Win32::UI::WindowsAndMessaging::{GetShellWindow, SW_SHOWNORMAL};

    let clean_aumid = aumid.trim();
    let is_uwp_shell = !clean_aumid.is_empty()
        && (exe_path.trim().is_empty()
            || exe_path
                .to_ascii_lowercase()
                .ends_with("applicationframehost.exe"));
    let target_file = if is_uwp_shell {
        format!("shell:AppsFolder\\{clean_aumid}")
    } else {
        exe_path.trim().to_string()
    };
    if target_file.is_empty() {
        return false;
    }

    // 1. 非管理员目标且当前宿主处于提权态：经桌面 Explorer 未提权令牌降权拉起（防止子应用继承管理员权限导致无法拖放文件）
    if !run_as_admin && !is_uwp_shell && is_process_elevated(unsafe { GetCurrentProcessId() }) {
        let token_launched = unsafe {
            (|| -> Option<()> {
                let shell_hwnd = GetShellWindow();
                if shell_hwnd.0.is_null() {
                    return None;
                }
                let mut shell_pid = 0u32;
                let _ = GetWindowThreadProcessId(shell_hwnd, Some(&mut shell_pid));
                if shell_pid == 0 {
                    return None;
                }
                let shell_proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, shell_pid).ok()?;
                let mut raw_token = HANDLE::default();
                let open_ok = OpenProcessToken(
                    shell_proc,
                    TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY,
                    &mut raw_token,
                )
                .is_ok();
                let _ = CloseHandle(shell_proc);
                if !open_ok {
                    return None;
                }

                // 确认 Explorer 令牌本身未提权
                let mut elev = TOKEN_ELEVATION::default();
                let mut ret_len = 0u32;
                let elev_ok = GetTokenInformation(
                    raw_token,
                    TokenElevation,
                    Some(&mut elev as *mut TOKEN_ELEVATION as *mut c_void),
                    size_of::<TOKEN_ELEVATION>() as u32,
                    &mut ret_len,
                )
                .is_ok();
                if elev_ok && elev.TokenIsElevated != 0 {
                    let _ = CloseHandle(raw_token);
                    return None;
                }

                let mut primary_token = HANDLE::default();
                let dup_ok = DuplicateTokenEx(
                    raw_token,
                    TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY,
                    None,
                    SecurityImpersonation,
                    TokenPrimary,
                    &mut primary_token,
                )
                .is_ok();
                let _ = CloseHandle(raw_token);
                if !dup_ok {
                    return None;
                }

                let app_w = to_wide(&target_file);
                let mut cmd_w = if args.trim().is_empty() {
                    Vec::new()
                } else {
                    to_wide(&format!("\"{}\" {}", target_file, args.trim()))
                };
                let cmd_pwstr = if cmd_w.is_empty() {
                    PWSTR::null()
                } else {
                    PWSTR(cmd_w.as_mut_ptr())
                };
                let mut si = STARTUPINFOW {
                    cb: size_of::<STARTUPINFOW>() as u32,
                    ..Default::default()
                };
                let mut pi = PROCESS_INFORMATION::default();
                let created = CreateProcessWithTokenW(
                    primary_token,
                    LOGON_WITH_PROFILE,
                    PCWSTR(app_w.as_ptr()),
                    cmd_pwstr,
                    PROCESS_CREATION_FLAGS::default(),
                    None,
                    PCWSTR::null(),
                    &mut si,
                    &mut pi,
                )
                .is_ok();
                let _ = CloseHandle(primary_token);
                if created {
                    let _ = CloseHandle(pi.hThread);
                    let _ = CloseHandle(pi.hProcess);
                    Some(())
                } else {
                    None
                }
            })()
            .is_some()
        };
        if token_launched {
            return true;
        }
    }

    // 2. 管理员提权启动、UWP 协议启动、非提权宿主直接启动或安全回退
    let verb = to_wide(if run_as_admin && !is_uwp_shell {
        "runas"
    } else {
        "open"
    });
    let file = to_wide(&target_file);
    let params = to_wide(args.trim());
    let params_ptr = if args.trim().is_empty() || is_uwp_shell {
        PCWSTR::null()
    } else {
        PCWSTR(params.as_ptr())
    };
    let res = unsafe {
        ShellExecuteW(
            HWND::default(),
            PCWSTR(verb.as_ptr()),
            PCWSTR(file.as_ptr()),
            params_ptr,
            PCWSTR::null(),
            SW_SHOWNORMAL,
        )
    };
    (res.0 as isize) > 32
}

/// 参考 AltTabio：读取进程创建时间用于防 PID 复用误杀校验
pub fn query_process_creation_time(pid: u32) -> u64 {
    use windows::Win32::Foundation::FILETIME;
    use windows::Win32::System::Threading::GetProcessTimes;
    if pid == 0 {
        return 0;
    }
    unsafe {
        let Ok(proc) = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid) else {
            return 0;
        };
        let mut created = FILETIME::default();
        let mut exit = FILETIME::default();
        let mut kernel = FILETIME::default();
        let mut user = FILETIME::default();
        let ok = GetProcessTimes(proc, &mut created, &mut exit, &mut kernel, &mut user).is_ok();
        let _ = CloseHandle(proc);
        if ok {
            ((created.dwHighDateTime as u64) << 32) | (created.dwLowDateTime as u64)
        } else {
            0
        }
    }
}

/// 参考 AltTabio：自动寻址目标窗口最前活跃模态子窗并异步恢复激活（杜绝激活被模态对话框阻塞的父窗）
pub fn activate_window_or_popup(hwnd: HWND) -> bool {
    use windows::Win32::UI::WindowsAndMessaging::GetLastActivePopup;
    if hwnd.0.is_null() || is_hung_window(hwnd) {
        return false;
    }
    let target = unsafe {
        let popup = GetLastActivePopup(hwnd);
        if !popup.0.is_null() && IsWindow(popup).as_bool() && IsWindowVisible(popup).as_bool() {
            popup
        } else {
            hwnd
        }
    };
    activate_window(target)
}

pub fn get_exe_shell_icon(exe_path: &str, large: bool) -> Option<SafeIcon> {
    if exe_path.is_empty() {
        return None;
    }
    let wide = to_wide(exe_path);
    let mut shfi = SHFILEINFOW::default();
    let flags = SHGFI_ICON | if large { SHGFI_LARGEICON } else { SHGFI_SMALLICON };
    let res = unsafe {
        SHGetFileInfoW(
            PCWSTR(wide.as_ptr()),
            windows::Win32::Storage::FileSystem::FILE_FLAGS_AND_ATTRIBUTES(0),
            Some(&mut shfi),
            size_of::<SHFILEINFOW>() as u32,
            flags,
        )
    };
    if res != 0 && !shfi.hIcon.is_invalid() {
        Some(SafeIcon::owned(shfi.hIcon))
    } else {
        None
    }
}

/// 采集单个窗口的完整候选元数据（安全无阻塞）
pub fn inspect_window_meta(hwnd: HWND) -> Option<(WindowCandidateMeta, u32, String)> {
    unsafe {
        if !IsWindow(hwnd).as_bool() {
            return None;
        }
        let is_visible = IsWindowVisible(hwnd).as_bool();
        if !is_visible {
            return None;
        }
        let is_hung = IsHungAppWindow(hwnd).as_bool();
        if is_hung {
            return None;
        }
        let is_cloaked = is_window_cloaked(hwnd);
        let is_minimized = IsIconic(hwnd).as_bool();
        let has_owner = GetWindow(hwnd, GW_OWNER)
            .map(|o| !o.0.is_null())
            .unwrap_or(false);
        let ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
        let is_tool_window = (ex_style & WS_EX_TOOLWINDOW.0) != 0;
        let is_app_window = (ex_style & WS_EX_APPWINDOW.0) != 0;
        let is_no_activate = (ex_style & WS_EX_NOACTIVATE.0) != 0;

        let mut rc = RECT::default();
        let _ = GetWindowRect(hwnd, &mut rc);
        let class_name = get_window_class_name(hwnd);
        let title = get_window_title_safe(hwnd);
        let (pid, process_name, exe_path) = get_window_process_info(hwnd);

        Some((
            WindowCandidateMeta {
                is_visible,
                is_cloaked,
                is_minimized,
                is_hung,
                has_owner,
                is_tool_window,
                is_app_window,
                is_no_activate,
                rect: rect_from_win32(rc),
                title,
                class_name,
                process_name,
            },
            pid,
            exe_path,
        ))
    }
}

/// 检查指定窗口是否为合法可管理用户窗口
pub fn is_manageable_window(hwnd: HWND, include_minimized: bool, excluded_apps: &[String]) -> bool {
    let self_pid = unsafe { GetCurrentProcessId() };
    let Some((meta, pid, _)) = inspect_window_meta(hwnd) else {
        return false;
    };
    if pid == self_pid {
        return false;
    }
    is_eligible_user_window(&meta, include_minimized, excluded_apps)
}

/// 按 Z-Order 枚举所有合法用户顶层窗口
pub fn enumerate_user_windows(
    include_minimized: bool,
    excluded_apps: &[String],
) -> Vec<WindowInfo> {
    let monitors = enumerate_monitors();
    let self_pid = unsafe { GetCurrentProcessId() };

    struct EnumState<'a> {
        include_minimized: bool,
        excluded_apps: &'a [String],
        monitors: &'a [MonitorInfo],
        self_pid: u32,
        out: Vec<WindowInfo>,
    }

    unsafe extern "system" fn enum_proc(hwnd: HWND, lparam: LPARAM) -> BOOL {
        let state = &mut *(lparam.0 as *mut EnumState<'_>);
        if let Some((meta, pid, exe_path)) = inspect_window_meta(hwnd) {
            if pid != state.self_pid
                && is_eligible_user_window(&meta, state.include_minimized, state.excluded_apps)
            {
                let mon_id = monitor_for_hwnd(hwnd, state.monitors)
                    .map(|m| m.id)
                    .unwrap_or_default();
                let is_maximized = IsZoomed(hwnd).as_bool();
                state.out.push(WindowInfo {
                    hwnd,
                    pid,
                    title: meta.title,
                    class_name: meta.class_name,
                    process_name: meta.process_name,
                    exe_path,
                    rect: meta.rect,
                    monitor_id: mon_id,
                    is_maximized,
                    is_minimized: meta.is_minimized,
                });
            }
        }
        TRUE
    }

    let mut state = EnumState {
        include_minimized,
        excluded_apps,
        monitors: &monitors,
        self_pid,
        out: Vec::new(),
    };
    unsafe {
        let _ = EnumWindows(
            Some(enum_proc),
            LPARAM(&mut state as *mut EnumState<'_> as isize),
        );
    }
    state.out
}

/// 获取目标窗口精确可视外框（优先 DWMWA_EXTENDED_FRAME_BOUNDS，回退 GetWindowRect）
pub fn get_window_visual_rect(hwnd: HWND) -> Option<RectI> {
    unsafe {
        if !IsWindow(hwnd).as_bool() {
            return None;
        }
        let mut ext = RECT::default();
        if DwmGetWindowAttribute(
            hwnd,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &mut ext as *mut RECT as *mut c_void,
            size_of::<RECT>() as u32,
        )
        .is_ok()
        {
            let r = rect_from_win32(ext);
            if r.is_valid() {
                return Some(r);
            }
        }
        let mut rc = RECT::default();
        if GetWindowRect(hwnd, &mut rc).is_ok() {
            let r = rect_from_win32(rc);
            if r.is_valid() {
                return Some(r);
            }
        }
        None
    }
}

/// 检测 Windows 系统当前是否处于深色应用主题（用于 theme == "system" 自动跟随）
pub fn is_system_dark_mode() -> bool {
    use windows::Win32::System::Registry::{
        RegCloseKey, RegOpenKeyExW, RegQueryValueExW, HKEY, HKEY_CURRENT_USER, KEY_READ,
        REG_DWORD, REG_VALUE_TYPE,
    };
    let subkey = to_wide(r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
    let val_name = to_wide("AppsUseLightTheme");
    unsafe {
        let mut hkey = HKEY::default();
        if RegOpenKeyExW(
            HKEY_CURRENT_USER,
            PCWSTR(subkey.as_ptr()),
            0,
            KEY_READ,
            &mut hkey,
        )
        .is_err()
        {
            return false;
        }
        let mut data: u32 = 1;
        let mut data_size = size_of::<u32>() as u32;
        let mut val_type = REG_VALUE_TYPE(0);
        let res = RegQueryValueExW(
            hkey,
            PCWSTR(val_name.as_ptr()),
            None,
            Some(&mut val_type),
            Some(&mut data as *mut u32 as *mut u8),
            Some(&mut data_size),
        );
        let _ = RegCloseKey(hkey);
        if res.is_ok() && val_type == REG_DWORD {
            data == 0
        } else {
            false
        }
    }
}

/// 两阶段 DPI 安全 DWM 零缝隙精确定位（自动解除最大化，先跨屏触发 WM_DPICHANGED，再按目标屏 DPI 补偿隐形阴影边框）
/// 若遭遇 UIPI 权限拒绝（提权窗口），静默返回 false 绝不刷错误日志
pub fn place_window_visual_rect(hwnd: HWND, desired_visible: RectI) -> bool {
    if !desired_visible.is_valid() || is_hung_window(hwnd) {
        return false;
    }
    unsafe {
        if IsIconic(hwnd).as_bool() || IsZoomed(hwnd).as_bool() {
            let _ = ShowWindow(hwnd, SW_RESTORE);
        }

        // 执行最多 2 轮定位：第 1 轮迁移至目标显示器触发 WM_DPICHANGED，第 2 轮按新 DPI 下隐形阴影精确贴齐
        for pass in 0..2 {
            let mut win_rc = RECT::default();
            if GetWindowRect(hwnd, &mut win_rc).is_err() {
                return false;
            }
            let win_rect = rect_from_win32(win_rc);
            let ext_rect = get_window_visual_rect(hwnd).unwrap_or(win_rect);

            if pass == 1 && ext_rect == desired_visible {
                break;
            }
            let compensated = compensate_dwm_frame_bounds(desired_visible, win_rect, ext_rect);
            if SetWindowPos(
                hwnd,
                HWND_TOP,
                compensated.left,
                compensated.top,
                compensated.width().max(64),
                compensated.height().max(48),
                SWP_NOZORDER | SWP_NOACTIVATE,
            )
            .is_err()
            {
                // UIPI 拒绝或窗口句柄并发销毁：静默退出不刷错误
                return false;
            }
        }
        true
    }
}

/// 安全激活并切至前台（自动恢复最小化窗口并绕过前台焦点锁）
pub fn activate_window(hwnd: HWND) -> bool {
    if is_hung_window(hwnd) {
        return false;
    }
    unsafe {
        if IsIconic(hwnd).as_bool() {
            let _ = ShowWindow(hwnd, SW_RESTORE);
        } else if !IsWindowVisible(hwnd).as_bool() {
            let _ = ShowWindow(hwnd, SW_SHOW);
        }

        let _ = AllowSetForegroundWindow(ASFW_ANY);
        if SetForegroundWindow(hwnd).as_bool() {
            return true;
        }

        let fg = GetForegroundWindow();
        let fg_tid = if !fg.0.is_null() {
            GetWindowThreadProcessId(fg, None)
        } else {
            0
        };
        let cur_tid = GetCurrentThreadId();
        let target_tid = GetWindowThreadProcessId(hwnd, None);

        let attached_fg = fg_tid != 0 && fg_tid != cur_tid && AttachThreadInput(cur_tid, fg_tid, true).as_bool();
        let attached_tgt = target_tid != 0 && target_tid != cur_tid && AttachThreadInput(cur_tid, target_tid, true).as_bool();

        let mut ok = SetForegroundWindow(hwnd).as_bool();
        if !ok {
            let _ = BringWindowToTop(hwnd);
            let _ = SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            ok = SetForegroundWindow(hwnd).as_bool() || GetForegroundWindow() == hwnd;
        }

        if attached_tgt {
            let _ = AttachThreadInput(cur_tid, target_tid, false);
        }
        if attached_fg {
            let _ = AttachThreadInput(cur_tid, fg_tid, false);
        }
        ok
    }
}

/// 非阻塞异步关闭窗口
pub fn close_window_async(hwnd: HWND) {
    if !is_hung_window(hwnd) {
        unsafe {
            let _ = PostMessageW(hwnd, WM_CLOSE, WPARAM(0), LPARAM(0));
        }
    }
}

/// 非阻塞异步最小化窗口
pub fn minimize_window_async(hwnd: HWND) {
    if !is_hung_window(hwnd) {
        unsafe {
            let _ = PostMessageW(hwnd, WM_SYSCOMMAND, WPARAM(SC_MINIMIZE as usize), LPARAM(0));
        }
    }
}

/// 强制结束指定窗口对应进程（严格阻断结束自身进程与 PID<=4 系统内核进程）
pub fn kill_window_process_safe(hwnd: HWND) -> bool {
    let mut pid = 0u32;
    unsafe {
        let _ = GetWindowThreadProcessId(hwnd, Some(&mut pid));
        if pid <= 4 || pid == GetCurrentProcessId() {
            return false;
        }
        if let Ok(handle) = OpenProcess(PROCESS_TERMINATE, false, pid) {
            let ok = TerminateProcess(handle, 1).is_ok();
            let _ = CloseHandle(handle);
            return ok;
        }
    }
    false
}

/// 为顶层卡片窗口应用跨 Win10 / Server 2019+ / Win11 硬件圆角与深浅色标题栏属性
#[allow(dead_code)]
pub fn apply_win11_card_chrome(hwnd: HWND, dark_mode: bool, small_radius: bool) {
    use crate::core::topology::windows_build_info;
    use windows::Win32::Graphics::Dwm::DWMWINDOWATTRIBUTE;

    let os = windows_build_info();
    unsafe {
        if let Some(attr) = os.dark_mode_attr() {
            let dark: i32 = if dark_mode { 1 } else { 0 };
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWINDOWATTRIBUTE(attr.as_u32() as i32),
                &dark as *const i32 as *const c_void,
                size_of::<i32>() as u32,
            );
        }
        if os.supports_win11_dwm_corners() {
            let pref = if small_radius {
                DWMWCP_ROUNDSMALL
            } else {
                DWMWCP_ROUND
            };
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWA_WINDOW_CORNER_PREFERENCE,
                &pref as *const _ as *const c_void,
                size_of::<u32>() as u32,
            );
        }
    }
}

/// 检测当前交互式桌面是否存在运行中的 Windows 任务栏（Shell_TrayWnd）
pub fn is_shell_taskbar_present() -> bool {
    let cls = to_wide("Shell_TrayWnd");
    unsafe {
        windows::Win32::UI::WindowsAndMessaging::FindWindowW(PCWSTR(cls.as_ptr()), PCWSTR::null())
            .map(|h| !h.0.is_null())
            .unwrap_or(false)
    }
}

/// 轻量调整顶层卡片不透明度（0..=255），供覆盖层平滑淡入动效使用
#[allow(dead_code)]
pub fn set_window_layered_alpha(hwnd: HWND, alpha: u8) {
    use windows::Win32::Foundation::COLORREF;
    use windows::Win32::UI::WindowsAndMessaging::{
        SetLayeredWindowAttributes, SetWindowLongPtrW, LWA_ALPHA, WS_EX_LAYERED,
    };
    unsafe {
        let ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
        if (ex & WS_EX_LAYERED.0) == 0 {
            let _ = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (ex | WS_EX_LAYERED.0) as isize);
        }
        let _ = SetLayeredWindowAttributes(hwnd, COLORREF(0), alpha, LWA_ALPHA);
    }
}

/// 获取指定窗口 DPI（高分屏物理缩放因子）
pub fn get_window_dpi(hwnd: HWND) -> u32 {
    let dpi = unsafe { GetDpiForWindow(hwnd) };
    if dpi == 0 {
        96
    } else {
        dpi
    }
}

/// 通过 Windows 公开 COM 接口 IVirtualDesktopManager 查询窗口所属虚拟桌面 GUID
pub fn get_window_virtual_desktop_id(hwnd: HWND) -> Option<String> {
    if hwnd.0.is_null() {
        return None;
    }
    unsafe {
        let mgr: IVirtualDesktopManager =
            CoCreateInstance(&VirtualDesktopManager, None, CLSCTX_ALL).ok()?;
        let guid: GUID = mgr.GetWindowDesktopId(hwnd).ok()?;
        if guid == GUID::zeroed() {
            return None;
        }
        Some(format_guid_lower(&guid))
    }
}

fn format_guid_lower(guid: &GUID) -> String {
    format!(
        "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        guid.data1,
        guid.data2,
        guid.data3,
        guid.data4[0],
        guid.data4[1],
        guid.data4[2],
        guid.data4[3],
        guid.data4[4],
        guid.data4[5],
        guid.data4[6],
        guid.data4[7],
    )
}

/// 从 Windows 注册表读取当前活动虚拟桌面 GUID（同时兼容 Win11 根目录与 Win10/Server 2019/2022 SessionInfo 路径）
pub fn get_current_virtual_desktop_id() -> Option<String> {
    crate::core::topology::query_current_desktop_guid_cross_os()
}

/// 优先解析窗口所属虚拟桌面 GUID，若对提权/UWP/瞬态窗无返回则安全回退到当前活动虚拟桌面 GUID
pub fn resolve_window_or_current_desktop_id(hwnd: HWND) -> Option<String> {
    get_window_virtual_desktop_id(hwnd).or_else(get_current_virtual_desktop_id)
}

/// 从注册表读取指定虚拟桌面 GUID 的用户自定义名称（若未命名则返回 None）
pub fn get_virtual_desktop_name(desktop_guid: &str) -> Option<String> {
    let trimmed = desktop_guid.trim().trim_matches('{').trim_matches('}');
    if trimmed.is_empty() {
        return None;
    }
    let key_path = format!(
        "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops\\Desktops\\{{{}}}",
        trimmed
    );
    unsafe {
        let subkey = to_wide(&key_path);
        let mut hkey = HKEY::default();
        if RegOpenKeyExW(
            HKEY_CURRENT_USER,
            PCWSTR(subkey.as_ptr()),
            0,
            KEY_READ,
            &mut hkey,
        )
        .is_err()
        {
            return None;
        }
        let val_name = to_wide("Name");
        let mut reg_type = REG_VALUE_TYPE::default();
        let mut buf = [0u16; 256];
        let mut buf_bytes = (buf.len() * size_of::<u16>()) as u32;
        let query_res = RegQueryValueExW(
            hkey,
            PCWSTR(val_name.as_ptr()),
            None,
            Some(&mut reg_type),
            Some(buf.as_mut_ptr() as *mut u8),
            Some(&mut buf_bytes),
        );
        let _ = RegCloseKey(hkey);
        if query_res.is_err() || reg_type != REG_SZ || buf_bytes < 2 {
            return None;
        }
        let u16_len = (buf_bytes as usize / 2).min(buf.len());
        let name = from_wide_nul(&buf[..u16_len]).trim().to_string();
        if name.is_empty() { None } else { Some(name) }
    }
}

/// 真实桌面可见窗口快照（用于 1:1 全屏布局编辑器的边缘磁吸参考线与「一键从当前窗口生成布局」）
#[derive(Debug, Clone)]
pub struct RealWindowSnapshot {
    #[allow(dead_code)]
    pub hwnd: HWND,
    pub process_name: String,
    #[allow(dead_code)]
    pub title: String,
    pub rect: RectI,
    pub is_maximized: bool,
}

/// 采集指定显示器（且位于当前活动虚拟桌面）的可见真实用户窗口可视矩形列表（按 Z-Order 从前到后）
pub fn enumerate_visible_windows_on_monitor(
    mon: &MonitorInfo,
    exclude_hwnd: Option<HWND>,
    excluded_apps: &[String],
) -> Vec<RealWindowSnapshot> {
    let windows = enumerate_user_windows(false, excluded_apps);
    let mut out = Vec::new();
    for w in windows {
        if w.is_minimized {
            continue;
        }
        if let Some(ex) = exclude_hwnd {
            if w.hwnd == ex {
                continue;
            }
        }
        let vis = get_window_visual_rect(w.hwnd).unwrap_or(w.rect);
        let Some(clipped) = vis.intersect(&mon.work_rect) else {
            continue;
        };
        // 至少有 40% 以上面积落在该显示器工作区内，避免跨屏残边污染磁吸线
        if clipped.width() < 80 || clipped.height() < 60 {
            continue;
        }
        if clipped.area() * 10 < vis.area().max(1) * 4 {
            continue;
        }
        out.push(RealWindowSnapshot {
            hwnd: w.hwnd,
            process_name: w.process_name,
            title: w.title,
            rect: clipped,
            is_maximized: w.is_maximized,
        });
    }
    out
}

/// 检测当前前台窗口是否处于独占全屏状态（用于焦点边框自动避让全屏游戏/视频）
pub fn is_foreground_fullscreen() -> bool {
    unsafe {
        let fg = GetForegroundWindow();
        if fg.0.is_null() || !IsWindow(fg).as_bool() || is_hung_window(fg) {
            return false;
        }
        let cls = get_window_class_name(fg);
        if cls.eq_ignore_ascii_case("Progman")
            || cls.eq_ignore_ascii_case("WorkerW")
            || cls.eq_ignore_ascii_case("Shell_TrayWnd")
            || cls.starts_with("Tools3000_")
        {
            return false;
        }
        let mut rc = RECT::default();
        if GetWindowRect(fg, &mut rc).is_err() {
            return false;
        }
        let hmon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        let mut mi = MONITORINFOEXW::default();
        mi.monitorInfo.cbSize = size_of::<MONITORINFOEXW>() as u32;
        if !GetMonitorInfoW(hmon, &mut mi.monitorInfo).as_bool() {
            return false;
        }
        let m = mi.monitorInfo.rcMonitor;
        rc.left <= m.left && rc.top <= m.top && rc.right >= m.right && rc.bottom >= m.bottom
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. 冷路径物理内存修剪与实时进程观测指标（Action 18）
// ─────────────────────────────────────────────────────────────────────────────

/// 仅在冷路径退场延迟 2000ms 后调用：紧缩进程堆并将闲置工作集归还操作系统
pub fn trim_working_set_cold_path() {
    unsafe {
        if let Ok(heap) = GetProcessHeap() {
            let _ = HeapCompact(heap, HEAP_FLAGS(0));
        }
        let _ = SetProcessWorkingSetSize(GetCurrentProcess(), usize::MAX, usize::MAX);
    }
}

/// 采集当前进程精确内存、句柄、GDI/USER 对象与业务运行时状态 JSON
pub fn query_runtime_stats_json(
    pinned_count: usize,
    tray_minimized_count: usize,
    active_features: &[&str],
) -> JsonValue {
    let mut working_set_bytes: u64 = 0;
    let mut private_bytes: u64 = 0;
    let mut handle_count: u32 = 0;
    let gdi_objects: u32;
    let user_objects: u32;
    let pid: u32;

    unsafe {
        let proc = GetCurrentProcess();
        pid = GetCurrentProcessId();

        let mut pmc = PROCESS_MEMORY_COUNTERS_EX::default();
        pmc.cb = size_of::<PROCESS_MEMORY_COUNTERS_EX>() as u32;
        if GetProcessMemoryInfo(
            proc,
            &mut pmc as *mut PROCESS_MEMORY_COUNTERS_EX as *mut PROCESS_MEMORY_COUNTERS,
            size_of::<PROCESS_MEMORY_COUNTERS_EX>() as u32,
        )
        .is_ok()
        {
            working_set_bytes = pmc.WorkingSetSize as u64;
            private_bytes = pmc.PrivateUsage as u64;
        }

        let _ = GetProcessHandleCount(proc, &mut handle_count);
        gdi_objects = GetGuiResources(proc, GR_GDIOBJECTS);
        user_objects = GetGuiResources(proc, GR_USEROBJECTS);
    }

    let mut obj = JsonValue::object();
    obj.insert("running", JsonValue::Bool(true));
    obj.insert("pid", JsonValue::from_u64(pid as u64));
    obj.insert("workingSetBytes", JsonValue::from_u64(working_set_bytes));
    obj.insert("privateBytes", JsonValue::from_u64(private_bytes));
    obj.insert("handleCount", JsonValue::from_u64(handle_count as u64));
    obj.insert("gdiObjects", JsonValue::from_u64(gdi_objects as u64));
    obj.insert("userObjects", JsonValue::from_u64(user_objects as u64));
    obj.insert("pinnedCount", JsonValue::from_usize(pinned_count));
    obj.insert(
        "trayMinimizedCount",
        JsonValue::from_usize(tray_minimized_count),
    );
    let mut feats = JsonValue::array();
    for f in active_features {
        feats.push(JsonValue::String((*f).to_string()));
    }
    obj.insert("activeFeatures", feats);
    obj
}

/// 原子写文件（先写临时文件再替换，避免宿主并发读取到半截 JSON）
pub fn write_text_file_atomic(path: &Path, content: &str) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, content.as_bytes()).map_err(|e| format!("write tmp failed: {e}"))?;
    if std::fs::rename(&tmp, path).is_err() {
        // Windows 上目标已存在时直接覆盖写入并清理临时文件
        let res = std::fs::write(path, content.as_bytes())
            .map_err(|e| format!("write target failed: {e}"));
        let _ = std::fs::remove_file(&tmp);
        return res;
    }
    Ok(())
}

/// 解析 PowerToys FancyZones 默认 custom-layouts.json 路径
pub fn default_powertoys_layouts_path() -> Option<std::path::PathBuf> {
    let local_app = std::env::var_os("LOCALAPPDATA")?;
    Some(
        std::path::PathBuf::from(local_app)
            .join("Microsoft")
            .join("PowerToys")
            .join("FancyZones")
            .join("custom-layouts.json"),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tray_recovery_registry_tracks_and_clears_slots() {
        let fake_hwnd = HWND(0x123456 as *mut c_void);
        register_tray_recovery_hwnd(fake_hwnd);
        // 重复注册不应占双槽
        register_tray_recovery_hwnd(fake_hwnd);
        let count = TRAY_RECOVERY_HWNDS
            .iter()
            .filter(|s| s.load(Ordering::Acquire) == 0x123456)
            .count();
        assert_eq!(count, 1);

        unregister_tray_recovery_hwnd(fake_hwnd);
        let after = TRAY_RECOVERY_HWNDS
            .iter()
            .filter(|s| s.load(Ordering::Acquire) == 0x123456)
            .count();
        assert_eq!(after, 0);
    }

    #[test]
    fn runtime_stats_reports_valid_process_metrics() {
        let stats = query_runtime_stats_json(2, 1, &["snapZones", "alwaysOnTop"]);
        assert!(stats.get_bool("running", false));
        assert!(stats.get("pid").and_then(|v| v.as_u32()).unwrap_or(0) > 0);
        assert!(stats.get("workingSetBytes").and_then(|v| v.as_i64()).unwrap_or(0) > 0);
        assert_eq!(stats.get_i32("pinnedCount", -1), 2);
        assert_eq!(stats.get_i32("trayMinimizedCount", -1), 1);
    }

    #[test]
    fn hicon_to_png_data_url_encodes_valid_png_signature() {
        use windows::Win32::UI::WindowsAndMessaging::{LoadIconW, IDI_APPLICATION};
        let hicon = unsafe { LoadIconW(None, IDI_APPLICATION) }.expect("IDI_APPLICATION icon");
        let data_url =
            hicon_to_png_data_url(hicon, 32).expect("WIC PNG encoder must encode system HICON");
        // PNG 文件头魔数 89 50 4E 47 0D 0A 1A 0A 经 Base64 编码后恒为 iVBORw0KGgo
        assert!(
            data_url.starts_with("data:image/png;base64,iVBORw0KGgo"),
            "unexpected PNG data URL prefix: {}",
            &data_url[..data_url.len().min(48)]
        );
    }

    #[test]
    fn hicon_to_rgba8_rasterizes_system_icon_cleanly() {
        use windows::Win32::UI::WindowsAndMessaging::{LoadIconW, IDI_APPLICATION};
        let hicon = unsafe { LoadIconW(None, IDI_APPLICATION) }.expect("IDI_APPLICATION icon");
        let (w, h, rgba) = hicon_to_rgba8(hicon, 32).expect("RGBA8 icon rasterization must succeed");
        assert_eq!((w, h), (32, 32));
        assert_eq!(rgba.len(), 32 * 32 * 4);
        assert!(
            rgba.chunks_exact(4).any(|px| px[3] > 0),
            "rasterized RGBA8 icon must contain visible non-transparent pixels"
        );
    }
}
