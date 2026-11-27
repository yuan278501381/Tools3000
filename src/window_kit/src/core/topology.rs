// ─────────────────────────────────────────────────────────────────────────────
// core/topology.rs — 多显示器 × 虚拟桌面 × 跨系统版本 (Win10 / Server 2019+ / Win11)
//                    统一拓扑感知、零轮询事件监听、4 级缺屏折叠回退与移除自愈引擎
//
// 核心工程红线:
//   1. 全平台兼容 Win10 1809+ / Server 2019 (17763) / Server 2022 (20348) /
//      Server 2025 (26100) / Win11 (22000+) / RDP 会话 / x64 & ARM64；
//   2. 100% 公开稳定 API（RtlGetVersion + DisplayConfig + 公开 IVirtualDesktopManager
//      + 官方 VirtualDesktops 注册表键 + RegNotifyChangeKeyValue），0 内部黑魔法；
//   3. 拔除副屏绝不破坏性删用户绑定；仅在 Explorer 明确报告存活桌面列表时审计过期桌面；
//   4. 0 后台轮询线程：虚拟桌面注册表变更直接以 Win32 Event 挂入主线程消息循环。
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::geometry::RectI;
use crate::core::json::JsonValue;
use crate::core::win_utils::{from_wide_nul, to_wide, MonitorInfo};
use std::ffi::c_void;
use std::mem::size_of;
use std::sync::OnceLock;
use windows::core::{GUID, PCWSTR};
use windows::Win32::Foundation::{CloseHandle, HANDLE, HWND};
use windows::Win32::System::Com::{CoCreateInstance, CLSCTX_ALL};
use windows::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};
use windows::Win32::System::Registry::{
    RegCloseKey, RegNotifyChangeKeyValue, RegOpenKeyExW, RegQueryValueExW, HKEY,
    HKEY_CURRENT_USER, KEY_NOTIFY, KEY_READ, REG_BINARY, REG_NOTIFY_CHANGE_LAST_SET,
    REG_NOTIFY_CHANGE_NAME, REG_SZ, REG_VALUE_TYPE,
};
use windows::Win32::System::Threading::{CreateEventW, GetCurrentProcessId, ResetEvent};

#[link(name = "kernel32")]
extern "system" {
    fn ProcessIdToSessionId(dwProcessId: u32, pSessionId: *mut u32) -> i32;
}
use windows::Win32::UI::Shell::{IVirtualDesktopManager, VirtualDesktopManager};
use windows::Win32::UI::WindowsAndMessaging::{GetSystemMetrics, SM_REMOTESESSION};

// ─────────────────────────────────────────────────────────────────────────────
// 1. Windows 真实版本号探测与跨版本能力矩阵 (Win10 / Server 2019 / 2022 / 2025 / Win11)
// ─────────────────────────────────────────────────────────────────────────────

pub const WIN10_1809_SERVER2019_BUILD: u32 = 17_763;
pub const WIN10_20H1_BUILD: u32 = 19_041;
#[allow(dead_code)]
pub const SERVER_2022_BUILD: u32 = 20_348;
pub const WIN11_21H2_BUILD: u32 = 22_000;
#[allow(dead_code)]
pub const WIN11_24H2_SERVER2025_BUILD: u32 = 26_100;

/// DWM 暗色标题栏属性编号分档（Win10 1809/Server 2019 为 19；Win10 20H1+/Server 2022+/Win11 为 20）
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DwmDarkModeAttr {
    Legacy19,
    Modern20,
}

