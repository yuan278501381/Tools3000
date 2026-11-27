// ─────────────────────────────────────────────────────────────────────────────
// features/layout_hub.rs — SnapZones v2「双层共视」Slint Fluent 布局管理器
//
// 世界级架构亮点（1:1 复刻并超越 PowerToys FancyZones 视觉与交互体验）:
//   1. 双层共视（所见即所得）：
//      - Layer-1 底层 1:1 铺满当前显示器工作区（任务栏可见）的通透磨砂实时分区预览；
//      - Layer-2 前景由 Slint 1.19.1 Fluent 引擎渲染居中圆角卡片（零 WebView 膨胀，
//        自动联动 Tools3000 Studio Alabaster 浅色 / Obsidian Atelier 深色与动态强调色）。
//   2. 顶部多屏物理选卡（编号 / 分辨率 / 缩放比）+ 多虚拟桌面胶囊；点击瞬间切换底层预览屏。
//   3. 6 大参数化模板 + 自定义卡片 + 右键/铅笔上下文菜单 + 3 大原生内嵌弹窗：
//      - 编辑模板：区域数 / 间距开关与滑块 / 突出显示距离 / 横竖屏默认星标，滑块拖动 0ms 联动底层预览；
//      - 编辑自定义：名称 / 0–9 热键下拉 / 圆形强调色铅笔直达 1:1 全屏 LayoutEditor / 复制与删除；
//      - 新建布局：名称 + 网格/画布双选卡，创建直入 1:1 全屏 LayoutEditor。
//   4. 拔屏/旋转/分辨率变化/切换虚拟桌面自动自愈，关闭即彻底释放底层预览与内存。
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{parse_hex_color, WindowKitConfig, ZoneLayout, ZoneLayoutType};
use crate::core::constants::{
    FILE_LAYOUT_EDITOR_RESULT, HOST_EDITOR_RESULT_MSG_NAME, HOST_MESSAGE_WINDOW_CLASS,
};
use crate::core::geometry::RectI;
use crate::core::json::JsonValue;
use crate::core::topology::{
    ensure_window_rect_on_live_monitors, enumerate_virtual_desktop_topology,
};
use crate::core::win_utils::{
    enumerate_monitors, monitor_for_point, to_wide, write_text_file_atomic, MonitorInfo,
};
use crate::features::layout_editor::OpenEditorRequest;
use crate::render::d2d::{rect_f, render_layered_window_d2d, rgb_a, rgba, SharedGraphics};
use crate::ui_slint::{
    hide_layout_hub, raise_layout_hub_above_preview, show_or_update_layout_hub, LayoutCardData,
    LayoutDesktopData, LayoutHubViewState, LayoutMonitorData, LayoutThumbData, SlintThemeSpec,
    SlintUiEvent,
};
use crate::{wk_debug, wk_info, wk_warn};
use std::mem::size_of;
use std::path::Path;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Once;
use std::time::{SystemTime, UNIX_EPOCH};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, POINT, WPARAM};
use windows::Win32::Graphics::Direct2D::D2D1_DRAW_TEXT_OPTIONS_CLIP;
use windows::Win32::Graphics::DirectWrite::DWRITE_MEASURING_MODE_NATURAL;
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, FindWindowW, GetCursorPos, IsWindow,
    LoadCursorW, PostMessageW, RegisterClassExW, RegisterWindowMessageW, SetWindowPos, ShowWindow,
    CS_HREDRAW, CS_VREDRAW, HWND_TOPMOST, IDC_ARROW, SWP_NOACTIVATE, SW_HIDE, SW_SHOWNOACTIVATE,
    WNDCLASSEXW, WS_EX_LAYERED, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TOPMOST,
    WS_EX_TRANSPARENT, WS_POPUP,
};

const LAYOUT_HUB_PREVIEW_CLASS: &str = "Tools3000_WindowKit_LayoutHubPreview";
static HUB_HOST_RESULT_MSG: AtomicU32 = AtomicU32::new(0);

