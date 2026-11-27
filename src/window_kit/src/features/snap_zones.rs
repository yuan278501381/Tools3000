// ─────────────────────────────────────────────────────────────────────────────
// features/snap_zones.rs — SnapZones 智能多屏分屏引擎（对标 Microsoft PowerToys FancyZones）
//                          - 拖出分区自动恢复原始窗口尺寸（RestoreSize 灵魂手感）
//                          - 按住 Ctrl 锚定跨区框选合并 + 接缝边缘自动跨区合并双模式
//                          - 144Hz 蒙层柔滑淡入 + 每分区独立悬停权重动画与编号徽章微缩放
//                          - 高分屏 Direct2D 磨砂玻璃叠加层 + 两阶段 DWM 零缝隙停靠
//                          - 记住上次分区、虚拟桌面独立绑定、键盘循环切区与实时预览
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{
    parse_hex_color, SnapTriggerMode, WindowKitConfig, ZoneLayout, ZoneLayoutType,
};
use crate::core::constants::{
    ANIM_SNAP_OVERLAY_FADE_MS, SNAP_MULTI_ZONE_EDGE_THRESHOLD_PX, SNAP_TOP_EDGE_CANCEL_PX,
    SNAP_TOP_EDGE_TRIGGER_PX, SNAP_UNSNAP_RESTORE_DRAG_PX, SNAP_ZONES_WINDOW_CLASS,
};
use crate::core::geometry::{
    hit_test_zones, merge_zone_rects, resolve_zone_by_index, RectI, ResolvedZone,
};
use crate::core::win_utils::{
    enumerate_monitors, get_window_process_info, get_window_visual_rect, is_hung_window,
    is_manageable_window, monitor_for_hwnd, monitor_for_point, place_window_visual_rect,
    resolve_foreground_or_target_hwnd, resolve_window_or_current_desktop_id, to_wide, MonitorInfo,
};
use crate::render::d2d::{
    ease_out_cubic, exp_damp_f32, lerp_f32, rect_f, render_layered_window_d2d_alpha, rgb_a, rgba,
    rounded_rect, update_layered_window_alpha_only, SharedGraphics,
};
use crate::{wk_debug, wk_info};
use std::collections::{HashMap, HashSet};
use std::sync::Once;
use std::time::Instant;
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, POINT, WPARAM};
use windows::Win32::Graphics::Direct2D::D2D1_DRAW_TEXT_OPTIONS_CLIP;
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::Input::KeyboardAndMouse::{GetAsyncKeyState, VK_CONTROL, VK_ESCAPE, VK_SHIFT};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, GetCursorPos, IsIconic, IsWindow, IsZoomed,
    LoadCursorW, RegisterClassExW, SetWindowPos, ShowWindow, CS_HREDRAW, CS_VREDRAW, HWND_TOPMOST,
    IDC_ARROW, SWP_NOACTIVATE, SW_HIDE, SW_SHOWNOACTIVATE, WNDCLASSEXW, WS_EX_LAYERED,
    WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_EX_TRANSPARENT, WS_POPUP,
};

struct DragSession {
    dragged_hwnd: HWND,
    process_name: String,
    start_cursor: (i32, i32),
    unsnap_restored: bool,
    right_click_toggled: bool,
    top_edge_latched: bool,
    cancelled: bool,
    monitors: Vec<MonitorInfo>,
    active_monitor_id: String,
    active_layout_name: String,
    active_highlight_distance_px: i32,
    active_layout_is_none: bool,
    active_zones: Vec<ResolvedZone>,
    hovered_indices: Vec<usize>,
    /// 按住 Ctrl 拖动框选多区时的起始锚点分区索引（复刻 FancyZones Ctrl 多区跨选）
    ctrl_anchor_zone: Option<usize>,
    /// 每分区独立悬停动画权重 [0.0..1.0]，丝滑过渡，收敛后停止重绘
    zone_weights: Vec<f32>,
    overlay_fade_started: Option<Instant>,
    last_anim_tick: Instant,
    overlay_visible: bool,
    last_render_key: (String, Vec<usize>),
}

struct PreviewAnimState {
    started_at: Instant,
    done: bool,
}

pub struct SnapZonesManager {
    overlay_hwnd: Option<HWND>,
    drag: Option<DragSession>,
    preview_state: Option<PreviewAnimState>,
    /// 记住每个进程最近一次停靠的分区索引（rememberLastZone）
    last_zone_by_proc: HashMap<String, usize>,
    /// 已自动处理过 rememberLastZone 的窗口句柄集合（防同一窗口重复拉扯）
    auto_snapped_hwnds: HashSet<isize>,
    /// 记录窗口首次停靠前的原始位置（用于拖出恢复原尺寸与 Action 8 还原）
    pre_snap_rects: HashMap<isize, RectI>,
    /// 记录窗口当前所处分区索引（用于 Action 5/6 顺序循环切换与拖出判定）
    current_zone_by_hwnd: HashMap<isize, usize>,
}

impl SnapZonesManager {
    pub fn new() -> Self {
        Self {
            overlay_hwnd: None,
            drag: None,
            preview_state: None,
            last_zone_by_proc: HashMap::new(),
            auto_snapped_hwnds: HashSet::new(),
            pre_snap_rects: HashMap::new(),
            current_zone_by_hwnd: HashMap::new(),
        }
    }

    pub fn is_dragging(&self) -> bool {
        self.drag.is_some()
    }

    /// 预览模式是否存在尚未收敛的 110ms 淡入动画
    pub fn has_active_animation(&self) -> bool {
        self.preview_state.as_ref().map(|p| !p.done).unwrap_or(false)
    }