impl DwmDarkModeAttr {
    pub const fn as_u32(self) -> u32 {
        match self {
            Self::Legacy19 => 19,
            Self::Modern20 => 20,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WindowsBuildInfo {
    pub major: u32,
    pub minor: u32,
    pub build: u32,
}

impl WindowsBuildInfo {
    pub const fn from_build(build: u32) -> Self {
        Self {
            major: 10,
            minor: 0,
            build,
        }
    }

    /// 按构建号解析支持的 DWM 暗色标题栏属性 ID
    pub const fn dark_mode_attr(self) -> Option<DwmDarkModeAttr> {
        if self.major < 10 || self.build < WIN10_1809_SERVER2019_BUILD {
            None
        } else if self.build < WIN10_20H1_BUILD {
            Some(DwmDarkModeAttr::Legacy19)
        } else {
            Some(DwmDarkModeAttr::Modern20)
        }
    }

    /// Windows 11 (22000+) 与 Server 2025 (26100+) 原生支持 DWM 硬件圆角与边框色
    pub const fn supports_win11_dwm_corners(self) -> bool {
        self.major > 10 || (self.major == 10 && self.build >= WIN11_21H2_BUILD)
    }

    /// Win10 1809 / Server 2019 / Server 2022 需走 RGN 回退圆角（非 Win11 DWM 硬件圆角）
    #[allow(dead_code)]
    pub const fn needs_rgn_rounded_corners_fallback(self) -> bool {
        !self.supports_win11_dwm_corners()
    }
}

#[repr(C)]
struct RtlOsVersionInfoW {
    dw_os_version_info_size: u32,
    dw_major_version: u32,
    dw_minor_version: u32,
    dw_build_number: u32,
    dw_platform_id: u32,
    sz_csd_version: [u16; 128],
}

/// 进程期缓存的真实 OS 版本（通过 ntdll!RtlGetVersion 绕过清单伪装，x64 & ARM64 通用）
pub fn windows_build_info() -> WindowsBuildInfo {
    static CACHED: OnceLock<WindowsBuildInfo> = OnceLock::new();
    *CACHED.get_or_init(|| unsafe {
        let mut info = RtlOsVersionInfoW {
            dw_os_version_info_size: size_of::<RtlOsVersionInfoW>() as u32,
            dw_major_version: 10,
            dw_minor_version: 0,
            dw_build_number: WIN10_1809_SERVER2019_BUILD,
            dw_platform_id: 2,
            sz_csd_version: [0u16; 128],
        };
        let ntdll = to_wide("ntdll.dll");
        if let Ok(hmod) = GetModuleHandleW(PCWSTR(ntdll.as_ptr())) {
            if let Some(proc_fn) =
                GetProcAddress(hmod, windows::core::PCSTR(b"RtlGetVersion\0".as_ptr()))
            {
                type RtlGetVersionFn = unsafe extern "system" fn(*mut RtlOsVersionInfoW) -> i32;
                let func: RtlGetVersionFn = std::mem::transmute(proc_fn);
                if func(&mut info) == 0 && info.dw_build_number > 0 {
                    return WindowsBuildInfo {
                        major: info.dw_major_version,
                        minor: info.dw_minor_version,
                        build: info.dw_build_number,
                    };
                }
            }
        }
        WindowsBuildInfo::from_build(WIN10_1809_SERVER2019_BUILD)
    })
}

/// 当前是否处于 RDP 远程桌面会话（Server 2019/2022/2025 与 Win10/11 远程办公高频场景）
pub fn is_remote_desktop_session() -> bool {
    unsafe { GetSystemMetrics(SM_REMOTESESSION) != 0 }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. 多显示器方向、归一化相对坐标与 4 级缺屏折叠/重连匹配器
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MonitorOrientation {
    Landscape,
    Portrait,
}

impl MonitorOrientation {
    pub const fn from_rect(r: RectI) -> Self {
        if r.height() > r.width() {
            Self::Portrait
        } else {
            Self::Landscape
        }
    }

    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Landscape => "landscape",
            Self::Portrait => "portrait",
        }
    }

    pub fn from_str_opt(s: &str) -> Option<Self> {
        match s.trim().to_ascii_lowercase().as_str() {
            "portrait" | "vertical" => Some(Self::Portrait),
            "landscape" | "horizontal" => Some(Self::Landscape),
            _ => None,
        }
    }
}

/// 相对某显示器工作区的归一化矩形 (0.0..=1.0)，用于跨分辨率/跨 DPI/拔副屏折叠无损自适应
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct NormalizedWorkRect {
    pub rel_x: f64,
    pub rel_y: f64,
    pub rel_w: f64,
    pub rel_h: f64,
}

impl NormalizedWorkRect {
    pub fn from_rect_in_work_area(rect: RectI, work: RectI) -> Option<Self> {
        if !rect.is_valid() || !work.is_valid() {
            return None;
        }
        let ww = work.width().max(1) as f64;
        let wh = work.height().max(1) as f64;
        Some(Self {
            rel_x: ((rect.left - work.left) as f64 / ww).clamp(0.0, 0.95),
            rel_y: ((rect.top - work.top) as f64 / wh).clamp(0.0, 0.95),
            rel_w: (rect.width() as f64 / ww).clamp(0.08, 1.0),
            rel_h: (rect.height() as f64 / wh).clamp(0.08, 1.0),
        })
    }

    /// 将归一化相对坐标解析并安全钳制到任意目标工作区内（严防掉出屏幕或超出边界）
    pub fn resolve_in_work_area(&self, dst_work: RectI, min_w: i32, min_h: i32) -> RectI {
        if !dst_work.is_valid() {
            return dst_work;
        }
        let dw = dst_work.width();
        let dh = dst_work.height();
        let w = ((self.rel_w * dw as f64).round() as i32).clamp(min_w.min(dw), dw);
        let h = ((self.rel_h * dh as f64).round() as i32).clamp(min_h.min(dh), dh);
        let raw_x = dst_work.left + (self.rel_x * dw as f64).round() as i32;
        let raw_y = dst_work.top + (self.rel_y * dh as f64).round() as i32;
        let x = raw_x.clamp(dst_work.left, (dst_work.right - w).max(dst_work.left));
        let y = raw_y.clamp(dst_work.top, (dst_work.bottom - h).max(dst_work.top));
        RectI::from_xywh(x, y, w, h)
    }

    #[allow(dead_code)]
    pub fn to_json(&self) -> JsonValue {
        let mut obj = JsonValue::object();
        obj.insert("relX", JsonValue::Number((self.rel_x * 10000.0).round() / 10000.0));
        obj.insert("relY", JsonValue::Number((self.rel_y * 10000.0).round() / 10000.0));
        obj.insert("relW", JsonValue::Number((self.rel_w * 10000.0).round() / 10000.0));
        obj.insert("relH", JsonValue::Number((self.rel_h * 10000.0).round() / 10000.0));
        obj
    }

    pub fn from_json(v: &JsonValue) -> Option<Self> {
        let rel_x = v.get("relX").or_else(|| v.get("x"))?.as_f64()?;
        let rel_y = v.get("relY").or_else(|| v.get("y"))?.as_f64()?;
        let rel_w = v.get("relW").or_else(|| v.get("w"))?.as_f64()?;
        let rel_h = v.get("relH").or_else(|| v.get("h"))?.as_f64()?;
        if !rel_x.is_finite() || !rel_y.is_finite() || !rel_w.is_finite() || !rel_h.is_finite() {
            return None;
        }
        Some(Self {
            rel_x: rel_x.clamp(0.0, 0.95),
            rel_y: rel_y.clamp(0.0, 0.95),
            rel_w: rel_w.clamp(0.08, 1.0),
            rel_h: rel_h.clamp(0.08, 1.0),
        })
    }
}

/// 匹配结果（区分精确命中与缺屏降级折叠，供启动进度卡向用户透明展示）
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MonitorMatchQuality {
    ExactId,
    HardwareFingerprint,
    IndexFallback,
    PrimaryFallback,
}

#[derive(Debug, Clone)]
pub struct MonitorMatchResult<'a> {
    pub monitor: &'a MonitorInfo,
    pub quality: MonitorMatchQuality,
}

/// 4 级确定性显示器匹配器（支持换扩展坞端口重连、双胞胎屏、以及 3 屏变单屏优雅折叠）
pub fn match_monitor_with_fallback<'a>(
    saved_id: &str,
    saved_friendly: &str,
    saved_orientation: Option<MonitorOrientation>,
    saved_index: Option<usize>,
    live_monitors: &'a [MonitorInfo],
) -> Option<MonitorMatchResult<'a>> {
    if live_monitors.is_empty() {
        return None;
    }

    let id_trim = saved_id.trim();
    // 1. 精确稳定 ID 匹配（硬件设备路径 / EDID 键）
    if !id_trim.is_empty() {
        if let Some(m) = live_monitors
            .iter()
            .find(|m| m.id.eq_ignore_ascii_case(id_trim) || m.device_name.eq_ignore_ascii_case(id_trim))
        {
            return Some(MonitorMatchResult {
                monitor: m,
                quality: MonitorMatchQuality::ExactId,
            });
        }
        // 提取 EDID 型号前缀（如 `DISPLAY#DELA182#...` -> `DELA182`），换扩展坞端口也能认出同一块物理屏！
        if let Some(model_token) = extract_display_model_token(id_trim) {
            let candidates: Vec<&MonitorInfo> = live_monitors
                .iter()
                .filter(|m| {
                    extract_display_model_token(&m.id)
                        .map(|t| t.eq_ignore_ascii_case(&model_token))
                        .unwrap_or(false)
                })
                .collect();
            if let Some(ori) = saved_orientation {
                if let Some(m) = candidates
                    .iter()
                    .copied()
                    .find(|m| MonitorOrientation::from_rect(m.physical_rect) == ori)
                {
                    return Some(MonitorMatchResult {
                        monitor: m,
                        quality: MonitorMatchQuality::HardwareFingerprint,
                    });
                }
            }
            if let Some(&m) = candidates.first() {
                return Some(MonitorMatchResult {
                    monitor: m,
                    quality: MonitorMatchQuality::HardwareFingerprint,
                });
            }
        }
    }

    // 2. 非通用友好型号名 + 方向匹配
    let friendly_trim = saved_friendly.trim();
    if !friendly_trim.is_empty() && !is_generic_display_name(friendly_trim) {
        let candidates: Vec<&MonitorInfo> = live_monitors
            .iter()
            .filter(|m| m.friendly_name.eq_ignore_ascii_case(friendly_trim))
            .collect();
        if let Some(ori) = saved_orientation {
            if let Some(m) = candidates
                .iter()
                .copied()
                .find(|m| MonitorOrientation::from_rect(m.physical_rect) == ori)
            {
                return Some(MonitorMatchResult {
                    monitor: m,
                    quality: MonitorMatchQuality::HardwareFingerprint,
                });
            }
        }
        if let Some(&m) = candidates.first() {
            return Some(MonitorMatchResult {
                monitor: m,
                quality: MonitorMatchQuality::HardwareFingerprint,
            });
        }
    }

    // 3. 捕获时的屏幕序号回退（若仍在线）
    if let Some(idx) = saved_index {
        if let Some(m) = live_monitors.get(idx) {
            return Some(MonitorMatchResult {
                monitor: m,
                quality: MonitorMatchQuality::IndexFallback,
            });
        }
    }

    // 4. 优雅折叠至主屏（或唯一在线屏）
    let primary = live_monitors
        .iter()
        .find(|m| m.is_primary)
        .unwrap_or(&live_monitors[0]);
    Some(MonitorMatchResult {
        monitor: primary,
        quality: MonitorMatchQuality::PrimaryFallback,
    })
}

