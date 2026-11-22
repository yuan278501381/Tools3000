// ─────────────────────────────────────────────────────────────────────────────
// app.rs — 独立看图器交互状态机与自适应高刷事件循环
//   - 四态生命周期：`Opening -> Viewing <-> Slideshow -> Closing`
//   - Windows DWM `monitor_height - 1` 真实桌面穿透 · Per-Monitor V2 高分屏缩放
//   - 自适应按需重绘调度：动画进行中 VSync 高刷推进，画面静止自动休眠定时器 (0% CPU)
// ─────────────────────────────────────────────────────────────────────────────

use crate::assoc_dialog::{
    detect_system_dark_theme, show_file_association_dialog, AssocDialogOutcome,
};
use crate::cache::{ImageCachePool, WM_VIEWER_IMAGE_READY, WM_VIEWER_THUMB_READY};
use crate::constants::{
    ANIM_SPEED_PRESETS, APP_WINDOW_CLASS, APP_WINDOW_TITLE, AUTOREPEAT_INITIAL_DELAY_MS,
    AUTOREPEAT_INTERVAL_MS, BACKDROP_OPEN_DURATION_SEC, CLOSE_ANIM_DURATION_SEC,
    CLOSE_END_SCALE_RATIO, FILMSTRIP_ITEM_GAP_DIP, FILMSTRIP_ITEM_SIZE_DIP,
    FILMSTRIP_SCROLL_DAMPING, HOVER_ANIM_DAMPING, IPC_COPYDATA_OPEN_PATH,
    LOADING_INDICATOR_DELAY_MS, OPEN_ANIM_DURATION_SEC, SLIDESHOW_CROSSFADE_DURATION_SEC,
    SLIDESHOW_ENTER_DIM_DURATION_SEC, SLIDESHOW_EXIT_DIM_DURATION_SEC, SLIDESHOW_INTERVAL_MS,
    SLIDE_FAST_REPEAT_DURATION_SEC, SLIDE_TRANSITION_DURATION_SEC, TOAST_DURATION_MS,
    WINDOWED_MAX_WORKAREA_RATIO, WINDOWED_MIN_CLIENT_H_DIP, WINDOWED_MIN_CLIENT_W_DIP,
    WINDOWED_TRACK_MIN_H_DIP, WINDOWED_TRACK_MIN_W_DIP, ZOOM_BADGE_FADE_MS, ZOOM_BADGE_HOLD_MS,
};
use crate::folder::{is_supported_image_path, normalize_full_path, scan_sibling_images};
use crate::i18n::ViewerLanguage;
use crate::integration::{
    copy_frame_and_file_to_clipboard, copy_path_to_clipboard, move_to_recycle_bin,
    open_with_system_dialog, reveal_in_explorer, save_frame_as_png_dialog,
    set_as_desktop_wallpaper, show_native_context_menu, ContextMenuState,
};
use crate::physics::exp_smooth;
use crate::renderer::{RenderError, RenderFrameParams, ViewerRenderer};
use crate::types::{
    BackdropMode, CloseImplodeSnapshot, DecodeError, DecodedImage, FitLayoutMode, ImageFrame,
    LifecyclePhase, RectF, SharedImage, SlideshowPortalSnapshot, ToolbarAction, WindowMode,
};
use crate::ui_layout::{HitTarget, LayoutInput, ViewerUiLayout};
use crate::viewport::ViewportCamera;
use std::borrow::Cow;
use std::collections::HashSet;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Dwm::{DwmSetWindowAttribute, DWMWA_USE_IMMERSIVE_DARK_MODE};
use windows::Win32::Graphics::Gdi::{
    GetMonitorInfoW, MonitorFromPoint, MonitorFromWindow, ScreenToClient, MONITORINFO,
    MONITOR_DEFAULTTONEAREST,
};
use windows::Win32::System::DataExchange::COPYDATASTRUCT;
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::Threading::{GetCurrentProcess, SetProcessWorkingSetSize};
use windows::Win32::UI::HiDpi::{
    AdjustWindowRectExForDpi, GetDpiForWindow, GetSystemMetricsForDpi,
    SetProcessDpiAwarenessContext, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetKeyState, ReleaseCapture, SetCapture, VK_CONTROL, VK_DELETE, VK_DOWN, VK_END, VK_ESCAPE,
    VK_F11, VK_F5, VK_HOME, VK_LEFT, VK_NEXT, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
    VK_OEM_PLUS, VK_PRIOR, VK_RETURN, VK_RIGHT, VK_SHIFT, VK_SPACE, VK_UP,
};
use windows::Win32::UI::Shell::{DragAcceptFiles, DragFinish, DragQueryFileW, HDROP};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyIcon, DestroyWindow, DispatchMessageW, FindWindowW,
    GetClientRect, GetCursorPos, GetMessageW, GetWindowLongPtrW, GetWindowRect, IsIconic,
    IsZoomed, LoadCursorW, LoadImageW, MsgWaitForMultipleObjects, PeekMessageW, PostQuitMessage,
    RegisterClassExW, SendMessageTimeoutW, SendMessageW, SetForegroundWindow, SetWindowLongPtrW,
    SetWindowPos, SetWindowTextW, ShowWindow, TranslateMessage, CS_DBLCLKS, GWLP_USERDATA,
    GWL_STYLE, HICON, HWND_NOTOPMOST, HWND_TOPMOST, ICON_BIG, ICON_SMALL, IDC_ARROW, IMAGE_ICON,
    LR_DEFAULTCOLOR, LR_SHARED, MINMAXINFO, MSG, PM_REMOVE, QS_ALLINPUT, SIZE_MINIMIZED,
    SMTO_ABORTIFHUNG, SMTO_NORMAL, SM_CXICON, SM_CXSMICON, SM_CYICON, SM_CYSMICON,
    SWP_FRAMECHANGED, SWP_NOACTIVATE, SWP_NOZORDER, SWP_SHOWWINDOW, SW_MAXIMIZE, SW_RESTORE,
    SW_SHOW, WM_ACTIVATE, WM_CANCELMODE, WM_CAPTURECHANGED, WM_CLOSE, WM_COPYDATA, WM_DESTROY,
    WM_DISPLAYCHANGE, WM_DPICHANGED, WM_DROPFILES, WM_GETMINMAXINFO, WM_KEYDOWN, WM_KILLFOCUS,
    WM_LBUTTONDBLCLK, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MBUTTONUP, WM_MOUSEMOVE, WM_MOUSEWHEEL,
    WM_MOVE, WM_PAINT, WM_QUIT, WM_RBUTTONUP, WM_SETICON, WM_SETTINGCHANGE, WM_SIZE, WNDCLASSEXW,
    WS_EX_NOREDIRECTIONBITMAP, WS_EX_TOPMOST, WS_OVERLAPPEDWINDOW, WS_POPUP, WS_VISIBLE,
};

fn to_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

fn loword_i16(l: isize) -> f32 {
    (l as u16 as i16) as f32
}

fn hiword_i16(l: isize) -> f32 {
    (((l >> 16) as u16) as i16) as f32
}

/// 计算切图后的下方缩略图带相对视觉偏移：将滞后缓冲钳制在 [-1.5, +1.5] 格内，确保连续高速连切绝不越界
fn compute_next_filmstrip_offset(
    old_idx: usize,
    old_offset: f32,
    target: usize,
    len: usize,
) -> f32 {
    if len <= 1 {
        return 0.0;
    }
    let max_idx = (len - 1) as f32;
    let prev_anchor = (old_idx as f32 + old_offset).clamp(0.0, max_idx);
    let raw_offset = (prev_anchor - target as f32).clamp(-1.5, 1.5);
    let clamped_anchor = (target as f32 + raw_offset).clamp(0.0, max_idx);
    clamped_anchor - target as f32
}

/// 计算正中央悬浮缩放倍率胶囊的不透明度（停留 850ms 后在 220ms 内柔和淡出）
fn compute_zoom_badge_alpha(shown_at: Option<Instant>, now: Instant) -> f32 {
    let Some(t0) = shown_at else {
        return 0.0;
    };
    let elapsed_ms = now.duration_since(t0).as_secs_f32() * 1000.0;
    let hold = ZOOM_BADGE_HOLD_MS as f32;
    let fade = ZOOM_BADGE_FADE_MS as f32;
    if elapsed_ms <= hold {
        1.0
    } else if elapsed_ms < hold + fade {
        let k = ((elapsed_ms - hold) / fade).clamp(0.0, 1.0);
        (1.0 - k) * (1.0 - k)
    } else {
        0.0
    }
}

/// 单实例复用：若已存在运行中的看图器窗口，则通过 `WM_COPYDATA` 转发新图片绝对路径并置前，杜绝叠加多层置顶黑幕
pub fn try_forward_to_existing_instance(path: &Path) -> bool {
    let abs = normalize_full_path(path);
    let wide_class = to_wide(APP_WINDOW_CLASS);
    let wide_path: Vec<u16> = abs.as_os_str().encode_wide().chain(Some(0)).collect();
    unsafe {
        let Ok(existing) = FindWindowW(PCWSTR(wide_class.as_ptr()), PCWSTR::null()) else {
            return false;
        };
        if existing.0.is_null() {
            return false;
        }
        let cds = COPYDATASTRUCT {
            dwData: IPC_COPYDATA_OPEN_PATH,
            cbData: (wide_path.len() * 2) as u32,
            lpData: wide_path.as_ptr() as *mut _,
        };
        let mut handled: usize = 0;
        let sent = SendMessageTimeoutW(
            existing,
            WM_COPYDATA,
            WPARAM(0),
            LPARAM(&cds as *const COPYDATASTRUCT as isize),
            SMTO_ABORTIFHUNG | SMTO_NORMAL,
            2000,
            Some(&mut handled),
        );
        if sent.0 != 0 && handled == 1 {
            let _ = ShowWindow(existing, SW_RESTORE);
            let _ = SetForegroundWindow(existing);
            return true;
        }
    }
    false
}

/// 判断是否应跳过导航：仅当目标索引与旧索引一致且当前已加载图像路径与目标路径完全一致时才跳过
/// （彻底修复删除非末尾图片后 `next_idx == old_index` 却误判跳过导致画面僵死）
fn should_skip_navigation(
    old_index: usize,
    target_index: usize,
    loaded_path: Option<&Path>,
    target_path: Option<&Path>,
) -> bool {
    target_index == old_index
        && loaded_path.is_some()
        && loaded_path == target_path
}

/// 计算删除当前图后的下一个索引（空列表返回 None 表示触发关窗）
fn next_index_after_delete(remaining_len: usize, removed_index: usize) -> Option<usize> {
    if remaining_len == 0 {
        None
    } else {
        Some(removed_index.min(remaining_len - 1))
    }
}

/// 根据新旧窗口物理屏幕 `RECT` 中心坐标差换算逻辑 DIP 补偿 `(dx_dip, dy_dip)`，
/// 100% 兼容任务栏位于底部、顶部、左侧、右侧四种方位
fn compute_window_center_compensation_dip(old_rect: RECT, new_rect: RECT, dpi: f32) -> (f32, f32) {
    let old_cx = (old_rect.left + old_rect.right) as f32 * 0.5;
    let old_cy = (old_rect.top + old_rect.bottom) as f32 * 0.5;
    let new_cx = (new_rect.left + new_rect.right) as f32 * 0.5;
    let new_cy = (new_rect.top + new_rect.bottom) as f32 * 0.5;
    let scale = (dpi / 96.0).max(0.5);
    ((old_cx - new_cx) / scale, (old_cy - new_cy) / scale)
}

/// 生成标准窗口标题：`"{文件名} - Tools3000 Picasa Viewer"`
fn format_window_title(path: Option<&Path>) -> String {
    if let Some(name) = path.and_then(|p| p.file_name()).map(|s| s.to_string_lossy()) {
        if !name.is_empty() {
            return format!("{name} - {APP_WINDOW_TITLE}");
        }
    }
    APP_WINDOW_TITLE.to_string()
}

/// 判断收到 `WM_ACTIVATE(WA_INACTIVE)` 时是否应自动从浮层态转为标准可缩放小窗口
fn should_auto_window_on_deactivate(
    mode: WindowMode,
    phase: LifecyclePhase,
    modal_dialog_active: bool,
    window_activated_once: bool,
    transitioning_window_mode: bool,
) -> bool {
    mode == WindowMode::Overlay
        && matches!(phase, LifecyclePhase::Opening | LifecyclePhase::Viewing)
        && !modal_dialog_active
        && window_activated_once
        && !transitioning_window_mode
}

/// 依据当前照片在屏幕上的实际显示几何、DPI 非客户区边框厚与显示器工作区，计算标准小窗口的外框物理 `RECT`：
/// - 客户区下限 `540×420` DIP 保证底部悬浮控制栏绝不裁切；
/// - 外框上限不超过当前显示器工作区的 `86%`；
/// - 尽量贴合照片在浮层中的中心位置并严格夹紧在 `work_area` 内。
fn compute_windowed_outer_rect(
    photo_screen_rect_dip: RectF,
    overlay_origin_phys: (i32, i32),
    work_area: RECT,
    dpi: f32,
    frame_insets_phys: (i32, i32, i32, i32),
) -> RECT {
    let scale = (dpi / 96.0).max(0.5);
    let (pad_l, pad_t, pad_r, pad_b) = frame_insets_phys;
    let extra_w_phys = (pad_l + pad_r).max(0);
    let extra_h_phys = (pad_t + pad_b).max(0);
    let extra_w_dip = extra_w_phys as f32 / scale;
    let extra_h_dip = extra_h_phys as f32 / scale;

    let work_w_phys = (work_area.right - work_area.left).max(320);
    let work_h_phys = (work_area.bottom - work_area.top).max(240);
    let work_w_dip = work_w_phys as f32 / scale;
    let work_h_dip = work_h_phys as f32 / scale;

    let max_outer_w_dip = (work_w_dip * WINDOWED_MAX_WORKAREA_RATIO).min(work_w_dip);
    let max_outer_h_dip = (work_h_dip * WINDOWED_MAX_WORKAREA_RATIO).min(work_h_dip);
    let max_client_w_dip = (max_outer_w_dip - extra_w_dip).max(320.0).min((work_w_dip - extra_w_dip).max(200.0));
    let max_client_h_dip = (max_outer_h_dip - extra_h_dip).max(240.0).min((work_h_dip - extra_h_dip).max(160.0));

    let min_client_w_dip = WINDOWED_MIN_CLIENT_W_DIP.min(max_client_w_dip);
    let min_client_h_dip = WINDOWED_MIN_CLIENT_H_DIP.min(max_client_h_dip);

    let (mut cw_dip, mut ch_dip, target_cx_phys, target_cy_phys) =
        if photo_screen_rect_dip.width() >= 1.0 && photo_screen_rect_dip.height() >= 1.0 {
            let cx = overlay_origin_phys.0 + (photo_screen_rect_dip.center_x() * scale).round() as i32;
            let cy = overlay_origin_phys.1 + (photo_screen_rect_dip.center_y() * scale).round() as i32;
            (
                photo_screen_rect_dip.width(),
                photo_screen_rect_dip.height(),
                cx,
                cy,
            )
        } else {
            (
                840.0,
                580.0,
                (work_area.left + work_area.right) / 2,
                (work_area.top + work_area.bottom) / 2,
            )
        };

    // 若照片显示尺寸超出最大独立小窗口比例，则按真实宽高比等比缩放至 max_client 内
    let shrink = (max_client_w_dip / cw_dip.max(1.0))
        .min(max_client_h_dip / ch_dip.max(1.0))
        .min(1.0);
    cw_dip *= shrink;
    ch_dip *= shrink;
    cw_dip = cw_dip.clamp(min_client_w_dip, max_client_w_dip);
    ch_dip = ch_dip.clamp(min_client_h_dip, max_client_h_dip);

    let client_w_phys = (cw_dip * scale).round() as i32;
    let client_h_phys = (ch_dip * scale).round() as i32;
    let outer_w = (client_w_phys + extra_w_phys).min(work_w_phys);
    let outer_h = (client_h_phys + extra_h_phys).min(work_h_phys);

    let raw_left = target_cx_phys - client_w_phys / 2 - pad_l;
    let raw_top = target_cy_phys - client_h_phys / 2 - pad_t;
    let max_left = (work_area.right - outer_w).max(work_area.left);
    let max_top = (work_area.bottom - outer_h).max(work_area.top);
    let left = raw_left.clamp(work_area.left, max_left);
    let top = raw_top.clamp(work_area.top, max_top);

    RECT {
        left,
        top,
        right: left + outer_w,
        bottom: top + outer_h,
    }
}

/// 将既有外框夹紧到目标显示器工作区内（防多屏拔插或切出幻灯片时窗口跑出可见区）
fn clamp_rect_to_work_area(rect: RECT, work_area: RECT) -> RECT {
    let work_w = (work_area.right - work_area.left).max(320);
    let work_h = (work_area.bottom - work_area.top).max(240);
    let w = (rect.right - rect.left).clamp(320, work_w);
    let h = (rect.bottom - rect.top).clamp(240, work_h);
    let left = rect
        .left
        .clamp(work_area.left, (work_area.right - w).max(work_area.left));
    let top = rect
        .top
        .clamp(work_area.top, (work_area.bottom - h).max(work_area.top));
    RECT {
        left,
        top,
        right: left + w,
        bottom: top + h,
    }
}

/// 获取剪贴板复制 / PNG 导出所需的当前帧完整像素；若静态图 CPU 像素已在 GPU 上传后释放，则从磁盘无缝按需重解单帧
fn resolve_frame_for_export(
    img: &DecodedImage,
    frame_idx: usize,
) -> Result<Cow<'_, ImageFrame>, DecodeError> {
    let safe_idx = frame_idx.min(img.frames.len().saturating_sub(1));
    if img.has_cpu_pixels(safe_idx) {
        return Ok(Cow::Borrowed(&img.frames[safe_idx]));
    }
    let redecoded = crate::decode::decode_image_file(&img.path)?;
    let re_idx = frame_idx.min(redecoded.frames.len().saturating_sub(1));
    redecoded
        .frames
        .into_iter()
        .nth(re_idx)
        .ok_or(DecodeError::NoValidFrames { format: "IMAGE" })
        .map(Cow::Owned)
}

pub struct ViewerApp {
    hwnd: HWND,
    lang: ViewerLanguage,
    dpi: f32,
    view_w: f32,
    view_h: f32,
    window_mode: WindowMode,
    is_fullscreen: bool,
    pre_slideshow_fullscreen: bool,
    pre_slideshow_window_mode: WindowMode,
    saved_windowed_rect: Option<RECT>,
    saved_windowed_maximized: bool,
    return_to_windowed_on_f11: bool,
    modal_dialog_active: bool,
    window_activated_once: bool,
    transitioning_window_mode: bool,
    suppress_size_render: bool,
    recreating_renderer: bool,
    memory_trim_dirty: bool,
    last_memory_trim_at: Option<Instant>,
    big_icon: HICON,
    small_icon: HICON,
    renderer: Option<ViewerRenderer>,
    cache: ImageCachePool,