    /// 推进预览层 110ms EaseOutCubic 零重绘硬件淡入（仅通过 ULW_ALPHA 更新整层透明度，0 DIB 分配、0% 冗余重绘！）
    pub fn tick_animation(&mut self, _cfg: &WindowKitConfig, _gfx: Option<&SharedGraphics>) -> bool {
        let Some(p) = self.preview_state.as_mut() else {
            return false;
        };
        if p.done {
            return false;
        }
        let elapsed = p.started_at.elapsed().as_secs_f32() * 1000.0;
        let t = (elapsed / ANIM_SNAP_OVERLAY_FADE_MS).clamp(0.0, 1.0);
        let alpha = (48.0 + ease_out_cubic(t) * 207.0).round().clamp(48.0, 255.0) as u8;
        if let Some(oh) = self.overlay_hwnd {
            update_layered_window_alpha_only(oh, alpha);
        }
        if t >= 1.0 {
            p.done = true;
        }
        self.has_active_animation()
    }

    /// 响应 EVENT_SYSTEM_MOVESIZESTART（或 Alt+Drag 起拖）：启动轻量拖拽跟踪
    pub fn on_move_size_start(&mut self, hwnd: HWND, cfg: &WindowKitConfig, muted: bool) -> bool {
        if !cfg.features.snap_zones || muted {
            return false;
        }
        let excluded = cfg.combined_excluded_apps(&cfg.snap_zones.excluded_apps);
        if !is_manageable_window(hwnd, false, &excluded) {
            return false;
        }
        let (_, process_name, _) = get_window_process_info(hwnd);
        let monitors = enumerate_monitors();
        if monitors.is_empty() {
            return false;
        }
        let mut pt = POINT::default();
        let _ = unsafe { GetCursorPos(&mut pt) };
        self.preview_state = None;
        self.drag = Some(DragSession {
            dragged_hwnd: hwnd,
            process_name,
            start_cursor: (pt.x, pt.y),
            unsnap_restored: false,
            right_click_toggled: false,
            top_edge_latched: false,
            cancelled: false,
            monitors,
            active_monitor_id: String::new(),
            active_layout_name: String::new(),
            active_highlight_distance_px: SNAP_MULTI_ZONE_EDGE_THRESHOLD_PX,
            active_layout_is_none: false,
            active_zones: Vec::new(),
            hovered_indices: Vec::new(),
            ctrl_anchor_zone: None,
            zone_weights: Vec::new(),
            overlay_fade_started: None,
            last_anim_tick: Instant::now(),
            overlay_visible: false,
            last_render_key: (String::new(), Vec::new()),
        });
        true
    }

    /// 右键触发模式：在拖拽期间单击右键即可切换分屏叠加层显隐
    pub fn on_right_click_during_drag(&mut self) {
        if let Some(drag) = self.drag.as_mut() {
            drag.right_click_toggled = !drag.right_click_toggled;
            drag.cancelled = false;
        }
    }