/// 保证任意自定义记忆窗口（Quick Switch / 布局管理器 / 捕获浮窗）100% 落在当前在线显示器可见区内
pub fn ensure_window_rect_on_live_monitors(
    saved_monitor_id: &str,
    saved_rel: Option<NormalizedWorkRect>,
    desired_w: i32,
    desired_h: i32,
    preferred_monitor: Option<&MonitorInfo>,
    live_monitors: &[MonitorInfo],
) -> RectI {
    let fallback_mon = preferred_monitor
        .or_else(|| live_monitors.iter().find(|m| m.is_primary))
        .or_else(|| live_monitors.first());
    let Some(fb) = fallback_mon else {
        return RectI::from_xywh(100, 100, desired_w.max(320), desired_h.max(240));
    };

    // 优先回到上次记忆的在线屏；若已拔出，则平滑映射至当前光标屏/主屏工作区
    let target_mon = if !saved_monitor_id.trim().is_empty() {
        live_monitors
            .iter()
            .find(|m| m.id.eq_ignore_ascii_case(saved_monitor_id.trim()))
            .unwrap_or(fb)
    } else {
        fb
    };

    let work = target_mon.work_rect;
    let clamped_w = desired_w.clamp(320.min(work.width()), work.width());
    let clamped_h = desired_h.clamp(220.min(work.height()), work.height());

    if let Some(rel) = saved_rel {
        let raw_x = work.left + (rel.rel_x * work.width() as f64).round() as i32;
        let raw_y = work.top + (rel.rel_y * work.height() as f64).round() as i32;
        let x = raw_x.clamp(work.left, (work.right - clamped_w).max(work.left));
        let y = raw_y.clamp(work.top, (work.bottom - clamped_h).max(work.top));
        RectI::from_xywh(x, y, clamped_w, clamped_h)
    } else {
        let x = work.left + (work.width() - clamped_w) / 2;
        let y = work.top + (work.height() - clamped_h) / 2;
        RectI::from_xywh(x, y, clamped_w, clamped_h)
    }
}