    siblings: Vec<PathBuf>,
    current_index: usize,
    current_image: Option<SharedImage>,
    current_error: Option<DecodeError>,
    user_adjusted_viewport_for_current: bool,
    loading_since: Instant,
    camera: ViewportCamera,
    backdrop: BackdropMode,
    layout: ViewerUiLayout,

    // 四态生命周期与原版 Picasa 灵魂物理动效状态
    phase: LifecyclePhase,
    backdrop_open_started_at: Instant,
    backdrop_open_progress: f32,
    open_anim_armed: bool,
    presented_frames: u32,
    open_started_at: Instant,
    open_progress: f32,
    close_started_at: Option<Instant>,
    close_progress: f32,
    close_snapshot: Option<CloseImplodeSnapshot>,

    slide_started_at: Instant,
    last_manual_nav_at: Instant,
    slide_progress: f32,
    slide_dir: f32,
    slide_is_crossfade: bool,
    slide_fast_repeat: bool,

    cinema_dim: f32,
    slideshow_portal: Option<SlideshowPortalSnapshot>,
    slideshow_paused: bool,
    slideshow_step_accum_ms: f32,

    close_hover_anim: f32,
    play_hover_anim: f32,
    zoom_badge_shown_at: Option<Instant>,

    // 动图播放状态
    frame_index: usize,
    anim_playing: bool,
    anim_speed_idx: usize,
    last_frame_step: Instant,

    // 悬浮 UI 2.5s 静止净空、胶卷拖拽、按需定时器调度
    last_activity: Instant,
    ui_opacity: f32,
    hovered: HitTarget,
    show_info: bool,
    filmstrip_scroll_offset: f32,
    last_cursor_dip: (f32, f32),
    anim_timer_running: bool,

    dragging_image: bool,
    dragging_minimap: bool,
    dragging_anim_scrubber: bool,
    pressing_filmstrip: bool,
    dragging_filmstrip: bool,
    filmstrip_clicked_idx: Option<usize>,
    filmstrip_drag_accum_px: f32,
    pressing_backdrop: bool,
    backdrop_press_start: (f32, f32),
    held_repeat_action: Option<(ToolbarAction, Instant, Instant)>,

    selecting_zoom_box: bool,
    selection_start: (f32, f32),
    selection_box: Option<RectF>,
    drag_last_x: f32,
    drag_last_y: f32,
    drag_last_move_at: Instant,
    drag_moved: bool,
    last_tick: Instant,
    toast: Option<(String, Instant)>,
}

