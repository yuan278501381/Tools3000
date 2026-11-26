// ─────────────────────────────────────────────────────────────────────────────
// core/geometry.rs — 纯数学窗口几何引擎（网格/画布分区求解、多区合并、
//                    边缘多区命中、DWM 隐形边框补偿、跨屏等比换算、离屏救援）
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::constants::SNAP_MULTI_ZONE_EDGE_THRESHOLD_PX;
use crate::core::json::JsonValue;

/// 物理像素矩形（左闭右开，与 Win32 RECT 完全一致）
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct RectI {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

impl RectI {
    pub const fn new(left: i32, top: i32, right: i32, bottom: i32) -> Self {
        Self {
            left,
            top,
            right,
            bottom,
        }
    }

    pub const fn from_xywh(x: i32, y: i32, w: i32, h: i32) -> Self {
        Self {
            left: x,
            top: y,
            right: x + w,
            bottom: y + h,
        }
    }

    pub const fn width(&self) -> i32 {
        self.right - self.left
    }

    pub const fn height(&self) -> i32 {
        self.bottom - self.top
    }

    pub const fn is_valid(&self) -> bool {
        self.right > self.left && self.bottom > self.top
    }

    pub const fn contains_point(&self, x: i32, y: i32) -> bool {
        x >= self.left && x < self.right && y >= self.top && y < self.bottom
    }

    pub fn intersect(&self, other: &RectI) -> Option<RectI> {
        let r = RectI {
            left: self.left.max(other.left),
            top: self.top.max(other.top),
            right: self.right.min(other.right),
            bottom: self.bottom.min(other.bottom),
        };
        if r.is_valid() {
            Some(r)
        } else {
            None
        }
    }

    pub fn union(&self, other: &RectI) -> RectI {
        RectI {
            left: self.left.min(other.left),
            top: self.top.min(other.top),
            right: self.right.max(other.right),
            bottom: self.bottom.max(other.bottom),
        }
    }

    pub fn area(&self) -> i64 {
        if !self.is_valid() {
            0
        } else {
            (self.width() as i64) * (self.height() as i64)
        }
    }

    pub fn to_json(&self) -> JsonValue {
        let mut obj = JsonValue::object();
        obj.insert("left", JsonValue::from_i32(self.left));
        obj.insert("top", JsonValue::from_i32(self.top));
        obj.insert("right", JsonValue::from_i32(self.right));
        obj.insert("bottom", JsonValue::from_i32(self.bottom));
        obj.insert("x", JsonValue::from_i32(self.left));
        obj.insert("y", JsonValue::from_i32(self.top));
        obj.insert("w", JsonValue::from_i32(self.width()));
        obj.insert("h", JsonValue::from_i32(self.height()));
        obj.insert("width", JsonValue::from_i32(self.width()));
        obj.insert("height", JsonValue::from_i32(self.height()));
        obj
    }

    pub fn from_json(v: &JsonValue) -> Option<Self> {
        let left = v.get("left").or_else(|| v.get("x"))?.as_i32()?;
        let top = v.get("top").or_else(|| v.get("y"))?.as_i32()?;
        let right = if let Some(r) = v.get("right").and_then(|x| x.as_i32()) {
            r
        } else {
            left + v.get("width").or_else(|| v.get("w"))?.as_i32()?
        };
        let bottom = if let Some(b) = v.get("bottom").and_then(|x| x.as_i32()) {
            b
        } else {
            top + v.get("height").or_else(|| v.get("h"))?.as_i32()?
        };
        Some(RectI {
            left,
            top,
            right,
            bottom,
        })
    }
}

/// 画布模式归一化分区（0.0..=1.0 相对显示器工作区比例）
#[derive(Debug, Clone, PartialEq)]
pub struct NormalizedZone {
    pub index: usize,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}

/// 求解后的具体物理像素分区
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResolvedZone {
    pub index: usize,
    pub rect: RectI,
}

/// 网格布局求解器（支持非均匀行列权重、单元格合并 cell_child_map 与统一间距 gap）
pub fn resolve_grid_zones(
    work_area: RectI,
    rows: usize,
    columns: usize,
    row_weights: &[f64],
    col_weights: &[f64],
    cell_child_map: &[Vec<usize>],
    gap: i32,
) -> Vec<ResolvedZone> {
    if !work_area.is_valid() || rows == 0 || columns == 0 {
        return Vec::new();
    }
    let gap = gap.max(0);
    let inner_left = work_area.left + gap;
    let inner_top = work_area.top + gap;
    let usable_w = (work_area.width() - gap * (columns as i32 + 1)).max(columns as i32);
    let usable_h = (work_area.height() - gap * (rows as i32 + 1)).max(rows as i32);

    let col_spans = split_weighted_spans(usable_w, columns, col_weights);
    let row_spans = split_weighted_spans(usable_h, rows, row_weights);

    // 计算每个格子独立像素边界
    let mut col_bounds = Vec::with_capacity(columns);
    let mut cx = inner_left;
    for w in col_spans {
        col_bounds.push((cx, cx + w));
        cx += w + gap;
    }

    let mut row_bounds = Vec::with_capacity(rows);
    let mut cy = inner_top;
    for h in row_spans {
        row_bounds.push((cy, cy + h));
        cy += h + gap;
    }

    // 根据 cell_child_map 聚合同一 zone index 所跨越的全部格子外接矩形
    let mut max_zone_idx: Option<usize> = None;
    let mut zone_rects: Vec<Option<RectI>> = Vec::new();

    for r in 0..rows {
        for c in 0..columns {
            let zone_idx = cell_child_map
                .get(r)
                .and_then(|row| row.get(c))
                .copied()
                .unwrap_or(r * columns + c);
            if zone_idx >= zone_rects.len() {
                zone_rects.resize(zone_idx + 1, None);
            }
            max_zone_idx = Some(max_zone_idx.map_or(zone_idx, |m| m.max(zone_idx)));

            let cell_rect = RectI::new(
                col_bounds[c].0,
                row_bounds[r].0,
                col_bounds[c].1,
                row_bounds[r].1,
            );
            zone_rects[zone_idx] = Some(match zone_rects[zone_idx] {
                Some(existing) => existing.union(&cell_rect),
                None => cell_rect,
            });
        }
    }

    let mut result = Vec::new();
    for (idx, maybe_rect) in zone_rects.into_iter().enumerate() {
        if let Some(rect) = maybe_rect {
            if rect.is_valid() {
                result.push(ResolvedZone { index: idx, rect });
            }
        }
    }
    result
}

/// 画布布局求解器（按 0..=1 归一化比例映射至工作区，并应用半间隙内缩以保障相邻分区间距一致）
pub fn resolve_canvas_zones(
    work_area: RectI,
    zones: &[NormalizedZone],
    gap: i32,
) -> Vec<ResolvedZone> {
    if !work_area.is_valid() {
        return Vec::new();
    }
    let gap = gap.max(0);
    let half_gap = gap / 2;
    let w = work_area.width() as f64;
    let h = work_area.height() as f64;

    let mut out = Vec::with_capacity(zones.len());
    for (seq, z) in zones.iter().enumerate() {
        let nx = z.x.clamp(0.0, 1.0);
        let ny = z.y.clamp(0.0, 1.0);
        let nw = z.width.clamp(0.01, 1.0 - nx);
        let nh = z.height.clamp(0.01, 1.0 - ny);

        let raw_left = work_area.left + (nx * w).round() as i32;
        let raw_top = work_area.top + (ny * h).round() as i32;
        let raw_right = work_area.left + ((nx + nw) * w).round() as i32;
        let raw_bottom = work_area.top + ((ny + nh) * h).round() as i32;

        // 贴边采用完整 gap，内部相接边各退 half_gap，保证相邻分区之间间距精确等于 gap
        let inset_l = if nx <= 0.001 { gap } else { half_gap };
        let inset_t = if ny <= 0.001 { gap } else { half_gap };
        let inset_r = if nx + nw >= 0.999 { gap } else { gap - half_gap };
        let inset_b = if ny + nh >= 0.999 { gap } else { gap - half_gap };

        let rect = RectI::new(
            (raw_left + inset_l).min(raw_right - 1),
            (raw_top + inset_t).min(raw_bottom - 1),
            (raw_right - inset_r).max(raw_left + 1),
            (raw_bottom - inset_b).max(raw_top + 1),
        );
        if rect.is_valid() {
            out.push(ResolvedZone {
                index: if z.index == 0 && seq > 0 { seq } else { z.index },
                rect,
            });
        }
    }
    out
}

/// 将多个分区索引合并为外接并集矩形（跨区合并停靠）
pub fn merge_zone_rects(zones: &[ResolvedZone], indices: &[usize]) -> Option<RectI> {
    let mut acc: Option<RectI> = None;
    for &idx in indices {
        if let Some(z) = zones.iter().find(|z| z.index == idx) {
            acc = Some(match acc {
                Some(r) => r.union(&z.rect),
                None => z.rect,
            });
        }
    }
    acc
}

/// 根据鼠标位置检测命中的分区集合：
/// - 正常情况返回包含光标的单个分区；
/// - 当 allow_multi_span 为真且光标落在多个分区之间的缝隙/边界附近（<= threshold px）时，自动合并返回所有邻接分区索引！
pub fn hit_test_zones(
    zones: &[ResolvedZone],
    cursor_x: i32,
    cursor_y: i32,
    allow_multi_span: bool,
    edge_threshold_px: i32,
) -> Vec<usize> {
    if zones.is_empty() {
        return Vec::new();
    }

    if allow_multi_span {
        let threshold = edge_threshold_px.max(4);
        let expanded_hits: Vec<usize> = zones
            .iter()
            .filter(|z| {
                cursor_x >= z.rect.left - threshold
                    && cursor_x <= z.rect.right + threshold
                    && cursor_y >= z.rect.top - threshold
                    && cursor_y <= z.rect.bottom + threshold
            })
            .map(|z| z.index)
            .collect();
        if expanded_hits.len() >= 2 {
            return expanded_hits;
        }
    }

    // 优先精确命中包含光标的最小面积分区（兼容画布重叠分区）
    let mut exact_hits: Vec<&ResolvedZone> = zones
        .iter()
        .filter(|z| z.rect.contains_point(cursor_x, cursor_y))
        .collect();
    if !exact_hits.is_empty() {
        exact_hits.sort_by_key(|z| z.rect.area());
        return vec![exact_hits[0].index];
    }

    // 若光标正好落在 gap 缝隙内，但 allow_multi_span 关闭，回退至中心距离最近的分区
    let mut best: Option<(i64, usize)> = None;
    for z in zones {
        let clamped_x = cursor_x.clamp(z.rect.left, z.rect.right);
        let clamped_y = cursor_y.clamp(z.rect.top, z.rect.bottom);
        let dx = (cursor_x - clamped_x) as i64;
        let dy = (cursor_y - clamped_y) as i64;
        let dist2 = dx * dx + dy * dy;
        let max_snap = (SNAP_MULTI_ZONE_EDGE_THRESHOLD_PX as i64) * (SNAP_MULTI_ZONE_EDGE_THRESHOLD_PX as i64) * 4;
        if dist2 <= max_snap {
            if best.map_or(true, |(b_d, _)| dist2 < b_d) {
                best = Some((dist2, z.index));
            }
        }
    }
    best.map(|(_, idx)| vec![idx]).unwrap_or_default()
}

/// DWM 隐形阴影边框精确补偿：
/// 已知目标可视区域 desired_visible、当前窗口外框 window_rect 与 DWM 可视边框 extended_frame_bounds，
/// 计算传给 SetWindowPos 的实际窗口矩形，实现零缝隙贴齐！
pub fn compensate_dwm_frame_bounds(
    desired_visible: RectI,
    window_rect: RectI,
    extended_frame_bounds: RectI,
) -> RectI {
    // 隐形边框在各方向的侵入量（Win10/11 通常左右下各约 7px*DPI，顶边 0px）
    let margin_left = (extended_frame_bounds.left - window_rect.left).clamp(-32, 64);
    let margin_top = (extended_frame_bounds.top - window_rect.top).clamp(-32, 64);
    let margin_right = (window_rect.right - extended_frame_bounds.right).clamp(-32, 64);
    let margin_bottom = (window_rect.bottom - extended_frame_bounds.bottom).clamp(-32, 64);

    RectI::new(
        desired_visible.left - margin_left,
        desired_visible.top - margin_top,
        desired_visible.right + margin_right,
        desired_visible.bottom + margin_bottom,
    )
}

/// 跨显示器等比迁移计算（妥善处理混合 DPI 与不同长宽比，结果强制约束在目标工作区内）
pub fn transform_rect_between_monitors(
    src_window: RectI,
    src_work: RectI,
    dst_work: RectI,
    keep_proportional: bool,
) -> RectI {
    if !src_work.is_valid() || !dst_work.is_valid() {
        return src_window;
    }

    let rel_x = (src_window.left - src_work.left) as f64 / (src_work.width() as f64);
    let rel_y = (src_window.top - src_work.top) as f64 / (src_work.height() as f64);

    let (new_w, new_h) = if keep_proportional {
        let rel_w = (src_window.width() as f64) / (src_work.width() as f64);
        let rel_h = (src_window.height() as f64) / (src_work.height() as f64);
        (
            ((rel_w * dst_work.width() as f64).round() as i32)
                .clamp(160.min(dst_work.width()), dst_work.width()),
            ((rel_h * dst_work.height() as f64).round() as i32)
                .clamp(100.min(dst_work.height()), dst_work.height()),
        )
    } else {
        (
            src_window.width().clamp(160.min(dst_work.width()), dst_work.width()),
            src_window.height().clamp(100.min(dst_work.height()), dst_work.height()),
        )
    };

    let raw_left = dst_work.left + (rel_x * dst_work.width() as f64).round() as i32;
    let raw_top = dst_work.top + (rel_y * dst_work.height() as f64).round() as i32;

    let clamped_left = raw_left.clamp(dst_work.left, (dst_work.right - new_w).max(dst_work.left));
    let clamped_top = raw_top.clamp(dst_work.top, (dst_work.bottom - new_h).max(dst_work.top));

    RectI::from_xywh(clamped_left, clamped_top, new_w, new_h)
}

/// 判定窗口是否严重离屏（标题栏不可抓或可见交集面积占比极低 / 落在已拔出显示器坐标上）
pub fn is_window_offscreen(win_rect: RectI, monitor_work_areas: &[RectI]) -> bool {
    if !win_rect.is_valid() || monitor_work_areas.is_empty() {
        return false;
    }
    let title_bar = RectI::new(
        win_rect.left,
        win_rect.top,
        win_rect.right,
        (win_rect.top + 36).min(win_rect.bottom),
    );
    let mut visible_area: i64 = 0;
    let mut title_reachable = false;

    for mon in monitor_work_areas {
        if let Some(inter) = win_rect.intersect(mon) {
            visible_area += inter.area();
        }
        if let Some(t_inter) = title_bar.intersect(mon) {
            if t_inter.width() >= 60 && t_inter.height() >= 12 {
                title_reachable = true;
            }
        }
    }

    let total_area = win_rect.area().max(1);
    !title_reachable || (visible_area * 100 / total_area) < 15
}

/// 将离屏窗口安全拉回并居中钳制到目标工作区
pub fn rescue_rect_into_work_area(win_rect: RectI, target_work: RectI) -> RectI {
    if !target_work.is_valid() {
        return win_rect;
    }
    let w = win_rect.width().clamp(240.min(target_work.width()), target_work.width());
    let h = win_rect.height().clamp(160.min(target_work.height()), target_work.height());
    let left = target_work.left + (target_work.width() - w) / 2;
    let top = target_work.top + (target_work.height() - h) / 2;
    RectI::from_xywh(left, top, w, h)
}

/// 根据工作区索引解析目标停靠矩形（供工作区启动器与键盘快捷键共用）
pub fn resolve_zone_by_index(zones: &[ResolvedZone], zone_index: usize) -> Option<RectI> {
    zones
        .iter()
        .find(|z| z.index == zone_index)
        .or_else(|| zones.get(zone_index))
        .map(|z| z.rect)
}

fn split_weighted_spans(total: i32, count: usize, weights: &[f64]) -> Vec<i32> {
    let mut eff_weights = Vec::with_capacity(count);
    for i in 0..count {
        let w = weights.get(i).copied().unwrap_or(1.0);
        eff_weights.push(if w.is_finite() && w > 0.0 { w } else { 1.0 });
    }
    let sum: f64 = eff_weights.iter().sum();
    let mut spans = Vec::with_capacity(count);
    let mut allocated = 0i32;
    for (i, w) in eff_weights.iter().enumerate() {
        if i + 1 == count {
            spans.push((total - allocated).max(1));
        } else {
            let v = ((total as f64) * (*w / sum)).round() as i32;
            let clamped = v.max(1);
            spans.push(clamped);
            allocated += clamped;
        }
    }
    spans
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolves_grid_with_merged_cells_and_gap() {
        let work = RectI::new(0, 0, 1920, 1040);
        // Priority grid: 左列 60% 合并上下两行，右列 40% 上下两格
        let zones = resolve_grid_zones(
            work,
            2,
            2,
            &[50.0, 50.0],
            &[60.0, 40.0],
            &[vec![0, 1], vec![0, 2]],
            10,
        );
        assert_eq!(zones.len(), 3);
        assert_eq!(zones[0].rect.left, 10);
        assert_eq!(zones[0].rect.top, 10);
        assert_eq!(zones[0].rect.bottom, 1030);
        assert_eq!(zones[1].rect.left, zones[0].rect.right + 10);
        assert_eq!(zones[2].rect.top, zones[1].rect.bottom + 10);
        assert_eq!(zones[2].rect.right, 1910);
    }

    #[test]
    fn resolves_canvas_and_merges_multi_zone_span() {
        let work = RectI::new(0, 0, 2000, 1000);
        let canvas = vec![
            NormalizedZone { index: 0, x: 0.0, y: 0.0, width: 0.5, height: 1.0 },
            NormalizedZone { index: 1, x: 0.5, y: 0.0, width: 0.5, height: 1.0 },
        ];
        let zones = resolve_canvas_zones(work, &canvas, 8);
        assert_eq!(zones.len(), 2);
        assert_eq!(zones[1].rect.left - zones[0].rect.right, 8);

        // 光标位于两区边界缝隙中点应同时命中 0 和 1
        let hits = hit_test_zones(&zones, 1000, 500, true, 24);
        assert_eq!(hits, vec![0, 1]);

        let merged = merge_zone_rects(&zones, &hits).expect("merge should produce rect");
        assert_eq!(merged.left, zones[0].rect.left);
        assert_eq!(merged.right, zones[1].rect.right);
    }

    #[test]
    fn compensates_dwm_invisible_shadow_border() {
        let desired = RectI::new(0, 0, 960, 1040);
        let win_rect = RectI::new(93, 100, 1007, 707);
        let ext_rect = RectI::new(100, 100, 1000, 700);
        let placed = compensate_dwm_frame_bounds(desired, win_rect, ext_rect);
        assert_eq!(placed, RectI::new(-7, 0, 967, 1047));
    }

    #[test]
    fn transforms_rect_across_mixed_dpi_monitors() {
        let src_work = RectI::new(0, 0, 1920, 1040);
        let dst_work = RectI::new(1920, 0, 5760, 2080); // 4K 副屏
        let win = RectI::new(192, 104, 1152, 624); // 10% 边距，50% 宽高
        let moved = transform_rect_between_monitors(win, src_work, dst_work, true);
        assert_eq!(moved.left, 1920 + 384);
        assert_eq!(moved.top, 208);
        assert_eq!(moved.width(), 1920);
        assert_eq!(moved.height(), 1040);
    }

    #[test]
    fn detects_and_rescues_offscreen_window() {
        let mon = RectI::new(0, 0, 1920, 1040);
        let lost = RectI::new(-3200, -100, -2000, 700);
        assert!(is_window_offscreen(lost, &[mon]));

        let rescued = rescue_rect_into_work_area(lost, mon);
        assert!(!is_window_offscreen(rescued, &[mon]));
        assert_eq!(rescued.width(), 1200);
        assert_eq!(rescued.height(), 800);
        assert_eq!(rescued.left, 360);
        assert_eq!(rescued.top, 120);
    }

    #[test]
    fn handles_negative_coordinate_secondary_monitor_and_dpi_transition() {
        // 左侧/上方负坐标副屏（例如 2560x1440 位于主屏左上方）
        let neg_mon = RectI::new(-2560, -360, 0, 1080);
        let zones = resolve_grid_zones(
            neg_mon,
            1,
            2,
            &[100.0],
            &[50.0, 50.0],
            &[vec![0, 1]],
            8,
        );
        assert_eq!(zones.len(), 2);
        assert_eq!(zones[0].rect.left, -2552);
        assert_eq!(zones[0].rect.top, -352);
        assert_eq!(zones[1].rect.right, -8);
        assert_eq!(zones[1].rect.bottom, 1072);

        // 光标落在负坐标副屏两区边界处应精准命中多区合并
        let hits = hit_test_zones(&zones, -1280, 360, true, 24);
        assert_eq!(hits, vec![0, 1]);

        // 从 1080p 主屏跨屏迁移到负坐标 1440p 副屏
        let primary = RectI::new(0, 0, 1920, 1040);
        let win = RectI::new(192, 104, 1152, 624);
        let moved = transform_rect_between_monitors(win, primary, neg_mon, true);
        assert!(moved.left >= neg_mon.left && moved.right <= neg_mon.right);
        assert!(moved.top >= neg_mon.top && moved.bottom <= neg_mon.bottom);
        assert!(!is_window_offscreen(moved, &[primary, neg_mon]));
    }
}