    /// 拖拽期间 80Hz~120Hz 丝滑驱动（仅在 drag != None 时由定时器驱动，闲置 0 CPU）
    pub fn on_drag_tick(
        &mut self,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        muted: bool,
    ) {
        let (drag_hwnd, start_cursor, already_restored, drag_cancelled) = match self.drag.as_ref() {
            Some(d) => (d.dragged_hwnd, d.start_cursor, d.unsnap_restored, d.cancelled),
            None => return,
        };

        if muted || !cfg.features.snap_zones || is_hung_window(drag_hwnd) {
            self.hide_overlay_only();
            return;
        }
        // 允许用户在拖拽过程中按 Esc 临时取消分屏叠加层
        if is_key_down(VK_ESCAPE.0 as i32) {
            if let Some(d) = self.drag.as_mut() {
                d.cancelled = true;
            }
            self.hide_overlay_only();
            return;
        }
        if drag_cancelled {
            return;
        }

        let mut pt = POINT::default();
        if unsafe { GetCursorPos(&mut pt) }.is_err() {
            return;
        }

        // 对标 PowerToys FancyZones 灵魂手感：拖出已吸附分区超过阈值时自动还原原始窗口尺寸并跟手锚定！
        if cfg.snap_zones.restore_original_size && !already_restored {
            let key = drag_hwnd.0 as isize;
            if self.current_zone_by_hwnd.contains_key(&key) {
                let dx = (pt.x - start_cursor.0).abs();
                let dy = (pt.y - start_cursor.1).abs();
                let mon_scale = self
                    .drag
                    .as_ref()
                    .and_then(|d| monitor_for_point(pt.x, pt.y, &d.monitors))
                    .map(|m| (m.dpi as f32 / 96.0).max(1.0))
                    .unwrap_or(1.0);
                let threshold = ((SNAP_UNSNAP_RESTORE_DRAG_PX as f32) * mon_scale).round() as i32;
                if dx >= threshold || dy >= threshold {
                    if let Some(orig) = self.pre_snap_rects.get(&key).copied() {
                        if let Some(cur) = get_window_visual_rect(drag_hwnd) {
                            let cur_w = cur.width().max(1) as f64;
                            let ratio_x = ((pt.x - cur.left) as f64 / cur_w).clamp(0.15, 0.85);
                            let restored_w = orig.width();
                            let restored_h = orig.height();
                            let new_left = pt.x - ((restored_w as f64) * ratio_x).round() as i32;
                            let new_top = cur.top;
                            let target_rect =
                                RectI::from_xywh(new_left, new_top, restored_w, restored_h);
                            let _ = place_window_visual_rect(drag_hwnd, target_rect);
                        }
                        self.current_zone_by_hwnd.remove(&key);
                        self.pre_snap_rects.remove(&key);
                    }
                    if let Some(d) = self.drag.as_mut() {
                        d.unsnap_restored = true;
                    }
                }
            }
        }

        let Some(drag) = self.drag.as_mut() else {
            return;
        };
        let Some(mon) = monitor_for_point(pt.x, pt.y, &drag.monitors) else {
            return;
        };

        let dpi_scale = (mon.dpi as f32 / 96.0).max(1.0);
        let shift_down = is_key_down(VK_SHIFT.0 as i32);
        let ctrl_down = is_key_down(VK_CONTROL.0 as i32);

        let triggered = match cfg.snap_zones.trigger_mode {
            SnapTriggerMode::Shift => shift_down,
            SnapTriggerMode::RightClick => drag.right_click_toggled || shift_down,
            SnapTriggerMode::TopEdge => {
                let trigger_y = mon.work_rect.top
                    + ((SNAP_TOP_EDGE_TRIGGER_PX as f32) * dpi_scale).round() as i32;
                let cancel_y = mon.work_rect.top
                    + ((SNAP_TOP_EDGE_CANCEL_PX as f32) * dpi_scale).round() as i32;
                if pt.y <= trigger_y {
                    drag.top_edge_latched = true;
                } else if drag.top_edge_latched && pt.y > cancel_y && !shift_down && drag.hovered_indices.is_empty() {
                    drag.top_edge_latched = false;
                }
                drag.top_edge_latched || shift_down
            }
            SnapTriggerMode::Always => true,
        };

        if !triggered {
            drag.ctrl_anchor_zone = None;
            if drag.overlay_visible {
                drag.overlay_visible = false;
                drag.hovered_indices.clear();
                drag.overlay_fade_started = None;
                drag.zone_weights.fill(0.0);
                drag.last_render_key = (String::new(), Vec::new());
                if let Some(oh) = self.overlay_hwnd {
                    unsafe {
                        let _ = ShowWindow(oh, SW_HIDE);
                    }
                }
            }
            return;
        }

        // 切换显示器或首次激活时重新求解分区布局（4 级解析：含虚拟桌面、横/竖屏默认与无布局静默）
        if drag.active_monitor_id != mon.id || (drag.active_zones.is_empty() && !drag.active_layout_is_none) {
            let desktop_id = if cfg.snap_zones.per_virtual_desktop {
                resolve_window_or_current_desktop_id(drag.dragged_hwnd)
            } else {
                None
            };
            let layout = cfg.resolve_layout_for_monitor(&mon, desktop_id.as_deref());
            drag.active_monitor_id = mon.id.clone();
            drag.active_layout_name = layout.name.clone();
            drag.active_highlight_distance_px =
                layout.effective_highlight_distance(cfg.snap_zones.edge_threshold_px);
            drag.active_layout_is_none = layout.layout_type == ZoneLayoutType::None;
            let gap = layout.effective_gap(cfg.snap_zones.zone_gap);
            drag.active_zones = layout.resolve(mon.work_rect, gap);
            drag.zone_weights = vec![0.0; drag.active_zones.len()];
            drag.ctrl_anchor_zone = None;
            drag.last_render_key = (String::new(), Vec::new());
        }

        // 若该屏显式配置为「无布局」，保持完全静默不干扰原生拖拽
        if drag.active_layout_is_none || drag.active_zones.is_empty() {
            if drag.overlay_visible {
                self.hide_overlay_only();
            }
            return;
        }

        if !drag.overlay_visible {
            drag.overlay_fade_started = Some(Instant::now());
            drag.last_anim_tick = Instant::now();
        }

        let base_edge = drag
            .active_highlight_distance_px
            .max(SNAP_MULTI_ZONE_EDGE_THRESHOLD_PX / 2);
        let edge_px = ((base_edge as f32) * dpi_scale).round() as i32;
        // 严格遵守 allow_multi_zone_span 开关：关闭后彻底禁用接缝跨区与按住 Ctrl 框选跨区合并
        let allow_multi = cfg.snap_zones.allow_multi_zone_span;
        let mut hovered = hit_test_zones(&drag.active_zones, pt.x, pt.y, allow_multi, edge_px);

        // 复刻 FancyZones 标志性按住 Ctrl 多区框选跨区合并：
        // 从首次按 Ctrl 的起始锚点分区到当前光标所在分区的外接包围盒相交全部分区自动融合！
        if allow_multi && ctrl_down {
            if let Some(&first_hit) = hovered.first() {
                let anchor = *drag.ctrl_anchor_zone.get_or_insert(first_hit);
                if anchor != first_hit {
                    if let Some(span_box) = merge_zone_rects(&drag.active_zones, &[anchor, first_hit]) {
                        let spanned: Vec<usize> = drag
                            .active_zones
                            .iter()
                            .filter(|z| z.rect.intersect(&span_box).is_some())
                            .map(|z| z.index)
                            .collect();
                        if !spanned.is_empty() {
                            hovered = spanned;
                        }
                    }
                }
            }
        } else {
            drag.ctrl_anchor_zone = None;
        }
        drag.hovered_indices = hovered.clone();

        // 推进 144Hz 丝滑动效（蒙层淡入 + 每分区独立悬停权重平滑插值）
        let now = Instant::now();
        let dt_ms = (now.duration_since(drag.last_anim_tick).as_secs_f32() * 1000.0).clamp(1.0, 50.0);
        drag.last_anim_tick = now;

        let mut anim_in_progress = false;
        let fade_alpha = if let Some(started) = drag.overlay_fade_started {
            let t = (started.elapsed().as_secs_f32() * 1000.0) / ANIM_SNAP_OVERLAY_FADE_MS;
            if t >= 1.0 {
                drag.overlay_fade_started = None;
                1.0
            } else {
                anim_in_progress = true;
                ease_out_cubic(t)
            }
        } else {
            1.0
        };

        if drag.zone_weights.len() != drag.active_zones.len() {
            drag.zone_weights = vec![0.0; drag.active_zones.len()];
        }
        for (slot, z) in drag.active_zones.iter().enumerate() {
            let target = if hovered.contains(&z.index) { 1.0 } else { 0.0 };
            let next = exp_damp_f32(drag.zone_weights[slot], target, dt_ms, 26.0);
            if (next - target).abs() > 0.015 {
                drag.zone_weights[slot] = next;
                anim_in_progress = true;
            } else if (drag.zone_weights[slot] - target).abs() > 0.0001 {
                drag.zone_weights[slot] = target;
                anim_in_progress = true;
            }
        }

        let render_key = (mon.id.clone(), hovered.clone());
        if drag.overlay_visible && drag.last_render_key == render_key && !anim_in_progress {
            return;
        }

        if let Some(g) = gfx {
            let zones_clone = drag.active_zones.clone();
            let weights_clone = drag.zone_weights.clone();
            let layout_name = drag.active_layout_name.clone();
            if self.ensure_and_render_overlay(
                g,
                &mon,
                &zones_clone,
                &hovered,
                &weights_clone,
                fade_alpha,
                &layout_name,
                false,
                cfg,
            ) {
                if let Some(d) = self.drag.as_mut() {
                    d.overlay_visible = true;
                    d.last_render_key = render_key;
                }
            }
        }
    }

