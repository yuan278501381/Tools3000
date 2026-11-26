// ─────────────────────────────────────────────────────────────────────────────
// core/constants.rs — Tools3000 WindowKit 全局常量、消息契约与安全门限定义
// ─────────────────────────────────────────────────────────────────────────────

use windows::Win32::UI::WindowsAndMessaging::WM_APP;

/// 当前引擎版本号（与 Cargo.toml / window_kit.rc / window_kit.manifest 单一事实源对齐）
pub const APP_VERSION: &str = "1.4.17";

/// 宿主通讯隐藏控制窗口类名（供 Tools3000.exe 通过 FindWindowW 精准定位并 PostMessageW）
pub const CONTROL_WINDOW_CLASS: &str = "Tools3000_WindowKit_Control";

/// 自身所有辅助覆盖窗口类名（窗口过滤器必须永久跳过）
pub const QUICK_SWITCH_WINDOW_CLASS: &str = "Tools3000_WindowKit_QuickSwitch";
pub const SNAP_ZONES_WINDOW_CLASS: &str = "Tools3000_WindowKit_SnapZones";
pub const LAYOUT_EDITOR_WINDOW_CLASS: &str = "Tools3000_WindowKit_LayoutEditor";
pub const PIN_BORDER_WINDOW_CLASS: &str = "Tools3000_WindowKit_PinBorder";
pub const FOCUS_BORDER_WINDOW_CLASS: &str = "Tools3000_WindowKit_FocusBorder";

/// 宿主隐藏消息窗口类名与自定义回调消息（用于全屏布局编辑器保存/取消后零阻塞回传）
pub const HOST_MESSAGE_WINDOW_CLASS: &str = "Tools3000_MessageWindow";
pub const HOST_EDITOR_RESULT_MSG_NAME: &str = "Tools3000_WindowKit_EditorResult";

/// 控制窗口自定义消息契约（宿主 <-> WindowKit）
pub const WM_WK_RELOAD_CONFIG: u32 = WM_APP + 1;
pub const WM_WK_EXECUTE_ACTION: u32 = WM_APP + 2;
pub const WM_WK_SET_MUTED_FLAGS: u32 = WM_APP + 3;
pub const WM_WK_GRACEFUL_SHUTDOWN: u32 = WM_APP + 4;

/// 内部异步钩子转发消息（确保 WH_KEYBOARD_LL / WH_MOUSE_LL 严格亚毫秒无阻塞）
pub const WM_WK_INTERNAL_ALT_TAB: u32 = WM_APP + 100;
pub const WM_WK_INTERNAL_ALT_RELEASED: u32 = WM_APP + 101;
pub const WM_WK_INTERNAL_ALT_DRAG_EVENT: u32 = WM_APP + 102;
pub const WM_WK_INTERNAL_OPACITY_WHEEL: u32 = WM_APP + 103;
pub const WM_WK_INTERNAL_TRAY_CALLBACK: u32 = WM_APP + 104;
pub const WM_WK_INTERNAL_SNAP_RIGHT_CLICK: u32 = WM_APP + 106;
pub const WM_WK_INTERNAL_QUICK_SWITCH_KEY: u32 = WM_APP + 107;
pub const WM_WK_INTERNAL_WORKSPACE_DONE: u32 = WM_APP + 109;
pub const WM_WK_INTERNAL_EDITOR_EVENT: u32 = WM_APP + 110;

/// WM_WK_EXECUTE_ACTION (WM_APP + 2) 的 wParam 动作编码契约
pub const ACTION_TOGGLE_ALWAYS_ON_TOP: usize = 1;
pub const ACTION_OPACITY_UP: usize = 2;
pub const ACTION_OPACITY_DOWN: usize = 3;
pub const ACTION_OPACITY_RESET: usize = 4;
pub const ACTION_MOVE_NEXT_ZONE: usize = 5;
pub const ACTION_MOVE_PREV_ZONE: usize = 6;
pub const ACTION_MOVE_ZONE_INDEX: usize = 7;
pub const ACTION_RESTORE_FROM_ZONE: usize = 8;
pub const ACTION_RESCUE_OFFSCREEN_WINDOWS: usize = 9;
pub const ACTION_MOVE_NEXT_MONITOR: usize = 10;
pub const ACTION_MINIMIZE_TO_TRAY: usize = 11;
pub const ACTION_RESTORE_ALL_TRAY: usize = 12;
pub const ACTION_OPEN_QUICK_SWITCH: usize = 13;
pub const ACTION_CAPTURE_WORKSPACE: usize = 14;
pub const ACTION_LAUNCH_WORKSPACE: usize = 15;
pub const ACTION_IMPORT_POWERTOYS_ZONES: usize = 16;
pub const ACTION_QUERY_MONITORS: usize = 17;
pub const ACTION_QUERY_RUNTIME_STATS: usize = 18;
pub const ACTION_SHOW_ZONES_PREVIEW: usize = 19;
pub const ACTION_OPEN_LAYOUT_EDITOR: usize = 20;

