// ─────────────────────────────────────────────────────────────────────────────
// core/config.rs — WindowKit Schema v3 配置模型（100% 向下兼容 v1/v2）、
//                  6 大 FancyZones 参数化模板引擎、4 级多屏/虚拟桌面/方向布局解析、
//                  Workspaces v2 组合场景契约、Quick Switch v2 自由窗口与同应用切窗契约
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::constants::{OPACITY_DEFAULT_STEP, OPACITY_MIN_PERCENT_FLOOR};
use crate::core::geometry::{
    resolve_canvas_zones, resolve_focus_zones, resolve_grid_zones, NormalizedZone, RectI,
    ResolvedZone,
};
use crate::core::json::{parse_json, JsonValue};
use crate::core::topology::{MonitorOrientation, NormalizedWorkRect};
use crate::core::win_utils::MonitorInfo;
use std::fs;
use std::path::Path;

/// 8 大核心特性独立开关（全部默认 false，严格遵守零占用默认关闭契约）
#[derive(Debug, Clone, Default)]
pub struct FeatureFlags {
    pub quick_switch: bool,
    pub snap_zones: bool,
    pub always_on_top: bool,
    pub opacity: bool,
    pub alt_drag: bool,
    pub focus_border: bool,
    pub multi_monitor: bool,
    pub workspaces: bool,
}

impl FeatureFlags {
    pub fn any_enabled(&self) -> bool {
        self.quick_switch
            || self.snap_zones
            || self.always_on_top
            || self.opacity
            || self.alt_drag
            || self.focus_border
            || self.multi_monitor
            || self.workspaces
    }

    pub fn active_names(&self) -> Vec<&'static str> {
        let mut names = Vec::new();
        if self.quick_switch {
            names.push("quickSwitch");
        }
        if self.snap_zones {
            names.push("snapZones");
        }
        if self.always_on_top {
            names.push("alwaysOnTop");
        }
        if self.opacity {
            names.push("opacity");
        }
        if self.alt_drag {
            names.push("altDrag");
        }
        if self.focus_border {
            names.push("focusBorder");
        }
        if self.multi_monitor {
            names.push("multiMonitor");
        }
        if self.workspaces {
            names.push("workspaces");
        }
        names
    }
}

/// Quick Switch v2 配置（双独立切窗轨道：全局 Alt+Tab 增强 + 同应用 Alt+` 切窗）
#[derive(Debug, Clone)]
pub struct QuickSwitchConfig {
    /// 是否接管全局 Alt+Tab（AltTabio 增强双栏切换器）
    pub replace_alt_tab: bool,
    /// 是否启用同应用窗口切换（对标并超越 PowerToys Window Hopper，默认 Alt+`，不碰原生 Alt+Tab）
    pub same_app_enabled: bool,
    pub same_app_hotkey: String,
    pub show_live_preview: bool,
    /// 桌面原位预览模式（按真实桌面比例投影 DWM 缩略图）
    pub preview_in_desktop_position: bool,
    pub show_all_monitors: bool,
    pub current_desktop_only: bool,
    pub show_process_badge: bool,
    pub show_app_names: bool,
    pub show_numbers: bool,
    pub compact_density: bool,
    pub large_icons: bool,
    pub visible_borders: bool,
    pub enable_pinyin_search: bool,
    pub include_minimized: bool,
    /// 标准窗口自由拖移、8 向缩放与位置记忆
    pub remember_bounds: bool,
    pub custom_width_dip: Option<f32>,
    pub custom_height_dip: Option<f32>,
    pub list_width_ratio: f32,
    pub custom_monitor_id: String,
    pub custom_rel_rect: Option<NormalizedWorkRect>,
}

impl Default for QuickSwitchConfig {
    fn default() -> Self {
        Self {
            replace_alt_tab: true,
            same_app_enabled: false,
            same_app_hotkey: "Alt+`".to_string(),
            show_live_preview: true,
            preview_in_desktop_position: false,
            show_all_monitors: true,
            current_desktop_only: true,
            show_process_badge: true,
            show_app_names: false,
            show_numbers: true,
            compact_density: false,
            large_icons: true,
            visible_borders: true,
            enable_pinyin_search: true,
            include_minimized: true,
            remember_bounds: true,
            custom_width_dip: None,
            custom_height_dip: None,
            list_width_ratio: 0.34,
            custom_monitor_id: String::new(),
            custom_rel_rect: None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SnapTriggerMode {
    Shift,
    RightClick,
    TopEdge,
    Always,
}

impl SnapTriggerMode {
    pub fn from_str_value(s: &str) -> Self {
        match s.trim().to_ascii_lowercase().as_str() {
            "rightclick" | "right_click" | "right-click" => SnapTriggerMode::RightClick,
            "topedge" | "top_edge" | "top-edge" | "edge" => SnapTriggerMode::TopEdge,
            "always" => SnapTriggerMode::Always,
            _ => SnapTriggerMode::Shift,
        }
    }
}

/// 分区布局渲染范式（完整对齐 PowerToys FancyZones：无布局 / 焦点层叠 / 网格 / 自由画布）
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ZoneLayoutType {
    None,
    Focus,
    Grid,
    Canvas,
}

/// 6 大内置参数化模板枚举（支持实时滑块调「区域数」）
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TemplateKind {
    None,
    Focus,
    Columns,
    Rows,
    Grid,
    PriorityGrid,
}

impl TemplateKind {
    #[allow(dead_code)]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::Focus => "focus",
            Self::Columns => "columns",
            Self::Rows => "rows",
            Self::Grid => "grid",
            Self::PriorityGrid => "priority_grid",
        }
    }

    pub fn from_str_opt(s: &str) -> Option<Self> {
        match s.trim().to_ascii_lowercase().as_str() {
            "none" | "no_layout" => Some(Self::None),
            "focus" => Some(Self::Focus),
            "columns" | "cols" => Some(Self::Columns),
            "rows" => Some(Self::Rows),
            "grid" => Some(Self::Grid),
            "priority_grid" | "priority-grid" | "prioritygrid" => Some(Self::PriorityGrid),
            _ => None,
        }
    }
}

#[derive(Debug, Clone)]
pub struct ZoneLayout {
    pub id: String,
    pub name: String,
    pub layout_type: ZoneLayoutType,
    pub built_in: bool,
    /// 内置参数化模板类型（自定义布局为 None）
    pub template_kind: Option<TemplateKind>,
    /// 参数化模板的区域数（1..=16，供实时滑块动态重算几何）
    pub zone_count: usize,
    /// 独立「区域周围的空间」开关（关闭时强制 0px 无缝拼接，对应 FancyZones 图 10/11）
    pub show_spacing: bool,
    /// 布局专属间距像素（None 时回退全局 zone_gap）
    pub gap: Option<i32>,
    /// 布局专属「突出显示距离」（交界处多区吸附感应距离，像素）
    pub highlight_distance: Option<i32>,
    /// 可选布局快捷键编号 0..=9（Win+Ctrl+Alt+数字 一键切换当前屏/桌面布局）
    pub hotkey_digit: Option<u8>,
    /// 设为水平监视器方向的默认布局
    pub default_horizontal: bool,
    /// 设为垂直监视器方向的默认布局
    pub default_vertical: bool,
    // Grid 参数
    pub rows: usize,
    pub columns: usize,
    pub row_weights: Vec<f64>,
    pub col_weights: Vec<f64>,
    pub cell_child_map: Vec<Vec<usize>>,
    // Canvas 参数
    pub zones: Vec<NormalizedZone>,
}

impl ZoneLayout {
    /// 创建标准网格布局（带完整安全默认值）
    pub fn new_grid(
        id: String,
        name: String,
        built_in: bool,
        rows: usize,
        columns: usize,
        row_weights: Vec<f64>,
        col_weights: Vec<f64>,
        cell_child_map: Vec<Vec<usize>>,
        gap: Option<i32>,
    ) -> Self {
        let max_z = cell_child_map
            .iter()
            .flat_map(|r| r.iter().copied())
            .max()
            .map(|m| m + 1)
            .unwrap_or((rows * columns).max(1));
        Self {
            id,
            name,
            layout_type: ZoneLayoutType::Grid,
            built_in,
            template_kind: None,
            zone_count: max_z,
            show_spacing: gap.map(|g| g > 0).unwrap_or(true),
            gap,
            highlight_distance: None,
            hotkey_digit: None,
            default_horizontal: false,
            default_vertical: false,
            rows: rows.max(1),
            columns: columns.max(1),
            row_weights,
            col_weights,
            cell_child_map,
            zones: Vec::new(),
        }
    }

    /// 创建标准画布布局（带完整安全默认值）
    pub fn new_canvas(
        id: String,
        name: String,
        built_in: bool,
        zones: Vec<NormalizedZone>,
        gap: Option<i32>,
    ) -> Self {
        let count = zones.len();
        Self {
            id,
            name,
            layout_type: ZoneLayoutType::Canvas,
            built_in,
            template_kind: None,
            zone_count: count,
            show_spacing: gap.map(|g| g > 0).unwrap_or(false),
            gap,
            highlight_distance: None,
            hotkey_digit: None,
            default_horizontal: false,
            default_vertical: false,
            rows: 0,
            columns: 0,
            row_weights: Vec::new(),
            col_weights: Vec::new(),
            cell_child_map: Vec::new(),
            zones,
        }
    }

    /// 根据模板枚举与区域数构造内置参数化模板（100% 对齐 PowerToys FancyZones）
    pub fn from_template(kind: TemplateKind, zone_count: usize, show_spacing: bool, gap: i32) -> Self {
        let count = if kind == TemplateKind::None {
            0
        } else {
            zone_count.clamp(1, 16)
        };
        let (id, name, layout_type) = match kind {
            TemplateKind::None => ("template_none", "No layout", ZoneLayoutType::None),
            TemplateKind::Focus => ("template_focus", "Focus", ZoneLayoutType::Focus),
            TemplateKind::Columns => ("template_columns", "Columns", ZoneLayoutType::Grid),
            TemplateKind::Rows => ("template_rows", "Rows", ZoneLayoutType::Grid),
            TemplateKind::Grid => ("template_grid", "Grid", ZoneLayoutType::Grid),
            TemplateKind::PriorityGrid => (
                "template_priority_grid",
                "Priority Grid",
                ZoneLayoutType::Grid,
            ),
        };
        let mut layout = Self {
            id: id.to_string(),
            name: name.to_string(),
            layout_type,
            built_in: true,
            template_kind: Some(kind),
            zone_count: count,
            show_spacing,
            gap: Some(gap.clamp(0, 64)),
            highlight_distance: Some(20),
            hotkey_digit: None,
            default_horizontal: kind == TemplateKind::PriorityGrid,
            default_vertical: kind == TemplateKind::Rows,
            rows: 1,
            columns: 1,
            row_weights: vec![100.0],
            col_weights: vec![100.0],
            cell_child_map: vec![vec![0]],
            zones: Vec::new(),
        };
        layout.rebuild_template_geometry();
        layout
    }