impl ViewerApp {
    pub fn run(initial_file: &Path, lang_override: Option<ViewerLanguage>) -> Result<(), String> {
        unsafe {
            let _ = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
        let lang = lang_override.unwrap_or_else(ViewerLanguage::detect_system);
        let (siblings, current_index) = scan_sibling_images(initial_file);
        if siblings.is_empty() {
            return Err(lang.strings().no_viewable_images.to_string());
        }

        let cache = ImageCachePool::new();
        cache.request_image_and_neighbors(&siblings, current_index);
        if let Some(first_path) = siblings.get(current_index) {
            cache.prefetch_thumb(first_path);
        }

        let now = Instant::now();
        let mut app = Box::new(Self {
            hwnd: HWND::default(),
            lang,
            dpi: 96.0,
            view_w: 1280.0,
            view_h: 720.0,
            window_mode: WindowMode::Overlay,
            // 常规看图态默认工作区贴合任务栏上沿（完全还原原版 Picasa），点播放进入幻灯片时自动全屏覆盖任务栏！
            is_fullscreen: false,
            pre_slideshow_fullscreen: false,
            pre_slideshow_window_mode: WindowMode::Overlay,
            saved_windowed_rect: None,
            saved_windowed_maximized: false,
            return_to_windowed_on_f11: false,
            modal_dialog_active: false,
            window_activated_once: false,
            transitioning_window_mode: false,
            suppress_size_render: false,
            recreating_renderer: false,
            memory_trim_dirty: true,
            last_memory_trim_at: None,
            big_icon: HICON::default(),
            small_icon: HICON::default(),
            renderer: None,
            cache,
            siblings,
            current_index,
            current_image: None,
            current_error: None,
            user_adjusted_viewport_for_current: false,
            loading_since: now,
            camera: ViewportCamera::default(),
            backdrop: BackdropMode::PicasaGlass,
            layout: ViewerUiLayout::default(),
            phase: LifecyclePhase::Opening,
            backdrop_open_started_at: now,
            backdrop_open_progress: 0.0,
            open_anim_armed: false,
            presented_frames: 0,
            open_started_at: now,
            open_progress: 0.0,
            close_started_at: None,
            close_progress: 0.0,
            close_snapshot: None,
            slide_started_at: now,
            last_manual_nav_at: now - Duration::from_secs(2),
            slide_progress: 1.0,
            slide_dir: 0.0,
            slide_is_crossfade: false,
            slide_fast_repeat: false,
            cinema_dim: 0.0,
            slideshow_portal: None,
            slideshow_paused: false,
            slideshow_step_accum_ms: 0.0,
            close_hover_anim: 0.0,
            play_hover_anim: 0.0,
            zoom_badge_shown_at: None,
            frame_index: 0,
            anim_playing: true,
            anim_speed_idx: 2, // 1.0x
            last_frame_step: now,
            last_activity: now,
            ui_opacity: 1.0,
            hovered: HitTarget::None,
            show_info: false,
            filmstrip_scroll_offset: 0.0,
            last_cursor_dip: (0.0, 0.0),
            anim_timer_running: false,
            dragging_image: false,
            dragging_minimap: false,
            dragging_anim_scrubber: false,
            pressing_filmstrip: false,
            dragging_filmstrip: false,
            filmstrip_clicked_idx: None,
            filmstrip_drag_accum_px: 0.0,
            pressing_backdrop: false,
            backdrop_press_start: (0.0, 0.0),
            held_repeat_action: None,
            selecting_zoom_box: false,
            selection_start: (0.0, 0.0),
            selection_box: None,
            drag_last_x: 0.0,
            drag_last_y: 0.0,
            drag_last_move_at: now,
            drag_moved: false,
            last_tick: now,
            toast: None,
        });

        let hwnd = create_overlay_window(&mut *app)?;
        app.hwnd = hwnd;
        app.cache.set_notify_hwnd(hwnd);

        unsafe {
            let dpi = GetDpiForWindow(hwnd).max(96) as f32;
            app.dpi = dpi;
            app.camera.set_dpi_scale(dpi / 96.0);
            app.apply_dpi_matched_icons(hwnd, dpi as u32);
            let mut rc = RECT::default();
            let _ = GetClientRect(hwnd, &mut rc);
            let phys_w = (rc.right - rc.left).max(1) as u32;
            let phys_h = (rc.bottom - rc.top).max(1) as u32;
            app.view_w = phys_w as f32 * 96.0 / dpi;
            app.view_h = phys_h as f32 * 96.0 / dpi;
            app.renderer = Some(ViewerRenderer::new(hwnd, phys_w, phys_h, dpi, lang)?);
            DragAcceptFiles(hwnd, true);
        }

        // 给后台解码线程最多 30ms 超短快取窗口：绝大多数本地图片首帧直接高清命中
        let wait_deadline = Instant::now() + Duration::from_millis(30);
        while Instant::now() < wait_deadline {
            if let Some(p) = app.current_path() {
                if app.cache.get_cached_image(p).is_some() {
                    break;
                }
            }
            std::thread::sleep(Duration::from_millis(2));
        }

        // 在窗口真正显现的第一帧先让 DWM 挂载可见，随后在下一 VSync 帧精准起跳 0.16x -> 1.0x 弹开动效，
        // 彻底杜绝 Windows 首帧显现延迟或大图解码耗时把开场动效吞掉！
        let show_instant = Instant::now();
        app.backdrop_open_started_at = show_instant;
        app.backdrop_open_progress = 0.0;
        app.open_anim_armed = false;
        app.presented_frames = 0;
        app.open_started_at = show_instant;
        app.open_progress = 0.0;
        app.last_tick = show_instant;
        app.last_activity = show_instant;
        app.sync_current_from_cache(true);
        app.update_window_title();
        app.recompute_layout();
        app.request_thumbs_for_visible_filmstrip();

        unsafe {
            let _ = ShowWindow(hwnd, SW_SHOW);
            let _ = SetForegroundWindow(hwnd);
        }
        app.ensure_anim_timer();
        app.render_now();

        let raw_ptr = Box::into_raw(app);
        unsafe {
            let mut msg = MSG::default();
            // 双模态 VSync 满血消息循环：
            // - 动画活跃期：PeekMessageW 排空输入消息后由硬件 VSync (Present(1)) 满血推进每一帧，
            //   彻底消除 Windows 15.6ms SetTimer 抖动与吞帧，60/120/144/240Hz 全程丝绸顺滑！
            // - 画面静止期：阻塞于 GetMessageW 实现 0.00% 静止 CPU 占用！
            'main_loop: loop {
                if (*raw_ptr).anim_timer_running {
                    while PeekMessageW(&mut msg, None, 0, 0, PM_REMOVE).as_bool() {
                        if msg.message == WM_QUIT {
                            break 'main_loop;
                        }
                        let _ = TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    if (*raw_ptr).anim_timer_running {
                        (*raw_ptr).on_tick();
                    }
                } else {
                    let res = GetMessageW(&mut msg, None, 0, 0);
                    if !res.as_bool() || res.0 == -1 {
                        break 'main_loop;
                    }
                    let _ = TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            let _ = Box::from_raw(raw_ptr);
        }
        Ok(())
    }

    fn is_slideshow(&self) -> bool {
        self.phase == LifecyclePhase::Slideshow
    }

    fn is_closing(&self) -> bool {
        self.phase == LifecyclePhase::Closing
    }

    fn is_windowed(&self) -> bool {
        self.window_mode == WindowMode::Windowed
    }

    fn current_fit_mode(&self) -> FitLayoutMode {
        if self.is_slideshow() {
            FitLayoutMode::Slideshow
        } else if self.is_windowed() {
            FitLayoutMode::Windowed
        } else {
            FitLayoutMode::Overlay
        }
    }

    fn ensure_anim_timer(&mut self) {
        if !self.hwnd.is_invalid() && !self.anim_timer_running {
            self.anim_timer_running = true;
            self.last_tick = Instant::now();
        }
    }

    /// 动画彻底收敛转入休眠（冷路径）：仅在非退出、非交互拖拽且有新增内存脏标时执行一次 DXGI Trim 与工作集修剪
    fn stop_anim_timer(&mut self) {
        let was_running = self.anim_timer_running;
        self.anim_timer_running = false;
        if was_running
            && !self.is_closing()
            && !self.dragging_image
            && !self.dragging_filmstrip
            && !self.dragging_minimap
            && !self.dragging_anim_scrubber
            && self.memory_trim_dirty
        {
            let cooldown_ok = self
                .last_memory_trim_at
                .map(|t| t.elapsed() >= Duration::from_millis(250))
                .unwrap_or(true);
            if cooldown_ok {
                self.trim_idle_memory();
            }
        }
    }

    /// 冷路径显存 + 物理工作集修剪（严禁在鼠标移动/拖拽/缩放/动画循环热路径调用）
    fn trim_idle_memory(&mut self) {
        if let Some(r) = self.renderer.as_ref() {
            r.trim_gpu_memory();
        }
        unsafe {
            let _ = SetProcessWorkingSetSize(GetCurrentProcess(), usize::MAX, usize::MAX);
        }
        self.memory_trim_dirty = false;
        self.last_memory_trim_at = Some(Instant::now());
    }

    /// 静态图成功上传至 GPU `ID2D1Bitmap1` 后立即释放 CPU 侧像素缓冲（零双份驻留），
    /// 保留完整元信息供 EXIF/状态栏显示；剪贴板复制/另存 PNG/设备丢失重建时无缝按需重解
    fn release_uploaded_static_image_cpu_pixels(&mut self) {
        let should_release = match (self.current_image.as_ref(), self.renderer.as_ref()) {
            (Some(img), Some(r)) => {
                !img.is_animated()
                    && img.has_cpu_pixels(0)
                    && r.has_main_bitmap_for(&img.path, 0)
            }
            _ => false,
        };
        if !should_release {
            return;
        }
        if let Some(img) = self.current_image.as_ref() {
            let path = img.path.clone();
            let meta_only = Arc::new(img.clone_metadata_without_pixels());
            self.cache.release_full_cache_entry(&path);
            self.current_image = Some(meta_only);
            self.memory_trim_dirty = true;
        }
    }

    fn update_window_title(&self) {
        if self.hwnd.is_invalid() {
            return;
        }
        let title = format_window_title(self.current_path().map(|p| p.as_path()));
        let wide = to_wide(&title);
        unsafe {
            let _ = SetWindowTextW(self.hwnd, PCWSTR(wide.as_ptr()));
        }
    }

    fn apply_window_dark_titlebar(&self, hwnd: HWND) {
        if hwnd.is_invalid() {
            return;
        }
        let dark_val: i32 = if detect_system_dark_theme() { 1 } else { 0 };
        unsafe {
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWA_USE_IMMERSIVE_DARK_MODE,
                &dark_val as *const i32 as *const _,
                std::mem::size_of::<i32>() as u32,
            );
        }
    }

    fn query_windowed_frame_insets(&self, dpi: u32) -> (i32, i32, i32, i32) {
        let mut r = RECT::default();
        unsafe {
            let _ = AdjustWindowRectExForDpi(
                &mut r,
                WS_OVERLAPPEDWINDOW,
                false,
                WS_EX_NOREDIRECTIONBITMAP,
                dpi.max(96),
            );
        }
        ((-r.left).max(0), (-r.top).max(0), r.right.max(0), r.bottom.max(0))
    }

    /// 从经典浮层 (`Overlay`) 平滑切换为标准可缩放小窗口 (`Windowed`)：
    /// - `preserve_inactive == true` 时传 `SWP_NOACTIVATE` + `HWND_NOTOPMOST`，绝不抢回任务栏或其他软件焦点；
    /// - 自动依当前照片显示尺寸/位置换算精致外框，SwapChain 同步缩小显著降低显存与内存占用。
    fn enter_windowed_mode(&mut self, preserve_inactive: bool) {
        if self.is_windowed() || self.is_closing() || self.hwnd.is_invalid() {
            return;
        }
        let hwnd = self.hwnd;
        let photo_rc = self.image_screen_rect();
        let dpi_u32 = self.dpi.round().max(96.0) as u32;
        let frame_insets = self.query_windowed_frame_insets(dpi_u32);

        let (old_origin, work_area) = unsafe {
            let mut old_rc = RECT::default();
            let _ = GetWindowRect(hwnd, &mut old_rc);
            let mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            let mut mi = MONITORINFO {
                cbSize: std::mem::size_of::<MONITORINFO>() as u32,
                ..Default::default()
            };
            let work = if GetMonitorInfoW(mon, &mut mi).as_bool() {
                mi.rcWork
            } else {
                RECT {
                    left: 0,
                    top: 0,
                    right: 1920,
                    bottom: 1032,
                }
            };
            ((old_rc.left, old_rc.top), work)
        };

        let outer = compute_windowed_outer_rect(
            photo_rc,
            old_origin,
            work_area,
            self.dpi,
            frame_insets,
        );
        self.apply_windowed_style_and_rect(outer, false, preserve_inactive);
    }

    fn restore_windowed_mode(
        &mut self,
        saved_rect: Option<RECT>,
        was_maximized: bool,
        preserve_inactive: bool,
    ) {
        if self.is_closing() || self.hwnd.is_invalid() {
            return;
        }
        let hwnd = self.hwnd;
        let work_area = unsafe {
            let mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            let mut mi = MONITORINFO {
                cbSize: std::mem::size_of::<MONITORINFO>() as u32,
                ..Default::default()
            };
            if GetMonitorInfoW(mon, &mut mi).as_bool() {
                mi.rcWork
            } else {
                RECT {
                    left: 0,
                    top: 0,
                    right: 1920,
                    bottom: 1032,
                }
            }
        };
        let outer = if let Some(r) = saved_rect {
            clamp_rect_to_work_area(r, work_area)
        } else {
            let dpi_u32 = self.dpi.round().max(96.0) as u32;
            let insets = self.query_windowed_frame_insets(dpi_u32);
            compute_windowed_outer_rect(RectF::default(), (work_area.left, work_area.top), work_area, self.dpi, insets)
        };
        self.apply_windowed_style_and_rect(outer, was_maximized, preserve_inactive);
    }

    fn apply_windowed_style_and_rect(
        &mut self,
        outer: RECT,
        maximize: bool,
        preserve_inactive: bool,
    ) {
        let hwnd = self.hwnd;
        self.transitioning_window_mode = true;
        self.clear_pointer_captures();

        self.window_mode = WindowMode::Windowed;
        self.is_fullscreen = false;
        self.return_to_windowed_on_f11 = false;
        self.phase = LifecyclePhase::Viewing;
        self.backdrop_open_progress = 1.0;
        self.open_anim_armed = true;
        self.open_progress = 1.0;
        self.cinema_dim = 0.0;
        self.slideshow_portal = None;
        self.hovered = HitTarget::None;
        self.ui_opacity = 1.0;
        self.camera.set_fit_layout_mode(FitLayoutMode::Windowed);

        let outer_w = (outer.right - outer.left).max(320);
        let outer_h = (outer.bottom - outer.top).max(240);
        let mut need_recreate = false;

        unsafe {
            self.suppress_size_render = true;
            SetWindowLongPtrW(hwnd, GWL_STYLE, (WS_OVERLAPPEDWINDOW | WS_VISIBLE).0 as isize);
            self.apply_window_dark_titlebar(hwnd);
            self.update_window_title();
            let flags = SWP_FRAMECHANGED
                | if preserve_inactive {
                    SWP_NOACTIVATE
                } else {
                    SWP_SHOWWINDOW
                };
            let _ = SetWindowPos(
                hwnd,
                HWND_NOTOPMOST,
                outer.left,
                outer.top,
                outer_w,
                outer_h,
                flags,
            );
            if maximize && !preserve_inactive {
                let _ = ShowWindow(hwnd, SW_MAXIMIZE);
            }
            self.suppress_size_render = false;

            let cur_dpi = GetDpiForWindow(hwnd).max(96) as f32;
            self.dpi = cur_dpi;
            self.camera.set_dpi_scale(cur_dpi / 96.0);
            self.apply_dpi_matched_icons(hwnd, cur_dpi as u32);

            let mut client_rc = RECT::default();
            let _ = GetClientRect(hwnd, &mut client_rc);
            let phys_w = (client_rc.right - client_rc.left).max(1) as u32;
            let phys_h = (client_rc.bottom - client_rc.top).max(1) as u32;
            self.view_w = phys_w as f32 * 96.0 / self.dpi;
            self.view_h = phys_h as f32 * 96.0 / self.dpi;
            if let Some(r) = self.renderer.as_mut() {
                if let Err(RenderError::DeviceLost(_)) = r.resize(phys_w, phys_h, self.dpi) {
                    need_recreate = true;
                }
            }
        }
        if need_recreate {
            let _ = self.recreate_renderer();
        }

        self.saved_windowed_rect = Some(outer);
        self.saved_windowed_maximized = maximize;

        let (iw, ih) = self.current_image_dims();
        if iw > 0.0 && ih > 0.0 {
            self.camera
                .snap_to_fit_mode(iw, ih, self.view_w, self.view_h, FitLayoutMode::Windowed);
        }

        self.transitioning_window_mode = false;
        self.memory_trim_dirty = true;
        self.recompute_layout();
        self.request_thumbs_for_visible_filmstrip();
        self.render_now();
        if !self.anim_timer_running {
            self.trim_idle_memory();
        }
    }

    /// 从小窗口态切回无边框置顶浮层/全屏态（F11 或右键菜单触发）
    fn enter_overlay_mode(&mut self, fullscreen: bool) {
        if self.is_closing() || self.hwnd.is_invalid() {
            return;
        }
        let hwnd = self.hwnd;
        if self.is_windowed() {
            unsafe {
                self.saved_windowed_maximized = IsZoomed(hwnd).as_bool();
                if !self.saved_windowed_maximized {
                    let mut rc = RECT::default();
                    if GetWindowRect(hwnd, &mut rc).is_ok() {
                        self.saved_windowed_rect = Some(rc);
                    }
                }
            }
        }

        self.transitioning_window_mode = true;
        self.window_mode = WindowMode::Overlay;
        self.is_fullscreen = fullscreen;
        self.camera.set_fit_layout_mode(FitLayoutMode::Overlay);

        let mut need_recreate = false;
        unsafe {
            self.suppress_size_render = true;
            SetWindowLongPtrW(hwnd, GWL_STYLE, (WS_POPUP | WS_VISIBLE).0 as isize);
            let mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            let mut mi = MONITORINFO {
                cbSize: std::mem::size_of::<MONITORINFO>() as u32,
                ..Default::default()
            };
            if GetMonitorInfoW(mon, &mut mi).as_bool() {
                let r = if fullscreen { mi.rcMonitor } else { mi.rcWork };
                let w = (r.right - r.left).max(640);
                let h = if fullscreen {
                    (r.bottom - r.top - 1).max(480)
                } else {
                    (r.bottom - r.top).max(480)
                };
                let _ = SetWindowPos(
                    hwnd,
                    HWND_TOPMOST,
                    r.left,
                    r.top,
                    w,
                    h,
                    SWP_FRAMECHANGED | SWP_SHOWWINDOW,
                );
            }
            self.suppress_size_render = false;

            let cur_dpi = GetDpiForWindow(hwnd).max(96) as f32;
            self.dpi = cur_dpi;
            self.camera.set_dpi_scale(cur_dpi / 96.0);
            let mut client_rc = RECT::default();
            let _ = GetClientRect(hwnd, &mut client_rc);
            let phys_w = (client_rc.right - client_rc.left).max(1) as u32;
            let phys_h = (client_rc.bottom - client_rc.top).max(1) as u32;
            self.view_w = phys_w as f32 * 96.0 / self.dpi;
            self.view_h = phys_h as f32 * 96.0 / self.dpi;
            if let Some(r) = self.renderer.as_mut() {
                if let Err(RenderError::DeviceLost(_)) = r.resize(phys_w, phys_h, self.dpi) {
                    need_recreate = true;
                }
            }
        }
        if need_recreate {
            let _ = self.recreate_renderer();
        }
        let (iw, ih) = self.current_image_dims();
        if iw > 0.0 && ih > 0.0 {
            if self.camera.is_fit_mode {
                self.camera
                    .snap_to_fit_mode(iw, ih, self.view_w, self.view_h, FitLayoutMode::Overlay);
            } else {
                self.camera
                    .clamp_target_pan(iw, ih, self.view_w, self.view_h);
            }
        }
        self.transitioning_window_mode = false;
        self.memory_trim_dirty = true;
        self.recompute_layout();
        self.request_thumbs_for_visible_filmstrip();
        self.render_now();
    }

    /// 立即干净退出（用于 `WM_CLOSE` 与标准小窗口下按 `Esc`/中键关闭）
    fn close_immediately(&mut self) {
        self.phase = LifecyclePhase::Closing;
        self.cache.begin_shutdown();
        self.anim_timer_running = false;
        self.clear_pointer_captures();
        if !self.hwnd.is_invalid() {
            unsafe {
                let _ = DestroyWindow(self.hwnd);
            }
        }
    }

    fn current_path(&self) -> Option<&PathBuf> {
        self.siblings.get(self.current_index)
    }

    fn current_image_dims(&self) -> (f32, f32) {
        if let Some(img) = self.current_image.as_ref() {
            (img.width as f32, img.height as f32)
        } else if let (Some(r), Some(p)) = (self.renderer.as_ref(), self.current_path()) {
            r.thumb_original_dims(p).unwrap_or((0.0, 0.0))
        } else {
            (0.0, 0.0)
        }
    }

    fn anim_speed(&self) -> f32 {
        ANIM_SPEED_PRESETS[self.anim_speed_idx % ANIM_SPEED_PRESETS.len()]
    }

    fn show_toast(&mut self, msg: impl Into<String>) {
        self.toast = Some((msg.into(), Instant::now()));
        self.ensure_anim_timer();
    }

    fn show_zoom_badge(&mut self) {
        self.zoom_badge_shown_at = Some(Instant::now());
        self.ensure_anim_timer();
    }

    fn touch_activity(&mut self) {
        self.last_activity = Instant::now();
    }

    fn mark_user_viewport_adjusted(&mut self) {
        if self.current_image.is_none() {
            self.user_adjusted_viewport_for_current = true;
        }
    }

    fn sync_current_from_cache(&mut self, reset_camera: bool) {
        let Some(path) = self.current_path().cloned() else {
            return;
        };
        if let Some(res) = self.cache.get_cached_image(&path) {
            match res {
                Ok(img) => {
                    let is_new = self
                        .current_image
                        .as_ref()
                        .map(|cur| cur.path != img.path)
                        .unwrap_or(true);
                    if is_new || reset_camera {
                        // 若用户在缩略图占位期已手动缩放/拖拽/旋转，原图解码到达时保留其姿态绝不粗暴重置
                        if !reset_camera && self.user_adjusted_viewport_for_current {
                            self.camera.clamp_target_pan(
                                img.width as f32,
                                img.height as f32,
                                self.view_w,
                                self.view_h,
                            );
                            self.ensure_anim_timer();
                        } else {
                            let fit_mode = self.current_fit_mode();
                            self.camera.reset_to_fit_with_mode(
                                img.width as f32,
                                img.height as f32,
                                self.view_w,
                                self.view_h,
                                fit_mode,
                            );
                        }
                        self.user_adjusted_viewport_for_current = false;
                        self.frame_index = 0;
                        self.last_frame_step = Instant::now();
                        self.anim_playing = true;
                        if let Some(r) = self.renderer.as_mut() {
                            r.clear_main_bitmap();
                        }
                        if img.is_animated() {
                            self.ensure_anim_timer();
                        }
                        self.memory_trim_dirty = true;
                    }
                    self.current_image = Some(img);
                    self.current_error = None;
                }
                Err(e) => {
                    self.current_image = None;
                    self.current_error = Some(e);
                }
            }
        }
    }

    /// 触发向心坍缩退出 (`Closing` 175ms)：捕获当前缩放与偏移快照，向屏幕正中心坍缩至 `0.28 * fit_zoom`
    fn request_animated_close(&mut self) {
        if self.is_closing() {
            return;
        }
        let (iw, ih) = self.current_image_dims();
        let fit_zoom = if iw > 0.0 && ih > 0.0 {
            ViewportCamera::compute_fit_zoom(
                iw,
                ih,
                self.camera.rotation,
                self.view_w,
                self.view_h,
                self.camera.dpi_scale,
            )
        } else {
            self.camera.zoom
        };
        self.close_snapshot = Some(CloseImplodeSnapshot {
            start_zoom: self.camera.zoom,
            target_end_zoom: (fit_zoom * CLOSE_END_SCALE_RATIO).max(0.02),
            start_pan_x: self.camera.pan_x,
            start_pan_y: self.camera.pan_y,
            start_cinema_dim: self.cinema_dim,
        });

        self.phase = LifecyclePhase::Closing;
        self.cache.begin_shutdown();
        self.close_started_at = Some(Instant::now());
        self.close_progress = 0.0;
        self.anim_playing = false;
        self.clear_pointer_captures();
        self.ensure_anim_timer();
        self.render_now();
    }

    fn clear_pointer_captures_without_release(&mut self) {
        self.dragging_image = false;
        self.camera.is_dragging = false;
        self.dragging_minimap = false;
        self.dragging_anim_scrubber = false;
        self.pressing_filmstrip = false;
        self.dragging_filmstrip = false;
        self.filmstrip_clicked_idx = None;
        self.pressing_backdrop = false;
        self.held_repeat_action = None;
        self.selecting_zoom_box = false;
        self.selection_box = None;
    }

    fn clear_pointer_captures(&mut self) {
        self.clear_pointer_captures_without_release();
        unsafe {
            let _ = ReleaseCapture();
        }
    }

    fn enter_slideshow(&mut self) {
        if self.siblings.len() <= 1 {
            let s = self.lang.strings();
            self.show_toast(s.toast_single_image_no_slideshow);
            self.render_now();
            return;
        }
        let from_windowed = self.is_windowed();
        let mut portal = SlideshowPortalSnapshot {
            zoom: self.camera.zoom,
            pan_x: self.camera.pan_x,
            pan_y: self.camera.pan_y,
            rotation_deg: self.camera.visual_rotation_deg,
            flip_x: self.camera.visual_flip_x,
            flip_y: self.camera.visual_flip_y,
        };

        self.pre_slideshow_window_mode = self.window_mode;
        self.pre_slideshow_fullscreen = self.is_fullscreen;
        if from_windowed && !self.hwnd.is_invalid() {
            unsafe {
                self.saved_windowed_maximized = IsZoomed(self.hwnd).as_bool();
                if !self.saved_windowed_maximized {
                    let mut rc = RECT::default();
                    if GetWindowRect(self.hwnd, &mut rc).is_ok() {
                        self.saved_windowed_rect = Some(rc);
                    }
                }
            }
            self.transitioning_window_mode = true;
            self.window_mode = WindowMode::Overlay;
            unsafe {
                self.suppress_size_render = true;
                SetWindowLongPtrW(self.hwnd, GWL_STYLE, (WS_POPUP | WS_VISIBLE).0 as isize);
                let _ = SetWindowPos(
                    self.hwnd,
                    HWND_TOPMOST,
                    0,
                    0,
                    0,
                    0,
                    SWP_FRAMECHANGED | SWP_SHOWWINDOW | windows::Win32::UI::WindowsAndMessaging::SWP_NOMOVE | windows::Win32::UI::WindowsAndMessaging::SWP_NOSIZE,
                );
                self.suppress_size_render = false;
            }
        }

        self.phase = LifecyclePhase::Slideshow;
        self.is_fullscreen = true;
        self.slideshow_paused = false;
        // 1.00s 双场景熄灯叠化完成后首张稳看 ~2.0s 即顺滑触发首场 1.05s 光学叠化切换
        self.slideshow_step_accum_ms = 1200.0;
        self.toast = None;
        self.zoom_badge_shown_at = None;
        self.hovered = HitTarget::None;
        self.ui_opacity = 0.0;
        self.last_activity = Instant::now() - Duration::from_secs(10);
        let (dx_comp, dy_comp) = self.sync_monitor_bounds_and_dpi(self.hwnd, self.dpi, false);
        self.transitioning_window_mode = false;

        if from_windowed {
            // 小窗口直切全屏幻灯片：直接纯净暗幕渐显，不叠加不同窗口样式间的旧窗框错位残影
            self.slideshow_portal = None;
        } else {
            // 依据新旧物理 RECT 中心坐标差补偿 X/Y，100% 兼容任务栏位于底/顶/左/右四种方位
            portal.pan_x += dx_comp;
            portal.pan_y += dy_comp;
            self.slideshow_portal = Some(portal);
        }

        let (iw, ih) = self.current_image_dims();
        if iw > 0.0 && ih > 0.0 {
            self.camera.snap_to_fit_mode(
                iw,
                ih,
                self.view_w,
                self.view_h,
                FitLayoutMode::Slideshow,
            );
        }
        self.recompute_layout();
        self.ensure_anim_timer();
        self.render_now();
    }

    /// 第一级 `Esc` 或单击画面：从 `Slideshow` 退回放映前形态（若由小窗口进入则精准还原小窗口位置与尺寸，若由浮层进入则 0.30s 逆向双场景叠化回工作区），绝不直接退出程序！
    fn stop_slideshow_gracefully(&mut self) {
        if !self.is_slideshow() {
            return;
        }
        if self.pre_slideshow_window_mode == WindowMode::Windowed {
            self.phase = LifecyclePhase::Viewing;
            self.slideshow_paused = false;
            self.hovered = HitTarget::None;
            self.ui_opacity = 1.0;
            self.cinema_dim = 0.0;
            self.slideshow_portal = None;
            self.last_activity = Instant::now();
            self.restore_windowed_mode(
                self.saved_windowed_rect,
                self.saved_windowed_maximized,
                false,
            );
            return;
        }

        let mut portal = SlideshowPortalSnapshot {
            zoom: self.camera.zoom,
            pan_x: self.camera.pan_x,
            pan_y: self.camera.pan_y,
            rotation_deg: self.camera.visual_rotation_deg,
            flip_x: self.camera.visual_flip_x,
            flip_y: self.camera.visual_flip_y,
        };

        self.phase = LifecyclePhase::Viewing;
        self.is_fullscreen = self.pre_slideshow_fullscreen;
        self.slideshow_paused = false;
        self.hovered = HitTarget::None;
        self.ui_opacity = 1.0;
        self.last_activity = Instant::now();
        let (dx_comp, dy_comp) = self.sync_monitor_bounds_and_dpi(self.hwnd, self.dpi, false);

        // 依据新旧物理 RECT 中心坐标差补偿 X/Y，使离场满屏大图在逆向叠化起点 0.00px 零跳动
        portal.pan_x += dx_comp;
        portal.pan_y += dy_comp;
        self.slideshow_portal = Some(portal);

        let (iw, ih) = self.current_image_dims();
        if iw > 0.0 && ih > 0.0 {
            self.camera
                .snap_to_fit_mode(iw, ih, self.view_w, self.view_h, FitLayoutMode::Overlay);
        }
        self.recompute_layout();
        self.ensure_anim_timer();
        self.render_now();
    }

    fn navigate_to_index(&mut self, next_idx: usize, keep_slideshow: bool) {
        self.navigate_to_index_with_dir(next_idx, None, keep_slideshow);
    }

    fn navigate_to_index_with_dir(
        &mut self,
        next_idx: usize,
        explicit_dir: Option<f32>,
        keep_slideshow: bool,
    ) {
        if self.siblings.is_empty() || self.is_closing() {
            return;
        }
        if !keep_slideshow && self.is_slideshow() {
            self.stop_slideshow_gracefully();
        }
        let len = self.siblings.len();
        let target = next_idx % len;
        let old_index = self.current_index;
        if should_skip_navigation(
            old_index,
            target,
            self.current_image.as_ref().map(|i| i.path.as_path()),
            self.siblings.get(target).map(|p| p.as_path()),
        ) {
            return;
        }

        let now = Instant::now();
        let fast_repeat = !keep_slideshow
            && now.duration_since(self.last_manual_nav_at) < Duration::from_millis(115);
        if !keep_slideshow {
            self.last_manual_nav_at = now;
        }

        let dir = explicit_dir.unwrap_or_else(|| {
            if target > old_index {
                1.0
            } else {
                -1.0
            }
        });

        let in_slideshow = self.is_slideshow();
        if let Some(prev_img) = self.current_image.as_ref() {
            if let Some(r) = self.renderer.as_mut() {
                r.capture_outgoing_for_slide(&self.camera, prev_img);
            }
        }

        self.current_index = target;
        self.user_adjusted_viewport_for_current = false;
        self.slideshow_portal = None;
        self.loading_since = now;
        self.slideshow_step_accum_ms = 0.0;
        self.zoom_badge_shown_at = None;
        self.slide_dir = dir;
        self.slide_is_crossfade = in_slideshow;
        self.slide_fast_repeat = fast_repeat;
        self.slide_progress = 0.0;
        self.slide_started_at = now;
        if !self.dragging_filmstrip {
            self.filmstrip_scroll_offset = compute_next_filmstrip_offset(
                old_index,
                self.filmstrip_scroll_offset,
                target,
                len,
            );
        }
        self.current_error = None;
        self.frame_index = 0;

        self.cache
            .request_image_and_neighbors(&self.siblings, self.current_index);
        self.update_window_title();
        if let Some(path) = self.current_path().cloned() {
            if self.cache.get_cached_image(&path).is_some() {
                self.sync_current_from_cache(true);
            } else {
                let mut init_dims = (1920.0f32, 1080.0f32);
                let has_gpu_thumb = self
                    .renderer
                    .as_ref()
                    .map(|r| r.has_thumb(&path))
                    .unwrap_or(false);
                if has_gpu_thumb {
                    if let Some(dims) = self
                        .renderer
                        .as_ref()
                        .and_then(|r| r.thumb_original_dims(&path))
                    {
                        init_dims = dims;
                    }
                } else if let Some(tb) = self.cache.take_thumb_for_upload(&path) {
                    init_dims = (tb.orig_width.max(1) as f32, tb.orig_height.max(1) as f32);
                    let mut protected: HashSet<&Path> = HashSet::new();
                    protected.insert(path.as_path());
                    if let Some(r) = self.renderer.as_mut() {
                        r.ensure_thumb_uploaded(&path, &tb, &protected);
                    }
                }
                let fit_mode = self.current_fit_mode();
                self.camera.reset_to_fit_with_mode(
                    init_dims.0,
                    init_dims.1,
                    self.view_w,
                    self.view_h,
                    fit_mode,
                );
                self.current_image = None;
                if let Some(r) = self.renderer.as_mut() {
                    r.clear_main_bitmap();
                }
            }
        }
        self.recompute_layout();
        self.request_thumbs_for_visible_filmstrip();
        if !in_slideshow {
            self.touch_activity();
        }
        self.ensure_anim_timer();
        self.render_now();
    }

    fn step_image(&mut self, delta: isize, keep_slideshow: bool) {
        if self.siblings.len() <= 1 || self.is_closing() {
            return;
        }
        let len = self.siblings.len() as isize;
        let next = (self.current_index as isize + delta).rem_euclid(len) as usize;
        let dir = if delta >= 0 { 1.0 } else { -1.0 };
        self.navigate_to_index_with_dir(next, Some(dir), keep_slideshow);
    }

    fn open_selected_or_dropped_file(&mut self, path: PathBuf) {
        if !path.is_file() || self.is_closing() {
            return;
        }
        let (siblings, idx) = scan_sibling_images(&path);
        if siblings.is_empty() {
            return;
        }
        // 若当前正处于幻灯片全屏态，先恢复退出前的窗口形态（小窗或浮层）
        if self.is_slideshow() {
            if self.pre_slideshow_window_mode == WindowMode::Windowed {
                self.phase = LifecyclePhase::Viewing;
                self.restore_windowed_mode(
                    self.saved_windowed_rect,
                    self.saved_windowed_maximized,
                    false,
                );
            } else {
                self.is_fullscreen = self.pre_slideshow_fullscreen;
                self.sync_monitor_bounds_and_dpi(self.hwnd, self.dpi, false);
            }
        }
        self.siblings = siblings;
        if let Some(r) = self.renderer.as_mut() {
            r.clear_main_bitmap();
            r.clear_outgoing_slide();
            r.reset_filmstrip_birth_clock();
        }
        let now = Instant::now();
        if self.is_windowed() {
            self.phase = LifecyclePhase::Viewing;
            self.backdrop_open_progress = 1.0;
            self.open_anim_armed = true;
            self.open_started_at = now - Duration::from_secs(2);
            self.open_progress = 1.0;
        } else {
            self.phase = LifecyclePhase::Opening;
            self.backdrop_open_progress = 1.0;
            self.open_anim_armed = false;
            self.open_started_at = now;
            self.open_progress = 0.0;
        }
        self.current_image = None;
        self.user_adjusted_viewport_for_current = false;
        self.loading_since = now;
        self.filmstrip_scroll_offset = 0.0;
        self.slide_progress = 1.0;
        self.slide_dir = 0.0;
        self.slide_is_crossfade = false;
        self.slide_fast_repeat = false;
        self.slideshow_paused = false;
        self.zoom_badge_shown_at = None;
        self.cinema_dim = 0.0;
        self.slideshow_portal = None;
        self.current_index = idx;
        self.current_error = None;
        self.frame_index = 0;
        self.cache
            .request_image_and_neighbors(&self.siblings, self.current_index);
        self.update_window_title();
        self.sync_current_from_cache(true);
        self.recompute_layout();
        self.request_thumbs_for_visible_filmstrip();
        self.ui_opacity = 1.0;
        self.touch_activity();
        self.ensure_anim_timer();
        self.render_now();
    }

    fn recompute_layout(&mut self) {
        let (eff_w, eff_h, is_anim, show_minimap, exif_lines) =
            if let Some(img) = self.current_image.as_ref() {
                let (ew, eh) = self
                    .camera
                    .effective_dims(img.width as f32, img.height as f32);
                let show_mm = self.camera.should_show_minimap(
                    img.width as f32,
                    img.height as f32,
                    self.view_w,
                    self.view_h,
                );
                (ew, eh, img.is_animated(), show_mm, img.exif_summary.len() + 2)
            } else {
                (0.0, 0.0, false, false, 3)
            };

        let is_actual_zoom =
            (self.camera.target_zoom - 1.0).abs() < 0.02 && !self.camera.is_fit_mode;

        self.layout = ViewerUiLayout::compute(&LayoutInput {
            view_w: self.view_w,
            view_h: self.view_h,
            sibling_count: self.siblings.len(),
            current_index: self.current_index,
            filmstrip_scroll_offset: self.filmstrip_scroll_offset,
            is_animated: is_anim,
            anim_playing: self.anim_playing,
            is_slideshow: self.is_slideshow(),
            is_windowed: self.is_windowed(),
            is_actual_zoom,
            is_flipped_h: self.camera.is_screen_flipped_h(),
            is_flipped_v: self.camera.is_screen_flipped_v(),
            show_info: self.show_info,
            show_minimap,
            img_eff_w: eff_w,
            img_eff_h: eff_h,
            exif_line_count: exif_lines,
        });
    }

    fn request_thumbs_for_visible_filmstrip(&mut self) {
        let indices: Vec<usize> = self
            .layout
            .filmstrip_items
            .iter()
            .map(|it| it.index)
            .collect();
        let mut protected: HashSet<&Path> = indices
            .iter()
            .filter_map(|&idx| self.siblings.get(idx).map(|p| p.as_path()))
            .collect();
        if let Some(cur) = self.siblings.get(self.current_index) {
            protected.insert(cur.as_path());
        }
        for idx in indices {
            if let Some(path) = self.siblings.get(idx) {
                let already_on_gpu = self
                    .renderer
                    .as_ref()
                    .map(|r| r.has_thumb(path))
                    .unwrap_or(false);
                if already_on_gpu {
                    continue;
                }
                if let Some(thumb) = self.cache.take_thumb_for_upload(path) {
                    if let Some(r) = self.renderer.as_mut() {
                        r.ensure_thumb_uploaded(path, &thumb, &protected);
                        self.memory_trim_dirty = true;
                    }
                } else if self.cache.is_thumb_failed(path) {
                    if let Some(r) = self.renderer.as_mut() {
                        r.mark_thumb_failed(path);
                    }
                }
            }
        }
    }

    fn image_screen_rect(&self) -> RectF {
        let (iw, ih) = self.current_image_dims();
        if iw > 0.0 && ih > 0.0 {
            self.camera
                .image_screen_rect(iw, ih, self.view_w, self.view_h)
        } else {
            RectF::default()
        }
    }

    fn open_context_menu_at_screen(&mut self, screen_x: i32, screen_y: i32) {
        let st = ContextMenuState {
            is_slideshow: self.is_slideshow(),
            is_fullscreen: self.window_mode == WindowMode::Overlay && self.is_fullscreen,
            show_info: self.show_info,
            is_animated: self
                .current_image
                .as_ref()
                .map(|i| i.is_animated())
                .unwrap_or(false),
            flipped_h: self.camera.is_screen_flipped_h(),
            flipped_v: self.camera.is_screen_flipped_v(),
            lang: self.lang,
        };
        self.modal_dialog_active = true;
        let chosen = show_native_context_menu(self.hwnd, screen_x, screen_y, &st);
        self.modal_dialog_active = false;
        if let Some(act) = chosen {
            self.execute_action(act);
        }
    }

    fn execute_action(&mut self, action: ToolbarAction) {
        let s = self.lang.strings();
        match action {
            ToolbarAction::PrevImage => {
                let keep_ss = self.is_slideshow();
                self.step_image(-1, keep_ss);
            }
            ToolbarAction::NextImage => {
                let keep_ss = self.is_slideshow();
                self.step_image(1, keep_ss);
            }
            ToolbarAction::ToggleSlideshow => {
                if self.is_slideshow() {
                    self.stop_slideshow_gracefully();
                } else if self.siblings.len() <= 1 {
                    self.show_toast(s.toast_single_image_no_slideshow);
                } else {
                    self.enter_slideshow();
                    return;
                }
            }
            ToolbarAction::SlideshowTogglePause => {
                if self.is_slideshow() {
                    self.slideshow_paused = !self.slideshow_paused;
                    self.touch_activity();
                    self.ensure_anim_timer();
                }
            }
            ToolbarAction::OpenImageFile => {
                self.clear_pointer_captures();
                self.modal_dialog_active = true;
                let picked_opt = crate::pick_image_via_dialog(self.hwnd, self.lang);
                self.modal_dialog_active = false;
                if let Some(picked) = picked_opt {
                    self.open_selected_or_dropped_file(picked);
                    return;
                }
                self.last_tick = Instant::now();
            }
            ToolbarAction::ZoomOut => {
                self.touch_activity();
                if self.is_slideshow() {
                    self.stop_slideshow_gracefully();
                }
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.mark_user_viewport_adjusted();
                    self.camera.zoom_at_point(
                        -1.5,
                        self.view_w * 0.5,
                        self.view_h * 0.5,
                        iw,
                        ih,
                        self.view_w,
                        self.view_h,
                    );
                    self.show_zoom_badge();
                }
            }
            ToolbarAction::ZoomIn => {
                self.touch_activity();
                if self.is_slideshow() {
                    self.stop_slideshow_gracefully();
                }
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.mark_user_viewport_adjusted();
                    self.camera.zoom_at_point(
                        1.5,
                        self.view_w * 0.5,
                        self.view_h * 0.5,
                        iw,
                        ih,
                        self.view_w,
                        self.view_h,
                    );
                    self.show_zoom_badge();
                }
            }
            ToolbarAction::ToggleActualSize => {
                self.touch_activity();
                if self.is_slideshow() {
                    self.stop_slideshow_gracefully();
                }
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.mark_user_viewport_adjusted();
                    self.camera.toggle_actual_or_fit(
                        self.view_w * 0.5,
                        self.view_h * 0.5,
                        iw,
                        ih,
                        self.view_w,
                        self.view_h,
                    );
                    self.show_zoom_badge();
                }
            }
            ToolbarAction::FitToWidth => {
                self.touch_activity();
                if self.is_slideshow() {
                    self.stop_slideshow_gracefully();
                }
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.mark_user_viewport_adjusted();
                    self.camera.fit_to_width(iw, ih, self.view_w, self.view_h);
                    self.show_zoom_badge();
                }
            }
            ToolbarAction::RotateCcw => {
                self.touch_activity();
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.mark_user_viewport_adjusted();
                    let fit_mode = self.current_fit_mode();
                    self.camera.rotate_step(
                        false,
                        iw,
                        ih,
                        self.view_w,
                        self.view_h,
                        fit_mode,
                    );
                    self.ensure_anim_timer();
                }
            }
            ToolbarAction::RotateCw => {
                self.touch_activity();
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.mark_user_viewport_adjusted();
                    let fit_mode = self.current_fit_mode();
                    self.camera.rotate_step(
                        true,
                        iw,
                        ih,
                        self.view_w,
                        self.view_h,
                        fit_mode,
                    );
                    self.ensure_anim_timer();
                }
            }
            ToolbarAction::FlipHorizontal => {
                self.touch_activity();
                self.mark_user_viewport_adjusted();
                self.camera.toggle_flip_horizontal();
                self.ensure_anim_timer();
            }
            ToolbarAction::FlipVertical => {
                self.touch_activity();
                self.mark_user_viewport_adjusted();
                self.camera.toggle_flip_vertical();
                self.ensure_anim_timer();
            }
            ToolbarAction::AnimTogglePlay => {
                self.touch_activity();
                if self
                    .current_image
                    .as_ref()
                    .map(|i| i.is_animated())
                    .unwrap_or(false)
                {
                    self.anim_playing = !self.anim_playing;
                    self.last_frame_step = Instant::now();
                    self.ensure_anim_timer();
                    self.show_toast(if self.anim_playing {
                        s.toast_anim_resumed
                    } else {
                        s.toast_anim_paused
                    });
                }
            }
            ToolbarAction::AnimPrevFrame => {
                self.touch_activity();
                if let Some(img) = self.current_image.as_ref() {
                    if img.is_animated() {
                        self.anim_playing = false;
                        self.frame_index =
                            (self.frame_index + img.frames.len() - 1) % img.frames.len();
                    }
                }
            }
            ToolbarAction::AnimNextFrame => {
                self.touch_activity();
                if let Some(img) = self.current_image.as_ref() {
                    if img.is_animated() {
                        self.anim_playing = false;
                        self.frame_index = (self.frame_index + 1) % img.frames.len();
                    }
                }
            }
            ToolbarAction::AnimCycleSpeed => {
                self.touch_activity();
                self.anim_speed_idx = (self.anim_speed_idx + 1) % ANIM_SPEED_PRESETS.len();
                self.show_toast(format!("{}{:.2}x", s.toast_anim_speed_prefix, self.anim_speed()));
            }
            ToolbarAction::SaveCurrentFramePng => {
                self.touch_activity();
                if let (Some(img), Some(path)) =
                    (self.current_image.clone(), self.current_path().cloned())
                {
                    let safe_idx = self.frame_index.min(img.frames.len().saturating_sub(1));
                    match resolve_frame_for_export(&img, safe_idx) {
                        Ok(frame) => {
                            self.modal_dialog_active = true;
                            let res = save_frame_as_png_dialog(
                                self.hwnd,
                                frame.as_ref(),
                                self.camera.rotation,
                                self.camera.flip_x,
                                self.camera.flip_y,
                                &path,
                                safe_idx,
                                img.is_animated(),
                                self.lang,
                            );
                            self.modal_dialog_active = false;
                            match res {
                                Ok(Some(saved)) => {
                                    let name = saved
                                        .file_name()
                                        .map(|st| st.to_string_lossy().to_string())
                                        .unwrap_or_default();
                                    self.show_toast(format!("{}{name}", s.toast_saved_png_prefix));
                                }
                                Ok(None) => {}
                                Err(e) => {
                                    self.show_toast(format!("{}{e}", s.toast_save_png_failed_prefix))
                                }
                            }
                        }
                        Err(e) => {
                            self.show_toast(format!("{}{e}", s.toast_save_png_failed_prefix));
                        }
                    }
                }
                self.last_tick = Instant::now();
            }
            ToolbarAction::CopyImage => {
                self.touch_activity();
                if let (Some(img), Some(path)) =
                    (self.current_image.clone(), self.current_path().cloned())
                {
                    let safe_idx = self.frame_index.min(img.frames.len().saturating_sub(1));
                    let copied = resolve_frame_for_export(&img, safe_idx)
                        .map(|frame| {
                            copy_frame_and_file_to_clipboard(
                                self.hwnd,
                                frame.as_ref(),
                                self.camera.rotation,
                                self.camera.flip_x,
                                self.camera.flip_y,
                                &path,
                            )
                        })
                        .unwrap_or(false);
                    if copied {
                        self.show_toast(s.toast_copied_image);
                    } else {
                        self.show_toast(s.toast_copy_failed);
                    }
                }
            }
            ToolbarAction::CopyPath => {
                self.touch_activity();
                if let Some(path) = self.current_path() {
                    if copy_path_to_clipboard(self.hwnd, path) {
                        self.show_toast(s.toast_copied_path);
                    }
                }
            }
            ToolbarAction::SetAsWallpaper => {
                self.touch_activity();
                if let Some(path) = self.current_path() {
                    if set_as_desktop_wallpaper(path) {
                        self.show_toast(s.toast_wallpaper_ok);
                    } else {
                        self.show_toast(s.toast_wallpaper_fail);
                    }
                }
            }
            ToolbarAction::CycleBackdrop => {
                self.touch_activity();
                self.backdrop = self.backdrop.next();
                self.show_toast(format!(
                    "{}{}",
                    s.toast_backdrop_prefix,
                    s.backdrop_label(self.backdrop)
                ));
            }
            ToolbarAction::ToggleFullscreen => {
                self.touch_activity();
                if self.is_slideshow() {
                    self.stop_slideshow_gracefully();
                    return;
                }
                if self.is_windowed() {
                    self.return_to_windowed_on_f11 = true;
                    self.enter_overlay_mode(true);
                    self.show_toast(s.toast_fullscreen_on);
                    self.render_now();
                    return;
                }
                if self.return_to_windowed_on_f11 {
                    self.return_to_windowed_on_f11 = false;
                    self.restore_windowed_mode(
                        self.saved_windowed_rect,
                        self.saved_windowed_maximized,
                        false,
                    );
                    return;
                }
                self.is_fullscreen = !self.is_fullscreen;
                self.sync_monitor_bounds_and_dpi(self.hwnd, self.dpi, true);
                self.show_toast(if self.is_fullscreen {
                    s.toast_fullscreen_on
                } else {
                    s.toast_workarea_on
                });
                return;
            }
            ToolbarAction::ToggleInfo => {
                self.touch_activity();
                self.show_info = !self.show_info;
            }
            ToolbarAction::RevealInExplorer => {
                self.touch_activity();
                if let Some(path) = self.current_path() {
                    reveal_in_explorer(path);
                }
            }
            ToolbarAction::OpenWithDialog => {
                self.touch_activity();
                if let Some(path) = self.current_path().cloned() {
                    self.modal_dialog_active = true;
                    open_with_system_dialog(self.hwnd, &path);
                    self.modal_dialog_active = false;
                }
            }
            ToolbarAction::RegisterDefaultApp => {
                self.touch_activity();
                self.clear_pointer_captures();
                self.modal_dialog_active = true;
                let outcome = show_file_association_dialog(self.hwnd, self.lang);
                self.modal_dialog_active = false;
                match outcome {
                    AssocDialogOutcome::Applied => self.show_toast(s.toast_registered_ok),
                    AssocDialogOutcome::Unregistered => self.show_toast(s.assoc_unreg_done),
                    AssocDialogOutcome::Failed(e) => {
                        self.show_toast(format!("{}{e}", s.toast_register_failed_prefix))
                    }
                    AssocDialogOutcome::Closed => {}
                }
                self.last_tick = Instant::now();
            }
            ToolbarAction::DeleteToRecycleBin => {
                self.touch_activity();
                if let Some(path) = self.current_path().cloned() {
                    self.modal_dialog_active = true;
                    let deleted = move_to_recycle_bin(self.hwnd, &path);
                    self.modal_dialog_active = false;
                    if deleted {
                        self.cache.invalidate_path(&path);
                        self.siblings.remove(self.current_index);
                        // 必须清空当前已删除图及其主纹理/错误态，防止删除非末尾图时索引不变而误判跳过导致画面僵死
                        self.current_image = None;
                        self.current_error = None;
                        self.user_adjusted_viewport_for_current = false;
                        if let Some(r) = self.renderer.as_mut() {
                            r.clear_main_bitmap();
                            r.clear_outgoing_slide();
                            r.invalidate_thumb(&path);
                        }
                        let Some(next_idx) =
                            next_index_after_delete(self.siblings.len(), self.current_index)
                        else {
                            if self.is_windowed() {
                                self.close_immediately();
                            } else {
                                self.request_animated_close();
                            }
                            return;
                        };
                        self.navigate_to_index_with_dir(next_idx, Some(1.0), false);
                        self.show_toast(s.toast_moved_to_recycle);
                        return;
                    } else {
                        self.show_toast(s.toast_recycle_cancelled);
                    }
                }
            }
            ToolbarAction::CloseViewer => {
                if self.is_windowed() {
                    self.close_immediately();
                } else {
                    self.request_animated_close();
                }
                return;
            }
        }
        self.recompute_layout();
        self.render_now();
    }

    fn on_tick(&mut self) {
        let now = Instant::now();
        let dt = now.duration_since(self.last_tick).as_secs_f32().clamp(0.0005, 0.05);
        self.last_tick = now;

        // 1. 向心坍缩退出 (`Closing` 175ms)：优先级最高，动画抵达 1.0 时销毁窗口干净退出进程
        if self.is_closing() {
            if let Some(started) = self.close_started_at {
                let elapsed = now.duration_since(started).as_secs_f32();
                self.close_progress = (elapsed / CLOSE_ANIM_DURATION_SEC).clamp(0.0, 1.0);
                self.render_now();
                if self.close_progress >= 1.0 {
                    self.stop_anim_timer();
                    unsafe {
                        let _ = DestroyWindow(self.hwnd);
                    }
                }
            }
            return;
        }

        let mut dirty = false;
        let mut keep_ticking = false;
        let mut strip_moved = false;

        // 2. 全屏雾灰遮罩淡入 + 照片防吞帧开场弹入 (`Opening` -> `Viewing`)
        if self.backdrop_open_progress < 1.0 {
            let b_elapsed = now.duration_since(self.backdrop_open_started_at).as_secs_f32();
            self.backdrop_open_progress = (b_elapsed / BACKDROP_OPEN_DURATION_SEC).min(1.0);
            dirty = true;
            if self.backdrop_open_progress < 1.0 {
                keep_ticking = true;
            }
        }

        if self.open_progress < 1.0 {
            if !self.open_anim_armed {
                let has_thumb = self
                    .current_path()
                    .and_then(|p| self.renderer.as_ref().and_then(|r| r.thumb_original_dims(p)))
                    .is_some();
                let has_visual = self.current_image.is_some()
                    || has_thumb
                    || self.current_error.is_some()
                    || now.duration_since(self.loading_since) >= Duration::from_millis(260);
                // 严守双条件防吞帧：窗口完成至少 2 帧 DWM DirectComposition 提交挂屏 + 图像/缩略图已就绪，
                // 才正式锚定 open_started_at 起跑 420ms 实体弹开与后续缩略图涟漪，人眼 100% 完整观赏开场动效！
                if self.presented_frames >= 2 && has_visual {
                    self.open_anim_armed = true;
                    self.open_started_at = now;
                    self.open_progress = 0.0;
                }
                dirty = true;
                keep_ticking = true;
            } else {
                let elapsed = now.duration_since(self.open_started_at).as_secs_f32();
                self.open_progress = (elapsed / OPEN_ANIM_DURATION_SEC).min(1.0);
                dirty = true;
                if self.open_progress >= 1.0 && self.phase == LifecyclePhase::Opening {
                    self.phase = LifecyclePhase::Viewing;
                } else {
                    keep_ticking = true;
                }
            }
        }

        if let Some(r) = self.renderer.as_ref() {
            if self.open_anim_armed && r.has_active_thumb_growth(now, self.open_started_at) {
                dirty = true;
                keep_ticking = true;
            }
        }

        // 3. 切图微位移 (165ms) / 幻灯片光学双图层交叉叠化 (1050ms)
        if self.slide_progress < 1.0 {
            let elapsed = now.duration_since(self.slide_started_at).as_secs_f32();
            let dur = if self.slide_is_crossfade {
                SLIDESHOW_CROSSFADE_DURATION_SEC
            } else if self.slide_fast_repeat {
                SLIDE_FAST_REPEAT_DURATION_SEC
            } else {
                SLIDE_TRANSITION_DURATION_SEC
            };
            self.slide_progress = (elapsed / dur).min(1.0);
            if self.slide_progress >= 1.0 {
                if let Some(r) = self.renderer.as_mut() {
                    r.clear_outgoing_slide();
                }
            } else {
                keep_ticking = true;
            }
            dirty = true;
        }

        // 4. 幻灯片原版实测双向整屏叠化：进入 1.00s 线性双场景交叉叠化，退出 0.30s 利落逆向叠化回工作区
        if self.is_slideshow() {
            if self.cinema_dim < 1.0 {
                self.cinema_dim = (self.cinema_dim + dt / SLIDESHOW_ENTER_DIM_DURATION_SEC).min(1.0);
                if self.cinema_dim >= 1.0 {
                    self.slideshow_portal = None;
                }
                dirty = true;
                keep_ticking = true;
            }
        } else if self.cinema_dim > 0.0 {
            self.cinema_dim = (self.cinema_dim - dt / SLIDESHOW_EXIT_DIM_DURATION_SEC).max(0.0);
            if self.cinema_dim <= 0.0 {
                self.slideshow_portal = None;
            }
            dirty = true;
            keep_ticking = true;
        }

        if self.is_slideshow() && !self.slideshow_paused && self.siblings.len() > 1 {
            keep_ticking = true;
            // 待 1.00s 熄灯全屏仪式完成且当前帧就绪后再推进轮播计时，确保首场叠化自然从容
            if self.cinema_dim >= 0.99 && (self.current_image.is_some() || self.current_error.is_some()) {
                self.slideshow_step_accum_ms += dt * 1000.0;
                if self.slideshow_step_accum_ms >= SLIDESHOW_INTERVAL_MS as f32 {
                    self.slideshow_step_accum_ms = 0.0;
                    self.step_image(1, true);
                    return;
                }
            }
        }

        // 5. 右上角关闭钮与中央播放水晶球的阻尼悬停动效
        let target_close_hover =
            if self.hovered == HitTarget::ToolbarButton(ToolbarAction::CloseViewer) {
                1.0
            } else {
                0.0
            };
        if (self.close_hover_anim - target_close_hover).abs() > 0.005 {
            self.close_hover_anim =
                exp_smooth(self.close_hover_anim, target_close_hover, HOVER_ANIM_DAMPING, dt);
            dirty = true;
            keep_ticking = true;
        } else if self.close_hover_anim != target_close_hover {
            self.close_hover_anim = target_close_hover;
            dirty = true;
        }

        let target_play_hover =
            if self.hovered == HitTarget::ToolbarButton(ToolbarAction::ToggleSlideshow) {
                1.0
            } else {
                0.0
            };
        if (self.play_hover_anim - target_play_hover).abs() > 0.005 {
            self.play_hover_anim =
                exp_smooth(self.play_hover_anim, target_play_hover, HOVER_ANIM_DAMPING, dt);
            dirty = true;
            keep_ticking = true;
        } else if self.play_hover_anim != target_play_hover {
            self.play_hover_anim = target_play_hover;
            dirty = true;
        }

        let vis_indices: Vec<usize> = self.layout.filmstrip_items.iter().map(|i| i.index).collect();
        if let Some(r) = self.renderer.as_mut() {
            if r.step_filmstrip_hover_anim(self.hovered, &vis_indices, dt) {
                dirty = true;
                keep_ticking = true;
            }
        }

        // 6. 按钮按住自动连发
        if let Some((act, pressed_at, last_rep)) = self.held_repeat_action {
            keep_ticking = true;
            if now.duration_since(pressed_at) >= Duration::from_millis(AUTOREPEAT_INITIAL_DELAY_MS)
                && now.duration_since(last_rep) >= Duration::from_millis(AUTOREPEAT_INTERVAL_MS)
            {
                self.held_repeat_action = Some((act, pressed_at, now));
                self.execute_action(act);
                return;
            }
        }

        // 7. 缩略图胶片流阻尼居中滚动
        if !self.dragging_filmstrip && self.filmstrip_scroll_offset.abs() > 0.002 {
            let decay = (-FILMSTRIP_SCROLL_DAMPING * dt).exp();
            self.filmstrip_scroll_offset *= decay;
            if self.filmstrip_scroll_offset.abs() <= 0.002 {
                self.filmstrip_scroll_offset = 0.0;
            } else {
                keep_ticking = true;
            }
            dirty = true;
            strip_moved = true;
        }

        // 8. 2D 物理相机（缩放、惯性滑行、橡皮筋边界回弹、连续旋转、水平/垂直镜像翻转）
        let (iw, ih) = self.current_image_dims();
        if self.camera.step_animation(dt, iw, ih, self.view_w, self.view_h) {
            dirty = true;
            keep_ticking = true;
        }

        // 9. 动图播放帧推进
        if self.anim_playing && !self.dragging_anim_scrubber {
            if let Some(img) = self.current_image.as_ref() {
                if img.is_animated() {
                    keep_ticking = true;
                    let cur_delay = img.frames[self.frame_index % img.frames.len()].delay_ms;
                    let scaled_ms = ((cur_delay as f32) / self.anim_speed()).max(12.0) as u64;
                    if now.duration_since(self.last_frame_step) >= Duration::from_millis(scaled_ms)
                    {
                        self.frame_index = (self.frame_index + 1) % img.frames.len();
                        self.last_frame_step = now;
                        dirty = true;
                    }
                }
            }
        }

        // 10. 常规看图态底栏与右上角关闭扇形常驻可见；幻灯片放映态全屏纯黑绝对纯净（1:1 还原原版 Picasa）
        self.ui_opacity = if self.is_slideshow() { 0.0 } else { 1.0 };

        if let Some(shown_at) = self.zoom_badge_shown_at {
            let total_ms = ZOOM_BADGE_HOLD_MS + ZOOM_BADGE_FADE_MS;
            if now.duration_since(shown_at) >= Duration::from_millis(total_ms) {
                self.zoom_badge_shown_at = None;
                dirty = true;
            } else {
                if now.duration_since(shown_at) >= Duration::from_millis(ZOOM_BADGE_HOLD_MS) {
                    dirty = true;
                }
                keep_ticking = true;
            }
        }

        if let Some((_, ts)) = &self.toast {
            if now.duration_since(*ts) >= Duration::from_millis(TOAST_DURATION_MS) {
                self.toast = None;
                dirty = true;
            } else {
                keep_ticking = true;
            }
        }

        if self.current_image.is_none() && self.current_error.is_none() {
            keep_ticking = true;
            // 仅在超过 LOADING_INDICATOR_DELAY_MS 且无占位缩略图（正在展示旋转加载环）时才持续重绘
            let has_thumb = self
                .current_path()
                .and_then(|p| self.renderer.as_ref().and_then(|r| r.thumb_original_dims(p)))
                .is_some();
            if !has_thumb
                && now.duration_since(self.loading_since)
                    >= Duration::from_millis(LOADING_INDICATOR_DELAY_MS)
            {
                dirty = true;
            }
        }

        if dirty {
            self.recompute_layout();
            if strip_moved {
                self.request_thumbs_for_visible_filmstrip();
            }
            self.render_now();
        } else if keep_ticking {
            // 精准计算距下一次帧切换/轮播/连发/提示淡出的剩余时间，用 MsgWaitForMultipleObjects 实现 0 延迟输入唤醒与精准节拍
            let wait_ms = self.compute_next_idle_wait_ms(now);
            unsafe {
                let _ = MsgWaitForMultipleObjects(None, false, wait_ms, QS_ALLINPUT);
            }
        }

        // 自适应休眠：若所有物理动画均已完美收敛，关闭高频 Tick 实现 0.0% 静止 CPU 占用！
        if !keep_ticking {
            self.stop_anim_timer();
        }
    }

    fn compute_next_idle_wait_ms(&self, now: Instant) -> u32 {
        let mut best_ms: u64 = 50;
        let mut consider = |ms: u64| {
            best_ms = best_ms.min(ms.max(1));
        };

        // 1. 动图下一帧精确到期时间
        if self.anim_playing && !self.dragging_anim_scrubber {
            if let Some(img) = self.current_image.as_ref() {
                if img.is_animated() {
                    let cur_delay = img.frames[self.frame_index % img.frames.len()].delay_ms;
                    let scaled_ms = ((cur_delay as f32) / self.anim_speed()).max(12.0) as u64;
                    let elapsed = now.duration_since(self.last_frame_step).as_millis() as u64;
                    consider(scaled_ms.saturating_sub(elapsed).max(1));
                }
            }
        }

        // 2. 幻灯片自动切换精确到期时间
        if self.is_slideshow() && !self.slideshow_paused && self.siblings.len() > 1 {
            let rem = (SLIDESHOW_INTERVAL_MS as f32 - self.slideshow_step_accum_ms).max(1.0) as u64;
            consider(rem);
        }

        // 3. 按钮长按自动连发精确到期时间
        if let Some((_, pressed_at, last_rep)) = self.held_repeat_action {
            let since_press = now.duration_since(pressed_at).as_millis() as u64;
            if since_press < AUTOREPEAT_INITIAL_DELAY_MS {
                consider(AUTOREPEAT_INITIAL_DELAY_MS - since_press);
            } else {
                let since_rep = now.duration_since(last_rep).as_millis() as u64;
                consider(AUTOREPEAT_INTERVAL_MS.saturating_sub(since_rep).max(1));
            }
        }

        // 4. 缩放倍率胶囊开始淡出/结束时间
        if let Some(shown_at) = self.zoom_badge_shown_at {
            let elapsed = now.duration_since(shown_at).as_millis() as u64;
            if elapsed < ZOOM_BADGE_HOLD_MS {
                consider(ZOOM_BADGE_HOLD_MS - elapsed);
            } else {
                consider(1);
            }
        }

        // 5. Toast 淡出到期时间
        if let Some((_, ts)) = &self.toast {
            let elapsed = now.duration_since(*ts).as_millis() as u64;
            consider(TOAST_DURATION_MS.saturating_sub(elapsed).max(1));
        }

        // 6. 加载指示器延迟出现时间
        if self.current_image.is_none() && self.current_error.is_none() {
            let elapsed = now.duration_since(self.loading_since).as_millis() as u64;
            if elapsed < LOADING_INDICATOR_DELAY_MS {
                consider(LOADING_INDICATOR_DELAY_MS - elapsed);
            } else {
                consider(16);
            }
        }

        best_ms.clamp(1, 50) as u32
    }

    fn update_anim_scrubber_drag(&mut self, dip_x: f32) {
        if let (Some(sc), Some(img)) = (self.layout.anim_scrubber_rect, self.current_image.as_ref())
        {
            if img.frames.len() > 1 {
                let x0 = sc.left + 4.0;
                let span = (sc.width() - 8.0).max(1.0);
                let frac = ((dip_x - x0) / span).clamp(0.0, 1.0);
                let idx = (frac * ((img.frames.len() - 1) as f32)).round() as usize;
                self.anim_playing = false;
                self.frame_index = idx.min(img.frames.len() - 1);
                // 拖动进度条暂停后立即同步刷新布局，使播放/暂停按钮图标即时更新
                self.recompute_layout();
                self.render_now();
            }
        }
    }

    fn update_minimap_drag(&mut self, dip_x: f32, dip_y: f32) {
        if let (Some(inner), Some((img_w, img_h))) = (
            self.layout.minimap_image_rect,
            self.current_image
                .as_ref()
                .map(|img| (img.width as f32, img.height as f32)),
        ) {
            self.mark_user_viewport_adjusted();
            let u = (dip_x - inner.left) / inner.width().max(1.0);
            let v = (dip_y - inner.top) / inner.height().max(1.0);
            self.camera
                .focus_normalized_point(u, v, img_w, img_h, self.view_w, self.view_h);
            self.recompute_layout();
            self.render_now();
        }
    }

    /// 捕获 GPU 设备丢失/重置后安全销毁并重建整个 Direct3D11/DirectComposition/Direct2D 渲染管线
    fn recreate_renderer(&mut self) -> bool {
        if self.recreating_renderer || self.hwnd.is_invalid() {
            return false;
        }
        self.recreating_renderer = true;
        self.renderer = None;
        let rebuilt = unsafe {
            let dpi = GetDpiForWindow(self.hwnd).max(96) as f32;
            self.dpi = dpi;
            self.camera.set_dpi_scale(dpi / 96.0);
            let mut rc = RECT::default();
            let _ = GetClientRect(self.hwnd, &mut rc);
            let phys_w = (rc.right - rc.left).max(1) as u32;
            let phys_h = (rc.bottom - rc.top).max(1) as u32;
            self.view_w = phys_w as f32 * 96.0 / dpi;
            self.view_h = phys_h as f32 * 96.0 / dpi;
            ViewerRenderer::new(self.hwnd, phys_w, phys_h, dpi, self.lang).ok()
        };
        self.recreating_renderer = false;
        if let Some(r) = rebuilt {
            self.renderer = Some(r);
            // 若当前静态图此前已在 GPU 上传后释放 CPU 像素缓冲，设备丢失重建后立即按需重解，绝不黑屏！
            let needs_reload = self
                .current_image
                .as_ref()
                .map(|img| {
                    let safe_idx = self.frame_index.min(img.frames.len().saturating_sub(1));
                    !img.has_cpu_pixels(safe_idx)
                })
                .unwrap_or(false);
            if needs_reload {
                if let Some(path) = self.current_path().cloned() {
                    if let Ok(redecoded) = crate::decode::decode_image_file(&path) {
                        self.current_image = Some(Arc::new(redecoded));
                    } else {
                        self.current_image = None;
                        self.cache
                            .request_image_and_neighbors(&self.siblings, self.current_index);
                    }
                }
            }
            self.memory_trim_dirty = true;
            self.recompute_layout();
            self.request_thumbs_for_visible_filmstrip();
            true
        } else {
            false
        }
    }

    fn render_now(&mut self) {
        if self.renderer.is_none() && !self.recreate_renderer() {
            return;
        }
        let mut device_lost = false;
        let mut rendered_ok = false;
        {
            let now = Instant::now();
            let loading_name = self
                .current_path()
                .and_then(|p| p.file_name())
                .map(|s| s.to_string_lossy().to_string());
            let fallback_thumb_path = if self.current_image.is_none() {
                self.siblings.get(self.current_index)
            } else {
                None
            };
            let show_loading_box = now.duration_since(self.loading_since)
                >= Duration::from_millis(LOADING_INDICATOR_DELAY_MS);
            let anim_speed = self.anim_speed();
            let is_slideshow = self.is_slideshow();
            let is_windowed = self.is_windowed();
            let zoom_badge_alpha = compute_zoom_badge_alpha(self.zoom_badge_shown_at, now);
            let params = RenderFrameParams {
                now,
                open_started_at: self.open_started_at,
                lang: self.lang,
                view_w: self.view_w,
                view_h: self.view_h,
                dpi: self.dpi,
                backdrop_open_progress: self.backdrop_open_progress,
                open_anim_armed: self.open_anim_armed,
                open_progress: self.open_progress,
                close_progress: self.close_progress,
                close_snapshot: self.close_snapshot,
                slide_progress: self.slide_progress,
                slide_dir: self.slide_dir,
                slide_is_crossfade: self.slide_is_crossfade,
                slide_fast_repeat: self.slide_fast_repeat,
                cinema_dim: self.cinema_dim,
                slideshow_portal: self.slideshow_portal,
                close_hover_anim: self.close_hover_anim,
                play_hover_anim: self.play_hover_anim,
                backdrop: self.backdrop,
                simulated_desktop_bg: None,
                camera: &self.camera,
                image: self.current_image.as_ref(),
                fallback_thumb_path,
                frame_index: self.frame_index,
                error: self.current_error.as_ref(),
                loading_name: loading_name.as_deref(),
                show_loading_box,
                current_index: self.current_index,
                sibling_paths: &self.siblings,
                is_slideshow,
                is_windowed,
                layout: &self.layout,
                hovered: self.hovered,
                anim_speed,
                selection_box: self.selection_box,
                toast_text: self.toast.as_ref().map(|(s, _)| s.as_str()),
                show_info: self.show_info,
                zoom_badge_alpha,
            };
            if let Some(renderer) = self.renderer.as_mut() {
                match renderer.render(&params) {
                    Ok(()) => {
                        self.presented_frames = self.presented_frames.saturating_add(1);
                        rendered_ok = true;
                    }
                    Err(RenderError::DeviceLost(_)) => {
                        device_lost = true;
                    }
                    Err(RenderError::Other(_)) => {}
                }
            }
        }
        if device_lost && self.recreate_renderer() {
            // 设备恢复后仅补绘一次当前帧，杜绝无限递归
            let now = Instant::now();
            let loading_name = self
                .current_path()
                .and_then(|p| p.file_name())
                .map(|s| s.to_string_lossy().to_string());
            let fallback_thumb_path = if self.current_image.is_none() {
                self.siblings.get(self.current_index)
            } else {
                None
            };
            let show_loading_box = now.duration_since(self.loading_since)
                >= Duration::from_millis(LOADING_INDICATOR_DELAY_MS);
            let params = RenderFrameParams {
                now,
                open_started_at: self.open_started_at,
                lang: self.lang,
                view_w: self.view_w,
                view_h: self.view_h,
                dpi: self.dpi,
                backdrop_open_progress: self.backdrop_open_progress,
                open_anim_armed: self.open_anim_armed,
                open_progress: self.open_progress,
                close_progress: self.close_progress,
                close_snapshot: self.close_snapshot,
                slide_progress: self.slide_progress,
                slide_dir: self.slide_dir,
                slide_is_crossfade: self.slide_is_crossfade,
                slide_fast_repeat: self.slide_fast_repeat,
                cinema_dim: self.cinema_dim,
                slideshow_portal: self.slideshow_portal,
                close_hover_anim: self.close_hover_anim,
                play_hover_anim: self.play_hover_anim,
                backdrop: self.backdrop,
                simulated_desktop_bg: None,
                camera: &self.camera,
                image: self.current_image.as_ref(),
                fallback_thumb_path,
                frame_index: self.frame_index,
                error: self.current_error.as_ref(),
                loading_name: loading_name.as_deref(),
                show_loading_box,
                current_index: self.current_index,
                sibling_paths: &self.siblings,
                is_slideshow: self.is_slideshow(),
                is_windowed: self.is_windowed(),
                layout: &self.layout,
                hovered: self.hovered,
                anim_speed: self.anim_speed(),
                selection_box: self.selection_box,
                toast_text: self.toast.as_ref().map(|(s, _)| s.as_str()),
                show_info: self.show_info,
                zoom_badge_alpha: compute_zoom_badge_alpha(self.zoom_badge_shown_at, now),
            };
            if let Some(renderer) = self.renderer.as_mut() {
                if renderer.render(&params).is_ok() {
                    self.presented_frames = self.presented_frames.saturating_add(1);
                    rendered_ok = true;
                }
            }
        }
        if rendered_ok {
            self.release_uploaded_static_image_cpu_pixels();
        }
    }

    fn sync_monitor_bounds_and_dpi(
        &mut self,
        hwnd: HWND,
        new_dpi: f32,
        render_immediately: bool,
    ) -> (f32, f32) {
        let clamped_dpi = new_dpi.max(96.0);
        self.dpi = clamped_dpi;
        self.camera.set_dpi_scale(clamped_dpi / 96.0);
        let mut center_comp = (0.0f32, 0.0f32);
        let mut need_recreate = false;
        unsafe {
            self.apply_dpi_matched_icons(hwnd, clamped_dpi as u32);
            let mut old_wnd_rc = RECT::default();
            let has_old_rc = GetWindowRect(hwnd, &mut old_wnd_rc).is_ok();

            let mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            let mut mi = MONITORINFO {
                cbSize: std::mem::size_of::<MONITORINFO>() as u32,
                ..Default::default()
            };
            if GetMonitorInfoW(mon, &mut mi).as_bool() {
                let r = if self.is_fullscreen {
                    mi.rcMonitor
                } else {
                    mi.rcWork
                };
                let w = (r.right - r.left).max(640);
                // 覆盖全屏态物理高度减 1px，阻止 DWM 激活 Fullscreen Redirection 吞掉叠化动画
                let h = if self.is_fullscreen {
                    (r.bottom - r.top - 1).max(480)
                } else {
                    (r.bottom - r.top).max(480)
                };
                // 抑制 SetWindowPos 同步派发 WM_SIZE 抢先渲染错误残影帧
                self.suppress_size_render = true;
                let _ = SetWindowPos(
                    hwnd,
                    None,
                    r.left,
                    r.top,
                    w,
                    h,
                    SWP_NOZORDER | SWP_NOACTIVATE,
                );
                self.suppress_size_render = false;
            }

            let mut new_wnd_rc = RECT::default();
            if has_old_rc && GetWindowRect(hwnd, &mut new_wnd_rc).is_ok() {
                center_comp =
                    compute_window_center_compensation_dip(old_wnd_rc, new_wnd_rc, clamped_dpi);
            }

            let mut rc = RECT::default();
            let _ = GetClientRect(hwnd, &mut rc);
            let phys_w = (rc.right - rc.left).max(1) as u32;
            let phys_h = (rc.bottom - rc.top).max(1) as u32;
            self.view_w = phys_w as f32 * 96.0 / clamped_dpi;
            self.view_h = phys_h as f32 * 96.0 / clamped_dpi;
            if self.presented_frames > 0 && render_immediately {
                let (dx_comp, dy_comp) = center_comp;
                if dx_comp.abs() > 0.1 {
                    self.camera.pan_x += dx_comp;
                    if !self.camera.is_fit_mode {
                        self.camera.target_pan_x += dx_comp;
                    }
                }
                if dy_comp.abs() > 0.1 {
                    self.camera.pan_y += dy_comp;
                    if !self.camera.is_fit_mode {
                        self.camera.target_pan_y += dy_comp;
                    }
                }
            }
            if let Some(r) = self.renderer.as_mut() {
                if let Err(RenderError::DeviceLost(_)) = r.resize(phys_w, phys_h, clamped_dpi) {
                    need_recreate = true;
                }
            }
        }
        if need_recreate {
            let _ = self.recreate_renderer();
        }
        self.memory_trim_dirty = true;
        if render_immediately {
            let (iw, ih) = self.current_image_dims();
            if iw > 0.0 && ih > 0.0 {
                if self.camera.is_fit_mode {
                    let fit_mode = self.current_fit_mode();
                    self.camera
                        .animate_to_fit_mode(iw, ih, self.view_w, self.view_h, fit_mode);
                    self.ensure_anim_timer();
                } else {
                    self.camera
                        .clamp_target_pan(iw, ih, self.view_w, self.view_h);
                    self.ensure_anim_timer();
                }
            }
            self.recompute_layout();
            self.render_now();
        }
        center_comp
    }

    fn to_dip(&self, phys: f32) -> f32 {
        phys * 96.0 / self.dpi.max(96.0)
    }

    fn handle_message(
        &mut self,
        hwnd: HWND,
        msg: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> Option<LRESULT> {
        match msg {
            WM_PAINT => {
                self.render_now();
                None
            }
            WM_VIEWER_IMAGE_READY => {
                self.memory_trim_dirty = true;
                self.sync_current_from_cache(false);
                self.recompute_layout();
                self.request_thumbs_for_visible_filmstrip();
                self.ensure_anim_timer();
                self.render_now();
                Some(LRESULT(0))
            }
            WM_VIEWER_THUMB_READY => {
                self.memory_trim_dirty = true;
                self.request_thumbs_for_visible_filmstrip();
                self.ensure_anim_timer();
                self.render_now();
                Some(LRESULT(0))
            }
            WM_ACTIVATE => {
                let active_state = (wparam.0 & 0xFFFF) as u32;
                if active_state != 0 {
                    self.window_activated_once = true;
                } else if should_auto_window_on_deactivate(
                    self.window_mode,
                    self.phase,
                    self.modal_dialog_active,
                    self.window_activated_once,
                    self.transitioning_window_mode,
                ) {
                    // 点击任务栏或切换至其他应用失焦时，静默切为非置顶标准可缩放小窗口，绝不抢回焦点
                    self.enter_windowed_mode(true);
                }
                None
            }
            WM_GETMINMAXINFO => {
                if self.is_windowed() && lparam.0 != 0 {
                    let dpi = self.dpi.max(96.0);
                    let scale = dpi / 96.0;
                    let (pl, pt, pr, pb) = self.query_windowed_frame_insets(dpi.round() as u32);
                    let min_w = (WINDOWED_TRACK_MIN_W_DIP * scale).round() as i32 + pl + pr;
                    let min_h = (WINDOWED_TRACK_MIN_H_DIP * scale).round() as i32 + pt + pb;
                    unsafe {
                        let mmi = &mut *(lparam.0 as *mut MINMAXINFO);
                        mmi.ptMinTrackSize.x = min_w;
                        mmi.ptMinTrackSize.y = min_h;
                    }
                    return Some(LRESULT(0));
                }
                None
            }
            WM_SETTINGCHANGE => {
                if self.is_windowed() {
                    self.apply_window_dark_titlebar(hwnd);
                }
                None
            }
            WM_CLOSE => {
                self.close_immediately();
                Some(LRESULT(0))
            }
            WM_MOVE => {
                if self.is_windowed() && !self.transitioning_window_mode {
                    unsafe {
                        if !IsZoomed(hwnd).as_bool() && !IsIconic(hwnd).as_bool() {
                            let mut rc = RECT::default();
                            if GetWindowRect(hwnd, &mut rc).is_ok() {
                                self.saved_windowed_rect = Some(rc);
                            }
                        }
                    }
                }
                None
            }
            WM_SIZE => {
                if wparam.0 == SIZE_MINIMIZED as usize {
                    // 窗口最小化冷路径立即修剪显存与物理工作集
                    self.trim_idle_memory();
                    return Some(LRESULT(0));
                }
                if self.suppress_size_render {
                    return Some(LRESULT(0));
                }
                let phys_w = (lparam.0 & 0xFFFF) as u32;
                let phys_h = ((lparam.0 >> 16) & 0xFFFF) as u32;
                if phys_w > 0 && phys_h > 0 {
                    let cur_dpi = unsafe { GetDpiForWindow(hwnd).max(96) as f32 };
                    let old_view_h = self.view_h;
                    self.dpi = cur_dpi;
                    self.camera.set_dpi_scale(cur_dpi / 96.0);
                    self.view_w = phys_w as f32 * 96.0 / self.dpi;
                    self.view_h = phys_h as f32 * 96.0 / self.dpi;
                    if self.is_windowed() && !self.transitioning_window_mode {
                        unsafe {
                            let zoomed = IsZoomed(hwnd).as_bool();
                            self.saved_windowed_maximized = zoomed;
                            if !zoomed {
                                let mut rc = RECT::default();
                                if GetWindowRect(hwnd, &mut rc).is_ok() {
                                    self.saved_windowed_rect = Some(rc);
                                }
                            }
                        }
                    } else if self.presented_frames > 0 {
                        let dy_comp = (old_view_h - self.view_h) * 0.5;
                        if dy_comp.abs() > 0.1 {
                            self.camera.pan_y += dy_comp;
                            if !self.camera.is_fit_mode {
                                self.camera.target_pan_y += dy_comp;
                            }
                        }
                    }
                    let mut need_recreate = false;
                    if let Some(r) = self.renderer.as_mut() {
                        if let Err(RenderError::DeviceLost(_)) = r.resize(phys_w, phys_h, self.dpi)
                        {
                            need_recreate = true;
                        }
                    }
                    if need_recreate {
                        let _ = self.recreate_renderer();
                    }
                    self.memory_trim_dirty = true;
                    let (iw, ih) = self.current_image_dims();
                    if iw > 0.0 && ih > 0.0 {
                        let fit_mode = self.current_fit_mode();
                        if self.camera.is_fit_mode {
                            if self.is_windowed() {
                                // 标准小窗拖动边框缩放时 1:1 瞬时紧贴客户区重排，零滞后拖影
                                self.camera
                                    .snap_to_fit_mode(iw, ih, self.view_w, self.view_h, fit_mode);
                            } else {
                                self.camera
                                    .animate_to_fit_mode(iw, ih, self.view_w, self.view_h, fit_mode);
                                self.ensure_anim_timer();
                            }
                        } else {
                            self.camera
                                .clamp_target_pan(iw, ih, self.view_w, self.view_h);
                            self.ensure_anim_timer();
                        }
                    }
                    self.recompute_layout();
                    self.request_thumbs_for_visible_filmstrip();
                    self.render_now();
                }
                Some(LRESULT(0))
            }
            WM_COPYDATA => {
                // 单实例复用：若正在关闭则拒绝接管，让新进程独立启动；否则先从最小化恢复再无缝切图并置前
                if self.is_closing() || lparam.0 == 0 {
                    return Some(LRESULT(0));
                }
                let cds = unsafe { &*(lparam.0 as *const COPYDATASTRUCT) };
                if cds.dwData != IPC_COPYDATA_OPEN_PATH
                    || cds.lpData.is_null()
                    || cds.cbData < 2
                    || cds.cbData % 2 != 0
                    || cds.cbData > 65536
                {
                    return Some(LRESULT(0));
                }
                let u16_slice = unsafe {
                    std::slice::from_raw_parts(cds.lpData as *const u16, (cds.cbData / 2) as usize)
                };
                let end = u16_slice
                    .iter()
                    .position(|&c| c == 0)
                    .unwrap_or(u16_slice.len());
                let path_str = String::from_utf16_lossy(&u16_slice[..end]);
                let p = PathBuf::from(path_str);
                if p.is_file() {
                    unsafe {
                        if IsIconic(hwnd).as_bool() {
                            let _ = ShowWindow(hwnd, SW_RESTORE);
                        }
                    }
                    self.open_selected_or_dropped_file(p);
                    unsafe {
                        let _ = SetForegroundWindow(hwnd);
                    }
                    return Some(LRESULT(1));
                }
                Some(LRESULT(0))
            }
            WM_CAPTURECHANGED => {
                // Alt+Tab / UAC / 外部弹窗强制夺走鼠标捕获时，立即清空所有拖拽/按压/长按连发状态
                self.clear_pointer_captures_without_release();
                self.render_now();
                Some(LRESULT(0))
            }
            WM_KILLFOCUS | WM_CANCELMODE => {
                self.clear_pointer_captures();
                self.render_now();
                Some(LRESULT(0))
            }
            WM_DPICHANGED => {
                let new_dpi = ((wparam.0 >> 16) & 0xFFFF) as f32;
                if self.is_windowed() {
                    let clamped_dpi = new_dpi.max(96.0);
                    self.dpi = clamped_dpi;
                    self.camera.set_dpi_scale(clamped_dpi / 96.0);
                    unsafe {
                        self.apply_dpi_matched_icons(hwnd, clamped_dpi as u32);
                        if lparam.0 != 0 {
                            let suggested = *(lparam.0 as *const RECT);
                            let _ = SetWindowPos(
                                hwnd,
                                None,
                                suggested.left,
                                suggested.top,
                                (suggested.right - suggested.left).max(320),
                                (suggested.bottom - suggested.top).max(240),
                                SWP_NOZORDER | SWP_NOACTIVATE,
                            );
                        }
                    }
                } else {
                    if lparam.0 != 0 {
                        unsafe {
                            let suggested = *(lparam.0 as *const RECT);
                            let _ = SetWindowPos(
                                hwnd,
                                None,
                                suggested.left,
                                suggested.top,
                                (suggested.right - suggested.left).max(320),
                                (suggested.bottom - suggested.top).max(240),
                                SWP_NOZORDER | SWP_NOACTIVATE,
                            );
                        }
                    }
                    self.sync_monitor_bounds_and_dpi(hwnd, new_dpi, true);
                }
                Some(LRESULT(0))
            }
            WM_DISPLAYCHANGE => {
                let cur_dpi = unsafe { GetDpiForWindow(hwnd).max(96) as f32 };
                if self.is_windowed() {
                    unsafe {
                        let mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                        let mut mi = MONITORINFO {
                            cbSize: std::mem::size_of::<MONITORINFO>() as u32,
                            ..Default::default()
                        };
                        let mut rc = RECT::default();
                        if GetMonitorInfoW(mon, &mut mi).as_bool()
                            && GetWindowRect(hwnd, &mut rc).is_ok()
                            && !IsZoomed(hwnd).as_bool()
                            && !IsIconic(hwnd).as_bool()
                        {
                            let clamped = clamp_rect_to_work_area(rc, mi.rcWork);
                            let _ = SetWindowPos(
                                hwnd,
                                None,
                                clamped.left,
                                clamped.top,
                                (clamped.right - clamped.left).max(320),
                                (clamped.bottom - clamped.top).max(240),
                                SWP_NOZORDER | SWP_NOACTIVATE,
                            );
                        }
                    }
                } else {
                    self.sync_monitor_bounds_and_dpi(hwnd, cur_dpi, true);
                }
                Some(LRESULT(0))
            }
            WM_MOUSEMOVE => {
                if self.is_closing() {
                    return Some(LRESULT(0));
                }
                let dx = self.to_dip(loword_i16(lparam.0));
                let dy = self.to_dip(hiword_i16(lparam.0));
                let now = Instant::now();

                let cursor_delta =
                    (dx - self.last_cursor_dip.0).hypot(dy - self.last_cursor_dip.1);
                if (!self.is_slideshow() && cursor_delta > 1.5)
                    || (self.is_slideshow() && cursor_delta > 10.0)
                {
                    self.last_cursor_dip = (dx, dy);
                    self.touch_activity();
                }

                if self.dragging_anim_scrubber {
                    self.update_anim_scrubber_drag(dx);
                    return Some(LRESULT(0));
                }
                if self.dragging_minimap {
                    self.update_minimap_drag(dx, dy);
                    return Some(LRESULT(0));
                }
                if self.selecting_zoom_box {
                    self.selection_box =
                        Some(RectF::new(self.selection_start.0, self.selection_start.1, dx, dy));
                    self.render_now();
                    return Some(LRESULT(0));
                }
                if self.pressing_filmstrip {
                    let step_x = dx - self.drag_last_x;
                    if !self.dragging_filmstrip && (dx - self.selection_start.0).abs() > 4.5 {
                        self.dragging_filmstrip = true;
                    }
                    if self.dragging_filmstrip && self.siblings.len() > 1 {
                        self.drag_last_x = dx;
                        let pitch = (FILMSTRIP_ITEM_SIZE_DIP + FILMSTRIP_ITEM_GAP_DIP).max(1.0);
                        self.filmstrip_drag_accum_px -= step_x;
                        let whole_steps = (self.filmstrip_drag_accum_px / pitch).trunc() as isize;
                        if whole_steps != 0 {
                            self.filmstrip_drag_accum_px -= (whole_steps as f32) * pitch;
                            let max_i = (self.siblings.len() - 1) as isize;
                            let target =
                                (self.current_index as isize + whole_steps).clamp(0, max_i) as usize;
                            if target != self.current_index {
                                self.navigate_to_index(target, false);
                            }
                        }
                        self.filmstrip_scroll_offset =
                            (self.filmstrip_drag_accum_px / pitch).clamp(-0.95, 0.95);
                        self.recompute_layout();
                        self.request_thumbs_for_visible_filmstrip();
                        self.render_now();
                    }
                    return Some(LRESULT(0));
                }
                if self.dragging_image {
                    let move_x = dx - self.drag_last_x;
                    let move_y = dy - self.drag_last_y;
                    if move_x.abs() < 1e-4 && move_y.abs() < 1e-4 {
                        return Some(LRESULT(0));
                    }
                    let move_dt = now.duration_since(self.drag_last_move_at).as_secs_f32();
                    if move_x.abs() > 0.8 || move_y.abs() > 0.8 {
                        self.drag_moved = true;
                    }
                    self.drag_last_x = dx;
                    self.drag_last_y = dy;
                    self.drag_last_move_at = now;
                    let (iw, ih) = self.current_image_dims();
                    if iw > 0.0 && ih > 0.0 {
                        self.mark_user_viewport_adjusted();
                        self.camera.pan_by_drag(
                            move_x,
                            move_y,
                            move_dt,
                            iw,
                            ih,
                            self.view_w,
                            self.view_h,
                        );
                        self.recompute_layout();
                        self.render_now();
                    }
                    return Some(LRESULT(0));
                }

                let prev_hover = self.hovered;
                self.hovered =
                    self.layout
                        .hit_test(dx, dy, self.image_screen_rect(), self.ui_opacity > 0.05);
                if prev_hover != self.hovered {
                    self.ensure_anim_timer();
                    self.render_now();
                }
                Some(LRESULT(0))
            }
            WM_LBUTTONDOWN => {
                if self.is_closing() {
                    return Some(LRESULT(0));
                }
                let dx = self.to_dip(loword_i16(lparam.0));
                let dy = self.to_dip(hiword_i16(lparam.0));
                let hit =
                    self.layout
                        .hit_test(dx, dy, self.image_screen_rect(), self.ui_opacity > 0.05);
                self.hovered = hit;
                self.drag_moved = false;
                let ctrl = unsafe { (GetKeyState(VK_CONTROL.0 as i32) as u16 & 0x8000) != 0 };

                match hit {
                    HitTarget::ToolbarButton(act) => {
                        if matches!(
                            act,
                            ToolbarAction::ZoomIn
                                | ToolbarAction::ZoomOut
                                | ToolbarAction::PrevImage
                                | ToolbarAction::NextImage
                        ) {
                            let now = Instant::now();
                            self.held_repeat_action = Some((act, now, now));
                            self.ensure_anim_timer();
                            unsafe {
                                SetCapture(hwnd);
                            }
                        }
                        self.execute_action(act);
                    }
                    HitTarget::AnimScrubber => {
                        self.dragging_anim_scrubber = true;
                        unsafe {
                            SetCapture(hwnd);
                        }
                        self.update_anim_scrubber_drag(dx);
                    }
                    HitTarget::FilmstripItem(idx) => {
                        self.pressing_filmstrip = true;
                        self.dragging_filmstrip = false;
                        self.filmstrip_clicked_idx = Some(idx);
                        self.filmstrip_drag_accum_px = 0.0;
                        self.selection_start = (dx, dy);
                        self.drag_last_x = dx;
                        unsafe {
                            SetCapture(hwnd);
                        }
                    }
                    HitTarget::FilmstripBackground => {
                        self.pressing_filmstrip = true;
                        self.dragging_filmstrip = false;
                        self.filmstrip_clicked_idx = None;
                        self.filmstrip_drag_accum_px = 0.0;
                        self.selection_start = (dx, dy);
                        self.drag_last_x = dx;
                        unsafe {
                            SetCapture(hwnd);
                        }
                    }
                    HitTarget::Minimap => {
                        self.dragging_minimap = true;
                        unsafe {
                            SetCapture(hwnd);
                        }
                        self.update_minimap_drag(dx, dy);
                    }
                    HitTarget::ImageCanvas => {
                        if self.is_slideshow() {
                            // 1:1 还原原版 Picasa：幻灯片放映态无底部播控条，单击画面任意处即柔滑亮灯退回常规看图态
                            self.stop_slideshow_gracefully();
                            self.recompute_layout();
                            self.render_now();
                        } else if ctrl {
                            self.selecting_zoom_box = true;
                            self.selection_start = (dx, dy);
                            self.selection_box = Some(RectF::new(dx, dy, dx, dy));
                            unsafe {
                                SetCapture(hwnd);
                            }
                        } else {
                            self.dragging_image = true;
                            self.drag_last_x = dx;
                            self.drag_last_y = dy;
                            self.drag_last_move_at = Instant::now();
                            self.camera.begin_drag();
                            unsafe {
                                SetCapture(hwnd);
                            }
                        }
                    }
                    HitTarget::EmptyBackdrop => {
                        if ctrl && !self.is_slideshow() {
                            self.selecting_zoom_box = true;
                            self.selection_start = (dx, dy);
                            self.selection_box = Some(RectF::new(dx, dy, dx, dy));
                        } else {
                            self.pressing_backdrop = true;
                            self.backdrop_press_start = (dx, dy);
                        }
                        unsafe {
                            SetCapture(hwnd);
                        }
                    }
                    _ => {}
                }
                Some(LRESULT(0))
            }
            WM_LBUTTONUP => {
                if self.is_closing() {
                    return Some(LRESULT(0));
                }
                let dx = self.to_dip(loword_i16(lparam.0));
                let dy = self.to_dip(hiword_i16(lparam.0));

                if self.held_repeat_action.take().is_some() {
                    unsafe {
                        let _ = ReleaseCapture();
                    }
                }

                if self.selecting_zoom_box {
                    self.selecting_zoom_box = false;
                    let (iw, ih) = self.current_image_dims();
                    if let Some(sel) = self.selection_box.take() {
                        if iw > 0.0 && ih > 0.0 {
                            self.mark_user_viewport_adjusted();
                            self.camera
                                .zoom_to_screen_rect(sel, iw, ih, self.view_w, self.view_h);
                            self.show_zoom_badge();
                        }
                    }
                    unsafe {
                        let _ = ReleaseCapture();
                    }
                    self.recompute_layout();
                    self.render_now();
                } else if self.pressing_filmstrip {
                    let was_drag = self.dragging_filmstrip;
                    let clicked = self.filmstrip_clicked_idx.take();
                    self.pressing_filmstrip = false;
                    self.dragging_filmstrip = false;
                    self.filmstrip_drag_accum_px = 0.0;
                    unsafe {
                        let _ = ReleaseCapture();
                    }
                    if !was_drag {
                        if let Some(idx) = clicked {
                            self.navigate_to_index(idx, false);
                        }
                    } else {
                        self.ensure_anim_timer();
                        self.render_now();
                    }
                } else if self.pressing_backdrop {
                    self.pressing_backdrop = false;
                    unsafe {
                        let _ = ReleaseCapture();
                    }
                    let dist = (dx - self.backdrop_press_start.0)
                        .hypot(dy - self.backdrop_press_start.1);
                    if dist <= 6.5 {
                        if self.is_slideshow() {
                            self.stop_slideshow_gracefully();
                            self.recompute_layout();
                            self.render_now();
                        } else if self.window_mode == WindowMode::Overlay {
                            // 1:1 复刻原版 Picasa：浮层态单击图片外空白区自动变为标准可缩放小窗口
                            self.enter_windowed_mode(false);
                        }
                    }
                } else if self.dragging_image {
                    self.dragging_image = false;
                    let stalled = Instant::now().duration_since(self.drag_last_move_at)
                        > Duration::from_millis(45);
                    self.camera.end_drag(stalled);
                    unsafe {
                        let _ = ReleaseCapture();
                    }
                    self.ensure_anim_timer();
                    self.render_now();
                } else if self.dragging_minimap || self.dragging_anim_scrubber {
                    self.dragging_minimap = false;
                    self.dragging_anim_scrubber = false;
                    unsafe {
                        let _ = ReleaseCapture();
                    }
                    self.render_now();
                }
                Some(LRESULT(0))
            }
            WM_LBUTTONDBLCLK => {
                if self.is_closing() {
                    return Some(LRESULT(0));
                }
                let dx = self.to_dip(loword_i16(lparam.0));
                let dy = self.to_dip(hiword_i16(lparam.0));
                let hit =
                    self.layout
                        .hit_test(dx, dy, self.image_screen_rect(), self.ui_opacity > 0.05);
                if hit == HitTarget::ImageCanvas {
                    if self.is_slideshow() {
                        self.stop_slideshow_gracefully();
                    } else {
                        let (iw, ih) = self.current_image_dims();
                        if iw > 0.0 && ih > 0.0 {
                            self.mark_user_viewport_adjusted();
                            self.camera.toggle_actual_or_fit(
                                dx,
                                dy,
                                iw,
                                ih,
                                self.view_w,
                                self.view_h,
                            );
                            self.show_zoom_badge();
                        }
                    }
                    self.recompute_layout();
                    self.render_now();
                }
                Some(LRESULT(0))
            }
            WM_MBUTTONUP => {
                if self.is_windowed() {
                    self.close_immediately();
                } else {
                    self.request_animated_close();
                }
                Some(LRESULT(0))
            }
            WM_RBUTTONUP => {
                if self.is_closing() {
                    return Some(LRESULT(0));
                }
                let mut pt = POINT::default();
                unsafe {
                    let _ = GetCursorPos(&mut pt);
                }
                self.open_context_menu_at_screen(pt.x, pt.y);
                Some(LRESULT(0))
            }
            WM_MOUSEWHEEL => {
                if self.is_closing() {
                    return Some(LRESULT(0));
                }
                let wheel_delta = ((wparam.0 >> 16) as u16 as i16) as f32 / 120.0;
                let mut pt = POINT::default();
                unsafe {
                    let _ = GetCursorPos(&mut pt);
                    let _ = ScreenToClient(hwnd, &mut pt);
                }
                let dx = self.to_dip(pt.x as f32);
                let dy = self.to_dip(pt.y as f32);

                if self.dragging_image {
                    if let Some(img) = self.current_image.as_ref() {
                        if img.is_animated() {
                            self.anim_playing = false;
                            let len = img.frames.len() as isize;
                            let step = if wheel_delta > 0.0 { -1 } else { 1 };
                            self.frame_index =
                                (self.frame_index as isize + step).rem_euclid(len) as usize;
                            // 滚轮步进动图暂停后立即刷新布局同步播放/暂停按钮图标
                            self.recompute_layout();
                            self.render_now();
                            return Some(LRESULT(0));
                        }
                    }
                }

                let hit =
                    self.layout
                        .hit_test(dx, dy, self.image_screen_rect(), self.ui_opacity > 0.05);
                if matches!(
                    hit,
                    HitTarget::FilmstripItem(_)
                        | HitTarget::FilmstripBackground
                        | HitTarget::ToolbarBackground
                ) {
                    let step = if wheel_delta > 0.0 { -1 } else { 1 };
                    self.step_image(step, self.is_slideshow());
                } else {
                    let (iw, ih) = self.current_image_dims();
                    if iw > 0.0 && ih > 0.0 {
                        if self.is_slideshow() {
                            self.stop_slideshow_gracefully();
                        }
                        self.mark_user_viewport_adjusted();
                        self.camera.zoom_at_point(
                            wheel_delta,
                            dx,
                            dy,
                            iw,
                            ih,
                            self.view_w,
                            self.view_h,
                        );
                        self.show_zoom_badge();
                        self.recompute_layout();
                        self.render_now();
                    }
                }
                Some(LRESULT(0))
            }
            WM_KEYDOWN => {
                let vk = wparam.0 as u16;
                if self.is_closing() {
                    if vk == VK_ESCAPE.0 {
                        unsafe {
                            let _ = DestroyWindow(hwnd);
                        }
                    }
                    return Some(LRESULT(0));
                }
                let ctrl = unsafe { (GetKeyState(VK_CONTROL.0 as i32) as u16 & 0x8000) != 0 };
                let shift = unsafe { (GetKeyState(VK_SHIFT.0 as i32) as u16 & 0x8000) != 0 };
                match vk {
                    // 严格两级 Esc 状态机：Slideshow -> Viewing -> Closing！
                    x if x == VK_ESCAPE.0 => {
                        if self.is_slideshow() {
                            self.stop_slideshow_gracefully();
                            self.recompute_layout();
                            self.render_now();
                        } else if self.is_windowed() {
                            self.close_immediately();
                        } else {
                            self.request_animated_close();
                        }
                    }
                    x if x == VK_LEFT.0 || x == b'J' as u16 || x == b'A' as u16 => {
                        self.execute_action(ToolbarAction::PrevImage)
                    }
                    x if x == VK_RIGHT.0 || x == b'K' as u16 || x == b'D' as u16 => {
                        self.execute_action(ToolbarAction::NextImage)
                    }
                    x if x == VK_HOME.0 => self.navigate_to_index(0, self.is_slideshow()),
                    x if x == VK_END.0 && !self.siblings.is_empty() => {
                        self.navigate_to_index(self.siblings.len() - 1, self.is_slideshow())
                    }
                    x if x == VK_OEM_PLUS.0 || x == 0x6B || x == VK_UP.0 => {
                        self.execute_action(ToolbarAction::ZoomIn)
                    }
                    x if x == VK_OEM_MINUS.0 || x == 0x6D || x == VK_DOWN.0 => {
                        self.execute_action(ToolbarAction::ZoomOut)
                    }
                    x if x == VK_PRIOR.0 => {
                        if self.is_slideshow() {
                            self.stop_slideshow_gracefully();
                        }
                        let (iw, ih) = self.current_image_dims();
                        if iw > 0.0 && ih > 0.0 {
                            self.mark_user_viewport_adjusted();
                            self.camera.zoom_at_point(
                                4.0,
                                self.view_w * 0.5,
                                self.view_h * 0.5,
                                iw,
                                ih,
                                self.view_w,
                                self.view_h,
                            );
                            self.show_zoom_badge();
                            self.recompute_layout();
                            self.render_now();
                        }
                    }
                    x if x == VK_NEXT.0 => {
                        if self.is_slideshow() {
                            self.stop_slideshow_gracefully();
                        }
                        let (iw, ih) = self.current_image_dims();
                        if iw > 0.0 && ih > 0.0 {
                            self.mark_user_viewport_adjusted();
                            self.camera.zoom_at_point(
                                -4.0,
                                self.view_w * 0.5,
                                self.view_h * 0.5,
                                iw,
                                ih,
                                self.view_w,
                                self.view_h,
                            );
                            self.show_zoom_badge();
                            self.recompute_layout();
                            self.render_now();
                        }
                    }
                    x if x == b'0' as u16 || x == b'1' as u16 => {
                        self.execute_action(ToolbarAction::ToggleActualSize)
                    }
                    x if x == b'W' as u16 && !ctrl => {
                        self.execute_action(ToolbarAction::FitToWidth)
                    }
                    x if x == b'O' as u16 => {
                        self.execute_action(ToolbarAction::OpenImageFile)
                    }
                    x if x == b'H' as u16 && !ctrl => {
                        self.execute_action(ToolbarAction::FlipHorizontal)
                    }
                    x if x == b'V' as u16 && !ctrl => {
                        self.execute_action(ToolbarAction::FlipVertical)
                    }
                    x if x == VK_F5.0 => self.execute_action(ToolbarAction::ToggleSlideshow),
                    x if x == VK_F11.0 || x == VK_RETURN.0 => {
                        self.execute_action(ToolbarAction::ToggleFullscreen)
                    }
                    x if (x == b'R' as u16 && ctrl && shift) || x == b'Q' as u16 => {
                        self.execute_action(ToolbarAction::RotateCcw)
                    }
                    x if (x == b'R' as u16 && ctrl) || x == b'E' as u16 => {
                        self.execute_action(ToolbarAction::RotateCw)
                    }
                    x if x == b'M' as u16 && ctrl => {
                        self.execute_action(ToolbarAction::RegisterDefaultApp)
                    }
                    x if x == VK_SPACE.0 => {
                        if self.is_slideshow() {
                            self.execute_action(ToolbarAction::SlideshowTogglePause);
                        } else if self
                            .current_image
                            .as_ref()
                            .map(|i| i.is_animated())
                            .unwrap_or(false)
                        {
                            self.execute_action(ToolbarAction::AnimTogglePlay);
                        } else {
                            self.execute_action(ToolbarAction::ToggleSlideshow);
                        }
                    }
                    x if x == VK_OEM_COMMA.0 => self.execute_action(ToolbarAction::AnimPrevFrame),
                    x if x == VK_OEM_PERIOD.0 => self.execute_action(ToolbarAction::AnimNextFrame),
                    x if x == b'S' as u16 && ctrl => {
                        self.execute_action(ToolbarAction::SaveCurrentFramePng)
                    }
                    x if x == b'S' as u16 => self.execute_action(ToolbarAction::AnimCycleSpeed),
                    x if x == b'B' as u16 => self.execute_action(ToolbarAction::CycleBackdrop),
                    x if x == b'I' as u16 => self.execute_action(ToolbarAction::ToggleInfo),
                    x if x == b'F' as u16 => self.execute_action(ToolbarAction::RevealInExplorer),
                    x if x == b'C' as u16 && ctrl && shift => {
                        self.execute_action(ToolbarAction::CopyPath)
                    }
                    x if x == b'C' as u16 && ctrl => {
                        self.execute_action(ToolbarAction::CopyImage)
                    }
                    x if x == VK_DELETE.0 => {
                        self.execute_action(ToolbarAction::DeleteToRecycleBin)
                    }
                    _ => {}
                }
                Some(LRESULT(0))
            }
            WM_DROPFILES => {
                let hdrop = HDROP(wparam.0 as *mut _);
                unsafe {
                    let count = DragQueryFileW(hdrop, 0xFFFF_FFFF, None);
                    for i in 0..count {
                        let len = DragQueryFileW(hdrop, i, None);
                        if len > 0 {
                            let mut buf = vec![0u16; (len + 1) as usize];
                            DragQueryFileW(hdrop, i, Some(&mut buf));
                            let s = String::from_utf16_lossy(&buf[..len as usize]);
                            let p = PathBuf::from(s);
                            if is_supported_image_path(&p) || p.is_file() {
                                self.open_selected_or_dropped_file(p);
                                break;
                            }
                        }
                    }
                    DragFinish(hdrop);
                }
                Some(LRESULT(0))
            }
            WM_DESTROY => {
                self.anim_timer_running = false;
                self.clear_pointer_captures_without_release();
                // 销毁最后一对动态加载的私有 HICON，确保零 GDI 句柄泄漏
                self.destroy_owned_icons(hwnd);
                unsafe {
                    PostQuitMessage(0);
                }
                Some(LRESULT(0))
            }
            _ => None,
        }
    }

    unsafe fn apply_dpi_matched_icons(&mut self, hwnd: HWND, dpi: u32) {
        let Ok(hmod) = GetModuleHandleW(None) else {
            return;
        };
        let hinst: HINSTANCE = hmod.into();
        let d = dpi.max(96);
        let big_w = GetSystemMetricsForDpi(SM_CXICON, d).max(32);
        let big_h = GetSystemMetricsForDpi(SM_CYICON, d).max(32);
        let sm_w = GetSystemMetricsForDpi(SM_CXSMICON, d).max(16);
        let sm_h = GetSystemMetricsForDpi(SM_CYSMICON, d).max(16);

        let new_big = load_embedded_icon(hinst, big_w, big_h, false);
        if !new_big.is_invalid() {
            let _ = SendMessageW(
                hwnd,
                WM_SETICON,
                WPARAM(ICON_BIG as usize),
                LPARAM(new_big.0 as isize),
            );
            if !self.big_icon.is_invalid() && self.big_icon != new_big {
                let _ = DestroyIcon(self.big_icon);
            }
            self.big_icon = new_big;
        }
        let new_sm = load_embedded_icon(hinst, sm_w, sm_h, false);
        if !new_sm.is_invalid() {
            let _ = SendMessageW(
                hwnd,
                WM_SETICON,
                WPARAM(ICON_SMALL as usize),
                LPARAM(new_sm.0 as isize),
            );
            if !self.small_icon.is_invalid() && self.small_icon != new_sm {
                let _ = DestroyIcon(self.small_icon);
            }
            self.small_icon = new_sm;
        }
    }

    fn destroy_owned_icons(&mut self, hwnd: HWND) {
        unsafe {
            if !self.big_icon.is_invalid() {
                let _ = SendMessageW(hwnd, WM_SETICON, WPARAM(ICON_BIG as usize), LPARAM(0));
                let _ = DestroyIcon(self.big_icon);
                self.big_icon = HICON::default();
            }
            if !self.small_icon.is_invalid() {
                let _ = SendMessageW(hwnd, WM_SETICON, WPARAM(ICON_SMALL as usize), LPARAM(0));
                let _ = DestroyIcon(self.small_icon);
                self.small_icon = HICON::default();
            }
        }
    }
}