/// 静音标志位（WM_APP + 3 的 wParam 位掩码）
pub const MUTED_FLAG_FULLSCREEN: usize = 1 << 0;
pub const MUTED_FLAG_REMOTE: usize = 1 << 1;

/// 定时器标识（控制窗口统一调度）
pub const TIMER_PARENT_WATCH: usize = 1001;
pub const TIMER_COLD_PATH_TRIM: usize = 1002;
pub const TIMER_SNAP_DRAG_POLL: usize = 1003;
pub const TIMER_ZONES_PREVIEW_HIDE: usize = 1004;
pub const TIMER_BORDER_RECONCILE: usize = 1005;
/// 按需驱动的 60~120Hz 丝滑动效帧定时器（仅存在活跃过渡动画时运行，收敛立即 KillTimer 保持静止 0% CPU）
pub const TIMER_ANIMATION_FRAME: usize = 1006;

/// 定时器周期与安全超时门限（严禁魔术字面量）
pub const PARENT_WATCH_INTERVAL_MS: u32 = 1000;
pub const COLD_PATH_TRIM_DELAY_MS: u32 = 2000;
pub const SNAP_DRAG_POLL_INTERVAL_MS: u32 = 12;
pub const ANIMATION_FRAME_INTERVAL_MS: u32 = 12;
pub const BORDER_RECONCILE_INTERVAL_MS: u32 = 120;
pub const FOREIGN_SEND_TIMEOUT_MS: u32 = 100;
pub const WORKSPACE_MATCH_TIMEOUT_MS: u64 = 8000;
pub const WORKSPACE_POLL_INTERVAL_MS: u64 = 150;

/// 动效时长与物理阻尼参数（对标原生桌面丝滑体验）
pub const ANIM_QUICK_SWITCH_FADE_MS: f32 = 115.0;
pub const ANIM_THUMBNAIL_FADE_MS: f32 = 95.0;
pub const ANIM_SNAP_OVERLAY_FADE_MS: f32 = 110.0;
pub const ANIM_AOT_PULSE_MS: f32 = 230.0;
pub const ANIM_FOCUS_FADE_MS: f32 = 115.0;
pub const SNAP_UNSNAP_RESTORE_DRAG_PX: i32 = 18;
pub const SNAP_TOP_EDGE_CANCEL_PX: i32 = 160;

/// 日志与文件轮转上限
pub const LOG_MAX_FILE_BYTES: u64 = 5 * 1024 * 1024;
pub const LOG_FILE_NAME: &str = "window_kit.log";
pub const LOG_BACKUP_FILE_NAME: &str = "window_kit.log.1";

/// 数据交互输出文件名（位于 --config 同目录）
pub const FILE_CAPTURE_WORKSPACE: &str = "window_kit_capture.json";
pub const FILE_ACTION_PAYLOAD: &str = "window_kit_action.json";
pub const FILE_IMPORT_POWERTOYS: &str = "window_kit_import.json";
pub const FILE_QUERY_MONITORS: &str = "window_kit_monitors.json";
pub const FILE_RUNTIME_STATS: &str = "window_kit_stats.json";
pub const FILE_LAYOUT_EDITOR_RESULT: &str = "window_kit_layout_editor_result.json";

/// 崩溃安全兜底数组容量（最多同时恢复 64 个最小化到托盘的用户窗口）
pub const MAX_TRAY_RECOVERY_SLOTS: usize = 64;

/// 最小化到托盘图标 UID 起始值
pub const TRAY_ICON_UID_BASE: u32 = 41000;

/// 界面字体族单一事实源（与 Tools3000 全局排版规范一致）
pub const FONT_UI_FAMILY: &str = "Segoe UI Variable Text";
pub const FONT_UI_FALLBACK: &str = "Microsoft YaHei UI";
pub const FONT_MONO_FAMILY: &str = "Cascadia Mono";

/// 默认透明度约束
pub const OPACITY_MIN_PERCENT_FLOOR: u8 = 20;
pub const OPACITY_MAX_PERCENT: u8 = 100;
pub const OPACITY_DEFAULT_STEP: u8 = 10;

/// 视觉与交互边界参数（逻辑 DIP）
pub const SNAP_MULTI_ZONE_EDGE_THRESHOLD_PX: i32 = 28;
pub const SNAP_TOP_EDGE_TRIGGER_PX: i32 = 10;
pub const ALT_DRAG_MIN_WINDOW_WIDTH: i32 = 160;
pub const ALT_DRAG_MIN_WINDOW_HEIGHT: i32 = 100;
pub const QUICK_SWITCH_MAX_VISIBLE_ROWS: usize = 9;
pub const QUICK_SWITCH_WIDTH_DIP: f32 = 880.0;
pub const QUICK_SWITCH_HEIGHT_DIP: f32 = 532.0;
pub const QUICK_SWITCH_LIST_WIDTH_DIP: f32 = 388.0;
pub const QUICK_SWITCH_ROW_COMFORTABLE_DIP: f32 = 46.0;
pub const QUICK_SWITCH_ROW_COMPACT_DIP: f32 = 38.0;