fn extract_display_model_token(id: &str) -> Option<String> {
    let upper = id.to_ascii_uppercase();
    // 形式 A: DISPLAY#DELA182#5&12345...
    if let Some(rest) = upper.strip_prefix("DISPLAY#") {
        let model = rest.split('#').next().unwrap_or("").trim();
        if model.len() >= 4 {
            return Some(model.to_string());
        }
    }
    // 形式 B: EDID_10AC_A182_0 -> EDID_10AC_A182
    if upper.starts_with("EDID_") {
        let parts: Vec<&str> = upper.split('_').collect();
        if parts.len() >= 3 {
            return Some(format!("EDID_{}_{}", parts[1], parts[2]));
        }
    }
    None
}

fn is_generic_display_name(name: &str) -> bool {
    let low = name.trim().to_ascii_lowercase();
    low.starts_with("display ")
        || low.starts_with(r"\\.\")
        || low.contains("generic pnp monitor")
        || low.contains("通用即插即用监视器")
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. 跨 Win10 / Server 2019+ / Win11 虚拟桌面完整拓扑与零轮询监听
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VirtualDesktopEntry {
    pub id: String,
    pub name: String,
    pub index: usize,
    pub is_current: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VirtualDesktopTopology {
    /// true 表示已成功从 Explorer 注册表读取到真实存活桌面列表（可用于安全识别已删除桌面）
    /// false 表示当前环境无多虚拟桌面注册表（如 Server 2019 精简态或 Explorer 重启瞬间），严禁误删绑定
    pub reliable: bool,
    pub current_id: String,
    pub desktops: Vec<VirtualDesktopEntry>,
}

impl VirtualDesktopTopology {
    #[allow(dead_code)]
    pub fn contains_desktop(&self, desktop_id: &str) -> bool {
        let trimmed = desktop_id.trim();
        if trimmed.is_empty() {
            return false;
        }
        self.desktops
            .iter()
            .any(|d| d.id.eq_ignore_ascii_case(trimmed))
    }

    pub fn current_entry(&self) -> Option<&VirtualDesktopEntry> {
        self.desktops
            .iter()
            .find(|d| d.is_current)
            .or_else(|| self.desktops.first())
    }

    #[allow(dead_code)]
    pub fn to_json(&self) -> JsonValue {
        let mut obj = JsonValue::object();
        obj.insert("reliable", JsonValue::Bool(self.reliable));
        obj.insert("currentDesktopId", JsonValue::String(self.current_id.clone()));
        let mut arr = JsonValue::array();
        for d in &self.desktops {
            let mut item = JsonValue::object();
            item.insert("id", JsonValue::String(d.id.clone()));
            item.insert("name", JsonValue::String(d.name.clone()));
            item.insert("index", JsonValue::from_usize(d.index));
            item.insert("isCurrent", JsonValue::Bool(d.is_current));
            arr.push(item);
        }
        obj.insert("desktops", arr);
        obj
    }
}

/// 枚举当前用户会话的全部真实虚拟桌面（完整兼容 Win11、Win10 1809–22H2、Server 2019/2022/2025）
pub fn enumerate_virtual_desktop_topology() -> VirtualDesktopTopology {
    let mut current_id = query_current_desktop_guid_cross_os().unwrap_or_default();
    let mut ids = query_virtual_desktop_id_list();

    // 若列表为空但拿到了 current_id（某些单桌面系统/首次登录前），把 current_id 视作唯一桌面
    let reliable = !ids.is_empty();
    if ids.is_empty() && !current_id.is_empty() {
        ids.push(current_id.clone());
    }
    if current_id.is_empty() && !ids.is_empty() {
        current_id = ids[0].clone();
    }

    let mut desktops = Vec::with_capacity(ids.len());
    for (idx, guid) in ids.into_iter().enumerate() {
        let name = query_virtual_desktop_custom_name(&guid).unwrap_or_default();
        let is_current = !current_id.is_empty() && guid.eq_ignore_ascii_case(&current_id);
        desktops.push(VirtualDesktopEntry {
            id: guid,
            name,
            index: idx + 1,
            is_current,
        });
    }

    VirtualDesktopTopology {
        reliable,
        current_id,
        desktops,
    }
}

/// 兼容 Win11 (`VirtualDesktops\CurrentVirtualDesktop`) 与 Win10/Server 2019/2022
/// (`VirtualDesktops\SessionInfo\{SessionId}\CurrentVirtualDesktop`) 读取当前活跃桌面 GUID
pub fn query_current_desktop_guid_cross_os() -> Option<String> {
    if let Some(g) = read_reg_guid(
        r"Software\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops",
        "CurrentVirtualDesktop",
    ) {
        return Some(g);
    }

    // Win10 1809–22H2 / Windows Server 2019 / 2022 存储路径：SessionInfo\{SessionId}\CurrentVirtualDesktop
    let mut session_id = 0u32;
    let ok = unsafe { ProcessIdToSessionId(GetCurrentProcessId(), &mut session_id) != 0 };
    if ok {
        let session_path = format!(
            r"Software\Microsoft\Windows\CurrentVersion\Explorer\SessionInfo\{}\VirtualDesktops",
            session_id
        );
        if let Some(g) = read_reg_guid(&session_path, "CurrentVirtualDesktop") {
            return Some(g);
        }
    }
    None
}

fn query_virtual_desktop_id_list() -> Vec<String> {
    let subkey = to_wide(r"Software\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops");
    let val_name = to_wide("VirtualDesktopIDs");
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
            return Vec::new();
        }
        let mut reg_type = REG_VALUE_TYPE::default();
        let mut size = 0u32;
        let probe = RegQueryValueExW(
            hkey,
            PCWSTR(val_name.as_ptr()),
            None,
            Some(&mut reg_type),
            None,
            Some(&mut size),
        );
        if probe.is_err() || reg_type != REG_BINARY || size < 16 || size > 16 * 64 {
            let _ = RegCloseKey(hkey);
            return Vec::new();
        }
        let mut buf = vec![0u8; size as usize];
        let mut actual_size = size;
        let read = RegQueryValueExW(
            hkey,
            PCWSTR(val_name.as_ptr()),
            None,
            Some(&mut reg_type),
            Some(buf.as_mut_ptr()),
            Some(&mut actual_size),
        );
        let _ = RegCloseKey(hkey);
        if read.is_err() || actual_size < 16 {
            return Vec::new();
        }
        let count = (actual_size as usize) / 16;
        let mut out = Vec::with_capacity(count);
        for i in 0..count {
            if let Some(g) = guid_bytes_to_lower_string(&buf[i * 16..(i + 1) * 16]) {
                out.push(g);
            }
        }
        out
    }
}

fn read_reg_guid(key_path: &str, value_name: &str) -> Option<String> {
    let subkey = to_wide(key_path);
    let val_name = to_wide(value_name);
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
            return None;
        }
        let mut reg_type = REG_VALUE_TYPE::default();
        let mut buf = [0u8; 16];
        let mut buf_len = buf.len() as u32;
        let res = RegQueryValueExW(
            hkey,
            PCWSTR(val_name.as_ptr()),
            None,
            Some(&mut reg_type),
            Some(buf.as_mut_ptr()),
            Some(&mut buf_len),
        );
        let _ = RegCloseKey(hkey);
        if res.is_err() || reg_type != REG_BINARY || buf_len != 16 {
            return None;
        }
        guid_bytes_to_lower_string(&buf)
    }
}