unsafe fn load_embedded_icon(hinst: HINSTANCE, cx: i32, cy: i32, shared: bool) -> HICON {
    let flags = if shared {
        LR_DEFAULTCOLOR | LR_SHARED
    } else {
        LR_DEFAULTCOLOR
    };
    let res_id = PCWSTR(1usize as *const u16);
    LoadImageW(hinst, res_id, IMAGE_ICON, cx, cy, flags)
        .map(|h| HICON(h.0))
        .unwrap_or_default()
}

fn create_overlay_window(app: &mut ViewerApp) -> Result<HWND, String> {
    unsafe {
        let hinst: HINSTANCE = GetModuleHandleW(None)
            .map_err(|e| format!("GetModuleHandleW 失败: {e}"))?
            .into();
        let wclass = to_wide(APP_WINDOW_CLASS);
        let wtitle = to_wide(APP_WINDOW_TITLE);

        let wc = WNDCLASSEXW {
            cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
            style: CS_DBLCLKS,
            lpfnWndProc: Some(viewer_wnd_proc),
            hInstance: hinst,
            hIcon: load_embedded_icon(hinst, 0, 0, true),
            hIconSm: load_embedded_icon(hinst, 16, 16, true),
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            lpszClassName: PCWSTR(wclass.as_ptr()),
            ..Default::default()
        };
        RegisterClassExW(&wc);

        let mut cursor = POINT::default();
        let _ = GetCursorPos(&mut cursor);
        let mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        let mut mi = MONITORINFO {
            cbSize: std::mem::size_of::<MONITORINFO>() as u32,
            ..Default::default()
        };
        let (x, y, w, h) = if GetMonitorInfoW(mon, &mut mi).as_bool() {
            let r = if app.is_fullscreen {
                mi.rcMonitor
            } else {
                mi.rcWork
            };
            (
                r.left,
                r.top,
                (r.right - r.left).max(640),
                // 关键工程防坑：严禁等于完整显示器高度导致 Windows DWM 关闭透明穿透变纯黑底！
                if app.is_fullscreen {
                    (r.bottom - r.top - 1).max(480)
                } else {
                    (r.bottom - r.top).max(480)
                },
            )
        } else {
            (0, 0, 1920, 1079)
        };

        // 初始创建时暂不附加 WS_VISIBLE，等 D3D11/DComp 就绪并锚定 Opening 帧后再 ShowWindow，杜绝首帧白闪或漏掉弹开前段！
        let hwnd = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST,
            PCWSTR(wclass.as_ptr()),
            PCWSTR(wtitle.as_ptr()),
            WS_POPUP,
            x,
            y,
            w,
            h,
            None,
            None,
            hinst,
            Some(app as *mut ViewerApp as *const _),
        )
        .map_err(|e| format!("CreateWindowExW 失败: {e}"))?;

        let _ = WS_VISIBLE; // 保留导入兼容
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, app as *mut ViewerApp as isize);
        Ok(hwnd)
    }
}

