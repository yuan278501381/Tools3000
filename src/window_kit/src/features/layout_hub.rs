// ─────────────────────────────────────────────────────────────────────────────
// features/layout_hub.rs — SnapZones v2 纯 Rust「双层共视」布局管理器
//
// 世界级架构亮点（100% 复刻并超越 PowerToys FancyZones 13 图精髓）:
//   1. 双层共视（所见即所得）：打开瞬间底层 1:1 铺满当前显示器真实工作区（任务栏可见），
//      前景居中悬浮轻量 Direct2D 硬件加速管理窗（< 6 MB，无 WebView，亚毫秒冷启动）。
//   2. 顶部多屏物理卡片（编号/真实分辨率/缩放比）+ 当前虚拟桌面徽章；点屏即刻切换底层预览屏。
//   3. 6 大参数化模板（无布局/焦点/列/行/网格/优先级网格）+ 自定义区 + 3 大原生内嵌弹窗：
//      - 模板参数实时弹窗：区域数、区域周围的空间开关与像素滑块、突出显示距离、横/竖屏默认星标、复制；
//      - 自定义属性弹窗：名称(支持中文 IME)、布局快捷键 0–9、居中缩略图直入 1:1 编辑区域、复制/删除；
//      - 新建布局弹窗：名称 + 网格/画布双选卡，点创建无缝直进 1:1 全屏 LayoutEditor。
//   4. 拔屏/旋转/改分辨率/切换虚拟桌面自动自愈，退出时 100% 释放渲染目标归还物理内存。
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{
    parse_hex_color, WindowKitConfig, ZoneLayout, ZoneLayoutType,
};
use crate::core::constants::{
    FILE_LAYOUT_EDITOR_RESULT, HOST_EDITOR_RESULT_MSG_NAME, HOST_MESSAGE_WINDOW_CLASS,
    LAYOUT_HUB_WINDOW_CLASS, WM_WK_INTERNAL_HUB_EVENT,
};
use crate::core::geometry::{RectI, ResolvedZone};
use crate::core::json::JsonValue;
use crate::core::topology::{
    apply_cross_os_popup_chrome, ensure_window_rect_on_live_monitors,
    enumerate_virtual_desktop_topology, MonitorOrientation,
};
use crate::core::win_utils::{
    enumerate_monitors, monitor_for_point, to_wide, write_text_file_atomic, MonitorInfo,
};
use crate::features::layout_editor::OpenEditorRequest;
use crate::render::d2d::{
    rect_f, render_layered_window_d2d, rgb_a, rgba, rounded_rect, SharedGraphics,
};
use crate::{wk_debug, wk_info, wk_warn};
use std::ffi::c_void;
use std::mem::size_of;
use std::path::Path;
use std::sync::atomic::{AtomicI64, AtomicIsize, AtomicU32, Ordering};
use std::sync::Once;
use std::time::{SystemTime, UNIX_EPOCH};
use windows::core::{Interface, PCWSTR};
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Direct2D::Common::{D2D_POINT_2F, D2D_SIZE_U};
use windows::Win32::Graphics::Direct2D::{
    ID2D1HwndRenderTarget, ID2D1RenderTarget, D2D1_DRAW_TEXT_OPTIONS_CLIP,
};
use windows::Win32::Graphics::DirectWrite::DWRITE_MEASURING_MODE_NATURAL;
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::Input::KeyboardAndMouse::{
    ReleaseCapture, SetCapture, SetFocus, VK_BACK, VK_ESCAPE, VK_RETURN,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, FindWindowW, GetClientRect, GetCursorPos,
    GetWindowRect, IsWindow, LoadCursorW, PostMessageW, RegisterClassExW, RegisterWindowMessageW,
    SetCursor, SetForegroundWindow, SetWindowPos, ShowWindow, CS_DBLCLKS, CS_HREDRAW, CS_VREDRAW,
    HCURSOR, HTCLIENT, HWND_TOPMOST, IDC_ARROW, IDC_HAND, IDC_IBEAM, IDC_SIZEALL, SWP_NOACTIVATE,
    SWP_NOZORDER, SW_HIDE, SW_SHOW, SW_SHOWNOACTIVATE, WM_CHAR, WM_CLOSE, WM_KEYDOWN,
    WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_PAINT, WM_RBUTTONDOWN,
    WM_SETCURSOR, WNDCLASSEXW, WS_CLIPCHILDREN, WS_EX_LAYERED, WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW,
    WS_EX_TOPMOST, WS_EX_TRANSPARENT, WS_POPUP,
};

const LAYOUT_HUB_PREVIEW_CLASS: &str = "Tools3000_WindowKit_LayoutHubPreview";

static HUB_CONTROL_HWND: AtomicIsize = AtomicIsize::new(0);
static HUB_CURSOR_KIND: AtomicU32 = AtomicU32::new(0);
static HUB_LATEST_PT: AtomicI64 = AtomicI64::new(0);
static HUB_MOVE_PENDING: AtomicU32 = AtomicU32::new(0);
static HUB_HOST_RESULT_MSG: AtomicU32 = AtomicU32::new(0);

const CURSOR_ARROW: u32 = 0;
const CURSOR_HAND: u32 = 1;
const CURSOR_SIZEALL: u32 = 2;
const CURSOR_IBEAM: u32 = 3;

const HUB_EVT_MOUSEMOVE: usize = 1;
const HUB_EVT_LBUTTONDOWN: usize = 2;
const HUB_EVT_LBUTTONUP: usize = 3;
const HUB_EVT_RBUTTONDOWN: usize = 4;
const HUB_EVT_WHEEL: usize = 5;
const HUB_EVT_KEYDOWN: usize = 6;
const HUB_EVT_CHAR: usize = 7;
const HUB_EVT_PAINT: usize = 8;
const HUB_EVT_CLOSE: usize = 9;

fn pack_xy(x: i32, y: i32) -> i64 {
    ((x as u32 as u64) | ((y as u32 as u64) << 32)) as i64
}

fn unpack_xy(v: i64) -> (i32, i32) {
    let u = v as u64;
    ((u & 0xFFFF_FFFF) as u32 as i32, (u >> 32) as u32 as i32)
}

fn unpack_lparam_xy(lp: isize) -> (i32, i32) {
    let x = (lp as u32 & 0xFFFF) as i16 as i32;
    let y = (((lp as u32) >> 16) & 0xFFFF) as i16 as i32;
    (x, y)
}

fn hub_cursor_handle(kind: u32) -> HCURSOR {
    unsafe {
        let id = match kind {
            CURSOR_HAND => IDC_HAND,
            CURSOR_SIZEALL => IDC_SIZEALL,
            CURSOR_IBEAM => IDC_IBEAM,
            _ => IDC_ARROW,
        };
        LoadCursorW(None, id).unwrap_or_default()
    }
}

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
// 2. 弹窗与交互命中的状态机模型
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, Eq)]
enum HitTarget {
    TitleBarDrag,
    BtnMaximize,
    BtnClose,
    MonitorCard(usize),
    LayoutCard(String),
    LayoutEditBtn(String),
    BtnNewLayout,
    ContextMenuItem { layout_id: String, action: ContextAction },
    ModalBackdrop,
    ModalCloseOrCancel,
    ModalSaveOrCreate,
    ModalDuplicate,
    ModalDelete,
    ModalEditZones,
    ModalNameInput,
    ModalZoneCountMinus,
    ModalZoneCountPlus,
    ModalShowSpacingToggle,
    ModalSpacingMinus,
    ModalSpacingPlus,
    ModalSpacingSlider,
    ModalHighlightMinus,
    ModalHighlightPlus,
    ModalHighlightSlider,
    ModalHotkeyPrev,
    ModalHotkeyNext,
    ModalDefaultHoriz,
    ModalDefaultVert,
    ModalNewTypeGrid,
    ModalNewTypeCanvas,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ContextAction {
    EditProps,
    EditZones,
    Duplicate,
    Delete,
}

#[derive(Debug, Clone)]
struct HitBox {
    rect: RectI, // 相对前景窗客户端像素
    target: HitTarget,
}

#[derive(Debug, Clone)]
struct ContextMenuState {
    layout_id: String,
    anchor_x: i32,
    anchor_y: i32,
}

#[derive(Debug, Clone)]
enum ActiveModal {
    /// 模板参数实时调节窗（图 9/10/11）
    TemplateEdit {
        original_id: String,
        draft: ZoneLayout,
        default_horiz: bool,
        default_vert: bool,
    },
    /// 自定义布局属性窗（图 4）
    CustomProps {
        original_id: String,
        draft: ZoneLayout,
        name_focused: bool,
        default_horiz: bool,
        default_vert: bool,
    },
    /// 新建布局弹窗（图 3）
    NewLayout {
        name: String,
        layout_type: ZoneLayoutType, // Grid 或 Canvas
        name_focused: bool,
    },
}

#[derive(Debug, Clone, Copy)]
enum HubDragMode {
    WindowMove {
        start_screen_x: i32,
        start_screen_y: i32,
        orig_win_x: i32,
        orig_win_y: i32,
    },
    SpacingSlider,
    HighlightSlider,
}

struct HubSession {
    hub_hwnd: HWND,
    preview_hwnd: Option<HWND>,
    rt: Option<ID2D1HwndRenderTarget>,
    monitors: Vec<MonitorInfo>,
    active_monitor_idx: usize,
    desktop_id: Option<String>,
    desktop_label: String,
    zh_ui: bool,
    restore_settings_hwnd: Option<isize>,
    win_rect: RectI,
    restore_rect: Option<RectI>,
    is_maximized: bool,
    scroll_y: i32,
    max_scroll_y: i32,
    hovered: Option<HitTarget>,
    context_menu: Option<ContextMenuState>,
    modal: Option<ActiveModal>,
    drag: Option<HubDragMode>,
    hit_boxes: Vec<HitBox>,
    spacing_slider_track: Option<RectI>,
    highlight_slider_track: Option<RectI>,
    hidden_for_editor: bool,
}

pub struct LayoutHubManager {
    control_hwnd: HWND,
    session: Option<HubSession>,
}

impl LayoutHubManager {
    pub fn new(control_hwnd: HWND) -> Self {
        Self {
            control_hwnd,
            session: None,
        }
    }

    pub fn set_control_hwnd(&mut self, hwnd: HWND) {
        self.control_hwnd = hwnd;
        HUB_CONTROL_HWND.store(hwnd.0 as isize, Ordering::Release);
    }

    pub fn is_open(&self) -> bool {
        self.session.is_some()
    }

    pub fn is_visible(&self) -> bool {
        self.session
            .as_ref()
            .map(|s| !s.hidden_for_editor)
            .unwrap_or(false)
    }

