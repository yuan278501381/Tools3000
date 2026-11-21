// ─────────────────────────────────────────────────────────────────────────────
// app.rs — 独立看图器交互状态机与自适应高刷事件循环
//   - 四态生命周期：`Opening -> Viewing <-> Slideshow -> Closing`
//   - Windows DWM `monitor_height - 1` 真实桌面穿透 · Per-Monitor V2 高分屏缩放
//   - 自适应按需重绘调度：动画进行中 VSync 高刷推进，画面静止自动休眠定时器 (0% CPU)
// ─────────────────────────────────────────────────────────────────────────────

use crate::assoc_dialog::{show_file_association_dialog, AssocDialogOutcome};
use crate::cache::{ImageCachePool, WM_VIEWER_IMAGE_READY, WM_VIEWER_THUMB_READY};
use crate::constants::{
    ANIM_SPEED_PRESETS, APP_WINDOW_CLASS, APP_WINDOW_TITLE, AUTOREPEAT_INITIAL_DELAY_MS,
    AUTOREPEAT_INTERVAL_MS, CINEMA_DIM_DAMPING, CLOSE_ANIM_DURATION_SEC, CLOSE_END_SCALE_RATIO,
    FILMSTRIP_ITEM_GAP_DIP, FILMSTRIP_ITEM_SIZE_DIP, FILMSTRIP_SCROLL_DAMPING, HOVER_ANIM_DAMPING,
    LOADING_INDICATOR_DELAY_MS, OPEN_ANIM_DURATION_SEC, SLIDESHOW_AUTOHIDE_MS,
    SLIDESHOW_CROSSFADE_DURATION_SEC, SLIDESHOW_INTERVAL_MS, SLIDE_FAST_REPEAT_DURATION_SEC,
    SLIDE_TRANSITION_DURATION_SEC, TOAST_DURATION_MS, UI_AUTOHIDE_MS,
};
use crate::folder::{is_supported_image_path, scan_sibling_images};
use crate::i18n::ViewerLanguage;
use crate::integration::{
    copy_frame_and_file_to_clipboard, copy_path_to_clipboard, move_to_recycle_bin,
    open_with_system_dialog, reveal_in_explorer, save_frame_as_png_dialog,
    set_as_desktop_wallpaper, show_native_context_menu, ContextMenuState,
};
use crate::physics::{exp_smooth, KenBurnsTrack};
use crate::renderer::{RenderFrameParams, ViewerRenderer};
use crate::types::{
    BackdropMode, CloseImplodeSnapshot, LifecyclePhase, RectF, SharedImage, ToolbarAction,
};
use crate::ui_layout::{HitTarget, LayoutInput, ViewerUiLayout};
use crate::viewport::ViewportCamera;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};
use windows::core::PCWSTR;
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    GetMonitorInfoW, MonitorFromPoint, MonitorFromWindow, ScreenToClient, MONITORINFO,
    MONITOR_DEFAULTTONEAREST,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::HiDpi::{
    GetDpiForWindow, GetSystemMetricsForDpi, SetProcessDpiAwarenessContext,
    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2,
};
use windows::Win32::UI::Input::KeyboardAndMouse::{
    GetKeyState, ReleaseCapture, SetCapture, VK_CONTROL, VK_DELETE, VK_DOWN, VK_END, VK_ESCAPE,
    VK_F11, VK_F5, VK_HOME, VK_LEFT, VK_NEXT, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
    VK_OEM_PLUS, VK_PRIOR, VK_RETURN, VK_RIGHT, VK_SHIFT, VK_SPACE, VK_UP,
};
use windows::Win32::UI::Shell::{DragAcceptFiles, DragFinish, DragQueryFileW, HDROP};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DefWindowProcW, DestroyIcon, DestroyWindow, DispatchMessageW, GetClientRect,
    GetCursorPos, GetMessageW, GetWindowLongPtrW, KillTimer, LoadCursorW, LoadImageW,
    PostQuitMessage, RegisterClassExW, SendMessageW, SetForegroundWindow, SetTimer,
    SetWindowLongPtrW, SetWindowPos, ShowWindow, TranslateMessage, CS_DBLCLKS, GWLP_USERDATA,
    HICON, ICON_BIG, ICON_SMALL, IDC_ARROW, IMAGE_ICON, LR_DEFAULTCOLOR, LR_SHARED, MSG,
    SM_CXICON, SM_CXSMICON, SM_CYICON, SM_CYSMICON, SWP_NOACTIVATE, SWP_NOZORDER, SW_SHOW,
    WM_DESTROY, WM_DISPLAYCHANGE, WM_DPICHANGED, WM_DROPFILES, WM_KEYDOWN, WM_LBUTTONDBLCLK,
    WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MBUTTONUP, WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_PAINT,
    WM_RBUTTONUP, WM_SETICON, WM_SIZE, WM_TIMER, WNDCLASSEXW, WS_EX_NOREDIRECTIONBITMAP,
    WS_EX_TOPMOST, WS_POPUP, WS_VISIBLE,
};