unsafe extern "system" fn viewer_wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    let ptr = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *mut ViewerApp;
    if !ptr.is_null() {
        if let Some(res) = (*ptr).handle_message(hwnd, msg, wparam, lparam) {
            return res;
        }
    }
    DefWindowProcW(hwnd, msg, wparam, lparam)
}

#[cfg(test)]
mod tests {
    use super::{
        compute_next_filmstrip_offset, compute_window_center_compensation_dip,
        compute_windowed_outer_rect, compute_zoom_badge_alpha, format_window_title,
        next_index_after_delete, resolve_frame_for_export, should_auto_window_on_deactivate,
        should_skip_navigation,
    };
    use crate::constants::{
        WINDOWED_MAX_WORKAREA_RATIO, WINDOWED_MIN_CLIENT_H_DIP, WINDOWED_MIN_CLIENT_W_DIP,
        ZOOM_BADGE_FADE_MS, ZOOM_BADGE_HOLD_MS,
    };
    use crate::types::{ImageFrame, LifecyclePhase, RectF, WindowMode};
    use std::path::Path;
    use std::time::{Duration, Instant};
    use windows::Win32::Foundation::RECT;

    #[test]
    fn zoom_badge_holds_then_fades_cleanly() {
        let t0 = Instant::now();
        assert!((compute_zoom_badge_alpha(None, t0) - 0.0).abs() < 1e-6);
        assert!((compute_zoom_badge_alpha(Some(t0), t0 + Duration::from_millis(200)) - 1.0).abs() < 1e-6);
        let mid = compute_zoom_badge_alpha(
            Some(t0),
            t0 + Duration::from_millis(ZOOM_BADGE_HOLD_MS + ZOOM_BADGE_FADE_MS / 2),
        );
        assert!(mid > 0.05 && mid < 0.95, "淡出中段应在 (0,1) 之间，实际 {mid}");
        let done = compute_zoom_badge_alpha(
            Some(t0),
            t0 + Duration::from_millis(ZOOM_BADGE_HOLD_MS + ZOOM_BADGE_FADE_MS + 10),
        );
        assert!((done - 0.0).abs() < 1e-6);
    }

