// ─────────────────────────────────────────────────────────────────────────────
// core/config.rs — WindowKit 配置模型、安全默认开关（全默认关闭）、
//                  6 大内置高频分区预设、PowerToys FancyZones 双向解析转换器
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::constants::{OPACITY_DEFAULT_STEP, OPACITY_MIN_PERCENT_FLOOR};
use crate::core::geometry::{
    resolve_canvas_zones, resolve_grid_zones, NormalizedZone, RectI, ResolvedZone,
};
use crate::core::json::{parse_json, JsonValue};
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

#[derive(Debug, Clone)]
pub struct QuickSwitchConfig {
    pub show_live_preview: bool,
    pub show_all_monitors: bool,
    pub show_process_badge: bool,
    pub show_numbers: bool,
    pub compact_density: bool,
    pub enable_pinyin_search: bool,
    pub include_minimized: bool,
}

impl Default for QuickSwitchConfig {
    fn default() -> Self {
        Self {
            show_live_preview: true,
            show_all_monitors: true,
            show_process_badge: true,
            show_numbers: true,
            compact_density: false,
            enable_pinyin_search: true,
            include_minimized: true,
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

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ZoneLayoutType {
    Grid,
    Canvas,
}

#[derive(Debug, Clone)]
pub struct ZoneLayout {
    pub id: String,
    pub name: String,
    pub layout_type: ZoneLayoutType,
    pub built_in: bool,
    pub gap: Option<i32>,
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
    pub fn resolve(&self, work_area: RectI, default_gap: i32) -> Vec<ResolvedZone> {
        let gap = self.gap.unwrap_or(default_gap);
        match self.layout_type {
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
                ZoneLayoutType::Grid => "grid".to_string(),
                ZoneLayoutType::Canvas => "canvas".to_string(),
            }),
        );
        obj.insert("builtIn", JsonValue::Bool(self.built_in));
        if let Some(g) = self.gap {
            obj.insert("gap", JsonValue::from_i32(g));
        }
        match self.layout_type {
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
        let gap = v
            .get("gap")
            .or_else(|| v.get("gapPx"))
            .and_then(|x| x.as_i32());

        // 兼容宿主与 UI 传入的归一化 zones 数组格式（含 {x,y,w,h} 与 {x,y,width,height}）
        if let Some(arr) = v.get("zones").and_then(|x| x.as_array()) {
            let mut zones = Vec::new();
            for (seq, item) in arr.iter().enumerate() {
                let x = item.get_f64("x", 0.0);
                let y = item.get_f64("y", 0.0);
                let width = item
                    .get("width")
                    .or_else(|| item.get("w"))
                    .and_then(|n| n.as_f64())
                    .unwrap_or(0.5);
                let height = item
                    .get("height")
                    .or_else(|| item.get("h"))
                    .and_then(|n| n.as_f64())
                    .unwrap_or(1.0);
                let index = item
                    .get("index")
                    .or_else(|| item.get("id"))
                    .and_then(|n| n.as_usize())
                    .unwrap_or(seq);
                zones.push(NormalizedZone {
                    index,
                    x,
                    y,
                    width,
                    height,
                });
            }
            if !zones.is_empty() {
                return Some(Self {
                    id,
                    name,
                    layout_type: ZoneLayoutType::Canvas,
                    built_in,
                    gap,
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
        Some(Self {
            id,
            name,
            layout_type: ZoneLayoutType::Grid,
            built_in,
            gap,
            rows,
            columns,
            row_weights,
            col_weights,
            cell_child_map,
            zones: Vec::new(),
        })
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
    pub active_layout_id: String,
    pub excluded_apps: Vec<String>,
    pub monitor_bindings: Vec<MonitorLayoutBinding>,
    pub layouts: Vec<ZoneLayout>,
}

impl Default for SnapZonesConfig {
    fn default() -> Self {
        Self {
            trigger_mode: SnapTriggerMode::Shift,
            edge_threshold_px: 24,
            zone_gap: 8,
            highlight_opacity: 0.28,
            show_zone_numbers: true,
            allow_multi_zone_span: true,
            remember_last_zone: true,
            restore_original_size: true,
            per_virtual_desktop: false,
            active_layout_id: "preset_columns_2".to_string(),
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

#[derive(Debug, Clone)]
pub struct WorkspaceAppEntry {
    pub exe_path: String,
    pub args: String,
    pub process_name: String,
    pub title_hint: String,
    pub monitor_id: String,
    pub zone_index: Option<usize>,
    pub rect: Option<RectI>,
    pub maximized: bool,
    pub minimized: bool,
}

#[derive(Debug, Clone)]
pub struct WorkspaceProfile {
    pub id: String,
    pub name: String,
    pub minimize_unrelated: Option<bool>,
    pub apps: Vec<WorkspaceAppEntry>,
}

#[derive(Debug, Clone, Default)]
pub struct WorkspacesConfig {
    pub minimize_unrelated: bool,
    pub items: Vec<WorkspaceProfile>,
}

/// 顶层完整配置根对象
#[derive(Debug, Clone)]
pub struct WindowKitConfig {
    pub schema_version: u32,
    pub enabled: bool,
    pub theme: String,
    pub accent_color: String,
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
            schema_version: 0,
            enabled: false,
            theme: "dark".to_string(),
            accent_color: "#3B82F6".to_string(),
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

    /// 查找某个显示器（及可选虚拟桌面）绑定的 SnapZones 布局
    pub fn layout_for_monitor(&self, monitor_id: &str, desktop_id: Option<&str>) -> &ZoneLayout {
        // 1. 若开启按虚拟桌面绑定，优先匹配 monitor_id + desktop_id
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
        // 3. 回退到全局默认布局
        self.find_layout(&self.snap_zones.active_layout_id)
            .unwrap_or(&self.snap_zones.layouts[0])
    }

    pub fn find_layout(&self, id: &str) -> Option<&ZoneLayout> {
        self.snap_zones.layouts.iter().find(|l| l.id == id)
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
            cfg.quick_switch.show_live_preview = qs
                .get("showLivePreview")
                .or_else(|| qs.get("showPreview"))
                .and_then(|v| v.as_bool())
                .unwrap_or(true);
            cfg.quick_switch.show_all_monitors = if let Some(cur_only) =
                qs.get("currentMonitorOnly").and_then(|v| v.as_bool())
            {
                !cur_only
            } else {
                qs.get_bool("showAllMonitors", true)
            };
            cfg.quick_switch.show_process_badge = qs.get_bool("showProcessBadge", true);
            cfg.quick_switch.show_numbers = qs.get_bool("showNumbers", true);
            cfg.quick_switch.compact_density = qs
                .get_str("density", "comfortable")
                .eq_ignore_ascii_case("compact");
            cfg.quick_switch.enable_pinyin_search = qs
                .get("enablePinyinSearch")
                .or_else(|| qs.get("pinyinSearch"))
                .and_then(|v| v.as_bool())
                .unwrap_or(true);
            cfg.quick_switch.include_minimized = qs.get_bool("includeMinimized", true);
        }

        if let Some(sz) = root.get("snapZones").or_else(|| root.get("zones")) {
            cfg.snap_zones.trigger_mode =
                SnapTriggerMode::from_str_value(sz.get_str("triggerMode", "shift"));
            cfg.snap_zones.edge_threshold_px =
                sz.get_i32("edgeThresholdPx", 24).clamp(6, 96);
            cfg.snap_zones.zone_gap = sz
                .get("zoneGap")
                .or_else(|| sz.get("zoneGapPx"))
                .and_then(|v| v.as_i32())
                .unwrap_or(8)
                .clamp(0, 64);
            let raw_opacity = sz
                .get("highlightOpacity")
                .or_else(|| sz.get("overlayOpacity"))
                .and_then(|v| v.as_f64())
                .unwrap_or(0.28);
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
            cfg.snap_zones.active_layout_id =
                sz.get_str("activeLayoutId", "preset_columns_2").to_string();
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
            // 也兼容 monitorBindings / activeLayoutByMonitor 为对象形式：
            //   - "<monitorId>": "<layoutId>" -> 全局显示器绑定
            //   - "<monitorId>@<desktopGuid>": "<layoutId>" -> 按虚拟桌面独立绑定
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
                .clamp(1, 8);
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
                .clamp(1, 8);
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
                    let minimize_unrelated = item.get("minimizeUnrelated").and_then(|v| v.as_bool());
                    let mut apps = Vec::new();
                    if let Some(app_arr) = item.get("apps").and_then(|v| v.as_array()) {
                        for a in app_arr {
                            let exe_path = a.get_str("exePath", "").to_string();
                            let process_name = a.get_str("processName", "").to_string();
                            if exe_path.is_empty() && process_name.is_empty() {
                                continue;
                            }
                            apps.push(WorkspaceAppEntry {
                                exe_path,
                                args: a.get_str("args", "").to_string(),
                                process_name,
                                title_hint: a
                                    .get("titleHint")
                                    .or_else(|| a.get("title"))
                                    .and_then(|v| v.as_str())
                                    .unwrap_or("")
                                    .to_string(),
                                monitor_id: a.get_str("monitorId", "").to_string(),
                                zone_index: a.get("zoneIndex").and_then(|v| v.as_usize()),
                                rect: a.get("rect").and_then(RectI::from_json),
                                maximized: a.get_bool("maximized", false),
                                minimized: a.get_bool("minimized", false),
                            });
                        }
                    }
                    cfg.workspaces.items.push(WorkspaceProfile {
                        id,
                        name,
                        minimize_unrelated,
                        apps,
                    });
                }
            }
        }

        Ok(cfg)
    }
}

/// 6 大高频内置 SnapZones 预设（覆含常规双栏、三栏、带鱼屏 25/50/25、主次三区、四宫格与竖屏三层）
pub fn builtin_zone_presets() -> Vec<ZoneLayout> {
    vec![
        ZoneLayout {
            id: "preset_columns_2".to_string(),
            name: "Columns 2 (50 / 50)".to_string(),
            layout_type: ZoneLayoutType::Grid,
            built_in: true,
            gap: None,
            rows: 1,
            columns: 2,
            row_weights: vec![100.0],
            col_weights: vec![50.0, 50.0],
            cell_child_map: vec![vec![0, 1]],
            zones: Vec::new(),
        },
        ZoneLayout {
            id: "preset_columns_3".to_string(),
            name: "Columns 3 (1 : 1 : 1)".to_string(),
            layout_type: ZoneLayoutType::Grid,
            built_in: true,
            gap: None,
            rows: 1,
            columns: 3,
            row_weights: vec![100.0],
            col_weights: vec![33.333, 33.334, 33.333],
            cell_child_map: vec![vec![0, 1, 2]],
            zones: Vec::new(),
        },
        ZoneLayout {
            id: "preset_ultrawide_121".to_string(),
            name: "Ultrawide Focus (25 / 50 / 25)".to_string(),
            layout_type: ZoneLayoutType::Grid,
            built_in: true,
            gap: None,
            rows: 1,
            columns: 3,
            row_weights: vec![100.0],
            col_weights: vec![25.0, 50.0, 25.0],
            cell_child_map: vec![vec![0, 1, 2]],
            zones: Vec::new(),
        },
        ZoneLayout {
            id: "preset_priority_grid".to_string(),
            name: "Priority Grid (60 Main + 2 Stacked)".to_string(),
            layout_type: ZoneLayoutType::Grid,
            built_in: true,
            gap: None,
            rows: 2,
            columns: 2,
            row_weights: vec![50.0, 50.0],
            col_weights: vec![60.0, 40.0],
            cell_child_map: vec![vec![0, 1], vec![0, 2]],
            zones: Vec::new(),
        },
        ZoneLayout {
            id: "preset_quad".to_string(),
            name: "Quad Grid (2 x 2)".to_string(),
            layout_type: ZoneLayoutType::Grid,
            built_in: true,
            gap: None,
            rows: 2,
            columns: 2,
            row_weights: vec![50.0, 50.0],
            col_weights: vec![50.0, 50.0],
            cell_child_map: vec![vec![0, 1], vec![2, 3]],
            zones: Vec::new(),
        },
        ZoneLayout {
            id: "preset_vertical_3".to_string(),
            name: "Portrait Stack (3 Rows)".to_string(),
            layout_type: ZoneLayoutType::Grid,
            built_in: true,
            gap: None,
            rows: 3,
            columns: 1,
            row_weights: vec![33.333, 33.334, 33.333],
            col_weights: vec![100.0],
            cell_child_map: vec![vec![0], vec![1], vec![2]],
            zones: Vec::new(),
        },
    ]
}

/// 解析 Microsoft PowerToys FancyZones 的 custom-layouts.json（完整支持 Grid 与 Canvas 双格式）
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
            let gap = if info.get_bool("show-spacing", true) {
                info.get("spacing").and_then(|v| v.as_i32()).map(|s| s.clamp(0, 64))
            } else {
                Some(0)
            };

            converted.push(ZoneLayout {
                id,
                name,
                layout_type: ZoneLayoutType::Grid,
                built_in: false,
                gap,
                rows,
                columns,
                row_weights,
                col_weights,
                cell_child_map,
                zones: Vec::new(),
            });
        } else if kind == "canvas" {
            let ref_w = info.get_f64("ref-width", 1920.0).max(1.0);
            let ref_h = info.get_f64("ref-height", 1080.0).max(1.0);
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
                converted.push(ZoneLayout {
                    id,
                    name,
                    layout_type: ZoneLayoutType::Canvas,
                    built_in: false,
                    gap: None,
                    rows: 0,
                    columns: 0,
                    row_weights: Vec::new(),
                    col_weights: Vec::new(),
                    cell_child_map: Vec::new(),
                    zones,
                });
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
        "emerald" | "green" => return [0.063, 0.725, 0.506],
        "teal" | "cyan" => return [0.024, 0.714, 0.831],
        "amber" | "orange" => return [0.961, 0.620, 0.043],
        "rose" | "red" => return [0.957, 0.247, 0.369],
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
    fn default_config_disables_all_features() {
        let cfg = WindowKitConfig::default();
        assert!(!cfg.enabled);
        assert!(!cfg.features.any_enabled());
        assert!(!cfg.should_stay_resident());
        assert_eq!(cfg.snap_zones.layouts.len(), 6);
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
                        "spacing": 12
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
                "activeLayoutId": "preset_columns_2",
                "monitorBindings": [
                    { "monitorId": "MON-A", "layoutId": "preset_columns_3" },
                    { "monitorId": "MON-A", "desktopId": "desk-2", "layoutId": "preset_quad" }
                ]
            }
        }"#;
        let cfg = WindowKitConfig::from_json_str(raw).expect("valid config");
        assert!(cfg.should_stay_resident());
        assert_eq!(cfg.layout_for_monitor("MON-A", Some("desk-2")).id, "preset_quad");
        assert_eq!(cfg.layout_for_monitor("MON-A", Some("desk-1")).id, "preset_columns_3");
        assert_eq!(cfg.layout_for_monitor("MON-B", None).id, "preset_columns_2");
    }

    #[test]
    fn all_six_builtin_presets_resolve_cleanly_and_roundtrip_json() {
        let presets = builtin_zone_presets();
        assert_eq!(presets.len(), 6);
        let work = RectI::new(0, 0, 2560, 1400);
        for preset in &presets {
            let zones = preset.resolve(work, 8);
            assert!(zones.len() >= 2, "preset {} must have >= 2 zones", preset.id);
            for z in &zones {
                assert!(z.rect.is_valid());
                assert!(z.rect.left >= work.left && z.rect.right <= work.right);
                assert!(z.rect.top >= work.top && z.rect.bottom <= work.bottom);
            }
            let rt = ZoneLayout::from_json(&preset.to_json()).expect("layout JSON roundtrip");
            assert_eq!(rt.id, preset.id);
            assert_eq!(rt.resolve(work, 8), zones);
        }
        assert_eq!(parse_hex_color("#3B82F6", [0.0, 0.0, 0.0])[0], 0x3B as f32 / 255.0);
        assert_eq!(parse_hex_color("invalid", [0.1, 0.2, 0.3]), [0.1, 0.2, 0.3]);
        assert!(resolve_color_or_accent("accent", "blue", [0.0, 0.0, 0.0])[2] > 0.9);
    }

    #[test]
    fn parses_host_controller_schema_v1_payload() {
        let host_payload = r#"{
            "schemaVersion": 1,
            "enabled": true,
            "theme": "system",
            "accentColor": "purple",
            "fullscreenMuted": true,
            "remoteMuted": false,
            "features": {
                "zones": true,
                "altTab": true,
                "activeBorder": true,
                "multiMonitorUtils": true
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
                        "zones": [
                            { "x": 0.0, "y": 0.0, "w": 0.25, "h": 1.0 },
                            { "x": 0.25, "y": 0.0, "w": 0.50, "h": 1.0 },
                            { "x": 0.75, "y": 0.0, "w": 0.25, "h": 1.0 }
                        ]
                    }
                ]
            },
            "altTab": { "currentMonitorOnly": true, "showPreview": true, "pinyinSearch": true },
            "alwaysOnTop": { "showBorder": true, "borderThicknessPx": 3, "borderColor": "accent" },
            "activeBorder": { "thicknessPx": 2, "color": "accent", "cornerRadiusPx": 10 },
            "altDrag": { "enableMove": true, "enableResize": false, "excludedApps": ["blender.exe"] }
        }"#;

        let cfg = WindowKitConfig::from_json_str(host_payload).expect("host schema v1 must parse");
        assert_eq!(cfg.schema_version, 1);
        assert!(cfg.features.snap_zones);
        assert!(cfg.features.quick_switch);
        assert!(cfg.features.focus_border);
        assert!(cfg.features.multi_monitor);
        assert!(cfg.fullscreen_muted);
        assert!(!cfg.quick_switch.show_all_monitors);
        assert_eq!(cfg.snap_zones.edge_threshold_px, 16);
        assert_eq!(cfg.snap_zones.zone_gap, 10);
        assert_eq!(cfg.always_on_top.border_width, 3);
        assert_eq!(cfg.focus_border.corner_radius, 10);
        assert!(!cfg.alt_drag.enable_resize);
        assert_eq!(cfg.alt_drag.excluded_apps, vec!["blender.exe".to_string()]);
        let layout = cfg.layout_for_monitor("MON-1", None);
        assert_eq!(layout.id, "builtin-priority-3");
        let resolved = layout.resolve(RectI::new(0, 0, 2000, 1000), 10);
        assert_eq!(resolved.len(), 3);
    }

    #[test]
    fn parses_per_virtual_desktop_composite_bindings_and_center_new_windows() {
        let raw = r#"{
            "schemaVersion": 2,
            "enabled": true,
            "features": { "snapZones": true, "multiMonitor": true },
            "snapZones": {
                "perVirtualDesktop": true,
                "activeLayoutId": "preset_columns_2",
                "monitorBindings": {
                    "MON-1": "preset_columns_2",
                    "MON-1@11111111-2222-3333-4444-555555555555": "preset_quad",
                    "MON-1@aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee": "preset_columns_3"
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
            "preset_quad"
        );
        assert_eq!(
            cfg.layout_for_monitor("MON-1", Some("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE")).id,
            "preset_columns_3"
        );
        // 未知虚拟桌面优雅回退至该显示器默认绑定
        assert_eq!(
            cfg.layout_for_monitor("MON-1", Some("99999999-9999-9999-9999-999999999999")).id,
            "preset_columns_2"
        );
    }
}
