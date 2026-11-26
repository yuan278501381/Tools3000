// ─────────────────────────────────────────────────────────────────────────────
// features/workspaces.rs — 一键捕获当前桌面工作区布局 & 异步并发启动编排引擎
//                          启动匹配全在后台工作线程执行，绝不阻塞主消息循环或钩子
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{WindowKitConfig, WorkspaceAppEntry, WorkspaceProfile};
use crate::core::constants::{
    WM_WK_INTERNAL_WORKSPACE_DONE, WORKSPACE_MATCH_TIMEOUT_MS, WORKSPACE_POLL_INTERVAL_MS,
};
use crate::core::geometry::resolve_zone_by_index;
use crate::core::json::JsonValue;
use crate::core::win_utils::{
    enumerate_monitors, enumerate_user_windows, minimize_window_async, place_window_visual_rect,
    to_wide, WindowInfo,
};
use crate::{wk_info, wk_warn};
use std::collections::HashSet;
use std::path::Path;
use std::thread;
use std::time::{Duration, Instant};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HWND, LPARAM, WPARAM};
use windows::Win32::UI::Shell::ShellExecuteW;
use windows::Win32::UI::WindowsAndMessaging::{
    PostMessageW, ShowWindow, SW_MAXIMIZE, SW_MINIMIZE, SW_SHOWNORMAL,
};

/// 捕获当前所有可见正常顶层用户窗口快照，输出标准工作区 JSON
pub fn capture_workspace_json(excluded_apps: &[String]) -> JsonValue {
    let windows = enumerate_user_windows(true, excluded_apps);
    let mut root = JsonValue::object();
    root.insert("count", JsonValue::from_usize(windows.len()));
    let mut arr = JsonValue::array();
    for w in &windows {
        arr.push(w.to_workspace_json());
    }
    root.insert("windows", arr);
    root
}

/// 在后台独立线程异步启动并编排工作区（最多等待 8 秒自动匹配与精准分区停靠）
pub fn launch_workspace_async(
    profile: WorkspaceProfile,
    cfg: WindowKitConfig,
    control_hwnd: HWND,
    trace_id: String,
) {
    let ctrl_raw = control_hwnd.0 as isize;
    thread::spawn(move || {
        let placed = execute_workspace_launch(&profile, &cfg, &trace_id);
        wk_info!(
            &trace_id,
            "workspaces",
            "workspace '{}' ({}) finished: placed {}/{} app(s)",
            profile.id,
            profile.name,
            placed,
            profile.apps.len()
        );
        if ctrl_raw != 0 {
            unsafe {
                let _ = PostMessageW(
                    HWND(ctrl_raw as *mut std::ffi::c_void),
                    WM_WK_INTERNAL_WORKSPACE_DONE,
                    WPARAM(placed),
                    LPARAM(0),
                );
            }
        }
    });
}

fn execute_workspace_launch(
    profile: &WorkspaceProfile,
    cfg: &WindowKitConfig,
    trace_id: &str,
) -> usize {
    if profile.apps.is_empty() {
        return 0;
    }

    let monitors = enumerate_monitors();
    let existing = enumerate_user_windows(true, &cfg.excluded_apps);
    let mut claimed_hwnds: HashSet<isize> = HashSet::new();
    let mut assigned: Vec<Option<HWND>> = vec![None; profile.apps.len()];

    // 1. 先复用已打开的匹配窗口（按进程名/可执行路径与标题提示一对一绑定）
    for (idx, app) in profile.apps.iter().enumerate() {
        if let Some(win) = find_matching_window(app, &existing, &claimed_hwnds) {
            claimed_hwnds.insert(win.hwnd.0 as isize);
            assigned[idx] = Some(win.hwnd);
        }
    }

    // 2. 若开启最小化无关窗口，立即异步最小化未在本次工作区内的现有可见窗口
    let should_minimize_unrelated = profile
        .minimize_unrelated
        .unwrap_or(cfg.workspaces.minimize_unrelated);
    if should_minimize_unrelated {
        for win in &existing {
            if !win.is_minimized && !claimed_hwnds.contains(&(win.hwnd.0 as isize)) {
                minimize_window_async(win.hwnd);
            }
        }
    }

    // 3. 并发拉起尚未运行的缺失应用
    let mut pending_indices = Vec::new();
    for (idx, app) in profile.apps.iter().enumerate() {
        if assigned[idx].is_some() {
            continue;
        }
        if !app.exe_path.trim().is_empty() {
            if spawn_app_nonblocking(&app.exe_path, &app.args) {
                wk_info!(
                    trace_id,
                    "workspaces",
                    "launched app exe='{}' for slot {}",
                    app.exe_path,
                    idx
                );
                pending_indices.push(idx);
            } else {
                wk_warn!(
                    trace_id,
                    "workspaces",
                    "failed to launch exe='{}' for slot {}",
                    app.exe_path,
                    idx
                );
            }
        }
    }

    // 4. 先安置已经就绪的既有窗口
    let mut placed_count = 0usize;
    for (idx, maybe_hwnd) in assigned.iter().enumerate() {
        if let Some(hwnd) = *maybe_hwnd {
            if apply_workspace_app_placement(hwnd, &profile.apps[idx], cfg, &monitors) {
                placed_count += 1;
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
                if apply_workspace_app_placement(win.hwnd, app, cfg, &monitors) {
                    placed_count += 1;
                }
            } else {
                still_pending.push(idx);
            }
        }
        pending_indices = still_pending;
    }

    placed_count
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
        if best.map_or(true, |(b_score, _)| score > b_score) {
            best = Some((score, win));
        }
    }
    best.map(|(_, w)| w)
}

fn apply_workspace_app_placement(
    hwnd: HWND,
    app: &WorkspaceAppEntry,
    cfg: &WindowKitConfig,
    monitors: &[crate::core::win_utils::MonitorInfo],
) -> bool {
    if app.minimized {
        unsafe {
            let _ = ShowWindow(hwnd, SW_MINIMIZE);
        }
        return true;
    }

    let target_mon = monitors
        .iter()
        .find(|m| !app.monitor_id.is_empty() && m.id.eq_ignore_ascii_case(&app.monitor_id))
        .or_else(|| monitors.iter().find(|m| m.is_primary))
        .or_else(|| monitors.first());

    // 优先按绑定的 SnapZones 分区索引停靠，其次使用记录的精确矩形
    let desired_rect = if let (Some(z_idx), Some(mon)) = (app.zone_index, target_mon) {
        let layout = cfg.layout_for_monitor(&mon.id, None);
        let zones = layout.resolve(mon.work_rect, cfg.snap_zones.zone_gap);
        resolve_zone_by_index(&zones, z_idx).or(app.rect)
    } else {
        app.rect
    };

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

fn spawn_app_nonblocking(exe_path: &str, args: &str) -> bool {
    let verb = to_wide("open");
    let file = to_wide(exe_path);
    let params = to_wide(args);
    let params_ptr = if args.trim().is_empty() {
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