    #[test]
    fn rapid_hold_left_or_right_never_flings_filmstrip_off_center() {
        for len in [2usize, 4, 12] {
            for step in [-1isize, 1] {
                let mut cur = 0usize;
                let mut offset = 0.0f32;
                for _ in 0..40 {
                    offset *= 0.65;
                    let next = (cur as isize + step).rem_euclid(len as isize) as usize;
                    offset = compute_next_filmstrip_offset(cur, offset, next, len);
                    cur = next;
                    let anchor = cur as f32 + offset;
                    assert!(
                        offset.abs() <= 1.5001,
                        "len={len} step={step}: offset 漂移越界: {offset}"
                    );
                    assert!(
                        anchor >= -0.0001 && anchor <= (len - 1) as f32 + 0.0001,
                        "len={len} step={step}: 视觉锚点越出缩略图范围: anchor={anchor}"
                    );
                }
            }
        }
    }

    #[test]
    fn delete_non_last_image_never_freezes_navigation() {
        // 3 张图删除索引 0 或索引 1 后，目标索引不变，但已加载图像为空或路径已变化，绝不能跳过导航
        let mut siblings = vec![
            Path::new("a.png").to_path_buf(),
            Path::new("b.png").to_path_buf(),
            Path::new("c.png").to_path_buf(),
        ];
        // 删除首图 (index 0)
        siblings.remove(0);
        let next = next_index_after_delete(siblings.len(), 0).expect("仍有剩余图片");
        assert_eq!(next, 0);
        // 清空 current_image 后必不跳过
        assert!(!should_skip_navigation(0, next, None, Some(siblings[next].as_path())));
        // 即使历史 current_image 尚未清空（仍指向 a.png），因路径与新 siblings[0] (b.png) 不同也绝不跳过
        assert!(!should_skip_navigation(
            0,
            next,
            Some(Path::new("a.png")),
            Some(siblings[next].as_path())
        ));
        // 仅当索引和路径双双匹配时才跳过重复加载
        assert!(should_skip_navigation(
            0,
            0,
            Some(Path::new("b.png")),
            Some(siblings[0].as_path())
        ));

        // 删除末尾图 (index 1 -> 回退到 index 0)
        siblings.remove(1);
        assert_eq!(next_index_after_delete(siblings.len(), 1), Some(0));

        // 删除唯一剩余图 -> None (关闭窗口)
        siblings.remove(0);
        assert_eq!(next_index_after_delete(siblings.len(), 0), None);
    }

