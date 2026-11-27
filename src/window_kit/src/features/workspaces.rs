// ─────────────────────────────────────────────────────────────────────────────
// features/workspaces.rs — Workspaces v2：工作区全量快照捕获 + 异步编排引擎 + 原生进度浮窗
//
// 世界级组合降维亮点:
//   1. 精准快照：仅捕获当前虚拟桌面窗口（含最小化窗真实还原位置），采集 PEB 启动参数、
//      TokenElevation 管理员标识、UWP AUMID、32px WIC PNG 真实彩色图标、4 级显示器指纹、
//      归一化相对坐标，并自动识别窗口当前命中的 SnapZones 分区槽位（含跨区并集）。
//   2. 智能启动编排：
//      - 按需联动绑定工作区关联的 SnapZones 布局；
//      - 按 runAsAdmin 精准分流（普通应用经 Explorer 降权启动，管理员应用 runas 提权，UWP 协议直启）；
//      - 4 级缺屏折叠回退（3 屏工作区切单屏笔记本按比例丝滑落屏绝不失联）；
//      - 三态无关窗口清场（保留 / 最小化 / 收纳系统托盘）；
//      - 场景态自动施加单应用置顶（Always-on-Top）与自定义透明度（Opacity）；
//   3. 原生轻量启动进度浮窗：位于主屏右下角，纯 Direct2D 分层透明窗，完成后 1.6s 销毁归零。
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{
    parse_hex_color, UnrelatedWindowAction, WindowKitConfig, WorkspaceAppEntry, WorkspaceProfile,
    ZoneLayoutType,
};
use crate::core::constants::{
    WM_WK_INTERNAL_WORKSPACE_DONE, WM_WK_INTERNAL_WORKSPACE_PROGRESS,
    WORKSPACE_MATCH_TIMEOUT_MS, WORKSPACE_POLL_INTERVAL_MS, WORKSPACE_PROGRESS_WINDOW_CLASS,
};
use crate::core::geometry::{
    detect_matching_zone_indices, merge_zone_rects, resolve_zone_by_index, RectI,
};
use crate::core::json::JsonValue;
use crate::core::topology::{
    enumerate_virtual_desktop_topology, is_window_on_current_virtual_desktop,
    match_monitor_with_fallback, move_window_to_current_virtual_desktop, MonitorMatchQuality,
    MonitorOrientation, NormalizedWorkRect,
};
use crate::core::win_utils::{
    enumerate_monitors, enumerate_user_windows, extract_window_icon_png_data_url,
    get_window_aumid, get_window_normal_placement_rect, get_window_visual_rect,
    is_process_elevated, minimize_window_async, monitor_for_hwnd, place_window_visual_rect,
    query_process_command_line_args, spawn_process_with_elevation_policy, to_wide, MonitorInfo,
    WindowInfo,
};
use crate::render::d2d::{
    rect_f, render_layered_window_d2d, rgb_a, rgba, rounded_rect, SharedGraphics,
};
use crate::{wk_info, wk_warn};
use std::collections::{HashMap, HashSet};
use std::ffi::c_void;
use std::path::Path;
use std::sync::{Mutex, Once, OnceLock};
use std::thread;
use std::time::{Duration, Instant};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::Graphics::Direct2D::D2D1_DRAW_TEXT_OPTIONS_CLIP;
use windows::Win32::Graphics::DirectWrite::DWRITE_MEASURING_MODE_NATURAL;
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, IsWindow, LoadCursorW, PostMessageW,
    RegisterClassExW, SetWindowPos, ShowWindow, CS_HREDRAW, CS_VREDRAW, HWND_TOPMOST, IDC_ARROW,
    SWP_NOACTIVATE, SW_HIDE, SW_MAXIMIZE, SW_MINIMIZE, SW_SHOWNOACTIVATE, WNDCLASSEXW,
    WS_EX_LAYERED, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_EX_TRANSPARENT, WS_POPUP,
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. 后台编排线程 -> 主线程无阻塞回调事件队列（零锁死风险）
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub enum WorkspaceRuntimeEvent {
    BindSnapLayout {
        monitor_id: String,
        desktop_id: String,
        layout_id: String,
    },
    MinimizeWindowToTray {
        hwnd_raw: isize,
    },
    ApplySceneWindowState {
        hwnd_raw: isize,
        always_on_top: bool,
        opacity_percent: u8,
    },
    Progress {
        workspace_name: String,
        placed: usize,
        total: usize,
        collapsed_screens: usize,
        finished: bool,
    },
}

fn event_queue() -> &'static Mutex<Vec<WorkspaceRuntimeEvent>> {
    static Q: OnceLock<Mutex<Vec<WorkspaceRuntimeEvent>>> = OnceLock::new();
    Q.get_or_init(|| Mutex::new(Vec::new()))
}