    /// 响应 EVENT_SYSTEM_MOVESIZEEND：若停靠叠加层激活且命中分区则执行两阶段 DWM 精准停靠，随后立即释放叠加层
    /// 返回 true 表示需要调度 2000ms 延迟冷路径内存修剪
    pub fn on_move_size_end(
        &mut self,
        hwnd: HWND,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> bool {
        let Some(drag) = self.drag.take() else {
            return false;
        };
        let was_visible = drag.overlay_visible;
        self.destroy_overlay_immediately();

        if !was_visible || drag.cancelled || drag.hovered_indices.is_empty() {
            return was_visible;
        }

        let target_hwnd = if hwnd.0.is_null() {
            drag.dragged_hwnd
        } else {
            hwnd
        };
        if let Some(target_rect) = merge_zone_rects(&drag.active_zones, &drag.hovered_indices) {
            self.record_pre_snap_if_absent(target_hwnd);
            if place_window_visual_rect(target_hwnd, target_rect) {
                let first_zone = drag.hovered_indices[0];
                self.current_zone_by_hwnd
                    .insert(target_hwnd.0 as isize, first_zone);
                if cfg.snap_zones.remember_last_zone && !drag.process_name.is_empty() {
                    self.last_zone_by_proc
                        .insert(drag.process_name.to_ascii_lowercase(), first_zone);
                }
                wk_info!(
                    trace_id,
                    "snapZones",
                    "snapped hwnd=0x{:x} proc={} zones={:?} rect=({},{},{},{})",
                    target_hwnd.0 as isize,
                    drag.process_name,
                    drag.hovered_indices,
                    target_rect.left,
                    target_rect.top,
                    target_rect.right,
                    target_rect.bottom
                );
            }
        }
        true
    }

    /// 响应 EVENT_OBJECT_SHOW：若开启 rememberLastZone 且该进程存在历史停靠记录，自动将新开窗口吸附回上次分区
    /// 返回 true 表示已执行自动停靠（避免紧随其后的 centerNewWindows 覆盖）
    pub fn on_window_shown(
        &mut self,
        hwnd: HWND,
        cfg: &WindowKitConfig,
        muted: bool,
        trace_id: &str,
    ) -> bool {
        if muted || !cfg.features.snap_zones || !cfg.snap_zones.remember_last_zone || hwnd.0.is_null() {
            return false;
        }
        let key = hwnd.0 as isize;
        if self.auto_snapped_hwnds.contains(&key) || self.current_zone_by_hwnd.contains_key(&key) {
            return false;
        }
        if unsafe { IsIconic(hwnd).as_bool() || IsZoomed(hwnd).as_bool() } {
            return false;
        }
        let excluded = cfg.combined_excluded_apps(&cfg.snap_zones.excluded_apps);
        if !is_manageable_window(hwnd, false, &excluded) {
            return false;
        }
        let (_, proc_name, _) = get_window_process_info(hwnd);
        if proc_name.is_empty() {
            return false;
        }
        let proc_key = proc_name.to_ascii_lowercase();
        let Some(&last_zone) = self.last_zone_by_proc.get(&proc_key) else {
            return false;
        };

        let monitors = enumerate_monitors();
        let Some(mon) = monitor_for_hwnd(hwnd, &monitors) else {
            return false;
        };
        let desktop_id = if cfg.snap_zones.per_virtual_desktop {
            resolve_window_or_current_desktop_id(hwnd)
        } else {
            None
        };
        let layout = cfg.resolve_layout_for_monitor(&mon, desktop_id.as_deref());
        if layout.layout_type == ZoneLayoutType::None {
            return false;
        }
        let gap = layout.effective_gap(cfg.snap_zones.zone_gap);
        let zones = layout.resolve(mon.work_rect, gap);
        let Some(rect) = resolve_zone_by_index(&zones, last_zone) else {
            return false;
        };

        // 定期清理已销毁窗口句柄，杜绝 HashSet 长期增长
        if self.auto_snapped_hwnds.len() > 256 {
            self.auto_snapped_hwnds
                .retain(|&raw| unsafe { IsWindow(HWND(raw as *mut std::ffi::c_void)).as_bool() });
        }
        self.auto_snapped_hwnds.insert(key);

        if self.snap_hwnd_to_zone_rect(hwnd, last_zone, rect, cfg, trace_id) {
            wk_info!(
                trace_id,
                "snapZones",
                "rememberLastZone auto-snapped new hwnd=0x{:x} proc={} to zone={}",
                key,
                proc_name,
                last_zone
            );
            return true;
        }
        false
    }

    /// 窗口销毁时回收记录表条目
    pub fn on_window_destroyed(&mut self, hwnd: HWND) {
        let key = hwnd.0 as isize;
        self.auto_snapped_hwnds.remove(&key);
        self.current_zone_by_hwnd.remove(&key);
        self.pre_snap_rects.remove(&key);
    }

    /// Action 5 / 6: 快捷键将当前前台窗口循环移至下一个 (+1) 或上一个 (-1) 分区
    pub fn move_foreground_relative(
        &mut self,
        delta: i32,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> bool {
        let fg = resolve_foreground_or_target_hwnd();
        let excluded = cfg.combined_excluded_apps(&cfg.snap_zones.excluded_apps);
        if fg.0.is_null() || !is_manageable_window(fg, false, &excluded) {
            return false;
        }
        let monitors = enumerate_monitors();
        let Some(mon) = monitor_for_hwnd(fg, &monitors) else {
            return false;
        };
        let desktop_id = if cfg.snap_zones.per_virtual_desktop {
            resolve_window_or_current_desktop_id(fg)
        } else {
            None
        };
        let layout = cfg.resolve_layout_for_monitor(&mon, desktop_id.as_deref());
        if layout.layout_type == ZoneLayoutType::None {
            return false;
        }
        let gap = layout.effective_gap(cfg.snap_zones.zone_gap);
        let zones = layout.resolve(mon.work_rect, gap);
        if zones.is_empty() {
            return false;
        }

        let key = fg.0 as isize;
        let current = self.current_zone_by_hwnd.get(&key).copied();
        let next_pos = match current {
            Some(cur_idx) => {
                let pos = zones
                    .iter()
                    .position(|z| z.index == cur_idx)
                    .unwrap_or(0) as i32;
                (pos + delta).rem_euclid(zones.len() as i32) as usize
            }
            None => {
                if delta >= 0 {
                    0
                } else {
                    zones.len() - 1
                }
            }
        };
        let target_zone = &zones[next_pos];
        self.snap_hwnd_to_zone_rect(fg, target_zone.index, target_zone.rect, cfg, trace_id)
    }

    /// Action 7: 将当前前台窗口精准停靠到 0-based 指定分区索引
    pub fn move_foreground_to_zone_index(
        &mut self,
        zone_index: usize,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> bool {
        let fg = resolve_foreground_or_target_hwnd();
        let excluded = cfg.combined_excluded_apps(&cfg.snap_zones.excluded_apps);
        if fg.0.is_null() || !is_manageable_window(fg, false, &excluded) {
            return false;
        }
        let monitors = enumerate_monitors();
        let Some(mon) = monitor_for_hwnd(fg, &monitors) else {
            return false;
        };
        let desktop_id = if cfg.snap_zones.per_virtual_desktop {
            resolve_window_or_current_desktop_id(fg)
        } else {
            None
        };
        let layout = cfg.resolve_layout_for_monitor(&mon, desktop_id.as_deref());
        if layout.layout_type == ZoneLayoutType::None {
            return false;
        }
        let gap = layout.effective_gap(cfg.snap_zones.zone_gap);
        let zones = layout.resolve(mon.work_rect, gap);
        let Some(rect) = resolve_zone_by_index(&zones, zone_index) else {
            return false;
        };
        self.snap_hwnd_to_zone_rect(fg, zone_index, rect, cfg, trace_id)
    }

    /// Action 8: 将当前前台窗口从分区还原至停靠前原始大小与坐标
    pub fn restore_foreground_from_zone(&mut self, cfg: &WindowKitConfig, trace_id: &str) -> bool {
        let fg = resolve_foreground_or_target_hwnd();
        let excluded = cfg.combined_excluded_apps(&cfg.snap_zones.excluded_apps);
        if fg.0.is_null() || !is_manageable_window(fg, false, &excluded) {
            return false;
        }
        let key = fg.0 as isize;
        self.current_zone_by_hwnd.remove(&key);
        let Some(orig) = self.pre_snap_rects.remove(&key) else {
            return false;
        };
        let ok = place_window_visual_rect(fg, orig);
        if ok {
            wk_info!(
                trace_id,
                "snapZones",
                "restored hwnd=0x{:x} to original rect ({},{},{},{})",
                key,
                orig.left,
                orig.top,
                orig.right,
                orig.bottom
            );
        }
        ok
    }

    /// Action 19: 在当前活动显示器上弹出实时 SnapZones Direct2D 预览层（带 110ms 柔滑淡入，lParam=0 立即关闭）
    pub fn show_preview_overlay(
        &mut self,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        show: bool,
        trace_id: &str,
    ) -> bool {
        if !show {
            self.hide_preview_overlay();
            return true;
        }
        let Some(g) = gfx else {
            return false;
        };
        let monitors = enumerate_monitors();
        let fg = resolve_foreground_or_target_hwnd();
        let mon = if !fg.0.is_null() {
            monitor_for_hwnd(fg, &monitors)
        } else {
            monitors.first().cloned()
        };
        let Some(mon) = mon else {
            return false;
        };
        let desktop_id = if cfg.snap_zones.per_virtual_desktop {
            resolve_window_or_current_desktop_id(fg)
        } else {
            None
        };
        let layout = cfg.resolve_layout_for_monitor(&mon, desktop_id.as_deref());
        if layout.layout_type == ZoneLayoutType::None {
            return false;
        }
        let gap = layout.effective_gap(cfg.snap_zones.zone_gap);
        let zones = layout.resolve(mon.work_rect, gap);
        if zones.is_empty() {
            return false;
        }
        let mut weights = vec![0.0f32; zones.len()];
        weights[0] = 1.0;
        // 仅渲染 1 次 4K 分层表面（初始 SourceConstantAlpha=56），后续 110ms 淡入 100% 走零重绘 ULW_ALPHA
        let ok = self.ensure_and_render_overlay(
            g,
            &mon,
            &zones,
            &[0],
            &weights,
            0.22,
            &layout.name,
            true,
            cfg,
        );
        if ok {
            self.preview_state = Some(PreviewAnimState {
                started_at: Instant::now(),
                done: false,
            });
            wk_debug!(
                trace_id,
                "snapZones",
                "showing preview overlay layout='{}' zones={}",
                layout.id,
                zones.len()
            );
        }
        ok
    }

    /// Action 21: 通过布局快捷数字键 (0..=9) 瞬切当前显示器(+虚拟桌面)的活动分屏布局
    pub fn apply_layout_by_hotkey_digit(
        &mut self,
        digit: u8,
        cfg: &mut WindowKitConfig,
        trace_id: &str,
    ) -> Option<(String, Option<String>, String)> {
        if digit > 9 {
            return None;
        }
        let layout_id = cfg
            .snap_zones
            .layouts
            .iter()
            .find(|l| l.hotkey_digit == Some(digit))
            .map(|l| l.id.clone())?;
        let monitors = enumerate_monitors();
        let fg = resolve_foreground_or_target_hwnd();
        let mon = if !fg.0.is_null() {
            monitor_for_hwnd(fg, &monitors)
        } else {
            monitors.first().cloned()
        }?;
        let desktop_id = if cfg.snap_zones.per_virtual_desktop {
            resolve_window_or_current_desktop_id(fg)
        } else {
            None
        };
        cfg.snap_zones.bind_monitor_layout(
            &mon.id,
            desktop_id.as_deref().unwrap_or(""),
            &layout_id,
        );
        wk_info!(
            trace_id,
            "snapZones",
            "switched monitor '{}' (desktop={:?}) to layout '{}' via hotkey digit {}",
            mon.id,
            desktop_id,
            layout_id,
            digit
        );
        Some((mon.id, desktop_id, layout_id))
    }

    /// 多屏拔插、旋转或虚拟桌面变更时自动自愈，避免遗留旧分辨率半屏叠加层
    pub fn on_topology_changed(&mut self, cfg: &WindowKitConfig, gfx: Option<&SharedGraphics>) {
        if let Some(drag) = self.drag.as_mut() {
            drag.monitors = enumerate_monitors();
            drag.active_monitor_id.clear();
            drag.active_zones.clear();
            drag.last_render_key = (String::new(), Vec::new());
        } else if self.preview_state.is_some() {
            let _ = self.show_preview_overlay(cfg, gfx, true, "topo");
        }
    }

    pub fn hide_preview_overlay(&mut self) {
        self.preview_state = None;
        if self.drag.is_none() {
            self.destroy_overlay_immediately();
        }
    }

    pub fn cancel_and_cleanup(&mut self) {
        self.drag = None;
        self.preview_state = None;
        self.destroy_overlay_immediately();
    }

    fn snap_hwnd_to_zone_rect(
        &mut self,
        hwnd: HWND,
        zone_index: usize,
        rect: RectI,
        cfg: &WindowKitConfig,
        trace_id: &str,
    ) -> bool {
        self.record_pre_snap_if_absent(hwnd);
        if place_window_visual_rect(hwnd, rect) {
            let key = hwnd.0 as isize;
            self.current_zone_by_hwnd.insert(key, zone_index);
            if cfg.snap_zones.remember_last_zone {
                let (_, proc_name, _) = get_window_process_info(hwnd);
                if !proc_name.is_empty() {
                    self.last_zone_by_proc
                        .insert(proc_name.to_ascii_lowercase(), zone_index);
                }
            }
            wk_info!(
                trace_id,
                "snapZones",
                "keyboard snapped hwnd=0x{:x} zone={} rect=({},{},{},{})",
                key,
                zone_index,
                rect.left,
                rect.top,
                rect.right,
                rect.bottom
            );
            true
        } else {
            false
        }
    }

    fn record_pre_snap_if_absent(&mut self, hwnd: HWND) {
        let key = hwnd.0 as isize;
        if !self.pre_snap_rects.contains_key(&key) {
            if let Some(vis) = get_window_visual_rect(hwnd) {
                self.pre_snap_rects.insert(key, vis);
            }
        }
    }

    fn hide_overlay_only(&mut self) {
        if let Some(d) = self.drag.as_mut() {
            d.overlay_visible = false;
            d.hovered_indices.clear();
            d.ctrl_anchor_zone = None;
            d.overlay_fade_started = None;
            d.last_render_key = (String::new(), Vec::new());
        }
        if let Some(oh) = self.overlay_hwnd {
            unsafe {
                if IsWindow(oh).as_bool() {
                    let _ = ShowWindow(oh, SW_HIDE);
                }
            }
        }
    }

    fn destroy_overlay_immediately(&mut self) {
        if let Some(oh) = self.overlay_hwnd.take() {
            unsafe {
                if IsWindow(oh).as_bool() {
                    let _ = ShowWindow(oh, SW_HIDE);
                    let _ = DestroyWindow(oh);
                }
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn ensure_and_render_overlay(
        &mut self,
        gfx: &SharedGraphics,
        mon: &MonitorInfo,
        zones: &[ResolvedZone],
        hovered: &[usize],
        zone_weights: &[f32],
        fade_alpha: f32,
        layout_name: &str,
        is_preview: bool,
        cfg: &WindowKitConfig,
    ) -> bool {
        if self.overlay_hwnd.is_none() {
            self.overlay_hwnd = create_snap_overlay_window();
        }
        let Some(oh) = self.overlay_hwnd else {
            return false;
        };

        let work = mon.work_rect;
        let w = work.width();
        let h = work.height();
        if w <= 0 || h <= 0 {
            return false;
        }

        let dark = cfg.is_dark_theme();
        let accent = parse_hex_color(&cfg.accent_color, [0.23, 0.51, 0.96]);
        let hi_alpha = cfg.snap_zones.highlight_opacity.clamp(0.12, 0.78);
        let scale = (mon.dpi as f32 / 96.0).max(1.0);
        let radius = 12.0 * scale;
        let show_numbers = cfg.snap_zones.show_zone_numbers;
        let source_alpha = (fade_alpha.clamp(0.10, 1.0) * 255.0).round() as u8;
        let master_a = 1.0f32;

        let num_fmt = gfx.ui_text_format(21.0 * scale, true, true).ok();
        let title_fmt = gfx.ui_text_format(13.0 * scale, true, true).ok();

        let merged_preview = if hovered.len() >= 2 {
            merge_zone_rects(zones, hovered)
        } else {
            None
        };

        let render_res =
            render_layered_window_d2d_alpha(gfx, oh, work.left, work.top, w, h, source_alpha, |rt| unsafe {
            // 对标 FancyZones 图 2 高通透克制美学：极浅磨砂白/暗玻璃 + 极细主题色描边，绝不盖脏底层真实应用内容
            let idle_rgb = if dark { [0.08, 0.11, 0.16] } else { [0.97, 0.98, 0.99] };
            let idle_base_a = if dark { 0.34 } else { 0.38 };
            let stroke_idle_rgb = accent;
            let badge_fill_col = if dark {
                rgba(0.085, 0.105, 0.155, 0.88 * master_a)
            } else {
                rgba(1.0, 1.0, 1.0, 0.94 * master_a)
            };
            let text_col = if dark {
                rgba(0.96, 0.98, 1.0, 0.94 * master_a)
            } else {
                rgba(0.10, 0.13, 0.19, 0.92 * master_a)
            };
            let white_col = rgba(1.0, 1.0, 1.0, 0.98 * master_a);

            let (
                Ok(b_badge_fill),
                Ok(b_text),
                Ok(b_white),
                Ok(b_hi_stroke),
                Ok(b_hi_glow),
            ) = (
                rt.CreateSolidColorBrush(&badge_fill_col, None),
                rt.CreateSolidColorBrush(&text_col, None),
                rt.CreateSolidColorBrush(&white_col, None),
                rt.CreateSolidColorBrush(&rgb_a(accent, 0.94 * master_a), None),
                rt.CreateSolidColorBrush(&rgb_a(accent, 0.24 * master_a), None),
            ) else {
                return;
            };

            for (slot, z) in zones.iter().enumerate() {
                let wt = zone_weights
                    .get(slot)
                    .copied()
                    .unwrap_or_else(|| if hovered.contains(&z.index) { 1.0 } else { 0.0 })
                    .clamp(0.0, 1.0);

                // 按权重在清透磨砂态与主题色高亮态之间丝滑插值
                let fill_col = rgba(
                    lerp_f32(idle_rgb[0], accent[0], wt),
                    lerp_f32(idle_rgb[1], accent[1], wt),
                    lerp_f32(idle_rgb[2], accent[2], wt),
                    lerp_f32(idle_base_a, hi_alpha.clamp(0.32, 0.56), wt) * master_a,
                );
                let stroke_col = rgba(
                    lerp_f32(stroke_idle_rgb[0], accent[0], wt),
                    lerp_f32(stroke_idle_rgb[1], accent[1], wt),
                    lerp_f32(stroke_idle_rgb[2], accent[2], wt),
                    lerp_f32(0.46, 0.96, wt) * master_a,
                );
                let stroke_w = lerp_f32(1.25, 2.4, wt) * scale;

                let lr = rect_f(
                    (z.rect.left - work.left) as f32 + 1.0,
                    (z.rect.top - work.top) as f32 + 1.0,
                    (z.rect.right - work.left) as f32 - 1.0,
                    (z.rect.bottom - work.top) as f32 - 1.0,
                );
                let rr = rounded_rect(lr, radius * 0.5);

                if let Ok(bf) = rt.CreateSolidColorBrush(&fill_col, None) {
                    rt.FillRoundedRectangle(&rr, &bf);
                }
                if wt > 0.15 {
                    if let Ok(bglow) = rt.CreateSolidColorBrush(&rgb_a(accent, 0.16 * wt * master_a), None) {
                        rt.DrawRoundedRectangle(&rr, &bglow, stroke_w + 2.6 * scale, None);
                    }
                }
                if let Ok(bs) = rt.CreateSolidColorBrush(&stroke_col, None) {
                    rt.DrawRoundedRectangle(&rr, &bs, stroke_w, None);
                }

                if show_numbers {
                    let cx = (lr.left + lr.right) * 0.5;
                    let cy = (lr.top + lr.bottom) * 0.5;
                    let box_r = lerp_f32(28.0, 34.0, wt) * scale;
                    let pr = rect_f(cx - box_r, cy - box_r, cx + box_r, cy + box_r);

                    // 复刻图 2 纯净无厚重外圈的数字居中排版，高亮时辅以极轻柔主题色光晕
                    if wt > 0.45 {
                        let halo_r = 22.0 * scale;
                        let hr = rect_f(cx - halo_r, cy - halo_r, cx + halo_r, cy + halo_r);
                        rt.FillRoundedRectangle(&rounded_rect(hr, halo_r), &b_hi_stroke);
                    }

                    if let Some(fmt) = &num_fmt {
                        let label = to_wide(&(z.index + 1).to_string());
                        let label_no_nul = &label[..label.len().saturating_sub(1)];
                        rt.DrawText(
                            label_no_nul,
                            fmt,
                            &pr,
                            if wt > 0.45 { &b_white } else { &b_text },
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                            windows::Win32::Graphics::DirectWrite::DWRITE_MEASURING_MODE_NATURAL,
                        );
                    }
                }
            }

            // 跨多区合并时额外绘制醒目统一辉光外框
            if let Some(merged) = merged_preview {
                let mr = rect_f(
                    (merged.left - work.left) as f32 + 2.0,
                    (merged.top - work.top) as f32 + 2.0,
                    (merged.right - work.left) as f32 - 2.0,
                    (merged.bottom - work.top) as f32 - 2.0,
                );
                let mrr = rounded_rect(mr, radius * 0.6);
                rt.DrawRoundedRectangle(&mrr, &b_hi_glow, 6.0 * scale, None);
                rt.DrawRoundedRectangle(&mrr, &b_hi_stroke, 2.8 * scale, None);
            }

            // 仅在跨区合并或主动预览时呈现克制顶部状态微胶囊，常规拖拽保持极致干净无干扰
            if (hovered.len() >= 2 || is_preview) && title_fmt.is_some() {
                if let Some(fmt) = &title_fmt {
                    let banner_text = if hovered.len() >= 2 {
                        let mut nums: Vec<usize> = hovered.iter().map(|i| i + 1).collect();
                        nums.sort_unstable();
                        let joined = nums
                            .iter()
                            .map(|n| n.to_string())
                            .collect::<Vec<_>>()
                            .join(" + ");
                        format!("{layout_name}  ·  {joined}")
                    } else {
                        format!("{layout_name}  ·  {} Zones", zones.len())
                    };
                    let bw = ((banner_text.chars().count() as f32 * 8.6 + 34.0) * scale)
                        .clamp(180.0 * scale, w as f32 * 0.62);
                    let bh = 30.0 * scale;
                    let bx = (w as f32 - bw) * 0.5;
                    let by = 14.0 * scale;
                    let br = rect_f(bx, by, bx + bw, by + bh);
                    let brr = rounded_rect(br, bh * 0.5);
                    rt.FillRoundedRectangle(&brr, &b_badge_fill);
                    rt.DrawRoundedRectangle(&brr, &b_hi_stroke, 1.3 * scale, None);

                    let wide = to_wide(&banner_text);
                    let slice = &wide[..wide.len().saturating_sub(1)];
                    rt.DrawText(
                        slice,
                        fmt,
                        &br,
                        &b_text,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        windows::Win32::Graphics::DirectWrite::DWRITE_MEASURING_MODE_NATURAL,
                    );
                }
            }
        });

        if render_res.is_err() {
            return false;
        }

        unsafe {
            let _ = SetWindowPos(oh, HWND_TOPMOST, work.left, work.top, w, h, SWP_NOACTIVATE);
            let _ = ShowWindow(oh, SW_SHOWNOACTIVATE);
        }
        true
    }
}

impl Drop for SnapZonesManager {
    fn drop(&mut self) {
        self.cancel_and_cleanup();
    }
}

fn is_key_down(vk: i32) -> bool {
    unsafe { (GetAsyncKeyState(vk) as u16 & 0x8000) != 0 }
}

fn ensure_snap_overlay_class() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(SNAP_ZONES_WINDOW_CLASS);
        let wc = WNDCLASSEXW {
            cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(snap_overlay_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&wc);
    });
}

fn create_snap_overlay_window() -> Option<HWND> {
    ensure_snap_overlay_class();
    unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(SNAP_ZONES_WINDOW_CLASS);
        let ex_style = WS_EX_LAYERED
            | WS_EX_TRANSPARENT
            | WS_EX_TOOLWINDOW
            | WS_EX_TOPMOST
            | WS_EX_NOACTIVATE;
        CreateWindowExW(
            ex_style,
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

unsafe extern "system" fn snap_overlay_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    DefWindowProcW(hwnd, msg, wparam, lparam)
}

#[allow(dead_code)]
fn _keep_layout_type_ref(_: &ZoneLayout) {}