    #[test]
    fn window_center_compensation_handles_all_four_taskbar_edges() {
        let full = RECT {
            left: 0,
            top: 0,
            right: 1920,
            bottom: 1080,
        };
        // 任务栏在底部 (高 48px)
        let work_bottom = RECT { left: 0, top: 0, right: 1920, bottom: 1032 };
        let (dx, dy) = compute_window_center_compensation_dip(work_bottom, full, 96.0);
        assert!((dx - 0.0).abs() < 1e-3 && (dy - (-24.0)).abs() < 1e-3);

        // 任务栏在顶部 (高 48px)
        let work_top = RECT { left: 0, top: 48, right: 1920, bottom: 1080 };
        let (dx, dy) = compute_window_center_compensation_dip(work_top, full, 96.0);
        assert!((dx - 0.0).abs() < 1e-3 && (dy - 24.0).abs() < 1e-3);

        // 任务栏在左侧 (宽 80px，150% DPI = 144)
        let work_left = RECT { left: 80, top: 0, right: 1920, bottom: 1080 };
        let (dx, dy) = compute_window_center_compensation_dip(work_left, full, 144.0);
        assert!((dx - (40.0 / 1.5)).abs() < 1e-2 && (dy - 0.0).abs() < 1e-3);

        // 任务栏在右侧 (宽 80px)
        let work_right = RECT { left: 0, top: 0, right: 1840, bottom: 1080 };
        let (dx, dy) = compute_window_center_compensation_dip(work_right, full, 96.0);
        assert!((dx - (-40.0)).abs() < 1e-3 && (dy - 0.0).abs() < 1e-3);
    }