const TIMER_ANIM_ID: usize = 1;
const TIMER_IDLE_HIDE_ID: usize = 2;
const TIMER_ANIM_MS: u32 = 8;

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

fn slideshow_ken_burns_span_sec() -> f32 {
    (SLIDESHOW_INTERVAL_MS as f32 / 1000.0) + SLIDESHOW_CROSSFADE_DURATION_SEC
}

pub struct ViewerApp {
    hwnd: HWND,
    lang: ViewerLanguage,
    dpi: f32,
    view_w: f32,
    view_h: f32,
    is_fullscreen: bool,
    renderer: Option<ViewerRenderer>,
    cache: ImageCachePool,

    siblings: Vec<PathBuf>,
    current_index: usize,
    current_image: Option<SharedImage>,
    current_error: Option<String>,
    loading_since: Instant,
    camera: ViewportCamera,
    backdrop: BackdropMode,
    layout: ViewerUiLayout,

    // 四态生命周期与 6 大灵魂物理动效状态
    phase: LifecyclePhase,
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
    slideshow_paused: bool,
    slideshow_step_accum_ms: f32,
    ken_burns: Option<KenBurnsTrack>,

    close_hover_anim: f32,
    play_hover_anim: f32,

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
            let _ = cache.get_or_request_thumb(first_path);
        }

        let now = Instant::now();
        let mut app = Box::new(Self {
            hwnd: HWND::default(),
            lang,
            dpi: 96.0,
            view_w: 1280.0,
            view_h: 720.0,
            is_fullscreen: true,
            renderer: None,
            cache,
            siblings,
            current_index,
            current_image: None,
            current_error: None,
            loading_since: now,
            camera: ViewportCamera::default(),
            backdrop: BackdropMode::PicasaGlass,
            layout: ViewerUiLayout::default(),
            phase: LifecyclePhase::Opening,
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
            slideshow_paused: false,
            slideshow_step_accum_ms: 0.0,
            ken_burns: None,
            close_hover_anim: 0.0,
            play_hover_anim: 0.0,
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
            apply_dpi_matched_window_icons(hwnd, dpi as u32);
            let mut rc = RECT::default();
            let _ = GetClientRect(hwnd, &mut rc);
            let phys_w = (rc.right - rc.left).max(1) as u32;
            let phys_h = (rc.bottom - rc.top).max(1) as u32;
            app.view_w = phys_w as f32 * 96.0 / dpi;
            app.view_h = phys_h as f32 * 96.0 / dpi;
            app.renderer = Some(ViewerRenderer::new(hwnd, phys_w, phys_h, dpi, lang)?);
            DragAcceptFiles(hwnd, true);
        }

        // 给后台解码线程最多 18ms 超短快取窗口：本地常规图片首帧直接高清命中，超大图则拿到缩略图兜底
        let wait_deadline = Instant::now() + Duration::from_millis(18);
        while Instant::now() < wait_deadline {
            if let Some(p) = app.current_path() {
                if app.cache.get_cached_image(p).is_some() {
                    break;
                }
            }
            std::thread::sleep(Duration::from_millis(2));
        }

        // 在窗口真正显现的第一帧锚定 Opening 起跳时钟，确保用户 100% 看到完整的 0.35x -> 1.0x 清脆弹入！
        let show_instant = Instant::now();
        app.open_started_at = show_instant;
        app.last_tick = show_instant;
        app.last_activity = show_instant;
        app.sync_current_from_cache(true);
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
            while GetMessageW(&mut msg, None, 0, 0).as_bool() {
                let _ = TranslateMessage(&msg);
                DispatchMessageW(&msg);
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

    fn ensure_anim_timer(&mut self) {
        if !self.hwnd.is_invalid() && !self.anim_timer_running {
            unsafe {
                SetTimer(self.hwnd, TIMER_ANIM_ID, TIMER_ANIM_MS, None);
            }
            self.anim_timer_running = true;
            self.last_tick = Instant::now();
        }
    }

    fn stop_anim_timer(&mut self) {
        if !self.hwnd.is_invalid() && self.anim_timer_running {
            unsafe {
                let _ = KillTimer(self.hwnd, TIMER_ANIM_ID);
            }
            self.anim_timer_running = false;
        }
    }

    fn schedule_idle_autohide_timer(&self, delay_ms: u32) {
        if !self.hwnd.is_invalid() {
            unsafe {
                SetTimer(self.hwnd, TIMER_IDLE_HIDE_ID, delay_ms.max(50), None);
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

    fn touch_activity(&mut self) {
        self.last_activity = Instant::now();
        if self.ui_opacity < 0.999 {
            self.ensure_anim_timer();
        } else {
            let timeout = if self.is_slideshow() {
                SLIDESHOW_AUTOHIDE_MS as u32
            } else {
                UI_AUTOHIDE_MS as u32
            };
            self.schedule_idle_autohide_timer(timeout);
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
                        self.camera.reset_to_fit_with_mode(
                            img.width as f32,
                            img.height as f32,
                            self.view_w,
                            self.view_h,
                            self.is_slideshow(),
                        );
                        self.frame_index = 0;
                        self.last_frame_step = Instant::now();
                        self.anim_playing = true;
                        if let Some(r) = self.renderer.as_mut() {
                            r.clear_main_bitmap();
                        }
                        if img.is_animated() {
                            self.ensure_anim_timer();
                        }
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

    fn clear_pointer_captures(&mut self) {
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
        unsafe {
            let _ = ReleaseCapture();
        }
    }

    fn enter_slideshow(&mut self) {
        self.phase = LifecyclePhase::Slideshow;
        self.slideshow_paused = false;
        self.slideshow_step_accum_ms = 0.0;
        self.ken_burns = Some(KenBurnsTrack::for_slide_index(
            self.current_index,
            slideshow_ken_burns_span_sec(),
        ));
        self.toast = None;
        self.hovered = HitTarget::None;
        // 入场即收起常规底栏，沉浸进入 #000000 电影放映厅；移动鼠标再唤醒迷你水晶播控胶囊
        self.last_activity = Instant::now() - Duration::from_secs(10);
        if let Some(img) = self.current_image.as_ref() {
            self.camera.animate_to_fit_mode(
                img.width as f32,
                img.height as f32,
                self.view_w,
                self.view_h,
                true,
            );
        }
        self.ensure_anim_timer();
    }

    /// 第一级 `Esc`：从 `Slideshow` 平滑退回 `Viewing` 半透明看图态，绝不直接退出整个程序！
    fn stop_slideshow_gracefully(&mut self) {
        if !self.is_slideshow() {
            return;
        }
        self.phase = LifecyclePhase::Viewing;
        self.slideshow_paused = false;
        self.ken_burns = None;
        self.hovered = HitTarget::None;
        self.last_activity = Instant::now();
        self.schedule_idle_autohide_timer(UI_AUTOHIDE_MS as u32);
        if let Some(img) = self.current_image.as_ref() {
            self.camera.animate_to_fit_mode(
                img.width as f32,
                img.height as f32,
                self.view_w,
                self.view_h,
                false,
            );
        }
        self.ensure_anim_timer();
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
        if target == old_index && self.current_image.is_some() {
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
        // 零拷贝接管旧图 GPU 纹理与当时的 Ken Burns 推镜状态，驱动 140ms 微位移或 900ms 电影级双图层叠化
        if let Some(prev_img) = self.current_image.as_ref() {
            if let Some(r) = self.renderer.as_mut() {
                let outgoing_kb = if in_slideshow { self.ken_burns } else { None };
                r.capture_outgoing_for_slide(&self.camera, prev_img, outgoing_kb);
            }
        }

        self.current_index = target;
        self.loading_since = now;
        self.slideshow_step_accum_ms = 0.0;
        if in_slideshow {
            self.ken_burns = Some(KenBurnsTrack::for_slide_index(
                target,
                slideshow_ken_burns_span_sec(),
            ));
        }
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
        if let Some(path) = self.current_path().cloned() {
            if self.cache.get_cached_image(&path).is_some() {
                self.sync_current_from_cache(true);
            } else {
                if let Some(tb) = self.cache.get_or_request_thumb(&path) {
                    if let Some(r) = self.renderer.as_mut() {
                        r.ensure_thumb_uploaded(&path, &tb);
                    }
                }
                self.camera
                    .reset_to_fit_with_mode(1920.0, 1080.0, self.view_w, self.view_h, in_slideshow);
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
        self.siblings = siblings;
        if let Some(r) = self.renderer.as_mut() {
            r.clear_outgoing_slide();
            r.reset_filmstrip_birth_clock();
        }
        let now = Instant::now();
        self.phase = LifecyclePhase::Opening;
        self.current_image = None;
        self.loading_since = now;
        self.open_started_at = now;
        self.open_progress = 0.0;
        self.filmstrip_scroll_offset = 0.0;
        self.slide_progress = 1.0;
        self.slide_dir = 0.0;
        self.slide_is_crossfade = false;
        self.slide_fast_repeat = false;
        self.slideshow_paused = false;
        self.ken_burns = None;
        self.cinema_dim = 0.0;
        self.current_index = idx;
        self.current_error = None;
        self.frame_index = 0;
        self.cache
            .request_image_and_neighbors(&self.siblings, self.current_index);
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
            slideshow_paused: self.slideshow_paused,
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
        for idx in indices {
            if let Some(path) = self.siblings.get(idx) {
                if let Some(thumb) = self.cache.get_or_request_thumb(path) {
                    if let Some(r) = self.renderer.as_mut() {
                        r.ensure_thumb_uploaded(path, &thumb);
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
            is_fullscreen: self.is_fullscreen,
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
        if let Some(act) = show_native_context_menu(self.hwnd, screen_x, screen_y, &st) {
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
                if self.siblings.len() <= 1 {
                    self.touch_activity();
                    self.show_toast(s.toast_single_image_no_slideshow);
                } else if self.is_slideshow() {
                    self.stop_slideshow_gracefully();
                } else {
                    self.enter_slideshow();
                }
            }
            ToolbarAction::SlideshowTogglePause => {
                if self.is_slideshow() {
                    self.slideshow_paused = !self.slideshow_paused;
                    self.touch_activity();
                    self.ensure_anim_timer();
                }
            }
            ToolbarAction::ExitSlideshow => {
                self.stop_slideshow_gracefully();
            }
            ToolbarAction::OpenImageFile => {
                self.clear_pointer_captures();
                if let Some(picked) = crate::pick_image_via_dialog(self.hwnd, self.lang) {
                    self.open_selected_or_dropped_file(picked);
                    return;
                }
                self.last_tick = Instant::now();
            }
            ToolbarAction::ZoomOut => {
                self.touch_activity();
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.camera.zoom_at_point(
                        -1.5,
                        self.view_w * 0.5,
                        self.view_h * 0.5,
                        iw,
                        ih,
                        self.view_w,
                        self.view_h,
                    );
                    self.ensure_anim_timer();
                }
            }
            ToolbarAction::ZoomIn => {
                self.touch_activity();
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.camera.zoom_at_point(
                        1.5,
                        self.view_w * 0.5,
                        self.view_h * 0.5,
                        iw,
                        ih,
                        self.view_w,
                        self.view_h,
                    );
                    self.ensure_anim_timer();
                }
            }
            ToolbarAction::ToggleActualSize => {
                self.touch_activity();
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.camera.toggle_actual_or_fit(
                        self.view_w * 0.5,
                        self.view_h * 0.5,
                        iw,
                        ih,
                        self.view_w,
                        self.view_h,
                    );
                    self.ensure_anim_timer();
                }
            }
            ToolbarAction::FitToWidth => {
                self.touch_activity();
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.camera.fit_to_width(iw, ih, self.view_w, self.view_h);
                    self.ensure_anim_timer();
                }
            }
            ToolbarAction::RotateCcw => {
                self.touch_activity();
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.camera.rotate_step(
                        false,
                        iw,
                        ih,
                        self.view_w,
                        self.view_h,
                        self.is_slideshow(),
                    );
                    self.ensure_anim_timer();
                }
            }
            ToolbarAction::RotateCw => {
                self.touch_activity();
                let (iw, ih) = self.current_image_dims();
                if iw > 0.0 && ih > 0.0 {
                    self.camera.rotate_step(
                        true,
                        iw,
                        ih,
                        self.view_w,
                        self.view_h,
                        self.is_slideshow(),
                    );
                    self.ensure_anim_timer();
                }
            }
            ToolbarAction::FlipHorizontal => {
                self.touch_activity();
                self.camera.toggle_flip_horizontal();
                self.ensure_anim_timer();
            }
            ToolbarAction::FlipVertical => {
                self.touch_activity();
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
                    match save_frame_as_png_dialog(
                        self.hwnd,
                        &img.frames[safe_idx],
                        self.camera.rotation,
                        self.camera.flip_x,
                        self.camera.flip_y,
                        &path,
                        safe_idx,
                        img.is_animated(),
                        self.lang,
                    ) {
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
                self.last_tick = Instant::now();
            }
            ToolbarAction::CopyImage => {
                self.touch_activity();
                if let (Some(img), Some(path)) = (self.current_image.as_ref(), self.current_path())
                {
                    let safe_idx = self.frame_index.min(img.frames.len().saturating_sub(1));
                    if copy_frame_and_file_to_clipboard(
                        self.hwnd,
                        &img.frames[safe_idx],
                        self.camera.rotation,
                        self.camera.flip_x,
                        self.camera.flip_y,
                        path,
                    ) {
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
                self.is_fullscreen = !self.is_fullscreen;
                self.sync_monitor_bounds_and_dpi(self.hwnd, self.dpi);
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
                if let Some(path) = self.current_path() {
                    open_with_system_dialog(path);
                }
            }
            ToolbarAction::RegisterDefaultApp => {
                self.touch_activity();
                self.clear_pointer_captures();
                match show_file_association_dialog(self.hwnd, self.lang) {
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
                    if move_to_recycle_bin(self.hwnd, &path) {
                        self.cache.invalidate_path(&path);
                        self.siblings.remove(self.current_index);
                        if self.siblings.is_empty() {
                            self.request_animated_close();
                            return;
                        }
                        let next_idx = self.current_index.min(self.siblings.len() - 1);
                        self.navigate_to_index_with_dir(next_idx, Some(1.0), false);
                        self.show_toast(s.toast_moved_to_recycle);
                    } else {
                        self.show_toast(s.toast_recycle_cancelled);
                    }
                }
            }
            ToolbarAction::CloseViewer => {
                self.request_animated_close();
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

        // 2. 打开弹入 (`Opening` -> `Viewing`)
        if self.open_progress < 1.0 {
            let elapsed = now.duration_since(self.open_started_at).as_secs_f32();
            self.open_progress = (elapsed / OPEN_ANIM_DURATION_SEC).min(1.0);
            dirty = true;
            if self.open_progress >= 1.0 && self.phase == LifecyclePhase::Opening {
                self.phase = LifecyclePhase::Viewing;
            } else {
                keep_ticking = true;
            }
        }

        if let Some(r) = self.renderer.as_ref() {
            if r.has_active_thumb_growth(now, self.open_started_at) {
                dirty = true;
                keep_ticking = true;
            }
        }

        // 3. 切图微位移 (140ms) / 幻灯片双缓冲交叉叠化 (900ms)
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

        // 4. 幻灯片 350ms 暗房沉降与电影级 Ken Burns 微推镜
        let target_dim = if self.is_slideshow() { 1.0 } else { 0.0 };
        if (self.cinema_dim - target_dim).abs() > 0.002 {
            self.cinema_dim = exp_smooth(self.cinema_dim, target_dim, CINEMA_DIM_DAMPING, dt);
            dirty = true;
            keep_ticking = true;
        } else if self.cinema_dim != target_dim {
            self.cinema_dim = target_dim;
            dirty = true;
        }

        if self.is_slideshow() {
            keep_ticking = true;
            if !self.slideshow_paused {
                if let Some(kb) = self.ken_burns.as_mut() {
                    kb.advance(dt);
                }
                if let Some(r) = self.renderer.as_mut() {
                    r.advance_outgoing_ken_burns(dt);
                }
                dirty = true;

                if self.siblings.len() > 1 && self.current_image.is_some() {
                    self.slideshow_step_accum_ms += dt * 1000.0;
                    if self.slideshow_step_accum_ms >= SLIDESHOW_INTERVAL_MS as f32 {
                        self.slideshow_step_accum_ms = 0.0;
                        self.step_image(1, true);
                        return;
                    }
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

        // 10. 2.5s 静止自动净空模式 (Viewing) / 2.0s 播控胶囊自动隐退 (Slideshow)
        let idle_timeout_ms = if self.is_slideshow() {
            SLIDESHOW_AUTOHIDE_MS
        } else {
            UI_AUTOHIDE_MS
        };
        let keep_ui_pinned = self.dragging_anim_scrubber
            || self.dragging_filmstrip
            || (self.ui_opacity > 0.05
                && matches!(
                    self.hovered,
                    HitTarget::ToolbarButton(_)
                        | HitTarget::AnimScrubber
                        | HitTarget::ToolbarBackground
                        | HitTarget::FilmstripItem(_)
                        | HitTarget::FilmstripBackground
                ));

        let target_ui_opacity = if keep_ui_pinned {
            self.last_activity = now;
            1.0
        } else if now.duration_since(self.last_activity) >= Duration::from_millis(idle_timeout_ms) {
            0.0
        } else {
            1.0
        };

        if (self.ui_opacity - target_ui_opacity).abs() > 0.005 {
            let speed = if target_ui_opacity > self.ui_opacity {
                18.0 // 120ms 敏捷唤醒
            } else {
                9.5 // 220ms 柔和净空淡出
            };
            self.ui_opacity = exp_smooth(self.ui_opacity, target_ui_opacity, speed, dt);
            dirty = true;
            keep_ticking = true;
        } else if self.ui_opacity != target_ui_opacity {
            self.ui_opacity = target_ui_opacity;
            dirty = true;
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
        }

        if dirty {
            self.recompute_layout();
            if strip_moved {
                self.request_thumbs_for_visible_filmstrip();
            }
            self.render_now();
        }

        // 自适应休眠：若所有物理动画均已完美收敛，关闭高频 Tick 实现 0.0% 静止 CPU 占用！
        if !keep_ticking {
            self.stop_anim_timer();
            if self.ui_opacity > 0.5 && !keep_ui_pinned {
                let elapsed_ms = now.duration_since(self.last_activity).as_millis() as u64;
                let rem_ms = idle_timeout_ms.saturating_sub(elapsed_ms).max(60) as u32;
                self.schedule_idle_autohide_timer(rem_ms);
            }
        }
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
                self.render_now();
            }
        }
    }

    fn update_minimap_drag(&mut self, dip_x: f32, dip_y: f32) {
        if let (Some(inner), Some(img)) =
            (self.layout.minimap_image_rect, self.current_image.as_ref())
        {
            let u = (dip_x - inner.left) / inner.width().max(1.0);
            let v = (dip_y - inner.top) / inner.height().max(1.0);
            self.camera.focus_normalized_point(
                u,
                v,
                img.width as f32,
                img.height as f32,
                self.view_w,
                self.view_h,
            );
            self.recompute_layout();
            self.render_now();
        }
    }

    fn render_now(&mut self) {
        if self.renderer.is_none() {
            return;
        }
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
        let show_loading_box =
            now.duration_since(self.loading_since) >= Duration::from_millis(LOADING_INDICATOR_DELAY_MS);
        let anim_speed = self.anim_speed();
        let is_slideshow = self.is_slideshow();
        let params = RenderFrameParams {
            now,
            open_started_at: self.open_started_at,
            lang: self.lang,
            view_w: self.view_w,
            view_h: self.view_h,
            dpi: self.dpi,
            open_progress: self.open_progress,
            close_progress: self.close_progress,
            close_snapshot: self.close_snapshot,
            slide_progress: self.slide_progress,
            slide_dir: self.slide_dir,
            slide_is_crossfade: self.slide_is_crossfade,
            slide_fast_repeat: self.slide_fast_repeat,
            cinema_dim: self.cinema_dim,
            ken_burns: self.ken_burns,
            close_hover_anim: self.close_hover_anim,
            play_hover_anim: self.play_hover_anim,
            backdrop: self.backdrop,
            simulated_desktop_bg: None,
            camera: &self.camera,
            image: self.current_image.as_ref(),
            fallback_thumb_path,
            frame_index: self.frame_index,
            error_text: self.current_error.as_deref(),
            loading_name: loading_name.as_deref(),
            show_loading_box,
            current_index: self.current_index,
            sibling_paths: &self.siblings,
            is_slideshow,
            slideshow_paused: self.slideshow_paused,
            layout: &self.layout,
            ui_opacity: self.ui_opacity,
            hovered: self.hovered,
            anim_speed,
            selection_box: self.selection_box,
            toast_text: self.toast.as_ref().map(|(s, _)| s.as_str()),
            show_info: self.show_info,
        };
        if let Some(renderer) = self.renderer.as_mut() {
            let _ = renderer.render(&params);
        }
    }

    fn sync_monitor_bounds_and_dpi(&mut self, hwnd: HWND, new_dpi: f32) {
        let clamped_dpi = new_dpi.max(96.0);
        self.dpi = clamped_dpi;
        self.camera.set_dpi_scale(clamped_dpi / 96.0);
        unsafe {
            apply_dpi_matched_window_icons(hwnd, clamped_dpi as u32);
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
                // 关键防坑：物理高度主动减 1px，100% 保证 Windows DWM 桌面 Alpha 半透明穿透永不失效变黑底！
                let h = if self.is_fullscreen {
                    (r.bottom - r.top - 1).max(480)
                } else {
                    (r.bottom - r.top).max(480)
                };
                let _ = SetWindowPos(
                    hwnd,
                    None,
                    r.left,
                    r.top,
                    w,
                    h,
                    SWP_NOZORDER | SWP_NOACTIVATE,
                );
            }
            let mut rc = RECT::default();
            let _ = GetClientRect(hwnd, &mut rc);
            let phys_w = (rc.right - rc.left).max(1) as u32;
            let phys_h = (rc.bottom - rc.top).max(1) as u32;
            self.view_w = phys_w as f32 * 96.0 / clamped_dpi;
            self.view_h = phys_h as f32 * 96.0 / clamped_dpi;
            if let Some(r) = self.renderer.as_mut() {
                let _ = r.resize(phys_w, phys_h, clamped_dpi);
            }
        }
        let (iw, ih) = self.current_image_dims();
        if iw > 0.0 && ih > 0.0 {
            if self.camera.is_fit_mode {
                self.camera
                    .refit_preserving_rotation(iw, ih, self.view_w, self.view_h);
            } else {
                self.camera
                    .clamp_target_pan(iw, ih, self.view_w, self.view_h);
            }
        }
        self.recompute_layout();
        self.render_now();
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
            WM_TIMER => {
                if wparam.0 == TIMER_ANIM_ID {
                    self.on_tick();
                    return Some(LRESULT(0));
                }
                if wparam.0 == TIMER_IDLE_HIDE_ID {
                    unsafe {
                        let _ = KillTimer(hwnd, TIMER_IDLE_HIDE_ID);
                    }
                    self.ensure_anim_timer();
                    self.on_tick();
                    return Some(LRESULT(0));
                }
                None
            }
            WM_VIEWER_IMAGE_READY => {
                self.sync_current_from_cache(false);
                self.recompute_layout();
                self.request_thumbs_for_visible_filmstrip();
                self.ensure_anim_timer();
                self.render_now();
                Some(LRESULT(0))
            }
            WM_VIEWER_THUMB_READY => {
                self.request_thumbs_for_visible_filmstrip();
                self.ensure_anim_timer();
                self.render_now();
                Some(LRESULT(0))
            }
            WM_SIZE => {
                let phys_w = (lparam.0 & 0xFFFF) as u32;
                let phys_h = ((lparam.0 >> 16) & 0xFFFF) as u32;
                if phys_w > 0 && phys_h > 0 {
                    let cur_dpi = unsafe { GetDpiForWindow(hwnd).max(96) as f32 };
                    self.dpi = cur_dpi;
                    self.camera.set_dpi_scale(cur_dpi / 96.0);
                    self.view_w = phys_w as f32 * 96.0 / self.dpi;
                    self.view_h = phys_h as f32 * 96.0 / self.dpi;
                    if let Some(r) = self.renderer.as_mut() {
                        let _ = r.resize(phys_w, phys_h, self.dpi);
                    }
                    let (iw, ih) = self.current_image_dims();
                    if iw > 0.0 && ih > 0.0 {
                        if self.camera.is_fit_mode {
                            self.camera
                                .refit_preserving_rotation(iw, ih, self.view_w, self.view_h);
                        } else {
                            self.camera
                                .clamp_target_pan(iw, ih, self.view_w, self.view_h);
                        }
                    }
                    self.recompute_layout();
                    self.request_thumbs_for_visible_filmstrip();
                    self.render_now();
                }
                Some(LRESULT(0))
            }
            WM_DPICHANGED => {
                let new_dpi = ((wparam.0 >> 16) & 0xFFFF) as f32;
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
                self.sync_monitor_bounds_and_dpi(hwnd, new_dpi);
                Some(LRESULT(0))
            }
            WM_DISPLAYCHANGE => {
                let cur_dpi = unsafe { GetDpiForWindow(hwnd).max(96) as f32 };
                self.sync_monitor_bounds_and_dpi(hwnd, cur_dpi);
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
                    let move_dt = now.duration_since(self.drag_last_move_at).as_secs_f32();
                    if move_x.abs() > 0.8 || move_y.abs() > 0.8 {
                        self.drag_moved = true;
                    }
                    self.drag_last_x = dx;
                    self.drag_last_y = dy;
                    self.drag_last_move_at = now;
                    let (iw, ih) = self.current_image_dims();
                    if iw > 0.0 && ih > 0.0 {
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
                            // 幻灯片中轻点图片唤醒/切换暂停继续，方便沉浸控制
                            self.touch_activity();
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
                            self.camera
                                .zoom_to_screen_rect(sel, iw, ih, self.view_w, self.view_h);
                            self.ensure_anim_timer();
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
                        } else {
                            self.request_animated_close();
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
                            self.camera.toggle_actual_or_fit(
                                dx,
                                dy,
                                iw,
                                ih,
                                self.view_w,
                                self.view_h,
                            );
                            self.ensure_anim_timer();
                        }
                    }
                    self.recompute_layout();
                    self.render_now();
                }
                Some(LRESULT(0))
            }
            WM_MBUTTONUP => {
                self.request_animated_close();
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
                        self.camera.zoom_at_point(
                            wheel_delta,
                            dx,
                            dy,
                            iw,
                            ih,
                            self.view_w,
                            self.view_h,
                        );
                        self.ensure_anim_timer();
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
                        let (iw, ih) = self.current_image_dims();
                        if iw > 0.0 && ih > 0.0 {
                            self.camera.zoom_at_point(
                                4.0,
                                self.view_w * 0.5,
                                self.view_h * 0.5,
                                iw,
                                ih,
                                self.view_w,
                                self.view_h,
                            );
                            self.ensure_anim_timer();
                            self.recompute_layout();
                            self.render_now();
                        }
                    }
                    x if x == VK_NEXT.0 => {
                        let (iw, ih) = self.current_image_dims();
                        if iw > 0.0 && ih > 0.0 {
                            self.camera.zoom_at_point(
                                -4.0,
                                self.view_w * 0.5,
                                self.view_h * 0.5,
                                iw,
                                ih,
                                self.view_w,
                                self.view_h,
                            );
                            self.ensure_anim_timer();
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
                unsafe {
                    let _ = KillTimer(hwnd, TIMER_ANIM_ID);
                    let _ = KillTimer(hwnd, TIMER_IDLE_HIDE_ID);
                    PostQuitMessage(0);
                }
                Some(LRESULT(0))
            }
            _ => None,
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

unsafe fn apply_dpi_matched_window_icons(hwnd: HWND, dpi: u32) {
    let Ok(hmod) = GetModuleHandleW(None) else {
        return;
    };
    let hinst: HINSTANCE = hmod.into();
    let d = dpi.max(96);
    let big_w = GetSystemMetricsForDpi(SM_CXICON, d).max(32);
    let big_h = GetSystemMetricsForDpi(SM_CYICON, d).max(32);
    let sm_w = GetSystemMetricsForDpi(SM_CXSMICON, d).max(16);
    let sm_h = GetSystemMetricsForDpi(SM_CYSMICON, d).max(16);

    let big_icon = load_embedded_icon(hinst, big_w, big_h, false);
    if !big_icon.is_invalid() {
        let prev = SendMessageW(
            hwnd,
            WM_SETICON,
            WPARAM(ICON_BIG as usize),
            LPARAM(big_icon.0 as isize),
        );
        if prev.0 != 0 {
            let _ = DestroyIcon(HICON(prev.0 as *mut _));
        }
    }
    let sm_icon = load_embedded_icon(hinst, sm_w, sm_h, false);
    if !sm_icon.is_invalid() {
        let prev = SendMessageW(
            hwnd,
            WM_SETICON,
            WPARAM(ICON_SMALL as usize),
            LPARAM(sm_icon.0 as isize),
        );
        if prev.0 != 0 {
            let _ = DestroyIcon(HICON(prev.0 as *mut _));
        }
    }
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
    use super::compute_next_filmstrip_offset;

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
}