    /// 打开双层共视布局管理器（底层当前显示器 1:1 实时磨砂预览 + 前景管理窗）
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
                sess.render_hub_window(cfg, g);
            }
            unsafe {
                let _ = ShowWindow(sess.hub_hwnd, SW_SHOW);
                let _ = SetForegroundWindow(sess.hub_hwnd);
                let _ = SetFocus(sess.hub_hwnd);
            }
            return true;
        }

        let Some(g) = gfx else {
            wk_warn!(trace_id, "layoutHub", "SharedGraphics unavailable");
            return false;
        };
        let monitors = enumerate_monitors();
        if monitors.is_empty() {
            return false;
        }

        // 确定初始目标显示器
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
        let desktop_label = desktop_id
            .as_deref()
            .and_then(|id| {
                topo.desktops
                    .iter()
                    .find(|d| d.id.eq_ignore_ascii_case(id))
                    .map(|d| d.name.clone())
            })
            .unwrap_or_default();

        // 计算前景窗物理尺寸（按当前屏 DPI 自适应并严格限制在工作区内）
        let scale = (mon.dpi as f32 / 96.0).max(1.0);
        let work = mon.work_rect;
        let ideal_w = ((980.0 * scale).round() as i32).min(work.width() - (64.0 * scale) as i32).max(620);
        let ideal_h = ((660.0 * scale).round() as i32).min(work.height() - (64.0 * scale) as i32).max(460);
        let wx = work.left + (work.width() - ideal_w) / 2;
        let wy = work.top + (work.height() - ideal_h) / 2;
        let win_rect = RectI::from_xywh(wx, wy, ideal_w, ideal_h);

        let preview_hwnd = create_hub_preview_window();
        let Some(hub_hwnd) = create_hub_window(win_rect) else {
            if let Some(ph) = preview_hwnd {
                unsafe {
                    let _ = DestroyWindow(ph);
                }
            }
            return false;
        };

        let dark = cfg.is_dark_theme();
        apply_cross_os_popup_chrome(
            hub_hwnd,
            dark,
            win_rect.width(),
            win_rect.height(),
            (10.0 * scale).round() as i32,
        );

        let rt = g
            .create_hwnd_target(
                hub_hwnd,
                win_rect.width().max(1) as u32,
                win_rect.height().max(1) as u32,
                96, // 逻辑像素即真实物理像素，直接按 scale 绘制保持极锐利 ClearType
            )
            .ok();

        let zh_ui = !req.ui_lang.trim().to_ascii_lowercase().starts_with("en");
        let mut sess = HubSession {
            hub_hwnd,
            preview_hwnd,
            rt,
            monitors,
            active_monitor_idx: active_idx,
            desktop_id,
            desktop_label,
            zh_ui,
            restore_settings_hwnd: req.restore_settings_hwnd,
            win_rect,
            restore_rect: None,
            is_maximized: false,
            scroll_y: 0,
            max_scroll_y: 0,
            hovered: None,
            context_menu: None,
            modal: None,
            drag: None,
            hit_boxes: Vec::new(),
            spacing_slider_track: None,
            highlight_slider_track: None,
            hidden_for_editor: false,
        };

        sess.render_preview_layer(cfg, g);
        sess.render_hub_window(cfg, g);

        unsafe {
            let _ = ShowWindow(hub_hwnd, SW_SHOW);
            let _ = SetForegroundWindow(hub_hwnd);
            let _ = SetFocus(hub_hwnd);
        }

        wk_info!(
            trace_id,
            "layoutHub",
            "opened dual-layer LayoutHub on monitor='{}' desktop='{:?}'",
            sess.active_monitor().id,
            sess.desktop_id
        );
        self.session = Some(sess);
        true
    }

    /// 临时隐身（进入 1:1 全屏 LayoutEditor 编辑区域期间）
    pub fn suspend_for_editor(&mut self) {
        if let Some(sess) = self.session.as_mut() {
            sess.hidden_for_editor = true;
            sess.context_menu = None;
            sess.modal = None;
            sess.drag = None;
            unsafe {
                if let Some(ph) = sess.preview_hwnd {
                    let _ = ShowWindow(ph, SW_HIDE);
                }
                let _ = ShowWindow(sess.hub_hwnd, SW_HIDE);
            }
        }
    }

    /// 从 1:1 全屏 LayoutEditor 保存或取消后无缝回到双层共视 Hub，并刷新最新布局预览
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
                sess.render_hub_window(cfg, g);
            }
            unsafe {
                let _ = ShowWindow(sess.hub_hwnd, SW_SHOW);
                let _ = SetForegroundWindow(sess.hub_hwnd);
                let _ = SetFocus(sess.hub_hwnd);
            }
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
        sess.desktop_label = sess
            .desktop_id
            .as_deref()
            .and_then(|id| {
                topo.desktops
                    .iter()
                    .find(|d| d.id.eq_ignore_ascii_case(id))
                    .map(|d| d.name.clone())
            })
            .unwrap_or_default();

        // 若前景窗所在显示器被拔掉，自动拉回存活显示器可见区
        let active_mon = sess.active_monitor().clone();
        let safe_rect = ensure_window_rect_on_live_monitors(
            &active_mon.id,
            None,
            sess.win_rect.width(),
            sess.win_rect.height(),
            Some(&active_mon),
            &sess.monitors,
        );
        if safe_rect != sess.win_rect {
            sess.win_rect = safe_rect;
            unsafe {
                let _ = SetWindowPos(
                    sess.hub_hwnd,
                    HWND_TOPMOST,
                    safe_rect.left,
                    safe_rect.top,
                    safe_rect.width(),
                    safe_rect.height(),
                    SWP_NOACTIVATE,
                );
            }
            sess.resize_render_target(gfx);
        }

        if !sess.hidden_for_editor {
            if let Some(g) = gfx {
                sess.render_preview_layer(cfg, g);
                sess.render_hub_window(cfg, g);
            }
        }
    }

    pub fn close(&mut self, trace_id: &str) -> bool {
        let Some(sess) = self.session.take() else {
            return false;
        };
        let restore_settings = sess.restore_settings_hwnd.is_some();
        let hub_hwnd = sess.hub_hwnd;
        let preview_hwnd = sess.preview_hwnd;
        drop(sess); // 先释放 ID2D1HwndRenderTarget
        unsafe {
            let _ = ReleaseCapture();
            if let Some(ph) = preview_hwnd {
                if IsWindow(ph).as_bool() {
                    let _ = ShowWindow(ph, SW_HIDE);
                    let _ = DestroyWindow(ph);
                }
            }
            if IsWindow(hub_hwnd).as_bool() {
                let _ = ShowWindow(hub_hwnd, SW_HIDE);
                let _ = DestroyWindow(hub_hwnd);
            }
        }
        HUB_CURSOR_KIND.store(CURSOR_ARROW, Ordering::Release);
        wk_debug!(trace_id, "layoutHub", "closed LayoutHub cleanly");
        restore_settings
    }

    /// 统一事件分发入口（由 main.rs `WM_WK_INTERNAL_HUB_EVENT` 调用）
    pub fn handle_event(
        &mut self,
        evt: usize,
        lparam: isize,
        cfg: &mut WindowKitConfig,
        config_dir: &Path,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> HubEventOutcome {
        if self.session.is_none() {
            return HubEventOutcome::None;
        }

        if evt == HUB_EVT_CLOSE {
            let restore = self.close(trace_id);
            notify_host_hub_sync(cfg, config_dir, false, restore, trace_id);
            return HubEventOutcome::Closed {
                restore_settings: restore,
            };
        }

        let mut close_now = false;
        let mut launch_editor: Option<OpenEditorRequest> = None;
        let mut mutated = false;
        let mut preview_dirty = false;
        let mut need_redraw = false;

        if let Some(sess) = self.session.as_mut() {
            match evt {
                HUB_EVT_PAINT => {
                    need_redraw = true;
                }
                HUB_EVT_MOUSEMOVE => {
                    HUB_MOVE_PENDING.store(0, Ordering::Release);
                    let (lx, ly) = unpack_xy(HUB_LATEST_PT.load(Ordering::Acquire));
                    if let Some(drag) = sess.drag {
                        match drag {
                            HubDragMode::WindowMove {
                                start_screen_x,
                                start_screen_y,
                                orig_win_x,
                                orig_win_y,
                            } => {
                                let mut pt = POINT::default();
                                if unsafe { GetCursorPos(&mut pt) }.is_ok() {
                                    let nx = orig_win_x + (pt.x - start_screen_x);
                                    let ny = orig_win_y + (pt.y - start_screen_y);
                                    let w = sess.win_rect.width();
                                    let h = sess.win_rect.height();
                                    sess.win_rect = RectI::from_xywh(nx, ny, w, h);
                                    unsafe {
                                        let _ = SetWindowPos(
                                            sess.hub_hwnd,
                                            HWND::default(),
                                            nx,
                                            ny,
                                            0,
                                            0,
                                            SWP_NOZORDER
                                                | SWP_NOACTIVATE
                                                | windows::Win32::UI::WindowsAndMessaging::SWP_NOSIZE,
                                        );
                                    }
                                }
                            }
                            HubDragMode::SpacingSlider => {
                                if sess.update_slider_from_x(lx, true) {
                                    preview_dirty = true;
                                    need_redraw = true;
                                }
                            }
                            HubDragMode::HighlightSlider => {
                                if sess.update_slider_from_x(lx, false) {
                                    need_redraw = true;
                                }
                            }
                        }
                    } else {
                        let hit = sess.hit_test(lx, ly);
                        let cursor = match &hit {
                            Some(HitTarget::TitleBarDrag) => CURSOR_SIZEALL,
                            Some(HitTarget::ModalNameInput) => CURSOR_IBEAM,
                            Some(HitTarget::ModalBackdrop) | None => CURSOR_ARROW,
                            Some(_) => CURSOR_HAND,
                        };
                        sess.set_cursor(cursor);
                        if hit != sess.hovered {
                            sess.hovered = hit;
                            need_redraw = true;
                        }
                    }
                }
                HUB_EVT_LBUTTONUP => {
                    if sess.drag.take().is_some() {
                        unsafe {
                            let _ = ReleaseCapture();
                        }
                        need_redraw = true;
                    }
                }
                HUB_EVT_WHEEL => {
                    if sess.modal.is_none() && sess.context_menu.is_none() {
                        let delta = (lparam as i16) as i32;
                        let step = if delta > 0 { -48 } else { 48 };
                        let next = (sess.scroll_y + step).clamp(0, sess.max_scroll_y);
                        if next != sess.scroll_y {
                            sess.scroll_y = next;
                            need_redraw = true;
                        }
                    }
                }
                HUB_EVT_RBUTTONDOWN => {
                    let (lx, ly) = unpack_lparam_xy(lparam);
                    if sess.modal.is_none() {
                        sess.context_menu = None;
                        if let Some(HitTarget::LayoutCard(id)) = sess.hit_test(lx, ly) {
                            if let Some(lay) = cfg.find_layout(&id) {
                                if !lay.built_in {
                                    sess.context_menu = Some(ContextMenuState {
                                        layout_id: id,
                                        anchor_x: lx,
                                        anchor_y: ly,
                                    });
                                    need_redraw = true;
                                }
                            }
                        }
                    }
                }
                HUB_EVT_LBUTTONDOWN => {
                    let (lx, ly) = unpack_lparam_xy(lparam);
                    let hit = sess.hit_test(lx, ly);
                    match hit {
                        Some(HitTarget::TitleBarDrag) => {
                            if !sess.is_maximized {
                                let mut pt = POINT::default();
                                let mut wr = RECT::default();
                                unsafe {
                                    let _ = GetCursorPos(&mut pt);
                                    let _ = GetWindowRect(sess.hub_hwnd, &mut wr);
                                    let _ = SetCapture(sess.hub_hwnd);
                                }
                                sess.drag = Some(HubDragMode::WindowMove {
                                    start_screen_x: pt.x,
                                    start_screen_y: pt.y,
                                    orig_win_x: wr.left,
                                    orig_win_y: wr.top,
                                });
                            }
                        }
                        Some(HitTarget::BtnClose) => {
                            close_now = true;
                        }
                        Some(HitTarget::BtnMaximize) => {
                            sess.toggle_maximize(cfg, gfx);
                            need_redraw = true;
                        }
                        Some(HitTarget::MonitorCard(idx)) => {
                            sess.context_menu = None;
                            if idx < sess.monitors.len() && idx != sess.active_monitor_idx {
                                sess.active_monitor_idx = idx;
                                preview_dirty = true;
                                need_redraw = true;
                            }
                        }
                        Some(HitTarget::LayoutCard(id)) => {
                            sess.context_menu = None;
                            sess.bind_layout_to_active_monitor(&id, cfg);
                            mutated = true;
                            preview_dirty = true;
                            need_redraw = true;
                        }
                        Some(HitTarget::LayoutEditBtn(id)) => {
                            sess.context_menu = None;
                            if let Some(lay) = cfg.find_layout(&id).cloned() {
                                if lay.built_in {
                                    if lay.layout_type != ZoneLayoutType::None {
                                        let dh = lay.default_horizontal;
                                        let dv = lay.default_vertical;
                                        sess.modal = Some(ActiveModal::TemplateEdit {
                                            original_id: lay.id.clone(),
                                            draft: lay,
                                            default_horiz: dh,
                                            default_vert: dv,
                                        });
                                        preview_dirty = true;
                                        need_redraw = true;
                                    }
                                } else {
                                    sess.context_menu = Some(ContextMenuState {
                                        layout_id: id,
                                        anchor_x: lx,
                                        anchor_y: ly,
                                    });
                                    need_redraw = true;
                                }
                            }
                        }
                        Some(HitTarget::BtnNewLayout) => {
                            sess.context_menu = None;
                            let next_name = next_custom_layout_name(&cfg.snap_zones.layouts, sess.zh_ui);
                            sess.modal = Some(ActiveModal::NewLayout {
                                name: next_name,
                                layout_type: ZoneLayoutType::Grid,
                                name_focused: true,
                            });
                            need_redraw = true;
                        }
                        Some(HitTarget::ContextMenuItem { layout_id, action }) => {
                            sess.context_menu = None;
                            match action {
                                ContextAction::EditProps => {
                                    if let Some(lay) = cfg.find_layout(&layout_id).cloned() {
                                        let dh = lay.default_horizontal;
                                        let dv = lay.default_vertical;
                                        sess.modal = Some(ActiveModal::CustomProps {
                                            original_id: lay.id.clone(),
                                            draft: lay,
                                            name_focused: false,
                                            default_horiz: dh,
                                            default_vert: dv,
                                        });
                                        need_redraw = true;
                                    }
                                }
                                ContextAction::EditZones => {
                                    launch_editor = Some(sess.build_editor_request(
                                        Some(layout_id),
                                        None,
                                        None,
                                        false,
                                    ));
                                }
                                ContextAction::Duplicate => {
                                    if let Some(new_id) = duplicate_layout_in_config(&layout_id, cfg, sess.zh_ui) {
                                        sess.bind_layout_to_active_monitor(&new_id, cfg);
                                        mutated = true;
                                        preview_dirty = true;
                                        need_redraw = true;
                                    }
                                }
                                ContextAction::Delete => {
                                    if delete_custom_layout_in_config(&layout_id, cfg) {
                                        mutated = true;
                                        preview_dirty = true;
                                        need_redraw = true;
                                    }
                                }
                            }
                        }
                        Some(HitTarget::ModalBackdrop) | Some(HitTarget::ModalCloseOrCancel) => {
                            sess.modal = None;
                            preview_dirty = true;
                            need_redraw = true;
                        }
                        Some(HitTarget::ModalNameInput) => {
                            if let Some(ActiveModal::CustomProps { name_focused, .. } | ActiveModal::NewLayout { name_focused, .. }) =
                                sess.modal.as_mut()
                            {
                                *name_focused = true;
                                need_redraw = true;
                            }
                        }
                        Some(HitTarget::ModalZoneCountMinus) => {
                            if let Some(ActiveModal::TemplateEdit { draft, .. }) = sess.modal.as_mut() {
                                let cur = draft.zone_count.max(1);
                                if cur > 1 {
                                    draft.zone_count = cur - 1;
                                    draft.rebuild_template_geometry();
                                    preview_dirty = true;
                                    need_redraw = true;
                                }
                            }
                        }
                        Some(HitTarget::ModalZoneCountPlus) => {
                            if let Some(ActiveModal::TemplateEdit { draft, .. }) = sess.modal.as_mut() {
                                let max_c = if draft.layout_type == ZoneLayoutType::Focus { 10 } else { 12 };
                                let cur = draft.zone_count.max(1);
                                if cur < max_c {
                                    draft.zone_count = cur + 1;
                                    draft.rebuild_template_geometry();
                                    preview_dirty = true;
                                    need_redraw = true;
                                }
                            }
                        }
                        Some(HitTarget::ModalShowSpacingToggle) => {
                            match sess.modal.as_mut() {
                                Some(ActiveModal::TemplateEdit { draft, .. })
                                | Some(ActiveModal::CustomProps { draft, .. }) => {
                                    draft.show_spacing = !draft.show_spacing;
                                    if draft.show_spacing && draft.gap.unwrap_or(0) == 0 {
                                        draft.gap = Some(cfg.snap_zones.zone_gap.max(12));
                                    }
                                    preview_dirty = true;
                                    need_redraw = true;
                                }
                                _ => {}
                            }
                        }
                        Some(HitTarget::ModalSpacingMinus) => {
                            match sess.modal.as_mut() {
                                Some(ActiveModal::TemplateEdit { draft, .. })
                                | Some(ActiveModal::CustomProps { draft, .. }) => {
                                    let cur = draft.gap.unwrap_or(cfg.snap_zones.zone_gap);
                                    draft.gap = Some((cur - 2).clamp(0, 48));
                                    draft.show_spacing = true;
                                    preview_dirty = true;
                                    need_redraw = true;
                                }
                                _ => {}
                            }
                        }
                        Some(HitTarget::ModalSpacingPlus) => {
                            match sess.modal.as_mut() {
                                Some(ActiveModal::TemplateEdit { draft, .. })
                                | Some(ActiveModal::CustomProps { draft, .. }) => {
                                    let cur = draft.gap.unwrap_or(cfg.snap_zones.zone_gap);
                                    draft.gap = Some((cur + 2).clamp(0, 48));
                                    draft.show_spacing = true;
                                    preview_dirty = true;
                                    need_redraw = true;
                                }
                                _ => {}
                            }
                        }
                        Some(HitTarget::ModalSpacingSlider) => {
                            unsafe {
                                let _ = SetCapture(sess.hub_hwnd);
                            }
                            sess.drag = Some(HubDragMode::SpacingSlider);
                            if sess.update_slider_from_x(lx, true) {
                                preview_dirty = true;
                                need_redraw = true;
                            }
                        }
                        Some(HitTarget::ModalHighlightMinus) => {
                            match sess.modal.as_mut() {
                                Some(ActiveModal::TemplateEdit { draft, .. })
                                | Some(ActiveModal::CustomProps { draft, .. }) => {
                                    let cur = draft
                                        .highlight_distance
                                        .unwrap_or(cfg.snap_zones.edge_threshold_px);
                                    draft.highlight_distance = Some((cur - 2).clamp(0, 64));
                                    need_redraw = true;
                                }
                                _ => {}
                            }
                        }
                        Some(HitTarget::ModalHighlightPlus) => {
                            match sess.modal.as_mut() {
                                Some(ActiveModal::TemplateEdit { draft, .. })
                                | Some(ActiveModal::CustomProps { draft, .. }) => {
                                    let cur = draft
                                        .highlight_distance
                                        .unwrap_or(cfg.snap_zones.edge_threshold_px);
                                    draft.highlight_distance = Some((cur + 2).clamp(0, 64));
                                    need_redraw = true;
                                }
                                _ => {}
                            }
                        }
                        Some(HitTarget::ModalHighlightSlider) => {
                            unsafe {
                                let _ = SetCapture(sess.hub_hwnd);
                            }
                            sess.drag = Some(HubDragMode::HighlightSlider);
                            if sess.update_slider_from_x(lx, false) {
                                need_redraw = true;
                            }
                        }
                        Some(HitTarget::ModalHotkeyPrev) => {
                            if let Some(ActiveModal::CustomProps { draft, .. }) = sess.modal.as_mut() {
                                draft.hotkey_digit = match draft.hotkey_digit {
                                    None => Some(9),
                                    Some(0) => None,
                                    Some(d) => Some(d - 1),
                                };
                                need_redraw = true;
                            }
                        }
                        Some(HitTarget::ModalHotkeyNext) => {
                            if let Some(ActiveModal::CustomProps { draft, .. }) = sess.modal.as_mut() {
                                draft.hotkey_digit = match draft.hotkey_digit {
                                    None => Some(0),
                                    Some(9) => None,
                                    Some(d) => Some(d + 1),
                                };
                                need_redraw = true;
                            }
                        }
                        Some(HitTarget::ModalDefaultHoriz) => {
                            match sess.modal.as_mut() {
                                Some(ActiveModal::TemplateEdit { default_horiz, .. })
                                | Some(ActiveModal::CustomProps { default_horiz, .. }) => {
                                    *default_horiz = !*default_horiz;
                                    need_redraw = true;
                                }
                                _ => {}
                            }
                        }
                        Some(HitTarget::ModalDefaultVert) => {
                            match sess.modal.as_mut() {
                                Some(ActiveModal::TemplateEdit { default_vert, .. })
                                | Some(ActiveModal::CustomProps { default_vert, .. }) => {
                                    *default_vert = !*default_vert;
                                    need_redraw = true;
                                }
                                _ => {}
                            }
                        }
                        Some(HitTarget::ModalNewTypeGrid) => {
                            if let Some(ActiveModal::NewLayout { layout_type, .. }) = sess.modal.as_mut() {
                                *layout_type = ZoneLayoutType::Grid;
                                need_redraw = true;
                            }
                        }
                        Some(HitTarget::ModalNewTypeCanvas) => {
                            if let Some(ActiveModal::NewLayout { layout_type, .. }) = sess.modal.as_mut() {
                                *layout_type = ZoneLayoutType::Canvas;
                                need_redraw = true;
                            }
                        }
                        Some(HitTarget::ModalDuplicate) => {
                            let dup_src = match &sess.modal {
                                Some(ActiveModal::TemplateEdit { original_id, .. })
                                | Some(ActiveModal::CustomProps { original_id, .. }) => {
                                    Some(original_id.clone())
                                }
                                _ => None,
                            };
                            if let Some(src_id) = dup_src {
                                sess.modal = None;
                                if let Some(new_id) = duplicate_layout_in_config(&src_id, cfg, sess.zh_ui) {
                                    sess.bind_layout_to_active_monitor(&new_id, cfg);
                                    mutated = true;
                                    preview_dirty = true;
                                    need_redraw = true;
                                }
                            }
                        }
                        Some(HitTarget::ModalDelete) => {
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
                                    need_redraw = true;
                                }
                            }
                        }
                        Some(HitTarget::ModalEditZones) => {
                            if let Some(ActiveModal::CustomProps {
                                original_id,
                                draft,
                                default_horiz,
                                default_vert,
                                ..
                            }) = sess.modal.take()
                            {
                                commit_layout_draft(
                                    &original_id,
                                    draft,
                                    default_horiz,
                                    default_vert,
                                    cfg,
                                );
                                mutated = true;
                                launch_editor = Some(sess.build_editor_request(
                                    Some(original_id),
                                    None,
                                    None,
                                    false,
                                ));
                            }
                        }
                        Some(HitTarget::ModalSaveOrCreate) => {
                            match sess.modal.take() {
                                Some(ActiveModal::TemplateEdit {
                                    original_id,
                                    draft,
                                    default_horiz,
                                    default_vert,
                                })
                                | Some(ActiveModal::CustomProps {
                                    original_id,
                                    draft,
                                    default_horiz,
                                    default_vert,
                                    ..
                                }) => {
                                    commit_layout_draft(
                                        &original_id,
                                        draft,
                                        default_horiz,
                                        default_vert,
                                        cfg,
                                    );
                                    sess.bind_layout_to_active_monitor(&original_id, cfg);
                                    mutated = true;
                                    preview_dirty = true;
                                    need_redraw = true;
                                }
                                Some(ActiveModal::NewLayout {
                                    name, layout_type, ..
                                }) => {
                                    let clean_name = if name.trim().is_empty() {
                                        next_custom_layout_name(&cfg.snap_zones.layouts, sess.zh_ui)
                                    } else {
                                        name.trim().to_string()
                                    };
                                    launch_editor = Some(sess.build_editor_request(
                                        None,
                                        Some(layout_type),
                                        Some(clean_name),
                                        true,
                                    ));
                                }
                                None => {}
                            }
                        }
                        None => {
                            if sess.context_menu.take().is_some() {
                                need_redraw = true;
                            }
                        }
                    }
                }
                HUB_EVT_KEYDOWN => {
                    let vk = lparam as u16;
                    if vk == VK_ESCAPE.0 {
                        if sess.context_menu.take().is_some() {
                            need_redraw = true;
                        } else if sess.modal.take().is_some() {
                            preview_dirty = true;
                            need_redraw = true;
                        } else {
                            close_now = true;
                        }
                    } else if vk == VK_RETURN.0 && sess.modal.is_some() {
                        // Enter 直接确认当前弹窗
                        match sess.modal.take() {
                            Some(ActiveModal::TemplateEdit {
                                original_id,
                                draft,
                                default_horiz,
                                default_vert,
                            })
                            | Some(ActiveModal::CustomProps {
                                original_id,
                                draft,
                                default_horiz,
                                default_vert,
                                ..
                            }) => {
                                commit_layout_draft(
                                    &original_id,
                                    draft,
                                    default_horiz,
                                    default_vert,
                                    cfg,
                                );
                                sess.bind_layout_to_active_monitor(&original_id, cfg);
                                mutated = true;
                                preview_dirty = true;
                                need_redraw = true;
                            }
                            Some(ActiveModal::NewLayout {
                                name, layout_type, ..
                            }) => {
                                let clean_name = if name.trim().is_empty() {
                                    next_custom_layout_name(&cfg.snap_zones.layouts, sess.zh_ui)
                                } else {
                                    name.trim().to_string()
                                };
                                launch_editor = Some(sess.build_editor_request(
                                    None,
                                    Some(layout_type),
                                    Some(clean_name),
                                    true,
                                ));
                            }
                            None => {}
                        }
                    } else if vk == VK_BACK.0 {
                        match sess.modal.as_mut() {
                            Some(ActiveModal::CustomProps {
                                draft,
                                name_focused: true,
                                ..
                            }) => {
                                if draft.name.pop().is_some() {
                                    need_redraw = true;
                                }
                            }
                            Some(ActiveModal::NewLayout {
                                name,
                                name_focused: true,
                                ..
                            }) => {
                                if name.pop().is_some() {
                                    need_redraw = true;
                                }
                            }
                            _ => {}
                        }
                    }
                }
                HUB_EVT_CHAR => {
                    if let Some(ch) = char::from_u32(lparam as u32) {
                        if !ch.is_control() {
                            match sess.modal.as_mut() {
                                Some(ActiveModal::CustomProps {
                                    draft,
                                    name_focused: true,
                                    ..
                                }) => {
                                    if draft.name.chars().count() < 32 {
                                        draft.name.push(ch);
                                        need_redraw = true;
                                    }
                                }
                                Some(ActiveModal::NewLayout {
                                    name,
                                    name_focused: true,
                                    ..
                                }) => {
                                    if name.chars().count() < 32 {
                                        name.push(ch);
                                        need_redraw = true;
                                    }
                                }
                                _ => {}
                            }
                        }
                    }
                }
                _ => {}
            }

            if !close_now && launch_editor.is_none() {
                if let Some(g) = gfx {
                    if preview_dirty {
                        sess.render_preview_layer(cfg, g);
                    }
                    if need_redraw {
                        sess.render_hub_window(cfg, g);
                    }
                }
            }
        }

        if mutated {
            notify_host_hub_sync(cfg, config_dir, true, false, trace_id);
        }

        if close_now {
            let restore = self.close(trace_id);
            notify_host_hub_sync(cfg, config_dir, false, restore, trace_id);
            return HubEventOutcome::Closed {
                restore_settings: restore,
            };
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
    let prefix = if zh_ui { "自定义布局 " } else { "Custom Layout " };
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
            wk_warn!(trace_id, "layoutHub", "failed to write hubSync result: {}", e);
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
// 4. HubSession 渲染管线（底层 1:1 实时预览 + 前景 Fluent 矢量管理窗）
// ─────────────────────────────────────────────────────────────────────────────

impl HubSession {
    fn active_monitor(&self) -> &MonitorInfo {
        &self.monitors[self.active_monitor_idx.min(self.monitors.len().saturating_sub(1))]
    }

    fn scale(&self) -> f32 {
        (self.active_monitor().dpi as f32 / 96.0).max(1.0)
    }

    fn set_cursor(&self, kind: u32) {
        let prev = HUB_CURSOR_KIND.swap(kind, Ordering::AcqRel);
        if prev != kind {
            unsafe {
                let _ = SetCursor(hub_cursor_handle(kind));
            }
        }
    }

    fn current_layout_for_preview<'a>(&'a self, cfg: &'a WindowKitConfig) -> ZoneLayout {
        // 如果正在调节模板或自定义属性弹窗，实时在底层 4K 屏幕预览草稿！
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

    fn toggle_maximize(&mut self, cfg: &WindowKitConfig, gfx: Option<&SharedGraphics>) {
        let active_mon = self.active_monitor().clone();
        let work = active_mon.work_rect;
        if self.is_maximized {
            if let Some(prev) = self.restore_rect.take() {
                self.win_rect = ensure_window_rect_on_live_monitors(
                    &active_mon.id,
                    None,
                    prev.width(),
                    prev.height(),
                    Some(&active_mon),
                    &self.monitors,
                );
            }
            self.is_maximized = false;
        } else {
            self.restore_rect = Some(self.win_rect);
            self.win_rect = work;
            self.is_maximized = true;
        }
        unsafe {
            let _ = SetWindowPos(
                self.hub_hwnd,
                HWND_TOPMOST,
                self.win_rect.left,
                self.win_rect.top,
                self.win_rect.width(),
                self.win_rect.height(),
                SWP_NOACTIVATE,
            );
        }
        let dark = cfg.is_dark_theme();
        apply_cross_os_popup_chrome(
            self.hub_hwnd,
            dark,
            self.win_rect.width(),
            self.win_rect.height(),
            if self.is_maximized { 0 } else { (10.0 * self.scale()).round() as i32 },
        );
        self.resize_render_target(gfx);
    }

    fn resize_render_target(&mut self, gfx: Option<&SharedGraphics>) {
        let mut rc = RECT::default();
        unsafe {
            let _ = GetClientRect(self.hub_hwnd, &mut rc);
        }
        let w = (rc.right - rc.left).max(1) as u32;
        let h = (rc.bottom - rc.top).max(1) as u32;
        if let Some(rt) = &self.rt {
            let sz = D2D_SIZE_U { width: w, height: h };
            if unsafe { rt.Resize(&sz) }.is_err() {
                self.rt = gfx.and_then(|g| g.create_hwnd_target(self.hub_hwnd, w, h, 96).ok());
            }
        } else if let Some(g) = gfx {
            self.rt = g.create_hwnd_target(self.hub_hwnd, w, h, 96).ok();
        }
    }

    fn update_slider_from_x(&mut self, lx: i32, is_spacing: bool) -> bool {
        let track = if is_spacing {
            self.spacing_slider_track
        } else {
            self.highlight_slider_track
        };
        let Some(tr) = track else {
            return false;
        };
        let frac = ((lx - tr.left) as f32 / tr.width().max(1) as f32).clamp(0.0, 1.0);
        match self.modal.as_mut() {
            Some(ActiveModal::TemplateEdit { draft, .. })
            | Some(ActiveModal::CustomProps { draft, .. }) => {
                if is_spacing {
                    let next = (frac * 48.0).round() as i32;
                    if draft.gap != Some(next) || !draft.show_spacing {
                        draft.show_spacing = true;
                        draft.gap = Some(next);
                        return true;
                    }
                } else {
                    let next = (frac * 64.0).round() as i32;
                    if draft.highlight_distance != Some(next) {
                        draft.highlight_distance = Some(next);
                        return true;
                    }
                }
            }
            _ => {}
        }
        false
    }

    fn hit_test(&self, lx: i32, ly: i32) -> Option<HitTarget> {
        // 逆序命中（弹窗 / 上下文菜单优先级最高）
        for hb in self.hit_boxes.iter().rev() {
            if hb.rect.contains_point(lx, ly) {
                return Some(hb.target.clone());
            }
        }
        None
    }

    /// 渲染底层 1:1 全屏磨砂实时预览（对标 FancyZones 图 1 / 图 10 / 图 11）
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
        let num_fmt = gfx.ui_text_format(28.0 * s, false, true).ok();

        let _ = render_layered_window_d2d(gfx, ph, work.left, work.top, w, h, |rt| unsafe {
            let fill_col = if dark {
                rgba(0.08, 0.10, 0.15, 0.52)
            } else {
                rgba(0.98, 0.985, 0.99, 0.58)
            };
            let stroke_col = rgb_a(accent, if dark { 0.85 } else { 0.78 });
            let text_col = if dark {
                rgba(0.96, 0.98, 1.0, 0.72)
            } else {
                rgba(0.12, 0.16, 0.22, 0.55)
            };

            let (Ok(b_fill), Ok(b_stroke), Ok(b_text)) = (
                rt.CreateSolidColorBrush(&fill_col, None),
                rt.CreateSolidColorBrush(&stroke_col, None),
                rt.CreateSolidColorBrush(&text_col, None),
            ) else {
                return;
            };

            if layout.layout_type == ZoneLayoutType::None || zones.is_empty() {
                // 无布局：仅画极淡全屏工作区提示边框
                let r = rect_f(4.0, 4.0, w as f32 - 4.0, h as f32 - 4.0);
                rt.DrawRectangle(&r, &b_stroke, 1.5 * s, None);
                return;
            }

            for z in &zones {
                let lr = rect_f(
                    (z.rect.left - work.left) as f32 + 0.5,
                    (z.rect.top - work.top) as f32 + 0.5,
                    (z.rect.right - work.left) as f32 - 0.5,
                    (z.rect.bottom - work.top) as f32 - 0.5,
                );
                rt.FillRectangle(&lr, &b_fill);
                rt.DrawRectangle(&lr, &b_stroke, 1.35 * s, None);
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
            // 始终位于目标屏工作区，并置于前景 Hub 窗的正下方
            let _ = SetWindowPos(ph, HWND_TOPMOST, work.left, work.top, w, h, SWP_NOACTIVATE);
            let _ = ShowWindow(ph, SW_SHOWNOACTIVATE);
            let _ = SetWindowPos(
                self.hub_hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOACTIVATE
                    | windows::Win32::UI::WindowsAndMessaging::SWP_NOMOVE
                    | windows::Win32::UI::WindowsAndMessaging::SWP_NOSIZE,
            );
        }
    }

    /// 渲染前景「布局管理器」居中窗口（纯 Direct2D 硬件矢量 + 3 大内嵌弹窗）
    fn render_hub_window(&mut self, cfg: &WindowKitConfig, gfx: &SharedGraphics) {
        if self.rt.is_none() {
            self.resize_render_target(Some(gfx));
        }
        let Some(hwnd_rt) = self.rt.clone() else {
            return;
        };
        let Ok(rt): Result<ID2D1RenderTarget, _> = hwnd_rt.cast() else {
            return;
        };

        let mut rc = RECT::default();
        unsafe {
            let _ = GetClientRect(self.hub_hwnd, &mut rc);
        }
        let w = (rc.right - rc.left).max(1) as f32;
        let h = (rc.bottom - rc.top).max(1) as f32;
        let s = self.scale();
        let dark = cfg.is_dark_theme();
        let accent = parse_hex_color(&cfg.accent_color, [0.23, 0.51, 0.96]);
        let zh = self.zh_ui;

        let active_mon = self.active_monitor().clone();
        let active_layout_id = {
            let did = if cfg.snap_zones.per_virtual_desktop {
                self.desktop_id.as_deref()
            } else {
                None
            };
            cfg.resolve_layout_for_monitor(&active_mon, did).id.clone()
        };

        self.hit_boxes.clear();
        self.spacing_slider_track = None;
        self.highlight_slider_track = None;

        // 字体格式准备（全站字号 >= 12px，字重 >= 500）
        let title_fmt = gfx.ui_text_format(13.0 * s, false, false).ok();
        let section_fmt = gfx.ui_text_format(16.0 * s, true, false).ok();
        let card_title_fmt = gfx.ui_text_format(13.0 * s, true, false).ok();
        let mon_num_fmt = gfx.ui_text_format(20.0 * s, true, true).ok();
        let mon_meta_fmt = gfx.mono_text_format(11.8 * s, false, true).ok();
        let btn_fmt = gfx.ui_text_format(13.0 * s, true, true).ok();
        let body_fmt = gfx.ui_text_format(13.0 * s, false, false).ok();
        let body_center_fmt = gfx.ui_text_format(13.0 * s, false, true).ok();
        let badge_fmt = gfx.mono_text_format(11.8 * s, true, true).ok();
        let modal_title_fmt = gfx.ui_text_format(18.0 * s, true, false).ok();

        let titlebar_h = (36.0 * s).round();
        let mon_bar_h = (92.0 * s).round();
        let footer_h = (56.0 * s).round();

        unsafe {
            rt.BeginDraw();

            // 调色板（对齐 Fluent Design 深/浅双主题）
            let bg_main = if dark { rgba(0.11, 0.125, 0.155, 1.0) } else { rgba(0.975, 0.98, 0.99, 1.0) };
            let bg_surface = if dark { rgba(0.145, 0.165, 0.205, 1.0) } else { rgba(1.0, 1.0, 1.0, 1.0) };
            let bg_header = if dark { rgba(0.095, 0.11, 0.14, 1.0) } else { rgba(0.95, 0.96, 0.975, 1.0) };
            let bg_thumb = if dark { rgba(0.20, 0.23, 0.28, 1.0) } else { rgba(0.94, 0.95, 0.965, 1.0) };
            let thumb_zone = if dark { rgba(0.35, 0.40, 0.48, 1.0) } else { rgba(0.84, 0.86, 0.89, 1.0) };
            let border_col = if dark { rgba(1.0, 1.0, 1.0, 0.12) } else { rgba(0.10, 0.14, 0.22, 0.12) };
            let hover_bg = if dark { rgba(1.0, 1.0, 1.0, 0.08) } else { rgba(0.08, 0.12, 0.20, 0.05) };
            let text_main = if dark { rgba(0.95, 0.97, 1.0, 1.0) } else { rgba(0.10, 0.13, 0.18, 1.0) };
            let text_muted = if dark { rgba(0.68, 0.74, 0.83, 1.0) } else { rgba(0.38, 0.44, 0.52, 1.0) };
            let white = rgba(1.0, 1.0, 1.0, 1.0);
            let danger = rgba(0.86, 0.22, 0.24, 1.0);
            let accent_col = rgb_a(accent, 1.0);
            let accent_soft = rgb_a(accent, if dark { 0.22 } else { 0.11 });

            rt.Clear(Some(&bg_main));

            let (
                Ok(b_surface),
                Ok(b_header),
                Ok(b_thumb),
                Ok(b_thumb_zone),
                Ok(b_border),
                Ok(b_hover),
                Ok(b_text),
                Ok(b_muted),
                Ok(b_white),
                Ok(b_danger),
                Ok(b_accent),
                Ok(b_accent_soft),
            ) = (
                rt.CreateSolidColorBrush(&bg_surface, None),
                rt.CreateSolidColorBrush(&bg_header, None),
                rt.CreateSolidColorBrush(&bg_thumb, None),
                rt.CreateSolidColorBrush(&thumb_zone, None),
                rt.CreateSolidColorBrush(&border_col, None),
                rt.CreateSolidColorBrush(&hover_bg, None),
                rt.CreateSolidColorBrush(&text_main, None),
                rt.CreateSolidColorBrush(&text_muted, None),
                rt.CreateSolidColorBrush(&white, None),
                rt.CreateSolidColorBrush(&danger, None),
                rt.CreateSolidColorBrush(&accent_col, None),
                rt.CreateSolidColorBrush(&accent_soft, None),
            ) else {
                let _ = rt.EndDraw(None, None);
                return;
            };

            // ── A. 顶部标题栏（可拖拽 + 最大化 + 关闭） ──
            let tb_r = rect_f(0.0, 0.0, w, titlebar_h);
            rt.FillRectangle(&tb_r, &b_header);

            let ctl_w = (44.0 * s).round();
            let close_r = RectI::from_xywh((w - ctl_w) as i32, 0, ctl_w as i32, titlebar_h as i32);
            let max_r = RectI::from_xywh((w - ctl_w * 2.0) as i32, 0, ctl_w as i32, titlebar_h as i32);
            let drag_r = RectI::from_xywh(0, 0, (w - ctl_w * 2.0).max(1.0) as i32, titlebar_h as i32);
            self.hit_boxes.push(HitBox { rect: drag_r, target: HitTarget::TitleBarDrag });
            self.hit_boxes.push(HitBox { rect: max_r, target: HitTarget::BtnMaximize });
            self.hit_boxes.push(HitBox { rect: close_r, target: HitTarget::BtnClose });

            // 标题栏左侧矢量宫格图标 + 标题
            let ico_x = 14.0 * s;
            let ico_y = (titlebar_h - 14.0 * s) * 0.5;
            rt.FillRoundedRectangle(
                &rounded_rect(rect_f(ico_x, ico_y, ico_x + 6.0 * s, ico_y + 14.0 * s), 1.5 * s),
                &b_accent,
            );
            rt.FillRoundedRectangle(
                &rounded_rect(rect_f(ico_x + 8.0 * s, ico_y, ico_x + 14.0 * s, ico_y + 6.0 * s), 1.5 * s),
                &b_accent,
            );
            rt.FillRoundedRectangle(
                &rounded_rect(rect_f(ico_x + 8.0 * s, ico_y + 8.0 * s, ico_x + 14.0 * s, ico_y + 14.0 * s), 1.5 * s),
                &b_accent,
            );
            if let Some(fmt) = &title_fmt {
                let app_title = if zh { "SnapZones 布局管理器" } else { "SnapZones Layout Manager" };
                let wide = to_wide(app_title);
                let tr = rect_f(36.0 * s, 8.0 * s, w - ctl_w * 2.2, titlebar_h);
                rt.DrawText(
                    &wide[..wide.len().saturating_sub(1)],
                    fmt,
                    &tr,
                    &b_text,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }

            // 最大化 / 还原按钮
            let max_rf = rect_f(max_r.left as f32, max_r.top as f32, max_r.right as f32, max_r.bottom as f32);
            if self.hovered == Some(HitTarget::BtnMaximize) {
                rt.FillRectangle(&max_rf, &b_hover);
            }
            let mcx = (max_rf.left + max_rf.right) * 0.5;
            let mcy = (max_rf.top + max_rf.bottom) * 0.5;
            let ms = 4.5 * s;
            rt.DrawRectangle(&rect_f(mcx - ms, mcy - ms, mcx + ms, mcy + ms), &b_text, 1.3 * s, None);

            // 关闭按钮
            let cls_rf = rect_f(close_r.left as f32, close_r.top as f32, close_r.right as f32, close_r.bottom as f32);
            let close_hov = self.hovered == Some(HitTarget::BtnClose);
            if close_hov {
                rt.FillRectangle(&cls_rf, &b_danger);
            }
            let ccx = (cls_rf.left + cls_rf.right) * 0.5;
            let ccy = (cls_rf.top + cls_rf.bottom) * 0.5;
            let cs = 4.8 * s;
            let x_brush = if close_hov { &b_white } else { &b_text };
            rt.DrawLine(
                D2D_POINT_2F { x: ccx - cs, y: ccy - cs },
                D2D_POINT_2F { x: ccx + cs, y: ccy + cs },
                x_brush,
                1.4 * s,
                None,
            );
            rt.DrawLine(
                D2D_POINT_2F { x: ccx + cs, y: ccy - cs },
                D2D_POINT_2F { x: ccx - cs, y: ccy + cs },
                x_brush,
                1.4 * s,
                None,
            );

            // ── B. 顶部多显示器卡片栏 + 当前虚拟桌面徽章 ──
            let mb_top = titlebar_h;
            let mb_r = rect_f(0.0, mb_top, w, mb_top + mon_bar_h);
            rt.FillRectangle(&mb_r, &b_header);
            rt.DrawLine(
                D2D_POINT_2F { x: 0.0, y: mb_r.bottom },
                D2D_POINT_2F { x: w, y: mb_r.bottom },
                &b_border,
                1.0,
                None,
            );

            let mcard_w = (128.0 * s).round();
            let mcard_h = (70.0 * s).round();
            let mcard_gap = (12.0 * s).round();
            let total_mw =
                self.monitors.len() as f32 * mcard_w + self.monitors.len().saturating_sub(1) as f32 * mcard_gap;
            let mut mx = ((w - total_mw) * 0.5).max(20.0 * s);
            let my = mb_top + (mon_bar_h - mcard_h) * 0.5;

            for (idx, mon) in self.monitors.iter().enumerate() {
                let r = rect_f(mx, my, mx + mcard_w, my + mcard_h);
                let rr = rounded_rect(r, 6.0 * s);
                let selected = idx == self.active_monitor_idx;
                let hovered = self.hovered == Some(HitTarget::MonitorCard(idx));
                rt.FillRoundedRectangle(&rr, if selected { &b_surface } else if hovered { &b_hover } else { &b_surface });
                rt.DrawRoundedRectangle(
                    &rr,
                    if selected { &b_accent } else { &b_border },
                    if selected { 2.0 * s } else { 1.0 * s },
                    None,
                );
                if let Some(fmt) = &mon_num_fmt {
                    let num_s = to_wide(&(idx + 1).to_string());
                    let nr = rect_f(r.left, r.top + 6.0 * s, r.right, r.top + 32.0 * s);
                    rt.DrawText(
                        &num_s[..num_s.len().saturating_sub(1)],
                        fmt,
                        &nr,
                        if selected { &b_accent } else { &b_text },
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        DWRITE_MEASURING_MODE_NATURAL,
                    );
                }
                if let Some(fmt) = &mon_meta_fmt {
                    let res_s = format!(
                        "{} x {}\n{}%",
                        mon.physical_rect.width(),
                        mon.physical_rect.height(),
                        mon.scale_percent
                    );
                    let wide = to_wide(&res_s);
                    let mr = rect_f(r.left + 4.0 * s, r.top + 34.0 * s, r.right - 4.0 * s, r.bottom - 4.0 * s);
                    rt.DrawText(
                        &wide[..wide.len().saturating_sub(1)],
                        fmt,
                        &mr,
                        if selected { &b_accent } else { &b_muted },
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        DWRITE_MEASURING_MODE_NATURAL,
                    );
                }
                self.hit_boxes.push(HitBox {
                    rect: RectI::new(r.left as i32, r.top as i32, r.right as i32, r.bottom as i32),
                    target: HitTarget::MonitorCard(idx),
                });
                mx += mcard_w + mcard_gap;
            }

            // 虚拟桌面独立绑定徽章（右上角，仅在开启按虚拟桌面独立布局且有桌面名时显示）
            if cfg.snap_zones.per_virtual_desktop && !self.desktop_label.is_empty() {
                if let Some(fmt) = &badge_fmt {
                    let txt = if zh {
                        format!("桌面: {}", self.desktop_label)
                    } else {
                        format!("Desktop: {}", self.desktop_label)
                    };
                    let bw = ((txt.chars().count() as f32 * 8.0 + 22.0) * s).clamp(96.0 * s, 210.0 * s);
                    let bh = 26.0 * s;
                    let br = rect_f(w - bw - 18.0 * s, mb_top + (mon_bar_h - bh) * 0.5, w - 18.0 * s, mb_top + (mon_bar_h + bh) * 0.5);
                    rt.FillRoundedRectangle(&rounded_rect(br, bh * 0.5), &b_accent_soft);
                    rt.DrawRoundedRectangle(&rounded_rect(br, bh * 0.5), &b_accent, 1.0 * s, None);
                    let wide = to_wide(&txt);
                    rt.DrawText(
                        &wide[..wide.len().saturating_sub(1)],
                        fmt,
                        &br,
                        &b_accent,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        DWRITE_MEASURING_MODE_NATURAL,
                    );
                }
            }

            // ── C. 主内容滚动区（模板 6 卡 + 自定义卡片列表） ──
            let content_top = mb_r.bottom;
            let content_bottom = h - footer_h;
            let clip_rect = rect_f(0.0, content_top, w, content_bottom);
            rt.PushAxisAlignedClip(&clip_rect, windows::Win32::Graphics::Direct2D::D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            let pad_x = (22.0 * s).round();
            let mut cur_y = content_top + (18.0 * s) - self.scroll_y as f32;

            // 分区 1：模板
            if let Some(fmt) = &section_fmt {
                let sec_t = to_wide(if zh { "模板" } else { "Templates" });
                rt.DrawText(
                    &sec_t[..sec_t.len().saturating_sub(1)],
                    fmt,
                    &rect_f(pad_x, cur_y, w - pad_x, cur_y + 26.0 * s),
                    &b_text,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }
            cur_y += 32.0 * s;

            let templates: Vec<&ZoneLayout> = cfg
                .snap_zones
                .layouts
                .iter()
                .filter(|l| l.built_in)
                .collect();
            let customs: Vec<&ZoneLayout> = cfg
                .snap_zones
                .layouts
                .iter()
                .filter(|l| !l.built_in)
                .collect();

            let avail_w = (w - pad_x * 2.0).max(200.0);
            let card_gap = (12.0 * s).round();
            let cols = if avail_w >= 840.0 * s { 6 } else if avail_w >= 560.0 * s { 3 } else { 2 };
            let card_w = ((avail_w - card_gap * (cols - 1) as f32) / cols as f32).floor();
            let card_h = (132.0 * s).round();

            cur_y = self.draw_layout_grid(
                &rt,
                &templates,
                cur_y,
                pad_x,
                card_w,
                card_h,
                card_gap,
                cols,
                content_top,
                content_bottom,
                &active_layout_id,
                cfg,
                s,
                &b_surface,
                &b_thumb,
                &b_thumb_zone,
                &b_border,
                &b_hover,
                &b_text,
                &b_muted,
                &b_accent,
                &b_accent_soft,
                card_title_fmt.as_ref(),
                badge_fmt.as_ref(),
            );

            cur_y += 22.0 * s;

            // 分区 2：自定义
            if let Some(fmt) = &section_fmt {
                let sec_c = to_wide(if zh { "自定义" } else { "Custom" });
                rt.DrawText(
                    &sec_c[..sec_c.len().saturating_sub(1)],
                    fmt,
                    &rect_f(pad_x, cur_y, w - pad_x, cur_y + 26.0 * s),
                    &b_text,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }
            cur_y += 32.0 * s;

            if customs.is_empty() {
                if let Some(fmt) = &body_fmt {
                    let empty_t = to_wide(if zh {
                        "暂无自定义布局，点击右下角「+ 新建布局」创建专属网格或自由画布"
                    } else {
                        "No custom layouts yet. Click '+ Create new layout' below to create one."
                    });
                    rt.DrawText(
                        &empty_t[..empty_t.len().saturating_sub(1)],
                        fmt,
                        &rect_f(pad_x, cur_y + 6.0 * s, w - pad_x, cur_y + 34.0 * s),
                        &b_muted,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        DWRITE_MEASURING_MODE_NATURAL,
                    );
                }
                cur_y += 48.0 * s;
            } else {
                cur_y = self.draw_layout_grid(
                    &rt,
                    &customs,
                    cur_y,
                    pad_x,
                    card_w,
                    card_h,
                    card_gap,
                    cols,
                    content_top,
                    content_bottom,
                    &active_layout_id,
                    cfg,
                    s,
                    &b_surface,
                    &b_thumb,
                    &b_thumb_zone,
                    &b_border,
                    &b_hover,
                    &b_text,
                    &b_muted,
                    &b_accent,
                    &b_accent_soft,
                    card_title_fmt.as_ref(),
                    badge_fmt.as_ref(),
                );
            }

            let content_total_h = (cur_y + self.scroll_y as f32) - content_top + 18.0 * s;
            self.max_scroll_y = ((content_total_h - (content_bottom - content_top)).max(0.0)).ceil() as i32;
            if self.scroll_y > self.max_scroll_y {
                self.scroll_y = self.max_scroll_y;
            }

            rt.PopAxisAlignedClip();

            // ── D. 底部操作条（右侧主题色「+ 新建布局」主按钮） ──
            let foot_r = rect_f(0.0, content_bottom, w, h);
            rt.FillRectangle(&foot_r, &b_header);
            rt.DrawLine(
                D2D_POINT_2F { x: 0.0, y: content_bottom },
                D2D_POINT_2F { x: w, y: content_bottom },
                &b_border,
                1.0,
                None,
            );

            if let Some(fmt) = &body_fmt {
                let ori_tag = match MonitorOrientation::from_rect(active_mon.physical_rect) {
                    MonitorOrientation::Landscape => if zh { "横屏" } else { "Landscape" },
                    MonitorOrientation::Portrait => if zh { "竖屏" } else { "Portrait" },
                };
                let tip = if zh {
                    format!(
                        "当前屏幕: {} ({}) · 点击任意布局即刻应用并在桌面实时预览",
                        active_mon.friendly_name, ori_tag
                    )
                } else {
                    format!(
                        "Monitor: {} ({}) · Click any layout to apply & preview live",
                        active_mon.friendly_name, ori_tag
                    )
                };
                let wide = to_wide(&tip);
                rt.DrawText(
                    &wide[..wide.len().saturating_sub(1)],
                    fmt,
                    &rect_f(pad_x, content_bottom + 18.0 * s, w - 180.0 * s, h - 10.0 * s),
                    &b_muted,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }

            let new_btn_w = (if zh { 120.0 } else { 156.0 } * s).round();
            let new_btn_h = (34.0 * s).round();
            let new_btn_r = rect_f(
                w - pad_x - new_btn_w,
                content_bottom + (footer_h - new_btn_h) * 0.5,
                w - pad_x,
                content_bottom + (footer_h + new_btn_h) * 0.5,
            );
            rt.FillRoundedRectangle(&rounded_rect(new_btn_r, 6.0 * s), &b_accent);
            if self.hovered == Some(HitTarget::BtnNewLayout) {
                rt.DrawRoundedRectangle(&rounded_rect(new_btn_r, 6.0 * s), &b_white, 1.4 * s, None);
            }
            if let Some(fmt) = &btn_fmt {
                let lbl = to_wide(if zh { "+ 新建布局" } else { "+ Create new" });
                rt.DrawText(
                    &lbl[..lbl.len().saturating_sub(1)],
                    fmt,
                    &new_btn_r,
                    &b_white,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }
            self.hit_boxes.push(HitBox {
                rect: RectI::new(
                    new_btn_r.left as i32,
                    new_btn_r.top as i32,
                    new_btn_r.right as i32,
                    new_btn_r.bottom as i32,
                ),
                target: HitTarget::BtnNewLayout,
            });

            // ── E. 自定义卡片 4 项悬浮菜单（图 5：编辑 / 编辑区域 / 复制 / 删除） ──
            if let Some(menu) = self.context_menu.clone() {
                self.draw_context_menu(
                    &rt,
                    &menu,
                    w,
                    h,
                    s,
                    zh,
                    &b_surface,
                    &b_border,
                    &b_hover,
                    &b_text,
                    &b_danger,
                    body_fmt.as_ref(),
                );
            }

            // ── F. 3 大内嵌原生模态弹窗（图 3 / 图 4 / 图 9 / 图 10 / 图 11） ──
            if let Some(modal) = self.modal.clone() {
                self.draw_modal(
                    &rt,
                    &modal,
                    cfg,
                    w,
                    h,
                    s,
                    dark,
                    zh,
                    &b_surface,
                    &b_thumb,
                    &b_thumb_zone,
                    &b_border,
                    &b_hover,
                    &b_text,
                    &b_muted,
                    &b_white,
                    &b_danger,
                    &b_accent,
                    &b_accent_soft,
                    modal_title_fmt.as_ref(),
                    body_fmt.as_ref(),
                    body_center_fmt.as_ref(),
                    btn_fmt.as_ref(),
                );
            }

            // 外框极细边界线
            rt.DrawRectangle(&rect_f(0.5, 0.5, w - 0.5, h - 0.5), &b_border, 1.0, None);

            if rt.EndDraw(None, None).is_err() {
                self.rt = None;
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn draw_layout_grid(
        &mut self,
        rt: &ID2D1RenderTarget,
        items: &[&ZoneLayout],
        start_y: f32,
        pad_x: f32,
        card_w: f32,
        card_h: f32,
        card_gap: f32,
        cols: usize,
        clip_top: f32,
        clip_bottom: f32,
        active_layout_id: &str,
        cfg: &WindowKitConfig,
        s: f32,
        b_surface: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_thumb: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_thumb_zone: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_border: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_hover: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_text: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_muted: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_accent: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_accent_soft: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        card_title_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
        badge_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
    ) -> f32 {
        let mut max_bottom = start_y;
        for (i, lay) in items.iter().enumerate() {
            let row = i / cols;
            let col = i % cols;
            let x = pad_x + col as f32 * (card_w + card_gap);
            let y = start_y + row as f32 * (card_h + card_gap);
            let r = rect_f(x, y, x + card_w, y + card_h);
            max_bottom = max_bottom.max(r.bottom);

            if r.bottom < clip_top || r.top > clip_bottom {
                continue;
            }

            let selected = lay.id == active_layout_id;
            let hovered = self.hovered == Some(HitTarget::LayoutCard(lay.id.clone()));
            let rr = rounded_rect(r, 7.0 * s);
            rt.FillRoundedRectangle(&rr, b_surface);
            if hovered && !selected {
                rt.FillRoundedRectangle(&rr, b_hover);
            }
            rt.DrawRoundedRectangle(
                &rr,
                if selected { b_accent } else { b_border },
                if selected { 2.0 * s } else { 1.0 * s },
                None,
            );

            // 卡片顶部：标题 + 右上角铅笔编辑图标（无布局不显示铅笔）
            let title_r = rect_f(r.left + 10.0 * s, r.top + 8.0 * s, r.right - 34.0 * s, r.top + 28.0 * s);
            if let Some(fmt) = card_title_fmt {
                let display_name = localized_layout_name(lay, self.zh_ui);
                let wide = to_wide(&display_name);
                rt.DrawText(
                    &wide[..wide.len().saturating_sub(1)],
                    fmt,
                    &title_r,
                    if selected { b_accent } else { b_text },
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }

            // 注册整卡点击区域（先压入，再压入右上角铅笔按钮使铅笔优先命中）
            self.hit_boxes.push(HitBox {
                rect: RectI::new(
                    r.left as i32,
                    r.top.max(clip_top) as i32,
                    r.right as i32,
                    r.bottom.min(clip_bottom) as i32,
                ),
                target: HitTarget::LayoutCard(lay.id.clone()),
            });

            if lay.layout_type != ZoneLayoutType::None {
                let eb_sz = 24.0 * s;
                let eb_r = rect_f(r.right - eb_sz - 6.0 * s, r.top + 6.0 * s, r.right - 6.0 * s, r.top + 6.0 * s + eb_sz);
                let eb_hov = self.hovered == Some(HitTarget::LayoutEditBtn(lay.id.clone()));
                if eb_hov {
                    rt.FillRoundedRectangle(&rounded_rect(eb_r, 5.0 * s), b_accent_soft);
                }
                draw_pencil_icon(
                    rt,
                    (eb_r.left + eb_r.right) * 0.5,
                    (eb_r.top + eb_r.bottom) * 0.5,
                    5.5 * s,
                    if eb_hov || selected { b_accent } else { b_muted },
                    1.4 * s,
                );
                self.hit_boxes.push(HitBox {
                    rect: RectI::new(eb_r.left as i32, eb_r.top as i32, eb_r.right as i32, eb_r.bottom as i32),
                    target: HitTarget::LayoutEditBtn(lay.id.clone()),
                });
            }

            // 下半部真实几何比例微缩图
            let thumb_r = rect_f(
                r.left + 10.0 * s,
                r.top + 34.0 * s,
                r.right - 10.0 * s,
                r.bottom - 10.0 * s,
            );
            draw_layout_mini_preview(
                rt,
                lay,
                thumb_r,
                cfg.snap_zones.zone_gap,
                s,
                b_thumb,
                b_thumb_zone,
                b_border,
            );

            // 若绑定了布局快捷键数字 0..=9，在右下角微绘极简等宽徽章
            if let Some(digit) = lay.hotkey_digit {
                if let Some(fmt) = badge_fmt {
                    let br = rect_f(thumb_r.right - 22.0 * s, thumb_r.bottom - 18.0 * s, thumb_r.right - 2.0 * s, thumb_r.bottom - 2.0 * s);
                    rt.FillRoundedRectangle(&rounded_rect(br, 4.0 * s), b_accent_soft);
                    let d_str = to_wide(&digit.to_string());
                    rt.DrawText(
                        &d_str[..d_str.len().saturating_sub(1)],
                        fmt,
                        &br,
                        b_accent,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        DWRITE_MEASURING_MODE_NATURAL,
                    );
                }
            }
        }
        max_bottom
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn draw_context_menu(
        &mut self,
        rt: &ID2D1RenderTarget,
        menu: &ContextMenuState,
        win_w: f32,
        win_h: f32,
        s: f32,
        zh: bool,
        b_surface: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_border: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_hover: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_text: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_danger: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        body_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
    ) {
        let mw = (140.0 * s).round();
        let item_h = (32.0 * s).round();
        let pad = (6.0 * s).round();
        let mh = item_h * 4.0 + pad * 2.0 + 4.0 * s;
        let mx = (menu.anchor_x as f32).min(win_w - mw - 12.0 * s).max(12.0 * s);
        let my = (menu.anchor_y as f32).min(win_h - mh - 12.0 * s).max(12.0 * s);
        let mr = rect_f(mx, my, mx + mw, my + mh);

        if let Ok(b_shadow) = rt.CreateSolidColorBrush(&rgba(0.0, 0.0, 0.0, 0.22), None) {
            rt.FillRoundedRectangle(
                &rounded_rect(rect_f(mr.left + 2.0, mr.top + 4.0 * s, mr.right + 2.0, mr.bottom + 6.0 * s), 8.0 * s),
                &b_shadow,
            );
        }
        rt.FillRoundedRectangle(&rounded_rect(mr, 8.0 * s), b_surface);
        rt.DrawRoundedRectangle(&rounded_rect(mr, 8.0 * s), b_border, 1.0 * s, None);

        let rows = [
            (ContextAction::EditProps, if zh { "编辑" } else { "Edit" }, false),
            (ContextAction::EditZones, if zh { "编辑区域" } else { "Edit zones" }, false),
            (ContextAction::Duplicate, if zh { "复制" } else { "Duplicate" }, false),
            (ContextAction::Delete, if zh { "删除" } else { "Delete" }, true),
        ];

        let mut iy = my + pad;
        for (idx, (act, label, is_danger)) in rows.iter().enumerate() {
            if idx == 3 {
                rt.DrawLine(
                    D2D_POINT_2F { x: mx + 10.0 * s, y: iy + 1.0 * s },
                    D2D_POINT_2F { x: mx + mw - 10.0 * s, y: iy + 1.0 * s },
                    b_border,
                    1.0,
                    None,
                );
                iy += 4.0 * s;
            }
            let ir = rect_f(mx + 4.0 * s, iy, mx + mw - 4.0 * s, iy + item_h);
            let target = HitTarget::ContextMenuItem {
                layout_id: menu.layout_id.clone(),
                action: *act,
            };
            if self.hovered.as_ref() == Some(&target) {
                rt.FillRoundedRectangle(&rounded_rect(ir, 5.0 * s), b_hover);
            }
            if let Some(fmt) = body_fmt {
                let wide = to_wide(label);
                rt.DrawText(
                    &wide[..wide.len().saturating_sub(1)],
                    fmt,
                    &rect_f(ir.left + 12.0 * s, ir.top + 7.0 * s, ir.right - 8.0 * s, ir.bottom),
                    if *is_danger { b_danger } else { b_text },
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }
            self.hit_boxes.push(HitBox {
                rect: RectI::new(ir.left as i32, ir.top as i32, ir.right as i32, ir.bottom as i32),
                target,
            });
            iy += item_h;
        }
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn draw_modal(
        &mut self,
        rt: &ID2D1RenderTarget,
        modal: &ActiveModal,
        cfg: &WindowKitConfig,
        win_w: f32,
        win_h: f32,
        s: f32,
        dark: bool,
        zh: bool,
        b_surface: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_thumb: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_thumb_zone: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_border: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_hover: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_text: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_muted: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_white: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_danger: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_accent: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_accent_soft: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        modal_title_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
        body_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
        body_center_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
        btn_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
    ) {
        // 遮罩底层并拦截点击
        let scrim = if dark { rgba(0.0, 0.0, 0.0, 0.56) } else { rgba(0.08, 0.12, 0.18, 0.30) };
        if let Ok(b_scrim) = rt.CreateSolidColorBrush(&scrim, None) {
            rt.FillRectangle(&rect_f(0.0, 0.0, win_w, win_h), &b_scrim);
        }
        self.hit_boxes.push(HitBox {
            rect: RectI::new(0, 0, win_w as i32, win_h as i32),
            target: HitTarget::ModalBackdrop,
        });

        let (mw, mh) = match modal {
            ActiveModal::NewLayout { .. } => ((420.0 * s).round(), (350.0 * s).round()),
            ActiveModal::TemplateEdit { .. } => ((430.0 * s).round(), (455.0 * s).round()),
            ActiveModal::CustomProps { .. } => ((450.0 * s).round(), (540.0 * s).round()),
        };
        let mw = mw.min(win_w - 36.0 * s);
        let mh = mh.min(win_h - 36.0 * s);
        let mx = ((win_w - mw) * 0.5).round();
        let my = ((win_h - mh) * 0.5).round();
        let mr = rect_f(mx, my, mx + mw, my + mh);

        if let Ok(b_shadow) = rt.CreateSolidColorBrush(&rgba(0.0, 0.0, 0.0, 0.30), None) {
            rt.FillRoundedRectangle(
                &rounded_rect(rect_f(mr.left, mr.top + 6.0 * s, mr.right, mr.bottom + 10.0 * s), 12.0 * s),
                &b_shadow,
            );
        }
        rt.FillRoundedRectangle(&rounded_rect(mr, 10.0 * s), b_surface);
        rt.DrawRoundedRectangle(&rounded_rect(mr, 10.0 * s), b_border, 1.2 * s, None);
        // 吞掉弹窗内部空白点击避免误关弹窗
        self.hit_boxes.push(HitBox {
            rect: RectI::new(mr.left as i32, mr.top as i32, mr.right as i32, mr.bottom as i32),
            target: HitTarget::TitleBarDrag, // 占位吞掉 backdrop
        });

        let pad = (22.0 * s).round();
        let ix = mx + pad;
        let iw = mw - pad * 2.0;
        let mut cy = my + pad;

        match modal {
            ActiveModal::NewLayout {
                name,
                layout_type,
                name_focused,
            } => {
                if let Some(fmt) = modal_title_fmt {
                    let t = to_wide(if zh { "选择布局类型" } else { "Choose layout type" });
                    rt.DrawText(&t[..t.len().saturating_sub(1)], fmt, &rect_f(ix, cy, ix + iw, cy + 28.0 * s), b_text, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
                }
                cy += 38.0 * s;

                // 名称输入框
                self.draw_text_input_row(
                    rt,
                    if zh { "名称" } else { "Name" },
                    name,
                    *name_focused,
                    ix,
                    cy,
                    iw,
                    s,
                    b_thumb,
                    b_border,
                    b_accent,
                    b_text,
                    b_muted,
                    body_fmt,
                );
                cy += 64.0 * s;

                // 网格 vs 画布单选卡片
                let opt_h = (68.0 * s).round();
                let options = [
                    (
                        ZoneLayoutType::Grid,
                        HitTarget::ModalNewTypeGrid,
                        if zh { "网格" } else { "Grid" },
                        if zh { "创建水平或垂直堆叠的分区布局" } else { "Create horizontal or vertical stacked zones" },
                    ),
                    (
                        ZoneLayoutType::Canvas,
                        HitTarget::ModalNewTypeCanvas,
                        if zh { "画布" } else { "Canvas" },
                        if zh { "创建可自由拖动、重叠与磁吸窗口的区域" } else { "Create freeform overlapping zones" },
                    ),
                ];
                for (lt, target, heading, desc) in options {
                    let or = rect_f(ix, cy, ix + iw, cy + opt_h);
                    let active = *layout_type == lt;
                    rt.FillRoundedRectangle(&rounded_rect(or, 8.0 * s), if active { b_accent_soft } else { b_thumb });
                    rt.DrawRoundedRectangle(&rounded_rect(or, 8.0 * s), if active { b_accent } else { b_border }, if active { 2.0 * s } else { 1.0 * s }, None);
                    if let Some(fmt) = btn_fmt {
                        let hw = to_wide(heading);
                        rt.DrawText(&hw[..hw.len().saturating_sub(1)], fmt, &rect_f(or.left + 16.0 * s, or.top + 10.0 * s, or.right - 12.0 * s, or.top + 34.0 * s), if active { b_accent } else { b_text }, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
                    }
                    if let Some(fmt) = body_fmt {
                        let dw = to_wide(desc);
                        rt.DrawText(&dw[..dw.len().saturating_sub(1)], fmt, &rect_f(or.left + 16.0 * s, or.top + 36.0 * s, or.right - 12.0 * s, or.bottom - 6.0 * s), b_muted, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
                    }
                    self.hit_boxes.push(HitBox {
                        rect: RectI::new(or.left as i32, or.top as i32, or.right as i32, or.bottom as i32),
                        target,
                    });
                    cy += opt_h + 12.0 * s;
                }
            }
            ActiveModal::TemplateEdit {
                draft,
                default_horiz,
                default_vert,
                ..
            } => {
                // 标题 + 右上角复制图标
                if let Some(fmt) = modal_title_fmt {
                    let disp = localized_layout_name(draft, zh);
                    let title = if zh { format!("编辑\"{disp}\"") } else { format!("Edit \"{disp}\"") };
                    let tw = to_wide(&title);
                    rt.DrawText(&tw[..tw.len().saturating_sub(1)], fmt, &rect_f(ix, cy, ix + iw - 44.0 * s, cy + 28.0 * s), b_text, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
                }
                let dup_r = rect_f(ix + iw - 30.0 * s, cy, ix + iw, cy + 28.0 * s);
                if self.hovered == Some(HitTarget::ModalDuplicate) {
                    rt.FillRoundedRectangle(&rounded_rect(dup_r, 5.0 * s), b_hover);
                }
                draw_copy_icon(rt, (dup_r.left + dup_r.right) * 0.5, (dup_r.top + dup_r.bottom) * 0.5, 6.0 * s, b_text, 1.3 * s);
                self.hit_boxes.push(HitBox {
                    rect: RectI::new(dup_r.left as i32, dup_r.top as i32, dup_r.right as i32, dup_r.bottom as i32),
                    target: HitTarget::ModalDuplicate,
                });
                cy += 42.0 * s;

                // 区域数 - / N / +
                self.draw_stepper_row(
                    rt,
                    if zh { "区域数" } else { "Number of zones" },
                    &draft.zone_count.max(1).to_string(),
                    HitTarget::ModalZoneCountMinus,
                    HitTarget::ModalZoneCountPlus,
                    ix,
                    cy,
                    iw,
                    s,
                    b_thumb,
                    b_border,
                    b_hover,
                    b_text,
                    body_fmt,
                    body_center_fmt,
                );
                cy += 46.0 * s;

                // 区域周围的空间开关 + 滑块（图 10 vs 图 11 实时切换）
                cy = self.draw_spacing_and_highlight_controls(
                    rt,
                    draft,
                    cfg,
                    ix,
                    cy,
                    iw,
                    s,
                    zh,
                    b_thumb,
                    b_border,
                    b_hover,
                    b_text,
                    b_muted,
                    b_white,
                    b_accent,
                    body_fmt,
                    body_center_fmt,
                );

                // 横/竖屏默认星标
                self.draw_default_orientation_rows(
                    rt,
                    *default_horiz,
                    *default_vert,
                    ix,
                    cy + 8.0 * s,
                    iw,
                    s,
                    zh,
                    b_hover,
                    b_text,
                    b_accent,
                    body_fmt,
                );
            }
            ActiveModal::CustomProps {
                draft,
                name_focused,
                default_horiz,
                default_vert,
                ..
            } => {
                // 标题 + 复制/删除图标
                if let Some(fmt) = modal_title_fmt {
                    let title = if zh { format!("编辑\"{}\"", draft.name) } else { format!("Edit \"{}\"", draft.name) };
                    let tw = to_wide(&title);
                    rt.DrawText(&tw[..tw.len().saturating_sub(1)], fmt, &rect_f(ix, cy, ix + iw - 72.0 * s, cy + 28.0 * s), b_text, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
                }
                let del_r = rect_f(ix + iw - 28.0 * s, cy, ix + iw, cy + 28.0 * s);
                let dup_r = rect_f(del_r.left - 34.0 * s, cy, del_r.left - 6.0 * s, cy + 28.0 * s);
                if self.hovered == Some(HitTarget::ModalDuplicate) {
                    rt.FillRoundedRectangle(&rounded_rect(dup_r, 5.0 * s), b_hover);
                }
                draw_copy_icon(rt, (dup_r.left + dup_r.right) * 0.5, (dup_r.top + dup_r.bottom) * 0.5, 6.0 * s, b_text, 1.3 * s);
                self.hit_boxes.push(HitBox {
                    rect: RectI::new(dup_r.left as i32, dup_r.top as i32, dup_r.right as i32, dup_r.bottom as i32),
                    target: HitTarget::ModalDuplicate,
                });
                if self.hovered == Some(HitTarget::ModalDelete) {
                    rt.FillRoundedRectangle(&rounded_rect(del_r, 5.0 * s), b_hover);
                }
                draw_trash_icon(rt, (del_r.left + del_r.right) * 0.5, (del_r.top + del_r.bottom) * 0.5, 6.0 * s, b_danger, 1.3 * s);
                self.hit_boxes.push(HitBox {
                    rect: RectI::new(del_r.left as i32, del_r.top as i32, del_r.right as i32, del_r.bottom as i32),
                    target: HitTarget::ModalDelete,
                });
                cy += 38.0 * s;

                // 名称输入框
                self.draw_text_input_row(
                    rt,
                    if zh { "名称" } else { "Name" },
                    &draft.name,
                    *name_focused,
                    ix,
                    cy,
                    iw,
                    s,
                    b_thumb,
                    b_border,
                    b_accent,
                    b_text,
                    b_muted,
                    body_fmt,
                );
                cy += 60.0 * s;

                // 布局快捷键 (无 / 0..9)
                let hk_val = match draft.hotkey_digit {
                    Some(d) => format!("Win+Ctrl+Alt+{d}"),
                    None => if zh { "无" } else { "None" }.to_string(),
                };
                self.draw_stepper_row(
                    rt,
                    if zh { "布局快捷键" } else { "Shortcut key" },
                    &hk_val,
                    HitTarget::ModalHotkeyPrev,
                    HitTarget::ModalHotkeyNext,
                    ix,
                    cy,
                    iw,
                    s,
                    b_thumb,
                    b_border,
                    b_hover,
                    b_text,
                    body_fmt,
                    body_center_fmt,
                );
                cy += 44.0 * s;

                // 中央微缩预览 + 居中主题色圆形铅笔按钮（点它进 1:1 编辑区域！复刻图 4）
                let prev_w = (220.0 * s).min(iw);
                let prev_h = (110.0 * s).round();
                let pr = rect_f(ix + (iw - prev_w) * 0.5, cy, ix + (iw + prev_w) * 0.5, cy + prev_h);
                draw_layout_mini_preview(rt, draft, pr, cfg.snap_zones.zone_gap, s, b_thumb, b_thumb_zone, b_border);
                let pcx = (pr.left + pr.right) * 0.5;
                let pcy = (pr.top + pr.bottom) * 0.5;
                let cr = 20.0 * s;
                let btn_r = rect_f(pcx - cr, pcy - cr, pcx + cr, pcy + cr);
                rt.FillRoundedRectangle(&rounded_rect(btn_r, cr), b_accent);
                if self.hovered == Some(HitTarget::ModalEditZones) {
                    rt.DrawRoundedRectangle(&rounded_rect(btn_r, cr), b_white, 1.8 * s, None);
                }
                draw_pencil_icon(rt, pcx, pcy, 7.0 * s, b_white, 1.7 * s);
                self.hit_boxes.push(HitBox {
                    rect: RectI::new(pr.left as i32, pr.top as i32, pr.right as i32, pr.bottom as i32),
                    target: HitTarget::ModalEditZones,
                });
                cy += prev_h + 16.0 * s;

                // 间距与突出显示距离
                cy = self.draw_spacing_and_highlight_controls(
                    rt,
                    draft,
                    cfg,
                    ix,
                    cy,
                    iw,
                    s,
                    zh,
                    b_thumb,
                    b_border,
                    b_hover,
                    b_text,
                    b_muted,
                    b_white,
                    b_accent,
                    body_fmt,
                    body_center_fmt,
                );

                // 横/竖屏默认星标
                self.draw_default_orientation_rows(
                    rt,
                    *default_horiz,
                    *default_vert,
                    ix,
                    cy + 4.0 * s,
                    iw,
                    s,
                    zh,
                    b_hover,
                    b_text,
                    b_accent,
                    body_fmt,
                );
            }
        }

        // 弹窗底部双按钮：主题色主操作（保存 / 创建） + 取消
        let btn_h = (36.0 * s).round();
        let btn_y = mr.bottom - pad - btn_h;
        let btn_gap = (12.0 * s).round();
        let btn_w = ((iw - btn_gap) * 0.5).floor();
        let primary_r = rect_f(ix, btn_y, ix + btn_w, btn_y + btn_h);
        let cancel_r = rect_f(ix + btn_w + btn_gap, btn_y, ix + iw, btn_y + btn_h);

        rt.FillRoundedRectangle(&rounded_rect(primary_r, 6.0 * s), b_accent);
        rt.FillRoundedRectangle(&rounded_rect(cancel_r, 6.0 * s), b_thumb);
        rt.DrawRoundedRectangle(&rounded_rect(cancel_r, 6.0 * s), b_border, 1.0 * s, None);

        let primary_label = match modal {
            ActiveModal::NewLayout { .. } => if zh { "创建" } else { "Create" },
            _ => if zh { "保存" } else { "Save" },
        };
        let cancel_label = if zh { "取消" } else { "Cancel" };
        if let Some(fmt) = btn_fmt {
            let pw = to_wide(primary_label);
            let cw = to_wide(cancel_label);
            rt.DrawText(&pw[..pw.len().saturating_sub(1)], fmt, &primary_r, b_white, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            rt.DrawText(&cw[..cw.len().saturating_sub(1)], fmt, &cancel_r, b_text, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }
        self.hit_boxes.push(HitBox {
            rect: RectI::new(primary_r.left as i32, primary_r.top as i32, primary_r.right as i32, primary_r.bottom as i32),
            target: HitTarget::ModalSaveOrCreate,
        });
        self.hit_boxes.push(HitBox {
            rect: RectI::new(cancel_r.left as i32, cancel_r.top as i32, cancel_r.right as i32, cancel_r.bottom as i32),
            target: HitTarget::ModalCloseOrCancel,
        });
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn draw_text_input_row(
        &mut self,
        rt: &ID2D1RenderTarget,
        label: &str,
        value: &str,
        focused: bool,
        x: f32,
        y: f32,
        w: f32,
        s: f32,
        b_thumb: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_border: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_accent: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_text: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_muted: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        body_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
    ) {
        if let Some(fmt) = body_fmt {
            let lw = to_wide(label);
            rt.DrawText(&lw[..lw.len().saturating_sub(1)], fmt, &rect_f(x, y, x + w, y + 20.0 * s), b_muted, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }
        let ir = rect_f(x, y + 22.0 * s, x + w, y + 54.0 * s);
        rt.FillRoundedRectangle(&rounded_rect(ir, 6.0 * s), b_thumb);
        rt.DrawRoundedRectangle(&rounded_rect(ir, 6.0 * s), if focused { b_accent } else { b_border }, if focused { 1.8 * s } else { 1.0 * s }, None);
        if let Some(fmt) = body_fmt {
            let display = if focused { format!("{value}|") } else { value.to_string() };
            let vw = to_wide(&display);
            rt.DrawText(&vw[..vw.len().saturating_sub(1)], fmt, &rect_f(ir.left + 10.0 * s, ir.top + 7.0 * s, ir.right - 10.0 * s, ir.bottom - 4.0 * s), b_text, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }
        self.hit_boxes.push(HitBox {
            rect: RectI::new(ir.left as i32, ir.top as i32, ir.right as i32, ir.bottom as i32),
            target: HitTarget::ModalNameInput,
        });
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn draw_stepper_row(
        &mut self,
        rt: &ID2D1RenderTarget,
        label: &str,
        value_text: &str,
        minus_target: HitTarget,
        plus_target: HitTarget,
        x: f32,
        y: f32,
        w: f32,
        s: f32,
        b_thumb: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_border: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_hover: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_text: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        body_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
        body_center_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
    ) {
        let row_h = (32.0 * s).round();
        if let Some(fmt) = body_fmt {
            let lw = to_wide(label);
            rt.DrawText(&lw[..lw.len().saturating_sub(1)], fmt, &rect_f(x, y + 6.0 * s, x + w * 0.48, y + row_h), b_text, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }
        let btn_w = (32.0 * s).round();
        let val_w = (108.0 * s).round();
        let total_ctl = btn_w * 2.0 + val_w + 8.0 * s;
        let cx = x + w - total_ctl;
        let mr = rect_f(cx, y, cx + btn_w, y + row_h);
        let vr = rect_f(mr.right + 4.0 * s, y, mr.right + 4.0 * s + val_w, y + row_h);
        let pr = rect_f(vr.right + 4.0 * s, y, vr.right + 4.0 * s + btn_w, y + row_h);

        for (r, txt, target) in [(mr, "-", minus_target), (pr, "+", plus_target)] {
            rt.FillRoundedRectangle(&rounded_rect(r, 6.0 * s), b_thumb);
            if self.hovered.as_ref() == Some(&target) {
                rt.FillRoundedRectangle(&rounded_rect(r, 6.0 * s), b_hover);
            }
            rt.DrawRoundedRectangle(&rounded_rect(r, 6.0 * s), b_border, 1.0 * s, None);
            if let Some(fmt) = body_center_fmt {
                let tw = to_wide(txt);
                rt.DrawText(&tw[..tw.len().saturating_sub(1)], fmt, &r, b_text, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
            self.hit_boxes.push(HitBox {
                rect: RectI::new(r.left as i32, r.top as i32, r.right as i32, r.bottom as i32),
                target,
            });
        }

        rt.FillRoundedRectangle(&rounded_rect(vr, 6.0 * s), b_thumb);
        rt.DrawRoundedRectangle(&rounded_rect(vr, 6.0 * s), b_border, 1.0 * s, None);
        if let Some(fmt) = body_center_fmt {
            let vw = to_wide(value_text);
            rt.DrawText(&vw[..vw.len().saturating_sub(1)], fmt, &vr, b_text, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn draw_spacing_and_highlight_controls(
        &mut self,
        rt: &ID2D1RenderTarget,
        draft: &ZoneLayout,
        cfg: &WindowKitConfig,
        x: f32,
        mut y: f32,
        w: f32,
        s: f32,
        zh: bool,
        b_thumb: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_border: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        _b_hover: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_text: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_muted: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_white: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_accent: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        body_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
        body_center_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
    ) -> f32 {
        // 行 1：区域周围的空间 Toggle 开关
        if let Some(fmt) = body_fmt {
            let lw = to_wide(if zh { "区域周围的空间" } else { "Space around zones" });
            rt.DrawText(&lw[..lw.len().saturating_sub(1)], fmt, &rect_f(x, y + 3.0 * s, x + w - 58.0 * s, y + 26.0 * s), b_text, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }
        let sw_w = (44.0 * s).round();
        let sw_h = (22.0 * s).round();
        let sw_r = rect_f(x + w - sw_w, y + 2.0 * s, x + w, y + 2.0 * s + sw_h);
        rt.FillRoundedRectangle(&rounded_rect(sw_r, sw_h * 0.5), if draft.show_spacing { b_accent } else { b_thumb });
        rt.DrawRoundedRectangle(&rounded_rect(sw_r, sw_h * 0.5), if draft.show_spacing { b_accent } else { b_border }, 1.0 * s, None);
        let knob_r = (sw_h - 6.0 * s) * 0.5;
        let knob_cx = if draft.show_spacing { sw_r.right - 3.0 * s - knob_r } else { sw_r.left + 3.0 * s + knob_r };
        let knob_cy = (sw_r.top + sw_r.bottom) * 0.5;
        rt.FillRoundedRectangle(
            &rounded_rect(rect_f(knob_cx - knob_r, knob_cy - knob_r, knob_cx + knob_r, knob_cy + knob_r), knob_r),
            if draft.show_spacing { b_white } else { b_muted },
        );
        self.hit_boxes.push(HitBox {
            rect: RectI::new(sw_r.left as i32, sw_r.top as i32, sw_r.right as i32, sw_r.bottom as i32),
            target: HitTarget::ModalShowSpacingToggle,
        });
        y += 34.0 * s;

        // 间距像素滑块（开时可拖动，关时灰化显示 0px）
        let gap_val = if draft.show_spacing {
            draft.gap.unwrap_or(cfg.snap_zones.zone_gap).clamp(0, 48)
        } else {
            0
        };
        let spacing_track = self.draw_slider_row(
            rt,
            gap_val,
            48,
            draft.show_spacing,
            HitTarget::ModalSpacingSlider,
            HitTarget::ModalSpacingMinus,
            HitTarget::ModalSpacingPlus,
            x,
            y,
            w,
            s,
            zh,
            b_thumb,
            b_border,
            b_text,
            b_muted,
            b_white,
            b_accent,
            body_center_fmt,
        );
        self.spacing_slider_track = Some(spacing_track);
        y += 38.0 * s;

        // 行 2：突出显示距离滑块
        if let Some(fmt) = body_fmt {
            let lw = to_wide(if zh { "突出显示距离" } else { "Distance to highlight adjacent zones" });
            rt.DrawText(&lw[..lw.len().saturating_sub(1)], fmt, &rect_f(x, y, x + w, y + 22.0 * s), b_text, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }
        y += 24.0 * s;
        let hi_val = draft
            .highlight_distance
            .unwrap_or(cfg.snap_zones.edge_threshold_px)
            .clamp(0, 64);
        let hi_track = self.draw_slider_row(
            rt,
            hi_val,
            64,
            true,
            HitTarget::ModalHighlightSlider,
            HitTarget::ModalHighlightMinus,
            HitTarget::ModalHighlightPlus,
            x,
            y,
            w,
            s,
            zh,
            b_thumb,
            b_border,
            b_text,
            b_muted,
            b_white,
            b_accent,
            body_center_fmt,
        );
        self.highlight_slider_track = Some(hi_track);
        y + 38.0 * s
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn draw_slider_row(
        &mut self,
        rt: &ID2D1RenderTarget,
        value: i32,
        max_val: i32,
        enabled: bool,
        slider_target: HitTarget,
        minus_target: HitTarget,
        plus_target: HitTarget,
        x: f32,
        y: f32,
        w: f32,
        s: f32,
        zh: bool,
        b_thumb: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_border: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_text: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_muted: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_white: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_accent: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        body_center_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
    ) -> RectI {
        let row_h = (28.0 * s).round();
        let val_w = (76.0 * s).round();
        let btn_w = (24.0 * s).round();
        let track_left = x + btn_w + 10.0 * s;
        let track_right = x + w - val_w - btn_w - 14.0 * s;
        let cy = y + row_h * 0.5;

        let mr = rect_f(x, y + 2.0 * s, x + btn_w, y + row_h - 2.0 * s);
        let pr = rect_f(track_right + 10.0 * s, y + 2.0 * s, track_right + 10.0 * s + btn_w, y + row_h - 2.0 * s);
        for (r, txt, target) in [(mr, "-", minus_target), (pr, "+", plus_target)] {
            rt.FillRoundedRectangle(&rounded_rect(r, 5.0 * s), b_thumb);
            rt.DrawRoundedRectangle(&rounded_rect(r, 5.0 * s), b_border, 1.0 * s, None);
            if let Some(fmt) = body_center_fmt {
                let tw = to_wide(txt);
                rt.DrawText(&tw[..tw.len().saturating_sub(1)], fmt, &r, if enabled { b_text } else { b_muted }, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
            self.hit_boxes.push(HitBox {
                rect: RectI::new(r.left as i32, r.top as i32, r.right as i32, r.bottom as i32),
                target,
            });
        }

        // 水平滑轨
        let frac = (value as f32 / max_val.max(1) as f32).clamp(0.0, 1.0);
        let fill_x = track_left + (track_right - track_left) * frac;
        let bar_h = 4.0 * s;
        rt.FillRoundedRectangle(
            &rounded_rect(rect_f(track_left, cy - bar_h * 0.5, track_right, cy + bar_h * 0.5), bar_h * 0.5),
            b_thumb,
        );
        if enabled && fill_x > track_left {
            rt.FillRoundedRectangle(
                &rounded_rect(rect_f(track_left, cy - bar_h * 0.5, fill_x, cy + bar_h * 0.5), bar_h * 0.5),
                b_accent,
            );
        }
        let kr = 8.0 * s;
        rt.FillRoundedRectangle(
            &rounded_rect(rect_f(fill_x - kr, cy - kr, fill_x + kr, cy + kr), kr),
            if enabled { b_accent } else { b_muted },
        );
        rt.DrawRoundedRectangle(
            &rounded_rect(rect_f(fill_x - kr, cy - kr, fill_x + kr, cy + kr), kr),
            b_white,
            1.6 * s,
            None,
        );
        let track_rect = RectI::new(
            track_left as i32 - 4,
            y as i32,
            track_right as i32 + 4,
            (y + row_h) as i32,
        );
        self.hit_boxes.push(HitBox {
            rect: track_rect,
            target: slider_target,
        });

        // 右侧数值标签
        let vr = rect_f(x + w - val_w, y, x + w, y + row_h);
        if let Some(fmt) = body_center_fmt {
            let txt = if zh { format!("{value} 像素") } else { format!("{value} px") };
            let tw = to_wide(&txt);
            rt.DrawText(&tw[..tw.len().saturating_sub(1)], fmt, &vr, if enabled { b_text } else { b_muted }, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }

        track_rect
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn draw_default_orientation_rows(
        &mut self,
        rt: &ID2D1RenderTarget,
        default_horiz: bool,
        default_vert: bool,
        x: f32,
        y: f32,
        w: f32,
        s: f32,
        zh: bool,
        b_hover: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_text: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        b_accent: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
        body_fmt: Option<&windows::Win32::Graphics::DirectWrite::IDWriteTextFormat>,
    ) {
        let rows = [
            (
                default_horiz,
                HitTarget::ModalDefaultHoriz,
                if zh { "水平监视器方向的默认布局" } else { "Default layout for horizontal monitors" },
            ),
            (
                default_vert,
                HitTarget::ModalDefaultVert,
                if zh { "垂直监视器方向的默认布局" } else { "Default layout for vertical monitors" },
            ),
        ];
        let row_h = (30.0 * s).round();
        for (idx, (checked, target, label)) in rows.into_iter().enumerate() {
            let ry = y + idx as f32 * (row_h + 2.0 * s);
            let rr = rect_f(x, ry, x + w, ry + row_h);
            if self.hovered.as_ref() == Some(&target) {
                rt.FillRoundedRectangle(&rounded_rect(rr, 5.0 * s), b_hover);
            }
            // 星标几何勾选框
            let box_sz = 14.0 * s;
            let bx = x + 6.0 * s;
            let by = ry + (row_h - box_sz) * 0.5;
            let br = rect_f(bx, by, bx + box_sz, by + box_sz);
            if checked {
                rt.FillRoundedRectangle(&rounded_rect(br, 3.5 * s), b_accent);
            } else {
                rt.DrawRoundedRectangle(&rounded_rect(br, 3.5 * s), b_text, 1.3 * s, None);
            }
            if let Some(fmt) = body_fmt {
                let lw = to_wide(label);
                rt.DrawText(
                    &lw[..lw.len().saturating_sub(1)],
                    fmt,
                    &rect_f(bx + box_sz + 10.0 * s, ry + 5.0 * s, rr.right - 6.0 * s, rr.bottom),
                    if checked { b_accent } else { b_text },
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }
            self.hit_boxes.push(HitBox {
                rect: RectI::new(rr.left as i32, rr.top as i32, rr.right as i32, rr.bottom as i32),
                target,
            });
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. 纯矢量图标与布局缩略图绘制助手（100% 矢量，0 Emoji）
// ─────────────────────────────────────────────────────────────────────────────

fn localized_layout_name(lay: &ZoneLayout, zh: bool) -> String {
    if !lay.built_in {
        return lay.name.clone();
    }
    match lay.id.as_str() {
        "template_none" => if zh { "无布局" } else { "No layout" }.to_string(),
        "template_focus" => if zh { "焦点" } else { "Focus" }.to_string(),
        "template_columns" => if zh { "列" } else { "Columns" }.to_string(),
        "template_rows" => if zh { "行" } else { "Rows" }.to_string(),
        "template_grid" => if zh { "网格" } else { "Grid" }.to_string(),
        "template_priority_grid" => if zh { "“优先级”网格" } else { "Priority Grid" }.to_string(),
        _ => lay.name.clone(),
    }
}

#[allow(clippy::too_many_arguments)]
unsafe fn draw_layout_mini_preview(
    rt: &ID2D1RenderTarget,
    layout: &ZoneLayout,
    box_rect: windows::Win32::Graphics::Direct2D::Common::D2D_RECT_F,
    global_gap: i32,
    s: f32,
    b_thumb: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
    b_thumb_zone: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
    b_border: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
) {
    rt.FillRoundedRectangle(&rounded_rect(box_rect, 4.0 * s), b_thumb);
    if layout.layout_type == ZoneLayoutType::None {
        return;
    }
    let pad = 4.0 * s;
    let inner_w = ((box_rect.right - box_rect.left) - pad * 2.0).max(16.0);
    let inner_h = ((box_rect.bottom - box_rect.top) - pad * 2.0).max(12.0);
    let virt = RectI::new(0, 0, inner_w.round() as i32, inner_h.round() as i32);
    let mini_gap = if layout.show_spacing {
        ((layout.effective_gap(global_gap) as f32 / 8.0).clamp(1.5, 4.0) * s).round() as i32
    } else {
        0
    };
    let zones: Vec<ResolvedZone> = layout.resolve(virt, mini_gap);
    for z in zones {
        let zr = rect_f(
            box_rect.left + pad + z.rect.left as f32,
            box_rect.top + pad + z.rect.top as f32,
            box_rect.left + pad + z.rect.right as f32,
            box_rect.top + pad + z.rect.bottom as f32,
        );
        rt.FillRoundedRectangle(&rounded_rect(zr, 2.0 * s), b_thumb_zone);
        rt.DrawRoundedRectangle(&rounded_rect(zr, 2.0 * s), b_border, 0.8 * s, None);
    }
}

unsafe fn draw_pencil_icon(
    rt: &ID2D1RenderTarget,
    cx: f32,
    cy: f32,
    r: f32,
    brush: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
    stroke: f32,
) {
    rt.DrawLine(
        D2D_POINT_2F { x: cx - r * 0.7, y: cy + r * 0.7 },
        D2D_POINT_2F { x: cx + r * 0.65, y: cy - r * 0.65 },
        brush,
        stroke * 1.4,
        None,
    );
    rt.DrawLine(
        D2D_POINT_2F { x: cx - r * 0.9, y: cy + r * 0.9 },
        D2D_POINT_2F { x: cx - r * 0.45, y: cy + r * 0.75 },
        brush,
        stroke,
        None,
    );
}

unsafe fn draw_copy_icon(
    rt: &ID2D1RenderTarget,
    cx: f32,
    cy: f32,
    r: f32,
    brush: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
    stroke: f32,
) {
    rt.DrawRectangle(
        &rect_f(cx - r * 0.8, cy - r * 0.5, cx + r * 0.35, cy + r * 0.85),
        brush,
        stroke,
        None,
    );
    rt.DrawRectangle(
        &rect_f(cx - r * 0.35, cy - r * 0.9, cx + r * 0.8, cy + r * 0.45),
        brush,
        stroke,
        None,
    );
}

unsafe fn draw_trash_icon(
    rt: &ID2D1RenderTarget,
    cx: f32,
    cy: f32,
    r: f32,
    brush: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush,
    stroke: f32,
) {
    rt.DrawLine(
        D2D_POINT_2F { x: cx - r * 0.9, y: cy - r * 0.5 },
        D2D_POINT_2F { x: cx + r * 0.9, y: cy - r * 0.5 },
        brush,
        stroke,
        None,
    );
    rt.DrawRectangle(
        &rect_f(cx - r * 0.65, cy - r * 0.45, cx + r * 0.65, cy + r * 0.85),
        brush,
        stroke,
        None,
    );
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Win32 窗口注册与事件无锁转发
// ─────────────────────────────────────────────────────────────────────────────

fn ensure_hub_classes() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);

        let p_cls = to_wide(LAYOUT_HUB_PREVIEW_CLASS);
        let p_wc = WNDCLASSEXW {
            cbSize: size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(hub_preview_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(p_cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&p_wc);

        let h_cls = to_wide(LAYOUT_HUB_WINDOW_CLASS);
        let h_wc = WNDCLASSEXW {
            cbSize: size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
            lpfnWndProc: Some(hub_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(h_cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&h_wc);
    });
}

fn create_hub_preview_window() -> Option<HWND> {
    ensure_hub_classes();
    unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(LAYOUT_HUB_PREVIEW_CLASS);
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

fn create_hub_window(rect: RectI) -> Option<HWND> {
    ensure_hub_classes();
    unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(LAYOUT_HUB_WINDOW_CLASS);
        let title = to_wide("Tools3000 SnapZones");
        CreateWindowExW(
            WS_EX_TOPMOST,
            PCWSTR(cls.as_ptr()),
            PCWSTR(title.as_ptr()),
            WS_POPUP | WS_CLIPCHILDREN,
            rect.left,
            rect.top,
            rect.width().max(320),
            rect.height().max(240),
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

unsafe extern "system" fn hub_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if msg == WM_SETCURSOR {
        let hit_test = (lparam.0 as u32) & 0xFFFF;
        if hit_test == HTCLIENT {
            let kind = HUB_CURSOR_KIND.load(Ordering::Acquire);
            let _ = SetCursor(hub_cursor_handle(kind));
            return LRESULT(1);
        }
    }

    let ctrl_raw = HUB_CONTROL_HWND.load(Ordering::Acquire);
    if ctrl_raw != 0 {
        let ctrl_hwnd = HWND(ctrl_raw as *mut c_void);
        match msg {
            WM_MOUSEMOVE => {
                let (x, y) = unpack_lparam_xy(lparam.0);
                HUB_LATEST_PT.store(pack_xy(x, y), Ordering::Release);
                if HUB_MOVE_PENDING.swap(1, Ordering::AcqRel) == 0 {
                    let _ = PostMessageW(
                        ctrl_hwnd,
                        WM_WK_INTERNAL_HUB_EVENT,
                        WPARAM(HUB_EVT_MOUSEMOVE),
                        LPARAM(0),
                    );
                }
                return LRESULT(0);
            }
            WM_LBUTTONDOWN => {
                let _ = PostMessageW(ctrl_hwnd, WM_WK_INTERNAL_HUB_EVENT, WPARAM(HUB_EVT_LBUTTONDOWN), lparam);
                return LRESULT(0);
            }
            WM_LBUTTONUP => {
                let _ = PostMessageW(ctrl_hwnd, WM_WK_INTERNAL_HUB_EVENT, WPARAM(HUB_EVT_LBUTTONUP), lparam);
                return LRESULT(0);
            }
            WM_RBUTTONDOWN => {
                let _ = PostMessageW(ctrl_hwnd, WM_WK_INTERNAL_HUB_EVENT, WPARAM(HUB_EVT_RBUTTONDOWN), lparam);
                return LRESULT(0);
            }
            WM_MOUSEWHEEL => {
                let delta = ((wparam.0 >> 16) & 0xFFFF) as i16 as isize;
                let _ = PostMessageW(ctrl_hwnd, WM_WK_INTERNAL_HUB_EVENT, WPARAM(HUB_EVT_WHEEL), LPARAM(delta));
                return LRESULT(0);
            }
            WM_KEYDOWN => {
                let _ = PostMessageW(ctrl_hwnd, WM_WK_INTERNAL_HUB_EVENT, WPARAM(HUB_EVT_KEYDOWN), LPARAM(wparam.0 as isize));
                return LRESULT(0);
            }
            WM_CHAR => {
                let _ = PostMessageW(ctrl_hwnd, WM_WK_INTERNAL_HUB_EVENT, WPARAM(HUB_EVT_CHAR), LPARAM(wparam.0 as isize));
                return LRESULT(0);
            }
            WM_PAINT => {
                let _ = PostMessageW(ctrl_hwnd, WM_WK_INTERNAL_HUB_EVENT, WPARAM(HUB_EVT_PAINT), LPARAM(0));
            }
            WM_CLOSE => {
                let _ = PostMessageW(ctrl_hwnd, WM_WK_INTERNAL_HUB_EVENT, WPARAM(HUB_EVT_CLOSE), LPARAM(0));
                return LRESULT(0);
            }
            _ => {}
        }
    }
    DefWindowProcW(hwnd, msg, wparam, lparam)
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. 单元测试
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