fn query_virtual_desktop_custom_name(desktop_guid: &str) -> Option<String> {
    let trimmed = desktop_guid.trim().trim_matches('{').trim_matches('}');
    if trimmed.is_empty() {
        return None;
    }
    let key_path = format!(
        r"Software\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops\Desktops\{{{}}}",
        trimmed
    );
    let subkey = to_wide(&key_path);
    let val_name = to_wide("Name");
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
            return None;
        }
        let mut reg_type = REG_VALUE_TYPE::default();
        let mut buf = [0u16; 256];
        let mut buf_bytes = (buf.len() * size_of::<u16>()) as u32;
        let res = RegQueryValueExW(
            hkey,
            PCWSTR(val_name.as_ptr()),
            None,
            Some(&mut reg_type),
            Some(buf.as_mut_ptr() as *mut u8),
            Some(&mut buf_bytes),
        );
        let _ = RegCloseKey(hkey);
        if res.is_err() || reg_type != REG_SZ || buf_bytes < 2 {
            return None;
        }
        let u16_len = (buf_bytes as usize / 2).min(buf.len());
        let name = from_wide_nul(&buf[..u16_len]).trim().to_string();
        if name.is_empty() { None } else { Some(name) }
    }
}

pub fn guid_bytes_to_lower_string(b: &[u8]) -> Option<String> {
    if b.len() != 16 {
        return None;
    }
    let d1 = u32::from_le_bytes([b[0], b[1], b[2], b[3]]);
    let d2 = u16::from_le_bytes([b[4], b[5]]);
    let d3 = u16::from_le_bytes([b[6], b[7]]);
    if d1 == 0 && d2 == 0 && d3 == 0 && b[8..16].iter().all(|&x| x == 0) {
        return None;
    }
    Some(format!(
        "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        d1, d2, d3, b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]
    ))
}

pub fn parse_guid_string(s: &str) -> Option<GUID> {
    let clean = s.trim().trim_matches('{').trim_matches('}');
    let parts: Vec<&str> = clean.split('-').collect();
    if parts.len() != 5
        || parts[0].len() != 8
        || parts[1].len() != 4
        || parts[2].len() != 4
        || parts[3].len() != 4
        || parts[4].len() != 12
    {
        return None;
    }
    let data1 = u32::from_str_radix(parts[0], 16).ok()?;
    let data2 = u16::from_str_radix(parts[1], 16).ok()?;
    let data3 = u16::from_str_radix(parts[2], 16).ok()?;
    let tail_hex = format!("{}{}", parts[3], parts[4]);
    let mut data4 = [0u8; 8];
    for i in 0..8 {
        data4[i] = u8::from_str_radix(&tail_hex[i * 2..i * 2 + 2], 16).ok()?;
    }
    Some(GUID {
        data1,
        data2,
        data3,
        data4,
    })
}