    /// 当模板的 zone_count 调整时，实时重建其行列权重与单元格合并映射
    pub fn rebuild_template_geometry(&mut self) {
        let Some(kind) = self.template_kind else {
            return;
        };
        match kind {
            TemplateKind::None => {
                self.layout_type = ZoneLayoutType::None;
                self.zone_count = 0;
                self.rows = 0;
                self.columns = 0;
                self.row_weights.clear();
                self.col_weights.clear();
                self.cell_child_map.clear();
                self.zones.clear();
            }
            TemplateKind::Focus => {
                self.layout_type = ZoneLayoutType::Focus;
                self.zone_count = self.zone_count.clamp(1, 16);
                self.rows = 0;
                self.columns = 0;
                self.row_weights.clear();
                self.col_weights.clear();
                self.cell_child_map.clear();
            }
            TemplateKind::Columns => {
                let n = self.zone_count.clamp(1, 16);
                self.layout_type = ZoneLayoutType::Grid;
                self.zone_count = n;
                self.rows = 1;
                self.columns = n;
                self.row_weights = vec![100.0];
                self.col_weights = vec![100.0 / (n as f64); n];
                self.cell_child_map = vec![(0..n).collect()];
            }
            TemplateKind::Rows => {
                let n = self.zone_count.clamp(1, 16);
                self.layout_type = ZoneLayoutType::Grid;
                self.zone_count = n;
                self.rows = n;
                self.columns = 1;
                self.row_weights = vec![100.0 / (n as f64); n];
                self.col_weights = vec![100.0];
                self.cell_child_map = (0..n).map(|r| vec![r]).collect();
            }
            TemplateKind::Grid => {
                let n = self.zone_count.clamp(1, 16);
                self.layout_type = ZoneLayoutType::Grid;
                self.zone_count = n;
                let (rows, cols, map) = build_balanced_grid(n);
                self.rows = rows;
                self.columns = cols;
                self.row_weights = vec![100.0 / (rows as f64); rows];
                self.col_weights = vec![100.0 / (cols as f64); cols];
                self.cell_child_map = map;
            }
            TemplateKind::PriorityGrid => {
                let n = self.zone_count.clamp(1, 16);
                self.layout_type = ZoneLayoutType::Grid;
                self.zone_count = n;
                let (rows, cols, rw, cw, map) = build_priority_grid(n);
                self.rows = rows;
                self.columns = cols;
                self.row_weights = rw;
                self.col_weights = cw;
                self.cell_child_map = map;
            }
        }
    }

    /// 克隆为可自由编辑的自定义布局（对应 FancyZones 模板「复制」按钮）
    pub fn clone_as_custom(&self, new_id: String, new_name: String) -> Self {
        let mut copy = self.clone();
        copy.id = new_id;
        copy.name = new_name;
        copy.built_in = false;
        copy.template_kind = None;
        copy.hotkey_digit = None;
        copy.default_horizontal = false;
        copy.default_vertical = false;
        // 若复制自「焦点」模板，自动转换为等价的自由画布区域便于逐个精调
        if copy.layout_type == ZoneLayoutType::Focus {
            let ref_work = RectI::new(0, 0, 2000, 1200);
            let resolved = resolve_focus_zones(ref_work, copy.zone_count.max(1), 16);
            copy.layout_type = ZoneLayoutType::Canvas;
            copy.zones = resolved
                .into_iter()
                .map(|z| NormalizedZone {
                    index: z.index,
                    x: (z.rect.left as f64 / 2000.0).clamp(0.0, 0.95),
                    y: (z.rect.top as f64 / 1200.0).clamp(0.0, 0.95),
                    width: (z.rect.width() as f64 / 2000.0).clamp(0.05, 1.0),
                    height: (z.rect.height() as f64 / 1200.0).clamp(0.05, 1.0),
                })
                .collect();
        } else if copy.layout_type == ZoneLayoutType::None {
            copy.layout_type = ZoneLayoutType::Grid;
            copy.rows = 1;
            copy.columns = 2;
            copy.row_weights = vec![100.0];
            copy.col_weights = vec![50.0, 50.0];
            copy.cell_child_map = vec![vec![0, 1]];
            copy.zone_count = 2;
        }
        copy
    }

    /// 结合「区域周围的空间」开关计算真实生效间距（关开关直接 0px 无缝贴合）
    pub fn effective_gap(&self, global_default_gap: i32) -> i32 {
        if !self.show_spacing {
            0
        } else {
            self.gap.unwrap_or(global_default_gap).clamp(0, 64)
        }
    }

    /// 布局级突出显示感应距离（像素）
    pub fn effective_highlight_distance(&self, global_edge_px: i32) -> i32 {
        self.highlight_distance
            .unwrap_or(global_edge_px)
            .clamp(4, 96)
    }

    pub fn resolve(&self, work_area: RectI, default_gap: i32) -> Vec<ResolvedZone> {
        let gap = self.effective_gap(default_gap);
        match self.layout_type {
            ZoneLayoutType::None => Vec::new(),
            ZoneLayoutType::Focus => resolve_focus_zones(work_area, self.zone_count.max(1), gap),
            ZoneLayoutType::Grid => resolve_grid_zones(
                work_area,
                self.rows,
                self.columns,
                &self.row_weights,
                &self.col_weights,
                &self.cell_child_map,
                gap,
            ),
            ZoneLayoutType::Canvas => resolve_canvas_zones(work_area, &self.zones, gap),
        }
    }

    pub fn to_json(&self) -> JsonValue {
        let mut obj = JsonValue::object();
        obj.insert("id", JsonValue::String(self.id.clone()));
        obj.insert("name", JsonValue::String(self.name.clone()));
        obj.insert(
            "type",
            JsonValue::String(match self.layout_type {
                ZoneLayoutType::None => "none".to_string(),
                ZoneLayoutType::Focus => "focus".to_string(),
                ZoneLayoutType::Grid => "grid".to_string(),
                ZoneLayoutType::Canvas => "canvas".to_string(),
            }),
        );
        obj.insert("builtIn", JsonValue::Bool(self.built_in));
        if let Some(tk) = self.template_kind {
            obj.insert("templateKind", JsonValue::String(tk.as_str().to_string()));
        }
        obj.insert("zoneCount", JsonValue::from_usize(self.zone_count));
        obj.insert("showSpacing", JsonValue::Bool(self.show_spacing));
        if let Some(g) = self.gap {
            obj.insert("gap", JsonValue::from_i32(g));
        }
        if let Some(hd) = self.highlight_distance {
            obj.insert("highlightDistance", JsonValue::from_i32(hd));
        }
        if let Some(d) = self.hotkey_digit {
            obj.insert("hotkeyDigit", JsonValue::from_u64(d as u64));
        }
        obj.insert("defaultHorizontal", JsonValue::Bool(self.default_horizontal));
        obj.insert("defaultVertical", JsonValue::Bool(self.default_vertical));
        match self.layout_type {
            ZoneLayoutType::None | ZoneLayoutType::Focus => {}
            ZoneLayoutType::Grid => {
                obj.insert("rows", JsonValue::from_usize(self.rows));
                obj.insert("columns", JsonValue::from_usize(self.columns));
                let mut rw = JsonValue::array();
                for &w in &self.row_weights {
                    rw.push(JsonValue::Number(w));
                }
                obj.insert("rowWeights", rw);
                let mut cw = JsonValue::array();
                for &w in &self.col_weights {
                    cw.push(JsonValue::Number(w));
                }
                obj.insert("colWeights", cw);
                let mut map = JsonValue::array();
                for row in &self.cell_child_map {
                    let mut r_arr = JsonValue::array();
                    for &cell in row {
                        r_arr.push(JsonValue::from_usize(cell));
                    }
                    map.push(r_arr);
                }
                obj.insert("cellChildMap", map);
            }
            ZoneLayoutType::Canvas => {
                let mut arr = JsonValue::array();
                for z in &self.zones {
                    let mut zo = JsonValue::object();
                    zo.insert("index", JsonValue::from_usize(z.index));
                    zo.insert("x", JsonValue::Number(z.x));
                    zo.insert("y", JsonValue::Number(z.y));
                    zo.insert("width", JsonValue::Number(z.width));
                    zo.insert("height", JsonValue::Number(z.height));
                    arr.push(zo);
                }
                obj.insert("zones", arr);
            }
        }
        obj
    }

