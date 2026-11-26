// ─────────────────────────────────────────────────────────────────────────────
// main.rs — Tools3000 WindowKit 独立原生窗口管理引擎入口
//           负责 CLI 一次性命令分发、隐藏控制窗口通信、亚毫秒无阻塞低级钩子转发、
//           WinEvent 动态按需挂接、父进程守护、冷路径内存修剪与极端崩溃零丢窗兜底
// ─────────────────────────────────────────────────────────────────────────────

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod core;
mod features;
mod render;

use crate::core::config::{
    import_powertoys_fancyzones_str, serialize_imported_layouts, SnapTriggerMode, WindowKitConfig,
};
use crate::core::constants::*;
use crate::core::json::parse_json;
use crate::core::logger::{configure_logger, flush_logger, next_trace_id, LogLevel};
use crate::core::win_utils::{
    default_powertoys_layouts_path, emergency_restore_all_tray_windows, enumerate_monitors,
    install_crash_recovery_guards, monitors_to_json, query_runtime_stats_json, to_wide,
    trim_working_set_cold_path, write_text_file_atomic, ActionTargetOverrideGuard,
};
use crate::features::alt_drag::AltDragManager;
use crate::features::always_on_top::AlwaysOnTopManager;
use crate::features::focus_border::FocusBorderManager;
use crate::features::layout_editor::{LayoutEditorManager, OpenEditorRequest};
use crate::features::multi_monitor::MultiMonitorManager;
use crate::features::opacity::OpacityManager;
use crate::features::quick_switch::{QuickSwitchManager, QUICK_SWITCH_IS_OPEN};
use crate::features::snap_zones::SnapZonesManager;
use crate::features::workspaces::{capture_workspace_json, launch_workspace_async};
use crate::render::d2d::SharedGraphics;
use std::cell::RefCell;
use std::ffi::c_void;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicIsize, AtomicU32, Ordering};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{
    CloseHandle, GetLastError, ERROR_ALREADY_EXISTS, HANDLE, HINSTANCE, HWND, LPARAM, LRESULT,
    WAIT_OBJECT_0, WPARAM,
};
use windows::Win32::System::Com::{CoInitializeEx, CoUninitialize, COINIT_APARTMENTTHREADED};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::Threading::{
    CreateMutexW, OpenProcess, WaitForSingleObject, PROCESS_SYNCHRONIZE,
};
use windows::Win32::UI::Accessibility::{SetWinEventHook, UnhookWinEvent, HWINEVENTHOOK};
use windows::Win32::UI::HiDpi::{
    SetProcessDpiAwarenessContext, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetAsyncKeyState, VK_CONTROL, VK_LMENU, VK_LWIN, VK_MENU, VK_RMENU, VK_RWIN, VK_SHIFT, VK_TAB,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CallNextHookEx, CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, FindWindowW,
    GetMessageW, KillTimer, LoadCursorW, PostMessageW, PostQuitMessage, RegisterClassExW,
    RegisterWindowMessageW, SetTimer, SetWindowsHookExW, TranslateMessage, UnhookWindowsHookEx,
    EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_SHOW, EVENT_SYSTEM_FOREGROUND,
    EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MOVESIZEEND,
    EVENT_SYSTEM_MOVESIZESTART, HHOOK, IDC_ARROW, KBDLLHOOKSTRUCT, LLKHF_ALTDOWN, MSG,
    MSLLHOOKSTRUCT, OBJID_WINDOW, WH_KEYBOARD_LL, WH_MOUSE_LL, WINEVENT_OUTOFCONTEXT,
    WINEVENT_SKIPOWNPROCESS, WM_CLOSE, WM_ENDSESSION, WM_KEYDOWN, WM_KEYUP, WM_LBUTTONDOWN,
    WM_LBUTTONUP, WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_QUERYENDSESSION, WM_RBUTTONDOWN, WM_RBUTTONUP,
    WM_SYSKEYDOWN, WM_SYSKEYUP, WM_TIMER, WNDCLASSEXW, WS_EX_TOOLWINDOW, WS_POPUP,
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. 低级钩子严格无锁原子共享状态（保障钩子回调 < 0.05ms，绝不阻塞、绝不锁竞争）
// ─────────────────────────────────────────────────────────────────────────────

static HOOK_CONTROL_HWND: AtomicIsize = AtomicIsize::new(0);
static HOOK_MUTED: AtomicBool = AtomicBool::new(false);
static HOOK_QUICK_SWITCH_ENABLED: AtomicBool = AtomicBool::new(false);
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
static WM_TASKBAR_CREATED: AtomicU32 = AtomicU32::new(0);

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

            if !HOOK_MUTED.load(Ordering::Acquire)
                && HOOK_QUICK_SWITCH_ENABLED.load(Ordering::Acquire)
            {
                // 拦截 Alt+Tab / Alt+Shift+Tab（不干扰 Ctrl+Alt+Tab 或 Win 组合）
                if (msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) && vk == VK_TAB.0 {
                    let alt_down = (kb.flags.0 & LLKHF_ALTDOWN.0) != 0 || async_key_down(VK_MENU.0);
                    let ctrl_down = async_key_down(VK_CONTROL.0);
                    let win_down = async_key_down(VK_LWIN.0) || async_key_down(VK_RWIN.0);
                    if alt_down && !ctrl_down && !win_down {
                        let reverse = if async_key_down(VK_SHIFT.0) { 1usize } else { 0usize };
                        let _ = PostMessageW(
                            ctrl_hwnd,
                            WM_WK_INTERNAL_ALT_TAB,
                            WPARAM(reverse),
                            LPARAM(0),
                        );
                        return LRESULT(1);
                    }
                }
            }

            // Alt 键释放时通知 QuickSwitch 提交切换，同时绝不吞掉 Alt 抬起避免卡键
            if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
                && (vk == VK_LMENU.0 || vk == VK_RMENU.0 || vk == VK_MENU.0)
                && QUICK_SWITCH_IS_OPEN.load(Ordering::Acquire)
            {
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
    kbd_hook: Option<HHOOK>,
    mouse_hook: Option<HHOOK>,
    winevent_movesize: Option<HWINEVENTHOOK>,
    winevent_focus: Option<HWINEVENTHOOK>,
    winevent_location: Option<HWINEVENTHOOK>,
    winevent_destroy: Option<HWINEVENTHOOK>,
    winevent_minimize: Option<HWINEVENTHOOK>,
    winevent_show: Option<HWINEVENTHOOK>,
    quick_switch: QuickSwitchManager,
    snap_zones: SnapZonesManager,
    layout_editor: LayoutEditorManager,
    always_on_top: AlwaysOnTopManager,
    opacity: OpacityManager,
    alt_drag: AltDragManager,
    focus_border: FocusBorderManager,
    multi_monitor: MultiMonitorManager,
    last_hook_summary: String,
    anim_timer_active: bool,
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
            kbd_hook: None,
            mouse_hook: None,
            winevent_movesize: None,
            winevent_focus: None,
            winevent_location: None,
            winevent_destroy: None,
            winevent_minimize: None,
            winevent_show: None,
            quick_switch: QuickSwitchManager::new(control_hwnd),
            snap_zones: SnapZonesManager::new(),
            layout_editor: LayoutEditorManager::new(control_hwnd),
            always_on_top: AlwaysOnTopManager::new(),
            opacity: OpacityManager::new(),
            alt_drag: AltDragManager::new(),
            focus_border: FocusBorderManager::new(),
            multi_monitor: MultiMonitorManager::new(control_hwnd),
            last_hook_summary: String::new(),
            anim_timer_active: false,
            shutting_down: false,
        }
    }

    /// 按需惰性获取或重建 D2D/DWrite/WIC 共享工厂（冷路径可完整释放归还提交内存）
    fn ensure_gfx(&mut self) {
        if self.gfx.is_none() {
            self.gfx = SharedGraphics::new().ok();
        }
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
            }
        }
    }

    /// 根据当前启用特性精确对账低级钩子、WinEventHook 与定时巡检器（未启用则 0 挂钩）
    fn reconcile_hooks(&mut self, trace_id: &str) {
        let f = self.config.features.clone();

        HOOK_QUICK_SWITCH_ENABLED.store(f.quick_switch, Ordering::Release);
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

        let need_kbd = f.quick_switch;
        let need_mouse = f.alt_drag
            || (f.opacity && self.config.opacity.alt_wheel_enabled)
            || (f.snap_zones && self.config.snap_zones.trigger_mode == SnapTriggerMode::RightClick);
        let need_movesize = f.snap_zones;
        let need_borders = f.always_on_top || f.focus_border;
        let need_show_event = (f.snap_zones && self.config.snap_zones.remember_last_zone)
            || (f.multi_monitor && self.config.multi_monitor.center_new_windows);

        unsafe {
            // 1. 键盘钩子
            if need_kbd && self.kbd_hook.is_none() {
                let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
                if let Ok(h) = SetWindowsHookExW(WH_KEYBOARD_LL, Some(low_level_keyboard_proc), hinst, 0) {
                    self.kbd_hook = Some(h);
                }
            } else if !need_kbd {
                if let Some(h) = self.kbd_hook.take() {
                    let _ = UnhookWindowsHookEx(h);
                }
                self.quick_switch.close_and_release(trace_id);
            }

            // 2. 鼠标钩子
            if need_mouse && self.mouse_hook.is_none() {
                let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
                if let Ok(h) = SetWindowsHookExW(WH_MOUSE_LL, Some(low_level_mouse_proc), hinst, 0) {
                    self.mouse_hook = Some(h);
                }
            } else if !need_mouse {
                if let Some(h) = self.mouse_hook.take() {
                    let _ = UnhookWindowsHookEx(h);
                }
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
                let _ = SetTimer(
                    self.control_hwnd,
                    TIMER_BORDER_RECONCILE,
                    BORDER_RECONCILE_INTERVAL_MS,
                    None,
                );
            } else {
                for slot in [
                    &mut self.winevent_focus,
                    &mut self.winevent_location,
                    &mut self.winevent_destroy,
                    &mut self.winevent_minimize,
                ] {
                    if let Some(h) = slot.take() {
                        let _ = UnhookWinEvent(h);
                    }
                }
                let _ = KillTimer(self.control_hwnd, TIMER_BORDER_RECONCILE);
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
                self.quick_switch.is_open() || self.layout_editor.is_open(),
            );
        }

        if !f.always_on_top {
            self.always_on_top.cleanup_overlays();
        } else {
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
            self.kbd_hook.is_some(),
            self.mouse_hook.is_some(),
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
            }
            Err(err) => {
                wk_warn!(trace_id, "runtime", "reload config failed: {}", err);
            }
        }
    }

    fn set_muted_flags(&mut self, flags: usize, trace_id: &str) {
        self.muted_flags = flags;
        let muted = self.is_muted();
        HOOK_MUTED.store(muted, Ordering::Release);
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
            self.focus_border.refresh(
                &self.config,
                self.gfx.as_ref(),
                false,
                self.quick_switch.is_open(),
            );
            self.always_on_top
                .sync_borders(&self.config, self.gfx.as_ref(), false);
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
                    let closed = self.quick_switch.toggle_programmatic(
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
                    self.ensure_gfx();
                }
                let closed = self.quick_switch.toggle_programmatic(
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
                let json = capture_workspace_json(&self.config.excluded_apps);
                let out_path = self.config_dir.join(FILE_CAPTURE_WORKSPACE);
                if let Err(e) = write_text_file_atomic(&out_path, &json.to_string_pretty()) {
                    wk_error!(trace_id, "workspaces", "capture write failed: {}", e);
                } else {
                    wk_info!(
                        trace_id,
                        "workspaces",
                        "captured workspace to {}",
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
                // 打开 1:1 全屏原生布局编辑器前先收起可能遮挡的浮层并解析可选参数
                self.snap_zones.hide_preview_overlay();
                self.focus_border.hide();
                let mut req = OpenEditorRequest {
                    monitor_id: None,
                    desktop_id: None,
                    layout_id: None,
                    restore_settings_hwnd: if arg > 0x100 { Some(arg) } else { None },
                    ui_lang: "zh".to_string(),
                };
                let payload_path = self.config_dir.join(FILE_ACTION_PAYLOAD);
                if let Ok(raw) = std::fs::read_to_string(&payload_path) {
                    if let Ok(val) = parse_json(&raw) {
                        let mid = val.get_str("monitorId", "").trim().to_string();
                        if !mid.is_empty() {
                            req.monitor_id = Some(mid);
                        }
                        let did = val.get_str("desktopId", "").trim().to_string();
                        if !did.is_empty() {
                            req.desktop_id = Some(did);
                        }
                        let lid = val.get_str("layoutId", "").trim().to_string();
                        if !lid.is_empty() {
                            req.layout_id = Some(lid);
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
                self.ensure_gfx();
                let _ = self
                    .layout_editor
                    .open(req, &self.config, self.gfx.as_ref(), trace_id);
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
                self.always_on_top
                    .sync_borders(&self.config, self.gfx.as_ref(), self.is_muted());
                self.focus_border.refresh(
                    &self.config,
                    self.gfx.as_ref(),
                    self.is_muted(),
                    self.quick_switch.is_open(),
                );
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
            | EVENT_SYSTEM_MINIMIZESTART
            | EVENT_SYSTEM_MINIMIZEEND => {
                if event == EVENT_OBJECT_DESTROY {
                    self.snap_zones.on_window_destroyed(hwnd);
                    self.multi_monitor.on_window_destroyed(hwnd);
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
                    self.quick_switch.is_open() || self.layout_editor.is_open(),
                );
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
                TIMER_SNAP_DRAG_POLL,
                TIMER_ZONES_PREVIEW_HIDE,
                TIMER_BORDER_RECONCILE,
                TIMER_ANIMATION_FRAME,
            ] {
                let _ = KillTimer(self.control_hwnd, timer_id);
            }
            self.anim_timer_active = false;
            if let Some(h) = self.kbd_hook.take() {
                let _ = UnhookWindowsHookEx(h);
            }
            if let Some(h) = self.mouse_hook.take() {
                let _ = UnhookWindowsHookEx(h);
            }
            for slot in [
                &mut self.winevent_movesize,
                &mut self.winevent_focus,
                &mut self.winevent_location,
                &mut self.winevent_destroy,
                &mut self.winevent_minimize,
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
        self.snap_zones.cancel_and_cleanup();
        self.always_on_top.cleanup_overlays();
        self.focus_border.destroy();
        self.alt_drag.end();

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
                rt.ensure_gfx();
                rt.quick_switch
                    .on_alt_tab_step(reverse, &rt.config, rt.gfx.as_ref(), &trace);
                rt.ensure_animation_timer();
                true
            }
            WM_WK_INTERNAL_ALT_RELEASED => {
                let trace = next_trace_id("qs");
                if rt.quick_switch.on_alt_released(&trace) {
                    rt.schedule_cold_path_trim();
                }
                rt.ensure_animation_timer();
                true
            }
            WM_WK_INTERNAL_QUICK_SWITCH_KEY => {
                let trace = next_trace_id("qs");
                if rt.quick_switch.handle_card_event(
                    wparam.0,
                    lparam.0,
                    &rt.config,
                    rt.gfx.as_ref(),
                    &trace,
                ) {
                    rt.schedule_cold_path_trim();
                }
                rt.ensure_animation_timer();
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
            WM_WK_INTERNAL_WORKSPACE_DONE => {
                rt.schedule_cold_path_trim();
                true
            }
            WM_WK_INTERNAL_EDITOR_EVENT => {
                let trace = next_trace_id("edit");
                let closed = rt.layout_editor.handle_event(
                    wparam.0,
                    lparam.0,
                    &rt.config_dir,
                    rt.gfx.as_ref(),
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
                true
            }
            WM_TIMER => {
                match wparam.0 {
                    TIMER_PARENT_WATCH => {
                        rt.check_parent_alive();
                        rt.multi_monitor.prune_closed_tray_windows();
                    }
                    TIMER_COLD_PATH_TRIM => {
                        let _ = KillTimer(hwnd, TIMER_COLD_PATH_TRIM);
                        // 仅在真正处于冷态（未拖拽、未开启速切卡片、未开启全屏布局编辑器、无活跃动效）时释放 D2D 工厂并收缩工作集
                        if !rt.quick_switch.is_open()
                            && !rt.layout_editor.is_open()
                            && !rt.snap_zones.is_dragging()
                            && !rt.alt_drag.is_active()
                            && !rt.anim_timer_active
                        {
                            rt.gfx = None;
                            trim_working_set_cold_path();
                        }
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
                        rt.always_on_top
                            .sync_borders(&rt.config, rt.gfx.as_ref(), rt.is_muted());
                        rt.focus_border.refresh(
                            &rt.config,
                            rt.gfx.as_ref(),
                            rt.is_muted(),
                            rt.quick_switch.is_open(),
                        );
                        rt.ensure_animation_timer();
                    }
                    TIMER_ANIMATION_FRAME => {
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
                            rt.quick_switch.is_open(),
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

fn run_managed(config_path: PathBuf, parent_pid: Option<u32>) -> i32 {
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

    // 单实例互斥保护：如已有常驻实例则通知其热重载配置并优雅退出
    let mutex_name = to_wide("Local\\Tools3000_WindowKit_SingleInstance");
    let mutex_handle = unsafe { CreateMutexW(None, false, PCWSTR(mutex_name.as_ptr())) };
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
    runtime.layout_editor.set_control_hwnd(control_hwnd);
    runtime.multi_monitor.set_control_hwnd(control_hwnd);
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

    wk_info!(
        &trace_id,
        "runtime",
        "Tools3000 WindowKit v{} started (ctrl=0x{:x}, parentPid={:?})",
        APP_VERSION,
        control_hwnd.0 as isize,
        parent_pid
    );

    // 主消息循环
    let mut msg = MSG::default();
    unsafe {
        while GetMessageW(&mut msg, HWND::default(), 0, 0).as_bool() {
            let _ = TranslateMessage(&msg);
            let _ = DispatchMessageW(&msg);
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
        }) => run_managed(config_path, parent_pid),
        Err(_) => 2,
    };

    if com_inited {
        unsafe {
            CoUninitialize();
        }
    }
    std::process::exit(exit_code);
}
