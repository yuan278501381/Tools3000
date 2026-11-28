// ─────────────────────────────────────────────────────────────────────────────
// features/layout_editor.rs — 方案 A：纯 Rust Direct2D 1:1 全屏原生 WYSIWYG 布局编辑器
//
// 世界级架构亮点:
//   1. 1:1 真实物理屏幕透视编辑：全屏分层透明窗口精确覆于目标显示器工作区（保留任务栏可见），
//      底层真实软件窗口原样透出，彻底告别在狭窄 WebView 小盒子里盲调比例。
//   2. 复用内存渲染表面 (EditorSurface)：开启时仅分配一次 32bpp DIB + ID2D1DCRenderTarget，
//      拖拽期间 0 DIB 重分配、144Hz 丝滑重绘；退出/保存瞬间彻底销毁归还全部物理内存。
//   3. 超越原版 PowerToys FancyZones 的两大杀手级增强：
//      - 一键「捕获当前真实窗口生成布局」(Capture Windows, 快捷键 C)
//      - 拖拽分区边界自动磁吸到底层真实软件窗口边缘 + 相邻分区缝隙 + 屏幕黄金分割线，
//        实时显示青色穿透参考线、吸附来源胶囊与真实物理像素 W x H px！
//   4. Grid + Canvas 双范式全功能 + 60 步 Ctrl+Z 撤销栈 + 可拖动悬浮玻璃控制舱。
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::config::{parse_hex_color, WindowKitConfig, ZoneLayout, ZoneLayoutType};
use crate::core::constants::{
    FILE_LAYOUT_EDITOR_RESULT, HOST_EDITOR_RESULT_MSG_NAME, HOST_MESSAGE_WINDOW_CLASS,
    LAYOUT_EDITOR_WINDOW_CLASS, WM_WK_INTERNAL_EDITOR_EVENT,
};
use crate::core::geometry::{
    resolve_canvas_zones, resolve_grid_zones, NormalizedZone, RectI, ResolvedZone,
};
use crate::core::json::JsonValue;
use crate::core::win_utils::{
    enumerate_monitors, enumerate_visible_windows_on_monitor, get_current_virtual_desktop_id,
    get_virtual_desktop_name, monitor_for_point, to_wide, write_text_file_atomic, MonitorInfo,
    RealWindowSnapshot,
};
use crate::render::d2d::{rect_f, rgb_a, rgba, rounded_rect, SharedGraphics};
use crate::{wk_debug, wk_info, wk_warn};
use std::ffi::c_void;
use std::mem::size_of;
use std::path::Path;
use std::sync::atomic::{AtomicI64, AtomicIsize, AtomicU32, Ordering};
use std::sync::Once;
use std::time::{Instant, SystemTime, UNIX_EPOCH};
use windows::core::{Interface, PCWSTR};
use windows::Win32::Foundation::{COLORREF, HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, SIZE, WPARAM};
use windows::Win32::Graphics::Direct2D::Common::D2D_POINT_2F;
use windows::Win32::Graphics::Direct2D::{
    ID2D1DCRenderTarget, ID2D1RenderTarget, ID2D1StrokeStyle, D2D1_CAP_STYLE_ROUND,
    D2D1_DASH_STYLE_DASH, D2D1_DRAW_TEXT_OPTIONS_CLIP, D2D1_STROKE_STYLE_PROPERTIES,
    D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE,
};
use windows::Win32::Graphics::DirectWrite::DWRITE_MEASURING_MODE_NATURAL;
use windows::Win32::Graphics::Gdi::{
    CreateCompatibleDC, CreateDIBSection, DeleteDC, DeleteObject, SelectObject, AC_SRC_ALPHA,
    AC_SRC_OVER, BITMAPINFO, BITMAPINFOHEADER, BI_RGB, BLENDFUNCTION, DIB_RGB_COLORS, HBITMAP,
    HDC, HGDIOBJ,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetAsyncKeyState, ReleaseCapture, SetCapture, SetFocus, VK_BACK, VK_CONTROL, VK_DELETE,
    VK_DOWN, VK_ESCAPE, VK_LEFT, VK_MENU, VK_RETURN, VK_RIGHT, VK_SHIFT, VK_TAB, VK_UP,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyWindow, FindWindowW, GetCursorPos, IsWindow,
    LoadCursorW, PostMessageW, RegisterClassExW, RegisterWindowMessageW, SetCursor,
    SetForegroundWindow, SetWindowPos, ShowWindow, UpdateLayeredWindow, CS_DBLCLKS, CS_HREDRAW,
    CS_VREDRAW, HCURSOR, HTCLIENT, HWND_TOPMOST, IDC_ARROW, IDC_HAND, IDC_SIZEALL, IDC_SIZENESW,
    IDC_SIZENS, IDC_SIZENWSE, IDC_SIZEWE, SW_HIDE, SW_SHOW, ULW_ALPHA, WM_CAPTURECHANGED,
    WM_CLOSE, WM_KEYDOWN, WM_KEYUP, WM_LBUTTONDBLCLK, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE,
    WM_RBUTTONDOWN, WM_SETCURSOR, WM_SYSKEYDOWN, WM_SYSKEYUP, WNDCLASSEXW, WS_EX_LAYERED,
    WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_POPUP,
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. 窗口过程与主线程之间的无锁事件转发契约
// ─────────────────────────────────────────────────────────────────────────────

static EDITOR_CONTROL_HWND: AtomicIsize = AtomicIsize::new(0);
static EDITOR_CURSOR_KIND: AtomicU32 = AtomicU32::new(0);
static EDITOR_LATEST_PT: AtomicI64 = AtomicI64::new(0);
static EDITOR_MOVE_PENDING: AtomicU32 = AtomicU32::new(0);
static HOST_EDITOR_RESULT_MSG: AtomicU32 = AtomicU32::new(0);

const CURSOR_ARROW: u32 = 0;
const CURSOR_HAND: u32 = 1;
const CURSOR_SIZEALL: u32 = 2;
const CURSOR_SIZEWE: u32 = 3;
const CURSOR_SIZENS: u32 = 4;
const CURSOR_SIZENWSE: u32 = 5;
const CURSOR_SIZENESW: u32 = 6;

const ED_EVT_MOUSEMOVE: usize = 1;
const ED_EVT_LBUTTONDOWN: usize = 2;
const ED_EVT_LBUTTONUP: usize = 3;
const ED_EVT_LBUTTONDBLCLK: usize = 4;
const ED_EVT_RBUTTONDOWN: usize = 5;
const ED_EVT_KEYDOWN: usize = 6;
const ED_EVT_KEYUP: usize = 7;
const ED_EVT_CAPTURE_LOST: usize = 8;
const ED_EVT_CLOSE: usize = 9;

const MAX_UNDO_STEPS: usize = 60;
const MAX_GRID_DIM: usize = 8;
const MIN_GRID_TRACK_PCT: f64 = 8.0;
const MIN_CANVAS_W: f64 = 0.06;
const MIN_CANVAS_H: f64 = 0.06;
const SNAP_THRESHOLD_DIP: f32 = 10.0;
const GRID_SLICER_SNAP_THRESHOLD_DIP: f32 = 8.0;

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

fn is_vk_down(vk: u16) -> bool {
    unsafe { (GetAsyncKeyState(vk as i32) as u16 & 0x8000) != 0 }
}

fn cursor_handle_for_kind(kind: u32) -> HCURSOR {
    unsafe {
        let id = match kind {
            CURSOR_HAND => IDC_HAND,
            CURSOR_SIZEALL => IDC_SIZEALL,
            CURSOR_SIZEWE => IDC_SIZEWE,
            CURSOR_SIZENS => IDC_SIZENS,
            CURSOR_SIZENWSE => IDC_SIZENWSE,
            CURSOR_SIZENESW => IDC_SIZENESW,
            _ => IDC_ARROW,
        };
        LoadCursorW(None, id).unwrap_or_default()
    }
}

fn host_result_message_id() -> u32 {
    let cached = HOST_EDITOR_RESULT_MSG.load(Ordering::Acquire);
    if cached != 0 {
        return cached;
    }
    let name = to_wide(HOST_EDITOR_RESULT_MSG_NAME);
    let msg = unsafe { RegisterWindowMessageW(PCWSTR(name.as_ptr())) };
    HOST_EDITOR_RESULT_MSG.store(msg, Ordering::Release);
    msg
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. 复用型 32bpp Direct2D 分层渲染表面（编辑期 0 重分配，退出即完全销毁归还内存）
// ─────────────────────────────────────────────────────────────────────────────

struct EditorSurface {
    width: i32,
    height: i32,
    mem_dc: HDC,
    dib: HBITMAP,
    old_obj: HGDIOBJ,
    dc_rt: ID2D1DCRenderTarget,
}

impl EditorSurface {
    fn new(gfx: &SharedGraphics, width: i32, height: i32) -> Result<Self, String> {
        if width <= 0 || height <= 0 {
            return Err("invalid surface dimensions".to_string());
        }
        let dc_rt = gfx.create_dc_target()?;
        unsafe {
            let mem_dc = CreateCompatibleDC(HDC::default());
            if mem_dc.is_invalid() {
                return Err("CreateCompatibleDC failed".to_string());
            }
            let bmi = BITMAPINFO {
                bmiHeader: BITMAPINFOHEADER {
                    biSize: size_of::<BITMAPINFOHEADER>() as u32,
                    biWidth: width,
                    biHeight: -height,
                    biPlanes: 1,
                    biBitCount: 32,
                    biCompression: BI_RGB.0,
                    ..Default::default()
                },
                ..Default::default()
            };
            let mut bits: *mut c_void = std::ptr::null_mut();
            let dib = match CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &mut bits, None, 0) {
                Ok(bmp) => bmp,
                Err(e) => {
                    let _ = DeleteDC(mem_dc);
                    return Err(format!("CreateDIBSection failed: {e}"));
                }
            };
            let old_obj = SelectObject(mem_dc, dib);
            let bind_rc = RECT {
                left: 0,
                top: 0,
                right: width,
                bottom: height,
            };
            if let Err(e) = dc_rt.BindDC(mem_dc, &bind_rc) {
                SelectObject(mem_dc, old_obj);
                let _ = DeleteObject(dib);
                let _ = DeleteDC(mem_dc);
                return Err(format!("BindDC failed: {e}"));
            }
            Ok(Self {
                width,
                height,
                mem_dc,
                dib,
                old_obj,
                dc_rt,
            })
        }
    }

    fn render<F>(&self, hwnd: HWND, screen_x: i32, screen_y: i32, draw_fn: F) -> Result<(), String>
    where
        F: FnOnce(&ID2D1RenderTarget),
    {
        unsafe {
            let rt: ID2D1RenderTarget = self
                .dc_rt
                .cast()
                .map_err(|e| format!("cast render target failed: {e}"))?;
            rt.SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt.BeginDraw();
            rt.Clear(Some(&rgba(0.0, 0.0, 0.0, 0.0)));
            draw_fn(&rt);
            rt.EndDraw(None, None)
                .map_err(|e| format!("EndDraw failed: {e}"))?;

            let dst_pt = POINT {
                x: screen_x,
                y: screen_y,
            };
            let src_pt = POINT { x: 0, y: 0 };
            let sz = SIZE {
                cx: self.width,
                cy: self.height,
            };
            let blend = BLENDFUNCTION {
                BlendOp: AC_SRC_OVER as u8,
                BlendFlags: 0,
                SourceConstantAlpha: 255,
                AlphaFormat: AC_SRC_ALPHA as u8,
            };
            UpdateLayeredWindow(
                hwnd,
                HDC::default(),
                Some(&dst_pt),
                Some(&sz),
                self.mem_dc,
                Some(&src_pt),
                COLORREF(0),
                Some(&blend),
                ULW_ALPHA,
            )
            .map_err(|e| format!("UpdateLayeredWindow failed: {e}"))
        }
    }
}

impl Drop for EditorSurface {
    fn drop(&mut self) {
        unsafe {
            if !self.mem_dc.is_invalid() {
                let _ = SelectObject(self.mem_dc, self.old_obj);
                let _ = DeleteObject(self.dib);
                let _ = DeleteDC(self.mem_dc);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. 纯数据布局编辑模型、智能磁吸引擎与「一键捕获当前窗口」算法（100% 可单测）
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq)]
pub struct EditorSnapshot {
    pub mode: ZoneLayoutType,
    pub zone_gap: i32,
    pub rows: usize,
    pub columns: usize,
    pub row_weights: Vec<f64>,
    pub col_weights: Vec<f64>,
    pub cell_map: Vec<Vec<usize>>,
    pub canvas_zones: Vec<NormalizedZone>,
}

#[derive(Debug, Clone)]
pub struct SnapTarget {
    pub pos_px: i32,
    pub label: String,
    pub priority: u8, // 0=屏幕边/黄金线, 1=同布局其他区, 2=底层真实窗口
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ActiveSnapGuide {
    pub is_vertical: bool,
    pub local_px: i32,
    pub label: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CanvasHandle {
    Move,
    N,
    S,
    E,
    W,
    NW,
    NE,
    SW,
    SE,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct GridDividerHit {
    is_vertical: bool,
    track_index: usize, // 分隔 track_index 与 track_index + 1 之间
    local_px: i32,
    span_start_px: i32,
    span_end_px: i32,
}

/// 纯数学磁吸计算：给定候选坐标和所有吸附目标，返回吸附后的新坐标与命中的视觉引导线
pub fn snap_coord_to_targets(
    raw_px: i32,
    targets: &[SnapTarget],
    threshold_px: i32,
    is_vertical: bool,
) -> (i32, Option<ActiveSnapGuide>) {
    let mut best: Option<(i32, u8, &SnapTarget)> = None;
    for t in targets {
        let dist = (raw_px - t.pos_px).abs();
        if dist <= threshold_px {
            let better = match best {
                None => true,
                Some((bd, bp, _)) => dist < bd || (dist == bd && t.priority > bp),
            };
            if better {
                best = Some((dist, t.priority, t));
            }
        }
    }
    match best {
        Some((_, _, t)) => (
            t.pos_px,
            Some(ActiveSnapGuide {
                is_vertical,
                local_px: t.pos_px,
                label: t.label.clone(),
            }),
        ),
        None => (raw_px, None),
    }
}

/// Grid 切割光刀（Slicer）实时预演与微磁吸求解结果（预览渲染与落刀切分共用单一事实源，100% 所见即所得）
#[derive(Debug, Clone, PartialEq)]
pub struct GridSlicerPreview {
    pub horizontal: bool,
    pub cut_local_px: i32,
    pub snapped: bool,
    pub snap_label: Option<String>,
    pub snap_from_window: bool,
    pub first_pct: i32,
    pub second_pct: i32,
    pub first_px: i32,
    pub second_px: i32,
    /// 在当前分区内部的归一化切分比例 (0.0..1.0)
    pub local_ratio: f64,
    /// 相对整块工作区的绝对切分比例 (0.0..1.0)
    pub abs_fraction: f64,
}

/// 计算 Grid 模式下单分区内高级切割光刀的精准落刀位置与微磁吸结果：
/// - 磁吸分区内经典等分点（25% / 33.3% / 50% / 66.7% / 75%）及底层真实桌面窗口边缘；
/// - 按住 Alt 时临时关闭所有磁吸，自由按像素落刀；
/// - 预览绘制与单击切分调用同一函数，保证 100% 所见即所得。
#[allow(clippy::too_many_arguments)]
pub fn compute_grid_slicer_preview(
    zone_local: RectI,
    work_area: RectI,
    cursor_local: (i32, i32),
    horizontal: bool,
    alt_disable_snap: bool,
    real_windows: &[RealWindowSnapshot],
    threshold_px: i32,
    zh_ui: bool,
) -> GridSlicerPreview {
    let work_w = work_area.width().max(1);
    let work_h = work_area.height().max(1);
    let (z_start, z_end, raw_px, work_len) = if horizontal {
        (zone_local.top, zone_local.bottom, cursor_local.1, work_h)
    } else {
        (zone_local.left, zone_local.right, cursor_local.0, work_w)
    };
    let z_len = (z_end - z_start).max(1);
    let min_inset = ((z_len as f64 * 0.12).round() as i32).clamp(1, (z_len / 2).max(1));
    let clamp_lo = z_start + min_inset;
    let clamp_hi = (z_end - min_inset).max(clamp_lo);
    let clamped_raw = raw_px.clamp(clamp_lo, clamp_hi);

    let (cut_px, snapped, snap_label, snap_from_window) = if alt_disable_snap || threshold_px <= 0 {
        (clamped_raw, false, None, false)
    } else {
        let mut targets = Vec::with_capacity(16);
        const CLASSIC_FRACS: &[(f64, &str)] = &[
            (0.25, "25%"),
            (1.0 / 3.0, "33.3%"),
            (0.5, "50%"),
            (2.0 / 3.0, "66.7%"),
            (0.75, "75%"),
        ];
        for &(frac, tag) in CLASSIC_FRACS {
            let pos = z_start + (frac * z_len as f64).round() as i32;
            if pos >= clamp_lo && pos <= clamp_hi {
                targets.push(SnapTarget {
                    pos_px: pos,
                    label: tag.to_string(),
                    priority: 1,
                });
            }
        }
        for win in real_windows {
            if win.is_maximized {
                continue;
            }
            let short_name = if win.process_name.is_empty() {
                if zh_ui {
                    "桌面窗口".to_string()
                } else {
                    "Window".to_string()
                }
            } else {
                win.process_name.clone()
            };
            let edges = if horizontal {
                [win.rect.top - work_area.top, win.rect.bottom - work_area.top]
            } else {
                [
                    win.rect.left - work_area.left,
                    win.rect.right - work_area.left,
                ]
            };
            for pos in edges {
                if pos >= clamp_lo && pos <= clamp_hi {
                    targets.push(SnapTarget {
                        pos_px: pos,
                        label: short_name.clone(),
                        priority: 2,
                    });
                }
            }
        }
        // 查找最优吸附并记录来源是否来自真实窗口（priority == 2）
        let mut best: Option<(i32, u8, &SnapTarget)> = None;
        for t in &targets {
            let dist = (clamped_raw - t.pos_px).abs();
            if dist <= threshold_px {
                let better = match best {
                    None => true,
                    Some((bd, bp, _)) => dist < bd || (dist == bd && t.priority > bp),
                };
                if better {
                    best = Some((dist, t.priority, t));
                }
            }
        }
        match best {
            Some((_, prio, t)) => (
                t.pos_px.clamp(clamp_lo, clamp_hi),
                true,
                Some(t.label.clone()),
                prio >= 2,
            ),
            None => (clamped_raw, false, None, false),
        }
    };

    let local_ratio = ((cut_px - z_start) as f64 / z_len as f64).clamp(0.12, 0.88);
    let first_pct = ((local_ratio * 100.0).round() as i32).clamp(1, 99);
    let second_pct = 100 - first_pct;
    let first_px = (cut_px - z_start).max(1);
    let second_px = (z_end - cut_px).max(1);
    let abs_fraction = (cut_px as f64 / work_len as f64).clamp(0.02, 0.98);

    GridSlicerPreview {
        horizontal,
        cut_local_px: cut_px,
        snapped,
        snap_label,
        snap_from_window,
        first_pct,
        second_pct,
        first_px,
        second_px,
        local_ratio,
        abs_fraction,
    }
}

/// 构建当前编辑态的全部水平/垂直智能磁吸目标（屏幕黄金线 + 其他分区边/缝隙 + 底层真实窗口可视边）
pub fn build_snap_targets(
    work_area: RectI,
    other_zones: &[ResolvedZone],
    real_windows: &[RealWindowSnapshot],
    zone_gap: i32,
    zh_ui: bool,
) -> (Vec<SnapTarget>, Vec<SnapTarget>) {
    let w = work_area.width().max(1);
    let h = work_area.height().max(1);
    let gap = zone_gap.max(0);
    let mut xs = Vec::with_capacity(32);
    let mut ys = Vec::with_capacity(32);

    // 1. 屏幕工作区边与黄金等分线
    let fractions: &[(f64, &str)] = &[
        (0.0, "0%"),
        (0.25, "25%"),
        (1.0 / 3.0, "33%"),
        (0.5, "50%"),
        (2.0 / 3.0, "67%"),
        (0.75, "75%"),
        (1.0, "100%"),
    ];
    for &(f, tag) in fractions {
        xs.push(SnapTarget {
            pos_px: (f * w as f64).round() as i32,
            label: tag.to_string(),
            priority: 0,
        });
        ys.push(SnapTarget {
            pos_px: (f * h as f64).round() as i32,
            label: tag.to_string(),
            priority: 0,
        });
    }

    // 2. 同布局其他分区的边缘与精确贴缝偏移（含 zone_gap 留白）
    for z in other_zones {
        let zl = z.rect.left - work_area.left;
        let zr = z.rect.right - work_area.left;
        let zt = z.rect.top - work_area.top;
        let zb = z.rect.bottom - work_area.top;
        let z_tag = if zh_ui {
            format!("区 {}", z.index + 1)
        } else {
            format!("Zone {}", z.index + 1)
        };
        for px in [zl, zr, (zl - gap).max(0), (zr + gap).min(w)] {
            xs.push(SnapTarget {
                pos_px: px,
                label: z_tag.clone(),
                priority: 1,
            });
        }
        for py in [zt, zb, (zt - gap).max(0), (zb + gap).min(h)] {
            ys.push(SnapTarget {
                pos_px: py,
                label: z_tag.clone(),
                priority: 1,
            });
        }
    }

    // 3. 底层真实桌面窗口的精确可视边缘（超越原版 FancyZones 核心增强！）
    for win in real_windows {
        let wl = (win.rect.left - work_area.left).clamp(0, w);
        let wr = (win.rect.right - work_area.left).clamp(0, w);
        let wt = (win.rect.top - work_area.top).clamp(0, h);
        let wb = (win.rect.bottom - work_area.top).clamp(0, h);
        let short_name = if win.process_name.is_empty() {
            if zh_ui { "桌面窗口".to_string() } else { "Window".to_string() }
        } else {
            win.process_name.clone()
        };
        if wl > 4 && wl < w - 4 {
            xs.push(SnapTarget {
                pos_px: wl,
                label: short_name.clone(),
                priority: 2,
            });
        }
        if wr > 4 && wr < w - 4 {
            xs.push(SnapTarget {
                pos_px: wr,
                label: short_name.clone(),
                priority: 2,
            });
        }
        if wt > 4 && wt < h - 4 {
            ys.push(SnapTarget {
                pos_px: wt,
                label: short_name.clone(),
                priority: 2,
            });
        }
        if wb > 4 && wb < h - 4 {
            ys.push(SnapTarget {
                pos_px: wb,
                label: short_name,
                priority: 2,
            });
        }
    }

    (xs, ys)
}

/// 杀手级核心算法：一键从当前显示器真实桌面窗口生成精确 1:1 对应的 Canvas 分区集合
/// - 优先采集非最大化窗口；若全为最大化窗口则自动回退包含；
/// - 剔除高度重合的冗余窗口（IoU >= 0.85），最多保留 12 个最有价值的真实工作区；
/// - 所有坐标严格归一化并钳制在 [0.0, 1.0] 范围内，永不溢屏。
pub fn capture_zones_from_real_windows(
    work_area: RectI,
    real_windows: &[RealWindowSnapshot],
) -> Vec<NormalizedZone> {
    if !work_area.is_valid() || real_windows.is_empty() {
        return Vec::new();
    }
    let work_w = work_area.width().max(1) as f64;
    let work_h = work_area.height().max(1) as f64;

    let mut candidates: Vec<&RealWindowSnapshot> = real_windows
        .iter()
        .filter(|w| !w.is_maximized)
        .collect();
    if candidates.is_empty() {
        candidates = real_windows.iter().take(4).collect();
    }

    let mut accepted_rects: Vec<RectI> = Vec::new();
    let mut zones: Vec<NormalizedZone> = Vec::new();

    for win in candidates {
        let Some(clipped) = win.rect.intersect(&work_area) else {
            continue;
        };
        if clipped.width() < 120 || clipped.height() < 90 {
            continue;
        }
        // 过滤与已收录窗口高度重叠（IoU >= 0.85）的重复窗口
        let duplicate = accepted_rects.iter().any(|prev| {
            let inter_area = prev.intersect(&clipped).map(|r| r.area()).unwrap_or(0);
            let union_area = (prev.area() + clipped.area() - inter_area).max(1);
            (inter_area as f64 / union_area as f64) >= 0.85
        });
        if duplicate {
            continue;
        }

        let nx = ((clipped.left - work_area.left) as f64 / work_w).clamp(0.0, 1.0 - MIN_CANVAS_W);
        let ny = ((clipped.top - work_area.top) as f64 / work_h).clamp(0.0, 1.0 - MIN_CANVAS_H);
        let nw = (clipped.width() as f64 / work_w).clamp(MIN_CANVAS_W, 1.0 - nx);
        let nh = (clipped.height() as f64 / work_h).clamp(MIN_CANVAS_H, 1.0 - ny);

        // 保留千分位精度（0.1%），兼顾精确对齐与 JSON 整洁度
        let rx = (nx * 1000.0).round() / 1000.0;
        let ry = (ny * 1000.0).round() / 1000.0;
        let rw = ((nw * 1000.0).round() / 1000.0).min(1.0 - rx).max(MIN_CANVAS_W);
        let rh = ((nh * 1000.0).round() / 1000.0).min(1.0 - ry).max(MIN_CANVAS_H);

        accepted_rects.push(clipped);
        zones.push(NormalizedZone {
            index: zones.len(),
            x: rx,
            y: ry,
            width: rw,
            height: rh,
        });
        if zones.len() >= 12 {
            break;
        }
    }

    zones
}

fn normalize_weights_to_100(weights: &[f64], count: usize) -> Vec<f64> {
    let n = count.max(1);
    if weights.len() != n {
        let each = 100.0 / (n as f64);
        let mut v = vec![each; n];
        let sum: f64 = v.iter().sum();
        if let Some(last) = v.last_mut() {
            *last += 100.0 - sum;
        }
        return v;
    }
    let pos: Vec<f64> = weights
        .iter()
        .map(|&w| if w.is_finite() && w > 0.0 { w } else { 1.0 })
        .collect();
    let sum: f64 = pos.iter().sum();
    if sum <= 0.0 {
        return normalize_weights_to_100(&[], n);
    }
    let mut scaled: Vec<f64> = pos
        .into_iter()
        .map(|w| ((w / sum) * 1000.0).round() / 10.0)
        .collect();
    let s2: f64 = scaled.iter().sum();
    if let Some(last) = scaled.last_mut() {
        *last = ((*last + (100.0 - s2)) * 10.0).round() / 10.0;
    }
    scaled
}

fn compact_cell_map(map: &[Vec<usize>]) -> Vec<Vec<usize>> {
    let mut mapping: Vec<(usize, usize)> = Vec::new();
    let mut next_id = 0usize;
    map.iter()
        .map(|row| {
            row.iter()
                .map(|&id| {
                    if let Some(&(_, mapped)) = mapping.iter().find(|&&(k, _)| k == id) {
                        mapped
                    } else {
                        let assigned = next_id;
                        next_id += 1;
                        mapping.push((id, assigned));
                        assigned
                    }
                })
                .collect()
        })
        .collect()
}

fn clamp_canvas_zone(mut z: NormalizedZone) -> NormalizedZone {
    z.width = z.width.clamp(MIN_CANVAS_W, 1.0);
    z.height = z.height.clamp(MIN_CANVAS_H, 1.0);
    z.x = z.x.clamp(0.0, (1.0 - z.width).max(0.0));
    z.y = z.y.clamp(0.0, (1.0 - z.height).max(0.0));
    z
}

fn snapshot_from_layout(layout: &ZoneLayout, default_gap: i32) -> EditorSnapshot {
    let gap = layout.effective_gap(default_gap).clamp(0, 48);
    match layout.layout_type {
        ZoneLayoutType::None => EditorSnapshot {
            mode: ZoneLayoutType::Grid,
            zone_gap: gap,
            rows: 1,
            columns: 2,
            row_weights: vec![100.0],
            col_weights: vec![50.0, 50.0],
            cell_map: vec![vec![0, 1]],
            canvas_zones: vec![
                NormalizedZone { index: 0, x: 0.0, y: 0.0, width: 0.5, height: 1.0 },
                NormalizedZone { index: 1, x: 0.5, y: 0.0, width: 0.5, height: 1.0 },
            ],
        },
        ZoneLayoutType::Focus => {
            let ref_work = RectI::new(0, 0, 2000, 1200);
            let resolved = crate::core::geometry::resolve_focus_zones(
                ref_work,
                layout.zone_count.max(1),
                gap,
            );
            let zones: Vec<NormalizedZone> = resolved
                .into_iter()
                .map(|z| {
                    clamp_canvas_zone(NormalizedZone {
                        index: z.index,
                        x: z.rect.left as f64 / 2000.0,
                        y: z.rect.top as f64 / 1200.0,
                        width: z.rect.width() as f64 / 2000.0,
                        height: z.rect.height() as f64 / 1200.0,
                    })
                })
                .collect();
            EditorSnapshot {
                mode: ZoneLayoutType::Canvas,
                zone_gap: gap,
                rows: 1,
                columns: 2,
                row_weights: vec![100.0],
                col_weights: vec![50.0, 50.0],
                cell_map: vec![vec![0, 1]],
                canvas_zones: zones,
            }
        }
        ZoneLayoutType::Grid => {
            let rows = layout.rows.clamp(1, MAX_GRID_DIM);
            let columns = layout.columns.clamp(1, MAX_GRID_DIM);
            let row_weights = normalize_weights_to_100(&layout.row_weights, rows);
            let col_weights = normalize_weights_to_100(&layout.col_weights, columns);
            let mut cell_map = if layout.cell_child_map.len() == rows
                && layout.cell_child_map.iter().all(|r| r.len() == columns)
            {
                layout.cell_child_map.clone()
            } else {
                let mut id = 0usize;
                (0..rows)
                    .map(|_| {
                        (0..columns)
                            .map(|_| {
                                let cur = id;
                                id += 1;
                                cur
                            })
                            .collect()
                    })
                    .collect()
            };
            cell_map = compact_cell_map(&cell_map);
            let canvas_zones = grid_snapshot_to_canvas_zones(
                rows,
                columns,
                &row_weights,
                &col_weights,
                &cell_map,
            );
            EditorSnapshot {
                mode: ZoneLayoutType::Grid,
                zone_gap: gap,
                rows,
                columns,
                row_weights,
                col_weights,
                cell_map,
                canvas_zones,
            }
        }
        ZoneLayoutType::Canvas => {
            let mut zones: Vec<NormalizedZone> = layout
                .zones
                .iter()
                .enumerate()
                .map(|(i, z)| {
                    let mut n = z.clone();
                    if n.width > 1.05 || n.height > 1.05 || n.x > 1.05 || n.y > 1.05 {
                        n.x /= 100.0;
                        n.y /= 100.0;
                        n.width /= 100.0;
                        n.height /= 100.0;
                    }
                    n.index = i;
                    clamp_canvas_zone(n)
                })
                .collect();
            if zones.is_empty() {
                zones.push(NormalizedZone {
                    index: 0,
                    x: 0.0,
                    y: 0.0,
                    width: 0.5,
                    height: 1.0,
                });
                zones.push(NormalizedZone {
                    index: 1,
                    x: 0.5,
                    y: 0.0,
                    width: 0.5,
                    height: 1.0,
                });
            }
            EditorSnapshot {
                mode: ZoneLayoutType::Canvas,
                zone_gap: gap,
                rows: 1,
                columns: 2,
                row_weights: vec![100.0],
                col_weights: vec![50.0, 50.0],
                cell_map: vec![vec![0, 1]],
                canvas_zones: zones,
            }
        }
    }
}

fn grid_snapshot_to_canvas_zones(
    rows: usize,
    columns: usize,
    row_weights: &[f64],
    col_weights: &[f64],
    cell_map: &[Vec<usize>],
) -> Vec<NormalizedZone> {
    let rw = normalize_weights_to_100(row_weights, rows);
    let cw = normalize_weights_to_100(col_weights, columns);
    let mut row_starts = vec![0.0f64; rows + 1];
    for r in 0..rows {
        row_starts[r + 1] = (row_starts[r] + rw[r] / 100.0).min(1.0);
    }
    row_starts[rows] = 1.0;

    let mut col_starts = vec![0.0f64; columns + 1];
    for c in 0..columns {
        col_starts[c + 1] = (col_starts[c] + cw[c] / 100.0).min(1.0);
    }
    col_starts[columns] = 1.0;

    let mut bounds: Vec<(usize, usize, usize, usize, usize)> = Vec::new();
    for r in 0..rows {
        for c in 0..columns {
            let zid = cell_map[r][c];
            if let Some(entry) = bounds.iter_mut().find(|e| e.0 == zid) {
                entry.1 = entry.1.min(r);
                entry.2 = entry.2.max(r);
                entry.3 = entry.3.min(c);
                entry.4 = entry.4.max(c);
            } else {
                bounds.push((zid, r, r, c, c));
            }
        }
    }
    bounds.sort_by_key(|e| (e.1, e.3, e.0));
    bounds
        .into_iter()
        .enumerate()
        .map(|(seq, (_, r0, r1, c0, c1))| {
            let x = col_starts[c0];
            let y = row_starts[r0];
            let w = (col_starts[c1 + 1] - x).max(MIN_CANVAS_W);
            let h = (row_starts[r1 + 1] - y).max(MIN_CANVAS_H);
            clamp_canvas_zone(NormalizedZone {
                index: seq,
                x,
                y,
                width: w,
                height: h,
            })
        })
        .collect()
}

impl EditorSnapshot {
    pub fn resolve(&self, work_area: RectI) -> Vec<ResolvedZone> {
        match self.mode {
            ZoneLayoutType::None => Vec::new(),
            ZoneLayoutType::Focus => crate::core::geometry::resolve_focus_zones(
                work_area,
                self.canvas_zones.len().max(1),
                self.zone_gap,
            ),
            ZoneLayoutType::Grid => resolve_grid_zones(
                work_area,
                self.rows,
                self.columns,
                &self.row_weights,
                &self.col_weights,
                &self.cell_map,
                self.zone_gap,
            ),
            ZoneLayoutType::Canvas => {
                resolve_canvas_zones(work_area, &self.canvas_zones, self.zone_gap)
            }
        }
    }

    pub fn to_zone_layout(&self, id: String, name: String, built_in: bool) -> ZoneLayout {
        match self.mode {
            ZoneLayoutType::Grid | ZoneLayoutType::None => ZoneLayout::new_grid(
                id,
                name,
                built_in,
                self.rows,
                self.columns,
                normalize_weights_to_100(&self.row_weights, self.rows),
                normalize_weights_to_100(&self.col_weights, self.columns),
                compact_cell_map(&self.cell_map),
                Some(self.zone_gap),
            ),
            ZoneLayoutType::Canvas | ZoneLayoutType::Focus => ZoneLayout::new_canvas(
                id,
                name,
                built_in,
                self.canvas_zones
                    .iter()
                    .enumerate()
                    .map(|(i, z)| {
                        let mut c = clamp_canvas_zone(z.clone());
                        c.index = i;
                        c
                    })
                    .collect(),
                Some(self.zone_gap),
            ),
        }
    }

    /// 网格就地精确切分（复刻并超越 PowerToys FancyZones 点击/Shift 切分）
    /// - `horizontal == false`: 沿 X 轴纵切（左右一分为二）
    /// - `horizontal == true`: 沿 Y 轴横切（上下一分为二）
    /// - `abs_fraction`: 0.0..1.0 相对整块工作区的切分位置
    pub fn split_grid_zone_at(
        &mut self,
        target_zone_id: usize,
        horizontal: bool,
        abs_fraction: f64,
    ) -> bool {
        if self.mode != ZoneLayoutType::Grid {
            return false;
        }
        let mut r0 = self.rows;
        let mut r1 = 0usize;
        let mut c0 = self.columns;
        let mut c1 = 0usize;
        let mut found = false;
        for r in 0..self.rows {
            for c in 0..self.columns {
                if self.cell_map[r][c] == target_zone_id {
                    found = true;
                    r0 = r0.min(r);
                    r1 = r1.max(r);
                    c0 = c0.min(c);
                    c1 = c1.max(c);
                }
            }
        }
        if !found {
            return false;
        }

        let new_zone_id = self
            .cell_map
            .iter()
            .flat_map(|r| r.iter())
            .copied()
            .max()
            .unwrap_or(0)
            + 1;

        if !horizontal {
            // 纵切：若目标区已跨越多个列，优先沿最接近光标的内部列边界直接拆开（不增加轨道数！）
            let mut col_starts = vec![0.0f64; self.columns + 1];
            for c in 0..self.columns {
                col_starts[c + 1] = col_starts[c] + self.col_weights[c] / 100.0;
            }
            if c1 > c0 {
                let mut best_split = c0 + 1;
                let mut best_dist = f64::MAX;
                for split_c in (c0 + 1)..=c1 {
                    let d = (col_starts[split_c] - abs_fraction).abs();
                    if d < best_dist {
                        best_dist = d;
                        best_split = split_c;
                    }
                }
                for r in r0..=r1 {
                    for c in best_split..=c1 {
                        if self.cell_map[r][c] == target_zone_id {
                            self.cell_map[r][c] = new_zone_id;
                        }
                    }
                }
                self.cell_map = compact_cell_map(&self.cell_map);
                return true;
            }
            if self.columns >= MAX_GRID_DIM {
                return false;
            }
            // 在第 c0 列内部按精确比例一分为二
            let col_start = col_starts[c0];
            let col_span = (self.col_weights[c0] / 100.0).max(0.01);
            let local_ratio = ((abs_fraction - col_start) / col_span).clamp(0.12, 0.88);
            let total_w = self.col_weights[c0];
            let w_left = (total_w * local_ratio).max(MIN_GRID_TRACK_PCT / 2.0);
            let w_right = (total_w - w_left).max(MIN_GRID_TRACK_PCT / 2.0);
            self.col_weights[c0] = w_left;
            self.col_weights.insert(c0 + 1, w_right);
            self.columns += 1;
            self.col_weights = normalize_weights_to_100(&self.col_weights, self.columns);

            for r in 0..self.rows {
                let orig = self.cell_map[r][c0];
                let inserted = if r >= r0 && r <= r1 && orig == target_zone_id {
                    new_zone_id
                } else {
                    orig
                };
                self.cell_map[r].insert(c0 + 1, inserted);
            }
            self.cell_map = compact_cell_map(&self.cell_map);
            true
        } else {
            // 横切：若目标区已跨越多个行，沿最接近光标的内部行边界直接拆开
            let mut row_starts = vec![0.0f64; self.rows + 1];
            for r in 0..self.rows {
                row_starts[r + 1] = row_starts[r] + self.row_weights[r] / 100.0;
            }
            if r1 > r0 {
                let mut best_split = r0 + 1;
                let mut best_dist = f64::MAX;
                for split_r in (r0 + 1)..=r1 {
                    let d = (row_starts[split_r] - abs_fraction).abs();
                    if d < best_dist {
                        best_dist = d;
                        best_split = split_r;
                    }
                }
                for r in best_split..=r1 {
                    for c in c0..=c1 {
                        if self.cell_map[r][c] == target_zone_id {
                            self.cell_map[r][c] = new_zone_id;
                        }
                    }
                }
                self.cell_map = compact_cell_map(&self.cell_map);
                return true;
            }
            if self.rows >= MAX_GRID_DIM {
                return false;
            }
            let row_start = row_starts[r0];
            let row_span = (self.row_weights[r0] / 100.0).max(0.01);
            let local_ratio = ((abs_fraction - row_start) / row_span).clamp(0.12, 0.88);
            let total_h = self.row_weights[r0];
            let h_top = (total_h * local_ratio).max(MIN_GRID_TRACK_PCT / 2.0);
            let h_bot = (total_h - h_top).max(MIN_GRID_TRACK_PCT / 2.0);
            self.row_weights[r0] = h_top;
            self.row_weights.insert(r0 + 1, h_bot);
            self.rows += 1;
            self.row_weights = normalize_weights_to_100(&self.row_weights, self.rows);

            let mut new_row = self.cell_map[r0].clone();
            for c in c0..=c1 {
                if new_row[c] == target_zone_id {
                    new_row[c] = new_zone_id;
                }
            }
            self.cell_map.insert(r0 + 1, new_row);
            self.cell_map = compact_cell_map(&self.cell_map);
            true
        }
    }

    /// 将 GridSlicerPreview 精准映射为传给 split_grid_zone_at 的绝对轨道比例，
    /// 消除存在 zone_gap 时单轨切分比例偏移，确保 50%/33.3%/25% 所见即所得落刀。
    pub fn zone_split_abs_fraction(
        &self,
        target_zone_id: usize,
        horizontal: bool,
        slicer: &GridSlicerPreview,
    ) -> f64 {
        if self.mode != ZoneLayoutType::Grid {
            return slicer.abs_fraction;
        }
        let mut r0 = self.rows;
        let mut r1 = 0usize;
        let mut c0 = self.columns;
        let mut c1 = 0usize;
        let mut found = false;
        for r in 0..self.rows {
            for c in 0..self.columns {
                if self.cell_map[r][c] == target_zone_id {
                    found = true;
                    r0 = r0.min(r);
                    r1 = r1.max(r);
                    c0 = c0.min(c);
                    c1 = c1.max(c);
                }
            }
        }
        if !found {
            return slicer.abs_fraction;
        }
        if !horizontal && c0 == c1 {
            let start: f64 = self.col_weights[..c0].iter().sum::<f64>() / 100.0;
            let span = self.col_weights[c0] / 100.0;
            (start + slicer.local_ratio * span).clamp(0.01, 0.99)
        } else if horizontal && r0 == r1 {
            let start: f64 = self.row_weights[..r0].iter().sum::<f64>() / 100.0;
            let span = self.row_weights[r0] / 100.0;
            (start + slicer.local_ratio * span).clamp(0.01, 0.99)
        } else {
            slicer.abs_fraction
        }
    }

    /// 合并网格中所有被框选命中的区域（按外接矩形闭包含，自动折叠全同冗余行列）
    pub fn merge_grid_zones(&mut self, zone_ids: &[usize]) -> bool {
        if self.mode != ZoneLayoutType::Grid || zone_ids.len() < 2 {
            return false;
        }
        let mut r0 = self.rows;
        let mut r1 = 0usize;
        let mut c0 = self.columns;
        let mut c1 = 0usize;
        let mut found = false;
        for r in 0..self.rows {
            for c in 0..self.columns {
                if zone_ids.contains(&self.cell_map[r][c]) {
                    found = true;
                    r0 = r0.min(r);
                    r1 = r1.max(r);
                    c0 = c0.min(c);
                    c1 = c1.max(c);
                }
            }
        }
        if !found {
            return false;
        }
        let target_id = zone_ids[0];
        let mut changed = false;
        for r in r0..=r1 {
            for c in c0..=c1 {
                if self.cell_map[r][c] != target_id {
                    self.cell_map[r][c] = target_id;
                    changed = true;
                }
            }
        }
        if !changed {
            return false;
        }
        self.prune_redundant_grid_tracks();
        self.cell_map = compact_cell_map(&self.cell_map);
        true
    }

    /// 折叠相邻两行或两列完全相同的冗余网格轨道（保持网格拓扑干净整洁）
    fn prune_redundant_grid_tracks(&mut self) {
        // 合并完全相同的相邻列
        let mut c = 0usize;
        while c + 1 < self.columns {
            let identical = (0..self.rows).all(|r| self.cell_map[r][c] == self.cell_map[r][c + 1]);
            if identical {
                let merged_w = self.col_weights[c] + self.col_weights[c + 1];
                self.col_weights[c] = merged_w;
                self.col_weights.remove(c + 1);
                for r in 0..self.rows {
                    self.cell_map[r].remove(c + 1);
                }
                self.columns -= 1;
            } else {
                c += 1;
            }
        }
        // 合并完全相同的相邻行
        let mut r = 0usize;
        while r + 1 < self.rows {
            if self.cell_map[r] == self.cell_map[r + 1] {
                let merged_h = self.row_weights[r] + self.row_weights[r + 1];
                self.row_weights[r] = merged_h;
                self.row_weights.remove(r + 1);
                self.cell_map.remove(r + 1);
                self.rows -= 1;
            } else {
                r += 1;
            }
        }
        self.col_weights = normalize_weights_to_100(&self.col_weights, self.columns);
        self.row_weights = normalize_weights_to_100(&self.row_weights, self.rows);
    }

    /// 删除指定网格分区（自动并入相邻最优邻居，确保不出现空洞）
    pub fn delete_grid_zone(&mut self, zone_id: usize) -> bool {
        if self.mode != ZoneLayoutType::Grid {
            return false;
        }
        let mut neighbor: Option<usize> = None;
        for r in 0..self.rows {
            for c in 0..self.columns {
                if self.cell_map[r][c] == zone_id {
                    let probes = [
                        (r, c.wrapping_sub(1)),
                        (r, c + 1),
                        (r.wrapping_sub(1), c),
                        (r + 1, c),
                    ];
                    for (pr, pc) in probes {
                        if pr < self.rows && pc < self.columns {
                            let cand = self.cell_map[pr][pc];
                            if cand != zone_id {
                                neighbor = Some(cand);
                                break;
                            }
                        }
                    }
                }
                if neighbor.is_some() {
                    break;
                }
            }
            if neighbor.is_some() {
                break;
            }
        }
        let Some(nb) = neighbor else {
            return false;
        };
        self.merge_grid_zones(&[nb, zone_id])
    }

    /// 拖动网格内部某条分隔线，调整相邻两条轨道权重
    pub fn move_grid_divider(
        &mut self,
        is_vertical: bool,
        track_index: usize,
        new_pos_fraction: f64,
    ) -> bool {
        if self.mode != ZoneLayoutType::Grid {
            return false;
        }
        let weights = if is_vertical {
            if track_index + 1 >= self.columns {
                return false;
            }
            &mut self.col_weights
        } else {
            if track_index + 1 >= self.rows {
                return false;
            }
            &mut self.row_weights
        };
        let prefix: f64 = weights[..track_index].iter().sum();
        let pair_sum = weights[track_index] + weights[track_index + 1];
        let min_w = MIN_GRID_TRACK_PCT.min(pair_sum * 0.4);
        let desired_first = (new_pos_fraction * 100.0 - prefix).clamp(min_w, pair_sum - min_w);
        let next_first = (desired_first * 10.0).round() / 10.0;
        let next_second = ((pair_sum - next_first) * 10.0).round() / 10.0;
        if (weights[track_index] - next_first).abs() < 0.05 {
            return false;
        }
        weights[track_index] = next_first;
        weights[track_index + 1] = next_second;
        true
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. 悬浮控制舱按钮几何与交互状态
// ─────────────────────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DockButtonId {
    DragGrip,
    ModeGrid,
    ModeCanvas,
    AddOrPreset,
    CaptureWindows,
    GapMinus,
    GapPlus,
    Undo,
    Cancel,
    Save,
}

#[derive(Debug, Clone)]
struct DockButtonRect {
    id: DockButtonId,
    rect: RectI, // 相对编辑器窗口局部坐标
    label: String,
    active: bool,
    primary: bool,
    accent_outline: bool,
    disabled: bool,
}

#[derive(Debug, Clone)]
enum ActiveDrag {
    DockMove {
        offset_x: i32,
        offset_y: i32,
    },
    GridDivider {
        is_vertical: bool,
        track_index: usize,
    },
    GridMarquee {
        start_x: i32,
        start_y: i32,
        cur_x: i32,
        cur_y: i32,
        start_zone: Option<usize>,
    },
    CanvasTransform {
        zone_idx: usize,
        handle: CanvasHandle,
        start_x: i32,
        start_y: i32,
        orig_zone: NormalizedZone,
        pushed_undo: bool,
    },
}

pub struct OpenEditorRequest {
    pub monitor_id: Option<String>,
    pub desktop_id: Option<String>,
    pub layout_id: Option<String>,
    pub restore_settings_hwnd: Option<isize>,
    pub ui_lang: String,
    pub return_to_hub: bool,
    pub initial_mode: Option<ZoneLayoutType>,
    pub custom_name: Option<String>,
    pub is_new_custom: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EditorCloseOutcome {
    StillOpen,
    Closed { saved: bool, return_to_hub: bool },
}

struct EditorSession {
    hwnd: HWND,
    surface: EditorSurface,
    monitor: MonitorInfo,
    desktop_id: Option<String>,
    desktop_label: String,
    per_virtual_desktop: bool,
    source_layout_id: String,
    source_layout_name: String,
    source_built_in: bool,
    source_show_spacing: bool,
    source_highlight_distance: Option<i32>,
    source_hotkey_digit: Option<u8>,
    return_to_hub: bool,
    #[allow(dead_code)]
    is_new_custom: bool,
    dirty: bool,
    zh_ui: bool,
    dark_theme: bool,
    accent_rgb: [f32; 3],
    restore_settings_hwnd: Option<isize>,
    state: EditorSnapshot,
    undo_stack: Vec<EditorSnapshot>,
    preset_cycle: usize,
    real_windows: Vec<RealWindowSnapshot>,
    selected_zone: Option<usize>,
    hovered_zone: Option<usize>,
    hovered_divider: Option<GridDividerHit>,
    hovered_dock_btn: Option<DockButtonId>,
    cursor_local: (i32, i32),
    shift_down: bool,
    alt_down: bool,
    dock_x: i32,
    dock_y: i32,
    dock_rect: RectI,
    dock_buttons: Vec<DockButtonRect>,
    drag: Option<ActiveDrag>,
    active_guides: Vec<ActiveSnapGuide>,
    status_toast: Option<(String, Instant)>,
}

pub struct LayoutEditorManager {
    control_hwnd: HWND,
    session: Option<EditorSession>,
}

impl LayoutEditorManager {
    pub fn new(control_hwnd: HWND) -> Self {
        Self {
            control_hwnd,
            session: None,
        }
    }

    pub fn set_control_hwnd(&mut self, hwnd: HWND) {
        self.control_hwnd = hwnd;
        EDITOR_CONTROL_HWND.store(hwnd.0 as isize, Ordering::Release);
    }

    pub fn is_open(&self) -> bool {
        self.session.is_some()
    }

    /// 启动 1:1 全屏原生 Direct2D 布局编辑器（自动识别目标显示器、当前虚拟桌面与底层真实窗口）
    pub fn open(
        &mut self,
        req: OpenEditorRequest,
        cfg: &WindowKitConfig,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> bool {
        // 若已开启则直接置顶聚焦
        if let Some(sess) = &self.session {
            unsafe {
                let _ = SetForegroundWindow(sess.hwnd);
                let _ = SetFocus(sess.hwnd);
            }
            return true;
        }
        let Some(g) = gfx else {
            wk_warn!(trace_id, "layoutEditor", "SharedGraphics unavailable");
            return false;
        };
        let monitors = enumerate_monitors();
        if monitors.is_empty() {
            return false;
        }

        // 1. 解析目标显示器（优先显式指定 monitorId，否则取光标所在显示器）
        let target_mon = req
            .monitor_id
            .as_deref()
            .filter(|s| !s.is_empty())
            .and_then(|mid| {
                monitors
                    .iter()
                    .find(|m| m.id.eq_ignore_ascii_case(mid))
                    .cloned()
            })
            .or_else(|| {
                let mut pt = POINT::default();
                if unsafe { GetCursorPos(&mut pt) }.is_ok() {
                    monitor_for_point(pt.x, pt.y, &monitors)
                } else {
                    None
                }
            })
            .or_else(|| monitors.first().cloned());
        let Some(mon) = target_mon else {
            return false;
        };

        // 2. 解析当前虚拟桌面 GUID 与自定义名称
        let desktop_id = req
            .desktop_id
            .filter(|s| !s.trim().is_empty())
            .or_else(get_current_virtual_desktop_id);
        let desktop_label = desktop_id
            .as_deref()
            .and_then(get_virtual_desktop_name)
            .unwrap_or_default();

        // 3. 解析待编辑布局（支持从 Hub 新建网格/画布自定义布局或编辑既有布局）
        let zh_ui = !req.ui_lang.trim().to_ascii_lowercase().starts_with("en");
        let chosen_layout: ZoneLayout = if req.is_new_custom {
            let mode = match req.initial_mode {
                Some(ZoneLayoutType::Canvas) => ZoneLayoutType::Canvas,
                _ => ZoneLayoutType::Grid,
            };
            let ts = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|d| d.as_millis())
                .unwrap_or(0);
            let name = req
                .custom_name
                .filter(|s| !s.trim().is_empty())
                .unwrap_or_else(|| {
                    if zh_ui {
                        "自定义布局".to_string()
                    } else {
                        "Custom Layout".to_string()
                    }
                });
            if mode == ZoneLayoutType::Canvas {
                ZoneLayout::new_canvas(
                    format!("custom_{ts}"),
                    name,
                    false,
                    vec![NormalizedZone {
                        index: 0,
                        x: 0.28,
                        y: 0.18,
                        width: 0.44,
                        height: 0.62,
                    }],
                    Some(cfg.snap_zones.zone_gap),
                )
            } else {
                ZoneLayout::new_grid(
                    format!("custom_{ts}"),
                    name,
                    false,
                    1,
                    2,
                    vec![100.0],
                    vec![50.0, 50.0],
                    vec![vec![0, 1]],
                    Some(cfg.snap_zones.zone_gap),
                )
            }
        } else {
            req.layout_id
                .as_deref()
                .filter(|s| !s.is_empty())
                .and_then(|lid| cfg.find_layout(lid))
                .unwrap_or_else(|| {
                    cfg.resolve_layout_for_monitor(
                        &mon,
                        if cfg.snap_zones.per_virtual_desktop {
                            desktop_id.as_deref()
                        } else {
                            None
                        },
                    )
                })
                .clone()
        };

        let work = mon.work_rect;
        let w = work.width();
        let h = work.height();
        if w < 200 || h < 160 {
            return false;
        }

        // 4. 在覆盖层显示前，先采集底层真实可见窗口（排除触发编辑器的设置窗）
        let exclude_hwnd = req
            .restore_settings_hwnd
            .filter(|&raw| raw > 0)
            .map(|raw| HWND(raw as *mut c_void));
        let real_windows = enumerate_visible_windows_on_monitor(&mon, exclude_hwnd, &[]);

        // 5. 创建复用表面与全屏分层交互窗口
        let surface = match EditorSurface::new(g, w, h) {
            Ok(s) => s,
            Err(e) => {
                wk_warn!(trace_id, "layoutEditor", "surface init failed: {}", e);
                return false;
            }
        };
        let Some(hwnd) = create_layout_editor_window(work) else {
            wk_warn!(trace_id, "layoutEditor", "create editor window failed");
            return false;
        };

        let scale = (mon.dpi as f32 / 96.0).max(1.0);
        let state = snapshot_from_layout(&chosen_layout, chosen_layout.effective_gap(cfg.snap_zones.zone_gap));
        let selected_zone = if state.mode == ZoneLayoutType::Canvas && !state.canvas_zones.is_empty() {
            Some(0)
        } else {
            None
        };

        let mut sess = EditorSession {
            hwnd,
            surface,
            monitor: mon.clone(),
            desktop_id,
            desktop_label,
            per_virtual_desktop: cfg.snap_zones.per_virtual_desktop,
            source_layout_id: chosen_layout.id.clone(),
            source_layout_name: chosen_layout.name.clone(),
            source_built_in: chosen_layout.built_in,
            source_show_spacing: chosen_layout.show_spacing,
            source_highlight_distance: chosen_layout.highlight_distance,
            source_hotkey_digit: chosen_layout.hotkey_digit,
            return_to_hub: req.return_to_hub,
            is_new_custom: req.is_new_custom,
            dirty: req.is_new_custom,
            zh_ui,
            dark_theme: cfg.is_dark_theme(),
            accent_rgb: parse_hex_color(&cfg.accent_color, [0.23, 0.51, 0.96]),
            restore_settings_hwnd: req.restore_settings_hwnd,
            state,
            undo_stack: Vec::new(),
            preset_cycle: 0,
            real_windows,
            selected_zone,
            hovered_zone: None,
            hovered_divider: None,
            hovered_dock_btn: None,
            cursor_local: (w / 2, h / 2),
            shift_down: is_vk_down(VK_SHIFT.0),
            alt_down: is_vk_down(VK_MENU.0),
            dock_x: 0,
            dock_y: (18.0 * scale).round() as i32,
            dock_rect: RectI::default(),
            dock_buttons: Vec::new(),
            drag: None,
            active_guides: Vec::new(),
            status_toast: None,
        };
        sess.rebuild_dock_geometry(true);
        sess.render(g);

        unsafe {
            let _ = SetWindowPos(hwnd, HWND_TOPMOST, work.left, work.top, w, h, Default::default());
            let _ = ShowWindow(hwnd, SW_SHOW);
            let _ = SetForegroundWindow(hwnd);
            let _ = SetFocus(hwnd);
        }

        EDITOR_CURSOR_KIND.store(CURSOR_ARROW, Ordering::Release);
        wk_info!(
            trace_id,
            "layoutEditor",
            "opened 1:1 fullscreen editor on {} ({}x{} @{}%) layout='{}' realWindows={}",
            mon.friendly_name,
            w,
            h,
            mon.scale_percent,
            chosen_layout.id,
            sess.real_windows.len()
        );
        self.session = Some(sess);
        true
    }

    /// 多屏拔插、旋转或分辨率变化时原地重建 Surface 并自适应重排，避免花屏或卡死
    pub fn on_topology_changed(&mut self, gfx: Option<&SharedGraphics>) {
        let (Some(sess), Some(g)) = (self.session.as_mut(), gfx) else {
            return;
        };
        let monitors = enumerate_monitors();
        if monitors.is_empty() {
            return;
        }
        let next_mon = monitors
            .iter()
            .find(|m| m.id.eq_ignore_ascii_case(&sess.monitor.id))
            .cloned()
            .unwrap_or_else(|| monitors[0].clone());
        let w = next_mon.work_rect.width();
        let h = next_mon.work_rect.height();
        if w < 200 || h < 160 {
            return;
        }
        if next_mon.work_rect != sess.monitor.work_rect || next_mon.dpi != sess.monitor.dpi {
            if let Ok(new_surface) = EditorSurface::new(g, w, h) {
                sess.surface = new_surface;
                sess.monitor = next_mon.clone();
                sess.rebuild_dock_geometry(true);
                unsafe {
                    let _ = SetWindowPos(
                        sess.hwnd,
                        HWND_TOPMOST,
                        next_mon.work_rect.left,
                        next_mon.work_rect.top,
                        w,
                        h,
                        Default::default(),
                    );
                }
                sess.render(g);
            }
        }
    }

    /// 处理由窗口过程转发来的交互事件
    pub fn handle_event(
        &mut self,
        evt: usize,
        payload: isize,
        cfg: &mut WindowKitConfig,
        config_dir: &Path,
        gfx: Option<&SharedGraphics>,
        trace_id: &str,
    ) -> EditorCloseOutcome {
        let Some(sess) = self.session.as_mut() else {
            return EditorCloseOutcome::StillOpen;
        };

        let mut need_render = false;
        let mut finish_action: Option<bool> = None; // Some(true)=保存, Some(false)=取消

        match evt {
            ED_EVT_MOUSEMOVE => {
                EDITOR_MOVE_PENDING.store(0, Ordering::Release);
                let (lx, ly) = unpack_xy(EDITOR_LATEST_PT.load(Ordering::Acquire));
                sess.cursor_local = (lx, ly);
                sess.shift_down = is_vk_down(VK_SHIFT.0);
                sess.alt_down = is_vk_down(VK_MENU.0);
                need_render = sess.on_mouse_move(lx, ly);
            }
            ED_EVT_LBUTTONDOWN => {
                let (lx, ly) = unpack_lparam_xy(payload);
                sess.cursor_local = (lx, ly);
                sess.shift_down = is_vk_down(VK_SHIFT.0);
                sess.alt_down = is_vk_down(VK_MENU.0);
                match sess.on_lbutton_down(lx, ly) {
                    ClickOutcome::None => {}
                    ClickOutcome::Redraw => need_render = true,
                    ClickOutcome::Save => finish_action = Some(true),
                    ClickOutcome::Cancel => finish_action = Some(false),
                }
            }
            ED_EVT_LBUTTONUP => {
                let (lx, ly) = unpack_lparam_xy(payload);
                sess.cursor_local = (lx, ly);
                need_render = sess.on_lbutton_up(lx, ly);
            }
            ED_EVT_LBUTTONDBLCLK => {
                let (lx, ly) = unpack_lparam_xy(payload);
                sess.cursor_local = (lx, ly);
                need_render = sess.on_lbutton_dblclk(lx, ly);
            }
            ED_EVT_RBUTTONDOWN => {
                if sess.drag.is_some() {
                    sess.drag = None;
                    sess.active_guides.clear();
                    unsafe {
                        let _ = ReleaseCapture();
                    }
                    need_render = true;
                }
            }
            ED_EVT_KEYDOWN => {
                let vk = payload as u16;
                sess.shift_down = is_vk_down(VK_SHIFT.0);
                sess.alt_down = is_vk_down(VK_MENU.0);
                match sess.on_key_down(vk) {
                    ClickOutcome::None => {}
                    ClickOutcome::Redraw => need_render = true,
                    ClickOutcome::Save => finish_action = Some(true),
                    ClickOutcome::Cancel => finish_action = Some(false),
                }
            }
            ED_EVT_KEYUP => {
                let prev_shift = sess.shift_down;
                let prev_alt = sess.alt_down;
                sess.shift_down = is_vk_down(VK_SHIFT.0);
                sess.alt_down = is_vk_down(VK_MENU.0);
                if prev_shift != sess.shift_down || prev_alt != sess.alt_down {
                    need_render = true;
                }
            }
            ED_EVT_CAPTURE_LOST => {
                if sess.drag.is_some() {
                    sess.drag = None;
                    sess.active_guides.clear();
                    need_render = true;
                }
            }
            ED_EVT_CLOSE => {
                finish_action = Some(false);
            }
            _ => {}
        }

        if let Some(save) = finish_action {
            return self.finish_and_close(save, cfg, config_dir, trace_id);
        }

        if need_render {
            if let (Some(sess), Some(g)) = (self.session.as_mut(), gfx) {
                sess.render(g);
            }
        }
        EditorCloseOutcome::StillOpen
    }

    pub fn close_discard(&mut self, trace_id: &str) {
        if let Some(sess) = self.session.take() {
            destroy_editor_session(sess, false, false, trace_id);
        }
    }

    fn finish_and_close(
        &mut self,
        save: bool,
        cfg: &mut WindowKitConfig,
        config_dir: &Path,
        trace_id: &str,
    ) -> EditorCloseOutcome {
        let Some(sess) = self.session.take() else {
            return EditorCloseOutcome::StillOpen;
        };
        let return_to_hub = sess.return_to_hub;
        let mut actually_saved = false;
        if save {
            let (out_id, out_name, out_builtin) = if sess.source_built_in && sess.dirty {
                let ts = SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .map(|d| d.as_millis())
                    .unwrap_or(0);
                let suffix = if sess.zh_ui { "自定义" } else { "Custom" };
                (
                    format!("custom-{ts}"),
                    format!("{} ({suffix})", sess.source_layout_name),
                    false,
                )
            } else {
                (
                    sess.source_layout_id.clone(),
                    sess.source_layout_name.clone(),
                    sess.source_built_in,
                )
            };

            let mut final_layout = sess.state.to_zone_layout(out_id, out_name, out_builtin);
            final_layout.show_spacing = sess.source_show_spacing || sess.state.zone_gap > 0;
            final_layout.highlight_distance = sess.source_highlight_distance;
            final_layout.hotkey_digit = sess.source_hotkey_digit;

            // 即刻同步进运行期内存配置，回 Hub 时 0 延迟立即可见并预览！
            if let Some(existing) = cfg
                .snap_zones
                .layouts
                .iter_mut()
                .find(|l| l.id == final_layout.id)
            {
                *existing = final_layout.clone();
            } else {
                cfg.snap_zones.layouts.push(final_layout.clone());
            }
            let bind_desktop = if sess.per_virtual_desktop {
                sess.desktop_id.as_deref().unwrap_or("")
            } else {
                ""
            };
            cfg.snap_zones
                .bind_monitor_layout(&sess.monitor.id, bind_desktop, &final_layout.id);

            let mut root = JsonValue::object();
            root.insert("saved", JsonValue::Bool(true));
            root.insert("monitorId", JsonValue::String(sess.monitor.id.clone()));
            root.insert(
                "desktopId",
                JsonValue::String(sess.desktop_id.clone().unwrap_or_default()),
            );
            root.insert(
                "perVirtualDesktop",
                JsonValue::Bool(sess.per_virtual_desktop),
            );
            root.insert("zoneGap", JsonValue::from_i32(sess.state.zone_gap));
            root.insert("layout", final_layout.to_json());

            let result_path = config_dir.join(FILE_LAYOUT_EDITOR_RESULT);
            match write_text_file_atomic(&result_path, &root.to_string_pretty()) {
                Ok(()) => {
                    actually_saved = true;
                    wk_info!(
                        trace_id,
                        "layoutEditor",
                        "saved layout id='{}' mode={:?} monitor='{}' desktop={:?} gap={}px -> {}",
                        final_layout.id,
                        final_layout.layout_type,
                        sess.monitor.id,
                        sess.desktop_id,
                        sess.state.zone_gap,
                        result_path.display()
                    );
                }
                Err(e) => {
                    wk_warn!(
                        trace_id,
                        "layoutEditor",
                        "failed to write editor result: {}",
                        e
                    );
                }
            }
        } else {
            wk_debug!(trace_id, "layoutEditor", "closed editor without saving");
        }
        destroy_editor_session(sess, actually_saved, !return_to_hub, trace_id);
        EditorCloseOutcome::Closed {
            saved: actually_saved,
            return_to_hub,
        }
    }
}

impl Drop for LayoutEditorManager {
    fn drop(&mut self) {
        self.close_discard("drop");
    }
}

fn destroy_editor_session(
    sess: EditorSession,
    saved: bool,
    allow_restore_settings: bool,
    trace_id: &str,
) {
    let hwnd = sess.hwnd;
    let restore_flag = if allow_restore_settings && sess.restore_settings_hwnd.is_some() {
        1isize
    } else {
        0isize
    };
    // 先释放 D2D Surface（Drop），再销毁窗口
    drop(sess);
    unsafe {
        let _ = ReleaseCapture();
        if IsWindow(hwnd).as_bool() {
            let _ = ShowWindow(hwnd, SW_HIDE);
            let _ = DestroyWindow(hwnd);
        }
    }
    EDITOR_CURSOR_KIND.store(CURSOR_ARROW, Ordering::Release);

    // 异步零阻塞通知 C++ 宿主读取保存结果并恢复设置窗口
    let cls = to_wide(HOST_MESSAGE_WINDOW_CLASS);
    if let Ok(host_hwnd) = unsafe { FindWindowW(PCWSTR(cls.as_ptr()), PCWSTR::null()) } {
        if !host_hwnd.0.is_null() {
            let msg_id = host_result_message_id();
            if msg_id != 0 {
                unsafe {
                    let _ = PostMessageW(
                        host_hwnd,
                        msg_id,
                        WPARAM(if saved { 1 } else { 0 }),
                        LPARAM(restore_flag),
                    );
                }
                wk_debug!(
                    trace_id,
                    "layoutEditor",
                    "notified host message window saved={} restoreSettings={}",
                    saved,
                    restore_flag
                );
            }
        }
    }
}

enum ClickOutcome {
    None,
    Redraw,
    Save,
    Cancel,
}

impl EditorSession {
    fn scale(&self) -> f32 {
        (self.monitor.dpi as f32 / 96.0).max(1.0)
    }

    fn work_w(&self) -> i32 {
        self.monitor.work_rect.width().max(1)
    }

    fn work_h(&self) -> i32 {
        self.monitor.work_rect.height().max(1)
    }

    fn push_undo(&mut self) {
        self.undo_stack.push(self.state.clone());
        if self.undo_stack.len() > MAX_UNDO_STEPS {
            self.undo_stack.remove(0);
        }
        self.dirty = true;
    }

    fn show_toast(&mut self, msg: String) {
        self.status_toast = Some((msg, Instant::now()));
    }

    fn resolved_zones(&self) -> Vec<ResolvedZone> {
        self.state.resolve(self.monitor.work_rect)
    }

    fn local_rect(&self, screen_rect: RectI) -> RectI {
        let work = self.monitor.work_rect;
        RectI::new(
            screen_rect.left - work.left,
            screen_rect.top - work.top,
            screen_rect.right - work.left,
            screen_rect.bottom - work.top,
        )
    }

    /// 重建顶部可拖动玻璃悬浮控制舱按钮精确矩形
    fn rebuild_dock_geometry(&mut self, center_horizontally: bool) {
        let s = self.scale();
        let pad_x = (10.0 * s).round() as i32;
        let gap = (6.0 * s).round() as i32;
        let btn_h = (32.0 * s).round() as i32;
        let dock_h = (48.0 * s).round() as i32;
        let btn_top_off = (dock_h - btn_h) / 2;

        let zh = self.zh_ui;
        let is_grid = self.state.mode == ZoneLayoutType::Grid;

        let mut specs: Vec<(DockButtonId, String, i32, bool, bool, bool, bool)> = Vec::new();
        // (id, label, width_px, active, primary, accent_outline, disabled)
        specs.push((DockButtonId::DragGrip, String::new(), (22.0 * s).round() as i32, false, false, false, false));
        specs.push((
            DockButtonId::ModeGrid,
            if zh { "网格" } else { "Grid" }.to_string(),
            (58.0 * s).round() as i32,
            is_grid,
            false,
            false,
            false,
        ));
        specs.push((
            DockButtonId::ModeCanvas,
            if zh { "自由画布" } else { "Canvas" }.to_string(),
            (76.0 * s).round() as i32,
            !is_grid,
            false,
            false,
            false,
        ));
        specs.push((
            DockButtonId::AddOrPreset,
            if is_grid {
                if zh { "换模板" } else { "Preset" }
            } else if zh {
                "+ 区域"
            } else {
                "+ Zone"
            }
            .to_string(),
            (70.0 * s).round() as i32,
            false,
            false,
            false,
            false,
        ));
        specs.push((
            DockButtonId::CaptureWindows,
            if zh { "捕获窗口 (C)" } else { "Capture Wins (C)" }.to_string(),
            (if zh { 104.0 } else { 122.0 } * s).round() as i32,
            false,
            false,
            true,
            false,
        ));
        specs.push((
            DockButtonId::GapMinus,
            "-".to_string(),
            (28.0 * s).round() as i32,
            false,
            false,
            false,
            self.state.zone_gap <= 0,
        ));
        specs.push((
            DockButtonId::GapPlus,
            "+".to_string(),
            (28.0 * s).round() as i32,
            false,
            false,
            false,
            self.state.zone_gap >= 48,
        ));
        specs.push((
            DockButtonId::Undo,
            if zh {
                format!("撤销 ({})", self.undo_stack.len())
            } else {
                format!("Undo ({})", self.undo_stack.len())
            },
            (74.0 * s).round() as i32,
            false,
            false,
            false,
            self.undo_stack.is_empty(),
        ));
        specs.push((
            DockButtonId::Cancel,
            if zh { "取消" } else { "Cancel" }.to_string(),
            (60.0 * s).round() as i32,
            false,
            false,
            false,
            false,
        ));
        specs.push((
            DockButtonId::Save,
            if zh { "保存生效" } else { "Save" }.to_string(),
            (if zh { 88.0 } else { 72.0 } * s).round() as i32,
            false,
            true,
            false,
            false,
        ));

        let gap_label_w = (46.0 * s).round() as i32;
        let sep_w = (10.0 * s).round() as i32;
        let mut total_w = pad_x * 2;
        for (i, sp) in specs.iter().enumerate() {
            total_w += sp.2;
            if sp.0 == DockButtonId::GapMinus {
                total_w += gap_label_w;
            }
            if matches!(
                sp.0,
                DockButtonId::DragGrip | DockButtonId::ModeCanvas | DockButtonId::Undo
            ) {
                total_w += sep_w;
            }
            if i + 1 < specs.len() {
                total_w += gap;
            }
        }

        let w = self.work_w();
        let h = self.work_h();
        if center_horizontally {
            self.dock_x = ((w - total_w) / 2).max(12);
        }
        self.dock_x = self.dock_x.clamp(12, (w - total_w - 12).max(12));
        self.dock_y = self.dock_y.clamp(12, (h - dock_h - 48).max(12));
        self.dock_rect = RectI::from_xywh(self.dock_x, self.dock_y, total_w, dock_h);

        let mut cursor_x = self.dock_x + pad_x;
        self.dock_buttons.clear();
        for (id, label, bw, active, primary, accent_outline, disabled) in specs {
            let rect = RectI::from_xywh(cursor_x, self.dock_y + btn_top_off, bw, btn_h);
            self.dock_buttons.push(DockButtonRect {
                id,
                rect,
                label,
                active,
                primary,
                accent_outline,
                disabled,
            });
            cursor_x += bw + gap;
            if id == DockButtonId::GapMinus {
                cursor_x += gap_label_w;
            }
            if matches!(
                id,
                DockButtonId::DragGrip | DockButtonId::ModeCanvas | DockButtonId::Undo
            ) {
                cursor_x += sep_w;
            }
        }
    }

    fn hit_test_dock_button(&self, lx: i32, ly: i32) -> Option<DockButtonId> {
        if !self.dock_rect.contains_point(lx, ly) {
            return None;
        }
        for btn in &self.dock_buttons {
            if btn.rect.contains_point(lx, ly) {
                return Some(btn.id);
            }
        }
        // 点击控制舱空白处也可拖动控制舱
        Some(DockButtonId::DragGrip)
    }

    /// 枚举当前 Grid 布局中所有真实可见的内部分割线（跨合并区的消除边不会返回）
    fn compute_grid_dividers(&self) -> Vec<GridDividerHit> {
        if self.state.mode != ZoneLayoutType::Grid {
            return Vec::new();
        }
        let work = self.monitor.work_rect;
        let w = work.width().max(1) as f64;
        let h = work.height().max(1) as f64;
        let mut col_px = vec![0i32; self.state.columns + 1];
        let mut acc = 0.0f64;
        for c in 0..self.state.columns {
            acc += self.state.col_weights[c];
            col_px[c + 1] = ((acc / 100.0) * w).round() as i32;
        }
        let mut row_px = vec![0i32; self.state.rows + 1];
        let mut acc_r = 0.0f64;
        for r in 0..self.state.rows {
            acc_r += self.state.row_weights[r];
            row_px[r + 1] = ((acc_r / 100.0) * h).round() as i32;
        }

        let mut out = Vec::new();
        // 纵向分割线（列之间）
        for c in 0..self.state.columns.saturating_sub(1) {
            let mut r = 0usize;
            while r < self.state.rows {
                if self.state.cell_map[r][c] != self.state.cell_map[r][c + 1] {
                    let start_r = r;
                    while r < self.state.rows
                        && self.state.cell_map[r][c] != self.state.cell_map[r][c + 1]
                    {
                        r += 1;
                    }
                    out.push(GridDividerHit {
                        is_vertical: true,
                        track_index: c,
                        local_px: col_px[c + 1],
                        span_start_px: row_px[start_r],
                        span_end_px: row_px[r],
                    });
                } else {
                    r += 1;
                }
            }
        }
        // 横向分割线（行之间）
        for r in 0..self.state.rows.saturating_sub(1) {
            let mut c = 0usize;
            while c < self.state.columns {
                if self.state.cell_map[r][c] != self.state.cell_map[r + 1][c] {
                    let start_c = c;
                    while c < self.state.columns
                        && self.state.cell_map[r][c] != self.state.cell_map[r + 1][c]
                    {
                        c += 1;
                    }
                    out.push(GridDividerHit {
                        is_vertical: false,
                        track_index: r,
                        local_px: row_px[r + 1],
                        span_start_px: col_px[start_c],
                        span_end_px: col_px[c],
                    });
                } else {
                    c += 1;
                }
            }
        }
        out
    }

    fn hit_test_grid_divider(&self, lx: i32, ly: i32) -> Option<GridDividerHit> {
        let hit_r = ((10.0 * self.scale()).round() as i32).max(self.state.zone_gap / 2 + 6);
        for d in self.compute_grid_dividers() {
            if d.is_vertical {
                if (lx - d.local_px).abs() <= hit_r && ly >= d.span_start_px && ly <= d.span_end_px {
                    return Some(d);
                }
            } else if (ly - d.local_px).abs() <= hit_r && lx >= d.span_start_px && lx <= d.span_end_px {
                return Some(d);
            }
        }
        None
    }

    fn hit_test_canvas_handle(&self, lx: i32, ly: i32) -> Option<(usize, CanvasHandle)> {
        if self.state.mode != ZoneLayoutType::Canvas {
            return None;
        }
        let zones = self.resolved_zones();
        let edge_r = (11.0 * self.scale()).round() as i32;
        let corner_r = (16.0 * self.scale()).round() as i32;

        // 优先检测当前选中区，其次按从上到下逆序（最上层优先）检测
        let mut order: Vec<usize> = (0..zones.len()).rev().collect();
        if let Some(sel) = self.selected_zone {
            if let Some(pos) = order.iter().position(|&i| i == sel) {
                order.remove(pos);
                order.insert(0, sel);
            }
        }

        for idx in order {
            let lr = self.local_rect(zones[idx].rect);
            if lx < lr.left - edge_r
                || lx > lr.right + edge_r
                || ly < lr.top - edge_r
                || ly > lr.bottom + edge_r
            {
                continue;
            }
            let near_l = (lx - lr.left).abs() <= corner_r;
            let near_r = (lx - lr.right).abs() <= corner_r;
            let near_t = (ly - lr.top).abs() <= corner_r;
            let near_b = (ly - lr.bottom).abs() <= corner_r;

            if near_l && near_t {
                return Some((idx, CanvasHandle::NW));
            }
            if near_r && near_t {
                return Some((idx, CanvasHandle::NE));
            }
            if near_l && near_b {
                return Some((idx, CanvasHandle::SW));
            }
            if near_r && near_b {
                return Some((idx, CanvasHandle::SE));
            }
            if (lx - lr.left).abs() <= edge_r && ly >= lr.top && ly <= lr.bottom {
                return Some((idx, CanvasHandle::W));
            }
            if (lx - lr.right).abs() <= edge_r && ly >= lr.top && ly <= lr.bottom {
                return Some((idx, CanvasHandle::E));
            }
            if (ly - lr.top).abs() <= edge_r && lx >= lr.left && lx <= lr.right {
                return Some((idx, CanvasHandle::N));
            }
            if (ly - lr.bottom).abs() <= edge_r && lx >= lr.left && lx <= lr.right {
                return Some((idx, CanvasHandle::S));
            }
            if lr.contains_point(lx, ly) {
                return Some((idx, CanvasHandle::Move));
            }
        }
        None
    }

    fn hit_test_zone_at(&self, lx: i32, ly: i32) -> Option<usize> {
        let zones = self.resolved_zones();
        let mut matches: Vec<&ResolvedZone> = zones
            .iter()
            .filter(|z| self.local_rect(z.rect).contains_point(lx, ly))
            .collect();
        matches.sort_by_key(|z| z.rect.area());
        matches.first().map(|z| z.index)
    }

    fn compute_slicer_for_zone(
        &self,
        zone_local: RectI,
        cursor_local: (i32, i32),
        horizontal: bool,
    ) -> GridSlicerPreview {
        let thr = (GRID_SLICER_SNAP_THRESHOLD_DIP * self.scale()).round() as i32;
        compute_grid_slicer_preview(
            zone_local,
            self.monitor.work_rect,
            cursor_local,
            horizontal,
            self.alt_down,
            &self.real_windows,
            thr,
            self.zh_ui,
        )
    }

    fn set_cursor_kind(&self, kind: u32) {
        let prev = EDITOR_CURSOR_KIND.swap(kind, Ordering::AcqRel);
        if prev != kind {
            unsafe {
                let _ = SetCursor(cursor_handle_for_kind(kind));
            }
        }
    }

    fn on_mouse_move(&mut self, lx: i32, ly: i32) -> bool {
        if let Some(drag) = self.drag.clone() {
            match drag {
                ActiveDrag::DockMove { offset_x, offset_y } => {
                    self.dock_x = lx - offset_x;
                    self.dock_y = ly - offset_y;
                    self.rebuild_dock_geometry(false);
                    self.set_cursor_kind(CURSOR_SIZEALL);
                    return true;
                }
                ActiveDrag::GridDivider {
                    is_vertical,
                    track_index,
                } => {
                    let threshold = (SNAP_THRESHOLD_DIP * self.scale()).round() as i32;
                    let raw = if is_vertical { lx } else { ly };
                    let (snapped_px, guide) = if self.alt_down {
                        (raw, None)
                    } else {
                        let (xs, ys) = build_snap_targets(
                            self.monitor.work_rect,
                            &[],
                            &self.real_windows,
                            self.state.zone_gap,
                            self.zh_ui,
                        );
                        snap_coord_to_targets(
                            raw,
                            if is_vertical { &xs } else { &ys },
                            threshold,
                            is_vertical,
                        )
                    };
                    self.active_guides = guide.into_iter().collect();
                    let denom = if is_vertical {
                        self.work_w() as f64
                    } else {
                        self.work_h() as f64
                    };
                    let frac = (snapped_px as f64 / denom).clamp(0.02, 0.98);
                    let _ = self
                        .state
                        .move_grid_divider(is_vertical, track_index, frac);
                    self.dirty = true;
                    self.set_cursor_kind(if is_vertical {
                        CURSOR_SIZEWE
                    } else {
                        CURSOR_SIZENS
                    });
                    return true;
                }
                ActiveDrag::GridMarquee {
                    start_x,
                    start_y,
                    start_zone,
                    ..
                } => {
                    self.drag = Some(ActiveDrag::GridMarquee {
                        start_x,
                        start_y,
                        cur_x: lx,
                        cur_y: ly,
                        start_zone,
                    });
                    return true;
                }
                ActiveDrag::CanvasTransform {
                    zone_idx,
                    handle,
                    start_x,
                    start_y,
                    orig_zone,
                    pushed_undo,
                } => {
                    if zone_idx >= self.state.canvas_zones.len() {
                        return false;
                    }
                    if !pushed_undo && ((lx - start_x).abs() >= 2 || (ly - start_y).abs() >= 2) {
                        self.push_undo();
                        if let Some(ActiveDrag::CanvasTransform { pushed_undo: pu, .. }) =
                            self.drag.as_mut()
                        {
                            *pu = true;
                        }
                    }
                    self.apply_canvas_transform(zone_idx, handle, start_x, start_y, lx, ly, &orig_zone);
                    return true;
                }
            }
        }

        // 悬停态光标与高亮更新
        let prev_btn = self.hovered_dock_btn;
        let prev_div = self.hovered_divider;
        let prev_zone = self.hovered_zone;

        if let Some(btn) = self.hit_test_dock_button(lx, ly) {
            self.hovered_dock_btn = Some(btn);
            self.hovered_divider = None;
            self.hovered_zone = None;
            self.set_cursor_kind(if btn == DockButtonId::DragGrip {
                CURSOR_SIZEALL
            } else {
                CURSOR_HAND
            });
            return prev_btn != self.hovered_dock_btn || prev_div.is_some() || prev_zone.is_some();
        }
        self.hovered_dock_btn = None;

        if self.state.mode == ZoneLayoutType::Grid {
            if let Some(div) = self.hit_test_grid_divider(lx, ly) {
                self.hovered_divider = Some(div);
                self.hovered_zone = None;
                self.set_cursor_kind(if div.is_vertical {
                    CURSOR_SIZEWE
                } else {
                    CURSOR_SIZENS
                });
                return prev_div != self.hovered_divider || prev_zone.is_some() || prev_btn.is_some();
            }
            self.hovered_divider = None;
            self.hovered_zone = self.hit_test_zone_at(lx, ly);
            self.set_cursor_kind(if self.hovered_zone.is_some() {
                CURSOR_HAND
            } else {
                CURSOR_ARROW
            });
            // Grid 模式下悬停在分区内部时，实时跟着光标绘制切分引导虚线，所以需要重绘
            return self.hovered_zone.is_some() || prev_zone.is_some() || prev_div.is_some() || prev_btn.is_some();
        } else {
            self.hovered_divider = None;
            if let Some((idx, handle)) = self.hit_test_canvas_handle(lx, ly) {
                self.hovered_zone = Some(idx);
                let cur = match handle {
                    CanvasHandle::Move => CURSOR_SIZEALL,
                    CanvasHandle::N | CanvasHandle::S => CURSOR_SIZENS,
                    CanvasHandle::E | CanvasHandle::W => CURSOR_SIZEWE,
                    CanvasHandle::NW | CanvasHandle::SE => CURSOR_SIZENWSE,
                    CanvasHandle::NE | CanvasHandle::SW => CURSOR_SIZENESW,
                };
                self.set_cursor_kind(cur);
            } else {
                self.hovered_zone = None;
                self.set_cursor_kind(CURSOR_ARROW);
            }
            return prev_zone != self.hovered_zone || prev_btn.is_some();
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn apply_canvas_transform(
        &mut self,
        zone_idx: usize,
        handle: CanvasHandle,
        start_x: i32,
        start_y: i32,
        lx: i32,
        ly: i32,
        orig: &NormalizedZone,
    ) {
        let w = self.work_w();
        let h = self.work_h();
        let dx = lx - start_x;
        let dy = ly - start_y;

        let orig_l = (orig.x * w as f64).round() as i32;
        let orig_t = (orig.y * h as f64).round() as i32;
        let orig_r = ((orig.x + orig.width) * w as f64).round() as i32;
        let orig_b = ((orig.y + orig.height) * h as f64).round() as i32;
        let min_w_px = ((MIN_CANVAS_W * w as f64).round() as i32).max(110);
        let min_h_px = ((MIN_CANVAS_H * h as f64).round() as i32).max(76);

        let all_resolved = self.resolved_zones();
        let others: Vec<ResolvedZone> = all_resolved
            .into_iter()
            .filter(|z| z.index != zone_idx)
            .collect();
        let (xs, ys) = if self.alt_down {
            (Vec::new(), Vec::new())
        } else {
            build_snap_targets(
                self.monitor.work_rect,
                &others,
                &self.real_windows,
                self.state.zone_gap,
                self.zh_ui,
            )
        };
        let thr = (SNAP_THRESHOLD_DIP * self.scale()).round() as i32;

        let mut guides = Vec::new();
        let (mut nl, mut nt, mut nr, mut nb) = (orig_l, orig_t, orig_r, orig_b);

        match handle {
            CanvasHandle::Move => {
                let bw = orig_r - orig_l;
                let bh = orig_b - orig_t;
                let mut raw_l = (orig_l + dx).clamp(0, (w - bw).max(0));
                let mut raw_t = (orig_t + dy).clamp(0, (h - bh).max(0));
                if !self.alt_down {
                    // 优先尝试左边缘磁吸，否则右边缘磁吸
                    let (sl, gl) = snap_coord_to_targets(raw_l, &xs, thr, true);
                    if let Some(g) = gl {
                        raw_l = sl.clamp(0, (w - bw).max(0));
                        guides.push(g);
                    } else {
                        let (sr, gr) = snap_coord_to_targets(raw_l + bw, &xs, thr, true);
                        if let Some(g) = gr {
                            raw_l = (sr - bw).clamp(0, (w - bw).max(0));
                            guides.push(g);
                        }
                    }
                    // 优先尝试上边缘磁吸，否则下边缘磁吸
                    let (st, gt) = snap_coord_to_targets(raw_t, &ys, thr, false);
                    if let Some(g) = gt {
                        raw_t = st.clamp(0, (h - bh).max(0));
                        guides.push(g);
                    } else {
                        let (sb, gb) = snap_coord_to_targets(raw_t + bh, &ys, thr, false);
                        if let Some(g) = gb {
                            raw_t = (sb - bh).clamp(0, (h - bh).max(0));
                            guides.push(g);
                        }
                    }
                }
                nl = raw_l;
                nt = raw_t;
                nr = raw_l + bw;
                nb = raw_t + bh;
            }
            _ => {
                let adj_l = matches!(handle, CanvasHandle::W | CanvasHandle::NW | CanvasHandle::SW);
                let adj_r = matches!(handle, CanvasHandle::E | CanvasHandle::NE | CanvasHandle::SE);
                let adj_t = matches!(handle, CanvasHandle::N | CanvasHandle::NW | CanvasHandle::NE);
                let adj_b = matches!(handle, CanvasHandle::S | CanvasHandle::SW | CanvasHandle::SE);

                if adj_l {
                    let raw = (orig_l + dx).clamp(0, orig_r - min_w_px);
                    let (sv, g) = if self.alt_down {
                        (raw, None)
                    } else {
                        snap_coord_to_targets(raw, &xs, thr, true)
                    };
                    nl = sv.clamp(0, orig_r - min_w_px);
                    if let Some(gg) = g {
                        guides.push(gg);
                    }
                }
                if adj_r {
                    let raw = (orig_r + dx).clamp(orig_l + min_w_px, w);
                    let (sv, g) = if self.alt_down {
                        (raw, None)
                    } else {
                        snap_coord_to_targets(raw, &xs, thr, true)
                    };
                    nr = sv.clamp(orig_l + min_w_px, w);
                    if let Some(gg) = g {
                        guides.push(gg);
                    }
                }
                if adj_t {
                    let raw = (orig_t + dy).clamp(0, orig_b - min_h_px);
                    let (sv, g) = if self.alt_down {
                        (raw, None)
                    } else {
                        snap_coord_to_targets(raw, &ys, thr, false)
                    };
                    nt = sv.clamp(0, orig_b - min_h_px);
                    if let Some(gg) = g {
                        guides.push(gg);
                    }
                }
                if adj_b {
                    let raw = (orig_b + dy).clamp(orig_t + min_h_px, h);
                    let (sv, g) = if self.alt_down {
                        (raw, None)
                    } else {
                        snap_coord_to_targets(raw, &ys, thr, false)
                    };
                    nb = sv.clamp(orig_t + min_h_px, h);
                    if let Some(gg) = g {
                        guides.push(gg);
                    }
                }
            }
        }

        self.active_guides = guides;
        let updated = clamp_canvas_zone(NormalizedZone {
            index: zone_idx,
            x: nl as f64 / w as f64,
            y: nt as f64 / h as f64,
            width: (nr - nl) as f64 / w as f64,
            height: (nb - nt) as f64 / h as f64,
        });
        self.state.canvas_zones[zone_idx] = updated;
        self.dirty = true;
    }

    fn on_lbutton_down(&mut self, lx: i32, ly: i32) -> ClickOutcome {
        // 1. 优先响应悬浮控制舱点击
        if let Some(btn) = self.hit_test_dock_button(lx, ly) {
            return self.handle_dock_click(btn, lx, ly);
        }

        // 2. Grid 模式：拖分割线或起手框选/单击切分
        if self.state.mode == ZoneLayoutType::Grid {
            if let Some(div) = self.hit_test_grid_divider(lx, ly) {
                self.push_undo();
                self.drag = Some(ActiveDrag::GridDivider {
                    is_vertical: div.is_vertical,
                    track_index: div.track_index,
                });
                unsafe {
                    let _ = SetCapture(self.hwnd);
                }
                return ClickOutcome::Redraw;
            }
            let hit_z = self.hit_test_zone_at(lx, ly);
            self.selected_zone = None;
            self.drag = Some(ActiveDrag::GridMarquee {
                start_x: lx,
                start_y: ly,
                cur_x: lx,
                cur_y: ly,
                start_zone: hit_z,
            });
            unsafe {
                let _ = SetCapture(self.hwnd);
            }
            return ClickOutcome::Redraw;
        }

        // 3. Canvas 模式：先检查右上角圆形 x 快捷删除按钮，再检测 8 向缩放边/角或区内部移动
        if self.state.canvas_zones.len() > 1 {
            let zones = self.resolved_zones();
            let s = self.scale();
            let btn_sz = (24.0 * s).round() as i32;
            let pad = (8.0 * s).round() as i32;
            for z in zones.iter().rev() {
                let lr = self.local_rect(z.rect);
                if lr.width() >= btn_sz * 2 + pad && lr.height() >= btn_sz * 2 + pad {
                    let close_rc = RectI::from_xywh(lr.right - pad - btn_sz, lr.top + pad, btn_sz, btn_sz);
                    if close_rc.contains_point(lx, ly) {
                        self.selected_zone = Some(z.index);
                        let _ = self.delete_selected_zone();
                        return ClickOutcome::Redraw;
                    }
                }
            }
        }
        if let Some((idx, handle)) = self.hit_test_canvas_handle(lx, ly) {
            self.selected_zone = Some(idx);
            let orig_zone = self.state.canvas_zones[idx].clone();
            self.drag = Some(ActiveDrag::CanvasTransform {
                zone_idx: idx,
                handle,
                start_x: lx,
                start_y: ly,
                orig_zone,
                pushed_undo: false,
            });
            unsafe {
                let _ = SetCapture(self.hwnd);
            }
            return ClickOutcome::Redraw;
        }

        self.selected_zone = None;
        ClickOutcome::Redraw
    }

    fn on_lbutton_up(&mut self, lx: i32, ly: i32) -> bool {
        let Some(drag) = self.drag.take() else {
            return false;
        };
        unsafe {
            let _ = ReleaseCapture();
        }
        self.active_guides.clear();

        match drag {
            ActiveDrag::GridMarquee {
                start_x,
                start_y,
                start_zone,
                ..
            } => {
                let moved = (lx - start_x).abs().max((ly - start_y).abs());
                if moved >= 14 {
                    // 跨多区框选 -> 合并
                    let sel_rect = RectI::new(
                        start_x.min(lx),
                        start_y.min(ly),
                        start_x.max(lx),
                        start_y.max(ly),
                    );
                    let zones = self.resolved_zones();
                    let hit_ids: Vec<usize> = zones
                        .iter()
                        .filter(|z| self.local_rect(z.rect).intersect(&sel_rect).is_some())
                        .map(|z| z.index)
                        .collect();
                    if hit_ids.len() >= 2 {
                        self.push_undo();
                        if self.state.merge_grid_zones(&hit_ids) {
                            let msg = if self.zh_ui {
                                format!("已合并 {} 个相邻区域", hit_ids.len())
                            } else {
                                format!("Merged {} zones", hit_ids.len())
                            };
                            self.show_toast(msg);
                        }
                    }
                } else if let Some(zid) = start_zone {
                    // 单击格子内部 -> 使用与光刀预览完全一致的微磁吸坐标落刀切分（所见即所得！）
                    let horiz = self.shift_down;
                    let zones = self.resolved_zones();
                    let frac = if let Some(z) = zones.iter().find(|item| item.index == zid) {
                        let lr = self.local_rect(z.rect);
                        let slicer = self.compute_slicer_for_zone(lr, (lx, ly), horiz);
                        self.state.zone_split_abs_fraction(zid, horiz, &slicer)
                    } else if horiz {
                        (ly as f64 / self.work_h() as f64).clamp(0.05, 0.95)
                    } else {
                        (lx as f64 / self.work_w() as f64).clamp(0.05, 0.95)
                    };
                    self.push_undo();
                    if !self.state.split_grid_zone_at(zid, horiz, frac) {
                        let _ = self.undo_stack.pop();
                        let msg = if self.zh_ui {
                            "已达最大网格细分上限 (8x8)".to_string()
                        } else {
                            "Maximum grid subdivision reached (8x8)".to_string()
                        };
                        self.show_toast(msg);
                    }
                }
                self.selected_zone = None;
                self.hovered_zone = self.hit_test_zone_at(lx, ly);
                self.rebuild_dock_geometry(false);
                true
            }
            ActiveDrag::GridDivider { .. } | ActiveDrag::CanvasTransform { .. } | ActiveDrag::DockMove { .. } => {
                self.rebuild_dock_geometry(false);
                true
            }
        }
    }

    fn on_lbutton_dblclk(&mut self, lx: i32, ly: i32) -> bool {
        if self.dock_rect.contains_point(lx, ly) {
            return false;
        }
        if self.state.mode == ZoneLayoutType::Canvas && self.hit_test_canvas_handle(lx, ly).is_none() {
            self.add_canvas_zone_at(lx as f64 / self.work_w() as f64, ly as f64 / self.work_h() as f64);
            return true;
        }
        false
    }

    fn on_key_down(&mut self, vk: u16) -> ClickOutcome {
        let ctrl = is_vk_down(VK_CONTROL.0);
        if vk == VK_ESCAPE.0 {
            if self.drag.is_some() {
                self.drag = None;
                self.active_guides.clear();
                unsafe {
                    let _ = ReleaseCapture();
                }
                return ClickOutcome::Redraw;
            }
            return ClickOutcome::Cancel;
        }
        if vk == VK_RETURN.0 || (ctrl && (vk == b'S' as u16)) {
            return ClickOutcome::Save;
        }
        if ctrl && vk == b'Z' as u16 {
            if self.perform_undo() {
                return ClickOutcome::Redraw;
            }
            return ClickOutcome::None;
        }
        if !ctrl && vk == b'C' as u16 {
            self.capture_current_windows();
            return ClickOutcome::Redraw;
        }
        if ctrl && vk == b'D' as u16 && self.state.mode == ZoneLayoutType::Canvas {
            if let Some(sel) = self.selected_zone {
                if let Some(src) = self.state.canvas_zones.get(sel).cloned() {
                    self.push_undo();
                    let next_idx = self.state.canvas_zones.len();
                    let dup = clamp_canvas_zone(NormalizedZone {
                        index: next_idx,
                        x: src.x + 0.03,
                        y: src.y + 0.03,
                        width: src.width,
                        height: src.height,
                    });
                    self.state.canvas_zones.push(dup);
                    self.selected_zone = Some(next_idx);
                    self.rebuild_dock_geometry(false);
                    return ClickOutcome::Redraw;
                }
            }
        }
        if vk == VK_DELETE.0 || vk == VK_BACK.0 {
            if self.delete_selected_zone() {
                return ClickOutcome::Redraw;
            }
            return ClickOutcome::None;
        }
        // Ctrl+Tab 在 Canvas 区域间循环切换焦点（复刻 FancyZones 图 6 快捷键）
        if self.state.mode == ZoneLayoutType::Canvas && ctrl && vk == VK_TAB.0 {
            let total = self.state.canvas_zones.len();
            if total > 0 {
                let next = match self.selected_zone {
                    Some(cur) => {
                        if self.shift_down {
                            (cur + total - 1) % total
                        } else {
                            (cur + 1) % total
                        }
                    }
                    None => 0,
                };
                self.selected_zone = Some(next);
                return ClickOutcome::Redraw;
            }
        }
        // Canvas 模式精准物理像素微调（复刻 FancyZones 图 6）：
        // 方向键 = 移动 10px，Ctrl+方向键 = 移动 1px；
        // Shift+方向键 = 改大小 10px，Ctrl+Shift+方向键 = 改大小 2px。
        if self.state.mode == ZoneLayoutType::Canvas
            && matches!(vk, x if x == VK_LEFT.0 || x == VK_RIGHT.0 || x == VK_UP.0 || x == VK_DOWN.0)
        {
            if let Some(sel) = self.selected_zone {
                if sel < self.state.canvas_zones.len() {
                    let px_step = if self.shift_down {
                        if ctrl { 2.0 } else { 10.0 }
                    } else if ctrl {
                        1.0
                    } else {
                        10.0
                    };
                    let step_x = px_step / self.work_w().max(1) as f64;
                    let step_y = px_step / self.work_h().max(1) as f64;
                    self.push_undo();
                    let z = &mut self.state.canvas_zones[sel];
                    if self.shift_down {
                        if vk == VK_LEFT.0 {
                            z.width -= step_x;
                        } else if vk == VK_RIGHT.0 {
                            z.width += step_x;
                        } else if vk == VK_UP.0 {
                            z.height -= step_y;
                        } else if vk == VK_DOWN.0 {
                            z.height += step_y;
                        }
                    } else if vk == VK_LEFT.0 {
                        z.x -= step_x;
                    } else if vk == VK_RIGHT.0 {
                        z.x += step_x;
                    } else if vk == VK_UP.0 {
                        z.y -= step_y;
                    } else if vk == VK_DOWN.0 {
                        z.y += step_y;
                    }
                    *z = clamp_canvas_zone(z.clone());
                    self.rebuild_dock_geometry(false);
                    return ClickOutcome::Redraw;
                }
            }
        }
        // Shift / Alt 状态变化触发重绘（实时更新横/纵切分引导线）
        if vk == VK_SHIFT.0 || vk == VK_MENU.0 {
            return ClickOutcome::Redraw;
        }
        ClickOutcome::None
    }

    fn handle_dock_click(&mut self, btn: DockButtonId, lx: i32, ly: i32) -> ClickOutcome {
        match btn {
            DockButtonId::DragGrip => {
                self.drag = Some(ActiveDrag::DockMove {
                    offset_x: lx - self.dock_x,
                    offset_y: ly - self.dock_y,
                });
                unsafe {
                    let _ = SetCapture(self.hwnd);
                }
                ClickOutcome::Redraw
            }
            DockButtonId::ModeGrid => {
                if self.state.mode != ZoneLayoutType::Grid {
                    self.push_undo();
                    self.state.mode = ZoneLayoutType::Grid;
                    self.selected_zone = None;
                    self.rebuild_dock_geometry(false);
                    return ClickOutcome::Redraw;
                }
                ClickOutcome::None
            }
            DockButtonId::ModeCanvas => {
                if self.state.mode != ZoneLayoutType::Canvas {
                    self.push_undo();
                    self.state.canvas_zones = grid_snapshot_to_canvas_zones(
                        self.state.rows,
                        self.state.columns,
                        &self.state.row_weights,
                        &self.state.col_weights,
                        &self.state.cell_map,
                    );
                    self.state.mode = ZoneLayoutType::Canvas;
                    self.selected_zone = Some(0);
                    self.rebuild_dock_geometry(false);
                    let msg = if self.zh_ui {
                        "已切换至自由画布模式 · 拖拽边缘可自动磁吸底层真实窗口".to_string()
                    } else {
                        "Canvas mode active · Drag edges to snap to real windows".to_string()
                    };
                    self.show_toast(msg);
                    return ClickOutcome::Redraw;
                }
                ClickOutcome::None
            }
            DockButtonId::AddOrPreset => {
                if self.state.mode == ZoneLayoutType::Canvas {
                    self.add_canvas_zone_at(0.5, 0.5);
                } else {
                    self.cycle_grid_preset();
                }
                ClickOutcome::Redraw
            }
            DockButtonId::CaptureWindows => {
                self.capture_current_windows();
                ClickOutcome::Redraw
            }
            DockButtonId::GapMinus => {
                if self.state.zone_gap > 0 {
                    self.push_undo();
                    self.state.zone_gap = (self.state.zone_gap - 2).max(0);
                    self.rebuild_dock_geometry(false);
                    return ClickOutcome::Redraw;
                }
                ClickOutcome::None
            }
            DockButtonId::GapPlus => {
                if self.state.zone_gap < 48 {
                    self.push_undo();
                    self.state.zone_gap = (self.state.zone_gap + 2).min(48);
                    self.rebuild_dock_geometry(false);
                    return ClickOutcome::Redraw;
                }
                ClickOutcome::None
            }
            DockButtonId::Undo => {
                if self.perform_undo() {
                    ClickOutcome::Redraw
                } else {
                    ClickOutcome::None
                }
            }
            DockButtonId::Cancel => ClickOutcome::Cancel,
            DockButtonId::Save => ClickOutcome::Save,
        }
    }

    fn add_canvas_zone_at(&mut self, center_x: f64, center_y: f64) {
        self.push_undo();
        let w = 0.34;
        let h = 0.42;
        let idx = self.state.canvas_zones.len();
        let offset = (idx % 4) as f64 * 0.025;
        let z = clamp_canvas_zone(NormalizedZone {
            index: idx,
            x: center_x - w * 0.5 + offset,
            y: center_y - h * 0.5 + offset,
            width: w,
            height: h,
        });
        self.state.canvas_zones.push(z);
        self.selected_zone = Some(idx);
        self.rebuild_dock_geometry(false);
    }

    fn cycle_grid_preset(&mut self) {
        self.push_undo();
        let presets: &[(usize, usize, &[f64], &[f64], &[&[usize]], &str, &str)] = &[
            (1, 2, &[100.0], &[50.0, 50.0], &[&[0, 1]], "左右双栏 (50/50)", "2 Columns (50/50)"),
            (
                1,
                3,
                &[100.0],
                &[25.0, 50.0, 25.0],
                &[&[0, 1, 2]],
                "居中聚焦 (25/50/25)",
                "Priority (25/50/25)",
            ),
            (
                2,
                2,
                &[50.0, 50.0],
                &[50.0, 50.0],
                &[&[0, 1], &[2, 3]],
                "2x2 四宫格",
                "2x2 Grid",
            ),
            (
                2,
                2,
                &[50.0, 50.0],
                &[60.0, 40.0],
                &[&[0, 1], &[0, 2]],
                "左侧主屏 + 右侧堆叠",
                "Main + Stack",
            ),
        ];
        let p = &presets[self.preset_cycle % presets.len()];
        self.preset_cycle = self.preset_cycle.wrapping_add(1);
        self.state.rows = p.0;
        self.state.columns = p.1;
        self.state.row_weights = p.2.to_vec();
        self.state.col_weights = p.3.to_vec();
        self.state.cell_map = p.4.iter().map(|r| r.to_vec()).collect();
        self.rebuild_dock_geometry(false);
        self.show_toast(if self.zh_ui {
            format!("已应用模板: {}", p.5)
        } else {
            format!("Applied preset: {}", p.6)
        });
    }

    fn capture_current_windows(&mut self) {
        let captured = capture_zones_from_real_windows(self.monitor.work_rect, &self.real_windows);
        if captured.is_empty() {
            self.show_toast(if self.zh_ui {
                "当前屏幕未检测到可捕获的可见普通窗口".to_string()
            } else {
                "No visible regular windows found on this display".to_string()
            });
            return;
        }
        let count = captured.len();
        self.push_undo();
        self.state.mode = ZoneLayoutType::Canvas;
        self.state.canvas_zones = captured;
        self.selected_zone = Some(0);
        self.rebuild_dock_geometry(false);
        self.show_toast(if self.zh_ui {
            format!("已从 {count} 个桌面真实窗口一键生成布局 · Ctrl+Z 可撤销")
        } else {
            format!("Captured {count} real desktop window(s) into zones · Ctrl+Z to undo")
        });
    }

    fn delete_selected_zone(&mut self) -> bool {
        let target = if self.state.mode == ZoneLayoutType::Grid {
            self.selected_zone.or(self.hovered_zone)
        } else {
            self.selected_zone
        };
        let Some(sel) = target else {
            return false;
        };
        if self.state.mode == ZoneLayoutType::Canvas {
            if self.state.canvas_zones.len() <= 1 || sel >= self.state.canvas_zones.len() {
                self.show_toast(if self.zh_ui {
                    "至少需保留 1 个分屏区域".to_string()
                } else {
                    "At least 1 zone is required".to_string()
                });
                return true;
            }
            self.push_undo();
            self.state.canvas_zones.remove(sel);
            for (i, z) in self.state.canvas_zones.iter_mut().enumerate() {
                z.index = i;
            }
            self.selected_zone = Some(sel.min(self.state.canvas_zones.len() - 1));
            self.rebuild_dock_geometry(false);
            return true;
        }
        self.push_undo();
        if self.state.delete_grid_zone(sel) {
            self.selected_zone = None;
            self.rebuild_dock_geometry(false);
            true
        } else {
            let _ = self.undo_stack.pop();
            false
        }
    }

    fn perform_undo(&mut self) -> bool {
        let Some(prev) = self.undo_stack.pop() else {
            return false;
        };
        self.state = prev;
        self.selected_zone = None;
        self.active_guides.clear();
        self.rebuild_dock_geometry(false);
        self.show_toast(if self.zh_ui {
            "已撤销上一步操作".to_string()
        } else {
            "Undid last change".to_string()
        });
        true
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 5. Direct2D 144Hz 透明全屏渲染管线（纯矢量、零 Emoji、高分屏锐利排版）
    // ─────────────────────────────────────────────────────────────────────────

    fn render(&self, gfx: &SharedGraphics) {
        let work = self.monitor.work_rect;
        let w = self.work_w() as f32;
        let h = self.work_h() as f32;
        let s = self.scale();
        let dark = self.dark_theme;
        let accent = self.accent_rgb;
        let zones = self.resolved_zones();
        let dividers = self.compute_grid_dividers();

        let num_fmt = gfx.ui_text_format(20.0 * s, true, true).ok();
        let px_fmt = gfx.mono_text_format(12.5 * s, false, true).ok();
        let slicer_pct_fmt = gfx.mono_text_format(12.5 * s, true, true).ok();
        let slicer_sub_fmt = gfx.mono_text_format(12.0 * s, false, true).ok();
        let dock_fmt = gfx.ui_text_format(12.5 * s, false, true).ok();
        let dock_bold_fmt = gfx.ui_text_format(12.5 * s, true, true).ok();
        let hint_fmt = gfx.ui_text_format(12.0 * s, false, true).ok();
        let tag_fmt = gfx.mono_text_format(12.0 * s, false, false).ok();

        let dash_props = D2D1_STROKE_STYLE_PROPERTIES {
            startCap: D2D1_CAP_STYLE_ROUND,
            endCap: D2D1_CAP_STYLE_ROUND,
            dashCap: D2D1_CAP_STYLE_ROUND,
            dashStyle: D2D1_DASH_STYLE_DASH,
            ..Default::default()
        };
        let dashed_style: Option<ID2D1StrokeStyle> =
            unsafe { gfx.d2d_factory.CreateStrokeStyle(&dash_props, None).ok() };
        let round_cap_props = D2D1_STROKE_STYLE_PROPERTIES {
            startCap: D2D1_CAP_STYLE_ROUND,
            endCap: D2D1_CAP_STYLE_ROUND,
            ..Default::default()
        };
        let round_cap_style: Option<ID2D1StrokeStyle> =
            unsafe { gfx.d2d_factory.CreateStrokeStyle(&round_cap_props, None).ok() };

        let _ = self.surface.render(self.hwnd, work.left, work.top, |rt| unsafe {
            // 1. 极轻柔全局透视幕布（复刻 FancyZones 图 6/12 通透质感，底层软件窗口保持清晰明亮）
            let scrim = if dark {
                rgba(0.03, 0.05, 0.09, 0.14)
            } else {
                rgba(0.05, 0.08, 0.14, 0.06)
            };
            if let Ok(b_scrim) = rt.CreateSolidColorBrush(&scrim, None) {
                rt.FillRectangle(&rect_f(0.0, 0.0, w, h), &b_scrim);
            }

            // 通用笔刷（100% 对齐原版 PowerToys FancyZones Fluent 极简质感）
            let snap_cyan = rgba(0.02, 0.78, 0.90, 0.96);
            // 主题色提亮态（对齐原版 GridResizer IsMouseOver -> SystemAccentColorLight1Brush）
            let accent_hover = rgba(
                (accent[0] + (1.0 - accent[0]) * 0.18).min(1.0),
                (accent[1] + (1.0 - accent[1]) * 0.18).min(1.0),
                (accent[2] + (1.0 - accent[2]) * 0.18).min(1.0),
                0.98,
            );
            // 已有分割线静息态（与切割线同源的 Fluent 实体色条，静息柔和、激活实心高亮）
            let divider_idle_fill = rgb_a(accent, if dark { 0.24 } else { 0.20 });
            let soft_shadow_col = rgba(0.0, 0.0, 0.0, 0.24);
            let real_win_outline = rgba(0.02, 0.78, 0.90, 0.24);
            let real_win_tag_bg = rgba(0.06, 0.09, 0.14, 0.60);
            let zone_idle_fill = if dark {
                rgba(0.08, 0.11, 0.17, 0.44)
            } else {
                rgba(0.98, 0.985, 0.995, 0.58)
            };
            // Canvas 悬停采用克制薄雾高亮（Grid 模式绝不整块涂蓝）
            let zone_hover_fill = rgb_a(accent, 0.10);
            let zone_sel_fill = rgb_a(accent, 0.22);
            // Grid 模式静息 1px 主题色细腻边框（对齐原版 GridZone.xaml BorderThickness=1 + AccentBrush）
            let zone_grid_idle_stroke = rgb_a(accent, if dark { 0.44 } else { 0.38 });
            let zone_idle_stroke = rgb_a(accent, if dark { 0.72 } else { 0.66 });
            let accent_solid = rgb_a(accent, 0.95);
            let accent_glow = rgb_a(accent, 0.24);
            let card_bg = if dark {
                rgba(0.08, 0.10, 0.15, 0.92)
            } else {
                rgba(0.99, 0.99, 1.0, 0.94)
            };
            let card_border = if dark {
                rgba(1.0, 1.0, 1.0, 0.18)
            } else {
                rgba(0.08, 0.12, 0.20, 0.16)
            };
            let text_main = if dark {
                rgba(0.96, 0.98, 1.0, 0.96)
            } else {
                rgba(0.08, 0.11, 0.18, 0.95)
            };
            let text_muted = if dark {
                rgba(0.75, 0.82, 0.92, 0.88)
            } else {
                rgba(0.28, 0.35, 0.46, 0.88)
            };
            let white = rgba(1.0, 1.0, 1.0, 0.98);

            let (
                Ok(b_cyan),
                Ok(b_accent_hover),
                Ok(b_divider_idle),
                Ok(b_soft_shadow),
                Ok(b_real_outline),
                Ok(b_real_tag_bg),
                Ok(b_idle_fill),
                Ok(b_hover_fill),
                Ok(b_sel_fill),
                Ok(b_grid_idle_stroke),
                Ok(b_idle_stroke),
                Ok(b_accent),
                Ok(b_accent_glow),
                Ok(b_card_bg),
                Ok(b_card_border),
                Ok(b_text),
                Ok(b_muted),
                Ok(b_white),
            ) = (
                rt.CreateSolidColorBrush(&snap_cyan, None),
                rt.CreateSolidColorBrush(&accent_hover, None),
                rt.CreateSolidColorBrush(&divider_idle_fill, None),
                rt.CreateSolidColorBrush(&soft_shadow_col, None),
                rt.CreateSolidColorBrush(&real_win_outline, None),
                rt.CreateSolidColorBrush(&real_win_tag_bg, None),
                rt.CreateSolidColorBrush(&zone_idle_fill, None),
                rt.CreateSolidColorBrush(&zone_hover_fill, None),
                rt.CreateSolidColorBrush(&zone_sel_fill, None),
                rt.CreateSolidColorBrush(&zone_grid_idle_stroke, None),
                rt.CreateSolidColorBrush(&zone_idle_stroke, None),
                rt.CreateSolidColorBrush(&accent_solid, None),
                rt.CreateSolidColorBrush(&accent_glow, None),
                rt.CreateSolidColorBrush(&card_bg, None),
                rt.CreateSolidColorBrush(&card_border, None),
                rt.CreateSolidColorBrush(&text_main, None),
                rt.CreateSolidColorBrush(&text_muted, None),
                rt.CreateSolidColorBrush(&white, None),
            ) else {
                return;
            };

            // 2. 底层真实桌面窗口的极淡磁吸参考轮廓 + 进程水印（不挡视线，直观标明可磁吸边缘）
            for rw in self.real_windows.iter().take(8) {
                if rw.is_maximized {
                    continue;
                }
                let lr = self.local_rect(rw.rect);
                let rf = rect_f(
                    lr.left as f32 + 1.0,
                    lr.top as f32 + 1.0,
                    lr.right as f32 - 1.0,
                    lr.bottom as f32 - 1.0,
                );
                rt.DrawRectangle(&rf, &b_real_outline, 1.2 * s, dashed_style.as_ref());
                if let Some(fmt) = &tag_fmt {
                    if !rw.process_name.is_empty() && lr.width() > 140 {
                        let tw = ((rw.process_name.chars().count() as f32 * 7.2 + 14.0) * s)
                            .min((lr.width() as f32 - 16.0).max(40.0));
                        let th = 19.0 * s;
                        let tr = rect_f(rf.left + 6.0 * s, rf.top + 6.0 * s, rf.left + 6.0 * s + tw, rf.top + 6.0 * s + th);
                        rt.FillRoundedRectangle(&rounded_rect(tr, 4.0 * s), &b_real_tag_bg);
                        let wide = to_wide(&rw.process_name);
                        let text_box = rect_f(tr.left + 6.0 * s, tr.top + 1.0 * s, tr.right - 4.0 * s, tr.bottom);
                        rt.DrawText(
                            &wide[..wide.len().saturating_sub(1)],
                            fmt,
                            &text_box,
                            &b_white,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                            DWRITE_MEASURING_MODE_NATURAL,
                        );
                    }
                }
            }

            // 3. 绘制所有分区（1:1 真实物理尺寸 + 中央编号 + 真实像素 W x H px 与百分比）
            // Grid 模式对齐原版 GridZone.xaml 的精致小圆角 (5*s)，与等宽实心分割线严丝合缝
            let is_grid_mode = self.state.mode == ZoneLayoutType::Grid;
            let radius = if is_grid_mode { 5.0 * s } else { 10.0 * s };
            let marquee_rect = if let Some(ActiveDrag::GridMarquee {
                start_x,
                start_y,
                cur_x,
                cur_y,
                ..
            }) = &self.drag
            {
                if (cur_x - start_x).abs().max((cur_y - start_y).abs()) >= 14 {
                    Some(RectI::new(
                        (*start_x).min(*cur_x),
                        (*start_y).min(*cur_y),
                        (*start_x).max(*cur_x),
                        (*start_y).max(*cur_y),
                    ))
                } else {
                    None
                }
            } else {
                None
            };

            let mut marquee_hit_count = 0usize;

            for z in &zones {
                let lr = self.local_rect(z.rect);
                let rf = rect_f(
                    lr.left as f32 + 1.0,
                    lr.top as f32 + 1.0,
                    lr.right as f32 - 1.0,
                    lr.bottom as f32 - 1.0,
                );
                let rr = rounded_rect(rf, radius);

                let in_marquee = marquee_rect
                    .map(|mr| lr.intersect(&mr).is_some())
                    .unwrap_or(false);
                if in_marquee {
                    marquee_hit_count += 1;
                }
                // Grid 模式下彻底禁止普通悬停/点击后给整块分区涂蓝高亮或加粗描边；
                // 仅按住左键拖拽框选合并 (in_marquee) 时才高亮待合并分区！
                let is_sel = if is_grid_mode {
                    in_marquee
                } else {
                    self.selected_zone == Some(z.index)
                };
                let is_hov = if is_grid_mode {
                    false
                } else {
                    self.hovered_zone == Some(z.index)
                };

                let fill_brush = if is_sel {
                    &b_sel_fill
                } else if is_hov {
                    &b_hover_fill
                } else {
                    &b_idle_fill
                };
                rt.FillRoundedRectangle(&rr, fill_brush);
                if is_sel {
                    rt.DrawRoundedRectangle(&rr, &b_accent_glow, 5.5 * s, None);
                    rt.DrawRoundedRectangle(&rr, &b_accent, 2.2 * s, None);
                } else if is_hov {
                    rt.DrawRoundedRectangle(&rr, &b_accent, 1.5 * s, None);
                } else if is_grid_mode {
                    rt.DrawRoundedRectangle(&rr, &b_grid_idle_stroke, 1.0 * s, None);
                } else {
                    rt.DrawRoundedRectangle(&rr, &b_idle_stroke, 1.4 * s, None);
                }

                // 中央编号圆形徽章 + 下方实时物理像素尺寸与百分比
                let cx = (rf.left + rf.right) * 0.5;
                let cy = (rf.top + rf.bottom) * 0.5;
                let pill_r = 20.0 * s;
                let badge_cy = cy - 12.0 * s;
                let pr = rect_f(cx - pill_r, badge_cy - pill_r, cx + pill_r, badge_cy + pill_r);
                let prr = rounded_rect(pr, pill_r);
                rt.FillRoundedRectangle(&prr, if is_sel { &b_accent } else { &b_card_bg });
                rt.DrawRoundedRectangle(&prr, if is_sel || is_hov { &b_accent } else { &b_card_border }, 1.5 * s, None);

                if let Some(fmt) = &num_fmt {
                    let num_w = to_wide(&(z.index + 1).to_string());
                    rt.DrawText(
                        &num_w[..num_w.len().saturating_sub(1)],
                        fmt,
                        &pr,
                        if is_sel { &b_white } else { &b_text },
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        DWRITE_MEASURING_MODE_NATURAL,
                    );
                }

                if let Some(fmt) = &px_fmt {
                    let pct_w = ((z.rect.width() as f64 / w as f64) * 100.0).round() as i32;
                    let pct_h = ((z.rect.height() as f64 / h as f64) * 100.0).round() as i32;
                    let info_str = format!(
                        "{} x {} px  ({}% x {}%)",
                        z.rect.width(),
                        z.rect.height(),
                        pct_w,
                        pct_h
                    );
                    let iw = ((info_str.len() as f32 * 7.6 + 22.0) * s).min((rf.right - rf.left - 12.0 * s).max(80.0));
                    let ih = 24.0 * s;
                    let ir = rect_f(cx - iw * 0.5, badge_cy + pill_r + 8.0 * s, cx + iw * 0.5, badge_cy + pill_r + 8.0 * s + ih);
                    if ir.bottom < rf.bottom - 4.0 * s {
                        rt.FillRoundedRectangle(&rounded_rect(ir, 6.0 * s), &b_card_bg);
                        rt.DrawRoundedRectangle(&rounded_rect(ir, 6.0 * s), &b_card_border, 1.0 * s, None);
                        let wide = to_wide(&info_str);
                        rt.DrawText(
                            &wide[..wide.len().saturating_sub(1)],
                            fmt,
                            &ir,
                            &b_text,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                            DWRITE_MEASURING_MODE_NATURAL,
                        );
                    }
                }

                // Canvas 模式下：右上角快捷关闭圆钮（图 6/7）+ 选中区 8 向缩放控制手柄
                if self.state.mode == ZoneLayoutType::Canvas {
                    if self.state.canvas_zones.len() > 1 {
                        let btn_sz = 24.0 * s;
                        let pad = 8.0 * s;
                        if (rf.right - rf.left) >= btn_sz * 2.0 + pad && (rf.bottom - rf.top) >= btn_sz * 2.0 + pad {
                            let xr = rect_f(rf.right - pad - btn_sz, rf.top + pad, rf.right - pad, rf.top + pad + btn_sz);
                            rt.FillRoundedRectangle(&rounded_rect(xr, btn_sz * 0.5), &b_card_bg);
                            rt.DrawRoundedRectangle(&rounded_rect(xr, btn_sz * 0.5), &b_card_border, 1.0 * s, None);
                            let xcx = (xr.left + xr.right) * 0.5;
                            let xcy = (xr.top + xr.bottom) * 0.5;
                            let xs = 4.2 * s;
                            rt.DrawLine(D2D_POINT_2F { x: xcx - xs, y: xcy - xs }, D2D_POINT_2F { x: xcx + xs, y: xcy + xs }, &b_text, 1.4 * s, None);
                            rt.DrawLine(D2D_POINT_2F { x: xcx + xs, y: xcy - xs }, D2D_POINT_2F { x: xcx - xs, y: xcy + xs }, &b_text, 1.4 * s, None);
                        }
                    }
                    if is_sel || is_hov {
                        let hs = 5.5 * s;
                        let handles = [
                            (rf.left, rf.top),
                            ((rf.left + rf.right) * 0.5, rf.top),
                            (rf.right, rf.top),
                            (rf.right, (rf.top + rf.bottom) * 0.5),
                            (rf.right, rf.bottom),
                            ((rf.left + rf.right) * 0.5, rf.bottom),
                            (rf.left, rf.bottom),
                            (rf.left, (rf.top + rf.bottom) * 0.5),
                        ];
                        for (hx, hy) in handles {
                            let hr = rect_f(hx - hs, hy - hs, hx + hs, hy + hs);
                            let hrr = rounded_rect(hr, 3.0 * s);
                            rt.FillRoundedRectangle(&hrr, &b_white);
                            rt.DrawRoundedRectangle(&hrr, &b_accent, 1.8 * s, None);
                        }
                    }
                }
            }

            // 4. Grid 模式：100% 统一原版 PowerToys FancyZones Fluent 风格
            // 已有分割线与新建切割线共用完全同一套实体等宽间距条（thickness = (zone_gap * s).max(4*s)）与纯净圆钮，浑然一体！
            if self.state.mode == ZoneLayoutType::Grid {
                let splitter_thickness = (self.state.zone_gap as f32 * s).max(4.0 * s);
                let splitter_half = splitter_thickness * 0.5;
                let bar_corner = (splitter_thickness * 0.25).min(2.5 * s);

                // 统一原版 Fluent 实心分割/切割色条绘制原语（已有分割线与悬停切割线 100% 同源）
                let draw_fluent_splitter_bar = |is_vertical: bool,
                                                axis_px: f32,
                                                span_a: f32,
                                                span_b: f32,
                                                brush: &windows::Win32::Graphics::Direct2D::ID2D1SolidColorBrush| {
                    let bar_rc = if is_vertical {
                        rect_f(axis_px - splitter_half, span_a, axis_px + splitter_half, span_b)
                    } else {
                        rect_f(span_a, axis_px - splitter_half, span_b, axis_px + splitter_half)
                    };
                    if bar_corner >= 0.8 {
                        rt.FillRoundedRectangle(&rounded_rect(bar_rc, bar_corner), brush);
                    } else {
                        rt.FillRectangle(&bar_rc, brush);
                    }
                };

                for d in &dividers {
                    let is_active = self.hovered_divider == Some(*d)
                        || matches!(
                            self.drag,
                            Some(ActiveDrag::GridDivider { is_vertical, track_index })
                                if is_vertical == d.is_vertical && track_index == d.track_index
                        );
                    let mid = ((d.span_start_px + d.span_end_px) as f32) * 0.5;
                    let (cx, cy) = if d.is_vertical {
                        (d.local_px as f32, mid)
                    } else {
                        (mid, d.local_px as f32)
                    };

                    // 已有分割线本体：静息态即绘制与缝隙同宽的柔和 Fluent 分隔条，悬停/拖动时实心高亮，与切割线 100% 同款！
                    let span_a = d.span_start_px as f32 + 1.0;
                    let span_b = d.span_end_px as f32 - 1.0;
                    draw_fluent_splitter_bar(
                        d.is_vertical,
                        d.local_px as f32,
                        span_a,
                        span_b,
                        if is_active { &b_accent } else { &b_divider_idle },
                    );

                    // 原版 GridResizer.xaml 同款纯净主题色圆形拖动手柄：极柔投影 + 无生硬白圈 + 两条简洁白色平行短杠 (||)/(=)
                    let hr = if is_active { 16.5 * s } else { 15.0 * s };
                    let sh_r = hr + 1.2 * s;
                    let sh_box = rect_f(cx - sh_r, cy - sh_r + 1.6 * s, cx + sh_r, cy + sh_r + 1.6 * s);
                    rt.FillRoundedRectangle(&rounded_rect(sh_box, sh_r), &b_soft_shadow);
                    let hbox = rect_f(cx - hr, cy - hr, cx + hr, cy + hr);
                    rt.FillRoundedRectangle(
                        &rounded_rect(hbox, hr),
                        if is_active { &b_accent_hover } else { &b_accent },
                    );
                    let bar_off = 2.8 * s;
                    let bar_half = 6.2 * s;
                    if d.is_vertical {
                        rt.DrawLine(
                            D2D_POINT_2F { x: cx - bar_off, y: cy - bar_half },
                            D2D_POINT_2F { x: cx - bar_off, y: cy + bar_half },
                            &b_white,
                            1.8 * s,
                            round_cap_style.as_ref(),
                        );
                        rt.DrawLine(
                            D2D_POINT_2F { x: cx + bar_off, y: cy - bar_half },
                            D2D_POINT_2F { x: cx + bar_off, y: cy + bar_half },
                            &b_white,
                            1.8 * s,
                            round_cap_style.as_ref(),
                        );
                    } else {
                        rt.DrawLine(
                            D2D_POINT_2F { x: cx - bar_half, y: cy - bar_off },
                            D2D_POINT_2F { x: cx + bar_half, y: cy - bar_off },
                            &b_white,
                            1.8 * s,
                            round_cap_style.as_ref(),
                        );
                        rt.DrawLine(
                            D2D_POINT_2F { x: cx - bar_half, y: cy + bar_off },
                            D2D_POINT_2F { x: cx + bar_half, y: cy + bar_off },
                            &b_white,
                            1.8 * s,
                            round_cap_style.as_ref(),
                        );
                    }
                }

                // 悬停在网格分区内部且未拖拽/未悬停分割线/未悬停控制舱时：
                // 直接复用 draw_fluent_splitter_bar 绘制与已有分割线 100% 相同 UI 风格的实体切割条（精确 1:1 预演切后间距）+ 极简比例胶囊
                if self.drag.is_none() && self.hovered_divider.is_none() && self.hovered_dock_btn.is_none() {
                    if let Some(hz) = self.hovered_zone {
                        if let Some(z) = zones.iter().find(|item| item.index == hz) {
                            let lr = self.local_rect(z.rect);
                            let (cx, cy) = self.cursor_local;
                            if lr.contains_point(cx, cy) {
                                let slicer = self.compute_slicer_for_zone(lr, (cx, cy), self.shift_down);
                                let cut = slicer.cut_local_px as f32;
                                let bar_brush = if slicer.snapped { &b_cyan } else { &b_accent };

                                if slicer.horizontal {
                                    draw_fluent_splitter_bar(
                                        false,
                                        cut,
                                        lr.left as f32 + 1.0,
                                        lr.right as f32 - 1.0,
                                        bar_brush,
                                    );
                                } else {
                                    draw_fluent_splitter_bar(
                                        true,
                                        cut,
                                        lr.top as f32 + 1.0,
                                        lr.bottom as f32 - 1.0,
                                        bar_brush,
                                    );
                                }

                                // 光标旁侧极简圆角磨砂微晶胶囊：实时百分比 + 像素 + Shift 切向提示
                                let top_line = if slicer.snap_from_window {
                                    if let Some(ref lbl) = slicer.snap_label {
                                        format!("{}% | {}%  ·  {}", slicer.first_pct, slicer.second_pct, lbl)
                                    } else {
                                        format!("{}% | {}%", slicer.first_pct, slicer.second_pct)
                                    }
                                } else {
                                    format!("{}% | {}%", slicer.first_pct, slicer.second_pct)
                                };
                                let shift_hint = if slicer.horizontal { "Shift ↔" } else { "Shift ↕" };
                                let sub_line = format!("{} | {}  ·  {}", slicer.first_px, slicer.second_px, shift_hint);

                                let max_chars = top_line.chars().count().max(sub_line.chars().count());
                                let cap_w = ((max_chars as f32 * 7.6 + 24.0) * s).clamp(146.0 * s, 260.0 * s);
                                let cap_h = 44.0 * s;
                                let gap_off = splitter_half + 12.0 * s;

                                let (raw_x, raw_y) = if slicer.horizontal {
                                    let y_below = cut + gap_off;
                                    let y_pref = if y_below + cap_h <= h - 54.0 * s {
                                        y_below
                                    } else {
                                        cut - gap_off - cap_h
                                    };
                                    (cx as f32 + 14.0 * s, y_pref)
                                } else {
                                    let x_right = cut + gap_off;
                                    let x_pref = if x_right + cap_w <= w - 12.0 * s {
                                        x_right
                                    } else {
                                        cut - gap_off - cap_w
                                    };
                                    (x_pref, cy as f32 - cap_h * 0.5)
                                };

                                let cap_x = raw_x.clamp(12.0 * s, (w - cap_w - 12.0 * s).max(12.0 * s));
                                let cap_y = raw_y.clamp(12.0 * s, (h - cap_h - 54.0 * s).max(12.0 * s));
                                let cap_rc = rect_f(cap_x, cap_y, cap_x + cap_w, cap_y + cap_h);
                                let cap_rr = rounded_rect(cap_rc, 9.0 * s);

                                // 胶囊柔和投影 + 磨砂卡片底 + 吸附态青蓝描边
                                let cap_sh = rect_f(
                                    cap_rc.left - 1.0,
                                    cap_rc.top + 2.5 * s,
                                    cap_rc.right + 1.0,
                                    cap_rc.bottom + 4.0 * s,
                                );
                                rt.FillRoundedRectangle(&rounded_rect(cap_sh, 10.0 * s), &b_soft_shadow);
                                rt.FillRoundedRectangle(&cap_rr, &b_card_bg);
                                rt.DrawRoundedRectangle(
                                    &cap_rr,
                                    if slicer.snapped { &b_cyan } else { &b_card_border },
                                    if slicer.snapped { 1.4 * s } else { 1.1 * s },
                                    None,
                                );

                                if let Some(fmt) = &slicer_pct_fmt {
                                    let r_top = rect_f(cap_rc.left + 6.0 * s, cap_rc.top + 4.0 * s, cap_rc.right - 6.0 * s, cap_rc.top + 22.0 * s);
                                    let wide = to_wide(&top_line);
                                    rt.DrawText(
                                        &wide[..wide.len().saturating_sub(1)],
                                        fmt,
                                        &r_top,
                                        if slicer.snapped { &b_cyan } else { &b_text },
                                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                                        DWRITE_MEASURING_MODE_NATURAL,
                                    );
                                }
                                if let Some(fmt) = &slicer_sub_fmt {
                                    let r_sub = rect_f(cap_rc.left + 6.0 * s, cap_rc.top + 22.0 * s, cap_rc.right - 6.0 * s, cap_rc.bottom - 3.0 * s);
                                    let wide = to_wide(&sub_line);
                                    rt.DrawText(
                                        &wide[..wide.len().saturating_sub(1)],
                                        fmt,
                                        &r_sub,
                                        &b_muted,
                                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                                        DWRITE_MEASURING_MODE_NATURAL,
                                    );
                                }
                            }
                        }
                    }
                }
            }

            // 5. 框选合并矩形 + 清晰合并数量反馈胶囊
            if let Some(mr) = marquee_rect {
                let mf = rect_f(mr.left as f32, mr.top as f32, mr.right as f32, mr.bottom as f32);
                rt.FillRectangle(&mf, &b_accent_glow);
                rt.DrawRectangle(&mf, &b_accent, 2.0 * s, dashed_style.as_ref());
                if let Some(fmt) = &dock_bold_fmt {
                    let merge_txt = if marquee_hit_count >= 2 {
                        if self.zh_ui {
                            format!("松开合并 {} 个区域", marquee_hit_count)
                        } else {
                            format!("Release to merge {} zones", marquee_hit_count)
                        }
                    } else if self.zh_ui {
                        "拖框跨越相邻区域以合并".to_string()
                    } else {
                        "Drag across zones to merge".to_string()
                    };
                    let mw = ((merge_txt.chars().count() as f32 * 8.4 + 26.0) * s).clamp(150.0 * s, 260.0 * s);
                    let mh = 28.0 * s;
                    let mx = ((mf.left + mf.right - mw) * 0.5).clamp(12.0 * s, (w - mw - 12.0 * s).max(12.0 * s));
                    let my = ((mf.top + mf.bottom - mh) * 0.5).clamp(12.0 * s, (h - mh - 54.0 * s).max(12.0 * s));
                    let pill = rect_f(mx, my, mx + mw, my + mh);
                    let pill_rr = rounded_rect(pill, mh * 0.5);
                    rt.FillRoundedRectangle(&pill_rr, if marquee_hit_count >= 2 { &b_accent } else { &b_card_bg });
                    rt.DrawRoundedRectangle(&pill_rr, if marquee_hit_count >= 2 { &b_white } else { &b_card_border }, 1.2 * s, None);
                    let wide = to_wide(&merge_txt);
                    rt.DrawText(
                        &wide[..wide.len().saturating_sub(1)],
                        fmt,
                        &pill,
                        if marquee_hit_count >= 2 { &b_white } else { &b_text },
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        DWRITE_MEASURING_MODE_NATURAL,
                    );
                }
            }

            // 6. 青色智能磁吸穿透引导线 + 吸附来源小胶囊（如 Snap: Code.exe / 50%）
            for g in &self.active_guides {
                let (p0, p1, lx, ly) = if g.is_vertical {
                    (
                        D2D_POINT_2F { x: g.local_px as f32, y: 0.0 },
                        D2D_POINT_2F { x: g.local_px as f32, y: h },
                        (g.local_px as f32 + 8.0 * s).min(w - 140.0 * s),
                        (self.cursor_local.1 as f32 - 28.0 * s).clamp(76.0 * s, h - 64.0 * s),
                    )
                } else {
                    (
                        D2D_POINT_2F { x: 0.0, y: g.local_px as f32 },
                        D2D_POINT_2F { x: w, y: g.local_px as f32 },
                        (self.cursor_local.0 as f32 + 12.0 * s).clamp(16.0 * s, w - 150.0 * s),
                        (g.local_px as f32 - 28.0 * s).clamp(76.0 * s, h - 64.0 * s),
                    )
                };
                rt.DrawLine(p0, p1, &b_cyan, 2.0 * s, dashed_style.as_ref());
                if let Some(fmt) = &tag_fmt {
                    let txt = if self.zh_ui {
                        format!("磁吸: {}", g.label)
                    } else {
                        format!("Snap: {}", g.label)
                    };
                    let tw = ((txt.chars().count() as f32 * 8.0 + 18.0) * s).clamp(68.0 * s, 180.0 * s);
                    let th = 22.0 * s;
                    let tr = rect_f(lx, ly, lx + tw, ly + th);
                    rt.FillRoundedRectangle(&rounded_rect(tr, 6.0 * s), &b_cyan);
                    let wide = to_wide(&txt);
                    let box_in = rect_f(tr.left + 7.0 * s, tr.top + 2.0 * s, tr.right - 4.0 * s, tr.bottom);
                    if let Ok(b_dark_ink) = rt.CreateSolidColorBrush(&rgba(0.03, 0.06, 0.10, 0.98), None) {
                        rt.DrawText(
                            &wide[..wide.len().saturating_sub(1)],
                            fmt,
                            &box_in,
                            &b_dark_ink,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                            DWRITE_MEASURING_MODE_NATURAL,
                        );
                    }
                }
            }

            // 7. 顶部可拖动玻璃悬浮控制舱（Floating Glass Dock）
            let dr = rect_f(
                self.dock_rect.left as f32,
                self.dock_rect.top as f32,
                self.dock_rect.right as f32,
                self.dock_rect.bottom as f32,
            );
            let drr = rounded_rect(dr, 14.0 * s);
            // 柔和外阴影环
            if let Ok(b_shadow) = rt.CreateSolidColorBrush(&rgba(0.0, 0.0, 0.0, 0.28), None) {
                let sr = rect_f(dr.left - 1.0, dr.top + 3.0 * s, dr.right + 1.0, dr.bottom + 5.0 * s);
                rt.FillRoundedRectangle(&rounded_rect(sr, 15.0 * s), &b_shadow);
            }
            rt.FillRoundedRectangle(&drr, &b_card_bg);
            rt.DrawRoundedRectangle(&drr, &b_card_border, 1.3 * s, None);

            // 控制舱按钮绘制
            let btn_hover_bg = if dark {
                rgba(1.0, 1.0, 1.0, 0.12)
            } else {
                rgba(0.08, 0.12, 0.20, 0.08)
            };
            let btn_active_bg = rgb_a(accent, 0.22);
            let b_btn_hover = rt.CreateSolidColorBrush(&btn_hover_bg, None).ok();
            let b_btn_active = rt.CreateSolidColorBrush(&btn_active_bg, None).ok();

            for btn in &self.dock_buttons {
                let br = rect_f(
                    btn.rect.left as f32,
                    btn.rect.top as f32,
                    btn.rect.right as f32,
                    btn.rect.bottom as f32,
                );
                let brr = rounded_rect(br, 8.0 * s);
                let is_hov = self.hovered_dock_btn == Some(btn.id) && !btn.disabled;

                if btn.id == DockButtonId::DragGrip {
                    // 绘制 6 点矢量拖动手柄
                    let dot_r = 1.6 * s;
                    let cx0 = (br.left + br.right) * 0.5 - 3.0 * s;
                    let cx1 = cx0 + 6.0 * s;
                    let cy_mid = (br.top + br.bottom) * 0.5;
                    for dy_i in [-1, 0, 1] {
                        let cy = cy_mid + (dy_i as f32) * 5.2 * s;
                        for cx in [cx0, cx1] {
                            rt.FillRoundedRectangle(
                                &rounded_rect(rect_f(cx - dot_r, cy - dot_r, cx + dot_r, cy + dot_r), dot_r),
                                &b_muted,
                            );
                        }
                    }
                    continue;
                }

                if btn.primary {
                    rt.FillRoundedRectangle(&brr, &b_accent);
                } else if btn.active {
                    if let Some(ba) = &b_btn_active {
                        rt.FillRoundedRectangle(&brr, ba);
                    }
                    rt.DrawRoundedRectangle(&brr, &b_accent, 1.3 * s, None);
                } else if is_hov {
                    if let Some(bh) = &b_btn_hover {
                        rt.FillRoundedRectangle(&brr, bh);
                    }
                }
                if btn.accent_outline {
                    rt.DrawRoundedRectangle(&brr, &b_cyan, 1.3 * s, None);
                }

                let fmt_opt = if btn.primary || btn.active || btn.accent_outline {
                    &dock_bold_fmt
                } else {
                    &dock_fmt
                };
                if let Some(fmt) = fmt_opt {
                    let wide = to_wide(&btn.label);
                    let ink = if btn.primary {
                        &b_white
                    } else if btn.disabled {
                        &b_muted
                    } else if btn.accent_outline {
                        &b_cyan
                    } else {
                        &b_text
                    };
                    rt.DrawText(
                        &wide[..wide.len().saturating_sub(1)],
                        fmt,
                        &br,
                        ink,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,
                        DWRITE_MEASURING_MODE_NATURAL,
                    );
                }

                // 间距数值文本紧跟在 GapMinus 按钮右侧
                if btn.id == DockButtonId::GapMinus {
                    if let Some(fmt) = &px_fmt {
                        let gap_txt = format!("{}px", self.state.zone_gap);
                        let gw = (46.0 * s).round();
                        let gr = rect_f(br.right + 3.0 * s, br.top, br.right + 3.0 * s + gw, br.bottom);
                        let wide = to_wide(&gap_txt);
                        rt.DrawText(
                            &wide[..wide.len().saturating_sub(1)],
                            fmt,
                            &gr,
                            &b_text,
                            D2D1_DRAW_TEXT_OPTIONS_CLIP,
                            DWRITE_MEASURING_MODE_NATURAL,
                        );
                    }
                }
            }

            // 上下文微胶囊（紧贴悬浮控制舱下方居中：显示器友好名 · 分辨率 · 虚拟桌面名）
            if let Some(fmt) = &hint_fmt {
                let ctx_text = if self.per_virtual_desktop && !self.desktop_label.is_empty() {
                    format!(
                        "{} ({}x{} · {}%)  |  {}  |  {}",
                        self.monitor.friendly_name,
                        self.monitor.physical_rect.width(),
                        self.monitor.physical_rect.height(),
                        self.monitor.scale_percent,
                        self.desktop_label,
                        self.source_layout_name
                    )
                } else {
                    format!(
                        "{} ({}x{} · {}%)  |  {}",
                        self.monitor.friendly_name,
                        self.monitor.physical_rect.width(),
                        self.monitor.physical_rect.height(),
                        self.monitor.scale_percent,
                        self.source_layout_name
                    )
                };
                let cw = ((ctx_text.chars().count() as f32 * 7.6 + 26.0) * s).clamp(220.0 * s, w * 0.72);
                let ch = 24.0 * s;
                let cx0 = ((dr.left + dr.right) * 0.5 - cw * 0.5).clamp(12.0 * s, (w - cw - 12.0 * s).max(12.0 * s));
                let cy0 = dr.bottom + 6.0 * s;
                let cr = rect_f(cx0, cy0, cx0 + cw, cy0 + ch);
                rt.FillRoundedRectangle(&rounded_rect(cr, ch * 0.5), &b_card_bg);
                rt.DrawRoundedRectangle(&rounded_rect(cr, ch * 0.5), &b_card_border, 1.0 * s, None);
                let wide = to_wide(&ctx_text);
                rt.DrawText(
                    &wide[..wide.len().saturating_sub(1)],
                    fmt,
                    &cr,
                    &b_muted,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }

            // 8. 底部快捷键指引 / 瞬态操作状态提示条
            if let Some(fmt) = &hint_fmt {
                let toast_active = self
                    .status_toast
                    .as_ref()
                    .filter(|(_, ts)| ts.elapsed().as_millis() < 2600)
                    .map(|(m, _)| m.clone());
                let (bar_text, highlight_bar) = if let Some(msg) = toast_active {
                    (msg, true)
                } else if self.state.mode == ZoneLayoutType::Grid {
                    (
                        if self.zh_ui {
                            "单击切分 (按住 Shift 横切)  |  拖分割线自动磁吸真实窗口  |  拖框跨区合并  |  C 捕获当前窗口  |  Del 删除  |  Ctrl+S 保存  |  Esc 取消".to_string()
                        } else {
                            "Click to split (Shift=Horizontal)  |  Drag dividers (snaps to real windows)  |  Marquee to merge  |  C Capture windows  |  Ctrl+S Save  |  Esc Cancel".to_string()
                        },
                        false,
                    )
                } else {
                    (
                        if self.zh_ui {
                            "拖动移动 / 8向边角缩放 (自动磁吸底层真实窗口 · Alt自由微调)  |  双击空白新增  |  C 捕获当前窗口  |  Ctrl+D 复制  |  Del 删除  |  Ctrl+S 保存  |  Esc 取消".to_string()
                        } else {
                            "Drag to move / 8-way resize (snaps to real windows · Hold Alt to disable snap)  |  Dbl-Click add  |  C Capture  |  Ctrl+D Duplicate  |  Ctrl+S Save  |  Esc Cancel".to_string()
                        },
                        false,
                    )
                };
                let bw = ((bar_text.chars().count() as f32 * 8.2 + 34.0) * s).clamp(320.0 * s, w - 32.0 * s);
                let bh = 30.0 * s;
                let bx = (w - bw) * 0.5;
                let by = h - bh - 14.0 * s;
                let br = rect_f(bx, by, bx + bw, by + bh);
                rt.FillRoundedRectangle(&rounded_rect(br, bh * 0.5), &b_card_bg);
                rt.DrawRoundedRectangle(
                    &rounded_rect(br, bh * 0.5),
                    if highlight_bar { &b_cyan } else { &b_card_border },
                    if highlight_bar { 1.5 * s } else { 1.0 * s },
                    None,
                );
                let wide = to_wide(&bar_text);
                rt.DrawText(
                    &wide[..wide.len().saturating_sub(1)],
                    fmt,
                    &br,
                    if highlight_bar { &b_cyan } else { &b_text },
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL,
                );
            }
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Win32 全屏交互覆盖层窗口注册与过程（完整光标托管、零卡顿转发）
// ─────────────────────────────────────────────────────────────────────────────

fn ensure_layout_editor_class() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(LAYOUT_EDITOR_WINDOW_CLASS);
        let wc = WNDCLASSEXW {
            cbSize: size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
            lpfnWndProc: Some(layout_editor_wnd_proc),
            hInstance: hinst,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(cls.as_ptr()),
            ..Default::default()
        };
        let _ = RegisterClassExW(&wc);
    });
}

fn create_layout_editor_window(work_rect: RectI) -> Option<HWND> {
    ensure_layout_editor_class();
    unsafe {
        let hinst = HINSTANCE(GetModuleHandleW(None).unwrap_or_default().0);
        let cls = to_wide(LAYOUT_EDITOR_WINDOW_CLASS);
        let ex_style = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
        CreateWindowExW(
            ex_style,
            PCWSTR(cls.as_ptr()),
            PCWSTR::null(),
            WS_POPUP,
            work_rect.left,
            work_rect.top,
            work_rect.width().max(1),
            work_rect.height().max(1),
            None,
            None,
            hinst,
            None,
        )
        .ok()
    }
}

unsafe extern "system" fn layout_editor_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if msg == WM_SETCURSOR {
        let hit_test = (lparam.0 as u32) & 0xFFFF;
        if hit_test == HTCLIENT {
            let kind = EDITOR_CURSOR_KIND.load(Ordering::Acquire);
            let _ = SetCursor(cursor_handle_for_kind(kind));
            return LRESULT(1);
        }
    }

    let ctrl_raw = EDITOR_CONTROL_HWND.load(Ordering::Acquire);
    if ctrl_raw != 0 {
        let ctrl_hwnd = HWND(ctrl_raw as *mut c_void);
        match msg {
            WM_MOUSEMOVE => {
                let (x, y) = unpack_lparam_xy(lparam.0);
                EDITOR_LATEST_PT.store(pack_xy(x, y), Ordering::Release);
                if EDITOR_MOVE_PENDING.swap(1, Ordering::AcqRel) == 0 {
                    let _ = PostMessageW(
                        ctrl_hwnd,
                        WM_WK_INTERNAL_EDITOR_EVENT,
                        WPARAM(ED_EVT_MOUSEMOVE),
                        LPARAM(0),
                    );
                }
                return LRESULT(0);
            }
            WM_LBUTTONDOWN => {
                let _ = PostMessageW(
                    ctrl_hwnd,
                    WM_WK_INTERNAL_EDITOR_EVENT,
                    WPARAM(ED_EVT_LBUTTONDOWN),
                    lparam,
                );
                return LRESULT(0);
            }
            WM_LBUTTONUP => {
                let _ = PostMessageW(
                    ctrl_hwnd,
                    WM_WK_INTERNAL_EDITOR_EVENT,
                    WPARAM(ED_EVT_LBUTTONUP),
                    lparam,
                );
                return LRESULT(0);
            }
            WM_LBUTTONDBLCLK => {
                let _ = PostMessageW(
                    ctrl_hwnd,
                    WM_WK_INTERNAL_EDITOR_EVENT,
                    WPARAM(ED_EVT_LBUTTONDBLCLK),
                    lparam,
                );
                return LRESULT(0);
            }
            WM_RBUTTONDOWN => {
                let _ = PostMessageW(
                    ctrl_hwnd,
                    WM_WK_INTERNAL_EDITOR_EVENT,
                    WPARAM(ED_EVT_RBUTTONDOWN),
                    lparam,
                );
                return LRESULT(0);
            }
            WM_KEYDOWN | WM_SYSKEYDOWN => {
                let _ = PostMessageW(
                    ctrl_hwnd,
                    WM_WK_INTERNAL_EDITOR_EVENT,
                    WPARAM(ED_EVT_KEYDOWN),
                    LPARAM(wparam.0 as isize),
                );
                return LRESULT(0);
            }
            WM_KEYUP | WM_SYSKEYUP => {
                let _ = PostMessageW(
                    ctrl_hwnd,
                    WM_WK_INTERNAL_EDITOR_EVENT,
                    WPARAM(ED_EVT_KEYUP),
                    LPARAM(wparam.0 as isize),
                );
                return LRESULT(0);
            }
            WM_CAPTURECHANGED => {
                let _ = PostMessageW(
                    ctrl_hwnd,
                    WM_WK_INTERNAL_EDITOR_EVENT,
                    WPARAM(ED_EVT_CAPTURE_LOST),
                    LPARAM(0),
                );
                return LRESULT(0);
            }
            WM_CLOSE => {
                let _ = PostMessageW(
                    ctrl_hwnd,
                    WM_WK_INTERNAL_EDITOR_EVENT,
                    WPARAM(ED_EVT_CLOSE),
                    LPARAM(0),
                );
                return LRESULT(0);
            }
            _ => {}
        }
    }

    DefWindowProcW(hwnd, msg, wparam, lparam)
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. 纯数学与交互状态机单元测试（MECE 覆盖全核心路径）
// ─────────────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_grid() -> EditorSnapshot {
        EditorSnapshot {
            mode: ZoneLayoutType::Grid,
            zone_gap: 8,
            rows: 1,
            columns: 2,
            row_weights: vec![100.0],
            col_weights: vec![50.0, 50.0],
            cell_map: vec![vec![0, 1]],
            canvas_zones: Vec::new(),
        }
    }

    #[test]
    fn grid_split_merge_divider_and_delete_preserve_topology() {
        let mut snap = sample_grid();
        // 纵切右侧区域 (id=1) 在 75% 处 -> 3 列 (50 / 25 / 25)
        assert!(snap.split_grid_zone_at(1, false, 0.75));
        assert_eq!(snap.columns, 3);
        assert_eq!(snap.cell_map, vec![vec![0, 1, 2]]);
        assert!((snap.col_weights.iter().sum::<f64>() - 100.0).abs() < 0.2);

        // 横切中间区域 (id=1) -> 2 行 3 列，仅中间列被上下拆开
        assert!(snap.split_grid_zone_at(1, true, 0.5));
        assert_eq!(snap.rows, 2);
        assert_eq!(snap.columns, 3);
        assert_eq!(snap.cell_map[0][0], snap.cell_map[1][0]); // 左列保持合并
        assert_ne!(snap.cell_map[0][1], snap.cell_map[1][1]); // 中列上下独立

        // 拖动第一条纵分割线至 40%
        assert!(snap.move_grid_divider(true, 0, 0.40));
        assert!((snap.col_weights[0] - 40.0).abs() < 0.2);

        // 框选合并中间上下两格 -> 自动折叠回 1 行
        let mid_top = snap.cell_map[0][1];
        let mid_bot = snap.cell_map[1][1];
        assert!(snap.merge_grid_zones(&[mid_top, mid_bot]));
        assert_eq!(snap.rows, 1);
        assert_eq!(snap.columns, 3);

        // 删除最右格 -> 并入相邻格后折叠回 2 列
        let right_id = snap.cell_map[0][2];
        assert!(snap.delete_grid_zone(right_id));
        assert_eq!(snap.columns, 2);
    }

    #[test]
    fn snap_engine_prioritizes_real_window_edges_and_gap_targets() {
        let work = RectI::new(0, 0, 2000, 1000);
        let real_wins = vec![RealWindowSnapshot {
            hwnd: HWND::default(),
            process_name: "Code.exe".to_string(),
            title: "VS Code".to_string(),
            rect: RectI::new(120, 40, 1180, 960),
            is_maximized: false,
        }];
        let other_zones = vec![ResolvedZone {
            index: 0,
            rect: RectI::new(8, 8, 600, 992),
        }];
        let (xs, ys) = build_snap_targets(work, &other_zones, &real_wins, 8, false);

        // 靠近底层真实窗口右边缘 1180（偏 5px）-> 精准磁吸并标出 Code.exe
        let (sx, guide_x) = snap_coord_to_targets(1185, &xs, 10, true);
        assert_eq!(sx, 1180);
        assert_eq!(guide_x.as_ref().map(|g| g.label.as_str()), Some("Code.exe"));

        // 靠近其他分区右侧 + zone_gap (600 + 8 = 608) -> 磁吸贴缝
        let (sx2, guide_x2) = snap_coord_to_targets(612, &xs, 10, true);
        assert_eq!(sx2, 608);
        assert_eq!(guide_x2.as_ref().map(|g| g.label.as_str()), Some("Zone 1"));

        // 靠近屏幕 50% 中心线 (500) -> 磁吸 50%
        let (sy, guide_y) = snap_coord_to_targets(504, &ys, 10, false);
        assert_eq!(sy, 500);
        assert_eq!(guide_y.as_ref().map(|g| g.label.as_str()), Some("50%"));

        // 远离一切目标 -> 不误吸
        let (raw, none_g) = snap_coord_to_targets(873, &xs, 10, true);
        assert_eq!(raw, 873);
        assert!(none_g.is_none());
    }

    #[test]
    fn capture_zones_from_real_windows_filters_duplicates_and_clamps() {
        let work = RectI::new(0, 0, 2000, 1000);
        let wins = vec![
            RealWindowSnapshot {
                hwnd: HWND::default(),
                process_name: "Code.exe".to_string(),
                title: "IDE".to_string(),
                rect: RectI::new(0, 0, 1200, 1000),
                is_maximized: false,
            },
            // 几乎完全重叠的重复窗口 -> 应被过滤
            RealWindowSnapshot {
                hwnd: HWND::default(),
                process_name: "Code.exe".to_string(),
                title: "IDE Duplicate".to_string(),
                rect: RectI::new(5, 5, 1195, 995),
                is_maximized: false,
            },
            // 右侧聊天窗口
            RealWindowSnapshot {
                hwnd: HWND::default(),
                process_name: "WeChat.exe".to_string(),
                title: "WeChat".to_string(),
                rect: RectI::new(1210, 50, 1990, 950),
                is_maximized: false,
            },
            // 最大化背景窗 -> 存在非最大化普通窗时跳过
            RealWindowSnapshot {
                hwnd: HWND::default(),
                process_name: "msedge.exe".to_string(),
                title: "Browser".to_string(),
                rect: RectI::new(0, 0, 2000, 1000),
                is_maximized: true,
            },
        ];

        let captured = capture_zones_from_real_windows(work, &wins);
        assert_eq!(captured.len(), 2);
        assert!((captured[0].x - 0.0).abs() < 0.01);
        assert!((captured[0].width - 0.6).abs() < 0.01);
        assert!((captured[1].x - 0.605).abs() < 0.01);
        assert!(captured.iter().all(|z| z.x >= 0.0 && z.y >= 0.0 && z.x + z.width <= 1.001 && z.y + z.height <= 1.001));
    }

    #[test]
    fn grid_to_canvas_conversion_is_lossless_and_bounded() {
        let mut snap = sample_grid();
        assert!(snap.split_grid_zone_at(1, true, 0.4));
        let canvas = grid_snapshot_to_canvas_zones(
            snap.rows,
            snap.columns,
            &snap.row_weights,
            &snap.col_weights,
            &snap.cell_map,
        );
        assert_eq!(canvas.len(), 3);
        for z in &canvas {
            assert!(z.x >= 0.0 && z.y >= 0.0);
            assert!(z.x + z.width <= 1.0001);
            assert!(z.y + z.height <= 1.0001);
        }
    }

    #[test]
    fn grid_slicer_snaps_to_classic_fractions_real_windows_and_respects_alt_disable() {
        let work = RectI::new(0, 0, 1920, 1080);
        let zone = RectI::new(0, 0, 1920, 1080);
        let real_wins = vec![RealWindowSnapshot {
            hwnd: HWND::default(),
            process_name: "Code.exe".to_string(),
            title: "VS Code".to_string(),
            rect: RectI::new(0, 0, 1100, 1080),
            is_maximized: false,
        }];

        // 1. 靠近分区 50% (960) 偏移 5px -> 精准微磁吸到 960 (50% | 50%, 960 | 960)
        let mid = compute_grid_slicer_preview(zone, work, (965, 500), false, false, &real_wins, 8, false);
        assert!(mid.snapped);
        assert!(!mid.snap_from_window);
        assert_eq!(mid.cut_local_px, 960);
        assert_eq!(mid.first_pct, 50);
        assert_eq!(mid.second_pct, 50);
        assert_eq!(mid.first_px, 960);
        assert_eq!(mid.second_px, 960);

        // 2. 靠近 33.3% (640) 偏移 4px -> 磁吸 33.3% (33% | 67%)
        let third = compute_grid_slicer_preview(zone, work, (636, 500), false, false, &real_wins, 8, false);
        assert!(third.snapped);
        assert_eq!(third.cut_local_px, 640);
        assert_eq!(third.first_pct, 33);
        assert_eq!(third.second_pct, 67);

        // 3. 靠近底层真实窗口边缘 1100 偏移 6px -> 磁吸至 Code.exe 边缘 1100
        let win_snap = compute_grid_slicer_preview(zone, work, (1106, 500), false, false, &real_wins, 8, false);
        assert!(win_snap.snapped);
        assert!(win_snap.snap_from_window);
        assert_eq!(win_snap.cut_local_px, 1100);
        assert_eq!(win_snap.snap_label.as_deref(), Some("Code.exe"));
        assert_eq!(win_snap.first_px + win_snap.second_px, 1920);

        // 4. 按住 Alt 关闭磁吸 -> 即使在 965 处也不吸附到 960
        let free = compute_grid_slicer_preview(zone, work, (965, 500), false, true, &real_wins, 8, false);
        assert!(!free.snapped);
        assert_eq!(free.cut_local_px, 965);
        assert_eq!(free.snap_label, None);

        // 5. 所见即所得：带 zone_gap 的 2 列布局，在左列 50% 磁吸处落刀，左列精确对半切分为 25/25/50
        let mut grid = sample_grid();
        let resolved = grid.resolve(work);
        let left_local = resolved[0].rect;
        let left_mid_x = (left_local.left + left_local.right) / 2 + 3;
        let left_slicer = compute_grid_slicer_preview(left_local, work, (left_mid_x, 400), false, false, &[], 8, false);
        assert!(left_slicer.snapped);
        assert_eq!(left_slicer.first_pct, 50);
        let frac = grid.zone_split_abs_fraction(0, false, &left_slicer);
        assert!(grid.split_grid_zone_at(0, false, frac));
        assert_eq!(grid.columns, 3);
        assert!((grid.col_weights[0] - 25.0).abs() < 0.2);
        assert!((grid.col_weights[1] - 25.0).abs() < 0.2);
        assert!((grid.col_weights[2] - 50.0).abs() < 0.2);
    }
}