    #[test]
    fn windowed_outer_rect_respects_min_max_aspect_and_workarea_clamp() {
        let work = RECT {
            left: 0,
            top: 0,
            right: 1920,
            bottom: 1032,
        };
        let insets = (8, 31, 8, 8);

        // 1. 极小图片客户区应受 540x420 DIP 下限保护，防止底栏按钮裁切
        let small_rc = RectF::new(860.0, 416.0, 1060.0, 616.0);
        let out_small = compute_windowed_outer_rect(small_rc, (0, 0), work, 96.0, insets);
        let client_w_small = (out_small.right - out_small.left) - (insets.0 + insets.2);
        let client_h_small = (out_small.bottom - out_small.top) - (insets.1 + insets.3);
        assert!(client_w_small >= WINDOWED_MIN_CLIENT_W_DIP as i32);
        assert!(client_h_small >= WINDOWED_MIN_CLIENT_H_DIP as i32);

        // 2. 超大图/放大图外框不得超过工作区 86%
        let huge_rc = RectF::new(-500.0, -300.0, 2600.0, 1400.0);
        let out_huge = compute_windowed_outer_rect(huge_rc, (0, 0), work, 96.0, insets);
        let max_w = (1920.0 * WINDOWED_MAX_WORKAREA_RATIO).ceil() as i32 + 2;
        let max_h = (1032.0 * WINDOWED_MAX_WORKAREA_RATIO).ceil() as i32 + 2;
        assert!(out_huge.right - out_huge.left <= max_w);
        assert!(out_huge.bottom - out_huge.top <= max_h);
        assert!(out_huge.left >= work.left && out_huge.right <= work.right);
        assert!(out_huge.top >= work.top && out_huge.bottom <= work.bottom);

        // 3. 拖拽到屏幕极右侧后切小窗，应贴靠右侧且严格夹紧在工作区内不越屏
        let dragged_right = RectF::new(1500.0, 200.0, 2200.0, 900.0);
        let out_drag = compute_windowed_outer_rect(dragged_right, (0, 0), work, 96.0, insets);
        assert!(out_drag.right <= work.right);
        assert!(out_drag.left >= work.left);
        assert!(out_drag.top >= work.top && out_drag.bottom <= work.bottom);
    }

    #[test]
    fn auto_window_on_deactivate_state_machine_respects_all_guards() {
        // 正常浮层态且已激活、非模态弹窗 -> 失焦自动切小窗
        assert!(should_auto_window_on_deactivate(
            WindowMode::Overlay,
            LifecyclePhase::Viewing,
            false,
            true,
            false,
        ));
        assert!(should_auto_window_on_deactivate(
            WindowMode::Overlay,
            LifecyclePhase::Opening,
            false,
            true,
            false,
        ));
        // 模态菜单/对话框弹出期间绝不误触发切小窗
        assert!(!should_auto_window_on_deactivate(
            WindowMode::Overlay,
            LifecyclePhase::Viewing,
            true,
            true,
            false,
        ));
        // 幻灯片、关闭中、已在小窗口态、尚未首次激活、正在模式切换中 -> 一律不触发
        assert!(!should_auto_window_on_deactivate(
            WindowMode::Overlay,
            LifecyclePhase::Slideshow,
            false,
            true,
            false,
        ));
        assert!(!should_auto_window_on_deactivate(
            WindowMode::Overlay,
            LifecyclePhase::Closing,
            false,
            true,
            false,
        ));
        assert!(!should_auto_window_on_deactivate(
            WindowMode::Windowed,
            LifecyclePhase::Viewing,
            false,
            true,
            false,
        ));
        assert!(!should_auto_window_on_deactivate(
            WindowMode::Overlay,
            LifecyclePhase::Viewing,
            false,
            false,
            false,
        ));
        assert!(!should_auto_window_on_deactivate(
            WindowMode::Overlay,
            LifecyclePhase::Viewing,
            false,
            true,
            true,
        ));
    }

    #[test]
    fn window_title_formats_filename_cleanly() {
        assert_eq!(
            format_window_title(Some(Path::new(r"C:\Photos\image3.png"))),
            "image3.png - Tools3000 Picasa Viewer"
        );
        assert_eq!(format_window_title(None), "Tools3000 Picasa Viewer");
    }

    #[test]
    fn resolve_frame_for_export_redecodes_from_disk_after_cpu_pixels_released() {
        let dir = std::env::temp_dir().join("t3k_picasa_redecode_export_test");
        let _ = std::fs::create_dir_all(&dir);
        let file = dir.join("tiny_export.png");
        let src_frame = ImageFrame {
            width: 4,
            height: 4,
            delay_ms: 0,
            bgra_premul: vec![120, 80, 200, 255].repeat(16),
        };
        crate::integration::save_premul_bgra_as_png(&src_frame, &file).expect("保存测试 PNG");

        let decoded = crate::decode::decode_image_file(&file).expect("首次解码成功");
        assert!(decoded.has_cpu_pixels(0));
        assert!(decoded.memory_bytes() == 4 * 4 * 4);

        // 模拟静态图上传 GPU 后释放 CPU 像素缓冲
        let gpu_only = decoded.clone_metadata_without_pixels();
        assert!(!gpu_only.has_cpu_pixels(0));
        assert_eq!(gpu_only.memory_bytes(), 0);
        assert_eq!(gpu_only.width, 4);
        assert_eq!(gpu_only.height, 4);

        // 剪贴板复制 / PNG 导出无缝按需重解，像素 100% 一致
        let resolved = resolve_frame_for_export(&gpu_only, 0).expect("按需重解成功");
        assert_eq!(resolved.width, 4);
        assert_eq!(resolved.height, 4);
        assert_eq!(resolved.bgra_premul.len(), 64);
        assert_eq!(&resolved.bgra_premul[0..4], &[120, 80, 200, 255]);

        let _ = std::fs::remove_file(&file);
        let _ = std::fs::remove_dir(&dir);
    }
}