/// 判定目标窗口是否位于当前活跃虚拟桌面（公开稳定 COM API + DWM Cloak 跨权限双校验）
pub fn is_window_on_current_virtual_desktop(hwnd: HWND) -> bool {
    if hwnd.0.is_null() {
        return false;
    }
    unsafe {
        let Ok(mgr): Result<IVirtualDesktopManager, _> =
            CoCreateInstance(&VirtualDesktopManager, None, CLSCTX_ALL)
        else {
            return true;
        };
        match mgr.IsWindowOnCurrentVirtualDesktop(hwnd) {
            Ok(b) if b.as_bool() => true,
            Ok(_) => {
                // 跨权限/UWP 窗口在部分 Windows 11/Server 版本上对其他进程调用
                // IsWindowOnCurrentVirtualDesktop 可能返回 Ok(false)；通过 GetWindowDesktopId 二次核验：
                // 仅当系统明确返回了非零且不等于当前桌面 GUID 时，才判定为「属于其他虚拟桌面」。
                if let Ok(win_guid) = mgr.GetWindowDesktopId(hwnd) {
                    let is_zero = win_guid.data1 == 0
                        && win_guid.data2 == 0
                        && win_guid.data3 == 0
                        && win_guid.data4.iter().all(|&x| x == 0);
                    if is_zero {
                        // 无明确桌面归属（如常驻所有桌面的固定窗或跨权限未知窗）-> 保守放行
                        return true;
                    }
                    if let Some(cur_str) = query_current_desktop_guid_cross_os() {
                        if let Some(cur_guid) = parse_guid_string(&cur_str) {
                            return win_guid == cur_guid;
                        }
                    }
                }
                false
            }
            Err(_) => true,
        }
    }
}

/// 将指定窗口迁移到当前活跃虚拟桌面（用于托盘收纳窗原桌面被删防失联、Workspaces 跨桌面复用迁回）
pub fn move_window_to_current_virtual_desktop(hwnd: HWND) -> bool {
    if hwnd.0.is_null() {
        return false;
    }
    let Some(cur_id) = query_current_desktop_guid_cross_os() else {
        return false;
    };
    let Some(guid) = parse_guid_string(&cur_id) else {
        return false;
    };
    unsafe {
        let Ok(mgr): Result<IVirtualDesktopManager, _> =
            CoCreateInstance(&VirtualDesktopManager, None, CLSCTX_ALL)
        else {
            return false;
        };
        mgr.MoveWindowToDesktop(hwnd, &guid).is_ok()
    }
}

/// 零轮询虚拟桌面注册表异步监听器（直接产出 Win32 Event HANDLE 挂入主线程 MsgWaitForMultipleObjectsEx）
pub struct VirtualDesktopRegWatcher {
    hkey_root: HKEY,
    hkey_session: HKEY,
    event: HANDLE,
}

impl VirtualDesktopRegWatcher {
    pub fn try_new() -> Option<Self> {
        unsafe {
            let Ok(event) = CreateEventW(None, true, false, PCWSTR::null()) else {
                return None;
            };
            let root_path =
                to_wide(r"Software\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops");
            let mut hkey_root = HKEY::default();
            let root_ok = RegOpenKeyExW(
                HKEY_CURRENT_USER,
                PCWSTR(root_path.as_ptr()),
                0,
                KEY_NOTIFY | KEY_READ,
                &mut hkey_root,
            )
            .is_ok();

            let mut hkey_session = HKEY::default();
            let mut session_id = 0u32;
            let session_ok = if ProcessIdToSessionId(GetCurrentProcessId(), &mut session_id) != 0 {
                let session_path = to_wide(&format!(
                    r"Software\Microsoft\Windows\CurrentVersion\Explorer\SessionInfo\{}\VirtualDesktops",
                    session_id
                ));
                RegOpenKeyExW(
                    HKEY_CURRENT_USER,
                    PCWSTR(session_path.as_ptr()),
                    0,
                    KEY_NOTIFY | KEY_READ,
                    &mut hkey_session,
                )
                .is_ok()
            } else {
                false
            };

            if !root_ok && !session_ok {
                let _ = CloseHandle(event);
                return None;
            }

            let watcher = Self {
                hkey_root: if root_ok { hkey_root } else { HKEY::default() },
                hkey_session: if session_ok {
                    hkey_session
                } else {
                    HKEY::default()
                },
                event,
            };
            watcher.rearm();
            Some(watcher)
        }
    }

    pub fn event_handle(&self) -> HANDLE {
        self.event
    }

    /// 收到通知后重置事件并重新挂接异步监听（0 轮询）
    pub fn rearm(&self) {
        unsafe {
            let _ = ResetEvent(self.event);
            let flags = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET;
            if !self.hkey_root.is_invalid() {
                let _ = RegNotifyChangeKeyValue(self.hkey_root, true, flags, self.event, true);
            }
            if !self.hkey_session.is_invalid() {
                let _ = RegNotifyChangeKeyValue(self.hkey_session, false, flags, self.event, true);
            }
        }
    }
}

