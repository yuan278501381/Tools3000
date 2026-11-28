// ─────────────────────────────────────────────────────────────────────────────
// main.rs — Tools3000 WindowKit 独立原生窗口管理引擎入口
//           负责 CLI 一次性命令分发、隐藏控制窗口通信、亚毫秒无阻塞低级钩子转发、
//           WinEvent 动态按需挂接、父进程守护、冷路径内存修剪与极端崩溃零丢窗兜底
// ─────────────────────────────────────────────────────────────────────────────

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod core;
mod features;
mod render;
mod ui_slint;

use crate::core::config::{
    import_powertoys_fancyzones_str, serialize_imported_layouts, SnapTriggerMode, WindowKitConfig,
};
use crate::core::constants::*;
use crate::core::json::parse_json;
use crate::core::logger::{configure_logger, flush_logger, next_trace_id, LogLevel};
use crate::core::topology::VirtualDesktopRegWatcher;
use crate::core::win_utils::{
    default_powertoys_layouts_path, emergency_restore_all_tray_windows, enumerate_monitors,
    install_crash_recovery_guards, monitors_to_json, query_runtime_stats_json, to_wide,
    trim_working_set_cold_path, write_text_file_atomic, ActionTargetOverrideGuard,
};
use crate::features::alt_drag::AltDragManager;
use crate::features::always_on_top::AlwaysOnTopManager;
use crate::features::focus_border::FocusBorderManager;
use crate::features::layout_editor::{EditorCloseOutcome, LayoutEditorManager, OpenEditorRequest};
use crate::features::layout_hub::{
    notify_host_hub_sync, HubEventOutcome, LayoutHubManager, OpenHubRequest,
};
use crate::features::multi_monitor::MultiMonitorManager;
use crate::features::opacity::OpacityManager;
use crate::features::quick_switch::{
    QuickSwitchDelegatedAction, QuickSwitchManager, QUICK_SWITCH_HAS_QUERY, QUICK_SWITCH_IS_OPEN,
};
use crate::features::snap_zones::SnapZonesManager;
use crate::features::workspace_capture::{StartCaptureRequest, WorkspaceCaptureManager};
use crate::features::workspaces::{
    capture_workspace_json, capture_workspace_v2_json, drain_workspace_runtime_events,
    launch_workspace_async, WorkspaceProgressOverlay, WorkspaceRuntimeEvent,
};
use crate::render::d2d::SharedGraphics;
use std::cell::RefCell;
use std::ffi::c_void;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicIsize, AtomicU32, Ordering};
use std::sync::mpsc;
use std::thread::{self, JoinHandle};
use std::time::{Instant, SystemTime};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_ALREADY_EXISTS, HANDLE, HINSTANCE, HWND, LPARAM, LRESULT,
    WAIT_OBJECT_0, WPARAM,
};
use windows::Win32::System::Com::{CoInitializeEx, CoUninitialize, COINIT_APARTMENTTHREADED};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::RemoteDesktop::{
    WTSRegisterSessionNotification, WTSUnRegisterSessionNotification, NOTIFY_FOR_THIS_SESSION,
};
use windows::Win32::System::Threading::{
    CreateMutexW, GetCurrentThread, GetCurrentThreadId, OpenProcess, SetThreadPriority,
    WaitForSingleObject, INFINITE, PROCESS_SYNCHRONIZE, THREAD_PRIORITY_HIGHEST,
};
use windows::Win32::UI::Accessibility::{SetWinEventHook, UnhookWinEvent, HWINEVENTHOOK};
use windows::Win32::UI::HiDpi::{
    SetProcessDpiAwarenessContext, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetAsyncKeyState, VK_BACK, VK_CONTROL, VK_DOWN, VK_ESCAPE, VK_LCONTROL, VK_LMENU, VK_LSHIFT,
    VK_LWIN, VK_MENU, VK_NUMPAD1, VK_NUMPAD9, VK_OEM_2, VK_OEM_3, VK_RCONTROL, VK_RETURN,
    VK_RMENU, VK_RSHIFT, VK_RWIN, VK_SHIFT, VK_SPACE, VK_TAB, VK_UP,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CallNextHookEx, CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, FindWindowW,
    GetForegroundWindow, GetMessageW, KillTimer, LoadCursorW, MsgWaitForMultipleObjectsEx,
    PeekMessageW, PostMessageW, PostQuitMessage, PostThreadMessageW, RegisterClassExW,
    RegisterWindowMessageW, SetTimer, SetWindowsHookExW, TranslateMessage, UnhookWindowsHookEx,
    EVENT_OBJECT_CLOAKED, EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, EVENT_OBJECT_LOCATIONCHANGE,
    EVENT_OBJECT_SHOW, EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND,
    EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZESTART, HHOOK,
    IDC_ARROW, KBDLLHOOKSTRUCT, LLKHF_ALTDOWN, LLKHF_EXTENDED, LLKHF_INJECTED, MSG, MSLLHOOKSTRUCT,
    MWMO_INPUTAVAILABLE, OBJID_WINDOW, PBT_APMRESUMEAUTOMATIC, PBT_APMRESUMECRITICAL,
    PBT_APMRESUMESUSPEND, PM_NOREMOVE, PM_REMOVE, QS_ALLINPUT, WH_KEYBOARD_LL, WH_MOUSE_LL,
    WINEVENT_OUTOFCONTEXT, WINEVENT_SKIPOWNPROCESS, WM_APP, WM_CLOSE, WM_DISPLAYCHANGE,
    WM_ENDSESSION, WM_KEYDOWN, WM_KEYUP, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE,
    WM_MOUSEWHEEL, WM_POWERBROADCAST, WM_QUERYENDSESSION, WM_QUIT, WM_RBUTTONDOWN, WM_RBUTTONUP,
    WM_SETTINGCHANGE, WM_SYSKEYDOWN, WM_SYSKEYUP, WM_TIMER, WM_WTSSESSION_CHANGE, WNDCLASSEXW,
    WS_EX_TOOLWINDOW, WS_POPUP, WTS_CONSOLE_CONNECT, WTS_REMOTE_CONNECT, WTS_SESSION_LOCK,
    WTS_SESSION_LOGON, WTS_SESSION_UNLOCK,
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. 低级钩子严格无锁原子共享状态（保障钩子回调 < 0.05ms，绝不阻塞、绝不锁竞争）
// ─────────────────────────────────────────────────────────────────────────────

static HOOK_CONTROL_HWND: AtomicIsize = AtomicIsize::new(0);
static HOOK_MUTED: AtomicBool = AtomicBool::new(false);
static HOOK_QUICK_SWITCH_ENABLED: AtomicBool = AtomicBool::new(false);
static HOOK_SAME_APP_SWITCH_ENABLED: AtomicBool = AtomicBool::new(false);
static HOOK_ALT_DRAG_ENABLED: AtomicBool = AtomicBool::new(false);
static HOOK_ALT_DRAG_REQUIRE_CTRL: AtomicBool = AtomicBool::new(false);
static HOOK_ALT_DRAG_ACTIVE: AtomicU32 = AtomicU32::new(0); // 0=空闲, 1=移动, 2=缩放
static ALT_DRAG_LATEST_PT: AtomicI64 = AtomicI64::new(0);
static ALT_DRAG_MOVE_PENDING: AtomicBool = AtomicBool::new(false);
static HOOK_OPACITY_WHEEL_ENABLED: AtomicBool = AtomicBool::new(false);
static HOOK_SNAP_DRAGGING: AtomicBool = AtomicBool::new(false);
static HOOK_SNAP_RCLICK_ENABLED: AtomicBool = AtomicBool::new(false);
/// 右键触发分屏叠加层时吞掉紧随其后的 WM_RBUTTONUP，防止被拖窗口标题栏弹出系统菜单（复刻 FancyZones）
static HOOK_SNAP_SWALLOW_RBUTTONUP: AtomicBool = AtomicBool::new(false);
/// 吞掉与 Alt+Tab / Alt+` / QuickSwitch 直通按键配对的 KEYUP，彻底杜绝孤立按键泄漏或系统蜂鸣
static HOOK_SWALLOW_TAB_UP: AtomicBool = AtomicBool::new(false);
static HOOK_SWALLOW_GRAVE_UP: AtomicBool = AtomicBool::new(false);
static HOOK_SWALLOW_QS_UP_VK: AtomicU32 = AtomicU32::new(0);
/// 独立输入钩子线程按需装卸目标开关
static HOOK_WANT_KBD: AtomicBool = AtomicBool::new(false);
static HOOK_WANT_MOUSE: AtomicBool = AtomicBool::new(false);
/// 低级键盘钩子内部无锁原子追踪物理修饰键位图（位 0=LAlt..7=RWin, 位 8=AltGr 伴生假 LCtrl）
static HOOK_PHYS_MODS: AtomicU32 = AtomicU32::new(0);
static HOOK_LAST_LCTRL_DOWN_TIME: AtomicU32 = AtomicU32::new(0);
static HOOK_LAST_LCTRL_SCAN: AtomicU32 = AtomicU32::new(0);
static HOOK_LAST_RCTRL_DOWN_TIME: AtomicU32 = AtomicU32::new(0);
static HOOK_LAST_WIN_DOWN_TIME: AtomicU32 = AtomicU32::new(0);
static HOOK_LAST_SHIFT_DOWN_TIME: AtomicU32 = AtomicU32::new(0);
/// 钩子侧即刻感知的 Alt+Tab / Alt+` 会话激活态：
/// 在首记 Alt+Tab 命中的 0.005ms 即置 true，彻底消除主线程开窗(提取图标 ~20ms)期间极速松开 Alt 漏发 ALT_RELEASED 导致面板卡开！
static HOOK_QS_ALT_SESSION_ACTIVE: AtomicBool = AtomicBool::new(false);

const PHYS_MOD_LALT: u32 = 1 << 0;
const PHYS_MOD_RALT: u32 = 1 << 1;
const PHYS_MOD_LCTRL: u32 = 1 << 2;
const PHYS_MOD_RCTRL: u32 = 1 << 3;
const PHYS_MOD_LSHIFT: u32 = 1 << 4;
const PHYS_MOD_RSHIFT: u32 = 1 << 5;
const PHYS_MOD_LWIN: u32 = 1 << 6;
const PHYS_MOD_RWIN: u32 = 1 << 7;
/// 国际键盘布局按右 Alt (AltGr) 时系统合成伴生假 LCtrl 标记位
const PHYS_MOD_ALTGR_LCTRL: u32 = 1 << 8;
/// Windows 驱动为 AltGr 合成假 LCtrl 的特征扫描码
const ALTGR_FAKE_LCTRL_SCAN: u32 = 0x21D;
/// AltGr 伴生 LCtrl 与随后扩展 RMENU 之间的最大时间窗（毫秒）
const ALTGR_PAIR_WINDOW_MS: u32 = 2;
/// 物理修饰键位图陈旧位自愈宽限窗（毫秒）：
/// 若某 Ctrl/Win/Shift 位在超过 60ms 后 GetAsyncKeyState 仍回报抬起，说明此前 KEYUP 在远控/UAC/安全桌面丢失，立即自愈清除！
const PHYS_MOD_STALE_HEAL_MS: u32 = 60;

const WM_HOOK_THREAD_RECONCILE: u32 = WM_APP + 201;
const WM_HOOK_THREAD_REINSTALL: u32 = WM_APP + 202;
static WM_TASKBAR_CREATED: AtomicU32 = AtomicU32::new(0);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct EffectiveMods {
    alt: bool,
    ctrl: bool,
    shift: bool,
    win: bool,
}

/// 纯函数：根据低级键盘事件无锁维护物理修饰键位图（忽略注入键，并识别 AltGr 伴生假 LCtrl）
#[inline]
#[allow(clippy::too_many_arguments)]
fn update_phys_mods_state(
    mut mods: u32,
    mut last_lctrl_time: u32,
    mut last_lctrl_scan: u32,
    vk: u16,
    scan_code: u32,
    flags: u32,
    time: u32,
    is_down: bool,
    is_up: bool,
) -> (u32, u32, u32) {
    // 忽略 LLKHF_INJECTED 注入键，防止宏工具/自动化输入污染真实物理按键位图
    if (flags & LLKHF_INJECTED.0) != 0 {
        return (mods, last_lctrl_time, last_lctrl_scan);
    }
    let extended = (flags & LLKHF_EXTENDED.0) != 0;

    if is_down {
        match vk {
            x if x == VK_LMENU.0 => mods |= PHYS_MOD_LALT,
            x if x == VK_RMENU.0 => {
                mods |= PHYS_MOD_RALT;
                if (mods & PHYS_MOD_LCTRL) != 0
                    && (last_lctrl_scan == ALTGR_FAKE_LCTRL_SCAN
                        || time.wrapping_sub(last_lctrl_time) <= ALTGR_PAIR_WINDOW_MS)
                {
                    mods |= PHYS_MOD_ALTGR_LCTRL;
                }
            }
            x if x == VK_MENU.0 => {
                if extended {
                    mods |= PHYS_MOD_RALT;
                } else {
                    mods |= PHYS_MOD_LALT;
                }
            }
            x if x == VK_LCONTROL.0 => {
                mods |= PHYS_MOD_LCTRL;
                last_lctrl_time = time;
                last_lctrl_scan = scan_code;
                if scan_code == ALTGR_FAKE_LCTRL_SCAN {
                    mods |= PHYS_MOD_ALTGR_LCTRL;
                } else {
                    mods &= !PHYS_MOD_ALTGR_LCTRL;
                }
            }
            x if x == VK_RCONTROL.0 => mods |= PHYS_MOD_RCTRL,
            x if x == VK_CONTROL.0 => {
                if extended {
                    mods |= PHYS_MOD_RCTRL;
                } else {
                    mods |= PHYS_MOD_LCTRL;
                }
            }
            x if x == VK_LSHIFT.0 => mods |= PHYS_MOD_LSHIFT,
            x if x == VK_RSHIFT.0 => mods |= PHYS_MOD_RSHIFT,
            x if x == VK_SHIFT.0 => mods |= PHYS_MOD_LSHIFT,
            x if x == VK_LWIN.0 => mods |= PHYS_MOD_LWIN,
            x if x == VK_RWIN.0 => mods |= PHYS_MOD_RWIN,
            _ => {}
        }
    } else if is_up {
        match vk {
            x if x == VK_LMENU.0 => mods &= !PHYS_MOD_LALT,
            x if x == VK_RMENU.0 => {
                mods &= !PHYS_MOD_RALT;
                if (mods & PHYS_MOD_ALTGR_LCTRL) != 0 {
                    mods &= !(PHYS_MOD_ALTGR_LCTRL | PHYS_MOD_LCTRL);
                }
            }
            x if x == VK_MENU.0 => mods &= !(PHYS_MOD_LALT | PHYS_MOD_RALT),
            x if x == VK_LCONTROL.0 => mods &= !(PHYS_MOD_LCTRL | PHYS_MOD_ALTGR_LCTRL),
            x if x == VK_RCONTROL.0 => mods &= !PHYS_MOD_RCTRL,
            x if x == VK_CONTROL.0 => {
                mods &= !(PHYS_MOD_LCTRL | PHYS_MOD_RCTRL | PHYS_MOD_ALTGR_LCTRL)
            }
            x if x == VK_LSHIFT.0 => mods &= !PHYS_MOD_LSHIFT,
            x if x == VK_RSHIFT.0 => mods &= !PHYS_MOD_RSHIFT,
            x if x == VK_SHIFT.0 => mods &= !(PHYS_MOD_LSHIFT | PHYS_MOD_RSHIFT),
            x if x == VK_LWIN.0 => mods &= !PHYS_MOD_LWIN,
            x if x == VK_RWIN.0 => mods &= !PHYS_MOD_RWIN,
            _ => {}
        }
    }

    (mods, last_lctrl_time, last_lctrl_scan)
}

/// 陈旧物理修饰键位自动自愈：若 Ctrl / Win / Shift 位图仍置位但 GetAsyncKeyState 回报抬起且已超过宽限窗，
/// 立即清零对应残留位，彻底根治远控/UAC/安全桌面丢失单次 KEYUP 后 ctrl_down/win_down 永久误判并放行原生 Alt+Tab！
#[inline]
#[allow(clippy::too_many_arguments)]
fn heal_stale_phys_mods(
    mut mods: u32,
    now_ms: u32,
    last_lctrl_ms: u32,
    last_rctrl_ms: u32,
    last_win_ms: u32,
    last_shift_ms: u32,
    async_lctrl: bool,
    async_rctrl: bool,
    async_shift: bool,
    async_win: bool,
) -> u32 {
    if (mods & PHYS_MOD_LCTRL) != 0
        && !async_lctrl
        && now_ms.wrapping_sub(last_lctrl_ms) > PHYS_MOD_STALE_HEAL_MS
    {
        mods &= !(PHYS_MOD_LCTRL | PHYS_MOD_ALTGR_LCTRL);
    }
    if (mods & PHYS_MOD_RCTRL) != 0
        && !async_rctrl
        && now_ms.wrapping_sub(last_rctrl_ms) > PHYS_MOD_STALE_HEAL_MS
    {
        mods &= !PHYS_MOD_RCTRL;
    }
    if (mods & (PHYS_MOD_LWIN | PHYS_MOD_RWIN)) != 0
        && !async_win
        && now_ms.wrapping_sub(last_win_ms) > PHYS_MOD_STALE_HEAL_MS
    {
        mods &= !(PHYS_MOD_LWIN | PHYS_MOD_RWIN);
    }
    if (mods & (PHYS_MOD_LSHIFT | PHYS_MOD_RSHIFT)) != 0
        && !async_shift
        && now_ms.wrapping_sub(last_shift_ms) > PHYS_MOD_STALE_HEAL_MS
    {
        mods &= !(PHYS_MOD_LSHIFT | PHYS_MOD_RSHIFT);
    }
    mods
}

/// 融合 WM_SYSKEYDOWN、LLKHF_ALTDOWN、原子物理修饰键位图与 GetAsyncKeyState 求解有效修饰键状态
/// 1) 彻底消除快按 Alt+Tab 时 GetAsyncKeyState 微秒级滞后竞态；
/// 2) 抵御外部注入 Alt UP（phys_mods 忽略 LLKHF_INJECTED，持续锁住物理 Alt 按住态）；
/// 3) 当右 Alt(AltGr) 伴生假 LCtrl 生效时绝不误判为 Ctrl+Alt，避免漏出原生 Alt+Tab。
#[inline]
#[allow(clippy::too_many_arguments)]
fn resolve_effective_mods(
    phys_mods: u32,
    kb_flags: u32,
    msg: u32,
    async_alt: bool,
    async_lctrl: bool,
    async_rctrl: bool,
    async_shift: bool,
    async_win: bool,
) -> EffectiveMods {
    let altgr_fake_lctrl =
        (phys_mods & PHYS_MOD_RALT) != 0 && (phys_mods & PHYS_MOD_ALTGR_LCTRL) != 0;
    let alt = (kb_flags & LLKHF_ALTDOWN.0) != 0
        || msg == WM_SYSKEYDOWN
        || msg == WM_SYSKEYUP
        || (phys_mods & (PHYS_MOD_LALT | PHYS_MOD_RALT)) != 0
        || async_alt;
    let real_lctrl = (phys_mods & PHYS_MOD_LCTRL) != 0 && !altgr_fake_lctrl;
    let real_rctrl = (phys_mods & PHYS_MOD_RCTRL) != 0;
    let async_real_ctrl = if altgr_fake_lctrl {
        async_rctrl
    } else {
        async_lctrl || async_rctrl
    };
    let ctrl = real_lctrl || real_rctrl || async_real_ctrl;
    let shift = (phys_mods & (PHYS_MOD_LSHIFT | PHYS_MOD_RSHIFT)) != 0 || async_shift;
    let win = (phys_mods & (PHYS_MOD_LWIN | PHYS_MOD_RWIN)) != 0 || async_win;
    EffectiveMods {
        alt,
        ctrl,
        shift,
        win,
    }
}

/// 钩子层 QuickSwitch 按键裁决纯函数（便于离线 100% 单元测试全序列，杜绝连按 Tab 漏出系统原生切屏）
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum HookQsDecision {
    PassThrough,
    SwallowOnly,
    AltTabStep { reverse: bool, same_app: bool },
    QuickSwitchKey { vk: u16, mod_mask: isize },
}

#[inline]
#[allow(clippy::too_many_arguments)]
fn decide_quick_switch_hook_action(
    vk: u16,
    is_down: bool,
    is_up: bool,
    mods: EffectiveMods,
    qs_enabled: bool,
    same_app_enabled: bool,
    qs_open: bool,
    qs_alt_session_active: bool,
    has_query: bool,
) -> HookQsDecision {
    let active_qs = qs_open || qs_alt_session_active;

    // 1. QuickSwitch 面板已打开或 Alt+Tab 会话已激活期间：
    //    所有 Tab DOWN/UP（只要不是 Win+Tab）100% 优先吞噬并在我方窗口内部切换，绝不漏给 Windows 原生 Alt+Tab！
    if active_qs && vk == VK_TAB.0 && !mods.win && !mods.ctrl {
        if is_up {
            return HookQsDecision::SwallowOnly;
        }
        if is_down {
            if mods.alt {
                return HookQsDecision::AltTabStep {
                    reverse: mods.shift,
                    same_app: false,
                };
            }
            let mod_mask: isize = if mods.shift { 4 } else { 0 };
            return HookQsDecision::QuickSwitchKey { vk, mod_mask };
        }
    }

    if !is_down {
        return HookQsDecision::PassThrough;
    }

    // 2. 轨道 A：全局增强 Alt+Tab / Alt+Shift+Tab（绝不干扰原生 Ctrl+Alt+Tab 与 Win+Tab）
    if qs_enabled && vk == VK_TAB.0 && mods.alt && !mods.ctrl && !mods.win {
        return HookQsDecision::AltTabStep {
            reverse: mods.shift,
            same_app: false,
        };
    }

    // 3. 轨道 B：独立同应用切窗 Alt+` / Alt+Shift+`
    if same_app_enabled && vk == VK_OEM_3.0 && mods.alt && !mods.ctrl && !mods.win {
        return HookQsDecision::AltTabStep {
            reverse: mods.shift,
            same_app: true,
        };
    }

    // 4. QuickSwitch 已打开态直通键盘流（1-9 秒切、Alt+方向/回车/Esc/退格/空格/斜杠/A-Z 无缝入搜）
    if qs_open && !mods.win {
        let mod_mask: isize = (if mods.alt { 1 } else { 0 })
            | (if mods.ctrl { 2 } else { 0 })
            | (if mods.shift { 4 } else { 0 });

        let is_digit = (0x31..=0x39).contains(&vk)
            || (vk >= VK_NUMPAD1.0 && vk <= VK_NUMPAD9.0);
        let intercept_digit = is_digit && (mods.alt || mods.ctrl || !has_query);
        let intercept_alt_nav_or_search = mods.alt
            && !mods.ctrl
            && (vk == VK_UP.0
                || vk == VK_DOWN.0
                || vk == VK_RETURN.0
                || vk == VK_ESCAPE.0
                || vk == VK_BACK.0
                || vk == VK_SPACE.0
                || vk == VK_OEM_2.0
                || (0x41..=0x5A).contains(&vk));

        if intercept_digit || intercept_alt_nav_or_search {
            return HookQsDecision::QuickSwitchKey { vk, mod_mask };
        }
    }

    HookQsDecision::PassThrough
}

#[inline]
fn clear_hook_phys_mods() {
    HOOK_PHYS_MODS.store(0, Ordering::Release);
    HOOK_LAST_LCTRL_DOWN_TIME.store(0, Ordering::Release);
    HOOK_LAST_LCTRL_SCAN.store(0, Ordering::Release);
    HOOK_LAST_RCTRL_DOWN_TIME.store(0, Ordering::Release);
    HOOK_LAST_WIN_DOWN_TIME.store(0, Ordering::Release);
    HOOK_LAST_SHIFT_DOWN_TIME.store(0, Ordering::Release);
    HOOK_QS_ALT_SESSION_ACTIVE.store(false, Ordering::Release);
    HOOK_SWALLOW_TAB_UP.store(false, Ordering::Release);
    HOOK_SWALLOW_GRAVE_UP.store(false, Ordering::Release);
    HOOK_SWALLOW_QS_UP_VK.store(0, Ordering::Release);
}

const ALT_DRAG_EVT_BEGIN_MOVE: usize = 1;
const ALT_DRAG_EVT_BEGIN_RESIZE: usize = 2;
const ALT_DRAG_EVT_MOVE_TICK: usize = 3;
const ALT_DRAG_EVT_END: usize = 4;

fn pack_point_i64(x: i32, y: i32) -> i64 {
    ((x as u32 as u64) | ((y as u32 as u64) << 32)) as i64
}

fn unpack_point_i64(v: i64) -> (i32, i32) {
    let u = v as u64;
    ((u & 0xFFFF_FFFF) as u32 as i32, (u >> 32) as u32 as i32)
}

#[inline]
fn async_key_down(vk: u16) -> bool {
    unsafe { (GetAsyncKeyState(vk as i32) as u16 & 0x8000) != 0 }
}

unsafe extern "system" fn low_level_keyboard_proc(
    code: i32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if code >= 0 && lparam.0 != 0 {
        let ctrl_raw = HOOK_CONTROL_HWND.load(Ordering::Acquire);
        if ctrl_raw != 0 {
            let kb = &*(lparam.0 as *const KBDLLHOOKSTRUCT);
            let msg = wparam.0 as u32;
            let vk = kb.vkCode as u16;
            let ctrl_hwnd = HWND(ctrl_raw as *mut c_void);

            let is_down = msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN;
            let is_up = msg == WM_SYSKEYUP || msg == WM_KEYUP;
            let is_injected = (kb.flags.0 & LLKHF_INJECTED.0) != 0;

            // 入口第一时间无锁维护物理修饰键位图（忽略注入键，杜绝外部 SendInput 污染真实物理按键状态）
            let (mut phys_mods, next_lctrl_time, next_lctrl_scan) = update_phys_mods_state(
                HOOK_PHYS_MODS.load(Ordering::Acquire),
                HOOK_LAST_LCTRL_DOWN_TIME.load(Ordering::Acquire),
                HOOK_LAST_LCTRL_SCAN.load(Ordering::Acquire),
                vk,
                kb.scanCode,
                kb.flags.0,
                kb.time,
                is_down,
                is_up,
            );
            if !is_injected && is_down {
                if vk == VK_RCONTROL.0 {
                    HOOK_LAST_RCTRL_DOWN_TIME.store(kb.time, Ordering::Release);
                } else if vk == VK_LWIN.0 || vk == VK_RWIN.0 {
                    HOOK_LAST_WIN_DOWN_TIME.store(kb.time, Ordering::Release);
                } else if vk == VK_LSHIFT.0 || vk == VK_RSHIFT.0 || vk == VK_SHIFT.0 {
                    HOOK_LAST_SHIFT_DOWN_TIME.store(kb.time, Ordering::Release);
                }
            }

            let async_alt = async_key_down(VK_MENU.0);
            let async_lctrl = async_key_down(VK_LCONTROL.0);
            let async_rctrl = async_key_down(VK_RCONTROL.0);
            let async_shift = async_key_down(VK_SHIFT.0);
            let async_win = async_key_down(VK_LWIN.0) || async_key_down(VK_RWIN.0);

            // 陈旧 Ctrl/Win/Shift 位图 60ms 自动自愈，杜绝远控/UAC 丢单次 KEYUP 后永远放行原生 Alt+Tab
            phys_mods = heal_stale_phys_mods(
                phys_mods,
                kb.time,
                next_lctrl_time,
                HOOK_LAST_RCTRL_DOWN_TIME.load(Ordering::Acquire),
                HOOK_LAST_WIN_DOWN_TIME.load(Ordering::Acquire),
                HOOK_LAST_SHIFT_DOWN_TIME.load(Ordering::Acquire),
                async_lctrl,
                async_rctrl,
                async_shift,
                async_win,
            );
            HOOK_PHYS_MODS.store(phys_mods, Ordering::Release);
            HOOK_LAST_LCTRL_DOWN_TIME.store(next_lctrl_time, Ordering::Release);
            HOOK_LAST_LCTRL_SCAN.store(next_lctrl_scan, Ordering::Release);

            // 配对按键抬起吞噬（防止系统或后台软件收到孤立 Tab / ` / 1-9 / 字母抬起事件）
            if is_up {
                if vk == VK_TAB.0 && HOOK_SWALLOW_TAB_UP.swap(false, Ordering::AcqRel) {
                    return LRESULT(1);
                }
                if vk == VK_OEM_3.0 && HOOK_SWALLOW_GRAVE_UP.swap(false, Ordering::AcqRel) {
                    return LRESULT(1);
                }
                let swallow_vk = HOOK_SWALLOW_QS_UP_VK.load(Ordering::Acquire);
                if swallow_vk != 0 && swallow_vk == vk as u32 {
                    HOOK_SWALLOW_QS_UP_VK.store(0, Ordering::Release);
                    return LRESULT(1);
                }
            }

            if !HOOK_MUTED.load(Ordering::Acquire) {
                let mods = resolve_effective_mods(
                    phys_mods,
                    kb.flags.0,
                    msg,
                    async_alt,
                    async_lctrl,
                    async_rctrl,
                    async_shift,
                    async_win,
                );
                let qs_open = QUICK_SWITCH_IS_OPEN.load(Ordering::Acquire);
                let qs_alt_session = HOOK_QS_ALT_SESSION_ACTIVE.load(Ordering::Acquire);
                let decision = decide_quick_switch_hook_action(
                    vk,
                    is_down,
                    is_up,
                    mods,
                    HOOK_QUICK_SWITCH_ENABLED.load(Ordering::Acquire),
                    HOOK_SAME_APP_SWITCH_ENABLED.load(Ordering::Acquire),
                    qs_open,
                    qs_alt_session,
                    QUICK_SWITCH_HAS_QUERY.load(Ordering::Acquire),
                );

                match decision {
                    HookQsDecision::SwallowOnly => {
                        return LRESULT(1);
                    }
                    HookQsDecision::AltTabStep { reverse, same_app } => {
                        HOOK_QS_ALT_SESSION_ACTIVE.store(true, Ordering::Release);
                        if same_app {
                            HOOK_SWALLOW_GRAVE_UP.store(true, Ordering::Release);
                        } else {
                            HOOK_SWALLOW_TAB_UP.store(true, Ordering::Release);
                        }
                        let _ = PostMessageW(
                            ctrl_hwnd,
                            WM_WK_INTERNAL_ALT_TAB,
                            WPARAM(if reverse { 1usize } else { 0usize }),
                            LPARAM(if same_app { 1 } else { 0 }),
                        );
                        return LRESULT(1);
                    }
                    HookQsDecision::QuickSwitchKey {
                        vk: target_vk,
                        mod_mask,
                    } => {
                        if target_vk == VK_TAB.0 {
                            HOOK_SWALLOW_TAB_UP.store(true, Ordering::Release);
                        } else {
                            HOOK_SWALLOW_QS_UP_VK.store(target_vk as u32, Ordering::Release);
                        }
                        let _ = PostMessageW(
                            ctrl_hwnd,
                            WM_WK_INTERNAL_QS_KEY,
                            WPARAM(target_vk as usize),
                            LPARAM(mod_mask),
                        );
                        return LRESULT(1);
                    }
                    HookQsDecision::PassThrough => {}
                }
            }

            // 真实物理 Alt 键全部抬起时通知 QuickSwitch 提交切换（严格过滤 LLKHF_INJECTED 注入脉冲，
            // 并借助 HOOK_QS_ALT_SESSION_ACTIVE 消除开窗未完期间极速松开 Alt 漏发提交事件的竞态）
            if is_up
                && !is_injected
                && (vk == VK_LMENU.0 || vk == VK_RMENU.0 || vk == VK_MENU.0)
                && (phys_mods & (PHYS_MOD_LALT | PHYS_MOD_RALT)) == 0
                && (QUICK_SWITCH_IS_OPEN.load(Ordering::Acquire)
                    || HOOK_QS_ALT_SESSION_ACTIVE.swap(false, Ordering::AcqRel))
            {
                HOOK_QS_ALT_SESSION_ACTIVE.store(false, Ordering::Release);
                let _ = PostMessageW(
                    ctrl_hwnd,
                    WM_WK_INTERNAL_ALT_RELEASED,
                    WPARAM(0),
                    LPARAM(0),
                );
            }
        }
    }
    CallNextHookEx(HHOOK::default(), code, wparam, lparam)
}

unsafe extern "system" fn low_level_mouse_proc(
    code: i32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if code >= 0 && lparam.0 != 0 && !HOOK_MUTED.load(Ordering::Acquire) {
        let ctrl_raw = HOOK_CONTROL_HWND.load(Ordering::Acquire);
        if ctrl_raw != 0 {
            let ms = &*(lparam.0 as *const MSLLHOOKSTRUCT);
            let msg = wparam.0 as u32;
            let ctrl_hwnd = HWND(ctrl_raw as *mut c_void);
            let drag_state = HOOK_ALT_DRAG_ACTIVE.load(Ordering::Acquire);

            // 1. Alt+Drag 进行中的移动合并与抬起结束
            if drag_state != 0 {
                match msg {
                    WM_MOUSEMOVE => {
                        ALT_DRAG_LATEST_PT
                            .store(pack_point_i64(ms.pt.x, ms.pt.y), Ordering::Release);
                        if !ALT_DRAG_MOVE_PENDING.swap(true, Ordering::AcqRel) {
                            let _ = PostMessageW(
                                ctrl_hwnd,
                                WM_WK_INTERNAL_ALT_DRAG_EVENT,
                                WPARAM(ALT_DRAG_EVT_MOVE_TICK),
                                LPARAM(0),
                            );
                        }
                        return CallNextHookEx(HHOOK::default(), code, wparam, lparam);
                    }
                    WM_LBUTTONUP if drag_state == 1 => {
                        HOOK_ALT_DRAG_ACTIVE.store(0, Ordering::Release);
                        let _ = PostMessageW(
                            ctrl_hwnd,
                            WM_WK_INTERNAL_ALT_DRAG_EVENT,
                            WPARAM(ALT_DRAG_EVT_END),
                            LPARAM(0),
                        );
                        return LRESULT(1);
                    }
                    WM_RBUTTONUP if drag_state == 2 => {
                        HOOK_ALT_DRAG_ACTIVE.store(0, Ordering::Release);
                        let _ = PostMessageW(
                            ctrl_hwnd,
                            WM_WK_INTERNAL_ALT_DRAG_EVENT,
                            WPARAM(ALT_DRAG_EVT_END),
                            LPARAM(0),
                        );
                        return LRESULT(1);
                    }
                    _ => {}
                }
            }

            let alt_down = async_key_down(VK_MENU.0);
            let drag_mod_ok = alt_down
                && (!HOOK_ALT_DRAG_REQUIRE_CTRL.load(Ordering::Acquire)
                    || async_key_down(VK_CONTROL.0));

            // 2. Alt+左键 / Alt+右键（或 Ctrl+Alt）发起拖动或缩放
            if drag_mod_ok && HOOK_ALT_DRAG_ENABLED.load(Ordering::Acquire) {
                if msg == WM_LBUTTONDOWN {
                    ALT_DRAG_LATEST_PT.store(pack_point_i64(ms.pt.x, ms.pt.y), Ordering::Release);
                    HOOK_ALT_DRAG_ACTIVE.store(1, Ordering::Release);
                    let _ = PostMessageW(
                        ctrl_hwnd,
                        WM_WK_INTERNAL_ALT_DRAG_EVENT,
                        WPARAM(ALT_DRAG_EVT_BEGIN_MOVE),
                        LPARAM(0),
                    );
                    return LRESULT(1);
                }
                if msg == WM_RBUTTONDOWN {
                    ALT_DRAG_LATEST_PT.store(pack_point_i64(ms.pt.x, ms.pt.y), Ordering::Release);
                    HOOK_ALT_DRAG_ACTIVE.store(2, Ordering::Release);
                    let _ = PostMessageW(
                        ctrl_hwnd,
                        WM_WK_INTERNAL_ALT_DRAG_EVENT,
                        WPARAM(ALT_DRAG_EVT_BEGIN_RESIZE),
                        LPARAM(0),
                    );
                    return LRESULT(1);
                }
            }

            // 3. Alt+滚轮 调节鼠标下方窗口透明度
            if alt_down && msg == WM_MOUSEWHEEL && HOOK_OPACITY_WHEEL_ENABLED.load(Ordering::Acquire)
            {
                let delta = ((ms.mouseData >> 16) & 0xFFFF) as i16 as isize;
                let _ = PostMessageW(
                    ctrl_hwnd,
                    WM_WK_INTERNAL_OPACITY_WHEEL,
                    WPARAM(0),
                    LPARAM(delta),
                );
                return LRESULT(1);
            }

            // 4. SnapZones 拖拽中右键切换分屏叠加层（同步吞掉配对 WM_RBUTTONUP，防弹出系统标题栏菜单）
            if msg == WM_RBUTTONUP && HOOK_SNAP_SWALLOW_RBUTTONUP.swap(false, Ordering::AcqRel) {
                return LRESULT(1);
            }
            if msg == WM_RBUTTONDOWN
                && HOOK_SNAP_RCLICK_ENABLED.load(Ordering::Acquire)
                && HOOK_SNAP_DRAGGING.load(Ordering::Acquire)
            {
                HOOK_SNAP_SWALLOW_RBUTTONUP.store(true, Ordering::Release);
                let _ = PostMessageW(
                    ctrl_hwnd,
                    WM_WK_INTERNAL_SNAP_RIGHT_CLICK,
                    WPARAM(0),
                    LPARAM(0),
                );
                return LRESULT(1);
            }
        }
    }
    CallNextHookEx(HHOOK::default(), code, wparam, lparam)
}

// ─────────────────────────────────────────────────────────────────────────────
// 1.5 独立永不阻塞低级输入钩子线程 (Tools3000-InputHooks)
//     将 WH_KEYBOARD_LL / WH_MOUSE_LL 与主线程图标提取、磁盘读写、D2D 渲染彻底物理隔离，
//     保证钩子回调恒定 < 0.01ms，100% 根治 Windows 300ms LowLevelHooksTimeout 超时漏拦截原生 Alt+Tab！
// ─────────────────────────────────────────────────────────────────────────────

struct InputHookWorker {
    thread_id: u32,
    join_handle: Option<JoinHandle<()>>,
}

impl InputHookWorker {
    fn spawn() -> Option<Self> {
        let (tx, rx) = mpsc::sync_channel::<u32>(1);
        let handle = thread::Builder::new()
            .name("Tools3000-InputHooks".to_string())
            .stack_size(128 * 1024)
            .spawn(move || unsafe {
                // 提升输入钩子线程优先级至 HIGHEST，避免 400 万像素高分屏 Slint 帧渲染抢占 CPU 触发 300ms LowLevelHooksTimeout
                let _ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                // 强制初始化线程消息队列后再通告线程 ID，确保 PostThreadMessageW 首次即成功
                let mut msg = MSG::default();
                let _ = PeekMessageW(&mut msg, HWND::default(), 0, 0, PM_NOREMOVE);
                let tid = GetCurrentThreadId();
                let _ = tx.send(tid);

                let mut kbd_hook: Option<HHOOK> = None;
                let mut mouse_hook: Option<HHOOK> = None;
                let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);

                let sync_hooks = |kbd: &mut Option<HHOOK>, mouse: &mut Option<HHOOK>| {
                    let want_k = HOOK_WANT_KBD.load(Ordering::Acquire);
                    let want_m = HOOK_WANT_MOUSE.load(Ordering::Acquire);
                    if want_k && kbd.is_none() {
                        if let Ok(h) =
                            SetWindowsHookExW(WH_KEYBOARD_LL, Some(low_level_keyboard_proc), hinst, 0)
                        {
                            *kbd = Some(h);
                        }
                    } else if !want_k {
                        if let Some(h) = kbd.take() {
                            let _ = UnhookWindowsHookEx(h);
                        }
                    }
                    if want_m && mouse.is_none() {
                        if let Ok(h) =
                            SetWindowsHookExW(WH_MOUSE_LL, Some(low_level_mouse_proc), hinst, 0)
                        {
                            *mouse = Some(h);
                        }
                    } else if !want_m {
                        if let Some(h) = mouse.take() {
                            let _ = UnhookWindowsHookEx(h);
                        }
                    }
                };

                sync_hooks(&mut kbd_hook, &mut mouse_hook);

                while GetMessageW(&mut msg, HWND::default(), 0, 0).as_bool() {
                    if msg.message == WM_HOOK_THREAD_RECONCILE {
                        sync_hooks(&mut kbd_hook, &mut mouse_hook);
                    } else if msg.message == WM_HOOK_THREAD_REINSTALL {
                        // 锁屏解锁 / 睡眠唤醒 / 时钟跳变健康自愈：安全卸载旧钩子、清空物理修饰键残留并重新挂钩
                        clear_hook_phys_mods();
                        if let Some(h) = kbd_hook.take() {
                            let _ = UnhookWindowsHookEx(h);
                        }
                        if let Some(h) = mouse_hook.take() {
                            let _ = UnhookWindowsHookEx(h);
                        }
                        sync_hooks(&mut kbd_hook, &mut mouse_hook);
                    } else {
                        let _ = TranslateMessage(&msg);
                        let _ = DispatchMessageW(&msg);
                    }
                }

                if let Some(h) = kbd_hook.take() {
                    let _ = UnhookWindowsHookEx(h);
                }
                if let Some(h) = mouse_hook.take() {
                    let _ = UnhookWindowsHookEx(h);
                }
            })
            .ok()?;

        match rx.recv_timeout(std::time::Duration::from_secs(3)) {
            Ok(tid) if tid != 0 => Some(Self {
                thread_id: tid,
                join_handle: Some(handle),
            }),
            _ => {
                let _ = handle.join();
                None
            }
        }
    }

    fn signal_reconcile(&self) {
        unsafe {
            let _ = PostThreadMessageW(
                self.thread_id,
                WM_HOOK_THREAD_RECONCILE,
                WPARAM(0),
                LPARAM(0),
            );
        }
    }

    fn signal_reinstall(&self) {
        unsafe {
            let _ = PostThreadMessageW(
                self.thread_id,
                WM_HOOK_THREAD_REINSTALL,
                WPARAM(0),
                LPARAM(0),
            );
        }
    }

    fn shutdown(&mut self) {
        HOOK_WANT_KBD.store(false, Ordering::Release);
        HOOK_WANT_MOUSE.store(false, Ordering::Release);
        unsafe {
            let _ = PostThreadMessageW(self.thread_id, WM_QUIT, WPARAM(0), LPARAM(0));
        }
        if let Some(h) = self.join_handle.take() {
            let _ = h.join();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. 主线程运行期状态与 WinEvent 调度（单线程 STA 零锁设计）
// ─────────────────────────────────────────────────────────────────────────────

struct ManagedRuntime {
    control_hwnd: HWND,
    config_path: PathBuf,
    config_dir: PathBuf,
    parent_handle: Option<HANDLE>,
    config: WindowKitConfig,
    muted_flags: usize,
    gfx: Option<SharedGraphics>,
    hook_worker: Option<InputHookWorker>,
    kbd_hook_active: bool,
    mouse_hook_active: bool,
    winevent_movesize: Option<HWINEVENTHOOK>,
    winevent_focus: Option<HWINEVENTHOOK>,
    winevent_location: Option<HWINEVENTHOOK>,
    winevent_destroy: Option<HWINEVENTHOOK>,
    winevent_minimize: Option<HWINEVENTHOOK>,
    winevent_hide: Option<HWINEVENTHOOK>,
    winevent_cloaked: Option<HWINEVENTHOOK>,
    winevent_show: Option<HWINEVENTHOOK>,
    quick_switch: QuickSwitchManager,
    snap_zones: SnapZonesManager,
    layout_hub: LayoutHubManager,
    layout_editor: LayoutEditorManager,
    workspace_capture: WorkspaceCaptureManager,
    workspace_progress: WorkspaceProgressOverlay,
    always_on_top: AlwaysOnTopManager,
    opacity: OpacityManager,
    alt_drag: AltDragManager,
    focus_border: FocusBorderManager,
    multi_monitor: MultiMonitorManager,
    last_hook_summary: String,
    anim_timer_active: bool,
    border_reconcile_timer_active: bool,
    wts_session_registered: bool,
    last_watch_instant: Instant,
    last_watch_wall: SystemTime,
    shutting_down: bool,
}

thread_local! {
    static RUNTIME: RefCell<Option<ManagedRuntime>> = const { RefCell::new(None) };
}

impl ManagedRuntime {
    fn new(
        control_hwnd: HWND,
        config_path: PathBuf,
        parent_handle: Option<HANDLE>,
        config: WindowKitConfig,
    ) -> Self {
        let config_dir = config_path
            .parent()
            .map(Path::to_path_buf)
            .unwrap_or_else(|| PathBuf::from("."));
        let initial_muted = (if config.fullscreen_muted { MUTED_FLAG_FULLSCREEN } else { 0 })
            | (if config.remote_muted { MUTED_FLAG_REMOTE } else { 0 });
        HOOK_MUTED.store(initial_muted != 0, Ordering::Release);
        let gfx = SharedGraphics::new().ok();
        Self {
            control_hwnd,
            config_path,
            config_dir,
            parent_handle,
            config,
            muted_flags: initial_muted,
            gfx,
            hook_worker: None,
            kbd_hook_active: false,
            mouse_hook_active: false,
            winevent_movesize: None,
            winevent_focus: None,
            winevent_location: None,
            winevent_destroy: None,
            winevent_minimize: None,
            winevent_hide: None,
            winevent_cloaked: None,
            winevent_show: None,
            quick_switch: QuickSwitchManager::new(control_hwnd),
            snap_zones: SnapZonesManager::new(),
            layout_hub: LayoutHubManager::new(control_hwnd),
            layout_editor: LayoutEditorManager::new(control_hwnd),
            workspace_capture: WorkspaceCaptureManager::new(control_hwnd),
            workspace_progress: WorkspaceProgressOverlay::new(),
            always_on_top: AlwaysOnTopManager::new(),
            opacity: OpacityManager::new(),
            alt_drag: AltDragManager::new(),
            focus_border: FocusBorderManager::new(),
            multi_monitor: MultiMonitorManager::new(control_hwnd),
            last_hook_summary: String::new(),
            anim_timer_active: false,
            border_reconcile_timer_active: false,
            wts_session_registered: false,
            last_watch_instant: Instant::now(),
            last_watch_wall: SystemTime::now(),
            shutting_down: false,
        }
    }

    /// 按需惰性获取或重建 D2D/DWrite/WIC 共享工厂（冷路径可完整释放归还提交内存）
    fn ensure_gfx(&mut self) {
        if self.gfx.is_none() {
            self.gfx = SharedGraphics::new().ok();
        }
    }

    /// 按需同步 120ms 边框巡检定时器：仅在启用焦点边框或存在置顶窗口时运行，
    /// 两者皆无时立即 KillTimer 确保平时 0 唤醒、绝不反向重建已释放的 SharedGraphics！
    fn sync_border_reconcile_timer(&mut self) {
        if self.control_hwnd.0.is_null() {
            return;
        }
        let need_timer =
            self.config.features.focus_border || self.always_on_top.pinned_count() > 0;
        unsafe {
            if need_timer {
                if !self.border_reconcile_timer_active {
                    let _ = SetTimer(
                        self.control_hwnd,
                        TIMER_BORDER_RECONCILE,
                        BORDER_RECONCILE_INTERVAL_MS,
                        None,
                    );
                    self.border_reconcile_timer_active = true;
                }
            } else {
                let _ = KillTimer(self.control_hwnd, TIMER_BORDER_RECONCILE);
                self.border_reconcile_timer_active = false;
            }
        }
    }

    /// 锁屏解锁 / 睡眠唤醒 / 静音恢复 / 时钟跳变后的低级键盘与鼠标钩子健康自愈重挂
    fn reinstall_input_hooks(&mut self, reason: &str, trace_id: &str) {
        if self.shutting_down {
            return;
        }
        clear_hook_phys_mods();
        let want_k = HOOK_WANT_KBD.load(Ordering::Acquire);
        let want_m = HOOK_WANT_MOUSE.load(Ordering::Acquire);
        if !want_k && !want_m {
            return;
        }
        if let Some(worker) = self.hook_worker.as_ref() {
            worker.signal_reinstall();
        } else {
            self.hook_worker = InputHookWorker::spawn();
        }
        self.kbd_hook_active = want_k && self.hook_worker.is_some();
        self.mouse_hook_active = want_m && self.hook_worker.is_some();
        wk_info!(
            trace_id,
            "runtime",
            "self-healed low-level input hooks (reason={}, kbd={}, mouse={})",
            reason,
            self.kbd_hook_active,
            self.mouse_hook_active
        );
    }

    /// 统一按需驱动 60~120Hz 动效定时器：有活跃动画时开启，收敛瞬间立即 KillTimer 保持 0% CPU！
    fn ensure_animation_timer(&mut self) {
        let need = self.quick_switch.has_active_animation()
            || self.snap_zones.has_active_animation()
            || self.always_on_top.has_active_animation()
            || self.focus_border.has_active_animation();
        if need && !self.anim_timer_active && !self.control_hwnd.0.is_null() {
            unsafe {
                let _ = SetTimer(
                    self.control_hwnd,
                    TIMER_ANIMATION_FRAME,
                    ANIMATION_FRAME_INTERVAL_MS,
                    None,
                );
            }
            self.anim_timer_active = true;
        } else if !need && self.anim_timer_active && !self.control_hwnd.0.is_null() {
            unsafe {
                let _ = KillTimer(self.control_hwnd, TIMER_ANIMATION_FRAME);
            }
            self.anim_timer_active = false;
        }
    }

    fn is_muted(&self) -> bool {
        self.muted_flags != 0
    }

    fn schedule_cold_path_trim(&self) {
        if !self.control_hwnd.0.is_null() {
            unsafe {
                let _ = SetTimer(
                    self.control_hwnd,
                    TIMER_COLD_PATH_TRIM,
                    COLD_PATH_TRIM_DELAY_MS,
                    None,
                );
                let _ = SetTimer(
                    self.control_hwnd,
                    TIMER_SLINT_IDLE_PURGE,
                    SLINT_IDLE_PURGE_DELAY_MS,
                    None,
                );
            }
        }
    }

    /// 即将打开任意 Slint 窗口前立即取消冷态回收定时器，防止刚打开就被回收
    fn cancel_slint_idle_purge(&self) {
        if !self.control_hwnd.0.is_null() {
            unsafe {
                let _ = KillTimer(self.control_hwnd, TIMER_SLINT_IDLE_PURGE);
            }
        }
    }

    /// 根据当前启用特性精确对账低级钩子、WinEventHook 与定时巡检器（未启用则 0 挂钩）
    fn reconcile_hooks(&mut self, trace_id: &str) {
        let f = self.config.features.clone();

        let qs_global = f.quick_switch && self.config.quick_switch.replace_alt_tab;
        let qs_same_app = f.quick_switch && self.config.quick_switch.same_app_enabled;
        HOOK_QUICK_SWITCH_ENABLED.store(qs_global, Ordering::Release);
        HOOK_SAME_APP_SWITCH_ENABLED.store(qs_same_app, Ordering::Release);
        HOOK_ALT_DRAG_ENABLED.store(f.alt_drag, Ordering::Release);
        HOOK_ALT_DRAG_REQUIRE_CTRL.store(self.config.alt_drag.require_ctrl, Ordering::Release);
        HOOK_OPACITY_WHEEL_ENABLED.store(
            f.opacity && self.config.opacity.alt_wheel_enabled,
            Ordering::Release,
        );
        HOOK_SNAP_RCLICK_ENABLED.store(
            f.snap_zones && self.config.snap_zones.trigger_mode == SnapTriggerMode::RightClick,
            Ordering::Release,
        );

        let need_kbd = qs_global || qs_same_app;
        let need_mouse = f.alt_drag
            || (f.opacity && self.config.opacity.alt_wheel_enabled)
            || (f.snap_zones && self.config.snap_zones.trigger_mode == SnapTriggerMode::RightClick);
        let need_movesize = f.snap_zones;
        let need_borders = f.always_on_top || f.focus_border;
        let need_show_event = (f.snap_zones && self.config.snap_zones.remember_last_zone)
            || (f.multi_monitor && self.config.multi_monitor.center_new_windows);

        unsafe {
            // 1 & 2. 独立永不阻塞输入钩子线程按需装卸 WH_KEYBOARD_LL / WH_MOUSE_LL（零超时防摘钩）
            HOOK_WANT_KBD.store(need_kbd, Ordering::Release);
            HOOK_WANT_MOUSE.store(need_mouse, Ordering::Release);
            if need_kbd || need_mouse {
                if let Some(worker) = self.hook_worker.as_ref() {
                    worker.signal_reconcile();
                } else {
                    self.hook_worker = InputHookWorker::spawn();
                }
            } else if let Some(mut worker) = self.hook_worker.take() {
                worker.shutdown();
            }
            self.kbd_hook_active = need_kbd && self.hook_worker.is_some();
            self.mouse_hook_active = need_mouse && self.hook_worker.is_some();
            if !need_kbd {
                self.quick_switch.close_and_release(trace_id);
            }
            if !need_mouse {
                HOOK_ALT_DRAG_ACTIVE.store(0, Ordering::Release);
                self.alt_drag.end();
            }

            // 3. SnapZones 拖拽 MoveSize 事件钩子
            if need_movesize && self.winevent_movesize.is_none() {
                let h = SetWinEventHook(
                    EVENT_SYSTEM_MOVESIZESTART,
                    EVENT_SYSTEM_MOVESIZEEND,
                    HINSTANCE::default(),
                    Some(win_event_proc),
                    0,
                    0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
                );
                if !h.is_invalid() {
                    self.winevent_movesize = Some(h);
                }
            } else if !need_movesize {
                if let Some(h) = self.winevent_movesize.take() {
                    let _ = UnhookWinEvent(h);
                }
                HOOK_SNAP_DRAGGING.store(false, Ordering::Release);
                let _ = KillTimer(self.control_hwnd, TIMER_SNAP_DRAG_POLL);
                self.snap_zones.cancel_and_cleanup();
            }

            // 4. 置顶边框与焦点边框 WinEvent 跟踪钩子
            if need_borders {
                if self.winevent_focus.is_none() {
                    let h = SetWinEventHook(
                        EVENT_SYSTEM_FOREGROUND,
                        EVENT_SYSTEM_FOREGROUND,
                        HINSTANCE::default(),
                        Some(win_event_proc),
                        0,
                        0,
                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
                    );
                    if !h.is_invalid() {
                        self.winevent_focus = Some(h);
                    }
                }
                if self.winevent_location.is_none() {
                    let h = SetWinEventHook(
                        EVENT_OBJECT_LOCATIONCHANGE,
                        EVENT_OBJECT_LOCATIONCHANGE,
                        HINSTANCE::default(),
                        Some(win_event_proc),
                        0,
                        0,
                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
                    );
                    if !h.is_invalid() {
                        self.winevent_location = Some(h);
                    }
                }
                if self.winevent_destroy.is_none() {
                    let h = SetWinEventHook(
                        EVENT_OBJECT_DESTROY,
                        EVENT_OBJECT_DESTROY,
                        HINSTANCE::default(),
                        Some(win_event_proc),
                        0,
                        0,
                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
                    );
                    if !h.is_invalid() {
                        self.winevent_destroy = Some(h);
                    }
                }
                if self.winevent_minimize.is_none() {
                    let h = SetWinEventHook(
                        EVENT_SYSTEM_MINIMIZESTART,
                        EVENT_SYSTEM_MINIMIZEEND,
                        HINSTANCE::default(),
                        Some(win_event_proc),
                        0,
                        0,
                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
                    );
                    if !h.is_invalid() {
                        self.winevent_minimize = Some(h);
                    }
                }
                // 捕获微信/托盘常驻/UWP 窗口隐藏与虚拟桌面 Cloak，实现 0ms 瞬隐杜绝鬼影边框
                if self.winevent_hide.is_none() {
                    let h = SetWinEventHook(
                        EVENT_OBJECT_HIDE,
                        EVENT_OBJECT_HIDE,
                        HINSTANCE::default(),
                        Some(win_event_proc),
                        0,
                        0,
                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
                    );
                    if !h.is_invalid() {
                        self.winevent_hide = Some(h);
                    }
                }
                if self.winevent_cloaked.is_none() {
                    let h = SetWinEventHook(
                        EVENT_OBJECT_CLOAKED,
                        EVENT_OBJECT_CLOAKED,
                        HINSTANCE::default(),
                        Some(win_event_proc),
                        0,
                        0,
                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
                    );
                    if !h.is_invalid() {
                        self.winevent_cloaked = Some(h);
                    }
                }
                if self.winevent_movesize.is_none() {
                    let h = SetWinEventHook(
                        EVENT_SYSTEM_MOVESIZESTART,
                        EVENT_SYSTEM_MOVESIZEEND,
                        HINSTANCE::default(),
                        Some(win_event_proc),
                        0,
                        0,
                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
                    );
                    if !h.is_invalid() {
                        self.winevent_movesize = Some(h);
                    }
                }
            } else {
                for slot in [
                    &mut self.winevent_focus,
                    &mut self.winevent_location,
                    &mut self.winevent_destroy,
                    &mut self.winevent_minimize,
                    &mut self.winevent_hide,
                    &mut self.winevent_cloaked,
                ] {
                    if let Some(h) = slot.take() {
                        let _ = UnhookWinEvent(h);
                    }
                }
            }

            // 5. 新窗口自动吸附 (rememberLastZone) / 新窗口自动居中 (centerNewWindows) 按需挂接 EVENT_OBJECT_SHOW
            if need_show_event && self.winevent_show.is_none() {
                let h = SetWinEventHook(
                    EVENT_OBJECT_SHOW,
                    EVENT_OBJECT_SHOW,
                    HINSTANCE::default(),
                    Some(win_event_proc),
                    0,
                    0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
                );
                if !h.is_invalid() {
                    self.winevent_show = Some(h);
                }
            } else if !need_show_event {
                if let Some(h) = self.winevent_show.take() {
                    let _ = UnhookWinEvent(h);
                }
            }
            // 若需要清理 rememberLastZone/centerNewWindows 句柄缓存，确保 destroy 钩子在线
            if need_show_event && self.winevent_destroy.is_none() {
                let h = SetWinEventHook(
                    EVENT_OBJECT_DESTROY,
                    EVENT_OBJECT_DESTROY,
                    HINSTANCE::default(),
                    Some(win_event_proc),
                    0,
                    0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
                );
                if !h.is_invalid() {
                    self.winevent_destroy = Some(h);
                }
            }
        }

        if !f.focus_border {
            self.focus_border.destroy();
        } else {
            self.ensure_gfx();
            self.focus_border.refresh(
                &self.config,
                self.gfx.as_ref(),
                self.is_muted(),
                self.quick_switch.is_open()
                    || self.layout_editor.is_open()
                    || self.layout_hub.is_visible(),
            );
        }

        if !f.always_on_top {
            self.always_on_top.cleanup_overlays();
        } else if self.always_on_top.pinned_count() > 0 {
            self.ensure_gfx();
            self.always_on_top
                .sync_borders(&self.config, self.gfx.as_ref(), self.is_muted());
        }

        if !f.multi_monitor {
            self.multi_monitor.restore_all_tray(trace_id);
        }

        let summary = format!(
            "active={:?} kbd={} mouse={} movesize={} borders={} show={}",
            f.active_names(),
            self.kbd_hook_active,
            self.mouse_hook_active,
            self.winevent_movesize.is_some(),
            need_borders,
            self.winevent_show.is_some()
        );
        if summary != self.last_hook_summary {
            self.last_hook_summary = summary.clone();
            wk_info!(trace_id, "runtime", "hooks reconciled: {}", summary);
        } else {
            wk_debug!(trace_id, "runtime", "hooks unchanged: {}", summary);
        }
        self.sync_border_reconcile_timer();
        self.ensure_animation_timer();
    }

    fn reload_config(&mut self, trace_id: &str) {
        match WindowKitConfig::load_from_file(&self.config_path) {
            Ok(new_cfg) => {
                let log_dir = resolve_log_dir(&new_cfg, &self.config_path);
                configure_logger(
                    Some(&log_dir),
                    LogLevel::from_str_config(&new_cfg.log_level),
                );
                self.config = new_cfg;
                let synced_muted =
                    (if self.config.fullscreen_muted { MUTED_FLAG_FULLSCREEN } else { 0 })
                        | (if self.config.remote_muted { MUTED_FLAG_REMOTE } else { 0 });
                if synced_muted != self.muted_flags {
                    self.set_muted_flags(synced_muted, trace_id);
                }
                if !self.config.should_stay_resident() {
                    wk_info!(
                        trace_id,
                        "runtime",
                        "config disabled or 0 features active, initiating graceful shutdown"
                    );
                    self.graceful_shutdown(trace_id);
                    return;
                }
                self.reconcile_hooks(trace_id);
                self.quick_switch.refresh_theme_if_open(&self.config);
                self.layout_hub
                    .refresh_theme_if_open(&self.config, self.gfx.as_ref());
            }
            Err(err) => {
                wk_warn!(trace_id, "runtime", "reload config failed: {}", err);
            }
        }
    }

    fn set_muted_flags(&mut self, flags: usize, trace_id: &str) {
        let was_muted = self.is_muted();
        self.muted_flags = flags;
        let muted = self.is_muted();
        HOOK_MUTED.store(muted, Ordering::Release);
        clear_hook_phys_mods();
        if muted {
            HOOK_ALT_DRAG_ACTIVE.store(0, Ordering::Release);
            self.alt_drag.end();
            self.focus_border.hide();
            self.snap_zones.cancel_and_cleanup();
            HOOK_SNAP_DRAGGING.store(false, Ordering::Release);
            if self.quick_switch.is_open() {
                self.quick_switch.close_and_release(trace_id);
            }
        } else {
            if was_muted {
                self.reinstall_input_hooks("unmuted", trace_id);
            }
            if self.config.features.focus_border || self.always_on_top.pinned_count() > 0 {
                self.ensure_gfx();
                self.focus_border.refresh(
                    &self.config,
                    self.gfx.as_ref(),
                    false,
                    self.quick_switch.is_open(),
                );
                self.always_on_top
                    .sync_borders(&self.config, self.gfx.as_ref(), false);
            }
            self.sync_border_reconcile_timer();
        }
        let fs_muted = (flags & MUTED_FLAG_FULLSCREEN) != 0;
        let rd_muted = (flags & MUTED_FLAG_REMOTE) != 0;
        wk_debug!(
            trace_id,
            "runtime",
            "muted flags updated: 0x{:x} (fullscreen={} remote={})",
            flags,
            fs_muted,
            rd_muted
        );
    }

    fn execute_action(&mut self, action_id: usize, arg: isize, trace_id: &str) {
        // 可选显式目标 HWND（来自 lParam 或 window_kit_action.json 的 targetHwnd 字段），
        // 动作结束自动通过 RAII Guard 归零，日常快捷键路径无负载直通 GetForegroundWindow。
        let mut explicit_target: Option<HWND> = None;
        if matches!(
            action_id,
            ACTION_TOGGLE_ALWAYS_ON_TOP
                | ACTION_OPACITY_UP
                | ACTION_OPACITY_DOWN
                | ACTION_OPACITY_RESET
                | ACTION_RESTORE_FROM_ZONE
                | ACTION_MOVE_NEXT_MONITOR
                | ACTION_MINIMIZE_TO_TRAY
        ) && arg > 0x100
        {
            explicit_target = Some(HWND(arg as *mut c_void));
        } else if matches!(
            action_id,
            ACTION_TOGGLE_ALWAYS_ON_TOP
                | ACTION_OPACITY_UP
                | ACTION_OPACITY_DOWN
                | ACTION_OPACITY_RESET
                | ACTION_MOVE_NEXT_ZONE
                | ACTION_MOVE_PREV_ZONE
                | ACTION_MOVE_ZONE_INDEX
                | ACTION_RESTORE_FROM_ZONE
                | ACTION_MOVE_NEXT_MONITOR
                | ACTION_MINIMIZE_TO_TRAY
        ) {
            let payload_path = self.config_dir.join(FILE_ACTION_PAYLOAD);
            if let Ok(raw) = std::fs::read_to_string(&payload_path) {
                if let Ok(val) = parse_json(&raw) {
                    if let Some(hwnd_num) = val.get("targetHwnd").and_then(|v| v.as_f64()) {
                        let raw_hwnd = hwnd_num as isize;
                        if raw_hwnd > 0x100 {
                            explicit_target = Some(HWND(raw_hwnd as *mut c_void));
                        }
                    }
                }
            }
        }
        let _target_guard = ActionTargetOverrideGuard::enter(explicit_target);

        wk_debug!(
            trace_id,
            "action",
            "dispatch action={} arg={} schemaVersion={}",
            action_id,
            arg,
            self.config.schema_version
        );

        // 兼容宿主 WindowKitController (schemaVersion == 1) 的 1..16 动作码表
        if self.config.schema_version == 1 {
            match action_id {
                7 => {
                    let _ = self
                        .multi_monitor
                        .move_foreground_to_monitor(1, &self.config, trace_id);
                    return;
                }
                8 => {
                    let _ = self
                        .multi_monitor
                        .move_foreground_to_monitor(-1, &self.config, trace_id);
                    return;
                }
                10 => {
                    let _ = self
                        .multi_monitor
                        .minimize_foreground_to_tray(&self.config, trace_id);
                    return;
                }
                11 => {
                    let _ = self.multi_monitor.restore_all_tray(trace_id);
                    return;
                }
                12 => {
                    if !self.quick_switch.is_open() {
                        self.cancel_slint_idle_purge();
                    }
                    let tray_list = self.multi_monitor.list_tray_windows();
                    let closed = self.quick_switch.toggle_programmatic(
                        &tray_list,
                        &self.config,
                        self.gfx.as_ref(),
                        trace_id,
                    );
                    if closed {
                        self.schedule_cold_path_trim();
                    }
                    return;
                }
                13 => {
                    if arg >= 0 {
                        if let Some(profile) =
                            self.config.workspaces.items.get(arg as usize).cloned()
                        {
                            launch_workspace_async(
                                profile,
                                self.config.clone(),
                                self.control_hwnd,
                                trace_id.to_string(),
                            );
                        }
                    }
                    return;
                }
                14 => {
                    let duration = if arg > 0 { arg } else { 2000 };
                    self.dispatch_zones_preview(duration, trace_id);
                    return;
                }
                15 => {
                    if arg >= 0 {
                        self.snap_zones.move_foreground_to_zone_index(
                            arg as usize,
                            &self.config,
                            trace_id,
                        );
                    }
                    return;
                }
                16 => {
                    if !self.quick_switch.is_open()
                        && !self.snap_zones.is_dragging()
                        && !self.alt_drag.is_active()
                    {
                        trim_working_set_cold_path();
                    }
                    return;
                }
                _ => {}
            }
        }

        match action_id {
            ACTION_TOGGLE_ALWAYS_ON_TOP => {
                self.ensure_gfx();
                self.always_on_top
                    .toggle_foreground(&self.config, self.gfx.as_ref(), trace_id);
                self.sync_border_reconcile_timer();
                self.ensure_animation_timer();
                self.schedule_cold_path_trim();
            }
            ACTION_OPACITY_UP => {
                self.opacity.adjust_foreground(1, &self.config, trace_id);
            }
            ACTION_OPACITY_DOWN => {
                self.opacity.adjust_foreground(-1, &self.config, trace_id);
            }
            ACTION_OPACITY_RESET => {
                self.opacity.reset_foreground(&self.config, trace_id);
            }
            ACTION_MOVE_NEXT_ZONE => {
                self.snap_zones
                    .move_foreground_relative(1, &self.config, trace_id);
            }
            ACTION_MOVE_PREV_ZONE => {
                self.snap_zones
                    .move_foreground_relative(-1, &self.config, trace_id);
            }
            ACTION_MOVE_ZONE_INDEX => {
                if arg >= 0 {
                    self.snap_zones.move_foreground_to_zone_index(
                        arg as usize,
                        &self.config,
                        trace_id,
                    );
                }
            }
            ACTION_RESTORE_FROM_ZONE => {
                self.snap_zones
                    .restore_foreground_from_zone(&self.config, trace_id);
            }
            ACTION_RESCUE_OFFSCREEN_WINDOWS => {
                let _ = self
                    .multi_monitor
                    .rescue_offscreen_windows(&self.config, trace_id);
            }
            ACTION_MOVE_NEXT_MONITOR => {
                let _ = self
                    .multi_monitor
                    .move_foreground_to_monitor(1, &self.config, trace_id);
            }
            ACTION_MINIMIZE_TO_TRAY => {
                let _ = self
                    .multi_monitor
                    .minimize_foreground_to_tray(&self.config, trace_id);
            }
            ACTION_RESTORE_ALL_TRAY => {
                let _ = self.multi_monitor.restore_all_tray(trace_id);
            }
            ACTION_OPEN_QUICK_SWITCH => {
                if !self.quick_switch.is_open() {
                    self.cancel_slint_idle_purge();
                    self.ensure_gfx();
                }
                let tray_list = self.multi_monitor.list_tray_windows();
                let closed = self.quick_switch.toggle_programmatic(
                    &tray_list,
                    &self.config,
                    self.gfx.as_ref(),
                    trace_id,
                );
                if closed {
                    self.schedule_cold_path_trim();
                }
                self.ensure_animation_timer();
            }
            ACTION_CAPTURE_WORKSPACE => {
                let json = capture_workspace_v2_json(&self.config, None);
                let out_path = self.config_dir.join(FILE_CAPTURE_WORKSPACE);
                if let Err(e) = write_text_file_atomic(&out_path, &json.to_string_pretty()) {
                    wk_error!(trace_id, "workspaces", "capture write failed: {}", e);
                } else {
                    wk_info!(
                        trace_id,
                        "workspaces",
                        "captured workspace v2 to {}",
                        out_path.display()
                    );
                }
            }
            ACTION_LAUNCH_WORKSPACE => {
                let payload_path = self.config_dir.join(FILE_ACTION_PAYLOAD);
                if let Ok(raw) = std::fs::read_to_string(&payload_path) {
                    if let Ok(val) = parse_json(&raw) {
                        let ws_id = val.get_str("workspaceId", "").trim().to_string();
                        if let Some(profile) = self
                            .config
                            .workspaces
                            .items
                            .iter()
                            .find(|w| w.id == ws_id)
                            .cloned()
                        {
                            launch_workspace_async(
                                profile,
                                self.config.clone(),
                                self.control_hwnd,
                                trace_id.to_string(),
                            );
                        } else {
                            wk_warn!(
                                trace_id,
                                "workspaces",
                                "workspaceId '{}' not found in config",
                                ws_id
                            );
                        }
                    }
                }
            }
            ACTION_IMPORT_POWERTOYS_ZONES => {
                let out_path = self.config_dir.join(FILE_IMPORT_POWERTOYS);
                if let Err(e) = run_import_powertoys(&out_path, None) {
                    wk_warn!(trace_id, "snapZones", "PowerToys import failed: {}", e);
                } else {
                    wk_info!(
                        trace_id,
                        "snapZones",
                        "imported PowerToys zones to {}",
                        out_path.display()
                    );
                }
            }
            ACTION_QUERY_MONITORS => {
                let monitors = enumerate_monitors();
                let json = monitors_to_json(&monitors);
                let out_path = self.config_dir.join(FILE_QUERY_MONITORS);
                if let Err(e) = write_text_file_atomic(&out_path, &json.to_string_pretty()) {
                    wk_error!(trace_id, "monitors", "query monitors write failed: {}", e);
                }
            }
            ACTION_QUERY_RUNTIME_STATS => {
                let active = self.config.features.active_names();
                let json = query_runtime_stats_json(
                    self.always_on_top.pinned_count(),
                    self.multi_monitor.tray_minimized_count(),
                    &active,
                );
                let out_path = self.config_dir.join(FILE_RUNTIME_STATS);
                if let Err(e) = write_text_file_atomic(&out_path, &json.to_string_pretty()) {
                    wk_error!(trace_id, "stats", "runtime stats write failed: {}", e);
                }
            }
            ACTION_SHOW_ZONES_PREVIEW => {
                self.dispatch_zones_preview(arg, trace_id);
            }
            ACTION_OPEN_LAYOUT_EDITOR => {
                // 打开双层共视布局管理器（或按 directEditor / layoutId 直入 1:1 全屏编辑区域）
                self.snap_zones.hide_preview_overlay();
                self.focus_border.hide();
                let mut monitor_id: Option<String> = None;
                let mut desktop_id: Option<String> = None;
                let mut layout_id: Option<String> = None;
                let mut direct_editor = false;
                let mut restore_settings_hwnd = if arg > 0x100 { Some(arg) } else { None };
                let mut ui_lang = "zh".to_string();

                let payload_path = self.config_dir.join(FILE_ACTION_PAYLOAD);
                if let Ok(raw) = std::fs::read_to_string(&payload_path) {
                    if let Ok(val) = parse_json(&raw) {
                        let mid = val.get_str("monitorId", "").trim().to_string();
                        if !mid.is_empty() {
                            monitor_id = Some(mid);
                        }
                        let did = val.get_str("desktopId", "").trim().to_string();
                        if !did.is_empty() {
                            desktop_id = Some(did);
                        }
                        let lid = val.get_str("layoutId", "").trim().to_string();
                        if !lid.is_empty() {
                            layout_id = Some(lid);
                        }
                        direct_editor = val.get_bool("directEditor", false);
                        if let Some(rh) = val.get("restoreSettingsHwnd").and_then(|v| v.as_f64()) {
                            let raw_h = rh as isize;
                            if raw_h > 0x100 {
                                restore_settings_hwnd = Some(raw_h);
                            }
                        }
                        let lang = val.get_str("uiLang", "").trim().to_string();
                        if !lang.is_empty() {
                            ui_lang = lang;
                        }
                    }
                }
                self.ensure_gfx();
                if direct_editor || layout_id.is_some() {
                    let req = OpenEditorRequest {
                        monitor_id,
                        desktop_id,
                        layout_id,
                        restore_settings_hwnd,
                        ui_lang,
                        return_to_hub: false,
                        initial_mode: None,
                        custom_name: None,
                        is_new_custom: false,
                    };
                    let _ = self
                        .layout_editor
                        .open(req, &self.config, self.gfx.as_ref(), trace_id);
                } else {
                    self.cancel_slint_idle_purge();
                    let req = OpenHubRequest {
                        monitor_id,
                        desktop_id,
                        restore_settings_hwnd,
                        ui_lang,
                    };
                    let _ = self
                        .layout_hub
                        .open(req, &self.config, self.gfx.as_ref(), trace_id);
                }
            }
            ACTION_APPLY_LAYOUT_HOTKEY_DIGIT => {
                if (0..=9).contains(&arg) {
                    if self
                        .snap_zones
                        .apply_layout_by_hotkey_digit(arg as u8, &mut self.config, trace_id)
                        .is_some()
                    {
                        notify_host_hub_sync(&self.config, &self.config_dir, true, false, trace_id);
                        self.dispatch_zones_preview(1200, trace_id);
                    }
                }
            }
            ACTION_START_WORKSPACE_CAPTURE => {
                self.snap_zones.hide_preview_overlay();
                self.focus_border.hide();
                let mut req = StartCaptureRequest {
                    workspace_id: None,
                    restore_settings_hwnd: if arg > 0x100 { Some(arg) } else { None },
                    ui_lang: "zh".to_string(),
                };
                let payload_path = self.config_dir.join(FILE_ACTION_PAYLOAD);
                if let Ok(raw) = std::fs::read_to_string(&payload_path) {
                    if let Ok(val) = parse_json(&raw) {
                        let wid = val.get_str("workspaceId", "").trim().to_string();
                        if !wid.is_empty() {
                            req.workspace_id = Some(wid);
                        }
                        if let Some(rh) = val.get("restoreSettingsHwnd").and_then(|v| v.as_f64()) {
                            let raw_h = rh as isize;
                            if raw_h > 0x100 {
                                req.restore_settings_hwnd = Some(raw_h);
                            }
                        }
                        let lang = val.get_str("uiLang", "").trim().to_string();
                        if !lang.is_empty() {
                            req.ui_lang = lang;
                        }
                    }
                }
                self.cancel_slint_idle_purge();
                self.ensure_gfx();
                let _ = self
                    .workspace_capture
                    .start(req, &self.config, self.gfx.as_ref(), trace_id);
            }
            ACTION_PREVIEW_BORDER_STYLE => {
                // arg == 1 预览置顶边框；其余预览焦点边框；可通过 action payload 传入实时未保存颜色/粗细
                let mut color_hex = if arg == 1 {
                    self.config.always_on_top.border_color.clone()
                } else {
                    self.config.focus_border.border_color.clone()
                };
                let mut width_px = if arg == 1 {
                    self.config.always_on_top.border_width
                } else {
                    self.config.focus_border.border_width
                };
                let mut radius = self.config.focus_border.corner_radius;
                let mut opacity = if arg == 1 {
                    0.95
                } else {
                    self.config.focus_border.opacity
                };
                let payload_path = self.config_dir.join(FILE_ACTION_PAYLOAD);
                if let Ok(raw) = std::fs::read_to_string(&payload_path) {
                    if let Ok(val) = parse_json(&raw) {
                        let c = val.get_str("borderColor", "").trim().to_string();
                        if !c.is_empty() {
                            color_hex = c;
                        }
                        let w_val = val.get_i32("borderWidth", -1);
                        if w_val > 0 {
                            width_px = w_val;
                        }
                        let r_val = val.get_i32("cornerRadius", -1);
                        if r_val >= 0 {
                            radius = r_val;
                        }
                        if let Some(op) = val.get("opacity").and_then(|v| v.as_f64()) {
                            opacity = op as f32;
                        }
                    }
                }
                self.ensure_gfx();
                if self.focus_border.preview_style_on_foreground(
                    &color_hex,
                    width_px,
                    radius,
                    opacity,
                    &self.config,
                    self.gfx.as_ref(),
                ) {
                    unsafe {
                        let _ = SetTimer(
                            self.control_hwnd,
                            TIMER_BORDER_PREVIEW_HIDE,
                            BORDER_STYLE_PREVIEW_MS,
                            None,
                        );
                    }
                }
            }
            ACTION_OPEN_LAYOUT_EDITOR_DIRECT => {
                self.snap_zones.hide_preview_overlay();
                self.focus_border.hide();
                let req = OpenEditorRequest {
                    monitor_id: None,
                    desktop_id: None,
                    layout_id: None,
                    restore_settings_hwnd: if arg > 0x100 { Some(arg) } else { None },
                    ui_lang: "zh".to_string(),
                    return_to_hub: false,
                    initial_mode: None,
                    custom_name: None,
                    is_new_custom: false,
                };
                self.ensure_gfx();
                let _ = self
                    .layout_editor
                    .open(req, &self.config, self.gfx.as_ref(), trace_id);
            }
            ACTION_APPLY_LAYOUT_BY_ID => {
                let payload_path = self.config_dir.join(FILE_ACTION_PAYLOAD);
                let mut layout_id = String::new();
                if let Ok(raw) = std::fs::read_to_string(&payload_path) {
                    if let Ok(val) = parse_json(&raw) {
                        layout_id = val.get_str("layoutId", "").trim().to_string();
                    }
                }
                if !layout_id.is_empty()
                    && self
                        .snap_zones
                        .apply_layout_by_id(&layout_id, &mut self.config, trace_id)
                        .is_some()
                {
                    notify_host_hub_sync(&self.config, &self.config_dir, true, false, trace_id);
                    self.dispatch_zones_preview(1200, trace_id);
                } else {
                    // 未指定或目标布局已被删除时回退打开布局管理器，避免手势静默无反馈
                    self.cancel_slint_idle_purge();
                    let req = OpenHubRequest {
                        monitor_id: None,
                        desktop_id: None,
                        restore_settings_hwnd: None,
                        ui_lang: "zh".to_string(),
                    };
                    self.ensure_gfx();
                    let _ = self
                        .layout_hub
                        .open(req, &self.config, self.gfx.as_ref(), trace_id);
                }
            }
            _ => {
                wk_warn!(trace_id, "action", "unknown action id={}", action_id);
            }
        }
    }

    fn dispatch_zones_preview(&mut self, duration_ms: isize, trace_id: &str) {
        unsafe {
            let _ = KillTimer(self.control_hwnd, TIMER_ZONES_PREVIEW_HIDE);
        }
        if duration_ms <= 0 {
            self.snap_zones.hide_preview_overlay();
            self.schedule_cold_path_trim();
        } else {
            let duration = (duration_ms as u32).clamp(150, 30_000);
            self.ensure_gfx();
            if self.snap_zones.show_preview_overlay(
                &self.config,
                self.gfx.as_ref(),
                true,
                trace_id,
            ) {
                unsafe {
                    let _ = SetTimer(
                        self.control_hwnd,
                        TIMER_ZONES_PREVIEW_HIDE,
                        duration,
                        None,
                    );
                }
                self.ensure_animation_timer();
            }
        }
    }

    fn handle_win_event(&mut self, event: u32, hwnd: HWND) {
        if self.shutting_down {
            return;
        }
        match event {
            EVENT_SYSTEM_MOVESIZESTART => {
                if self.always_on_top.pinned_count() > 0 {
                    self.always_on_top.on_move_size_start(hwnd);
                }
                if self.config.features.focus_border {
                    self.focus_border.on_move_size_start(hwnd);
                }
                self.ensure_animation_timer();
                if self
                    .snap_zones
                    .on_move_size_start(hwnd, &self.config, self.is_muted())
                {
                    HOOK_SNAP_DRAGGING.store(true, Ordering::Release);
                    unsafe {
                        let _ = SetTimer(
                            self.control_hwnd,
                            TIMER_SNAP_DRAG_POLL,
                            SNAP_DRAG_POLL_INTERVAL_MS,
                            None,
                        );
                    }
                }
            }
            EVENT_SYSTEM_MOVESIZEEND => {
                self.always_on_top.on_move_size_end(hwnd);
                self.focus_border.on_move_size_end();
                HOOK_SNAP_DRAGGING.store(false, Ordering::Release);
                unsafe {
                    let _ = KillTimer(self.control_hwnd, TIMER_SNAP_DRAG_POLL);
                }
                let trace_id = next_trace_id("snap");
                if self
                    .snap_zones
                    .on_move_size_end(hwnd, &self.config, &trace_id)
                {
                    self.schedule_cold_path_trim();
                }
                if self.always_on_top.pinned_count() > 0 || self.config.features.focus_border {
                    self.ensure_gfx();
                }
                self.always_on_top
                    .sync_borders(&self.config, self.gfx.as_ref(), self.is_muted());
                self.focus_border.refresh(
                    &self.config,
                    self.gfx.as_ref(),
                    self.is_muted(),
                    self.quick_switch.is_open()
                        || self.layout_editor.is_open()
                        || self.layout_hub.is_visible(),
                );
                self.sync_border_reconcile_timer();
                self.ensure_animation_timer();
            }
            EVENT_OBJECT_SHOW => {
                if !self.layout_editor.is_open() {
                    let trace_id = next_trace_id("show");
                    let snapped = self.snap_zones.on_window_shown(
                        hwnd,
                        &self.config,
                        self.is_muted(),
                        &trace_id,
                    );
                    if !snapped {
                        let _ = self.multi_monitor.maybe_center_new_window(
                            hwnd,
                            &self.config,
                            self.is_muted(),
                            &trace_id,
                        );
                    }
                }
            }
            EVENT_SYSTEM_FOREGROUND
            | EVENT_OBJECT_LOCATIONCHANGE
            | EVENT_OBJECT_DESTROY
            | EVENT_OBJECT_HIDE
            | EVENT_OBJECT_CLOAKED
            | EVENT_SYSTEM_MINIMIZESTART
            | EVENT_SYSTEM_MINIMIZEEND => {
                if event == EVENT_OBJECT_DESTROY {
                    self.snap_zones.on_window_destroyed(hwnd);
                    self.multi_monitor.on_window_destroyed(hwnd);
                }
                // 目标窗口一旦隐藏、最小化、被 Cloak 或销毁，0ms 瞬隐焦点边框（杜绝微信等托盘窗口残影）
                if matches!(
                    event,
                    EVENT_SYSTEM_MINIMIZESTART
                        | EVENT_OBJECT_HIDE
                        | EVENT_OBJECT_CLOAKED
                        | EVENT_OBJECT_DESTROY
                ) {
                    self.focus_border.on_target_vanished(hwnd);
                }
                // LOCATIONCHANGE 精准过滤：跳过 99% 无关后台窗口噪音，仅跟踪当前焦点窗或置顶窗
                if event == EVENT_OBJECT_LOCATIONCHANGE {
                    let raw = hwnd.0 as isize;
                    let is_focus_win = raw == self.focus_border.current_target_raw()
                        || hwnd == unsafe { GetForegroundWindow() };
                    if !is_focus_win && self.always_on_top.pinned_count() == 0 {
                        return;
                    }
                }
                if self.always_on_top.pinned_count() > 0 || self.config.features.focus_border {
                    self.ensure_gfx();
                }
                self.always_on_top
                    .sync_borders(&self.config, self.gfx.as_ref(), self.is_muted());
                self.focus_border.refresh(
                    &self.config,
                    self.gfx.as_ref(),
                    self.is_muted(),
                    self.quick_switch.is_open()
                        || self.layout_editor.is_open()
                        || self.layout_hub.is_visible(),
                );
                self.sync_border_reconcile_timer();
                self.ensure_animation_timer();
            }
            _ => {}
        }
    }

    fn check_parent_alive(&mut self) {
        if let Some(handle) = self.parent_handle {
            let status = unsafe { WaitForSingleObject(handle, 0) };
            if status == WAIT_OBJECT_0 {
                let trace_id = next_trace_id("parent");
                wk_info!(
                    &trace_id,
                    "watchdog",
                    "parent process exited, shutting down cleanly"
                );
                self.graceful_shutdown(&trace_id);
            }
        }
    }

    fn handle_topology_changed(&mut self, trace_id: &str) {
        if self.shutting_down {
            return;
        }
        wk_debug!(trace_id, "topology", "display or virtual desktop topology changed, healing active layers");
        self.snap_zones.on_topology_changed(&self.config, self.gfx.as_ref());
        self.layout_hub.on_topology_changed(&self.config, self.gfx.as_ref());
        self.layout_editor.on_topology_changed(self.gfx.as_ref());
        self.workspace_capture.on_topology_changed(&self.config, self.gfx.as_ref());
        self.quick_switch.on_topology_changed(&self.config, self.gfx.as_ref());
        self.always_on_top
            .sync_borders(&self.config, self.gfx.as_ref(), self.is_muted());
        self.focus_border.refresh(
            &self.config,
            self.gfx.as_ref(),
            self.is_muted(),
            self.quick_switch.is_open()
                || self.layout_editor.is_open()
                || self.layout_hub.is_visible(),
        );
        self.ensure_animation_timer();
    }

    fn graceful_shutdown(&mut self, trace_id: &str) {
        if self.shutting_down {
            return;
        }
        self.shutting_down = true;
        HOOK_CONTROL_HWND.store(0, Ordering::Release);
        HOOK_ALT_DRAG_ACTIVE.store(0, Ordering::Release);
        HOOK_SNAP_DRAGGING.store(false, Ordering::Release);

        unsafe {
            for timer_id in [
                TIMER_PARENT_WATCH,
                TIMER_COLD_PATH_TRIM,
                TIMER_SLINT_IDLE_PURGE,
                TIMER_SNAP_DRAG_POLL,
                TIMER_ZONES_PREVIEW_HIDE,
                TIMER_BORDER_RECONCILE,
                TIMER_ANIMATION_FRAME,
                TIMER_DISPLAY_CHANGE_DEBOUNCE,
                TIMER_BORDER_PREVIEW_HIDE,
                TIMER_WORKSPACE_PROGRESS_HIDE,
            ] {
                let _ = KillTimer(self.control_hwnd, timer_id);
            }
            self.anim_timer_active = false;
            self.border_reconcile_timer_active = false;
            if self.wts_session_registered && !self.control_hwnd.0.is_null() {
                let _ = WTSUnRegisterSessionNotification(self.control_hwnd);
                self.wts_session_registered = false;
            }
            if let Some(mut worker) = self.hook_worker.take() {
                worker.shutdown();
            }
            self.kbd_hook_active = false;
            self.mouse_hook_active = false;
            for slot in [
                &mut self.winevent_movesize,
                &mut self.winevent_focus,
                &mut self.winevent_location,
                &mut self.winevent_destroy,
                &mut self.winevent_minimize,
                &mut self.winevent_hide,
                &mut self.winevent_cloaked,
                &mut self.winevent_show,
            ] {
                if let Some(h) = slot.take() {
                    let _ = UnhookWinEvent(h);
                }
            }
            if let Some(h) = self.parent_handle.take() {
                let _ = CloseHandle(h);
            }
        }

        // 彻底恢复全部托盘最小化窗口、透明度与瞬态覆盖层
        self.multi_monitor.restore_all_tray(trace_id);
        emergency_restore_all_tray_windows();
        self.opacity.restore_all();
        self.quick_switch.destroy();
        self.layout_editor.close_discard(trace_id);
        let _ = self.layout_hub.close(trace_id);
        self.workspace_capture.cancel_and_close(trace_id);
        self.workspace_progress.hide_and_destroy();
        self.snap_zones.cancel_and_cleanup();
        self.always_on_top.cleanup_overlays();
        self.focus_border.destroy();
        self.alt_drag.end();
        crate::ui_slint::shutdown_slint_runtime();

        wk_info!(trace_id, "runtime", "graceful shutdown complete");
        flush_logger();

        unsafe {
            if !self.control_hwnd.0.is_null() {
                let _ = DestroyWindow(self.control_hwnd);
            }
            PostQuitMessage(0);
        }
    }
}

unsafe extern "system" fn win_event_proc(
    _hook: HWINEVENTHOOK,
    event: u32,
    hwnd: HWND,
    id_object: i32,
    id_child: i32,
    _event_thread: u32,
    _event_time: u32,
) {
    if id_object != OBJID_WINDOW.0 || id_child != 0 || hwnd.0.is_null() {
        return;
    }
    RUNTIME.with(|cell| {
        if let Ok(mut borrow) = cell.try_borrow_mut() {
            if let Some(rt) = borrow.as_mut() {
                rt.handle_win_event(event, hwnd);
            }
        }
    });
}

unsafe extern "system" fn control_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    let handled = RUNTIME.with(|cell| {
        let Ok(mut borrow) = cell.try_borrow_mut() else {
            return false;
        };
        let Some(rt) = borrow.as_mut() else {
            return false;
        };

        match msg {
            WM_WK_RELOAD_CONFIG => {
                let trace = next_trace_id("reload");
                rt.reload_config(&trace);
                true
            }
            WM_WK_EXECUTE_ACTION => {
                let trace = next_trace_id("act");
                rt.execute_action(wparam.0, lparam.0, &trace);
                true
            }
            WM_WK_SET_MUTED_FLAGS => {
                let trace = next_trace_id("mute");
                rt.set_muted_flags(wparam.0, &trace);
                true
            }
            WM_WK_GRACEFUL_SHUTDOWN | WM_CLOSE | WM_ENDSESSION => {
                let trace = next_trace_id("exit");
                rt.graceful_shutdown(&trace);
                true
            }
            WM_QUERYENDSESSION => true,
            WM_WK_INTERNAL_ALT_TAB => {
                let trace = next_trace_id("qs");
                let reverse = wparam.0 != 0;
                let same_app = lparam.0 != 0;
                rt.cancel_slint_idle_purge();
                rt.ensure_gfx();
                if same_app {
                    rt.quick_switch
                        .on_same_app_step(reverse, &rt.config, rt.gfx.as_ref(), &trace);
                } else {
                    let tray_list = rt.multi_monitor.list_tray_windows();
                    rt.quick_switch.on_alt_tab_step(
                        reverse,
                        &tray_list,
                        &rt.config,
                        rt.gfx.as_ref(),
                        &trace,
                    );
                }
                rt.ensure_animation_timer();
                true
            }
            WM_WK_INTERNAL_ALT_RELEASED => {
                let trace = next_trace_id("qs");
                match rt.quick_switch.on_alt_released(&trace) {
                    QuickSwitchDelegatedAction::Closed => {
                        HOOK_QS_ALT_SESSION_ACTIVE.store(false, Ordering::Release);
                        rt.schedule_cold_path_trim();
                    }
                    QuickSwitchDelegatedAction::RestoreTrayWindowAndClose(raw) => {
                        HOOK_QS_ALT_SESSION_ACTIVE.store(false, Ordering::Release);
                        let _ = rt
                            .multi_monitor
                            .restore_by_hwnd(HWND(raw as *mut c_void), &trace);
                        rt.schedule_cold_path_trim();
                    }
                    _ => {}
                }
                rt.ensure_animation_timer();
                true
            }
            WM_WK_INTERNAL_QS_KEY => {
                let trace = next_trace_id("qs");
                let vk = wparam.0 as u16;
                let mod_mask = lparam.0 as u32;
                match rt
                    .quick_switch
                    .handle_hook_key(vk, mod_mask, &rt.config, &trace)
                {
                    QuickSwitchDelegatedAction::Closed => {
                        HOOK_QS_ALT_SESSION_ACTIVE.store(false, Ordering::Release);
                        rt.schedule_cold_path_trim();
                    }
                    QuickSwitchDelegatedAction::RestoreTrayWindowAndClose(raw) => {
                        HOOK_QS_ALT_SESSION_ACTIVE.store(false, Ordering::Release);
                        let _ = rt
                            .multi_monitor
                            .restore_by_hwnd(HWND(raw as *mut c_void), &trace);
                        rt.schedule_cold_path_trim();
                    }
                    _ => {}
                }
                rt.ensure_animation_timer();
                true
            }
            WM_WK_INTERNAL_SLINT_EVENT => {
                for ev in crate::ui_slint::drain_slint_events() {
                    match &ev {
                        crate::ui_slint::SlintUiEvent::QsSelectRow(_)
                        | crate::ui_slint::SlintUiEvent::QsActivateRow(_)
                        | crate::ui_slint::SlintUiEvent::QsCloseRow(_)
                        | crate::ui_slint::SlintUiEvent::QsFilterChanged(_)
                        | crate::ui_slint::SlintUiEvent::QsClearFilter
                        | crate::ui_slint::SlintUiEvent::QsSearchEngaged
                        | crate::ui_slint::SlintUiEvent::QsAction(_)
                        | crate::ui_slint::SlintUiEvent::QsKeyCommand { .. }
                        | crate::ui_slint::SlintUiEvent::QsDismissed
                        | crate::ui_slint::SlintUiEvent::QsBoundsChanged { .. } => {
                            let trace = next_trace_id("qs");
                            let action = rt.quick_switch.handle_slint_event(
                                &ev,
                                &mut rt.config,
                                &rt.config_dir,
                                &trace,
                            );
                            match action {
                                QuickSwitchDelegatedAction::None => {}
                                QuickSwitchDelegatedAction::Closed => {
                                    HOOK_QS_ALT_SESSION_ACTIVE.store(false, Ordering::Release);
                                    rt.schedule_cold_path_trim();
                                }
                                QuickSwitchDelegatedAction::TogglePinWindow(raw) => {
                                    let _guard = ActionTargetOverrideGuard::enter(Some(HWND(
                                        raw as *mut c_void,
                                    )));
                                    rt.ensure_gfx();
                                    rt.always_on_top.toggle_foreground(
                                        &rt.config,
                                        rt.gfx.as_ref(),
                                        &trace,
                                    );
                                    rt.sync_border_reconcile_timer();
                                    rt.quick_switch.refresh_theme_if_open(&rt.config);
                                }
                                QuickSwitchDelegatedAction::MoveWindowToNextZone(raw) => {
                                    let _guard = ActionTargetOverrideGuard::enter(Some(HWND(
                                        raw as *mut c_void,
                                    )));
                                    let _ = rt.snap_zones.move_foreground_relative(
                                        1,
                                        &rt.config,
                                        &trace,
                                    );
                                }
                                QuickSwitchDelegatedAction::MinimizeWindowToTray(raw) => {
                                    let _ = rt.multi_monitor.minimize_hwnd_to_tray(
                                        HWND(raw as *mut c_void),
                                        &rt.config,
                                        &trace,
                                    );
                                }
                                QuickSwitchDelegatedAction::RestoreTrayWindowAndClose(raw) => {
                                    HOOK_QS_ALT_SESSION_ACTIVE.store(false, Ordering::Release);
                                    let _ = rt
                                        .multi_monitor
                                        .restore_by_hwnd(HWND(raw as *mut c_void), &trace);
                                    rt.schedule_cold_path_trim();
                                }
                            }
                            rt.ensure_animation_timer();
                        }
                        crate::ui_slint::SlintUiEvent::HubSelectMonitor(_)
                        | crate::ui_slint::SlintUiEvent::HubSelectDesktop(_)
                        | crate::ui_slint::SlintUiEvent::HubSelectLayout(_)
                        | crate::ui_slint::SlintUiEvent::HubOpenTemplateEditor(_)
                        | crate::ui_slint::SlintUiEvent::HubCustomMenuAction { .. }
                        | crate::ui_slint::SlintUiEvent::HubOpenCreateModal
                        | crate::ui_slint::SlintUiEvent::HubDraftPreviewChanged { .. }
                        | crate::ui_slint::SlintUiEvent::HubModalSave { .. }
                        | crate::ui_slint::SlintUiEvent::HubModalCancel
                        | crate::ui_slint::SlintUiEvent::HubModalDuplicate
                        | crate::ui_slint::SlintUiEvent::HubModalDelete
                        | crate::ui_slint::SlintUiEvent::HubModalOpenFullscreenZones
                        | crate::ui_slint::SlintUiEvent::HubCreateConfirm { .. }
                        | crate::ui_slint::SlintUiEvent::HubClosed => {
                            let trace = next_trace_id("hub");
                            rt.ensure_gfx();
                            match rt.layout_hub.handle_slint_event(
                                &ev,
                                &mut rt.config,
                                &rt.config_dir,
                                rt.gfx.as_ref(),
                                &trace,
                            ) {
                                HubEventOutcome::None | HubEventOutcome::ConfigMutated => {}
                                HubEventOutcome::Closed { .. } => {
                                    rt.focus_border.refresh(
                                        &rt.config,
                                        rt.gfx.as_ref(),
                                        rt.is_muted(),
                                        rt.quick_switch.is_open(),
                                    );
                                    rt.schedule_cold_path_trim();
                                }
                                HubEventOutcome::LaunchFullEditor(req) => {
                                    rt.ensure_gfx();
                                    let _ = rt.layout_editor.open(
                                        req,
                                        &rt.config,
                                        rt.gfx.as_ref(),
                                        &trace,
                                    );
                                }
                            }
                        }
                        crate::ui_slint::SlintUiEvent::CaptureConfirm => {
                            let trace = next_trace_id("cap");
                            let closed = rt.workspace_capture.handle_slint_decision(
                                true,
                                &rt.config,
                                &rt.config_dir,
                                &trace,
                            );
                            if closed {
                                rt.focus_border.refresh(
                                    &rt.config,
                                    rt.gfx.as_ref(),
                                    rt.is_muted(),
                                    rt.quick_switch.is_open(),
                                );
                                rt.schedule_cold_path_trim();
                            }
                        }
                        crate::ui_slint::SlintUiEvent::CaptureCancel => {
                            let trace = next_trace_id("cap");
                            let closed = rt.workspace_capture.handle_slint_decision(
                                false,
                                &rt.config,
                                &rt.config_dir,
                                &trace,
                            );
                            if closed {
                                rt.focus_border.refresh(
                                    &rt.config,
                                    rt.gfx.as_ref(),
                                    rt.is_muted(),
                                    rt.quick_switch.is_open(),
                                );
                                rt.schedule_cold_path_trim();
                            }
                        }
                    }
                }
                true
            }
            WM_WK_INTERNAL_ALT_DRAG_EVENT => {
                let (x, y) = unpack_point_i64(ALT_DRAG_LATEST_PT.load(Ordering::Acquire));
                match wparam.0 {
                    ALT_DRAG_EVT_BEGIN_MOVE => {
                        let trace = next_trace_id("drag");
                        if !rt.alt_drag.begin(false, x, y, &rt.config, &trace) {
                            HOOK_ALT_DRAG_ACTIVE.store(0, Ordering::Release);
                        } else if rt.config.alt_drag.snap_to_zones && rt.config.features.snap_zones {
                            if let Some(moved_hwnd) = rt.alt_drag.active_move_hwnd() {
                                if rt.snap_zones.on_move_size_start(
                                    moved_hwnd,
                                    &rt.config,
                                    rt.is_muted(),
                                ) {
                                    HOOK_SNAP_DRAGGING.store(true, Ordering::Release);
                                    let _ = SetTimer(
                                        rt.control_hwnd,
                                        TIMER_SNAP_DRAG_POLL,
                                        SNAP_DRAG_POLL_INTERVAL_MS,
                                        None,
                                    );
                                }
                            }
                        }
                    }
                    ALT_DRAG_EVT_BEGIN_RESIZE => {
                        let trace = next_trace_id("drag");
                        if !rt.alt_drag.begin(true, x, y, &rt.config, &trace) {
                            HOOK_ALT_DRAG_ACTIVE.store(0, Ordering::Release);
                        }
                    }
                    ALT_DRAG_EVT_MOVE_TICK => {
                        ALT_DRAG_MOVE_PENDING.store(false, Ordering::Release);
                        let (nx, ny) = unpack_point_i64(ALT_DRAG_LATEST_PT.load(Ordering::Acquire));
                        rt.alt_drag.update(nx, ny);
                    }
                    ALT_DRAG_EVT_END => {
                        if let Some(moved_hwnd) = rt.alt_drag.end() {
                            if rt.snap_zones.is_dragging() {
                                HOOK_SNAP_DRAGGING.store(false, Ordering::Release);
                                let _ = KillTimer(rt.control_hwnd, TIMER_SNAP_DRAG_POLL);
                                let trace = next_trace_id("snap");
                                if rt.snap_zones.on_move_size_end(moved_hwnd, &rt.config, &trace) {
                                    rt.schedule_cold_path_trim();
                                }
                            }
                        }
                    }
                    _ => {}
                }
                true
            }
            WM_WK_INTERNAL_OPACITY_WHEEL => {
                let trace = next_trace_id("opac");
                let delta = lparam.0 as i16;
                let _ = rt
                    .opacity
                    .adjust_under_cursor(delta, &rt.config, &trace);
                true
            }
            WM_WK_INTERNAL_SNAP_RIGHT_CLICK => {
                rt.snap_zones.on_right_click_during_drag();
                true
            }
            WM_WK_INTERNAL_TRAY_CALLBACK => {
                let trace = next_trace_id("tray");
                let uid = wparam.0 as u32;
                let mouse_msg = (lparam.0 as u32) & 0xFFFF;
                rt.multi_monitor.handle_tray_callback(uid, mouse_msg, &trace);
                true
            }
            WM_WK_INTERNAL_WORKSPACE_PROGRESS => {
                let trace = next_trace_id("ws");
                for ev in drain_workspace_runtime_events() {
                    match ev {
                        WorkspaceRuntimeEvent::BindSnapLayout {
                            monitor_id,
                            desktop_id,
                            layout_id,
                        } => {
                            rt.config.snap_zones.bind_monitor_layout(
                                &monitor_id,
                                &desktop_id,
                                &layout_id,
                            );
                            notify_host_hub_sync(&rt.config, &rt.config_dir, true, false, &trace);
                        }
                        WorkspaceRuntimeEvent::MinimizeWindowToTray { hwnd_raw } => {
                            let target = HWND(hwnd_raw as *mut c_void);
                            let _ = rt
                                .multi_monitor
                                .minimize_hwnd_to_tray(target, &rt.config, &trace);
                        }
                        WorkspaceRuntimeEvent::ApplySceneWindowState {
                            hwnd_raw,
                            always_on_top,
                            opacity_percent,
                        } => {
                            let target = HWND(hwnd_raw as *mut c_void);
                            if always_on_top {
                                rt.ensure_gfx();
                                let _ = rt.always_on_top.ensure_pinned_hwnd(
                                    target,
                                    &rt.config,
                                    rt.gfx.as_ref(),
                                    &trace,
                                );
                                rt.sync_border_reconcile_timer();
                                rt.ensure_animation_timer();
                            }
                            if opacity_percent < 100 {
                                let _ = rt.opacity.set_window_percent(
                                    target,
                                    opacity_percent,
                                    &rt.config,
                                    &trace,
                                );
                            }
                        }
                        WorkspaceRuntimeEvent::Progress {
                            workspace_name,
                            placed,
                            total,
                            collapsed_screens,
                            finished,
                        } => {
                            rt.cancel_slint_idle_purge();
                            rt.ensure_gfx();
                            rt.workspace_progress.show_progress(
                                &workspace_name,
                                placed,
                                total,
                                collapsed_screens,
                                finished,
                                &rt.config,
                                rt.gfx.as_ref(),
                            );
                            if finished {
                                let _ = SetTimer(
                                    hwnd,
                                    TIMER_WORKSPACE_PROGRESS_HIDE,
                                    1600,
                                    None,
                                );
                            }
                        }
                    }
                }
                true
            }
            WM_WK_INTERNAL_WORKSPACE_DONE => {
                rt.schedule_cold_path_trim();
                true
            }
            WM_WK_INTERNAL_EDITOR_EVENT => {
                let trace = next_trace_id("edit");
                match rt.layout_editor.handle_event(
                    wparam.0,
                    lparam.0,
                    &mut rt.config,
                    &rt.config_dir,
                    rt.gfx.as_ref(),
                    &trace,
                ) {
                    EditorCloseOutcome::StillOpen => {}
                    EditorCloseOutcome::Closed { return_to_hub, .. } => {
                        if return_to_hub && rt.layout_hub.is_open() {
                            rt.ensure_gfx();
                            rt.layout_hub.resume_from_editor(&rt.config, rt.gfx.as_ref());
                        } else {
                            rt.focus_border.refresh(
                                &rt.config,
                                rt.gfx.as_ref(),
                                rt.is_muted(),
                                rt.quick_switch.is_open(),
                            );
                            rt.schedule_cold_path_trim();
                        }
                    }
                }
                true
            }
            WM_POWERBROADCAST => {
                let pbt = wparam.0 as u32;
                if pbt == PBT_APMRESUMEAUTOMATIC
                    || pbt == PBT_APMRESUMESUSPEND
                    || pbt == PBT_APMRESUMECRITICAL
                {
                    let trace = next_trace_id("pwr");
                    rt.reinstall_input_hooks("power_resume", &trace);
                }
                false
            }
            WM_WTSSESSION_CHANGE => {
                let event = wparam.0 as u32;
                if event == WTS_SESSION_LOCK {
                    clear_hook_phys_mods();
                } else if event == WTS_SESSION_UNLOCK
                    || event == WTS_SESSION_LOGON
                    || event == WTS_CONSOLE_CONNECT
                    || event == WTS_REMOTE_CONNECT
                {
                    let trace = next_trace_id("wts");
                    rt.reinstall_input_hooks("wts_session_resume", &trace);
                }
                true
            }
            WM_DISPLAYCHANGE | WM_SETTINGCHANGE => {
                let _ = SetTimer(
                    hwnd,
                    TIMER_DISPLAY_CHANGE_DEBOUNCE,
                    DISPLAY_CHANGE_DEBOUNCE_MS,
                    None,
                );
                false
            }
            WM_TIMER => {
                match wparam.0 {
                    TIMER_PARENT_WATCH => {
                        let now_inst = Instant::now();
                        let now_wall = SystemTime::now();
                        let inst_jump = now_inst
                            .saturating_duration_since(rt.last_watch_instant)
                            .as_secs()
                            > HOOK_WATCH_CLOCK_JUMP_SECS;
                        let wall_jump = now_wall
                            .duration_since(rt.last_watch_wall)
                            .map(|d| d.as_secs() > HOOK_WATCH_CLOCK_JUMP_SECS)
                            .unwrap_or(false);
                        rt.last_watch_instant = now_inst;
                        rt.last_watch_wall = now_wall;
                        if inst_jump || wall_jump {
                            let trace = next_trace_id("watch");
                            rt.reinstall_input_hooks("clock_jump", &trace);
                        }
                        rt.check_parent_alive();
                        rt.multi_monitor.prune_closed_tray_windows();
                    }
                    TIMER_DISPLAY_CHANGE_DEBOUNCE => {
                        let _ = KillTimer(hwnd, TIMER_DISPLAY_CHANGE_DEBOUNCE);
                        let trace = next_trace_id("topo");
                        rt.handle_topology_changed(&trace);
                    }
                    TIMER_COLD_PATH_TRIM => {
                        let _ = KillTimer(hwnd, TIMER_COLD_PATH_TRIM);
                        // 仅在真正处于冷态（未拖拽、未开启速切卡片、未开启布局管理器/编辑器/工作区捕获、无活跃动效）时释放 D2D 工厂并收缩工作集
                        if !rt.quick_switch.is_open()
                            && !rt.layout_editor.is_open()
                            && !rt.layout_hub.is_open()
                            && !rt.workspace_capture.is_open()
                            && !rt.snap_zones.is_dragging()
                            && !rt.alt_drag.is_active()
                            && !rt.anim_timer_active
                        {
                            rt.gfx = None;
                            trim_working_set_cold_path();
                        }
                    }
                    TIMER_SLINT_IDLE_PURGE => {
                        let _ = KillTimer(hwnd, TIMER_SLINT_IDLE_PURGE);
                        // 闲置 25s 且全部 Slint 窗口均已关闭 -> 彻底 Drop 窗口池释放 16~32MB 软件渲染大帧缓冲
                        if !rt.quick_switch.is_open()
                            && !rt.layout_editor.is_open()
                            && !rt.layout_hub.is_open()
                            && !rt.workspace_capture.is_open()
                            && !rt.workspace_progress.is_visible()
                        {
                            crate::ui_slint::purge_idle_windows();
                            if !rt.snap_zones.is_dragging()
                                && !rt.alt_drag.is_active()
                                && !rt.anim_timer_active
                            {
                                rt.gfx = None;
                                trim_working_set_cold_path();
                            }
                        }
                    }
                    TIMER_BORDER_PREVIEW_HIDE => {
                        let _ = KillTimer(hwnd, TIMER_BORDER_PREVIEW_HIDE);
                        rt.focus_border.hide();
                        if rt.config.features.focus_border && !rt.is_muted() {
                            rt.focus_border.refresh(
                                &rt.config,
                                rt.gfx.as_ref(),
                                false,
                                rt.quick_switch.is_open(),
                            );
                        }
                        rt.schedule_cold_path_trim();
                    }
                    TIMER_WORKSPACE_PROGRESS_HIDE => {
                        let _ = KillTimer(hwnd, TIMER_WORKSPACE_PROGRESS_HIDE);
                        rt.workspace_progress.hide_and_destroy();
                        rt.schedule_cold_path_trim();
                    }
                    TIMER_SNAP_DRAG_POLL => {
                        rt.ensure_gfx();
                        rt.snap_zones
                            .on_drag_tick(&rt.config, rt.gfx.as_ref(), rt.is_muted());
                    }
                    TIMER_ZONES_PREVIEW_HIDE => {
                        let _ = KillTimer(hwnd, TIMER_ZONES_PREVIEW_HIDE);
                        rt.snap_zones.hide_preview_overlay();
                        rt.schedule_cold_path_trim();
                    }
                    TIMER_BORDER_RECONCILE => {
                        if !rt.config.features.focus_border && rt.always_on_top.pinned_count() == 0 {
                            rt.sync_border_reconcile_timer();
                        } else {
                            rt.ensure_gfx();
                            let overlay_active = rt.quick_switch.is_open()
                                || rt.layout_editor.is_open()
                                || rt.layout_hub.is_visible();
                            rt.always_on_top
                                .sync_borders(&rt.config, rt.gfx.as_ref(), rt.is_muted());
                            rt.focus_border.refresh(
                                &rt.config,
                                rt.gfx.as_ref(),
                                rt.is_muted(),
                                overlay_active,
                            );
                            rt.sync_border_reconcile_timer();
                            rt.ensure_animation_timer();
                        }
                    }
                    TIMER_ANIMATION_FRAME => {
                        if rt.always_on_top.has_active_animation()
                            || rt.focus_border.has_active_animation()
                        {
                            rt.ensure_gfx();
                        }
                        let overlay_active = rt.quick_switch.is_open()
                            || rt.layout_editor.is_open()
                            || rt.layout_hub.is_visible();
                        let qs_more = rt.quick_switch.tick_animation(&rt.config, rt.gfx.as_ref());
                        let sz_more = rt.snap_zones.tick_animation(&rt.config, rt.gfx.as_ref());
                        let aot_more = rt.always_on_top.tick_animation(
                            &rt.config,
                            rt.gfx.as_ref(),
                            rt.is_muted(),
                        );
                        let fb_more = rt.focus_border.tick_animation(
                            &rt.config,
                            rt.gfx.as_ref(),
                            rt.is_muted(),
                            overlay_active,
                        );
                        if !qs_more && !sz_more && !aot_more && !fb_more {
                            let _ = KillTimer(hwnd, TIMER_ANIMATION_FRAME);
                            rt.anim_timer_active = false;
                        }
                    }
                    _ => {}
                }
                true
            }
            m if m != 0 && m == WM_TASKBAR_CREATED.load(Ordering::Acquire) => {
                let trace = next_trace_id("tray");
                rt.multi_monitor.on_taskbar_created(&trace);
                true
            }
            _ => false,
        }
    });

    if handled {
        if msg == WM_QUERYENDSESSION {
            LRESULT(1)
        } else {
            LRESULT(0)
        }
    } else {
        DefWindowProcW(hwnd, msg, wparam, lparam)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. CLI 参数解析与一次性模式实现
// ─────────────────────────────────────────────────────────────────────────────

enum CliMode {
    Managed {
        config_path: PathBuf,
        parent_pid: Option<u32>,
        isolated: bool,
    },
    DumpMonitors {
        out_path: PathBuf,
    },
    CaptureWorkspace {
        out_path: PathBuf,
    },
    ImportPowerToys {
        out_path: PathBuf,
        source_path: Option<PathBuf>,
    },
    SelfTest {
        out_path: Option<PathBuf>,
    },
}

fn parse_cli_args() -> Result<CliMode, String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut managed = false;
    let mut isolated = false;
    let mut config_path: Option<PathBuf> = None;
    let mut parent_pid: Option<u32> = None;
    let mut dump_monitors: Option<PathBuf> = None;
    let mut capture_workspace: Option<PathBuf> = None;
    let mut import_powertoys: Option<PathBuf> = None;
    let mut source_path: Option<PathBuf> = None;
    let mut self_test = false;
    let mut self_test_out: Option<PathBuf> = None;

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--managed" => managed = true,
            "--isolated" => isolated = true,
            "--self-test" => {
                self_test = true;
                if let Some(next) = args.get(i + 1) {
                    if !next.starts_with("--") {
                        i += 1;
                        self_test_out = Some(PathBuf::from(next));
                    }
                }
            }
            "--config" => {
                i += 1;
                let val = args.get(i).ok_or("--config requires a path")?;
                config_path = Some(PathBuf::from(val));
            }
            "--parent-pid" => {
                i += 1;
                let val = args.get(i).ok_or("--parent-pid requires a pid")?;
                parent_pid = val.parse::<u32>().ok();
            }
            "--dump-monitors" => {
                i += 1;
                let val = args.get(i).ok_or("--dump-monitors requires output path")?;
                dump_monitors = Some(PathBuf::from(val));
            }
            "--capture-workspace" => {
                i += 1;
                let val = args
                    .get(i)
                    .ok_or("--capture-workspace requires output path")?;
                capture_workspace = Some(PathBuf::from(val));
            }
            "--import-powertoys" => {
                i += 1;
                let val = args
                    .get(i)
                    .ok_or("--import-powertoys requires output path")?;
                import_powertoys = Some(PathBuf::from(val));
            }
            "--source" => {
                i += 1;
                let val = args.get(i).ok_or("--source requires a path")?;
                source_path = Some(PathBuf::from(val));
            }
            _ => {}
        }
        i += 1;
    }

    if self_test {
        return Ok(CliMode::SelfTest {
            out_path: self_test_out,
        });
    }
    if let Some(out_path) = dump_monitors {
        return Ok(CliMode::DumpMonitors { out_path });
    }
    if let Some(out_path) = capture_workspace {
        return Ok(CliMode::CaptureWorkspace { out_path });
    }
    if let Some(out_path) = import_powertoys {
        return Ok(CliMode::ImportPowerToys {
            out_path,
            source_path,
        });
    }
    if managed {
        let cfg = config_path.ok_or("--managed requires --config <path>")?;
        return Ok(CliMode::Managed {
            config_path: cfg,
            parent_pid,
            isolated,
        });
    }
    Err("missing mode flag (--managed | --dump-monitors | --capture-workspace | --import-powertoys | --self-test)".to_string())
}

fn resolve_log_dir(cfg: &WindowKitConfig, config_path: &Path) -> PathBuf {
    if !cfg.log_dir.trim().is_empty() {
        PathBuf::from(cfg.log_dir.trim())
    } else {
        config_path
            .parent()
            .map(Path::to_path_buf)
            .unwrap_or_else(|| PathBuf::from("."))
    }
}

fn run_import_powertoys(out_path: &Path, explicit_source: Option<&Path>) -> Result<(), String> {
    let source = match explicit_source {
        Some(p) => p.to_path_buf(),
        None => default_powertoys_layouts_path().unwrap_or_else(|| {
            PathBuf::from(r"Microsoft\PowerToys\FancyZones\custom-layouts.json")
        }),
    };
    let source_str = source.to_string_lossy().to_string();

    if !source.is_file() {
        if explicit_source.is_some() {
            return Err(format!("source file not found: {source_str}"));
        }
        let mut empty = serialize_imported_layouts(&source_str, &[]);
        empty.insert("notFound", crate::core::json::JsonValue::Bool(true));
        return write_text_file_atomic(out_path, &empty.to_string_pretty());
    }

    let raw = std::fs::read_to_string(&source)
        .map_err(|e| format!("failed to read PowerToys layouts '{source_str}': {e}"))?;
    let layouts = import_powertoys_fancyzones_str(&raw)?;
    let out_json = serialize_imported_layouts(&source_str, &layouts);
    write_text_file_atomic(out_path, &out_json.to_string_pretty())
}

fn run_self_test(out_path: Option<&Path>) -> i32 {
    let d2d_ok = SharedGraphics::new().is_ok();
    let monitors = enumerate_monitors();
    let stats = query_runtime_stats_json(0, 0, &[]);
    let mut report = crate::core::json::JsonValue::object();
    report.insert("ok", crate::core::json::JsonValue::Bool(d2d_ok));
    report.insert(
        "version",
        crate::core::json::JsonValue::String(APP_VERSION.to_string()),
    );
    report.insert("d2dInitialized", crate::core::json::JsonValue::Bool(d2d_ok));
    report.insert(
        "monitorCount",
        crate::core::json::JsonValue::from_usize(monitors.len()),
    );
    report.insert("stats", stats);
    let compact = report.to_string_compact();
    if let Some(p) = out_path {
        let _ = write_text_file_atomic(p, &report.to_string_pretty());
    }
    // GUI 子系统在无控制台/管道关闭时安全忽略写入错误，绝不 panic
    let _ = std::io::Write::write_all(&mut std::io::stdout(), compact.as_bytes());
    let _ = std::io::Write::write_all(&mut std::io::stdout(), b"\n");
    if d2d_ok { 0 } else { 1 }
}

fn run_managed(config_path: PathBuf, parent_pid: Option<u32>, isolated: bool) -> i32 {
    let cfg = match WindowKitConfig::load_from_file(&config_path) {
        Ok(c) => c,
        Err(_) => WindowKitConfig::default(),
    };

    let log_dir = resolve_log_dir(&cfg, &config_path);
    configure_logger(Some(&log_dir), LogLevel::from_str_config(&cfg.log_level));
    let trace_id = next_trace_id("boot");

    if !cfg.should_stay_resident() {
        wk_info!(
            &trace_id,
            "runtime",
            "WindowKit disabled or 0 features enabled, exiting cleanly with 0 hooks"
        );
        flush_logger();
        return 0;
    }

    // 单实例互斥保护：常规模式严守唯一常驻实例；--isolated 隔离验证模式独立互斥避免干扰本机已装正式版
    let mutex_handle = if isolated {
        Err(windows::core::Error::empty())
    } else {
        let mutex_name = to_wide("Local\\Tools3000_WindowKit_SingleInstance");
        unsafe { CreateMutexW(None, false, PCWSTR(mutex_name.as_ptr())) }
    };
    if let Ok(mh) = mutex_handle {
        if unsafe { GetLastError() } == ERROR_ALREADY_EXISTS {
            let cls = to_wide(CONTROL_WINDOW_CLASS);
            if let Ok(existing) = unsafe { FindWindowW(PCWSTR(cls.as_ptr()), PCWSTR::null()) } {
                if !existing.0.is_null() {
                    unsafe {
                        let _ = PostMessageW(existing, WM_WK_RELOAD_CONFIG, WPARAM(0), LPARAM(0));
                    }
                }
            }
            unsafe {
                let _ = CloseHandle(mh);
            }
            wk_info!(
                &trace_id,
                "runtime",
                "existing WindowKit instance signaled to reload config"
            );
            flush_logger();
            return 0;
        }
    }

    // 创建隐藏顶层控制窗口（注意：严禁使用 HWND_MESSAGE，必须为隐藏顶层以便宿主 FindWindowW 定位）
    let control_hwnd = match create_control_window() {
        Some(h) => h,
        None => {
            wk_error!(&trace_id, "runtime", "failed to create hidden control window");
            flush_logger();
            return 1;
        }
    };

    HOOK_CONTROL_HWND.store(control_hwnd.0 as isize, Ordering::Release);
    crate::ui_slint::set_control_hwnd(control_hwnd);
    let taskbar_msg_name = to_wide("TaskbarCreated");
    let taskbar_msg = unsafe { RegisterWindowMessageW(PCWSTR(taskbar_msg_name.as_ptr())) };
    WM_TASKBAR_CREATED.store(taskbar_msg, Ordering::Release);

    let parent_handle = parent_pid.and_then(|pid| {
        if pid == 0 {
            None
        } else {
            unsafe { OpenProcess(PROCESS_SYNCHRONIZE, false, pid).ok() }
        }
    });

    let mut runtime = ManagedRuntime::new(control_hwnd, config_path, parent_handle, cfg);
    runtime.quick_switch.set_control_hwnd(control_hwnd);
    runtime.layout_hub.set_control_hwnd(control_hwnd);
    runtime.layout_editor.set_control_hwnd(control_hwnd);
    runtime.workspace_capture.set_control_hwnd(control_hwnd);
    runtime.multi_monitor.set_control_hwnd(control_hwnd);
    runtime.wts_session_registered =
        unsafe { WTSRegisterSessionNotification(control_hwnd, NOTIFY_FOR_THIS_SESSION).is_ok() };
    runtime.reconcile_hooks(&trace_id);

    RUNTIME.with(|cell| {
        *cell.borrow_mut() = Some(runtime);
    });

    unsafe {
        let _ = SetTimer(
            control_hwnd,
            TIMER_PARENT_WATCH,
            PARENT_WATCH_INTERVAL_MS,
            None,
        );
    }

    // 零轮询虚拟桌面变更注册表异步监听器（直接产出内核 Event HANDLE 挂入主线程等待，0 额外线程 / 0 CPU）
    let vd_watcher = VirtualDesktopRegWatcher::try_new();
    wk_info!(
        &trace_id,
        "runtime",
        "Tools3000 WindowKit v{} started (ctrl=0x{:x}, parentPid={:?}, vdWatcher={})",
        APP_VERSION,
        control_hwnd.0 as isize,
        parent_pid,
        vd_watcher.is_some()
    );

    // 主消息循环：MsgWaitForMultipleObjectsEx 同时等待 Win32 消息与虚拟桌面注册表内核事件
    let mut msg = MSG::default();
    let mut quit_received = false;
    unsafe {
        while !quit_received {
            while PeekMessageW(&mut msg, HWND::default(), 0, 0, PM_REMOVE).as_bool() {
                if msg.message == WM_QUIT {
                    quit_received = true;
                    break;
                }
                let _ = TranslateMessage(&msg);
                let _ = DispatchMessageW(&msg);
            }
            if quit_received {
                break;
            }
            let wait_res = if let Some(ref w) = vd_watcher {
                let handles = [w.event_handle()];
                MsgWaitForMultipleObjectsEx(
                    Some(&handles),
                    INFINITE,
                    QS_ALLINPUT,
                    MWMO_INPUTAVAILABLE,
                )
            } else {
                MsgWaitForMultipleObjectsEx(None, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE)
            };
            if vd_watcher.is_some() && wait_res == WAIT_OBJECT_0 {
                if let Some(ref w) = vd_watcher {
                    w.rearm();
                }
                let _ = SetTimer(
                    control_hwnd,
                    TIMER_DISPLAY_CHANGE_DEBOUNCE,
                    DISPLAY_CHANGE_DEBOUNCE_MS,
                    None,
                );
            }
        }
    }

    // 最终兜底清理（防遗漏）
    RUNTIME.with(|cell| {
        if let Some(mut rt) = cell.borrow_mut().take() {
            rt.graceful_shutdown("final");
        }
    });
    emergency_restore_all_tray_windows();
    flush_logger();

    if let Ok(mh) = mutex_handle {
        unsafe {
            let _ = CloseHandle(mh);
        }
    }
    0
}

fn create_control_window() -> Option<HWND> {
    unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(CONTROL_WINDOW_CLASS);
        let wc = WNDCLASSEXW {
            cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
            lpfnWndProc: Some(control_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&wc);

        CreateWindowExW(
            WS_EX_TOOLWINDOW,
            PCWSTR(cls.as_ptr()),
            PCWSTR::null(),
            WS_POPUP,
            0,
            0,
            0,
            0,
            None,
            None,
            hinst,
            None,
        )
        .ok()
    }
}

fn main() {
    install_crash_recovery_guards();
    unsafe {
        let _ = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    let com_inited = unsafe { CoInitializeEx(None, COINIT_APARTMENTTHREADED).is_ok() };

    let exit_code = match parse_cli_args() {
        Ok(CliMode::DumpMonitors { out_path }) => {
            let monitors = enumerate_monitors();
            let json = monitors_to_json(&monitors);
            match write_text_file_atomic(&out_path, &json.to_string_pretty()) {
                Ok(()) => 0,
                Err(_) => 1,
            }
        }
        Ok(CliMode::CaptureWorkspace { out_path }) => {
            let json = capture_workspace_json(&[]);
            match write_text_file_atomic(&out_path, &json.to_string_pretty()) {
                Ok(()) => 0,
                Err(_) => 1,
            }
        }
        Ok(CliMode::ImportPowerToys {
            out_path,
            source_path,
        }) => match run_import_powertoys(&out_path, source_path.as_deref()) {
            Ok(()) => 0,
            Err(_) => 1,
        },
        Ok(CliMode::SelfTest { out_path }) => run_self_test(out_path.as_deref()),
        Ok(CliMode::Managed {
            config_path,
            parent_pid,
            isolated,
        }) => run_managed(config_path, parent_pid, isolated),
        Err(_) => 2,
    };

    if com_inited {
        unsafe {
            CoUninitialize();
        }
    }
    std::process::exit(exit_code);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn phys_mods_tracks_keys_ignores_injected_and_bridges_async_race() {
        let mut mods = 0u32;
        let mut t_lctrl = 0u32;
        let mut s_lctrl = 0u32;

        // 1. 物理 LAlt 按下：即使 GetAsyncKeyState 竞态滞后返回 false，alt_down 仍为 true
        (mods, t_lctrl, s_lctrl) = update_phys_mods_state(
            mods,
            t_lctrl,
            s_lctrl,
            VK_LMENU.0,
            0x38,
            0,
            1000,
            true,
            false,
        );
        assert_ne!(mods & PHYS_MOD_LALT, 0);
        let eff = resolve_effective_mods(mods, 0, WM_KEYDOWN, false, false, false, false, false);
        assert!(eff.alt);
        assert!(!eff.ctrl);

        // 2. 注入的 LCtrl 按下 (LLKHF_INJECTED) 必须被忽略，不能污染物理位图
        let (mods_inj, _, _) = update_phys_mods_state(
            mods,
            t_lctrl,
            s_lctrl,
            VK_LCONTROL.0,
            0x1D,
            LLKHF_INJECTED.0,
            1005,
            true,
            false,
        );
        assert_eq!(mods_inj, mods);

        // 3. 物理 LShift + LWin 按下与抬起精准对称
        (mods, t_lctrl, s_lctrl) = update_phys_mods_state(
            mods,
            t_lctrl,
            s_lctrl,
            VK_LSHIFT.0,
            0x2A,
            0,
            1010,
            true,
            false,
        );
        (mods, t_lctrl, s_lctrl) = update_phys_mods_state(
            mods,
            t_lctrl,
            s_lctrl,
            VK_LWIN.0,
            0x5B,
            LLKHF_EXTENDED.0,
            1012,
            true,
            false,
        );
        let eff2 = resolve_effective_mods(mods, 0, WM_KEYDOWN, false, false, false, false, false);
        assert!(eff2.alt && eff2.shift && eff2.win && !eff2.ctrl);

        (mods, t_lctrl, s_lctrl) = update_phys_mods_state(
            mods,
            t_lctrl,
            s_lctrl,
            VK_LMENU.0,
            0x38,
            0,
            1050,
            false,
            true,
        );
        (mods, t_lctrl, s_lctrl) = update_phys_mods_state(
            mods,
            t_lctrl,
            s_lctrl,
            VK_LSHIFT.0,
            0x2A,
            0,
            1051,
            false,
            true,
        );
        (mods, _, _) = update_phys_mods_state(
            mods,
            t_lctrl,
            s_lctrl,
            VK_LWIN.0,
            0x5B,
            LLKHF_EXTENDED.0,
            1052,
            false,
            true,
        );
        assert_eq!(mods, 0);
    }

    #[test]
    fn altgr_companion_lctrl_does_not_falsely_set_ctrl_while_real_ctrl_alt_does() {
        // 场景 A：国际布局按 AltGr -> 系统先发同时间戳假 LCtrl (0x21D 或 delta <= 2ms)，再发扩展 RMENU
        let (mods, t_lc, s_lc) = update_phys_mods_state(
            0,
            0,
            0,
            VK_LCONTROL.0,
            ALTGR_FAKE_LCTRL_SCAN,
            0,
            5000,
            true,
            false,
        );
        let (mods, t_lc, s_lc) = update_phys_mods_state(
            mods,
            t_lc,
            s_lc,
            VK_RMENU.0,
            0x38,
            LLKHF_EXTENDED.0,
            5000,
            true,
            false,
        );
        // 即使 GetAsyncKeyState(VK_LCONTROL) 返回 true，AltGr 伴生假 LCtrl 也不应被判为 ctrl_down
        let eff_altgr = resolve_effective_mods(
            mods,
            LLKHF_ALTDOWN.0,
            WM_SYSKEYDOWN,
            true,
            true,
            false,
            false,
            false,
        );
        assert!(eff_altgr.alt);
        assert!(!eff_altgr.ctrl);

        // 松开 RMENU 后自动清理 AltGr 伴生位与假 LCtrl
        let (mods_after, _, _) = update_phys_mods_state(
            mods,
            t_lc,
            s_lc,
            VK_RMENU.0,
            0x38,
            LLKHF_EXTENDED.0,
            5100,
            false,
            true,
        );
        assert_eq!(mods_after, 0);

        // 场景 B：真实按住 LCtrl (普通扫描码 0x1D) 500ms 后再按 RMENU -> 合法 Ctrl+Alt，不误拦原生 Ctrl+Alt+Tab
        let (mods_real, t_lc2, s_lc2) =
            update_phys_mods_state(0, 0, 0, VK_LCONTROL.0, 0x1D, 0, 6000, true, false);
        let (mods_real, _, _) = update_phys_mods_state(
            mods_real,
            t_lc2,
            s_lc2,
            VK_RMENU.0,
            0x38,
            LLKHF_EXTENDED.0,
            6500,
            true,
            false,
        );
        let eff_real = resolve_effective_mods(
            mods_real,
            LLKHF_ALTDOWN.0,
            WM_SYSKEYDOWN,
            true,
            true,
            false,
            false,
            false,
        );
        assert!(eff_real.alt);
        assert!(eff_real.ctrl);
    }

    #[test]
    fn held_alt_repeated_tab_and_stale_ctrl_heal_never_leak_to_windows_alt_tab() {
        // 1. 按住物理 LAlt -> 首记 Tab（即使主线程尚未置 QUICK_SWITCH_IS_OPEN）必须命中 AltTabStep
        let (mut mods, mut t_lc, mut s_lc) =
            update_phys_mods_state(0, 0, 0, VK_LMENU.0, 0x38, 0, 1000, true, false);
        let eff1 =
            resolve_effective_mods(mods, LLKHF_ALTDOWN.0, WM_SYSKEYDOWN, true, false, false, false, false);
        let d1 = decide_quick_switch_hook_action(
            VK_TAB.0, true, false, eff1, true, true, false, false, false,
        );
        assert_eq!(
            d1,
            HookQsDecision::AltTabStep {
                reverse: false,
                same_app: false,
            }
        );

        // 2. 注入假 Alt UP（如 winit::force_window_active 的 SendInput）不得污染物理位图；
        //    即使主线程仍在开窗 (qs_open=false, session_active=true) 且系统不再带 LLKHF_ALTDOWN，
        //    继续按住 Alt 连按第 2 次 Tab / Shift+Tab 也必须 100% 吞噬并切项，绝不漏回 Windows 原生切屏！
        (mods, t_lc, s_lc) = update_phys_mods_state(
            mods,
            t_lc,
            s_lc,
            VK_LMENU.0,
            0x38,
            LLKHF_INJECTED.0,
            1010,
            false,
            true,
        );
        assert_ne!(mods & PHYS_MOD_LALT, 0);

        let eff2 =
            resolve_effective_mods(mods, 0, WM_KEYDOWN, false, false, false, false, false);
        assert!(eff2.alt);
        let d2 = decide_quick_switch_hook_action(
            VK_TAB.0, true, false, eff2, true, true, false, true, false,
        );
        assert_eq!(
            d2,
            HookQsDecision::AltTabStep {
                reverse: false,
                same_app: false,
            }
        );
        let d2_up = decide_quick_switch_hook_action(
            VK_TAB.0, false, true, eff2, true, true, false, true, false,
        );
        assert_eq!(d2_up, HookQsDecision::SwallowOnly);

        // Shift+Tab 反向切项
        let eff2_rev =
            resolve_effective_mods(mods, LLKHF_ALTDOWN.0, WM_SYSKEYDOWN, true, false, false, true, false);
        let d2_rev = decide_quick_switch_hook_action(
            VK_TAB.0, true, false, eff2_rev, true, true, true, true, false,
        );
        assert_eq!(
            d2_rev,
            HookQsDecision::AltTabStep {
                reverse: true,
                same_app: false,
            }
        );

        // 3. 远控/UAC 丢失 LCtrl KEYUP 导致 PHYS_MOD_LCTRL 陈旧卡死 > 60ms 时，自愈清零避免放行原生 Alt+Tab；
        //    而刚按下 <= 60ms 的真实 Ctrl 不得被误清（防止 GetAsyncKeyState 瞬态滞后误判）。
        let (stale_mods, t_lc_stale, _) =
            update_phys_mods_state(mods, t_lc, s_lc, VK_LCONTROL.0, 0x1D, 0, 2000, true, false);
        let fresh_kept = heal_stale_phys_mods(
            stale_mods, 2030, t_lc_stale, 0, 0, 0, false, false, false, false,
        );
        assert_ne!(fresh_kept & PHYS_MOD_LCTRL, 0);

        let healed = heal_stale_phys_mods(
            stale_mods, 2100, t_lc_stale, 0, 0, 0, false, false, false, false,
        );
        assert_eq!(healed & PHYS_MOD_LCTRL, 0);
        assert_ne!(healed & PHYS_MOD_LALT, 0);

        let eff_healed =
            resolve_effective_mods(healed, LLKHF_ALTDOWN.0, WM_SYSKEYDOWN, true, false, false, false, false);
        assert!(eff_healed.alt && !eff_healed.ctrl);
        let d3 = decide_quick_switch_hook_action(
            VK_TAB.0, true, false, eff_healed, true, true, false, false, false,
        );
        assert_eq!(
            d3,
            HookQsDecision::AltTabStep {
                reverse: false,
                same_app: false,
            }
        );

        // 4. 真实 Ctrl+Alt+Tab 与 Win+Tab 仍原样放行给系统
        let eff_ctrl_alt = EffectiveMods {
            alt: true,
            ctrl: true,
            shift: false,
            win: false,
        };
        assert_eq!(
            decide_quick_switch_hook_action(
                VK_TAB.0, true, false, eff_ctrl_alt, true, true, true, true, false
            ),
            HookQsDecision::PassThrough
        );
        let eff_win_tab = EffectiveMods {
            alt: false,
            ctrl: false,
            shift: false,
            win: true,
        };
        assert_eq!(
            decide_quick_switch_hook_action(
                VK_TAB.0, true, false, eff_win_tab, true, true, true, true, false
            ),
            HookQsDecision::PassThrough
        );
    }
}