fn push_runtime_event(ctrl_raw: isize, evt: WorkspaceRuntimeEvent) {
    if let Ok(mut q) = event_queue().lock() {
        q.push(evt);
    }
    if ctrl_raw != 0 {
        unsafe {
            let _ = PostMessageW(
                HWND(ctrl_raw as *mut c_void),
                WM_WK_INTERNAL_WORKSPACE_PROGRESS,
                WPARAM(0),
                LPARAM(0),
            );
        }
    }
}

/// 由主线程在收到 WM_WK_INTERNAL_WORKSPACE_PROGRESS 时一次性取出全部待处理事件
pub fn drain_workspace_runtime_events() -> Vec<WorkspaceRuntimeEvent> {
    if let Ok(mut q) = event_queue().lock() {
        std::mem::take(&mut *q)
    } else {
        Vec::new()
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Workspaces v2 全量智能快照捕获（当前虚拟桌面 + 真实图标 + PEB + 提权 + 分区识别）
// ─────────────────────────────────────────────────────────────────────────────

/// 兼容旧 CLI/Action 入口的默认捕获函数
pub fn capture_workspace_json(excluded_apps: &[String]) -> JsonValue {
    let mut cfg = WindowKitConfig::default();
    cfg.excluded_apps = excluded_apps.to_vec();
    capture_workspace_v2_json(&cfg, None)
}

/// 捕获当前虚拟桌面全量应用窗口快照，输出 Workspaces v2 标准 JSON
pub fn capture_workspace_v2_json(
    cfg: &WindowKitConfig,
    exclude_hwnd: Option<HWND>,
) -> JsonValue {
    let monitors = enumerate_monitors();
    let vd_topo = enumerate_virtual_desktop_topology();
    let desktop_id = vd_topo.current_id.clone();
    let desktop_name = vd_topo
        .current_entry()
        .map(|d| d.name.clone())
        .unwrap_or_default();

    let windows = enumerate_user_windows(true, &cfg.excluded_apps);
    let mut icon_cache: HashMap<String, String> = HashMap::new();
    let mut apps_arr = JsonValue::array();
    let mut active_primary_layout_id = String::new();

    if let Some(primary) = monitors.iter().find(|m| m.is_primary).or_else(|| monitors.first()) {
        let lay = cfg.resolve_layout_for_monitor(
            primary,
            if cfg.snap_zones.per_virtual_desktop && !desktop_id.is_empty() {
                Some(desktop_id.as_str())
            } else {
                None
            },
        );
        if lay.layout_type != ZoneLayoutType::None {
            active_primary_layout_id = lay.id.clone();
        }
    }

    for (seq, w) in windows.iter().enumerate() {
        if let Some(ex) = exclude_hwnd {
            if w.hwnd == ex {
                continue;
            }
        }
        // 严格只捕获当前虚拟桌面窗口，绝不串抓其他虚拟桌面！
        if !is_window_on_current_virtual_desktop(w.hwnd) {
            continue;
        }

        let mon_idx = monitor_for_hwnd(w.hwnd, &monitors)
            .and_then(|m| monitors.iter().position(|x| x.id == m.id))
            .unwrap_or(0);
        let mon_opt = monitors.get(mon_idx);
        let mon_id = mon_opt.map(|m| m.id.clone()).unwrap_or_else(|| w.monitor_id.clone());
        let mon_friendly = mon_opt
            .map(|m| m.friendly_name.clone())
            .unwrap_or_default();
        let mon_ori = mon_opt
            .map(|m| MonitorOrientation::from_rect(m.physical_rect))
            .unwrap_or(MonitorOrientation::Landscape);

        // 最小化窗使用 GetWindowPlacement 获取真实还原桌面坐标，供前端微缩舞台精确定位
        let effective_rect = if w.is_minimized {
            get_window_normal_placement_rect(w.hwnd).unwrap_or(w.rect)
        } else {
            get_window_visual_rect(w.hwnd).unwrap_or(w.rect)
        };
        let rel_rect =
            mon_opt.and_then(|m| NormalizedWorkRect::from_rect_in_work_area(effective_rect, m.work_rect));

        // 自动识别该窗口当前是否停靠在显示器绑定的 SnapZones 单个或多个分区槽位内
        let mut matched_zones: Vec<usize> = Vec::new();
        if !w.is_minimized && !w.is_maximized {
            if let Some(mon) = mon_opt {
                let lay = cfg.resolve_layout_for_monitor(
                    mon,
                    if cfg.snap_zones.per_virtual_desktop && !desktop_id.is_empty() {
                        Some(desktop_id.as_str())
                    } else {
                        None
                    },
                );
                if lay.layout_type != ZoneLayoutType::None {
                    let gap = lay.effective_gap(cfg.snap_zones.zone_gap);
                    let zones = lay.resolve(mon.work_rect, gap);
                    matched_zones = detect_matching_zone_indices(effective_rect, &zones);
                }
            }
        }

        let aumid = get_window_aumid(w.hwnd).unwrap_or_default();
        let args = query_process_command_line_args(w.pid);
        let run_as_admin = is_process_elevated(w.pid);

        // 图标去重缓存（按 AUMID 或 exePath 缓存，捕获 15 个窗口仅需 ~20ms）
        let cache_key = if !aumid.is_empty() {
            format!("aumid:{}", aumid.to_ascii_lowercase())
        } else if !w.exe_path.is_empty() {
            format!("exe:{}", w.exe_path.to_ascii_lowercase())
        } else {
            format!("hwnd:{}", w.hwnd.0 as isize)
        };
        let icon_data_url = if let Some(cached) = icon_cache.get(&cache_key) {
            cached.clone()
        } else {
            let encoded =
                extract_window_icon_png_data_url(w.hwnd, &w.exe_path, &aumid, 32).unwrap_or_default();
            icon_cache.insert(cache_key, encoded.clone());
            encoded
        };

        let app_name = derive_friendly_app_name(&w.process_name, &w.title);

        let mut item = JsonValue::object();
        item.insert("id", JsonValue::String(format!("app_{}", seq + 1)));
        item.insert("appName", JsonValue::String(app_name));
        item.insert("processName", JsonValue::String(w.process_name.clone()));
        item.insert("exePath", JsonValue::String(w.exe_path.clone()));
        item.insert("args", JsonValue::String(args));
        item.insert("titleHint", JsonValue::String(w.title.clone()));
        item.insert("aumid", JsonValue::String(aumid));
        item.insert("iconDataUrl", JsonValue::String(icon_data_url));
        item.insert("monitorId", JsonValue::String(mon_id));
        item.insert("monitorFriendly", JsonValue::String(mon_friendly));
        item.insert(
            "monitorOrientation",
            JsonValue::String(mon_ori.as_str().to_string()),
        );
        item.insert("monitorIndex", JsonValue::from_usize(mon_idx));
        item.insert("rect", effective_rect.to_json());
        if let Some(rr) = rel_rect {
            item.insert("relRect", rr.to_json());
        }
        if let Some(&first_z) = matched_zones.first() {
            item.insert("zoneIndex", JsonValue::from_usize(first_z));
        }
        if !matched_zones.is_empty() {
            item.insert(
                "zoneIndices",
                JsonValue::Array(
                    matched_zones
                        .iter()
                        .map(|&idx| JsonValue::from_usize(idx))
                        .collect(),
                ),
            );
        }
        item.insert("maximized", JsonValue::Bool(w.is_maximized));
        item.insert("minimized", JsonValue::Bool(w.is_minimized));
        item.insert("runAsAdmin", JsonValue::Bool(run_as_admin));
        item.insert("alwaysOnTop", JsonValue::Bool(false));
        item.insert("opacityPercent", JsonValue::from_i32(100));
        apps_arr.push(item);
    }

    let mut mons_arr = JsonValue::array();
    for m in &monitors {
        mons_arr.push(m.to_json());
    }

    let mut root = JsonValue::object();
    let count = match &apps_arr {
        JsonValue::Array(v) => v.len(),
        _ => 0,
    };
    root.insert("count", JsonValue::from_usize(count));
    root.insert("desktopId", JsonValue::String(desktop_id));
    root.insert("desktopName", JsonValue::String(desktop_name));
    root.insert(
        "boundLayoutId",
        JsonValue::String(active_primary_layout_id),
    );
    root.insert("monitors", mons_arr);
    // 同时提供 windows 与 apps 别名，确保旧版与新版前端无缝兼容
    root.insert("windows", apps_arr.clone());
    root.insert("apps", apps_arr);
    root
}

fn derive_friendly_app_name(process_name: &str, title: &str) -> String {
    let stem = Path::new(process_name)
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or(process_name)
        .trim();
    match stem.to_ascii_lowercase().as_str() {
        "code" => "Visual Studio Code".to_string(),
        "chrome" => "Google Chrome".to_string(),
        "msedge" => "Microsoft Edge".to_string(),
        "explorer" => "File Explorer".to_string(),
        "wechat" | "weixin" => "微信".to_string(),
        "windowsterminal" => "Windows Terminal".to_string(),
        "applicationframehost" => {
            let t = title.trim();
            if !t.is_empty() {
                t.to_string()
            } else {
                "Windows App".to_string()
            }
        }
        "" => {
            let t = title.trim();
            if !t.is_empty() {
                t.to_string()
            } else {
                "Application".to_string()
            }
        }
        _ => stem.to_string(),
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Workspaces v2 异步编排启动引擎（4 重组合 + 4 级缺屏折叠 + 降权启动）
// ─────────────────────────────────────────────────────────────────────────────

/// 在后台独立线程异步启动并编排工作区（绝不阻塞主消息循环与钩子）
pub fn launch_workspace_async(
    profile: WorkspaceProfile,
    cfg: WindowKitConfig,
    control_hwnd: HWND,
    trace_id: String,
) {
    let ctrl_raw = control_hwnd.0 as isize;
    thread::spawn(move || {
        // 后台线程同样需要 COM STA 才能调用 IShellDispatch2 降权启动 Explorer
        let com_inited = unsafe {
            windows::Win32::System::Com::CoInitializeEx(
                None,
                windows::Win32::System::Com::COINIT_APARTMENTTHREADED,
            )
            .is_ok()
        };

        let (placed, collapsed) = execute_workspace_launch(&profile, &cfg, ctrl_raw, &trace_id);
        push_runtime_event(
            ctrl_raw,
            WorkspaceRuntimeEvent::Progress {
                workspace_name: profile.name.clone(),
                placed,
                total: profile.apps.len(),
                collapsed_screens: collapsed,
                finished: true,
            },
        );
        wk_info!(
            &trace_id,
            "workspaces",
            "workspace '{}' ({}) finished: placed {}/{} app(s), collapsedScreens={}",
            profile.id,
            profile.name,
            placed,
            profile.apps.len(),
            collapsed
        );
        if ctrl_raw != 0 {
            unsafe {
                let _ = PostMessageW(
                    HWND(ctrl_raw as *mut c_void),
                    WM_WK_INTERNAL_WORKSPACE_DONE,
                    WPARAM(placed),
                    LPARAM(0),
                );
            }
        }

        if com_inited {
            unsafe {
                windows::Win32::System::Com::CoUninitialize();
            }
        }
    });
}

fn execute_workspace_launch(
    profile: &WorkspaceProfile,
    cfg: &WindowKitConfig,
    ctrl_raw: isize,
    trace_id: &str,
) -> (usize, usize) {
    if profile.apps.is_empty() {
        return (0, 0);
    }

    let monitors = enumerate_monitors();
    let total = profile.apps.len();
    let mut collapsed_screens = 0usize;
    let mut seen_collapsed_ids: HashSet<String> = HashSet::new();

    // 预检是否有绑定到离线显示器的应用（统计缺屏折叠数，用于右下角原生进度条提示）
    for app in &profile.apps {
        if !app.minimized && !app.monitor_id.trim().is_empty() {
            if let Some(m) = match_monitor_with_fallback(
                &app.monitor_id,
                &app.monitor_friendly,
                app.monitor_orientation,
                app.monitor_index,
                &monitors,
            ) {
                if matches!(
                    m.quality,
                    MonitorMatchQuality::IndexFallback | MonitorMatchQuality::PrimaryFallback
                ) && seen_collapsed_ids.insert(app.monitor_id.to_ascii_lowercase())
                {
                    collapsed_screens += 1;
                }
            }
        }
    }

    push_runtime_event(
        ctrl_raw,
        WorkspaceRuntimeEvent::Progress {
            workspace_name: profile.name.clone(),
            placed: 0,
            total,
            collapsed_screens,
            finished: false,
        },
    );

    // 组合优势 #1：工作区联动绑定 SnapZones 布局！
    if !profile.bound_layout_id.trim().is_empty()
        && cfg.find_layout(&profile.bound_layout_id).is_some()
    {
        if let Some(primary) = monitors.iter().find(|m| m.is_primary).or_else(|| monitors.first()) {
            let desktop_id = if cfg.snap_zones.per_virtual_desktop {
                enumerate_virtual_desktop_topology().current_id
            } else {
                String::new()
            };
            push_runtime_event(
                ctrl_raw,
                WorkspaceRuntimeEvent::BindSnapLayout {
                    monitor_id: primary.id.clone(),
                    desktop_id,
                    layout_id: profile.bound_layout_id.clone(),
                },
            );
        }
    }

    let existing = enumerate_user_windows(true, &cfg.excluded_apps);
    let mut claimed_hwnds: HashSet<isize> = HashSet::new();
    let mut assigned: Vec<Option<HWND>> = vec![None; total];

    // 1. 若开启 move_existing_windows，先一对一复用已有匹配窗口（跨虚拟桌面自动迁回当前桌面）
    if profile.move_existing_windows {
        for (idx, app) in profile.apps.iter().enumerate() {
            if let Some(win) = find_matching_window(app, &existing, &claimed_hwnds) {
                claimed_hwnds.insert(win.hwnd.0 as isize);
                assigned[idx] = Some(win.hwnd);
            }
        }
    }

    // 2. 组合优势 #3：无关窗口三态清场（Keep / Minimize / MinimizeToTray）
    let unrelated_action = profile.unrelated_action.unwrap_or_else(|| {
        if profile
            .minimize_unrelated
            .unwrap_or(cfg.workspaces.minimize_unrelated)
        {
            UnrelatedWindowAction::Minimize
        } else {
            UnrelatedWindowAction::Keep
        }
    });
    if unrelated_action != UnrelatedWindowAction::Keep {
        for win in &existing {
            if !win.is_minimized
                && !claimed_hwnds.contains(&(win.hwnd.0 as isize))
                && is_window_on_current_virtual_desktop(win.hwnd)
            {
                match unrelated_action {
                    UnrelatedWindowAction::Minimize => minimize_window_async(win.hwnd),
                    UnrelatedWindowAction::MinimizeToTray => {
                        push_runtime_event(
                            ctrl_raw,
                            WorkspaceRuntimeEvent::MinimizeWindowToTray {
                                hwnd_raw: win.hwnd.0 as isize,
                            },
                        );
                    }
                    UnrelatedWindowAction::Keep => {}
                }
            }
        }
    }

    // 3. 并发拉起尚未运行的缺失应用（按 runAsAdmin 精准区分 Explorer 降权启动 vs 管理员提权启动）
    let mut pending_indices = Vec::new();
    for (idx, app) in profile.apps.iter().enumerate() {
        if assigned[idx].is_some() {
            continue;
        }
        if !app.exe_path.trim().is_empty() || !app.aumid.trim().is_empty() {
            if spawn_process_with_elevation_policy(
                &app.exe_path,
                &app.args,
                &app.aumid,
                app.run_as_admin,
            ) {
                wk_info!(
                    trace_id,
                    "workspaces",
                    "launched app exe='{}' aumid='{}' admin={} for slot {}",
                    app.exe_path,
                    app.aumid,
                    app.run_as_admin,
                    idx
                );
                pending_indices.push(idx);
            } else {
                wk_warn!(
                    trace_id,
                    "workspaces",
                    "failed to launch exe='{}' aumid='{}' for slot {}",
                    app.exe_path,
                    app.aumid,
                    idx
                );
            }
        }
    }

    // 4. 先安置已经就绪的既有窗口 + 施加场景态置顶与透明度
    let mut placed_count = 0usize;
    for (idx, maybe_hwnd) in assigned.iter().enumerate() {
        if let Some(hwnd) = *maybe_hwnd {
            let app = &profile.apps[idx];
            if apply_workspace_app_placement(hwnd, app, profile, cfg, &monitors) {
                placed_count += 1;
                dispatch_scene_window_state(ctrl_raw, hwnd, app);
                push_runtime_event(
                    ctrl_raw,
                    WorkspaceRuntimeEvent::Progress {
                        workspace_name: profile.name.clone(),
                        placed: placed_count,
                        total,
                        collapsed_screens,
                        finished: false,
                    },
                );
            }
        }
    }

    // 5. 最长轮询 8 秒匹配新启动进程出现的主窗口并自动就位
    let deadline = Instant::now() + Duration::from_millis(WORKSPACE_MATCH_TIMEOUT_MS);
    while !pending_indices.is_empty() && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(WORKSPACE_POLL_INTERVAL_MS));
        let current_windows = enumerate_user_windows(true, &cfg.excluded_apps);
        let mut still_pending = Vec::new();
        for idx in pending_indices {
            let app = &profile.apps[idx];
            if let Some(win) = find_matching_window(app, &current_windows, &claimed_hwnds) {
                claimed_hwnds.insert(win.hwnd.0 as isize);
                if apply_workspace_app_placement(win.hwnd, app, profile, cfg, &monitors) {
                    placed_count += 1;
                    dispatch_scene_window_state(ctrl_raw, win.hwnd, app);
                    push_runtime_event(
                        ctrl_raw,
                        WorkspaceRuntimeEvent::Progress {
                            workspace_name: profile.name.clone(),
                            placed: placed_count,
                            total,
                            collapsed_screens,
                            finished: false,
                        },
                    );
                }
            } else {
                still_pending.push(idx);
            }
        }
        pending_indices = still_pending;
    }

    (placed_count, collapsed_screens)
}

fn dispatch_scene_window_state(ctrl_raw: isize, hwnd: HWND, app: &WorkspaceAppEntry) {
    let op = app.opacity_percent.unwrap_or(100).clamp(25, 100);
    if app.always_on_top || op < 100 {
        push_runtime_event(
            ctrl_raw,
            WorkspaceRuntimeEvent::ApplySceneWindowState {
                hwnd_raw: hwnd.0 as isize,
                always_on_top: app.always_on_top,
                opacity_percent: op,
            },
        );
    }
}

fn find_matching_window<'a>(
    app: &WorkspaceAppEntry,
    candidates: &'a [WindowInfo],
    claimed: &HashSet<isize>,
) -> Option<&'a WindowInfo> {
    let target_proc = if !app.process_name.is_empty() {
        app.process_name.to_ascii_lowercase()
    } else {
        Path::new(&app.exe_path)
            .file_name()
            .and_then(|s| s.to_str())
            .unwrap_or("")
            .to_ascii_lowercase()
    };
    let target_exe = app.exe_path.to_ascii_lowercase();
    let target_title = app.title_hint.to_ascii_lowercase();

    let mut best: Option<(i32, &'a WindowInfo)> = None;
    for win in candidates {
        if claimed.contains(&(win.hwnd.0 as isize)) {
            continue;
        }
        let mut score = 0i32;
        if !target_exe.is_empty() && win.exe_path.to_ascii_lowercase() == target_exe {
            score += 100;
        } else if !target_proc.is_empty() && win.process_name.to_ascii_lowercase() == target_proc {
            score += 80;
        } else {
            continue;
        }
        if !target_title.is_empty() && win.title.to_ascii_lowercase().contains(&target_title) {
            score += 25;
        }
        // 优先复用当前虚拟桌面的已有窗口
        if is_window_on_current_virtual_desktop(win.hwnd) {
            score += 15;
        }
        if best.map_or(true, |(b_score, _)| score > b_score) {
            best = Some((score, win));
        }
    }
    best.map(|(_, w)| w)
}

/// 精确计算并放置工作区单个应用窗口（支持 4 级缺屏折叠、SnapZones 单/多区槽位、归一化自适应）
pub fn resolve_workspace_app_target_rect(
    app: &WorkspaceAppEntry,
    profile: &WorkspaceProfile,
    cfg: &WindowKitConfig,
    monitors: &[MonitorInfo],
) -> Option<RectI> {
    let matched = match_monitor_with_fallback(
        &app.monitor_id,
        &app.monitor_friendly,
        app.monitor_orientation,
        app.monitor_index,
        monitors,
    )?;
    let mon = matched.monitor;

    // 1. 若绑定了 SnapZones 分区槽位（单区或跨多区），优先贴合工作区关联布局或当前屏布局
    let zone_indices: Vec<usize> = if !app.zone_indices.is_empty() {
        app.zone_indices.clone()
    } else if let Some(zi) = app.zone_index {
        vec![zi]
    } else {
        Vec::new()
    };
    if !zone_indices.is_empty() {
        let layout = if !profile.bound_layout_id.trim().is_empty() {
            cfg.find_layout(&profile.bound_layout_id)
                .unwrap_or_else(|| cfg.resolve_layout_for_monitor(mon, None))
        } else {
            cfg.resolve_layout_for_monitor(mon, None)
        };
        if layout.layout_type != ZoneLayoutType::None {
            let gap = layout.effective_gap(cfg.snap_zones.zone_gap);
            let zones = layout.resolve(mon.work_rect, gap);
            if zone_indices.len() == 1 {
                if let Some(r) = resolve_zone_by_index(&zones, zone_indices[0]) {
                    return Some(r);
                }
            } else if let Some(merged) = merge_zone_rects(&zones, &zone_indices) {
                return Some(merged);
            }
        }
    }

    // 2. 缺屏/换屏/换分辨率时优先用归一化相对坐标按比例安全落地，绝不飞出屏幕外！
    if matches!(
        matched.quality,
        MonitorMatchQuality::HardwareFingerprint
            | MonitorMatchQuality::IndexFallback
            | MonitorMatchQuality::PrimaryFallback
    ) {
        if let Some(rel) = app.rel_rect {
            return Some(rel.resolve_in_work_area(mon.work_rect, 240, 160));
        }
    }

    // 3. 精确同屏且有绝对矩形 -> 钳制在当前有效工作区内防止分辨率变小后溢屏
    if let Some(raw) = app.rect {
        if matched.quality == MonitorMatchQuality::ExactId && mon.work_rect.intersect(&raw).is_some()
        {
            return Some(clamp_rect_inside_work(raw, mon.work_rect));
        }
    }

    // 4. 最终回退归一化相对坐标
    app.rel_rect
        .map(|rel| rel.resolve_in_work_area(mon.work_rect, 240, 160))
        .or_else(|| app.rect.map(|r| clamp_rect_inside_work(r, mon.work_rect)))
}

fn clamp_rect_inside_work(rect: RectI, work: RectI) -> RectI {
    let w = rect.width().clamp(240.min(work.width()), work.width());
    let h = rect.height().clamp(160.min(work.height()), work.height());
    let x = rect.left.clamp(work.left, (work.right - w).max(work.left));
    let y = rect.top.clamp(work.top, (work.bottom - h).max(work.top));
    RectI::from_xywh(x, y, w, h)
}

fn apply_workspace_app_placement(
    hwnd: HWND,
    app: &WorkspaceAppEntry,
    profile: &WorkspaceProfile,
    cfg: &WindowKitConfig,
    monitors: &[MonitorInfo],
) -> bool {
    // 若窗口在其他虚拟桌面，自动迁回当前活动虚拟桌面
    if !is_window_on_current_virtual_desktop(hwnd) {
        let _ = move_window_to_current_virtual_desktop(hwnd);
    }

    if app.minimized {
        unsafe {
            let _ = ShowWindow(hwnd, SW_MINIMIZE);
        }
        return true;
    }

    let desired_rect = resolve_workspace_app_target_rect(app, profile, cfg, monitors);
    let mut ok = true;
    if let Some(rect) = desired_rect {
        ok = place_window_visual_rect(hwnd, rect);
    }
    if app.maximized {
        unsafe {
            let _ = ShowWindow(hwnd, SW_MAXIMIZE);
        }
    }
    ok
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. 原生轻量工作区启动进度浮窗（右下角 Direct2D 分层透明窗，完成后自动销毁归零）
// ─────────────────────────────────────────────────────────────────────────────

pub struct WorkspaceProgressOverlay {
    hwnd: Option<HWND>,
}

impl WorkspaceProgressOverlay {
    pub fn new() -> Self {
        Self { hwnd: None }
    }

    pub fn show_progress(
        &mut self,
        workspace_name: &str,
        placed: usize,
        total: usize,
        collapsed_screens: usize,
        finished: bool,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
    ) {
        let Some(g) = gfx else {
            return;
        };
        let monitors = enumerate_monitors();
        let Some(primary) = monitors.iter().find(|m| m.is_primary).or_else(|| monitors.first()) else {
            return;
        };
        if self.hwnd.is_none() {
            self.hwnd = create_progress_window();
        }
        let Some(hwnd) = self.hwnd else {
            return;
        };

        let s = (primary.dpi as f32 / 96.0).max(1.0);
        let w = (336.0 * s).round() as i32;
        let h = (82.0 * s).round() as i32;
        let margin = (18.0 * s).round() as i32;
        let x = primary.work_rect.right - w - margin;
        let y = primary.work_rect.bottom - h - margin;

        let dark = cfg.is_dark_theme();
        let accent = parse_hex_color(&cfg.accent_color, [0.23, 0.51, 0.96]);
        let title_fmt = g.ui_text_format(13.0 * s, true, false).ok();
        let sub_fmt = g.mono_text_format(11.8 * s, false, false).ok();

        let ratio = if total == 0 {
            1.0
        } else {
            (placed as f32 / total as f32).clamp(0.0, 1.0)
        };
        let heading = if finished {
            format!("工作区「{workspace_name}」已就绪 ({placed}/{total})")
        } else {
            format!("正在编排工作区「{workspace_name}」 ({placed}/{total})")
        };
        let subtitle = if collapsed_screens > 0 {
            format!("检测到 {collapsed_screens} 块离线副屏 · 已按比例自适应折叠至在线屏")
        } else if finished {
            "全部窗口位置、分区与状态已同步完成".to_string()
        } else {
            "正在启动并精准停靠应用窗口...".to_string()
        };

        let ok = render_layered_window_d2d(g, hwnd, x, y, w, h, |rt| unsafe {
            let bg = if dark { rgba(0.09, 0.11, 0.15, 0.94) } else { rgba(0.99, 0.995, 1.0, 0.96) };
            let border = if dark { rgba(1.0, 1.0, 1.0, 0.16) } else { rgba(0.10, 0.14, 0.22, 0.16) };
            let track = if dark { rgba(1.0, 1.0, 1.0, 0.12) } else { rgba(0.10, 0.14, 0.22, 0.10) };
            let text_main = if dark { rgba(0.96, 0.98, 1.0, 0.98) } else { rgba(0.08, 0.11, 0.17, 0.96) };
            let text_muted = if dark { rgba(0.72, 0.78, 0.86, 0.90) } else { rgba(0.32, 0.38, 0.48, 0.92) };
            let accent_col = if finished { rgba(0.13, 0.72, 0.42, 1.0) } else { rgb_a(accent, 1.0) };

            let (Ok(b_bg), Ok(b_border), Ok(b_track), Ok(b_text), Ok(b_muted), Ok(b_accent)) = (
                rt.CreateSolidColorBrush(&bg, None),
                rt.CreateSolidColorBrush(&border, None),
                rt.CreateSolidColorBrush(&track, None),
                rt.CreateSolidColorBrush(&text_main, None),
                rt.CreateSolidColorBrush(&text_muted, None),
                rt.CreateSolidColorBrush(&accent_col, None),
            ) else {
                return;
            };

            let wf = w as f32;
            let hf = h as f32;
            let card = rect_f(1.0, 1.0, wf - 1.0, hf - 1.0);
            rt.FillRoundedRectangle(&rounded_rect(card, 10.0 * s), &b_bg);
            rt.DrawRoundedRectangle(&rounded_rect(card, 10.0 * s), &b_border, 1.2 * s, None);

            if let Some(fmt) = &title_fmt {
                let hw = to_wide(&heading);
                rt.DrawText(
                    &hw[..hw.len().saturating_sub(1)],
                    fmt,
                    &rect_f(14.0 * s, 12.0 * s, wf - 14.0 * s, 34.0 * s),
                    &b_text,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }
            if let Some(fmt) = &sub_fmt {
                let sw = to_wide(&subtitle);
                rt.DrawText(
                    &sw[..sw.len().saturating_sub(1)],
                    fmt,
                    &rect_f(14.0 * s, 35.0 * s, wf - 14.0 * s, 55.0 * s),
                    &b_muted,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }

            let bar_r = rect_f(14.0 * s, hf - 14.0 * s, wf - 14.0 * s, hf - 9.0 * s);
            rt.FillRoundedRectangle(&rounded_rect(bar_r, 2.5 * s), &b_track);
            let fill_w = (bar_r.right - bar_r.left) * ratio;
            if fill_w > 1.0 {
                let fr = rect_f(bar_r.left, bar_r.top, bar_r.left + fill_w, bar_r.bottom);
                rt.FillRoundedRectangle(&rounded_rect(fr, 2.5 * s), &b_accent);
            }
        });

        if ok.is_ok() {
            unsafe {
                let _ = SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
                let _ = ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }
        }
    }

    pub fn hide_and_destroy(&mut self) {
        if let Some(h) = self.hwnd.take() {
            unsafe {
                if IsWindow(h).as_bool() {
                    let _ = ShowWindow(h, SW_HIDE);
                    let _ = DestroyWindow(h);
                }
            }
        }
    }
}

impl Drop for WorkspaceProgressOverlay {
    fn drop(&mut self) {
        self.hide_and_destroy();
    }
}

fn ensure_progress_class() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(WORKSPACE_PROGRESS_WINDOW_CLASS);
        let wc = WNDCLASSEXW {
            cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(progress_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&wc);
    });
}

fn create_progress_window() -> Option<HWND> {
    ensure_progress_class();
    unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(WORKSPACE_PROGRESS_WINDOW_CLASS);
        CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            PCWSTR(cls.as_ptr()),
            PCWSTR::null(),
            WS_POPUP,
            0,
            0,
            1,
            1,
            None,
            None,
            hinst,
            None,
        )
        .ok()
    }
}