fn hub_host_result_msg_id() -> u32 {
    let cached = HUB_HOST_RESULT_MSG.load(Ordering::Acquire);
    if cached != 0 {
        return cached;
    }
    let wide = to_wide(HOST_EDITOR_RESULT_MSG_NAME);
    let msg = unsafe { RegisterWindowMessageW(PCWSTR(wide.as_ptr())) };
    HUB_HOST_RESULT_MSG.store(msg, Ordering::Release);
    msg
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. 公开请求与回传结果契约
// ─────────────────────────────────────────────────────────────────────────────

pub struct OpenHubRequest {
    pub monitor_id: Option<String>,
    pub desktop_id: Option<String>,
    pub restore_settings_hwnd: Option<isize>,
    pub ui_lang: String,
}

pub enum HubEventOutcome {
    None,
    ConfigMutated,
    Closed {
        #[allow(dead_code)]
        restore_settings: bool,
    },
    LaunchFullEditor(OpenEditorRequest),
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. 弹窗状态机与会话管理
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
enum ActiveModal {
    TemplateEdit {
        original_id: String,
        draft: ZoneLayout,
        default_horiz: bool,
        default_vert: bool,
    },
    CustomProps {
        original_id: String,
        draft: ZoneLayout,
        default_horiz: bool,
        default_vert: bool,
    },
    NewLayout {
        name: String,
        is_canvas: bool,
    },
}

struct HubSession {
    preview_hwnd: Option<HWND>,
    monitors: Vec<MonitorInfo>,
    active_monitor_idx: usize,
    desktop_id: Option<String>,
    zh_ui: bool,
    restore_settings_hwnd: Option<isize>,
    win_rect: RectI,
    modal: Option<ActiveModal>,
    hidden_for_editor: bool,
}

pub struct LayoutHubManager {
    session: Option<HubSession>,
}

impl LayoutHubManager {
    pub fn new(_control_hwnd: HWND) -> Self {
        Self { session: None }
    }

    pub fn set_control_hwnd(&mut self, _hwnd: HWND) {}

    pub fn is_open(&self) -> bool {
        self.session.is_some()
    }

    pub fn is_visible(&self) -> bool {
        self.session
            .as_ref()
            .map(|s| !s.hidden_for_editor)
            .unwrap_or(false)
    }

    /// 打开双层共视布局管理器（底层 1:1 实时磨砂预览 + 前景 Slint Fluent 管理窗）
    pub fn open(
        &mut self,
        req: OpenHubRequest,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> bool {
        if let Some(sess) = self.session.as_mut() {
            sess.hidden_for_editor = false;
            sess.monitors = enumerate_monitors();
            if sess.active_monitor_idx >= sess.monitors.len() {
                sess.active_monitor_idx = 0;
            }
            if let Some(g) = gfx {
                sess.render_preview_layer(cfg, g);
            }
            sess.push_to_slint(cfg, true);
            return true;
        }

        let monitors = enumerate_monitors();
        if monitors.is_empty() {
            return false;
        }

        let active_idx = req
            .monitor_id
            .as_deref()
            .filter(|s| !s.is_empty())
            .and_then(|mid| {
                monitors
                    .iter()
                    .position(|m| m.id.eq_ignore_ascii_case(mid))
            })
            .or_else(|| {
                let mut pt = POINT::default();
                if unsafe { GetCursorPos(&mut pt) }.is_ok() {
                    monitor_for_point(pt.x, pt.y, &monitors)
                        .and_then(|m| monitors.iter().position(|x| x.id == m.id))
                } else {
                    None
                }
            })
            .unwrap_or(0);

        let mon = &monitors[active_idx];
        let topo = enumerate_virtual_desktop_topology();
        let desktop_id = req
            .desktop_id
            .filter(|s| !s.trim().is_empty())
            .or_else(|| {
                if topo.current_id.is_empty() {
                    None
                } else {
                    Some(topo.current_id.clone())
                }
            });

        let scale = (mon.dpi as f32 / 96.0).max(1.0);
        let work = mon.work_rect;
        let ideal_w = ((1060.0 * scale).round() as i32)
            .min(work.width() - (64.0 * scale) as i32)
            .max(760);
        let ideal_h = ((640.0 * scale).round() as i32)
            .min(work.height() - (64.0 * scale) as i32)
            .max(480);
        let wx = work.left + (work.width() - ideal_w) / 2;
        let wy = work.top + (work.height() - ideal_h) / 2;
        let win_rect = RectI::from_xywh(wx, wy, ideal_w, ideal_h);

        let preview_hwnd = create_hub_preview_window();
        let zh_ui = !req.ui_lang.trim().to_ascii_lowercase().starts_with("en");

        let sess = HubSession {
            preview_hwnd,
            monitors,
            active_monitor_idx: active_idx,
            desktop_id,
            zh_ui,
            restore_settings_hwnd: req.restore_settings_hwnd,
            win_rect,
            modal: None,
            hidden_for_editor: false,
        };

        if let Some(g) = gfx {
            sess.render_preview_layer(cfg, g);
        }
        if !sess.push_to_slint(cfg, true) {
            if let Some(ph) = sess.preview_hwnd {
                unsafe {
                    let _ = DestroyWindow(ph);
                }
            }
            return false;
        }

        wk_info!(
            trace_id,
            "layoutHub",
            "opened Slint dual-layer LayoutHub on monitor='{}' desktop='{:?}'",
            sess.active_monitor().id,
            sess.desktop_id
        );
        self.session = Some(sess);
        true
    }

    /// 配置热刷新（例如用户在设置页实时切换深浅色或动态主题色）时立刻联动已打开的 Hub
    pub fn refresh_theme_if_open(&self, cfg: &WindowKitConfig, gfx: Option<&SharedGraphics>) {
        let Some(sess) = self.session.as_ref() else {
            return;
        };
        if !sess.hidden_for_editor {
            if let Some(g) = gfx {
                sess.render_preview_layer(cfg, g);
            }
            sess.push_to_slint(cfg, false);
        }
    }

    /// 临时隐身（进入 1:1 全屏 LayoutEditor 编辑区域期间）
    pub fn suspend_for_editor(&mut self) {
        if let Some(sess) = self.session.as_mut() {
            sess.hidden_for_editor = true;
            sess.modal = None;
            hide_layout_hub();
            if let Some(ph) = sess.preview_hwnd {
                unsafe {
                    let _ = ShowWindow(ph, SW_HIDE);
                }
            }
        }
    }

    /// 从 1:1 全屏 LayoutEditor 返回后恢复双层共视 Hub，并刷新最新布局预览
    pub fn resume_from_editor(&mut self, cfg: &WindowKitConfig, gfx: Option<&SharedGraphics>) {
        if let Some(sess) = self.session.as_mut() {
            if !sess.hidden_for_editor {
                return;
            }
            sess.hidden_for_editor = false;
            sess.monitors = enumerate_monitors();
            if sess.active_monitor_idx >= sess.monitors.len() {
                sess.active_monitor_idx = 0;
            }
            if let Some(g) = gfx {
                sess.render_preview_layer(cfg, g);
            }
            sess.push_to_slint(cfg, true);
        }
    }

    /// 多显示器拔插 / 旋转 / 分辨率变化 / 虚拟桌面切换时自愈刷新
    pub fn on_topology_changed(&mut self, cfg: &WindowKitConfig, gfx: Option<&SharedGraphics>) {
        let Some(sess) = self.session.as_mut() else {
            return;
        };
        let prev_mon_id = sess
            .monitors
            .get(sess.active_monitor_idx)
            .map(|m| m.id.clone())
            .unwrap_or_default();
        let new_mons = enumerate_monitors();
        if new_mons.is_empty() {
            return;
        }
        sess.active_monitor_idx = new_mons
            .iter()
            .position(|m| m.id.eq_ignore_ascii_case(&prev_mon_id))
            .unwrap_or(0);
        sess.monitors = new_mons;

        let topo = enumerate_virtual_desktop_topology();
        sess.desktop_id = if topo.current_id.is_empty() {
            None
        } else {
            Some(topo.current_id.clone())
        };

        let active_mon = sess.active_monitor().clone();
        let safe_rect = ensure_window_rect_on_live_monitors(
            &active_mon.id,
            None,
            sess.win_rect.width(),
            sess.win_rect.height(),
            Some(&active_mon),
            &sess.monitors,
        );
        let moved = safe_rect != sess.win_rect;
        sess.win_rect = safe_rect;

        if !sess.hidden_for_editor {
            if let Some(g) = gfx {
                sess.render_preview_layer(cfg, g);
            }
            sess.push_to_slint(cfg, moved);
        }
    }

    pub fn close(&mut self, trace_id: &str) -> bool {
        let Some(sess) = self.session.take() else {
            return false;
        };
        let restore_settings = sess.restore_settings_hwnd.is_some();
        hide_layout_hub();
        if let Some(ph) = sess.preview_hwnd {
            unsafe {
                if IsWindow(ph).as_bool() {
                    let _ = ShowWindow(ph, SW_HIDE);
                    let _ = DestroyWindow(ph);
                }
            }
        }
        wk_debug!(trace_id, "layoutHub", "closed Slint LayoutHub cleanly");
        restore_settings
    }

    /// 统一处理 Slint LayoutHubWindow 回传的全部交互事件
    pub fn handle_slint_event(
        &mut self,
        ev: &SlintUiEvent,
        cfg: &mut WindowKitConfig,
        config_dir: &Path,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> HubEventOutcome {
        if self.session.is_none() {
            return HubEventOutcome::None;
        }

        if matches!(ev, SlintUiEvent::HubClosed) {
            let restore = self.close(trace_id);
            notify_host_hub_sync(cfg, config_dir, false, restore, trace_id);
            return HubEventOutcome::Closed {
                restore_settings: restore,
            };
        }

        let mut launch_editor: Option<OpenEditorRequest> = None;
        let mut mutated = false;
        let mut preview_dirty = false;
        let mut sync_ui = false;

        if let Some(sess) = self.session.as_mut() {
            match ev {
                SlintUiEvent::HubSelectMonitor(idx) => {
                    let uidx = (*idx).max(0) as usize;
                    if uidx < sess.monitors.len() && uidx != sess.active_monitor_idx {
                        sess.active_monitor_idx = uidx;
                        preview_dirty = true;
                        sync_ui = true;
                    }
                }
                SlintUiEvent::HubSelectDesktop(id) => {
                    let clean = id.trim().to_string();
                    sess.desktop_id = if clean.is_empty() { None } else { Some(clean) };
                    preview_dirty = true;
                    sync_ui = true;
                }
                SlintUiEvent::HubSelectLayout(layout_id) => {
                    sess.bind_layout_to_active_monitor(layout_id, cfg);
                    mutated = true;
                    preview_dirty = true;
                    sync_ui = true;
                }
                SlintUiEvent::HubOpenTemplateEditor(layout_id) => {
                    if let Some(lay) = cfg.find_layout(layout_id).cloned() {
                        if lay.built_in && lay.layout_type != ZoneLayoutType::None {
                            let dh = lay.default_horizontal;
                            let dv = lay.default_vertical;
                            sess.modal = Some(ActiveModal::TemplateEdit {
                                original_id: lay.id.clone(),
                                draft: lay,
                                default_horiz: dh,
                                default_vert: dv,
                            });
                            preview_dirty = true;
                            sync_ui = true;
                        }
                    }
                }
                SlintUiEvent::HubCustomMenuAction { action, layout_id } => match action.as_str() {
                    "edit" => {
                        if let Some(lay) = cfg.find_layout(layout_id).cloned() {
                            let dh = lay.default_horizontal;
                            let dv = lay.default_vertical;
                            sess.modal = Some(ActiveModal::CustomProps {
                                original_id: lay.id.clone(),
                                draft: lay,
                                default_horiz: dh,
                                default_vert: dv,
                            });
                            preview_dirty = true;
                            sync_ui = true;
                        }
                    }
                    "zones" => {
                        launch_editor = Some(sess.build_editor_request(
                            Some(layout_id.clone()),
                            None,
                            None,
                            false,
                        ));
                    }
                    "duplicate" => {
                        if let Some(new_id) = duplicate_layout_in_config(layout_id, cfg, sess.zh_ui)
                        {
                            sess.bind_layout_to_active_monitor(&new_id, cfg);
                            mutated = true;
                            preview_dirty = true;
                            sync_ui = true;
                        }
                    }
                    "delete" => {
                        if delete_custom_layout_in_config(layout_id, cfg) {
                            mutated = true;
                            preview_dirty = true;
                            sync_ui = true;
                        }
                    }
                    _ => {}
                },
                SlintUiEvent::HubOpenCreateModal => {
                    let next_name = next_custom_layout_name(&cfg.snap_zones.layouts, sess.zh_ui);
                    sess.modal = Some(ActiveModal::NewLayout {
                        name: next_name,
                        is_canvas: false,
                    });
                    sync_ui = true;
                }
                SlintUiEvent::HubDraftPreviewChanged {
                    zone_count,
                    show_spacing,
                    spacing,
                    highlight_distance,
                } => match sess.modal.as_mut() {
                    Some(ActiveModal::TemplateEdit { draft, .. }) => {
                        let max_c = if draft.layout_type == ZoneLayoutType::Focus {
                            10
                        } else {
                            12
                        };
                        draft.zone_count = (*zone_count).clamp(1, max_c) as usize;
                        draft.show_spacing = *show_spacing;
                        draft.gap = Some((*spacing).clamp(0, 64));
                        draft.highlight_distance = Some((*highlight_distance).clamp(0, 80));
                        draft.rebuild_template_geometry();
                        preview_dirty = true;
                        sync_ui = true;
                    }
                    Some(ActiveModal::CustomProps { draft, .. }) => {
                        draft.show_spacing = *show_spacing;
                        draft.gap = Some((*spacing).clamp(0, 64));
                        draft.highlight_distance = Some((*highlight_distance).clamp(0, 80));
                        preview_dirty = true;
                        sync_ui = true;
                    }
                    _ => {}
                },
                SlintUiEvent::HubModalSave {
                    name,
                    zone_count,
                    show_spacing,
                    spacing,
                    highlight_distance,
                    hotkey_index,
                    default_horizontal,
                    default_vertical,
                } => match sess.modal.take() {
                    Some(ActiveModal::TemplateEdit {
                        original_id,
                        mut draft,
                        ..
                    }) => {
                        let max_c = if draft.layout_type == ZoneLayoutType::Focus {
                            10
                        } else {
                            12
                        };
                        draft.zone_count = (*zone_count).clamp(1, max_c) as usize;
                        draft.show_spacing = *show_spacing;
                        draft.gap = Some((*spacing).clamp(0, 64));
                        draft.highlight_distance = Some((*highlight_distance).clamp(0, 80));
                        draft.rebuild_template_geometry();
                        commit_layout_draft(
                            &original_id,
                            draft,
                            *default_horizontal,
                            *default_vertical,
                            cfg,
                        );
                        sess.bind_layout_to_active_monitor(&original_id, cfg);
                        mutated = true;
                        preview_dirty = true;
                        sync_ui = true;
                    }
                    Some(ActiveModal::CustomProps {
                        original_id,
                        mut draft,
                        ..
                    }) => {
                        let trimmed = name.trim();
                        if !trimmed.is_empty() {
                            draft.name = trimmed.to_string();
                        }
                        draft.hotkey_digit = if *hotkey_index >= 1 && *hotkey_index <= 10 {
                            Some((*hotkey_index - 1) as u8)
                        } else {
                            None
                        };
                        draft.highlight_distance = Some((*highlight_distance).clamp(0, 80));
                        commit_layout_draft(
                            &original_id,
                            draft,
                            *default_horizontal,
                            *default_vertical,
                            cfg,
                        );
                        sess.bind_layout_to_active_monitor(&original_id, cfg);
                        mutated = true;
                        preview_dirty = true;
                        sync_ui = true;
                    }
                    _ => {}
                },
                SlintUiEvent::HubModalCancel => {
                    sess.modal = None;
                    preview_dirty = true;
                    sync_ui = true;
                }
                SlintUiEvent::HubModalDuplicate => {
                    let src_id = match &sess.modal {
                        Some(ActiveModal::TemplateEdit { original_id, .. })
                        | Some(ActiveModal::CustomProps { original_id, .. }) => {
                            Some(original_id.clone())
                        }
                        _ => None,
                    };
                    if let Some(id) = src_id {
                        sess.modal = None;
                        if let Some(new_id) = duplicate_layout_in_config(&id, cfg, sess.zh_ui) {
                            sess.bind_layout_to_active_monitor(&new_id, cfg);
                            mutated = true;
                            preview_dirty = true;
                            sync_ui = true;
                        }
                    }
                }
                SlintUiEvent::HubModalDelete => {
                    let del_id = match &sess.modal {
                        Some(ActiveModal::CustomProps { original_id, .. }) => {
                            Some(original_id.clone())
                        }
                        _ => None,
                    };
                    if let Some(id) = del_id {
                        sess.modal = None;
                        if delete_custom_layout_in_config(&id, cfg) {
                            mutated = true;
                            preview_dirty = true;
                            sync_ui = true;
                        }
                    }
                }
                SlintUiEvent::HubModalOpenFullscreenZones => {
                    if let Some(ActiveModal::CustomProps {
                        original_id,
                        draft,
                        default_horiz,
                        default_vert,
                    }) = sess.modal.take()
                    {
                        commit_layout_draft(&original_id, draft, default_horiz, default_vert, cfg);
                        mutated = true;
                        launch_editor = Some(sess.build_editor_request(
                            Some(original_id),
                            None,
                            None,
                            false,
                        ));
                    }
                }
                SlintUiEvent::HubCreateConfirm { name, is_canvas } => {
                    sess.modal = None;
                    let clean_name = if name.trim().is_empty() {
                        next_custom_layout_name(&cfg.snap_zones.layouts, sess.zh_ui)
                    } else {
                        name.trim().to_string()
                    };
                    let lay_type = if *is_canvas {
                        ZoneLayoutType::Canvas
                    } else {
                        ZoneLayoutType::Grid
                    };
                    launch_editor = Some(sess.build_editor_request(
                        None,
                        Some(lay_type),
                        Some(clean_name),
                        true,
                    ));
                }
                _ => {}
            }

            if launch_editor.is_none() {
                if preview_dirty {
                    if let Some(g) = gfx {
                        sess.render_preview_layer(cfg, g);
                    }
                }
                if sync_ui {
                    sess.push_to_slint(cfg, false);
                }
            }
        }

        if mutated {
            notify_host_hub_sync(cfg, config_dir, true, false, trace_id);
        }

        if let Some(req) = launch_editor {
            self.suspend_for_editor();
            return HubEventOutcome::LaunchFullEditor(req);
        }

        if mutated {
            HubEventOutcome::ConfigMutated
        } else {
            HubEventOutcome::None
        }
    }
}

impl Drop for LayoutHubManager {
    fn drop(&mut self) {
        let _ = self.close("drop");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. 纯数据配置变更助手（100% 可单测）
// ─────────────────────────────────────────────────────────────────────────────

pub fn next_custom_layout_name(layouts: &[ZoneLayout], zh_ui: bool) -> String {
    let prefix = if zh_ui {
        "自定义布局 "
    } else {
        "Custom Layout "
    };
    let mut n = 1usize;
    loop {
        let cand = format!("{prefix}{n}");
        if !layouts.iter().any(|l| l.name.eq_ignore_ascii_case(&cand)) {
            return cand;
        }
        n += 1;
    }
}

pub fn commit_layout_draft(
    original_id: &str,
    mut draft: ZoneLayout,
    default_horiz: bool,
    default_vert: bool,
    cfg: &mut WindowKitConfig,
) {
    if draft.name.trim().is_empty() {
        draft.name = original_id.to_string();
    }
    draft.default_horizontal = default_horiz;
    draft.default_vertical = default_vert;
    // 若设置了布局快捷键数字 0..=9 或设为横/竖屏默认布局，自动消除其他布局的冲突
    for other in &mut cfg.snap_zones.layouts {
        if other.id != original_id {
            if let Some(digit) = draft.hotkey_digit {
                if other.hotkey_digit == Some(digit) {
                    other.hotkey_digit = None;
                }
            }
            if default_horiz {
                other.default_horizontal = false;
            }
            if default_vert {
                other.default_vertical = false;
            }
        }
    }
    if let Some(slot) = cfg
        .snap_zones
        .layouts
        .iter_mut()
        .find(|l| l.id == original_id)
    {
        *slot = draft;
    }
}

pub fn duplicate_layout_in_config(
    source_id: &str,
    cfg: &mut WindowKitConfig,
    zh_ui: bool,
) -> Option<String> {
    let src = cfg.find_layout(source_id)?.clone();
    let ts = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0);
    let new_id = format!("custom_{ts}_{}", cfg.snap_zones.layouts.len());
    let suffix = if zh_ui { " (副本)" } else { " (Copy)" };
    let new_name = format!("{}{}", src.name.trim(), suffix);
    let dup = src.clone_as_custom(new_id.clone(), new_name);
    cfg.snap_zones.layouts.push(dup);
    Some(new_id)
}

pub fn delete_custom_layout_in_config(layout_id: &str, cfg: &mut WindowKitConfig) -> bool {
    let is_custom = cfg
        .find_layout(layout_id)
        .map(|l| !l.built_in)
        .unwrap_or(false);
    if !is_custom {
        return false;
    }
    cfg.snap_zones.layouts.retain(|l| l.id != layout_id);
    cfg.snap_zones
        .monitor_bindings
        .retain(|b| b.layout_id != layout_id);
    if cfg.snap_zones.active_layout_id == layout_id {
        cfg.snap_zones.active_layout_id = "template_priority_grid".to_string();
    }
    true
}

/// 将布局管理器最新状态写盘并零阻塞通知 C++ 宿主持久化与同步 React 设置页
pub fn notify_host_hub_sync(
    cfg: &WindowKitConfig,
    config_dir: &Path,
    saved: bool,
    restore_settings: bool,
    trace_id: &str,
) {
    if saved {
        let mut root = JsonValue::object();
        root.insert("action", JsonValue::String("hubSync".to_string()));
        root.insert("saved", JsonValue::Bool(true));
        root.insert(
            "activeLayoutId",
            JsonValue::String(cfg.snap_zones.active_layout_id.clone()),
        );
        root.insert("zoneGap", JsonValue::from_i32(cfg.snap_zones.zone_gap));
        root.insert(
            "layouts",
            JsonValue::Array(cfg.snap_zones.layouts.iter().map(|l| l.to_json()).collect()),
        );
        let bindings: Vec<JsonValue> = cfg
            .snap_zones
            .monitor_bindings
            .iter()
            .map(|b| {
                let mut o = JsonValue::object();
                o.insert("monitorId", JsonValue::String(b.monitor_id.clone()));
                o.insert("desktopId", JsonValue::String(b.desktop_id.clone()));
                o.insert("layoutId", JsonValue::String(b.layout_id.clone()));
                o
            })
            .collect();
        root.insert("monitorBindings", JsonValue::Array(bindings));

        let out_path = config_dir.join(FILE_LAYOUT_EDITOR_RESULT);
        if let Err(e) = write_text_file_atomic(&out_path, &root.to_string_pretty()) {
            wk_warn!(
                trace_id,
                "layoutHub",
                "failed to write hubSync result: {}",
                e
            );
        }
    }

    let cls = to_wide(HOST_MESSAGE_WINDOW_CLASS);
    if let Ok(host_hwnd) = unsafe { FindWindowW(PCWSTR(cls.as_ptr()), PCWSTR::null()) } {
        if !host_hwnd.0.is_null() {
            let msg_id = hub_host_result_msg_id();
            if msg_id != 0 {
                unsafe {
                    let _ = PostMessageW(
                        host_hwnd,
                        msg_id,
                        WPARAM(if saved { 1 } else { 0 }),
                        LPARAM(if restore_settings { 1 } else { 0 }),
                    );
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. HubSession：Slint 视图模型构建 + 底层 1:1 磨砂全屏预览层
// ─────────────────────────────────────────────────────────────────────────────

fn layout_display_name(lay: &ZoneLayout, zh: bool) -> String {
    if !lay.built_in {
        return lay.name.clone();
    }
    if zh {
        match lay.id.as_str() {
            "template_none" => "无布局".to_string(),
            "template_focus" => "焦点".to_string(),
            "template_columns" => "列".to_string(),
            "template_rows" => "行".to_string(),
            "template_grid" => "网格".to_string(),
            "template_priority_grid" => "优先级网格".to_string(),
            _ => lay.name.clone(),
        }
    } else {
        match lay.id.as_str() {
            "template_none" => "No layout".to_string(),
            "template_focus" => "Focus".to_string(),
            "template_columns" => "Columns".to_string(),
            "template_rows" => "Rows".to_string(),
            "template_grid" => "Grid".to_string(),
            "template_priority_grid" => "Priority Grid".to_string(),
            _ => lay.name.clone(),
        }
    }
}

fn layout_thumbs_overlap(thumbs: &[LayoutThumbData]) -> bool {
    for i in 0..thumbs.len() {
        for j in (i + 1)..thumbs.len() {
            let a = &thumbs[i];
            let b = &thumbs[j];
            let ox = (a.rx + a.rw).min(b.rx + b.rw) - a.rx.max(b.rx);
            let oy = (a.ry + a.rh).min(b.ry + b.rh) - a.ry.max(b.ry);
            if ox > 0.015 && oy > 0.015 {
                return true;
            }
        }
    }
    false
}

fn build_normalized_thumbs(lay: &ZoneLayout, default_gap: i32) -> (Vec<LayoutThumbData>, bool) {
    if lay.layout_type == ZoneLayoutType::None {
        return (Vec::new(), false);
    }
    // 1:1 对标微软 PowerToys FancyZones「焦点」3 层立体阶梯缩略排版（左上向右下错落分明）
    if lay.layout_type == ZoneLayoutType::Focus {
        let count = lay.zone_count.clamp(1, 5) as usize;
        let step_x = if count <= 3 { 0.085 } else { 0.062 };
        let step_y = if count <= 3 { 0.095 } else { 0.068 };
        let card_w = (0.72 - step_x * (count.saturating_sub(1) as f32)).clamp(0.46, 0.76);
        let card_h = (0.76 - step_y * (count.saturating_sub(1) as f32)).clamp(0.48, 0.78);
        let total_w = card_w + step_x * (count.saturating_sub(1) as f32);
        let total_h = card_h + step_y * (count.saturating_sub(1) as f32);
        let base_x = ((1.0 - total_w) * 0.5).max(0.06);
        let base_y = ((1.0 - total_h) * 0.5).max(0.06);
        let mut thumbs = Vec::with_capacity(count);
        for i in 0..count {
            let layer_tier = if count <= 1 {
                2
            } else if i == 0 {
                0
            } else if i + 1 == count {
                2
            } else {
                1
            };
            thumbs.push(LayoutThumbData {
                rx: base_x + step_x * i as f32,
                ry: base_y + step_y * i as f32,
                rw: card_w,
                rh: card_h,
                layer: layer_tier,
            });
        }
        return (thumbs, count >= 2);
    }

    let ref_work = RectI::from_xywh(0, 0, 1000, 600);
    // 网格类缩略图保持工业制图级精准细缝，不显得松垮
    let gap = if lay.show_spacing {
        (lay.effective_gap(default_gap) as f32 * 0.72).clamp(10.0, 26.0) as i32
    } else {
        0
    };
    let zones = lay.resolve(ref_work, gap);
    let total = zones.len();
    let thumbs: Vec<LayoutThumbData> = zones
        .into_iter()
        .enumerate()
        .map(|(i, z)| {
            let layer = if total <= 1 {
                2
            } else if i == 0 {
                0
            } else if i + 1 == total {
                2
            } else {
                1
            };
            LayoutThumbData {
                rx: (z.rect.left as f32 / 1000.0).clamp(0.0, 1.0),
                ry: (z.rect.top as f32 / 600.0).clamp(0.0, 1.0),
                rw: (z.rect.width() as f32 / 1000.0).clamp(0.02, 1.0),
                rh: (z.rect.height() as f32 / 600.0).clamp(0.02, 1.0),
                layer,
            }
        })
        .collect();
    let layered = layout_thumbs_overlap(&thumbs);
    (thumbs, layered)
}

impl HubSession {
    fn active_monitor(&self) -> &MonitorInfo {
        &self.monitors[self
            .active_monitor_idx
            .min(self.monitors.len().saturating_sub(1))]
    }

    fn scale(&self) -> f32 {
        (self.active_monitor().dpi as f32 / 96.0).max(1.0)
    }

    fn current_layout_for_preview<'a>(&'a self, cfg: &'a WindowKitConfig) -> ZoneLayout {
        match &self.modal {
            Some(ActiveModal::TemplateEdit { draft, .. })
            | Some(ActiveModal::CustomProps { draft, .. }) => return draft.clone(),
            _ => {}
        }
        let mon = self.active_monitor();
        let did = if cfg.snap_zones.per_virtual_desktop {
            self.desktop_id.as_deref()
        } else {
            None
        };
        cfg.resolve_layout_for_monitor(mon, did).clone()
    }

    fn bind_layout_to_active_monitor(&self, layout_id: &str, cfg: &mut WindowKitConfig) {
        let mon_id = self.active_monitor().id.clone();
        let desktop = if cfg.snap_zones.per_virtual_desktop {
            self.desktop_id.as_deref().unwrap_or("")
        } else {
            ""
        };
        cfg.snap_zones
            .bind_monitor_layout(&mon_id, desktop, layout_id);
    }

    fn build_editor_request(
        &self,
        layout_id: Option<String>,
        initial_mode: Option<ZoneLayoutType>,
        custom_name: Option<String>,
        is_new_custom: bool,
    ) -> OpenEditorRequest {
        OpenEditorRequest {
            monitor_id: Some(self.active_monitor().id.clone()),
            desktop_id: self.desktop_id.clone(),
            layout_id,
            restore_settings_hwnd: self.restore_settings_hwnd,
            ui_lang: if self.zh_ui { "zh" } else { "en" }.to_string(),
            return_to_hub: true,
            initial_mode,
            custom_name,
            is_new_custom,
        }
    }

    fn push_to_slint(&self, cfg: &WindowKitConfig, reposition_window: bool) -> bool {
        let active_mon = self.active_monitor();
        let did = if cfg.snap_zones.per_virtual_desktop {
            self.desktop_id.as_deref()
        } else {
            None
        };
        let active_layout_id = cfg.resolve_layout_for_monitor(active_mon, did).id.clone();

        let monitors: Vec<LayoutMonitorData> = self
            .monitors
            .iter()
            .enumerate()
            .map(|(i, m)| LayoutMonitorData {
                index: i as i32,
                badge_num: (i + 1).to_string(),
                resolution: format!("{} × {}", m.physical_rect.width(), m.physical_rect.height()),
                scale_text: format!("{}%", m.scale_percent),
                friendly_name: m.friendly_name.clone(),
                selected: i == self.active_monitor_idx,
            })
            .collect();

        let topo = enumerate_virtual_desktop_topology();
        let desktops: Vec<LayoutDesktopData> = topo
            .desktops
            .iter()
            .enumerate()
            .map(|(i, d)| {
                let label = if !d.name.trim().is_empty() {
                    d.name.clone()
                } else if self.zh_ui {
                    format!("桌面 {}", i + 1)
                } else {
                    format!("Desktop {}", i + 1)
                };
                let active = self
                    .desktop_id
                    .as_deref()
                    .map(|cur| cur.eq_ignore_ascii_case(&d.id))
                    .unwrap_or(d.is_current);
                LayoutDesktopData {
                    id: d.id.clone(),
                    label,
                    active,
                }
            })
            .collect();

        let mut template_layouts = Vec::new();
        let mut custom_layouts = Vec::new();
        for lay in &cfg.snap_zones.layouts {
            let (thumbs, is_layered) = build_normalized_thumbs(lay, cfg.snap_zones.zone_gap);
            let card = LayoutCardData {
                id: lay.id.clone(),
                name: layout_display_name(lay, self.zh_ui),
                is_builtin: lay.built_in,
                is_no_layout: lay.layout_type == ZoneLayoutType::None,
                is_layered,
                is_active: lay.id == active_layout_id,
                hotkey_badge: lay
                    .hotkey_digit
                    .map(|d| format!("#{d}"))
                    .unwrap_or_default(),
                thumbs,
            };
            if lay.built_in {
                template_layouts.push(card);
            } else {
                custom_layouts.push(card);
            }
        }

        let (
            modal_mode,
            modal_title,
            draft_layout_id,
            draft_name,
            draft_zone_count,
            draft_show_spacing,
            draft_spacing,
            draft_highlight_distance,
            draft_hotkey_index,
            draft_default_horizontal,
            draft_default_vertical,
            draft_create_canvas,
            draft_thumbs,
            draft_is_layered,
        ) = match &self.modal {
            None => (
                0,
                String::new(),
                String::new(),
                String::new(),
                3,
                true,
                cfg.snap_zones.zone_gap,
                cfg.snap_zones.edge_threshold_px,
                0,
                false,
                false,
                false,
                Vec::new(),
                false,
            ),
            Some(ActiveModal::TemplateEdit {
                original_id,
                draft,
                default_horiz,
                default_vert,
            }) => {
                let dname = layout_display_name(draft, self.zh_ui);
                let title = if self.zh_ui {
                    format!("编辑“{dname}”")
                } else {
                    format!("Edit \"{dname}\"")
                };
                let (d_thumbs, d_layered) = build_normalized_thumbs(draft, cfg.snap_zones.zone_gap);
                (
                    1,
                    title,
                    original_id.clone(),
                    dname,
                    draft.zone_count.max(1),
                    draft.show_spacing,
                    draft.gap.unwrap_or(cfg.snap_zones.zone_gap),
                    draft
                        .highlight_distance
                        .unwrap_or(cfg.snap_zones.edge_threshold_px),
                    0,
                    *default_horiz,
                    *default_vert,
                    false,
                    d_thumbs,
                    d_layered,
                )
            }
            Some(ActiveModal::CustomProps {
                original_id,
                draft,
                default_horiz,
                default_vert,
            }) => {
                let title = if self.zh_ui {
                    format!("编辑“{}”", draft.name)
                } else {
                    format!("Edit \"{}\"", draft.name)
                };
                let (d_thumbs, d_layered) = build_normalized_thumbs(draft, cfg.snap_zones.zone_gap);
                (
                    2,
                    title,
                    original_id.clone(),
                    draft.name.clone(),
                    draft.zone_count.max(1),
                    draft.show_spacing,
                    draft.gap.unwrap_or(cfg.snap_zones.zone_gap),
                    draft
                        .highlight_distance
                        .unwrap_or(cfg.snap_zones.edge_threshold_px),
                    draft.hotkey_digit.map(|d| (d as i32) + 1).unwrap_or(0),
                    *default_horiz,
                    *default_vert,
                    false,
                    d_thumbs,
                    d_layered,
                )
            }
            Some(ActiveModal::NewLayout { name, is_canvas }) => {
                let title = if self.zh_ui {
                    "新建布局".to_string()
                } else {
                    "Create new layout".to_string()
                };
                (
                    3,
                    title,
                    String::new(),
                    name.clone(),
                    3,
                    true,
                    cfg.snap_zones.zone_gap,
                    cfg.snap_zones.edge_threshold_px,
                    0,
                    false,
                    false,
                    *is_canvas,
                    Vec::new(),
                    false,
                )
            }
        };

        show_or_update_layout_hub(LayoutHubViewState {
            theme: SlintThemeSpec {
                dark: cfg.is_dark_theme(),
                accent_hex: cfg.accent_color.clone(),
            },
            window_rect: self.win_rect,
            reposition_window,
            zh_ui: self.zh_ui,
            monitors,
            desktops,
            template_layouts,
            custom_layouts,
            modal_mode,
            modal_title,
            draft_layout_id,
            draft_name,
            draft_zone_count: draft_zone_count as i32,
            draft_show_spacing,
            draft_spacing,
            draft_highlight_distance,
            draft_hotkey_index,
            draft_default_horizontal,
            draft_default_vertical,
            draft_create_canvas,
            draft_is_layered,
            draft_thumbs,
        })
    }

    /// 渲染 Layer-1 底层 1:1 全屏通透磨砂实时分区预览（对标参考截图，绝不阻挡桌面任务栏）
    fn render_preview_layer(&self, cfg: &WindowKitConfig, gfx: &SharedGraphics) {
        let Some(ph) = self.preview_hwnd else {
            return;
        };
        let mon = self.active_monitor();
        let work = mon.work_rect;
        let w = work.width();
        let h = work.height();
        if w <= 0 || h <= 0 {
            return;
        }
        let layout = self.current_layout_for_preview(cfg);
        let gap = layout.effective_gap(cfg.snap_zones.zone_gap);
        let zones = layout.resolve(work, gap);

        let dark = cfg.is_dark_theme();
        let accent = parse_hex_color(&cfg.accent_color, [0.23, 0.51, 0.96]);
        let s = self.scale();
        let num_fmt = gfx.ui_text_format(40.0 * s, true, true).ok();
        let is_focus = layout.layout_type == ZoneLayoutType::Focus;

        let _ = render_layered_window_d2d(gfx, ph, work.left, work.top, w, h, |rt| unsafe {
            // 1:1 复刻微软 PowerToys FancyZones 高通透清澈玻璃背景 + 锐利 2px 主题色细边
            let fill_base = if dark {
                rgba(0.065, 0.078, 0.105, 0.44)
            } else {
                rgba(0.975, 0.982, 0.992, 0.50)
            };
            let stroke_col = rgb_a(accent, if dark { 0.88 } else { 0.84 });
            let outer_shadow = if dark {
                rgba(0.0, 0.0, 0.0, 0.36)
            } else {
                rgba(0.08, 0.12, 0.20, 0.10)
            };
            let text_col = if dark {
                rgba(0.96, 0.98, 1.0, 0.58)
            } else {
                rgba(0.14, 0.18, 0.25, 0.42)
            };

            let (Ok(b_fill), Ok(b_stroke), Ok(b_shadow), Ok(b_text)) = (
                rt.CreateSolidColorBrush(&fill_base, None),
                rt.CreateSolidColorBrush(&stroke_col, None),
                rt.CreateSolidColorBrush(&outer_shadow, None),
                rt.CreateSolidColorBrush(&text_col, None),
            ) else {
                return;
            };

            if layout.layout_type == ZoneLayoutType::None || zones.is_empty() {
                let r = rect_f(4.0, 4.0, w as f32 - 4.0, h as f32 - 4.0);
                rt.DrawRectangle(&r, &b_stroke, 1.5 * s, None);
                return;
            }

            let stroke_w = 2.0 * s;
            for (idx, z) in zones.iter().enumerate() {
                let lr = rect_f(
                    (z.rect.left - work.left) as f32 + 1.0,
                    (z.rect.top - work.top) as f32 + 1.0,
                    (z.rect.right - work.left) as f32 - 1.0,
                    (z.rect.bottom - work.top) as f32 - 1.0,
                );
                if is_focus {
                    // 焦点叠层：逐层递增不透明度与微阴影轮廓，3 层前后空间层次分明
                    let layer_a = if dark {
                        0.36 + (idx as f32 * 0.08).min(0.28)
                    } else {
                        0.44 + (idx as f32 * 0.08).min(0.26)
                    };
                    let layer_col = if dark {
                        rgba(0.075, 0.090, 0.122, layer_a)
                    } else {
                        rgba(0.98, 0.985, 0.995, layer_a)
                    };
                    rt.DrawRectangle(&lr, &b_shadow, stroke_w + 3.0 * s, None);
                    if let Ok(bf) = rt.CreateSolidColorBrush(&layer_col, None) {
                        rt.FillRectangle(&lr, &bf);
                    }
                } else {
                    rt.FillRectangle(&lr, &b_fill);
                }
                rt.DrawRectangle(&lr, &b_stroke, stroke_w, None);
                if let Some(fmt) = &num_fmt {
                    let label = to_wide(&(z.index + 1).to_string());
                    rt.DrawText(
                        &label[..label.len().saturating_sub(1)],
                        fmt,
                        &lr,
                        &b_text,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        DWRITE_MEASURING_MODE_NATURAL,
                    );
                }
            }
        });

        unsafe {
            let _ = SetWindowPos(ph, HWND_TOPMOST, work.left, work.top, w, h, SWP_NOACTIVATE);
            let _ = ShowWindow(ph, SW_SHOWNOACTIVATE);
        }
        // 确保前景 Slint LayoutHubWindow 始终严格位于底层磨砂预览正上方
        raise_layout_hub_above_preview();
    }
}

fn create_hub_preview_window() -> Option<HWND> {
    static ONCE: Once = Once::new();
    let hinst = unsafe { HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0) };
    let cls = to_wide(LAYOUT_HUB_PREVIEW_CLASS);
    ONCE.call_once(|| unsafe {
        let wc = WNDCLASSEXW {
            cbSize: size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(hub_preview_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&wc);
    });
    unsafe {
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

unsafe extern "system" fn hub_preview_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    DefWindowProcW(hwnd, msg, wparam, lparam)
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. 单元测试
// ─────────────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn duplicate_and_delete_custom_layout_preserve_builtin_templates() {
        let mut cfg = WindowKitConfig::default();
        let initial_len = cfg.snap_zones.layouts.len();
        let new_id = duplicate_layout_in_config("template_focus", &mut cfg, true).expect("dup");
        assert_eq!(cfg.snap_zones.layouts.len(), initial_len + 1);
        let dup = cfg.find_layout(&new_id).expect("find dup");
        assert!(!dup.built_in);
        assert_eq!(dup.layout_type, ZoneLayoutType::Canvas);
        assert!(!dup.zones.is_empty());

        // 内置模板严禁被删除
        assert!(!delete_custom_layout_in_config("template_columns", &mut cfg));
        // 自定义副本可删除
        assert!(delete_custom_layout_in_config(&new_id, &mut cfg));
        assert_eq!(cfg.snap_zones.layouts.len(), initial_len);
    }

    #[test]
    fn commit_layout_draft_resolves_hotkey_digit_conflicts_and_orientation_defaults() {
        let mut cfg = WindowKitConfig::default();
        let id1 = duplicate_layout_in_config("template_columns", &mut cfg, true).unwrap();
        let id2 = duplicate_layout_in_config("template_rows", &mut cfg, true).unwrap();

        let mut d1 = cfg.find_layout(&id1).unwrap().clone();
        d1.hotkey_digit = Some(3);
        commit_layout_draft(&id1, d1, true, false, &mut cfg);
        assert_eq!(cfg.find_layout(&id1).unwrap().hotkey_digit, Some(3));
        assert!(cfg.find_layout(&id1).unwrap().default_horizontal);

        // 将 id2 也设为快捷键 3 -> id1 的 3 自动清除避免冲突
        let mut d2 = cfg.find_layout(&id2).unwrap().clone();
        d2.hotkey_digit = Some(3);
        commit_layout_draft(&id2, d2, false, true, &mut cfg);
        assert_eq!(cfg.find_layout(&id1).unwrap().hotkey_digit, None);
        assert_eq!(cfg.find_layout(&id2).unwrap().hotkey_digit, Some(3));
        assert!(cfg.find_layout(&id2).unwrap().default_vertical);
    }
}