impl Drop for VirtualDesktopRegWatcher {
    fn drop(&mut self) {
        unsafe {
            if !self.hkey_root.is_invalid() {
                let _ = RegCloseKey(self.hkey_root);
            }
            if !self.hkey_session.is_invalid() {
                let _ = RegCloseKey(self.hkey_session);
            }
            if !self.event.is_invalid() {
                let _ = CloseHandle(self.event);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. 离线显示器与已删除虚拟桌面绑定审计器（零破坏性保留 + 透明可清理）
// ─────────────────────────────────────────────────────────────────────────────

#[allow(dead_code)]
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BindingAuditSummary {
    pub online_binding_keys: Vec<String>,
    /// 拔出的历史显示器绑定键：绝不自动删除，插回自动复活，设置页允许手动清理
    pub offline_monitor_keys: Vec<String>,
    /// 已确认被系统删除的虚拟桌面残留绑定键（仅在 topology.reliable == true 时标记）
    pub removed_desktop_keys: Vec<String>,
}

#[allow(dead_code)]
pub fn audit_monitor_desktop_bindings<'a>(
    binding_keys: impl IntoIterator<Item = &'a str>,
    live_monitors: &[MonitorInfo],
    vd_topology: &VirtualDesktopTopology,
) -> BindingAuditSummary {
    let mut summary = BindingAuditSummary {
        online_binding_keys: Vec::new(),
        offline_monitor_keys: Vec::new(),
        removed_desktop_keys: Vec::new(),
    };

    for raw_key in binding_keys {
        let key = raw_key.trim();
        if key.is_empty() {
            continue;
        }
        let (mon_id, desk_id) = match key.rsplit_once('@') {
            Some((m, d)) if !m.trim().is_empty() && !d.trim().is_empty() => (m.trim(), Some(d.trim())),
            _ => (key, None),
        };
        let monitor_online = live_monitors
            .iter()
            .any(|m| m.id.eq_ignore_ascii_case(mon_id) || m.device_name.eq_ignore_ascii_case(mon_id));

        if let Some(did) = desk_id {
            if vd_topology.reliable && !vd_topology.contains_desktop(did) {
                summary.removed_desktop_keys.push(key.to_string());
                continue;
            }
        }

        if monitor_online {
            summary.online_binding_keys.push(key.to_string());
        } else {
            summary.offline_monitor_keys.push(key.to_string());
        }
    }

    summary
}

/// 跨 Win10 / Server 2019+ / Win11 硬件/RGN 通用圆角双保险（用于 WS_POPUP 无标题栏工具浮窗）
pub fn apply_cross_os_popup_chrome(
    hwnd: HWND,
    dark_mode: bool,
    width: i32,
    height: i32,
    radius_px: i32,
) {
    use windows::Win32::Graphics::Dwm::{
        DwmSetWindowAttribute, DWMWINDOWATTRIBUTE, DWMWCP_ROUND, DWMWA_WINDOW_CORNER_PREFERENCE,
    };
    use windows::Win32::Graphics::Gdi::{CreateRoundRectRgn, SetWindowRgn};

    let os = windows_build_info();
    unsafe {
        if let Some(attr) = os.dark_mode_attr() {
            let dark_val: i32 = if dark_mode { 1 } else { 0 };
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWINDOWATTRIBUTE(attr.as_u32() as i32),
                &dark_val as *const i32 as *const c_void,
                size_of::<i32>() as u32,
            );
        }

        if os.supports_win11_dwm_corners() && !is_remote_desktop_session() {
            let pref = DWMWCP_ROUND;
            let hr = DwmSetWindowAttribute(
                hwnd,
                DWMWA_WINDOW_CORNER_PREFERENCE,
                &pref as *const _ as *const c_void,
                size_of::<u32>() as u32,
            );
            if hr.is_ok() {
                // Win11 原生硬件圆角成功时不叠加 GDI RGN（避免锯齿）
                let _ = SetWindowRgn(hwnd, None, false);
                return;
            }
        }

        // Win10 / Server 2019 / Server 2022 / RDP 自动回退平滑圆角 RGN
        if width > 0 && height > 0 && radius_px > 0 {
            let rgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius_px * 2, radius_px * 2);
            if !rgn.is_invalid() {
                let _ = SetWindowRgn(hwnd, rgn, false);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_mon(
        id: &str,
        friendly: &str,
        rect: RectI,
        dpi: u32,
        is_primary: bool,
    ) -> MonitorInfo {
        MonitorInfo {
            hmonitor: 0,
            id: id.to_string(),
            device_name: r"\\.\DISPLAY1".to_string(),
            friendly_name: friendly.to_string(),
            physical_rect: rect,
            work_rect: RectI::new(rect.left, rect.top, rect.right, rect.bottom - 40),
            dpi,
            scale_percent: ((dpi as f64 / 96.0) * 100.0).round() as u32,
            is_primary,
        }
    }

    #[test]
    fn windows_build_matrix_covers_win10_server2019_2022_2025_and_win11() {
        // Server 2019 / Win10 1809 (17763)
        let s2019 = WindowsBuildInfo::from_build(17_763);
        assert_eq!(s2019.dark_mode_attr(), Some(DwmDarkModeAttr::Legacy19));
        assert!(!s2019.supports_win11_dwm_corners());
        assert!(s2019.needs_rgn_rounded_corners_fallback());

        // Win10 22H2 (19045) & Server 2022 (20348)
        for b in [19_045, 20_348] {
            let info = WindowsBuildInfo::from_build(b);
            assert_eq!(info.dark_mode_attr(), Some(DwmDarkModeAttr::Modern20));
            assert!(!info.supports_win11_dwm_corners());
        }

        // Win11 (22631) & Server 2025 (26100)
        for b in [22_000, 22_631, 26_100] {
            let info = WindowsBuildInfo::from_build(b);
            assert_eq!(info.dark_mode_attr(), Some(DwmDarkModeAttr::Modern20));
            assert!(info.supports_win11_dwm_corners());
            assert!(!info.needs_rgn_rounded_corners_fallback());
        }
    }

    #[test]
    fn four_tier_monitor_matching_handles_dock_port_change_and_unplug_collapse() {
        let primary = make_mon(
            "DISPLAY#LEN8A90#4&1111#0",
            "Built-in OLED",
            RectI::new(0, 0, 2880, 1800),
            192,
            true,
        );
        let dell_portrait = make_mon(
            "DISPLAY#DELA182#5&NEWPORT#0",
            "DELL U2723QE",
            RectI::new(-1440, 0, 0, 2560),
            144,
            false,
        );
        let live_two = vec![primary.clone(), dell_portrait.clone()];

        // 换了扩展坞端口后设备路径变了，但 EDID 型号 token + 竖屏方向仍精准识别！
        let m = match_monitor_with_fallback(
            "DISPLAY#DELA182#5&OLDPORT#0",
            "DELL U2723QE",
            Some(MonitorOrientation::Portrait),
            Some(1),
            &live_two,
        )
        .expect("should match moved dock monitor");
        assert_eq!(m.monitor.id, dell_portrait.id);
        assert_eq!(m.quality, MonitorMatchQuality::HardwareFingerprint);

        // 拔掉全部外接屏仅剩笔记本单屏时优雅折叠回主屏
        let live_one = vec![primary.clone()];
        let collapsed = match_monitor_with_fallback(
            "DISPLAY#DELA182#5&OLDPORT#0",
            "DELL U2723QE",
            Some(MonitorOrientation::Portrait),
            Some(2),
            &live_one,
        )
        .expect("should collapse to primary");
        assert_eq!(collapsed.monitor.id, primary.id);
        assert_eq!(collapsed.quality, MonitorMatchQuality::PrimaryFallback);
    }

    #[test]
    fn normalized_work_rect_adapts_cleanly_from_4k_secondary_to_1080p_primary() {
        let src_4k = RectI::new(1920, 0, 5760, 2100);
        let dst_1080 = RectI::new(0, 0, 1920, 1040);
        let app_rect = RectI::new(1920 + 960, 0, 1920 + 2880, 1050); // 4K 右半上区
        let norm = NormalizedWorkRect::from_rect_in_work_area(app_rect, src_4k).unwrap();
        let placed = norm.resolve_in_work_area(dst_1080, 240, 160);
        assert_eq!(placed.left, 480);
        assert_eq!(placed.top, 0);
        assert_eq!(placed.width(), 960);
        assert_eq!(placed.height(), 520);
        assert!(placed.right <= dst_1080.right && placed.bottom <= dst_1080.bottom);
    }

    #[test]
    fn ensure_window_rect_rescues_window_when_remembered_monitor_unplugged() {
        let primary = make_mon("MON-PRIMARY", "Primary", RectI::new(0, 0, 1920, 1080), 96, true);
        let rel = NormalizedWorkRect {
            rel_x: 0.75,
            rel_y: 0.65,
            rel_w: 0.5,
            rel_h: 0.5,
        };
        let rect = ensure_window_rect_on_live_monitors(
            "MON-UNPLUGGED-DOCK",
            Some(rel),
            1200,
            700,
            Some(&primary),
            &[primary.clone()],
        );
        assert!(rect.left >= primary.work_rect.left);
        assert!(rect.top >= primary.work_rect.top);
        assert!(rect.right <= primary.work_rect.right);
        assert!(rect.bottom <= primary.work_rect.bottom);
    }

    #[test]
    fn binding_audit_preserves_offline_monitors_and_flags_deleted_desktops_safely() {
        let primary = make_mon("MON-1", "Main", RectI::new(0, 0, 1920, 1080), 96, true);
        let vd_reliable = VirtualDesktopTopology {
            reliable: true,
            current_id: "11111111-1111-1111-1111-111111111111".to_string(),
            desktops: vec![VirtualDesktopEntry {
                id: "11111111-1111-1111-1111-111111111111".to_string(),
                name: "Main".to_string(),
                index: 1,
                is_current: true,
            }],
        };

        let keys = [
            "MON-1",
            "MON-1@11111111-1111-1111-1111-111111111111",
            "MON-1@99999999-9999-9999-9999-999999999999", // 已被删除的虚拟桌面
            "MON-OFFLINE-DOCK",                           // 拔掉的副屏：保留！
        ];
        let audit = audit_monitor_desktop_bindings(keys, &[primary.clone()], &vd_reliable);
        assert_eq!(audit.online_binding_keys.len(), 2);
        assert_eq!(audit.offline_monitor_keys, vec!["MON-OFFLINE-DOCK"]);
        assert_eq!(
            audit.removed_desktop_keys,
            vec!["MON-1@99999999-9999-9999-9999-999999999999"]
        );

        // 当 Explorer 尚未返回可靠列表 (reliable=false) 时，绝不误标任何桌面为已删除
        let vd_unreliable = VirtualDesktopTopology {
            reliable: false,
            current_id: String::new(),
            desktops: Vec::new(),
        };
        let safe_audit = audit_monitor_desktop_bindings(keys, &[primary], &vd_unreliable);
        assert!(safe_audit.removed_desktop_keys.is_empty());
    }

    #[test]
    fn guid_parse_and_format_roundtrips_cleanly() {
        let s = "a1b2c3d4-e5f6-7890-1234-56789abcdef0";
        let g = parse_guid_string(s).expect("valid guid");
        assert_eq!(g.data1, 0xa1b2c3d4);
        assert_eq!(g.data2, 0xe5f6);
        assert_eq!(g.data3, 0x7890);
        assert_eq!(g.data4, [0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0]);
    }
}