unsafe extern "system" fn progress_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    DefWindowProcW(hwnd, msg, wparam, lparam)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_secondary_monitor_collapses_gracefully_onto_primary_via_normalized_rect() {
        let primary = MonitorInfo {
            hmonitor: 1,
            id: "DISPLAY#PRIMARY".to_string(),
            device_name: r"\\.\DISPLAY1".to_string(),
            friendly_name: "Laptop 1080p".to_string(),
            physical_rect: RectI::new(0, 0, 1920, 1080),
            work_rect: RectI::new(0, 0, 1920, 1040),
            dpi: 120,
            scale_percent: 125,
            is_primary: true,
        };
        let app = WorkspaceAppEntry {
            app_name: "VS Code".to_string(),
            process_name: "Code.exe".to_string(),
            exe_path: r"C:\VSCode\Code.exe".to_string(),
            args: String::new(),
            title_hint: String::new(),
            aumid: String::new(),
            monitor_id: "DISPLAY#OFFLINE_4K".to_string(),
            monitor_friendly: "Dell 4K".to_string(),
            monitor_orientation: Some(MonitorOrientation::Landscape),
            monitor_index: Some(1),
            rect: Some(RectI::new(3840, 0, 5760, 2100)), // 离线副屏绝对坐标
            rel_rect: Some(NormalizedWorkRect {
                rel_x: 0.0,
                rel_y: 0.0,
                rel_w: 0.5,
                rel_h: 1.0,
            }),
            zone_index: None,
            zone_indices: Vec::new(),
            maximized: false,
            minimized: false,
            run_as_admin: false,
            always_on_top: false,
            opacity_percent: None,
        };
        let profile = WorkspaceProfile {
            id: "ws1".to_string(),
            name: "Dev".to_string(),
            bound_layout_id: String::new(),
            desktop_id: String::new(),
            desktop_name: String::new(),
            move_existing_windows: true,
            minimize_unrelated: None,
            unrelated_action: None,
            create_desktop_shortcut: false,
            last_launched_at: None,
            apps: vec![app.clone()],
        };
        let cfg = WindowKitConfig::default();
        let resolved = resolve_workspace_app_target_rect(&app, &profile, &cfg, &[primary]).unwrap();
        // 自动折叠到 1080p 主屏左半边，绝不飞出屏幕外！
        assert_eq!(resolved.left, 0);
        assert_eq!(resolved.right, 960);
        assert_eq!(resolved.bottom, 1040);
    }
}