    pub fn from_json(v: &JsonValue) -> Option<Self> {
        let id = v.get_str("id", "").trim().to_string();
        if id.is_empty() {
            return None;
        }
        let name = v.get_str("name", &id).to_string();
        let kind_raw = v
            .get("type")
            .or_else(|| v.get("layoutType"))
            .and_then(|x| x.as_str())
            .unwrap_or("grid");
        let built_in = v.get_bool("builtIn", v.get_bool("builtin", false));
        let template_kind = v
            .get("templateKind")
            .and_then(|x| x.as_str())
            .and_then(TemplateKind::from_str_opt)
            .or_else(|| infer_template_kind_from_id(&id));
        let gap = v
            .get("gap")
            .or_else(|| v.get("gapPx"))
            .and_then(|x| x.as_i32())
            .map(|g| g.clamp(0, 64));
        let show_spacing = v.get_bool("showSpacing", gap.map(|g| g > 0).unwrap_or(true));
        let highlight_distance = v
            .get("highlightDistance")
            .and_then(|x| x.as_i32())
            .map(|h| h.clamp(4, 96));
        let hotkey_digit = v
            .get("hotkeyDigit")
            .and_then(|x| x.as_i32())
            .and_then(|d| if (0..=9).contains(&d) { Some(d as u8) } else { None });
        let default_horizontal = v.get_bool("defaultHorizontal", false);
        let default_vertical = v.get_bool("defaultVertical", false);

        if kind_raw.eq_ignore_ascii_case("none") {
            return Some(Self {
                id,
                name,
                layout_type: ZoneLayoutType::None,
                built_in,
                template_kind: template_kind.or(Some(TemplateKind::None)),
                zone_count: 0,
                show_spacing: false,
                gap,
                highlight_distance,
                hotkey_digit,
                default_horizontal,
                default_vertical,
                rows: 0,
                columns: 0,
                row_weights: Vec::new(),
                col_weights: Vec::new(),
                cell_child_map: Vec::new(),
                zones: Vec::new(),
            });
        }

        if kind_raw.eq_ignore_ascii_case("focus") {
            let count = v.get("zoneCount").and_then(|x| x.as_usize()).unwrap_or(3).clamp(1, 16);
            return Some(Self {
                id,
                name,
                layout_type: ZoneLayoutType::Focus,
                built_in,
                template_kind: template_kind.or(Some(TemplateKind::Focus)),
                zone_count: count,
                show_spacing,
                gap,
                highlight_distance,
                hotkey_digit,
                default_horizontal,
                default_vertical,
                rows: 0,
                columns: 0,
                row_weights: Vec::new(),
                col_weights: Vec::new(),
                cell_child_map: Vec::new(),
                zones: Vec::new(),
            });
        }

        // 兼容宿主与 UI 传入的归一化 zones 数组格式（含 {x,y,w,h} 与 {x,y,width,height}）
        if let Some(arr) = v.get("zones").and_then(|x| x.as_array()) {
            let mut zones = Vec::new();
            for (seq, item) in arr.iter().enumerate() {
                let mut x = item.get_f64("x", 0.0);
                let mut y = item.get_f64("y", 0.0);
                let mut width = item
                    .get("width")
                    .or_else(|| item.get("w"))
                    .and_then(|n| n.as_f64())
                    .unwrap_or(0.5);
                let mut height = item
                    .get("height")
                    .or_else(|| item.get("h"))
                    .and_then(|n| n.as_f64())
                    .unwrap_or(1.0);
                if width > 1.0 || height > 1.0 || x > 1.0 || y > 1.0 {
                    x /= 100.0;
                    y /= 100.0;
                    width /= 100.0;
                    height /= 100.0;
                }
                let index = item
                    .get("index")
                    .or_else(|| item.get("id"))
                    .and_then(|n| n.as_usize())
                    .unwrap_or(seq);
                zones.push(NormalizedZone {
                    index,
                    x: x.clamp(0.0, 0.99),
                    y: y.clamp(0.0, 0.99),
                    width: width.clamp(0.01, 1.0),
                    height: height.clamp(0.01, 1.0),
                });
            }
            if !zones.is_empty() {
                let zc = zones.len();
                return Some(Self {
                    id,
                    name,
                    layout_type: ZoneLayoutType::Canvas,
                    built_in,
                    template_kind,
                    zone_count: zc,
                    show_spacing,
                    gap,
                    highlight_distance,
                    hotkey_digit,
                    default_horizontal,
                    default_vertical,
                    rows: 0,
                    columns: 0,
                    row_weights: Vec::new(),
                    col_weights: Vec::new(),
                    cell_child_map: Vec::new(),
                    zones,
                });
            }
        }

        if kind_raw.eq_ignore_ascii_case("canvas") {
            return None;
        }

        let rows = v.get("rows").and_then(|x| x.as_usize()).unwrap_or(1).max(1);
        let columns = v.get("columns").and_then(|x| x.as_usize()).unwrap_or(2).max(1);
        let row_weights = parse_f64_array(
            v.get("rowWeights").or_else(|| v.get("rowPercents")),
            rows,
        );
        let col_weights = parse_f64_array(
            v.get("colWeights").or_else(|| v.get("colPercents")),
            columns,
        );
        let cell_child_map = parse_cell_map(
            v.get("cellChildMap").or_else(|| v.get("cellMap")),
            rows,
            columns,
        );
        let computed_count = cell_child_map
            .iter()
            .flat_map(|r| r.iter().copied())
            .max()
            .map(|m| m + 1)
            .unwrap_or(rows * columns);
        let zone_count = v
            .get("zoneCount")
            .and_then(|x| x.as_usize())
            .unwrap_or(computed_count)
            .clamp(1, 32);

        Some(Self {
            id,
            name,
            layout_type: ZoneLayoutType::Grid,
            built_in,
            template_kind,
            zone_count,
            show_spacing,
            gap,
            highlight_distance,
            hotkey_digit,
            default_horizontal,
            default_vertical,
            rows,
            columns,
            row_weights,
            col_weights,
            cell_child_map,
            zones: Vec::new(),
        })
    }
}

fn infer_template_kind_from_id(id: &str) -> Option<TemplateKind> {
    match id.trim().to_ascii_lowercase().as_str() {
        "template_none" | "builtin-none" => Some(TemplateKind::None),
        "template_focus" | "builtin-focus" => Some(TemplateKind::Focus),
        "template_columns" | "builtin-columns-2" | "preset_columns_2" | "preset_columns_3" => {
            Some(TemplateKind::Columns)
        }
        "template_rows" | "preset_vertical_3" => Some(TemplateKind::Rows),
        "template_grid" | "builtin-grid-2x2" | "preset_quad" => Some(TemplateKind::Grid),
        "template_priority_grid" | "builtin-priority-3" | "preset_ultrawide_121"
        | "preset_priority_grid" | "builtin-focus-split" => Some(TemplateKind::PriorityGrid),
        _ => None,
    }
}

/// 生成最接近正方形的 N 格 Grid 切分（如 3->1x3, 4->2x2, 5->2x3 合并右下, 6->2x3, 9->3x3）
fn build_balanced_grid(n: usize) -> (usize, usize, Vec<Vec<usize>>) {
    let n = n.clamp(1, 16);
    if n <= 3 {
        return (1, n, vec![(0..n).collect()]);
    }
    let cols = (n as f64).sqrt().ceil() as usize;
    let rows = n.div_ceil(cols);
    let mut map = vec![vec![0usize; cols]; rows];
    let mut next_zone = 0usize;
    for r in 0..rows {
        let is_last = r + 1 == rows;
        let remaining = n - next_zone;
        if is_last && remaining < cols {
            // 末行不足 cols 个时，将最后格子向右铺满，避免产生空洞
            for c in 0..cols {
                let z = if c < remaining {
                    next_zone + c
                } else {
                    n - 1
                };
                map[r][c] = z;
            }
            next_zone = n;
        } else {
            for c in 0..cols {
                map[r][c] = next_zone;
                next_zone += 1;
            }
        }
    }
    (rows, cols, map)
}

/// 生成 PowerToys FancyZones 经典“优先级”网格（中央大主栏 + 两侧辅栏自动堆叠）
fn build_priority_grid(n: usize) -> (usize, usize, Vec<f64>, Vec<f64>, Vec<Vec<usize>>) {
    match n.clamp(1, 16) {
        1 => (1, 1, vec![100.0], vec![100.0], vec![vec![0]]),
        2 => (1, 2, vec![100.0], vec![66.667, 33.333], vec![vec![0, 1]]),
        3 => (
            1,
            3,
            vec![100.0],
            vec![25.0, 50.0, 25.0],
            vec![vec![0, 1, 2]],
        ),
        4 => (
            2,
            3,
            vec![50.0, 50.0],
            vec![25.0, 50.0, 25.0],
            vec![vec![0, 1, 2], vec![0, 1, 3]],
        ),
        5 => (
            2,
            3,
            vec![50.0, 50.0],
            vec![25.0, 50.0, 25.0],
            vec![vec![0, 1, 2], vec![3, 1, 4]],
        ),
        other => {
            // 6+ 格：保持中央 50% 主工作区独占通高，左右各均分剩余槽位
            let side_total = other - 1;
            let right_rows = side_total.div_ceil(2);
            let left_rows = side_total - right_rows;
            let rows = right_rows.max(left_rows).max(2);
            let mut map = vec![vec![0usize; 3]; rows];
            // 中间列固定为 1 号主区
            for row in &mut map {
                row[1] = 1;
            }
            // 左列分配区号 0 及后续
            let mut next_id = 0usize;
            for r in 0..rows {
                if r < left_rows {
                    let id = if next_id == 1 {
                        next_id += 1;
                        next_id
                    } else {
                        next_id
                    };
                    map[r][0] = id;
                    next_id = id + 1;
                } else {
                    map[r][0] = map[r.saturating_sub(1)][0];
                }
            }
            // 右列分配剩余区号
            for r in 0..rows {
                if r < right_rows {
                    let id = if next_id == 1 {
                        next_id += 1;
                        next_id
                    } else {
                        next_id
                    };
                    map[r][2] = id;
                    next_id = id + 1;
                } else {
                    map[r][2] = map[r.saturating_sub(1)][2];
                }
            }
            (
                rows,
                3,
                vec![100.0 / (rows as f64); rows],
                vec![25.0, 50.0, 25.0],
                map,
            )
        }
    }
}

#[derive(Debug, Clone)]
pub struct MonitorLayoutBinding {
    pub monitor_id: String,
    pub desktop_id: String,
    pub layout_id: String,
}

#[derive(Debug, Clone)]
pub struct SnapZonesConfig {
    pub trigger_mode: SnapTriggerMode,
    pub edge_threshold_px: i32,
    pub zone_gap: i32,
    pub highlight_opacity: f32,
    pub show_zone_numbers: bool,
    pub allow_multi_zone_span: bool,
    pub remember_last_zone: bool,
    pub restore_original_size: bool,
    pub per_virtual_desktop: bool,
    pub show_on_all_monitors: bool,
    pub override_win_arrows: bool,
    pub move_by_position: bool,
    pub active_layout_id: String,
    pub excluded_apps: Vec<String>,
    pub monitor_bindings: Vec<MonitorLayoutBinding>,
    pub layouts: Vec<ZoneLayout>,
}

impl SnapZonesConfig {
    /// 统一绑定或更新指定显示器（+可选虚拟桌面）的活动布局 ID（单一事实源）
    pub fn bind_monitor_layout(&mut self, monitor_id: &str, desktop_id: &str, layout_id: &str) {
        let mid = monitor_id.trim();
        let did = desktop_id.trim();
        if mid.is_empty() {
            self.active_layout_id = layout_id.to_string();
            return;
        }
        if let Some(existing) = self.monitor_bindings.iter_mut().find(|b| {
            b.monitor_id.eq_ignore_ascii_case(mid) && b.desktop_id.eq_ignore_ascii_case(did)
        }) {
            existing.layout_id = layout_id.to_string();
        } else {
            self.monitor_bindings.push(MonitorLayoutBinding {
                monitor_id: mid.to_string(),
                desktop_id: did.to_string(),
                layout_id: layout_id.to_string(),
            });
        }
    }
}

impl Default for SnapZonesConfig {
    fn default() -> Self {
        Self {
            trigger_mode: SnapTriggerMode::Shift,
            edge_threshold_px: 20,
            zone_gap: 16,
            highlight_opacity: 0.30,
            show_zone_numbers: true,
            allow_multi_zone_span: true,
            remember_last_zone: true,
            restore_original_size: true,
            per_virtual_desktop: false,
            show_on_all_monitors: false,
            override_win_arrows: false,
            move_by_position: false,
            active_layout_id: "template_priority_grid".to_string(),
            excluded_apps: Vec::new(),
            monitor_bindings: Vec::new(),
            layouts: builtin_zone_presets(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct AlwaysOnTopConfig {
    pub show_border: bool,
    pub border_color: String,
    pub border_width: i32,
}

impl Default for AlwaysOnTopConfig {
    fn default() -> Self {
        Self {
            show_border: true,
            border_color: "#F59E0B".to_string(),
            border_width: 2,
        }
    }
}

#[derive(Debug, Clone)]
pub struct OpacityConfig {
    pub step_percent: u8,
    pub min_percent: u8,
    pub alt_wheel_enabled: bool,
}

impl Default for OpacityConfig {
    fn default() -> Self {
        Self {
            step_percent: OPACITY_DEFAULT_STEP,
            min_percent: 30,
            alt_wheel_enabled: true,
        }
    }
}

#[derive(Debug, Clone)]
pub struct AltDragConfig {
    pub enable_move: bool,
    pub enable_resize: bool,
    pub bring_to_front: bool,
    pub require_ctrl: bool,
    pub snap_to_zones: bool,
    pub excluded_apps: Vec<String>,
}

impl Default for AltDragConfig {
    fn default() -> Self {
        Self {
            enable_move: true,
            enable_resize: true,
            bring_to_front: true,
            require_ctrl: false,
            snap_to_zones: true,
            excluded_apps: Vec::new(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct FocusBorderConfig {
    pub border_color: String,
    pub border_width: i32,
    pub corner_radius: i32,
    pub opacity: f32,
}

impl Default for FocusBorderConfig {
    fn default() -> Self {
        Self {
            border_color: String::new(),
            border_width: 2,
            corner_radius: 8,
            opacity: 0.90,
        }
    }
}

#[derive(Debug, Clone)]
pub struct MultiMonitorConfig {
    pub keep_proportional_size: bool,
    pub center_new_windows: bool,
    pub enable_tray_minimize: bool,
}

impl Default for MultiMonitorConfig {
    fn default() -> Self {
        Self {
            keep_proportional_size: true,
            center_new_windows: false,
            enable_tray_minimize: true,
        }
    }
}

/// 启动工作区时对无关窗口的三态处置策略（组合超越点：支持一键收纳至系统托盘）
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum UnrelatedWindowAction {
    #[default]
    Keep,
    Minimize,
    MinimizeToTray,
}

impl UnrelatedWindowAction {
    pub fn from_str_value(s: &str) -> Self {
        match s.trim().to_ascii_lowercase().as_str() {
            "minimize" | "min" => Self::Minimize,
            "tray" | "minimize_to_tray" | "minimizetotray" => Self::MinimizeToTray,
            _ => Self::Keep,
        }
    }

    #[allow(dead_code)]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Keep => "keep",
            Self::Minimize => "minimize",
            Self::MinimizeToTray => "minimize_to_tray",
        }
    }
}

#[derive(Debug, Clone)]
pub struct WorkspaceAppEntry {
    pub exe_path: String,
    pub args: String,
    pub process_name: String,
    #[allow(dead_code)]
    pub app_name: String,
    pub title_hint: String,
    pub aumid: String,
    pub monitor_id: String,
    pub monitor_friendly: String,
    pub monitor_orientation: Option<MonitorOrientation>,
    pub monitor_index: Option<usize>,
    /// 单区或多区合并槽位绑定（换分辨率/拔副屏 100% 严丝合缝自适应！）
    pub zone_index: Option<usize>,
    pub zone_indices: Vec<usize>,
    /// 相对所在显示器工作区的归一化比例（未绑定分区槽位时的跨分辨率无损自适应坐标）
    pub rel_rect: Option<NormalizedWorkRect>,
    pub rect: Option<RectI>,
    pub maximized: bool,
    pub minimized: bool,
    /// 精准区分以管理员身份启动 vs 通过 Explorer Shell 降权普通启动
    pub run_as_admin: bool,
    /// 组合场景态：启动后自动置顶与自动设定窗口透明度
    pub always_on_top: bool,
    pub opacity_percent: Option<u8>,
}

#[derive(Debug, Clone)]
pub struct WorkspaceProfile {
    pub id: String,
    pub name: String,
    pub bound_layout_id: String,
    #[allow(dead_code)]
    pub desktop_id: String,
    #[allow(dead_code)]
    pub desktop_name: String,
    pub move_existing_windows: bool,
    pub minimize_unrelated: Option<bool>,
    pub unrelated_action: Option<UnrelatedWindowAction>,
    #[allow(dead_code)]
    pub create_desktop_shortcut: bool,
    #[allow(dead_code)]
    pub last_launched_at: Option<u64>,
    pub apps: Vec<WorkspaceAppEntry>,
}

#[derive(Debug, Clone, Default)]
pub struct WorkspacesConfig {
    pub minimize_unrelated: bool,
    pub unrelated_action: UnrelatedWindowAction,
    pub items: Vec<WorkspaceProfile>,
}

/// 顶层完整配置根对象
#[derive(Debug, Clone)]
pub struct WindowKitConfig {
    pub schema_version: u32,
    pub enabled: bool,
    pub theme: String,
    pub accent_color: String,
    pub ui_lang: String,
    pub log_dir: String,
    pub log_level: String,
    pub fullscreen_muted: bool,
    pub remote_muted: bool,
    pub excluded_apps: Vec<String>,
    pub features: FeatureFlags,
    pub quick_switch: QuickSwitchConfig,
    pub snap_zones: SnapZonesConfig,
    pub always_on_top: AlwaysOnTopConfig,
    pub opacity: OpacityConfig,
    pub alt_drag: AltDragConfig,
    pub focus_border: FocusBorderConfig,
    pub multi_monitor: MultiMonitorConfig,
    pub workspaces: WorkspacesConfig,
}

impl Default for WindowKitConfig {
    fn default() -> Self {
        Self {
            schema_version: 3,
            enabled: false,
            theme: "dark".to_string(),
            accent_color: "#3B82F6".to_string(),
            ui_lang: "zh".to_string(),
            log_dir: String::new(),
            // TODO(pre-release): 待缺陷收敛进入正式发版周期前，切回 "info"
            log_level: "trace".to_string(),
            fullscreen_muted: false,
            remote_muted: false,
            excluded_apps: Vec::new(),
            features: FeatureFlags::default(),
            quick_switch: QuickSwitchConfig::default(),
            snap_zones: SnapZonesConfig::default(),
            always_on_top: AlwaysOnTopConfig::default(),
            opacity: OpacityConfig::default(),
            alt_drag: AltDragConfig::default(),
            focus_border: FocusBorderConfig::default(),
            multi_monitor: MultiMonitorConfig::default(),
            workspaces: WorkspacesConfig::default(),
        }
    }
}

impl WindowKitConfig {
    /// 是否应该保持常驻（主开关开启且至少有 1 项特性启用）
    pub fn should_stay_resident(&self) -> bool {
        self.enabled && self.features.any_enabled()
    }

    pub fn is_dark_theme(&self) -> bool {
        if self.theme.eq_ignore_ascii_case("light") {
            false
        } else if self.theme.eq_ignore_ascii_case("system") {
            crate::core::win_utils::is_system_dark_mode()
        } else {
            true
        }
    }

    /// 合并全局排除应用列表与模块专属排除应用列表
    pub fn combined_excluded_apps(&self, extra: &[String]) -> Vec<String> {
        let mut all = self.excluded_apps.clone();
        for item in extra {
            if !all.iter().any(|x| x.eq_ignore_ascii_case(item)) {
                all.push(item.clone());
            }
        }
        all
    }

    /// 4 级确定性布局解析链（支持虚拟桌面、显示器专属、横竖屏方向默认与全局兜底）
    pub fn resolve_layout_for_monitor(
        &self,
        mon: &MonitorInfo,
        desktop_id: Option<&str>,
    ) -> &ZoneLayout {
        let orientation = MonitorOrientation::from_rect(mon.physical_rect);
        self.layout_for_monitor_with_orientation(&mon.id, desktop_id, Some(orientation))
    }

    /// 兼容旧签名（不带方向时仅按 monitor + desktop + active_layout_id 解析）
    #[allow(dead_code)]
    pub fn layout_for_monitor(&self, monitor_id: &str, desktop_id: Option<&str>) -> &ZoneLayout {
        self.layout_for_monitor_with_orientation(monitor_id, desktop_id, None)
    }

    pub fn layout_for_monitor_with_orientation(
        &self,
        monitor_id: &str,
        desktop_id: Option<&str>,
        orientation: Option<MonitorOrientation>,
    ) -> &ZoneLayout {
        // 1. 若开启按虚拟桌面绑定，优先精确匹配 monitor_id + desktop_id
        if self.snap_zones.per_virtual_desktop {
            if let Some(did) = desktop_id {
                if let Some(binding) = self.snap_zones.monitor_bindings.iter().find(|b| {
                    b.monitor_id.eq_ignore_ascii_case(monitor_id)
                        && !b.desktop_id.is_empty()
                        && b.desktop_id.eq_ignore_ascii_case(did)
                }) {
                    if let Some(layout) = self.find_layout(&binding.layout_id) {
                        return layout;
                    }
                }
            }
        }
        // 2. 匹配按显示器绑定（不限虚拟桌面）
        if let Some(binding) = self.snap_zones.monitor_bindings.iter().find(|b| {
            b.monitor_id.eq_ignore_ascii_case(monitor_id) && b.desktop_id.is_empty()
        }) {
            if let Some(layout) = self.find_layout(&binding.layout_id) {
                return layout;
            }
        }
        // 3. 按显示器实际横/竖屏方向自动匹配默认布局星标
        if let Some(ori) = orientation {
            if let Some(layout) = self.snap_zones.layouts.iter().find(|l| match ori {
                MonitorOrientation::Landscape => l.default_horizontal,
                MonitorOrientation::Portrait => l.default_vertical,
            }) {
                return layout;
            }
        }
        // 4. 回退全局 active_layout_id -> 首个非 None 模板 -> 首个元素
        self.find_layout(&self.snap_zones.active_layout_id)
            .or_else(|| {
                self.snap_zones
                    .layouts
                    .iter()
                    .find(|l| l.layout_type != ZoneLayoutType::None)
            })
            .unwrap_or(&self.snap_zones.layouts[0])
    }

    pub fn find_layout(&self, id: &str) -> Option<&ZoneLayout> {
        self.snap_zones.layouts.iter().find(|l| l.id == id)
    }

    /// 根据 0..=9 布局快捷键编号查找对应布局
    #[allow(dead_code)]
    pub fn find_layout_by_hotkey_digit(&self, digit: u8) -> Option<&ZoneLayout> {
        self.snap_zones
            .layouts
            .iter()
            .find(|l| l.hotkey_digit == Some(digit))
    }

    /// 从磁盘配置文件加载（失败时安全返回默认关闭配置并记录原因）
    pub fn load_from_file(path: &Path) -> Result<Self, String> {
        let raw = fs::read_to_string(path).map_err(|e| format!("failed to read config: {e}"))?;
        Self::from_json_str(&raw)
    }

    pub fn from_json_str(raw: &str) -> Result<Self, String> {
        let root = parse_json(raw).map_err(|e| e.to_string())?;
        let mut cfg = Self::default();
        cfg.schema_version = root.get_i32("schemaVersion", 0).max(0) as u32;
        cfg.enabled = root.get_bool("enabled", false);
        cfg.theme = root.get_str("theme", "dark").to_string();
        cfg.accent_color = root.get_str("accentColor", "#3B82F6").to_string();
        cfg.ui_lang = root.get_str("uiLang", "zh").to_string();
        cfg.log_dir = root.get_str("logDir", "").to_string();
        cfg.log_level = root.get_str("logLevel", "trace").to_string();
        cfg.fullscreen_muted = root.get_bool("fullscreenMuted", false);
        cfg.remote_muted = root.get_bool("remoteMuted", false);

        if let Some(arr) = root.get("excludedApps").and_then(|v| v.as_array()) {
            cfg.excluded_apps = parse_lowercase_str_list(arr);
        }

        if let Some(f) = root.get("features") {
            cfg.features.quick_switch =
                f.get_bool("quickSwitch", false) || f.get_bool("altTab", false);
            cfg.features.snap_zones =
                f.get_bool("snapZones", false) || f.get_bool("zones", false);
            cfg.features.always_on_top = f.get_bool("alwaysOnTop", false);
            cfg.features.opacity = f.get_bool("opacity", false);
            cfg.features.alt_drag = f.get_bool("altDrag", false);
            cfg.features.focus_border =
                f.get_bool("focusBorder", false) || f.get_bool("activeBorder", false);
            cfg.features.multi_monitor =
                f.get_bool("multiMonitor", false) || f.get_bool("multiMonitorUtils", false);
            cfg.features.workspaces = f.get_bool("workspaces", false);
        }

        if let Some(qs) = root.get("quickSwitch").or_else(|| root.get("altTab")) {
            cfg.quick_switch.replace_alt_tab = qs.get_bool("replaceAltTab", true);
            cfg.quick_switch.same_app_enabled = qs.get_bool("sameAppEnabled", false);
            cfg.quick_switch.same_app_hotkey = qs.get_str("sameAppHotkey", "Alt+`").to_string();
            cfg.quick_switch.show_live_preview = qs
                .get("showLivePreview")
                .or_else(|| qs.get("showPreview"))
                .and_then(|v| v.as_bool())
                .unwrap_or(true);
            cfg.quick_switch.preview_in_desktop_position =
                qs.get_bool("previewInDesktopPosition", false);
            cfg.quick_switch.show_all_monitors = if let Some(cur_only) =
                qs.get("currentMonitorOnly").and_then(|v| v.as_bool())
            {
                !cur_only
            } else {
                qs.get_bool("showAllMonitors", true)
            };
            cfg.quick_switch.current_desktop_only = qs.get_bool("currentDesktopOnly", true);
            cfg.quick_switch.show_process_badge = qs.get_bool("showProcessBadge", true);
            cfg.quick_switch.show_app_names = qs.get_bool("showAppNames", false);
            cfg.quick_switch.show_numbers = qs.get_bool("showNumbers", true);
            cfg.quick_switch.compact_density = qs
                .get_str("density", "comfortable")
                .eq_ignore_ascii_case("compact");
            cfg.quick_switch.large_icons = qs.get_bool("largeIcons", true);
            cfg.quick_switch.visible_borders = qs.get_bool("visibleBorders", true);
            cfg.quick_switch.enable_pinyin_search = qs
                .get("enablePinyinSearch")
                .or_else(|| qs.get("pinyinSearch"))
                .and_then(|v| v.as_bool())
                .unwrap_or(true);
            cfg.quick_switch.include_minimized = qs.get_bool("includeMinimized", true);
            cfg.quick_switch.remember_bounds = qs.get_bool("rememberBounds", true);
            cfg.quick_switch.custom_width_dip = qs
                .get("customWidthDip")
                .and_then(|v| v.as_f64())
                .map(|v| (v as f32).clamp(480.0, 2400.0));
            cfg.quick_switch.custom_height_dip = qs
                .get("customHeightDip")
                .and_then(|v| v.as_f64())
                .map(|v| (v as f32).clamp(280.0, 1600.0));
            cfg.quick_switch.list_width_ratio =
                (qs.get_f64("listWidthRatio", 0.34) as f32).clamp(0.22, 0.60);
            cfg.quick_switch.custom_monitor_id = qs.get_str("customMonitorId", "").to_string();
            cfg.quick_switch.custom_rel_rect =
                qs.get("customRelRect").and_then(NormalizedWorkRect::from_json);
        }

        if let Some(sz) = root.get("snapZones").or_else(|| root.get("zones")) {
            cfg.snap_zones.trigger_mode =
                SnapTriggerMode::from_str_value(sz.get_str("triggerMode", "shift"));
            cfg.snap_zones.edge_threshold_px =
                sz.get_i32("edgeThresholdPx", 20).clamp(4, 96);
            cfg.snap_zones.zone_gap = sz
                .get("zoneGap")
                .or_else(|| sz.get("zoneGapPx"))
                .and_then(|v| v.as_i32())
                .unwrap_or(16)
                .clamp(0, 64);
            let raw_opacity = sz
                .get("highlightOpacity")
                .or_else(|| sz.get("overlayOpacity"))
                .and_then(|v| v.as_f64())
                .unwrap_or(0.30);
            let norm_opacity = if raw_opacity > 1.0 {
                (raw_opacity / 100.0) * 0.45
            } else {
                raw_opacity
            };
            cfg.snap_zones.highlight_opacity = (norm_opacity as f32).clamp(0.08, 0.85);
            cfg.snap_zones.show_zone_numbers = sz.get_bool("showZoneNumbers", true);
            cfg.snap_zones.allow_multi_zone_span = sz
                .get("allowMultiZoneSpan")
                .or_else(|| sz.get("spanMultipleZones"))
                .and_then(|v| v.as_bool())
                .unwrap_or(true);
            cfg.snap_zones.remember_last_zone = sz.get_bool("rememberLastZone", true);
            cfg.snap_zones.restore_original_size = sz.get_bool("restoreOriginalSize", true);
            cfg.snap_zones.per_virtual_desktop = sz.get_bool("perVirtualDesktop", false);
            cfg.snap_zones.show_on_all_monitors = sz.get_bool("showOnAllMonitors", false);
            cfg.snap_zones.override_win_arrows = sz.get_bool("overrideWinArrows", false);
            cfg.snap_zones.move_by_position = sz.get_bool("moveByPosition", false);
            cfg.snap_zones.active_layout_id =
                sz.get_str("activeLayoutId", "template_priority_grid").to_string();
            if let Some(arr) = sz.get("excludedApps").and_then(|v| v.as_array()) {
                cfg.snap_zones.excluded_apps = parse_lowercase_str_list(arr);
            }

            let bindings_node = sz
                .get("monitorBindings")
                .or_else(|| sz.get("activeLayoutByMonitor"));
            if let Some(bindings) = bindings_node.and_then(|v| v.as_array()) {
                for b in bindings {
                    let monitor_id = b.get_str("monitorId", "").trim().to_string();
                    let layout_id = b.get_str("layoutId", "").trim().to_string();
                    let desktop_id = b.get_str("desktopId", "").trim().to_string();
                    if !monitor_id.is_empty() && !layout_id.is_empty() {
                        cfg.snap_zones.monitor_bindings.push(MonitorLayoutBinding {
                            monitor_id,
                            desktop_id,
                            layout_id,
                        });
                    }
                }
            }
            if let Some(map) = bindings_node.and_then(|v| v.as_object()) {
                for (raw_key, val) in map {
                    if let Some(layout_id) = val.as_str() {
                        let layout_trimmed = layout_id.trim();
                        if layout_trimmed.is_empty() {
                            continue;
                        }
                        let (monitor_id, desktop_id) = match raw_key.rsplit_once('@') {
                            Some((m, d)) if !m.trim().is_empty() && !d.trim().is_empty() => {
                                (m.trim().to_string(), d.trim().to_string())
                            }
                            _ => (raw_key.trim().to_string(), String::new()),
                        };
                        if !monitor_id.is_empty() {
                            cfg.snap_zones.monitor_bindings.push(MonitorLayoutBinding {
                                monitor_id,
                                desktop_id,
                                layout_id: layout_trimmed.to_string(),
                            });
                        }
                    }
                }
            }

            if let Some(custom_layouts) = sz.get("layouts").and_then(|v| v.as_array()) {
                for item in custom_layouts {
                    if let Some(parsed) = ZoneLayout::from_json(item) {
                        if let Some(existing) =
                            cfg.snap_zones.layouts.iter_mut().find(|l| l.id == parsed.id)
                        {
                            *existing = parsed;
                        } else {
                            cfg.snap_zones.layouts.push(parsed);
                        }
                    }
                }
            }
        }

        if let Some(aot) = root.get("alwaysOnTop") {
            cfg.always_on_top.show_border = aot.get_bool("showBorder", true);
            cfg.always_on_top.border_color = aot.get_str("borderColor", "#F59E0B").to_string();
            cfg.always_on_top.border_width = aot
                .get("borderWidth")
                .or_else(|| aot.get("borderThicknessPx"))
                .or_else(|| aot.get("borderThickness"))
                .and_then(|v| v.as_i32())
                .unwrap_or(2)
                .clamp(1, 12);
        }

        if let Some(op) = root.get("opacity") {
            cfg.opacity.step_percent =
                (op.get_i32("stepPercent", OPACITY_DEFAULT_STEP as i32).clamp(1, 50)) as u8;
            cfg.opacity.min_percent = (op
                .get_i32("minPercent", 30)
                .clamp(OPACITY_MIN_PERCENT_FLOOR as i32, 95))
                as u8;
            cfg.opacity.alt_wheel_enabled = op
                .get("altWheelEnabled")
                .or_else(|| op.get("wheelOnTitlebar"))
                .and_then(|v| v.as_bool())
                .unwrap_or(true);
        }

        if let Some(ad) = root.get("altDrag") {
            cfg.alt_drag.enable_move = ad.get_bool("enableMove", true);
            cfg.alt_drag.enable_resize = ad.get_bool("enableResize", true);
            cfg.alt_drag.bring_to_front = ad.get_bool("bringToFront", true);
            let modifier_raw = ad.get_str("modifier", "alt");
            cfg.alt_drag.require_ctrl = modifier_raw.eq_ignore_ascii_case("ctrl_alt")
                || modifier_raw.eq_ignore_ascii_case("ctrl+alt");
            cfg.alt_drag.snap_to_zones = ad.get_bool("snapToZones", true);
            if let Some(arr) = ad.get("excludedApps").and_then(|v| v.as_array()) {
                cfg.alt_drag.excluded_apps = parse_lowercase_str_list(arr);
            }
        }

        if let Some(fb) = root.get("focusBorder").or_else(|| root.get("activeBorder")) {
            cfg.focus_border.border_color = fb
                .get("borderColor")
                .or_else(|| fb.get("color"))
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_string();
            cfg.focus_border.border_width = fb
                .get("borderWidth")
                .or_else(|| fb.get("thicknessPx"))
                .or_else(|| fb.get("thickness"))
                .and_then(|v| v.as_i32())
                .unwrap_or(2)
                .clamp(1, 10);
            cfg.focus_border.corner_radius = fb
                .get("cornerRadius")
                .or_else(|| fb.get("cornerRadiusPx"))
                .and_then(|v| v.as_i32())
                .unwrap_or(8)
                .clamp(0, 24);
            cfg.focus_border.opacity = (fb.get_f64("opacity", 0.90) as f32).clamp(0.1, 1.0);
        }

        if let Some(mm) = root.get("multiMonitor") {
            cfg.multi_monitor.keep_proportional_size = mm.get_bool("keepProportionalSize", true);
            cfg.multi_monitor.center_new_windows = mm.get_bool("centerNewWindows", false);
            cfg.multi_monitor.enable_tray_minimize = mm.get_bool("enableTrayMinimize", true);
        }

        if let Some(ws) = root.get("workspaces") {
            cfg.workspaces.minimize_unrelated = ws.get_bool("minimizeUnrelated", false);
            cfg.workspaces.unrelated_action = ws
                .get("unrelatedAction")
                .and_then(|v| v.as_str())
                .map(UnrelatedWindowAction::from_str_value)
                .unwrap_or(if cfg.workspaces.minimize_unrelated {
                    UnrelatedWindowAction::Minimize
                } else {
                    UnrelatedWindowAction::Keep
                });
            let list = ws
                .get("items")
                .or_else(|| ws.get("profiles"))
                .and_then(|v| v.as_array());
            if let Some(arr) = list {
                for item in arr {
                    let id = item.get_str("id", "").trim().to_string();
                    if id.is_empty() {
                        continue;
                    }
                    let name = item.get_str("name", &id).to_string();
                    let bound_layout_id = item.get_str("boundLayoutId", "").trim().to_string();
                    let desktop_id = item.get_str("desktopId", "").trim().to_string();
                    let desktop_name = item.get_str("desktopName", "").trim().to_string();
                    let move_existing_windows = item.get_bool("moveExistingWindows", true);
                    let minimize_unrelated = item.get("minimizeUnrelated").and_then(|v| v.as_bool());
                    let unrelated_action = item
                        .get("unrelatedAction")
                        .and_then(|v| v.as_str())
                        .map(UnrelatedWindowAction::from_str_value);
                    let create_desktop_shortcut = item.get_bool("createDesktopShortcut", false);
                    let last_launched_at = item
                        .get("lastLaunchedAt")
                        .and_then(|v| v.as_f64())
                        .and_then(|n| if n >= 0.0 { Some(n as u64) } else { None });
                    let mut apps = Vec::new();
                    if let Some(app_arr) = item.get("apps").and_then(|v| v.as_array()) {
                        for a in app_arr {
                            let exe_path = a.get_str("exePath", "").to_string();
                            let process_name = a.get_str("processName", "").to_string();
                            let aumid = a.get_str("aumid", "").to_string();
                            if exe_path.is_empty() && process_name.is_empty() && aumid.is_empty() {
                                continue;
                            }
                            let zone_index = a.get("zoneIndex").and_then(|v| v.as_usize());
                            let mut zone_indices: Vec<usize> = a
                                .get("zoneIndices")
                                .and_then(|v| v.as_array())
                                .map(|zarr| {
                                    zarr.iter().filter_map(|x| x.as_usize()).collect()
                                })
                                .unwrap_or_default();
                            if zone_indices.is_empty() {
                                if let Some(zi) = zone_index {
                                    zone_indices.push(zi);
                                }
                            }
                            apps.push(WorkspaceAppEntry {
                                exe_path,
                                args: a.get_str("args", "").to_string(),
                                process_name,
                                app_name: a.get_str("appName", "").to_string(),
                                title_hint: a
                                    .get("titleHint")
                                    .or_else(|| a.get("title"))
                                    .and_then(|v| v.as_str())
                                    .unwrap_or("")
                                    .to_string(),
                                aumid,
                                monitor_id: a.get_str("monitorId", "").to_string(),
                                monitor_friendly: a.get_str("monitorFriendly", "").to_string(),
                                monitor_orientation: a
                                    .get("monitorOrientation")
                                    .and_then(|v| v.as_str())
                                    .and_then(MonitorOrientation::from_str_opt),
                                monitor_index: a.get("monitorIndex").and_then(|v| v.as_usize()),
                                zone_index: zone_indices.first().copied().or(zone_index),
                                zone_indices,
                                rel_rect: a.get("relRect").and_then(NormalizedWorkRect::from_json),
                                rect: a.get("rect").and_then(RectI::from_json),
                                maximized: a.get_bool("maximized", false),
                                minimized: a.get_bool("minimized", false),
                                run_as_admin: a.get_bool("runAsAdmin", false),
                                always_on_top: a.get_bool("alwaysOnTop", false),
                                opacity_percent: a
                                    .get("opacityPercent")
                                    .and_then(|v| v.as_i32())
                                    .and_then(|p| {
                                        if (20..=100).contains(&p) {
                                            Some(p as u8)
                                        } else {
                                            None
                                        }
                                    }),
                            });
                        }
                    }
                    cfg.workspaces.items.push(WorkspaceProfile {
                        id,
                        name,
                        bound_layout_id,
                        desktop_id,
                        desktop_name,
                        move_existing_windows,
                        minimize_unrelated,
                        unrelated_action,
                        create_desktop_shortcut,
                        last_launched_at,
                        apps,
                    });
                }
            }
        }

        Ok(cfg)
    }
}

/// 6 大内置参数化模板（100% 对应 FancyZones 模板矩阵：无布局 / 焦点 / 列 / 行 / 网格 / “优先级”网格）
pub fn builtin_zone_presets() -> Vec<ZoneLayout> {
    vec![
        ZoneLayout::from_template(TemplateKind::None, 0, false, 0),
        ZoneLayout::from_template(TemplateKind::Focus, 3, true, 16),
        ZoneLayout::from_template(TemplateKind::Columns, 3, true, 16),
        ZoneLayout::from_template(TemplateKind::Rows, 3, true, 16),
        ZoneLayout::from_template(TemplateKind::Grid, 3, true, 16),
        ZoneLayout::from_template(TemplateKind::PriorityGrid, 3, true, 16),
    ]
}

/// 解析 Microsoft PowerToys FancyZones 的 custom-layouts.json（完整支持 Grid 与 Canvas 双格式及间距/感应距离）
pub fn import_powertoys_fancyzones_str(raw: &str) -> Result<Vec<ZoneLayout>, String> {
    let root = parse_json(raw).map_err(|e| format!("invalid PowerToys JSON: {e}"))?;
    let layouts_arr = root
        .get("custom-layouts")
        .and_then(|v| v.as_array())
        .ok_or_else(|| "missing 'custom-layouts' array".to_string())?;

    let mut converted = Vec::new();
    for (idx, item) in layouts_arr.iter().enumerate() {
        let raw_uuid = item.get_str("uuid", "");
        let sanitized_uuid: String = raw_uuid
            .chars()
            .filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
            .collect();
        let id = if sanitized_uuid.is_empty() {
            format!("pt_imported_{}", idx + 1)
        } else {
            format!("pt_{}", sanitized_uuid.to_ascii_lowercase())
        };
        let name = item.get_str("name", "Imported FancyZone").to_string();
        let kind = item.get_str("type", "").to_ascii_lowercase();
        let Some(info) = item.get("info") else {
            continue;
        };

        if kind == "grid" {
            let rows = info.get("rows").and_then(|v| v.as_usize()).unwrap_or(0);
            let columns = info.get("columns").and_then(|v| v.as_usize()).unwrap_or(0);
            if rows == 0 || columns == 0 {
                continue;
            }
            let row_weights = parse_f64_array(info.get("rows-percentage"), rows);
            let col_weights = parse_f64_array(info.get("columns-percentage"), columns);
            let cell_child_map = parse_cell_map(info.get("cell-child-map"), rows, columns);
            let show_spacing = info.get_bool("show-spacing", true);
            let spacing = info
                .get("spacing")
                .and_then(|v| v.as_i32())
                .map(|s| s.clamp(0, 64));
            let highlight_distance = info
                .get("sensitivity-radius")
                .and_then(|v| v.as_i32())
                .map(|s| s.clamp(4, 96));

            let mut layout = ZoneLayout::new_grid(
                id,
                name,
                false,
                rows,
                columns,
                row_weights,
                col_weights,
                cell_child_map,
                if show_spacing { spacing } else { Some(0) },
            );
            layout.show_spacing = show_spacing;
            layout.highlight_distance = highlight_distance;
            converted.push(layout);
        } else if kind == "canvas" {
            let ref_w = info.get_f64("ref-width", 1920.0).max(1.0);
            let ref_h = info.get_f64("ref-height", 1080.0).max(1.0);
            let highlight_distance = info
                .get("sensitivity-radius")
                .and_then(|v| v.as_i32())
                .map(|s| s.clamp(4, 96));
            let mut zones = Vec::new();
            if let Some(arr) = info.get("zones").and_then(|v| v.as_array()) {
                for (z_idx, z) in arr.iter().enumerate() {
                    let x = z
                        .get("X")
                        .or_else(|| z.get("x"))
                        .and_then(|v| v.as_f64())
                        .unwrap_or(0.0);
                    let y = z
                        .get("Y")
                        .or_else(|| z.get("y"))
                        .and_then(|v| v.as_f64())
                        .unwrap_or(0.0);
                    let w = z
                        .get("width")
                        .or_else(|| z.get("Width"))
                        .and_then(|v| v.as_f64())
                        .unwrap_or(0.0);
                    let h = z
                        .get("height")
                        .or_else(|| z.get("Height"))
                        .and_then(|v| v.as_f64())
                        .unwrap_or(0.0);
                    if w <= 0.0 || h <= 0.0 {
                        continue;
                    }
                    let nx = (x / ref_w).clamp(0.0, 0.99);
                    let ny = (y / ref_h).clamp(0.0, 0.99);
                    let nw = (w / ref_w).clamp(0.01, 1.0 - nx);
                    let nh = (h / ref_h).clamp(0.01, 1.0 - ny);
                    zones.push(NormalizedZone {
                        index: z_idx,
                        x: nx,
                        y: ny,
                        width: nw,
                        height: nh,
                    });
                }
            }
            if !zones.is_empty() {
                let mut layout = ZoneLayout::new_canvas(id, name, false, zones, Some(0));
                layout.show_spacing = false;
                layout.highlight_distance = highlight_distance;
                converted.push(layout);
            }
        }
    }

    Ok(converted)
}

/// 将导入的布局列表序列化为与宿主契约一致的 JSON 对象
pub fn serialize_imported_layouts(source_path: &str, layouts: &[ZoneLayout]) -> JsonValue {
    let mut root = JsonValue::object();
    root.insert("source", JsonValue::String(source_path.to_string()));
    root.insert("count", JsonValue::from_usize(layouts.len()));
    let mut arr = JsonValue::array();
    for l in layouts {
        arr.push(l.to_json());
    }
    root.insert("layouts", arr);
    root
}

/// 解析十六进制 RGB 颜色或 Tools3000 命名主题色令牌（如 "blue" / "purple" / "#3B82F6"）
pub fn parse_hex_color(hex: &str, fallback: [f32; 3]) -> [f32; 3] {
    let trimmed = hex.trim();
    match trimmed.to_ascii_lowercase().as_str() {
        "blue" => return [0.231, 0.510, 0.965],
        "indigo" => return [0.388, 0.400, 0.945],
        "purple" | "violet" => return [0.545, 0.361, 0.965],
        "emerald" | "green" | "mint" => return [0.063, 0.725, 0.506],
        "teal" | "cyan" => return [0.024, 0.714, 0.831],
        "amber" | "orange" => return [0.961, 0.620, 0.043],
        "rose" | "red" | "coral" => return [0.957, 0.247, 0.369],
        "pink" => return [0.925, 0.282, 0.600],
        "slate" | "gray" => return [0.392, 0.455, 0.545],
        _ => {}
    }
    let clean = trimmed.trim_start_matches('#');
    if clean.len() == 6 {
        if let (Ok(r), Ok(g), Ok(b)) = (
            u8::from_str_radix(&clean[0..2], 16),
            u8::from_str_radix(&clean[2..4], 16),
            u8::from_str_radix(&clean[4..6], 16),
        ) {
            return [r as f32 / 255.0, g as f32 / 255.0, b as f32 / 255.0];
        }
    }
    fallback
}

/// 解析支持 "accent" 继承语义的颜色令牌
pub fn resolve_color_or_accent(color: &str, accent_color: &str, fallback: [f32; 3]) -> [f32; 3] {
    let t = color.trim();
    if t.is_empty() || t.eq_ignore_ascii_case("accent") {
        parse_hex_color(accent_color, fallback)
    } else {
        parse_hex_color(t, fallback)
    }
}

fn parse_lowercase_str_list(arr: &[JsonValue]) -> Vec<String> {
    arr.iter()
        .filter_map(|v| v.as_str().map(|s| s.trim().to_ascii_lowercase()))
        .filter(|s| !s.is_empty())
        .collect()
}

fn parse_f64_array(v: Option<&JsonValue>, expected_len: usize) -> Vec<f64> {
    if let Some(arr) = v.and_then(|x| x.as_array()) {
        let mut out: Vec<f64> = arr
            .iter()
            .filter_map(|x| x.as_f64())
            .map(|n| if n.is_finite() && n > 0.0 { n } else { 1.0 })
            .collect();
        if out.len() == expected_len {
            return out;
        }
        if !out.is_empty() {
            out.resize(expected_len, 1.0);
            return out;
        }
    }
    vec![100.0 / (expected_len.max(1) as f64); expected_len]
}

fn parse_cell_map(v: Option<&JsonValue>, rows: usize, columns: usize) -> Vec<Vec<usize>> {
    if let Some(arr) = v.and_then(|x| x.as_array()) {
        if arr.len() == rows {
            let mut out = Vec::with_capacity(rows);
            let mut valid = true;
            for row_val in arr {
                if let Some(cols_arr) = row_val.as_array() {
                    if cols_arr.len() == columns {
                        out.push(
                            cols_arr
                                .iter()
                                .map(|c| c.as_usize().unwrap_or(0))
                                .collect(),
                        );
                        continue;
                    }
                }
                valid = false;
                break;
            }
            if valid {
                return out;
            }
        }
    }
    let mut fallback = Vec::with_capacity(rows);
    for r in 0..rows {
        let mut row = Vec::with_capacity(columns);
        for c in 0..columns {
            row.push(r * columns + c);
        }
        fallback.push(row);
    }
    fallback
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_config_disables_all_features_and_provides_six_fancyzones_templates() {
        let cfg = WindowKitConfig::default();
        assert!(!cfg.enabled);
        assert!(!cfg.features.any_enabled());
        assert!(!cfg.should_stay_resident());
        assert_eq!(cfg.snap_zones.layouts.len(), 6);
        assert_eq!(cfg.snap_zones.layouts[0].layout_type, ZoneLayoutType::None);
        assert_eq!(cfg.snap_zones.layouts[1].layout_type, ZoneLayoutType::Focus);
        assert_eq!(cfg.snap_zones.layouts[2].template_kind, Some(TemplateKind::Columns));
        assert_eq!(cfg.snap_zones.layouts[3].template_kind, Some(TemplateKind::Rows));
        assert_eq!(cfg.snap_zones.layouts[4].template_kind, Some(TemplateKind::Grid));
        assert_eq!(cfg.snap_zones.layouts[5].template_kind, Some(TemplateKind::PriorityGrid));
    }

    #[test]
    fn template_zone_count_and_show_spacing_toggle_resolve_accurately() {
        let work = RectI::new(0, 0, 3000, 1800);

        // Columns 4 列：开间距 16px vs 关间距 0px
        let mut cols = ZoneLayout::from_template(TemplateKind::Columns, 4, true, 16);
        let z_spaced = cols.resolve(work, 8);
        assert_eq!(z_spaced.len(), 4);
        assert_eq!(z_spaced[0].rect.left, 16);
        assert_eq!(z_spaced[1].rect.left - z_spaced[0].rect.right, 16);

        cols.show_spacing = false;
        let z_flush = cols.resolve(work, 16);
        assert_eq!(z_flush.len(), 4);
        assert_eq!(z_flush[0].rect.left, 0);
        assert_eq!(z_flush[1].rect.left - z_flush[0].rect.right, 0);
        assert_eq!(z_flush[3].rect.right, 3000);

        // Focus 焦点模板 4 张层叠卡片 + 克隆为自定义画布
        let focus = ZoneLayout::from_template(TemplateKind::Focus, 4, true, 16);
        let fz = focus.resolve(work, 16);
        assert_eq!(fz.len(), 4);
        assert!(fz[1].rect.left > fz[0].rect.left && fz[1].rect.top > fz[0].rect.top);
        let custom_from_focus = focus.clone_as_custom("custom_1".to_string(), "Focus Copy".to_string());
        assert_eq!(custom_from_focus.layout_type, ZoneLayoutType::Canvas);
        assert_eq!(custom_from_focus.zones.len(), 4);

        // Priority Grid 5 格：中央大主栏 + 左右上下各 2 辅栏
        let pg = ZoneLayout::from_template(TemplateKind::PriorityGrid, 5, true, 12);
        let pgz = pg.resolve(work, 12);
        assert_eq!(pgz.len(), 5);
        assert!(pgz[1].rect.width() > pgz[0].rect.width());
    }

    #[test]
    fn four_tier_layout_resolution_respects_orientation_defaults() {
        let cfg = WindowKitConfig::default();
        // 默认配置中 PriorityGrid 是横屏默认，Rows 是竖屏默认
        let landscape_layout = cfg.layout_for_monitor_with_orientation(
            "UNBOUND-MON",
            None,
            Some(MonitorOrientation::Landscape),
        );
        assert_eq!(landscape_layout.id, "template_priority_grid");

        let portrait_layout = cfg.layout_for_monitor_with_orientation(
            "UNBOUND-MON",
            None,
            Some(MonitorOrientation::Portrait),
        );
        assert_eq!(portrait_layout.id, "template_rows");
    }

    #[test]
    fn imports_powertoys_grid_and_canvas_layouts() {
        let sample = r#"{
            "custom-layouts": [
                {
                    "uuid": "{AA11-BB22}",
                    "name": "Dev Priority",
                    "type": "grid",
                    "info": {
                        "rows": 2,
                        "columns": 2,
                        "rows-percentage": [5000, 5000],
                        "columns-percentage": [6500, 3500],
                        "cell-child-map": [[0, 1], [0, 2]],
                        "show-spacing": true,
                        "spacing": 12,
                        "sensitivity-radius": 24
                    }
                },
                {
                    "uuid": "{CC33-DD44}",
                    "name": "Floating Canvas",
                    "type": "canvas",
                    "info": {
                        "ref-width": 2000,
                        "ref-height": 1000,
                        "zones": [
                            { "X": 0, "Y": 0, "width": 1000, "height": 1000 },
                            { "X": 1000, "Y": 0, "width": 1000, "height": 500 },
                            { "X": 1000, "Y": 500, "width": 1000, "height": 500 }
                        ]
                    }
                }
            ]
        }"#;

        let layouts = import_powertoys_fancyzones_str(sample).expect("should convert PowerToys layouts");
        assert_eq!(layouts.len(), 2);
        assert_eq!(layouts[0].layout_type, ZoneLayoutType::Grid);
        assert_eq!(layouts[0].gap, Some(12));
        assert_eq!(layouts[0].highlight_distance, Some(24));
        assert_eq!(layouts[0].col_weights, vec![6500.0, 3500.0]);
        assert_eq!(layouts[0].cell_child_map, vec![vec![0, 1], vec![0, 2]]);

        assert_eq!(layouts[1].layout_type, ZoneLayoutType::Canvas);
        assert_eq!(layouts[1].zones.len(), 3);
        assert!((layouts[1].zones[0].width - 0.5).abs() < 1e-6);
        assert!((layouts[1].zones[2].y - 0.5).abs() < 1e-6);
    }

    #[test]
    fn parses_monitor_and_desktop_bindings() {
        let raw = r#"{
            "enabled": true,
            "features": { "snapZones": true },
            "snapZones": {
                "perVirtualDesktop": true,
                "activeLayoutId": "template_columns",
                "monitorBindings": [
                    { "monitorId": "MON-A", "layoutId": "template_priority_grid" },
                    { "monitorId": "MON-A", "desktopId": "desk-2", "layoutId": "template_grid" }
                ]
            }
        }"#;
        let cfg = WindowKitConfig::from_json_str(raw).expect("valid config");
        assert!(cfg.should_stay_resident());
        assert_eq!(cfg.layout_for_monitor("MON-A", Some("desk-2")).id, "template_grid");
        assert_eq!(cfg.layout_for_monitor("MON-A", Some("desk-1")).id, "template_priority_grid");
        assert_eq!(cfg.layout_for_monitor("MON-B", None).id, "template_columns");
    }

    #[test]
    fn all_six_builtin_presets_resolve_cleanly_and_roundtrip_json() {
        let presets = builtin_zone_presets();
        assert_eq!(presets.len(), 6);
        let work = RectI::new(0, 0, 2560, 1400);
        for preset in &presets {
            let zones = preset.resolve(work, 16);
            if preset.layout_type == ZoneLayoutType::None {
                assert!(zones.is_empty());
            } else {
                assert!(zones.len() >= 2, "preset {} must have >= 2 zones", preset.id);
                for z in &zones {
                    assert!(z.rect.is_valid());
                    assert!(z.rect.left >= work.left && z.rect.right <= work.right);
                    assert!(z.rect.top >= work.top && z.rect.bottom <= work.bottom);
                }
            }
            let rt = ZoneLayout::from_json(&preset.to_json()).expect("layout JSON roundtrip");
            assert_eq!(rt.id, preset.id);
            assert_eq!(rt.resolve(work, 16), zones);
        }
        assert_eq!(parse_hex_color("#3B82F6", [0.0, 0.0, 0.0])[0], 0x3B as f32 / 255.0);
        assert_eq!(parse_hex_color("invalid", [0.1, 0.2, 0.3]), [0.1, 0.2, 0.3]);
        assert!(resolve_color_or_accent("accent", "blue", [0.0, 0.0, 0.0])[2] > 0.9);
    }

    #[test]
    fn parses_host_controller_schema_v1_payload_and_v3_extensions() {
        let host_payload = r#"{
            "schemaVersion": 3,
            "enabled": true,
            "theme": "system",
            "accentColor": "purple",
            "fullscreenMuted": true,
            "remoteMuted": false,
            "features": {
                "zones": true,
                "altTab": true,
                "activeBorder": true,
                "multiMonitorUtils": true,
                "workspaces": true
            },
            "zones": {
                "triggerMode": "shift",
                "edgeThresholdPx": 16,
                "zoneGapPx": 10,
                "spanMultipleZones": true,
                "activeLayoutId": "builtin-priority-3",
                "activeLayoutByMonitor": { "MON-1": "builtin-priority-3" },
                "layouts": [
                    {
                        "id": "builtin-priority-3",
                        "name": "Priority Grid",
                        "type": "grid",
                        "gapPx": 10,
                        "hotkeyDigit": 2,
                        "zones": [
                            { "x": 0.0, "y": 0.0, "w": 0.25, "h": 1.0 },
                            { "x": 0.25, "y": 0.0, "w": 0.50, "h": 1.0 },
                            { "x": 0.75, "y": 0.0, "w": 0.25, "h": 1.0 }
                        ]
                    }
                ]
            },
            "quickSwitch": {
                "replaceAltTab": true,
                "sameAppEnabled": true,
                "sameAppHotkey": "Alt+`",
                "previewInDesktopPosition": true,
                "currentMonitorOnly": true,
                "showPreview": true,
                "pinyinSearch": true,
                "rememberBounds": true,
                "customWidthDip": 1120,
                "customHeightDip": 680,
                "listWidthRatio": 0.38
            },
            "workspaces": {
                "unrelatedAction": "minimize_to_tray",
                "items": [{
                    "id": "ws-dev",
                    "name": "Deep Coding",
                    "boundLayoutId": "template_priority_grid",
                    "moveExistingWindows": true,
                    "apps": [{
                        "exePath": "C:\\Code.exe",
                        "processName": "Code.exe",
                        "zoneIndices": [0, 1],
                        "runAsAdmin": true,
                        "alwaysOnTop": true,
                        "opacityPercent": 85
                    }]
                }]
            },
            "alwaysOnTop": { "showBorder": true, "borderThicknessPx": 3, "borderColor": "accent" },
            "activeBorder": { "thicknessPx": 2, "color": "accent", "cornerRadiusPx": 10 },
            "altDrag": { "enableMove": true, "enableResize": false, "excludedApps": ["blender.exe"] }
        }"#;

        let cfg = WindowKitConfig::from_json_str(host_payload).expect("schema v3 must parse");
        assert_eq!(cfg.schema_version, 3);
        assert!(cfg.features.snap_zones && cfg.features.quick_switch && cfg.features.workspaces);
        assert!(cfg.quick_switch.same_app_enabled);
        assert!(cfg.quick_switch.preview_in_desktop_position);
        assert_eq!(cfg.quick_switch.custom_width_dip, Some(1120.0));
        assert_eq!(cfg.find_layout_by_hotkey_digit(2).map(|l| l.id.as_str()), Some("builtin-priority-3"));
        assert_eq!(cfg.workspaces.unrelated_action, UnrelatedWindowAction::MinimizeToTray);
        assert_eq!(cfg.workspaces.items.len(), 1);
        assert_eq!(cfg.workspaces.items[0].apps[0].zone_indices, vec![0, 1]);
        assert!(cfg.workspaces.items[0].apps[0].run_as_admin);
        assert!(cfg.workspaces.items[0].apps[0].always_on_top);
        assert_eq!(cfg.workspaces.items[0].apps[0].opacity_percent, Some(85));
    }

    #[test]
    fn parses_per_virtual_desktop_composite_bindings_and_center_new_windows() {
        let raw = r#"{
            "schemaVersion": 2,
            "enabled": true,
            "features": { "snapZones": true, "multiMonitor": true },
            "snapZones": {
                "perVirtualDesktop": true,
                "activeLayoutId": "template_columns",
                "monitorBindings": {
                    "MON-1": "template_columns",
                    "MON-1@11111111-2222-3333-4444-555555555555": "template_grid",
                    "MON-1@aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee": "template_priority_grid"
                }
            },
            "multiMonitor": {
                "centerNewWindows": true,
                "enableTrayMinimize": true
            }
        }"#;
        let cfg = WindowKitConfig::from_json_str(raw).expect("v2 payload must parse");
        assert!(cfg.snap_zones.per_virtual_desktop);
        assert!(cfg.multi_monitor.center_new_windows);
        assert_eq!(
            cfg.layout_for_monitor("MON-1", Some("11111111-2222-3333-4444-555555555555")).id,
            "template_grid"
        );
        assert_eq!(
            cfg.layout_for_monitor("MON-1", Some("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE")).id,
            "template_priority_grid"
        );
        // 未知虚拟桌面优雅回退至该显示器默认绑定
        assert_eq!(
            cfg.layout_for_monitor("MON-1", Some("99999999-9999-9999-9999-999999999999")).id,
            "template_columns"
        );
    }
}
