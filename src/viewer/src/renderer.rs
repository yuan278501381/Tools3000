// ─────────────────────────────────────────────────────────────────────────────
// renderer.rs — Direct3D11 + DirectComposition + Direct2D 1.1 + DirectWrite
//               硬件加速矢量自绘引擎（高分屏 100%~250%+ 原生超清 · 6 大物理动效）
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{
    CLOSE_BACKDROP_START_RATIO, CLOSE_HUD_FADEOUT_RATIO, CLOSE_IMAGE_IMPLODE_RATIO, COLOR_ACCENT,
    COLOR_BACKDROP_GLASS, COLOR_BACKDROP_LIGHT, COLOR_BACKDROP_SOLID_DARK, COLOR_BAR_BG,
    COLOR_PANEL_BG, COLOR_PANEL_BORDER, COLOR_PANEL_TOP_SHEEN, COLOR_TEXT_PRIMARY,
    COLOR_TEXT_SECONDARY, COLOR_WINDOWED_BG_DARK, COLOR_WINDOWED_BG_LIGHT,
    COLOR_WINDOWED_BG_SOLID_DARK, FILMSTRIP_DIM_OPACITY, FONT_MONO_FAMILY, FONT_UI_FAMILY,
    MAX_THUMB_CACHE_ENTRIES, OPEN_START_SCALE_RATIO, PHOTO_SHADOW_AMBIENT_OFFSET_Y_DIP,
    PHOTO_SHADOW_AMBIENT_OPACITY, PHOTO_SHADOW_AMBIENT_SPREAD_DIP,
    PHOTO_SHADOW_CONTACT_OFFSET_Y_DIP, PHOTO_SHADOW_CONTACT_OPACITY,
    PHOTO_SHADOW_CONTACT_SPREAD_DIP, SLIDE_FAST_OFFSET_DIP, SLIDE_MICRO_OFFSET_DIP,
    THUMB_INITIAL_STAGGER_SEC, THUMB_ITEM_GROW_DURATION_SEC, THUMB_MAX_STAGGER_SEC,
    THUMB_WAVE_START_DELAY_SEC,
};
use crate::i18n::ViewerLanguage;
use crate::physics::{
    close_implode_curve, ease_out_back_soft, ease_out_cubic, ease_out_picasa_open, ease_out_quad,
    ease_out_quart, smoothstep_s_curve,
};
use crate::types::{
    format_file_size, BackdropMode, CloseImplodeSnapshot, DecodeError, ImageFrame, RectF,
    SharedImage, SlideshowPortalSnapshot, ThumbnailBitmap, ToolbarAction,
};
use crate::ui_layout::{HitTarget, ToolbarButtonLayout, ViewerUiLayout};
use crate::viewport::ViewportCamera;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::time::Instant;
use windows::core::{Interface, HRESULT, PCWSTR};
use windows::Foundation::Numerics::Matrix3x2;
use windows::Win32::Foundation::{D2DERR_RECREATE_TARGET, HWND};
use windows::Win32::Graphics::Direct2D::Common::{
    D2D1_ALPHA_MODE_PREMULTIPLIED, D2D1_COLOR_F, D2D1_FIGURE_BEGIN_FILLED,
    D2D1_FIGURE_BEGIN_HOLLOW, D2D1_FIGURE_END_CLOSED, D2D1_FIGURE_END_OPEN, D2D1_GRADIENT_STOP,
    D2D1_PIXEL_FORMAT, D2D_POINT_2F, D2D_RECT_F, D2D_SIZE_F, D2D_SIZE_U,
};
use windows::Win32::Graphics::Direct2D::{
    D2D1CreateFactory, ID2D1Bitmap1, ID2D1BitmapBrush1, ID2D1Device, ID2D1DeviceContext,
    ID2D1Factory1, ID2D1LinearGradientBrush, ID2D1PathGeometry, ID2D1RadialGradientBrush,
    ID2D1SolidColorBrush, ID2D1StrokeStyle, D2D1_ARC_SEGMENT, D2D1_ARC_SIZE_LARGE,
    D2D1_ARC_SIZE_SMALL, D2D1_BITMAP_BRUSH_PROPERTIES1, D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
    D2D1_BITMAP_OPTIONS_NONE, D2D1_BITMAP_OPTIONS_TARGET, D2D1_BITMAP_PROPERTIES1,
    D2D1_BRUSH_PROPERTIES, D2D1_BUFFER_PRECISION_8BPC_UNORM, D2D1_CAP_STYLE_FLAT,
    D2D1_CAP_STYLE_ROUND, D2D1_COLOR_INTERPOLATION_MODE_STRAIGHT, D2D1_COLOR_SPACE_SRGB,
    D2D1_DEVICE_CONTEXT_OPTIONS_NONE, D2D1_DRAW_TEXT_OPTIONS_CLIP, D2D1_ELLIPSE,
    D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_WRAP, D2D1_FACTORY_OPTIONS,
    D2D1_FACTORY_TYPE_SINGLE_THREADED, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
    D2D1_INTERPOLATION_MODE_LINEAR, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES, D2D1_LINE_JOIN_MITER, D2D1_LINE_JOIN_ROUND,
    D2D1_PRIMITIVE_BLEND_ADD, D2D1_PRIMITIVE_BLEND_SOURCE_OVER, D2D1_QUADRATIC_BEZIER_SEGMENT,
    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES, D2D1_ROUNDED_RECT, D2D1_STROKE_STYLE_PROPERTIES1, D2D1_SWEEP_DIRECTION_CLOCKWISE,
    D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE,
};
use windows::Win32::Graphics::Direct3D::{
    D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP, D3D_FEATURE_LEVEL_10_0,
    D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
};
use windows::Win32::Graphics::Direct3D11::{
    D3D11CreateDevice, ID3D11Device, ID3D11DeviceContext, ID3D11Texture2D,
    D3D11_BIND_RENDER_TARGET, D3D11_CPU_ACCESS_READ, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    D3D11_MAPPED_SUBRESOURCE, D3D11_MAP_READ, D3D11_SDK_VERSION, D3D11_TEXTURE2D_DESC,
    D3D11_USAGE_DEFAULT, D3D11_USAGE_STAGING,
};
use windows::Win32::Graphics::DirectComposition::{
    DCompositionCreateDevice, IDCompositionDevice, IDCompositionTarget, IDCompositionVisual,
};
use windows::Win32::Graphics::DirectWrite::{
    DWriteCreateFactory, IDWriteFactory, IDWriteTextFormat, DWRITE_FACTORY_TYPE_SHARED,
    DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_WEIGHT_BOLD,
    DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
    DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_TEXT_ALIGNMENT_LEADING,
    DWRITE_WORD_WRAPPING_NO_WRAP, DWRITE_WORD_WRAPPING_WRAP,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_ALPHA_MODE_PREMULTIPLIED, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SAMPLE_DESC,
};
use windows::Win32::Graphics::Dxgi::{
    IDXGIDevice, IDXGIDevice3, IDXGIFactory2, IDXGISurface, IDXGISwapChain1,
    DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, DXGI_SCALING_STRETCH,
    DXGI_SWAP_CHAIN_DESC1, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_USAGE_RENDER_TARGET_OUTPUT,
};

fn color_f(c: [f32; 4]) -> D2D1_COLOR_F {
    D2D1_COLOR_F {
        r: c[0],
        g: c[1],
        b: c[2],
        a: c[3],
    }
}

fn color_alpha(c: [f32; 4], alpha_mul: f32) -> D2D1_COLOR_F {
    D2D1_COLOR_F {
        r: c[0],
        g: c[1],
        b: c[2],
        a: (c[3] * alpha_mul).clamp(0.0, 1.0),
    }
}

fn to_d2d_rect(r: RectF) -> D2D_RECT_F {
    D2D_RECT_F {
        left: r.left,
        top: r.top,
        right: r.right,
        bottom: r.bottom,
    }
}

fn to_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

fn p2(x: f32, y: f32) -> D2D_POINT_2F {
    D2D_POINT_2F { x, y }
}

fn matrix_scale(sx: f32, sy: f32) -> Matrix3x2 {
    Matrix3x2 {
        M11: sx,
        M12: 0.0,
        M21: 0.0,
        M22: sy,
        M31: 0.0,
        M32: 0.0,
    }
}

fn lerp_rgba(a: [f32; 4], b: [f32; 4], t: f32) -> [f32; 4] {
    let k = t.clamp(0.0, 1.0);
    [
        a[0] + (b[0] - a[0]) * k,
        a[1] + (b[1] - a[1]) * k,
        a[2] + (b[2] - a[2]) * k,
        a[3] + (b[3] - a[3]) * k,
    ]
}

/// 渲染管线错误（显式区分 GPU 设备丢失/重置，供宿主自动无感重建渲染器）
#[derive(Debug, Clone)]
pub enum RenderError {
    DeviceLost(String),
    Other(String),
}

impl std::fmt::Display for RenderError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::DeviceLost(s) | Self::Other(s) => f.write_str(s),
        }
    }
}

#[inline]
fn is_device_lost_hr(hr: HRESULT) -> bool {
    hr == D2DERR_RECREATE_TARGET
        || hr == DXGI_ERROR_DEVICE_REMOVED
        || hr == DXGI_ERROR_DEVICE_RESET
}

fn classify_win_err(context: &str, err: windows::core::Error) -> RenderError {
    let msg = format!("{context}: {err}");
    if is_device_lost_hr(err.code()) {
        RenderError::DeviceLost(msg)
    } else {
        RenderError::Other(msg)
    }
}

/// 离场旧图快照（用于常规快速切图或幻灯片 650ms 双图层柔和叠化）
struct OutgoingSlide {
    bitmap: ID2D1Bitmap1,
    img_w: f32,
    img_h: f32,
    has_alpha: bool,
    rotation_deg: f32,
    flip_x: f32,
    flip_y: f32,
    zoom: f32,
    pan_x: f32,
    pan_y: f32,
    dpi_scale: f32,
}

struct UploadedThumb {
    bitmap: ID2D1Bitmap1,
    orig_w: u32,
    orig_h: u32,
    last_used_tick: u64,
}

pub struct RenderFrameParams<'a> {
    pub now: Instant,
    pub open_started_at: Instant,
    pub lang: ViewerLanguage,
    pub view_w: f32,
    pub view_h: f32,
    pub dpi: f32,
    pub backdrop_open_progress: f32,
    pub open_anim_armed: bool,
    pub open_progress: f32,
    pub close_progress: f32,
    pub close_snapshot: Option<CloseImplodeSnapshot>,
    pub slide_progress: f32,
    pub slide_dir: f32,
    pub slide_is_crossfade: bool,
    pub slide_fast_repeat: bool,
    pub cinema_dim: f32,
    pub slideshow_portal: Option<SlideshowPortalSnapshot>,
    pub close_hover_anim: f32,
    pub play_hover_anim: f32,
    pub backdrop: BackdropMode,
    /// 仅在 `--dump-ui` 离线核验时模拟半透明下方的桌面底色
    pub simulated_desktop_bg: Option<[f32; 4]>,
    pub camera: &'a ViewportCamera,
    pub image: Option<&'a SharedImage>,
    pub fallback_thumb_path: Option<&'a PathBuf>,
    pub frame_index: usize,
    pub error: Option<&'a DecodeError>,
    pub loading_name: Option<&'a str>,
    pub show_loading_box: bool,
    pub current_index: usize,
    pub sibling_paths: &'a [PathBuf],
    pub is_slideshow: bool,
    pub is_windowed: bool,
    pub layout: &'a ViewerUiLayout,
    pub hovered: HitTarget,
    pub anim_speed: f32,
    pub selection_box: Option<RectF>,
    pub toast_text: Option<&'a str>,
    pub show_info: bool,
    pub zoom_badge_alpha: f32,
}

/// 静态预烘焙高频矢量几何体集合（彻底消除 120~240Hz 渲染循环中的每帧 PathGeometry 分配）
struct StaticGeometries {
    play_tri_unit: Option<ID2D1PathGeometry>,
    crystal_cap_unit: Option<ID2D1PathGeometry>,
    wing_left: Option<ID2D1PathGeometry>,
    wing_right: Option<ID2D1PathGeometry>,
    rot_ccw_arc: Option<ID2D1PathGeometry>,
    rot_ccw_head: Option<ID2D1PathGeometry>,
    rot_cw_arc: Option<ID2D1PathGeometry>,
    rot_cw_head: Option<ID2D1PathGeometry>,
    flip_h_left: Option<ID2D1PathGeometry>,
    flip_h_right: Option<ID2D1PathGeometry>,
    flip_v_top: Option<ID2D1PathGeometry>,
    flip_v_bot: Option<ID2D1PathGeometry>,
}

pub struct ViewerRenderer {
    d3d_device: ID3D11Device,
    d3d_ctx: ID3D11DeviceContext,
    d2d_factory: ID2D1Factory1,
    _d2d_device: ID2D1Device,
    d2d_ctx: ID2D1DeviceContext,
    _dwrite: IDWriteFactory,
    dcomp_device: Option<IDCompositionDevice>,
    _dcomp_target: Option<IDCompositionTarget>,
    _dcomp_visual: Option<IDCompositionVisual>,
    swap_chain: Option<IDXGISwapChain1>,
    offscreen_tex: Option<ID3D11Texture2D>,
    target_bitmap: Option<ID2D1Bitmap1>,
    brush: ID2D1SolidColorBrush,
    checker_brush: ID2D1BitmapBrush1,
    shadow_9slice: ID2D1Bitmap1,
    bar_grad_brush: Option<ID2D1LinearGradientBrush>,
    refl_fade_brush: Option<ID2D1LinearGradientBrush>,
    geoms: StaticGeometries,
    stroke_round: ID2D1StrokeStyle,
    stroke_flat: ID2D1StrokeStyle,
    stroke_miter: ID2D1StrokeStyle,
    fmt_ui: IDWriteTextFormat,
    fmt_ui_center: IDWriteTextFormat,
    fmt_status_bold: IDWriteTextFormat,
    fmt_mono: IDWriteTextFormat,
    fmt_mono_center: IDWriteTextFormat,
    fmt_zoom_badge: IDWriteTextFormat,
    fmt_error_title: IDWriteTextFormat,
    fmt_error_detail: IDWriteTextFormat,
    fmt_btn_1to1: IDWriteTextFormat,
    fmt_title: IDWriteTextFormat,

    phys_w: u32,
    phys_h: u32,
    dpi: f32,
    max_bitmap_size: u32,

    uploaded_key: Option<(PathBuf, usize)>,
    main_bitmap: Option<ID2D1Bitmap1>,
    upload_error: Option<DecodeError>,
    outgoing_slide: Option<OutgoingSlide>,
    thumb_lru_tick: u64,
    thumb_bitmaps: HashMap<PathBuf, UploadedThumb>,
    failed_thumbs: HashSet<PathBuf>,
    thumb_born_at: HashMap<PathBuf, Instant>,
    thumb_hover_anim: HashMap<usize, f32>,
}

impl ViewerRenderer {
    pub fn new(
        hwnd: HWND,
        phys_w: u32,
        phys_h: u32,
        dpi: f32,
        lang: ViewerLanguage,
    ) -> Result<Self, String> {
        Self::create_internal(Some(hwnd), phys_w, phys_h, dpi, lang)
    }

    pub fn new_offscreen(
        phys_w: u32,
        phys_h: u32,
        dpi: f32,
        lang: ViewerLanguage,
    ) -> Result<Self, String> {
        Self::create_internal(None, phys_w, phys_h, dpi, lang)
    }

    fn create_internal(
        hwnd_opt: Option<HWND>,
        phys_w: u32,
        phys_h: u32,
        dpi: f32,
        lang: ViewerLanguage,
    ) -> Result<Self, String> {
        unsafe {
            let feature_levels = [
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0,
            ];
            let mut device_opt = None;
            let mut d3d_ctx_opt = None;
            let hr = D3D11CreateDevice(
                None,
                D3D_DRIVER_TYPE_HARDWARE,
                None,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                Some(&feature_levels),
                D3D11_SDK_VERSION,
                Some(&mut device_opt),
                None,
                Some(&mut d3d_ctx_opt),
            );
            if hr.is_err() || device_opt.is_none() {
                D3D11CreateDevice(
                    None,
                    D3D_DRIVER_TYPE_WARP,
                    None,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    Some(&feature_levels),
                    D3D11_SDK_VERSION,
                    Some(&mut device_opt),
                    None,
                    Some(&mut d3d_ctx_opt),
                )
                .map_err(|e| format!("初始化 Direct3D11 失败: {e}"))?;
            }
            let d3d_device = device_opt.ok_or_else(|| "D3D11Device 为空".to_string())?;
            let d3d_ctx = d3d_ctx_opt.ok_or_else(|| "D3D11DeviceContext 为空".to_string())?;
            let dxgi_device: IDXGIDevice = d3d_device
                .cast()
                .map_err(|e| format!("查询 IDXGIDevice 失败: {e}"))?;

            let w = phys_w.max(1);
            let h = phys_h.max(1);

            let (dcomp_device, dcomp_target, dcomp_visual, swap_chain, offscreen_tex) =
                if let Some(hwnd) = hwnd_opt {
                    let adapter = dxgi_device
                        .GetAdapter()
                        .map_err(|e| format!("获取 DXGI 适配器失败: {e}"))?;
                    let dxgi_factory: IDXGIFactory2 = adapter
                        .GetParent()
                        .map_err(|e| format!("获取 IDXGIFactory2 失败: {e}"))?;
                    let sc_desc = DXGI_SWAP_CHAIN_DESC1 {
                        Width: w,
                        Height: h,
                        Format: DXGI_FORMAT_B8G8R8A8_UNORM,
                        Stereo: false.into(),
                        SampleDesc: DXGI_SAMPLE_DESC {
                            Count: 1,
                            Quality: 0,
                        },
                        BufferUsage: DXGI_USAGE_RENDER_TARGET_OUTPUT,
                        BufferCount: 2,
                        Scaling: DXGI_SCALING_STRETCH,
                        SwapEffect: DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
                        AlphaMode: DXGI_ALPHA_MODE_PREMULTIPLIED,
                        Flags: 0,
                    };
                    let sc = dxgi_factory
                        .CreateSwapChainForComposition(&dxgi_device, &sc_desc, None)
                        .map_err(|e| format!("创建 Composition SwapChain 失败: {e}"))?;

                    let dc_dev: IDCompositionDevice = DCompositionCreateDevice(&dxgi_device)
                        .map_err(|e| format!("DCompositionCreateDevice 失败: {e}"))?;
                    let dc_target = dc_dev
                        .CreateTargetForHwnd(hwnd, true)
                        .map_err(|e| format!("CreateTargetForHwnd 失败: {e}"))?;
                    let dc_vis = dc_dev
                        .CreateVisual()
                        .map_err(|e| format!("CreateVisual 失败: {e}"))?;
                    dc_vis
                        .SetContent(&sc)
                        .map_err(|e| format!("SetContent 失败: {e}"))?;
                    dc_target
                        .SetRoot(&dc_vis)
                        .map_err(|e| format!("SetRoot 失败: {e}"))?;
                    dc_dev
                        .Commit()
                        .map_err(|e| format!("DirectComposition Commit 失败: {e}"))?;
                    (Some(dc_dev), Some(dc_target), Some(dc_vis), Some(sc), None)
                } else {
                    let tex_desc = D3D11_TEXTURE2D_DESC {
                        Width: w,
                        Height: h,
                        MipLevels: 1,
                        ArraySize: 1,
                        Format: DXGI_FORMAT_B8G8R8A8_UNORM,
                        SampleDesc: DXGI_SAMPLE_DESC {
                            Count: 1,
                            Quality: 0,
                        },
                        Usage: D3D11_USAGE_DEFAULT,
                        BindFlags: D3D11_BIND_RENDER_TARGET.0 as u32,
                        CPUAccessFlags: 0,
                        MiscFlags: 0,
                    };
                    let mut tex = None;
                    d3d_device
                        .CreateTexture2D(&tex_desc, None, Some(&mut tex))
                        .map_err(|e| format!("创建离屏渲染纹理失败: {e}"))?;
                    (None, None, None, None, tex)
                };

            let d2d_factory: ID2D1Factory1 = D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                Some(&D2D1_FACTORY_OPTIONS::default()),
            )
            .map_err(|e| format!("D2D1CreateFactory 失败: {e}"))?;
            let d2d_device = d2d_factory
                .CreateDevice(&dxgi_device)
                .map_err(|e| format!("D2D CreateDevice 失败: {e}"))?;
            let d2d_ctx = d2d_device
                .CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE)
                .map_err(|e| format!("D2D CreateDeviceContext 失败: {e}"))?;
            d2d_ctx.SetDpi(dpi, dpi);
            d2d_ctx.SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

            let stroke_round: ID2D1StrokeStyle = d2d_factory
                .CreateStrokeStyle(
                    &D2D1_STROKE_STYLE_PROPERTIES1 {
                        startCap: D2D1_CAP_STYLE_ROUND,
                        endCap: D2D1_CAP_STYLE_ROUND,
                        dashCap: D2D1_CAP_STYLE_ROUND,
                        lineJoin: D2D1_LINE_JOIN_ROUND,
                        miterLimit: 4.0,
                        ..Default::default()
                    },
                    None,
                )
                .map_err(|e| format!("CreateStrokeStyle round 失败: {e}"))?
                .into();

            let stroke_flat: ID2D1StrokeStyle = d2d_factory
                .CreateStrokeStyle(
                    &D2D1_STROKE_STYLE_PROPERTIES1 {
                        startCap: D2D1_CAP_STYLE_FLAT,
                        endCap: D2D1_CAP_STYLE_FLAT,
                        dashCap: D2D1_CAP_STYLE_FLAT,
                        lineJoin: D2D1_LINE_JOIN_ROUND,
                        miterLimit: 4.0,
                        ..Default::default()
                    },
                    None,
                )
                .map_err(|e| format!("CreateStrokeStyle flat 失败: {e}"))?
                .into();

            let stroke_miter: ID2D1StrokeStyle = d2d_factory
                .CreateStrokeStyle(
                    &D2D1_STROKE_STYLE_PROPERTIES1 {
                        startCap: D2D1_CAP_STYLE_FLAT,
                        endCap: D2D1_CAP_STYLE_FLAT,
                        dashCap: D2D1_CAP_STYLE_FLAT,
                        lineJoin: D2D1_LINE_JOIN_MITER,
                        miterLimit: 6.0,
                        ..Default::default()
                    },
                    None,
                )
                .map_err(|e| format!("CreateStrokeStyle miter 失败: {e}"))?
                .into();

            let brush = d2d_ctx
                .CreateSolidColorBrush(&color_f(COLOR_TEXT_PRIMARY), None)
                .map_err(|e| format!("CreateSolidColorBrush 失败: {e}"))?;

            let checker_brush = create_gpu_checker_brush(&d2d_ctx)?;
            let shadow_9slice = create_gaussian_shadow_9slice_bitmap(&d2d_ctx)?;
            let max_bitmap_size = d2d_ctx.GetMaximumBitmapSize().max(2048);

            // 预烘焙常用渐变刷与高频静态矢量几何体，消除 120~240Hz 热路径 COM 分配
            let bar_grad_brush = create_linear_gradient_brush(
                &d2d_ctx,
                p2(0.0, 0.0),
                p2(0.0, 68.0),
                &[
                    (0.0, [0.115, 0.120, 0.130, 0.82]),
                    (0.68, [0.082, 0.086, 0.094, 0.87]),
                    (1.0, [0.055, 0.058, 0.066, 0.92]),
                ],
                1.0,
            );
            let refl_fade_brush = create_linear_gradient_brush(
                &d2d_ctx,
                p2(0.0, 0.0),
                p2(0.0, 18.0),
                &[
                    (0.0, [0.06, 0.064, 0.07, 0.25]),
                    (0.65, [0.055, 0.058, 0.064, 0.80]),
                    (1.0, [0.055, 0.058, 0.064, 0.96]),
                ],
                1.0,
            );
            let geoms = build_static_geometries(&d2d_factory);

            let dwrite: IDWriteFactory = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED)
                .map_err(|e| format!("DWriteCreateFactory 失败: {e}"))?;

            let cjk_fb = lang.preferred_cjk_fallback_font();
            let loc_tag = lang.tag();
            let ui_family = match lang {
                ViewerLanguage::ZhTW | ViewerLanguage::JaJP | ViewerLanguage::KoKR => {
                    resolve_available_font(&dwrite, cjk_fb, FONT_UI_FAMILY)
                }
                _ => resolve_available_font(&dwrite, FONT_UI_FAMILY, cjk_fb),
            };
            let mono_family = resolve_available_font(&dwrite, FONT_MONO_FAMILY, "Consolas");
            let arial_family = resolve_available_font(&dwrite, "Segoe UI", ui_family);

            // 高分屏清晰排版：字号全部 >= 12.4 DIP，字重 >= 500 Medium
            let fmt_ui = create_text_format(
                &dwrite,
                ui_family,
                12.8,
                DWRITE_FONT_WEIGHT_MEDIUM,
                false,
                loc_tag,
            )?;
            let fmt_ui_center = create_text_format(
                &dwrite,
                ui_family,
                12.8,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                true,
                loc_tag,
            )?;
            let fmt_status_bold = create_text_format(
                &dwrite,
                ui_family,
                13.5,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                true,
                loc_tag,
            )?;
            let fmt_mono = create_text_format(
                &dwrite,
                mono_family,
                12.4,
                DWRITE_FONT_WEIGHT_MEDIUM,
                false,
                loc_tag,
            )?;
            let fmt_mono_center = create_text_format(
                &dwrite,
                mono_family,
                12.4,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                true,
                loc_tag,
            )?;
            let fmt_zoom_badge = create_text_format(
                &dwrite,
                mono_family,
                15.5,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                true,
                "en-US",
            )?;
            let fmt_error_title = create_text_format(
                &dwrite,
                ui_family,
                17.0,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                true,
                loc_tag,
            )?;
            let fmt_error_detail = create_wrapped_text_format(
                &dwrite,
                ui_family,
                12.8,
                DWRITE_FONT_WEIGHT_MEDIUM,
                loc_tag,
            )?;
            let fmt_btn_1to1 = create_text_format(
                &dwrite,
                arial_family,
                12.4,
                DWRITE_FONT_WEIGHT_BOLD,
                true,
                "en-US",
            )?;
            let fmt_title = create_text_format(
                &dwrite,
                ui_family,
                14.0,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                false,
                loc_tag,
            )?;

            let mut renderer = Self {
                d3d_device,
                d3d_ctx,
                d2d_factory,
                _d2d_device: d2d_device,
                d2d_ctx,
                _dwrite: dwrite,
                dcomp_device,
                _dcomp_target: dcomp_target,
                _dcomp_visual: dcomp_visual,
                swap_chain,
                offscreen_tex,
                target_bitmap: None,
                brush,
                checker_brush,
                shadow_9slice,
                bar_grad_brush,
                refl_fade_brush,
                geoms,
                stroke_round,
                stroke_flat,
                stroke_miter,
                fmt_ui,
                fmt_ui_center,
                fmt_status_bold,
                fmt_mono,
                fmt_mono_center,
                fmt_zoom_badge,
                fmt_error_title,
                fmt_error_detail,
                fmt_btn_1to1,
                fmt_title,
                phys_w: w,
                phys_h: h,
                dpi,
                max_bitmap_size,
                uploaded_key: None,
                main_bitmap: None,
                upload_error: None,
                outgoing_slide: None,
                thumb_lru_tick: 0,
                thumb_bitmaps: HashMap::new(),
                failed_thumbs: HashSet::new(),
                thumb_born_at: HashMap::new(),
                thumb_hover_anim: HashMap::new(),
            };
            renderer.bind_render_target(dpi)?;
            Ok(renderer)
        }
    }

    fn bind_render_target(&mut self, dpi: f32) -> Result<(), String> {
        unsafe {
            self.d2d_ctx.SetTarget(None);
            self.target_bitmap = None;
            let surface: IDXGISurface = if let Some(sc) = self.swap_chain.as_ref() {
                sc.GetBuffer(0)
                    .map_err(|e| format!("SwapChain GetBuffer 失败: {e}"))?
            } else if let Some(tex) = self.offscreen_tex.as_ref() {
                tex.cast()
                    .map_err(|e| format!("离屏纹理转 IDXGISurface 失败: {e}"))?
            } else {
                return Err("未找到有效渲染目标表面".to_string());
            };
            let props = D2D1_BITMAP_PROPERTIES1 {
                pixelFormat: D2D1_PIXEL_FORMAT {
                    format: DXGI_FORMAT_B8G8R8A8_UNORM,
                    alphaMode: D2D1_ALPHA_MODE_PREMULTIPLIED,
                },
                dpiX: dpi,
                dpiY: dpi,
                bitmapOptions: D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                ..Default::default()
            };
            let bmp = self
                .d2d_ctx
                .CreateBitmapFromDxgiSurface(&surface, Some(&props))
                .map_err(|e| format!("CreateBitmapFromDxgiSurface 失败: {e}"))?;
            self.d2d_ctx.SetDpi(dpi, dpi);
            self.d2d_ctx
                .SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            self.d2d_ctx.SetTarget(&bmp);
            self.target_bitmap = Some(bmp);
            Ok(())
        }
    }

    pub fn resize(&mut self, phys_w: u32, phys_h: u32, dpi: f32) -> Result<(), RenderError> {
        let w = phys_w.max(1);
        let h = phys_h.max(1);
        let dpi_changed = (dpi - self.dpi).abs() > 0.5;
        if w == self.phys_w && h == self.phys_h {
            if dpi_changed {
                self.dpi = dpi;
                self.bind_render_target(dpi)
                    .map_err(RenderError::Other)?;
            }
            return Ok(());
        }
        unsafe {
            self.d2d_ctx.SetTarget(None);
            self.target_bitmap = None;
            if let Some(sc) = self.swap_chain.as_ref() {
                sc.ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, Default::default())
                    .map_err(|e| classify_win_err("ResizeBuffers 失败", e))?;
            }
        }
        self.phys_w = w;
        self.phys_h = h;
        self.dpi = dpi;
        self.bind_render_target(dpi)
            .map_err(RenderError::Other)?;
        if let Some(dc) = self.dcomp_device.as_ref() {
            unsafe {
                let _ = dc.Commit();
            }
        }
        Ok(())
    }

    /// 冷路径显存与驱动暂存页修剪（通过 IDXGIDevice3::Trim 归还 D2D/D3D11/DXGI 临时显存，严禁在动画热路径调用）
    pub fn trim_gpu_memory(&self) {
        unsafe {
            if let Ok(dxgi3) = self.d3d_device.cast::<IDXGIDevice3>() {
                dxgi3.Trim();
            }
        }
    }

    pub fn has_thumb(&self, path: &Path) -> bool {
        self.thumb_bitmaps.contains_key(path)
    }

    pub fn has_main_bitmap_for(&self, path: &Path, frame_idx: usize) -> bool {
        self.main_bitmap.is_some()
            && matches!(&self.uploaded_key, Some((p, idx)) if p == path && *idx == frame_idx)
    }

    /// 供 `--dump-ui` 将真实 GPU 渲染帧回读至内存导出 PNG
    pub fn readback_offscreen_frame(&self) -> Result<ImageFrame, String> {
        let Some(src_tex) = self.offscreen_tex.as_ref() else {
            return Err("仅离屏渲染模式支持回读帧纹理".to_string());
        };
        unsafe {
            let desc = D3D11_TEXTURE2D_DESC {
                Width: self.phys_w,
                Height: self.phys_h,
                MipLevels: 1,
                ArraySize: 1,
                Format: DXGI_FORMAT_B8G8R8A8_UNORM,
                SampleDesc: DXGI_SAMPLE_DESC {
                    Count: 1,
                    Quality: 0,
                },
                Usage: D3D11_USAGE_STAGING,
                BindFlags: 0,
                CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
                MiscFlags: 0,
            };
            let mut staging_opt = None;
            self.d3d_device
                .CreateTexture2D(&desc, None, Some(&mut staging_opt))
                .map_err(|e| format!("创建 Staging 纹理失败: {e}"))?;
            let staging = staging_opt.ok_or_else(|| "Staging 纹理为空".to_string())?;

            self.d3d_ctx.CopyResource(&staging, src_tex);
            let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
            self.d3d_ctx
                .Map(&staging, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
                .map_err(|e| format!("Map Staging 纹理失败: {e}"))?;

            let row_bytes = (self.phys_w as usize) * 4;
            let mut out = vec![0u8; row_bytes * (self.phys_h as usize)];
            let base_ptr = mapped.pData as *const u8;
            for y in 0..(self.phys_h as usize) {
                let src_row =
                    std::slice::from_raw_parts(base_ptr.add(y * (mapped.RowPitch as usize)), row_bytes);
                out[y * row_bytes..(y + 1) * row_bytes].copy_from_slice(src_row);
            }
            self.d3d_ctx.Unmap(&staging, 0);

            Ok(ImageFrame {
                width: self.phys_w,
                height: self.phys_h,
                delay_ms: 0,
                bgra_premul: out,
            })
        }
    }

    /// 上传缩略图至 GPU 并执行严格 LRU 淘汰（豁免当前可见窗口路径并同步清理辅助映射表，杜绝 >180 张全清黑闪）
    pub fn ensure_thumb_uploaded(
        &mut self,
        path: &PathBuf,
        thumb: &ThumbnailBitmap,
        protected: &HashSet<&Path>,
    ) {
        self.thumb_lru_tick = self.thumb_lru_tick.wrapping_add(1);
        let tick = self.thumb_lru_tick;
        if let Some(existing) = self.thumb_bitmaps.get_mut(path) {
            existing.last_used_tick = tick;
            return;
        }
        while self.thumb_bitmaps.len() >= MAX_THUMB_CACHE_ENTRIES {
            let victim = self
                .thumb_bitmaps
                .iter()
                .filter(|(k, _)| !protected.contains(k.as_path()))
                .min_by_key(|(_, v)| v.last_used_tick)
                .map(|(k, _)| k.clone());
            let Some(victim_key) = victim else {
                break;
            };
            self.thumb_bitmaps.remove(&victim_key);
            self.thumb_born_at.remove(&victim_key);
            self.failed_thumbs.remove(&victim_key);
        }
        if let Ok(bmp) = self.create_gpu_bitmap(thumb.width, thumb.height, &thumb.bgra_premul) {
            self.failed_thumbs.remove(path);
            self.thumb_bitmaps.insert(
                path.clone(),
                UploadedThumb {
                    bitmap: bmp,
                    orig_w: thumb.orig_width.max(1),
                    orig_h: thumb.orig_height.max(1),
                    last_used_tick: tick,
                },
            );
            self.thumb_born_at
                .entry(path.clone())
                .or_insert_with(Instant::now);
        }
    }

    pub fn invalidate_thumb(&mut self, path: &Path) {
        self.thumb_bitmaps.remove(path);
        self.thumb_born_at.remove(path);
        self.failed_thumbs.remove(path);
    }

    pub fn mark_thumb_failed(&mut self, path: &PathBuf) {
        if !self.thumb_bitmaps.contains_key(path) {
            self.failed_thumbs.insert(path.clone());
            self.thumb_born_at
                .entry(path.clone())
                .or_insert_with(Instant::now);
        }
    }

    pub fn thumb_original_dims(&self, path: &PathBuf) -> Option<(f32, f32)> {
        self.thumb_bitmaps
            .get(path)
            .map(|t| (t.orig_w as f32, t.orig_h as f32))
    }

    pub fn reset_filmstrip_birth_clock(&mut self) {
        self.thumb_born_at.clear();
        self.thumb_hover_anim.clear();
        self.failed_thumbs.clear();
    }

    pub fn clear_main_bitmap(&mut self) {
        self.uploaded_key = None;
        self.main_bitmap = None;
        self.upload_error = None;
    }

    /// 零拷贝接管当前 GPU 主图纹理作为翻图平滑淡切或幻灯片 650ms 交叉叠化的离场旧图
    pub fn capture_outgoing_for_slide(&mut self, camera: &ViewportCamera, img: &SharedImage) {
        if let Some(bmp) = self.main_bitmap.take() {
            self.uploaded_key = None;
            self.outgoing_slide = Some(OutgoingSlide {
                bitmap: bmp,
                img_w: img.width as f32,
                img_h: img.height as f32,
                has_alpha: img.has_alpha,
                rotation_deg: camera.visual_rotation_deg,
                flip_x: camera.visual_flip_x,
                flip_y: camera.visual_flip_y,
                zoom: camera.zoom,
                pan_x: camera.pan_x,
                pan_y: camera.pan_y,
                dpi_scale: (self.dpi / 96.0).max(0.5),
            });
        }
    }

    pub fn clear_outgoing_slide(&mut self) {
        self.outgoing_slide = None;
    }

    /// 推进缩略图悬停上浮与当前选中放大的平滑插值，返回是否仍有未收敛动画
    pub fn step_filmstrip_hover_anim(
        &mut self,
        hovered: HitTarget,
        visible_indices: &[usize],
        dt_sec: f32,
    ) -> bool {
        let hovered_idx = match hovered {
            HitTarget::FilmstripItem(i) => Some(i),
            _ => None,
        };
        let mut active = false;
        let f = crate::physics::exp_decay_factor(18.0, dt_sec);
        for &idx in visible_indices {
            let target = if hovered_idx == Some(idx) { 1.0f32 } else { 0.0f32 };
            let cur = self.thumb_hover_anim.get(&idx).copied().unwrap_or(0.0);
            let next = cur + (target - cur) * f;
            if (target - next).abs() > 0.008 {
                self.thumb_hover_anim.insert(idx, next);
                active = true;
            } else if next <= 0.008 {
                self.thumb_hover_anim.remove(&idx);
            } else {
                self.thumb_hover_anim.insert(idx, target);
            }
        }
        active
    }

    pub fn has_active_thumb_growth(&self, now: Instant, open_started_at: Instant) -> bool {
        let total_wave_window =
            THUMB_WAVE_START_DELAY_SEC + THUMB_MAX_STAGGER_SEC + THUMB_ITEM_GROW_DURATION_SEC + 0.08;
        if now.duration_since(open_started_at).as_secs_f32() < total_wave_window {
            return true;
        }
        self.thumb_born_at
            .values()
            .any(|t| now.duration_since(*t).as_secs_f32() < THUMB_ITEM_GROW_DURATION_SEC + 0.04)
    }

    unsafe fn make_linear_gradient(
        &self,
        start: D2D_POINT_2F,
        end: D2D_POINT_2F,
        stops: &[(f32, [f32; 4])],
        alpha: f32,
    ) -> Option<ID2D1LinearGradientBrush> {
        create_linear_gradient_brush(&self.d2d_ctx, start, end, stops, alpha)
    }

    unsafe fn make_radial_gradient(
        &self,
        center: D2D_POINT_2F,
        origin_offset: D2D_POINT_2F,
        radius_x: f32,
        radius_y: f32,
        stops: &[(f32, [f32; 4])],
        alpha: f32,
    ) -> Option<ID2D1RadialGradientBrush> {
        let d2d_stops: Vec<D2D1_GRADIENT_STOP> = stops
            .iter()
            .map(|(pos, col)| D2D1_GRADIENT_STOP {
                position: *pos,
                color: color_alpha(*col, alpha),
            })
            .collect();
        let col = self
            .d2d_ctx
            .CreateGradientStopCollection(
                &d2d_stops,
                D2D1_COLOR_SPACE_SRGB,
                D2D1_COLOR_SPACE_SRGB,
                D2D1_BUFFER_PRECISION_8BPC_UNORM,
                D2D1_EXTEND_MODE_CLAMP,
                D2D1_COLOR_INTERPOLATION_MODE_STRAIGHT,
            )
            .ok()?;
        let props = D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES {
            center,
            gradientOriginOffset: origin_offset,
            radiusX: radius_x,
            radiusY: radius_y,
        };
        let bprops = D2D1_BRUSH_PROPERTIES {
            opacity: 1.0,
            transform: Matrix3x2::identity(),
        };
        self.d2d_ctx
            .CreateRadialGradientBrush(&props, Some(&bprops), &col)
            .ok()
    }

    unsafe fn build_arc_geometry(
        &self,
        start: D2D_POINT_2F,
        end: D2D_POINT_2F,
        radius: f32,
        clockwise: bool,
        large_arc: bool,
    ) -> Option<ID2D1PathGeometry> {
        build_arc_geometry_with(&self.d2d_factory, start, end, radius, clockwise, large_arc)
    }

    fn ensure_main_frame_uploaded(&mut self, path: &PathBuf, frame_idx: usize, frame: &ImageFrame) {
        let same_key = match &self.uploaded_key {
            Some((p, idx)) => p == path && *idx == frame_idx,
            None => false,
        };
        // 若当前帧已上传成功或已登记失败，绝不每帧重复尝试上传避免死循环
        if same_key && (self.main_bitmap.is_some() || self.upload_error.is_some()) {
            return;
        }
        // 静态图上传 GPU 后已释放 CPU 像素缓冲时，安全跳过重复上传
        let expected_bytes = (frame.width as usize)
            .saturating_mul(frame.height as usize)
            .saturating_mul(4);
        if expected_bytes == 0 || frame.bgra_premul.len() < expected_bytes {
            return;
        }

        // 超长截图（宽或高超过硬件最大位图边长，如 16384 或 8192）先安全盒式降采样至 <= max_bitmap_size
        let downscaled;
        let (upload_w, upload_h, upload_pixels): (u32, u32, &[u8]) =
            if frame.width > self.max_bitmap_size || frame.height > self.max_bitmap_size {
                match crate::decode::downscale_bgra_box(
                    &frame.bgra_premul,
                    frame.width,
                    frame.height,
                    self.max_bitmap_size,
                ) {
                    Ok((dw, dh, buf)) => {
                        downscaled = buf;
                        (dw, dh, downscaled.as_slice())
                    }
                    Err(err) => {
                        self.main_bitmap = None;
                        self.uploaded_key = Some((path.clone(), frame_idx));
                        self.upload_error = Some(err);
                        return;
                    }
                }
            } else {
                (frame.width, frame.height, frame.bgra_premul.as_slice())
            };

        // 同尺寸动图播放优化：直接 CopyFromMemory 原地更新 GPU 纹理，零显存重分配开销
        if let Some(existing) = self.main_bitmap.as_ref() {
            let cur_px = unsafe { existing.GetPixelSize() };
            if cur_px.width == upload_w && cur_px.height == upload_h {
                let copy_res = unsafe {
                    existing.CopyFromMemory(
                        None,
                        upload_pixels.as_ptr() as *const _,
                        upload_w * 4,
                    )
                };
                if copy_res.is_ok() {
                    self.uploaded_key = Some((path.clone(), frame_idx));
                    self.upload_error = None;
                    return;
                }
            }
        }

        match self.create_gpu_bitmap(upload_w, upload_h, upload_pixels) {
            Ok(bmp) => {
                self.main_bitmap = Some(bmp);
                self.uploaded_key = Some((path.clone(), frame_idx));
                self.upload_error = None;
            }
            Err(err) => {
                self.main_bitmap = None;
                self.uploaded_key = Some((path.clone(), frame_idx));
                self.upload_error = Some(DecodeError::GpuUploadFailed(err));
            }
        }
    }

    fn create_gpu_bitmap(&self, w: u32, h: u32, bgra_premul: &[u8]) -> Result<ID2D1Bitmap1, String> {
        let props = D2D1_BITMAP_PROPERTIES1 {
            pixelFormat: D2D1_PIXEL_FORMAT {
                format: DXGI_FORMAT_B8G8R8A8_UNORM,
                alphaMode: D2D1_ALPHA_MODE_PREMULTIPLIED,
            },
            dpiX: 96.0,
            dpiY: 96.0,
            bitmapOptions: D2D1_BITMAP_OPTIONS_NONE,
            ..Default::default()
        };
        unsafe {
            self.d2d_ctx
                .CreateBitmap(
                    D2D_SIZE_U {
                        width: w,
                        height: h,
                    },
                    Some(bgra_premul.as_ptr() as *const _),
                    w * 4,
                    &props,
                )
                .map_err(|e| format!("{e}"))
        }
    }

    pub fn render(&mut self, params: &RenderFrameParams<'_>) -> Result<(), RenderError> {
        if let Some(img) = params.image {
            let safe_idx = params.frame_index.min(img.frames.len().saturating_sub(1));
            self.ensure_main_frame_uploaded(&img.path, safe_idx, &img.frames[safe_idx]);
        }

        // 刷新当前可见胶片缩略图 LRU 时间戳，保护可见窗口不被后台淘汰
        for item in &params.layout.filmstrip_items {
            if let Some(p) = params.sibling_paths.get(item.index) {
                if let Some(ut) = self.thumb_bitmaps.get_mut(p) {
                    self.thumb_lru_tick = self.thumb_lru_tick.wrapping_add(1);
                    ut.last_used_tick = self.thumb_lru_tick;
                }
            }
        }

        // ─── 1. 原版 Picasa 灵魂打开弹入 (`Opening` 420ms) 与两段式实体坍缩关闭 (`Closing` 340ms) ───
        let open_eased = ease_out_picasa_open(params.open_progress);
        let open_backdrop = ease_out_quad(params.backdrop_open_progress);
        let open_img_alpha = if !params.open_anim_armed {
            0.0
        } else if params.open_progress >= 0.11 {
            1.0
        } else {
            (0.28 + 0.72 * (params.open_progress / 0.11)).clamp(0.0, 1.0)
        };
        let open_scale_mul = OPEN_START_SCALE_RATIO + (1.0 - OPEN_START_SCALE_RATIO) * open_eased;

        let is_closing = params.close_progress > 0.0001 || params.close_snapshot.is_some();
        let close_t = params.close_progress.clamp(0.0, 1.0);
        let close_img_t = if is_closing {
            (close_t / CLOSE_IMAGE_IMPLODE_RATIO).clamp(0.0, 1.0)
        } else {
            0.0
        };
        let close_implode_k = close_implode_curve(close_img_t);
        let close_img_fade = if is_closing {
            if close_img_t < 0.78 {
                1.0
            } else {
                let tail = ((close_img_t - 0.78) / 0.22).clamp(0.0, 1.0);
                (1.0 - tail * tail).clamp(0.0, 1.0)
            }
        } else {
            1.0
        };
        let close_backdrop_fade = if is_closing {
            let bt = ((close_t - CLOSE_BACKDROP_START_RATIO) / (1.0 - CLOSE_BACKDROP_START_RATIO))
                .clamp(0.0, 1.0);
            1.0 - ease_out_quad(bt)
        } else {
            1.0
        };
        let hud_close_fade = if is_closing {
            let h = (1.0 - close_t / CLOSE_HUD_FADEOUT_RATIO).clamp(0.0, 1.0);
            h * h
        } else {
            1.0
        };

        let backdrop_life_alpha = (open_backdrop * close_backdrop_fade).clamp(0.0, 1.0);
        let img_life_alpha = (open_img_alpha * close_img_fade).clamp(0.0, 1.0);
        let hud_life_alpha = (open_backdrop * hud_close_fade).clamp(0.0, 1.0);

        // ─── 2. 切图微位移交叉淡化 vs 幻灯片 1050ms 光学双图层叠化 ──────────────
        let slide_active =
            !is_closing && params.slide_progress < 0.999 && params.slide_dir.abs() > 0.01;
        let slide_eased = if params.slide_is_crossfade {
            smoothstep_s_curve(params.slide_progress)
        } else {
            ease_out_quart(params.slide_progress)
        };
        let slide_offset_span = if params.slide_fast_repeat {
            SLIDE_FAST_OFFSET_DIP
        } else {
            SLIDE_MICRO_OFFSET_DIP
        };

        let effective_error = params.error.or(self.upload_error.as_ref());

        unsafe {
            self.d2d_ctx.BeginDraw();
            self.d2d_ctx.SetTransform(&Matrix3x2::identity());

            // ─── 图层 1：全屏半透明桌面穿透遮罩
            if let Some(sim_bg) = params.simulated_desktop_bg {
                self.d2d_ctx.Clear(Some(&color_f(sim_bg)));
            } else {
                self.d2d_ctx.Clear(Some(&D2D1_COLOR_F {
                    r: 0.0,
                    g: 0.0,
                    b: 0.0,
                    a: 0.0,
                }));
            }

            let base_bg = if params.is_windowed {
                match params.backdrop {
                    BackdropMode::PicasaGlass => COLOR_WINDOWED_BG_DARK,
                    BackdropMode::SolidDark => COLOR_WINDOWED_BG_SOLID_DARK,
                    BackdropMode::SoftLight => COLOR_WINDOWED_BG_LIGHT,
                }
            } else {
                match params.backdrop {
                    BackdropMode::PicasaGlass => COLOR_BACKDROP_GLASS,
                    BackdropMode::SolidDark => COLOR_BACKDROP_SOLID_DARK,
                    BackdropMode::SoftLight => COLOR_BACKDROP_LIGHT,
                }
            };
            let eff_dim = params
                .close_snapshot
                .map(|s| s.start_cinema_dim)
                .unwrap_or(params.cinema_dim);

            let prefer_fast_linear = is_closing
                || slide_active
                || (open_scale_mul - 1.0).abs() > 0.001
                || params.camera.is_camera_animating()
                || (eff_dim > 0.001 && eff_dim < 0.999);

            // ─── 进出幻灯片整屏双场景平滑交叉叠化（全程标准 SOURCE_OVER，彻底杜绝透明 PNG 过曝亮晕与棋盘格突变）───
            let portal_dissolve_active = !is_closing
                && !slide_active
                && eff_dim > 0.001
                && eff_dim < 0.999
                && params.slideshow_portal.is_some();

            if portal_dissolve_active {
                let portal = params.slideshow_portal.unwrap();
                let (view_pose, ss_pose) = if params.is_slideshow {
                    (
                        portal,
                        SlideshowPortalSnapshot {
                            zoom: params.camera.zoom,
                            pan_x: params.camera.pan_x,
                            pan_y: params.camera.pan_y,
                            rotation_deg: params.camera.visual_rotation_deg,
                            flip_x: params.camera.visual_flip_x,
                            flip_y: params.camera.visual_flip_y,
                        },
                    )
                } else {
                    (
                        SlideshowPortalSnapshot {
                            zoom: params.camera.zoom,
                            pan_x: params.camera.pan_x,
                            pan_y: params.camera.pan_y,
                            rotation_deg: params.camera.visual_rotation_deg,
                            flip_x: params.camera.visual_flip_x,
                            flip_y: params.camera.visual_flip_y,
                        },
                        portal,
                    )
                };

                let dim_clamped = eff_dim.clamp(0.0, 1.0);
                let checker_fade = (1.0 - dim_clamped).clamp(0.0, 1.0);

                // 1) 底层常规看图场景（半透明雾灰底 + 原位图 + 关闭钮 + 前 220ms 柔和隐退底栏）
                self.brush
                    .SetColor(&color_alpha(base_bg, backdrop_life_alpha));
                self.d2d_ctx.FillRectangle(
                    &D2D_RECT_F {
                        left: 0.0,
                        top: 0.0,
                        right: params.view_w,
                        bottom: params.view_h,
                    },
                    &self.brush,
                );

                if let (Some(img), Some(bmp)) = (params.image, self.main_bitmap.as_ref()) {
                    let dip_per_px = params.camera.dip_per_image_pixel(view_pose.zoom);
                    self.draw_image_surface(
                        bmp,
                        img.width as f32,
                        img.height as f32,
                        img.has_alpha,
                        view_pose.rotation_deg,
                        view_pose.flip_x,
                        view_pose.flip_y,
                        view_pose.zoom,
                        dip_per_px,
                        params.view_w * 0.5 + view_pose.pan_x,
                        params.view_h * 0.5 + view_pose.pan_y,
                        1.0,
                        false,
                        0.0,
                        checker_fade,
                        true,
                        params.dpi,
                    );
                } else if let Some(tp) = params.fallback_thumb_path {
                    if let Some(ut) = self.thumb_bitmaps.get(tp) {
                        let bmp_sz = ut.bitmap.GetSize();
                        let ow = ut.orig_w as f32;
                        let ratio_w = ow / bmp_sz.width.max(1.0);
                        let dip_per_orig = params.camera.dip_per_image_pixel(view_pose.zoom);
                        self.draw_image_surface(
                            &ut.bitmap,
                            bmp_sz.width.max(1.0),
                            bmp_sz.height.max(1.0),
                            false,
                            view_pose.rotation_deg,
                            view_pose.flip_x,
                            view_pose.flip_y,
                            view_pose.zoom,
                            dip_per_orig * ratio_w,
                            params.view_w * 0.5 + view_pose.pan_x,
                            params.view_h * 0.5 + view_pose.pan_y,
                            1.0,
                            false,
                            0.0,
                            0.0,
                            true,
                            params.dpi,
                        );
                    }
                }

                if !params.is_windowed {
                    self.draw_corner_close_button(params, hud_life_alpha);
                }

                let bar_fade = (1.0 - (dim_clamped / 0.24).clamp(0.0, 1.0)).powf(1.4);
                let bar_alpha = (hud_life_alpha * bar_fade).clamp(0.0, 1.0);
                if bar_alpha > 0.015 {
                    if !params.is_slideshow {
                        self.draw_bottom_status_text(params, bar_alpha);
                    }
                    self.draw_anim_bar(params, bar_alpha);
                    self.draw_toolbar_with_integrated_filmstrip(params, bar_alpha);
                }

                // 2) 顶层全屏影院暗幕与满屏图：严格保持 SOURCE_OVER 混合，消除透明 PNG 过曝亮晕与 Alpha 截断
                self.brush.SetColor(&D2D1_COLOR_F {
                    r: 0.0,
                    g: 0.0,
                    b: 0.0,
                    a: dim_clamped,
                });
                self.d2d_ctx.FillRectangle(
                    &D2D_RECT_F {
                        left: 0.0,
                        top: 0.0,
                        right: params.view_w,
                        bottom: params.view_h,
                    },
                    &self.brush,
                );

                if let (Some(img), Some(bmp)) = (params.image, self.main_bitmap.as_ref()) {
                    let dip_per_px = params.camera.dip_per_image_pixel(ss_pose.zoom);
                    self.draw_image_surface(
                        bmp,
                        img.width as f32,
                        img.height as f32,
                        img.has_alpha,
                        ss_pose.rotation_deg,
                        ss_pose.flip_x,
                        ss_pose.flip_y,
                        ss_pose.zoom,
                        dip_per_px,
                        params.view_w * 0.5 + ss_pose.pan_x,
                        params.view_h * 0.5 + ss_pose.pan_y,
                        dim_clamped,
                        false,
                        1.0,
                        0.0,
                        true,
                        params.dpi,
                    );
                } else if let Some(err) = effective_error {
                    if hud_life_alpha > 0.02 {
                        let s = params.lang.strings();
                        let detail = s.format_decode_error(err);
                        self.draw_center_status(
                            params.view_w,
                            params.view_h,
                            s.status_cannot_open,
                            &detail,
                            true,
                            0.0,
                            hud_life_alpha,
                        );
                    }
                } else if let Some(tp) = params.fallback_thumb_path {
                    if let Some(ut) = self.thumb_bitmaps.get(tp) {
                        let bmp_sz = ut.bitmap.GetSize();
                        let ow = ut.orig_w as f32;
                        let ratio_w = ow / bmp_sz.width.max(1.0);
                        let dip_per_orig = params.camera.dip_per_image_pixel(ss_pose.zoom);
                        self.draw_image_surface(
                            &ut.bitmap,
                            bmp_sz.width.max(1.0),
                            bmp_sz.height.max(1.0),
                            false,
                            ss_pose.rotation_deg,
                            ss_pose.flip_x,
                            ss_pose.flip_y,
                            ss_pose.zoom,
                            dip_per_orig * ratio_w,
                            params.view_w * 0.5 + ss_pose.pan_x,
                            params.view_h * 0.5 + ss_pose.pan_y,
                            dim_clamped,
                            false,
                            1.0,
                            0.0,
                            true,
                            params.dpi,
                        );
                    } else if params.show_loading_box && hud_life_alpha > 0.02 {
                        let s = params.lang.strings();
                        let msg = params.loading_name.unwrap_or(s.status_loading_default);
                        let spin_sec = params
                            .now
                            .duration_since(params.open_started_at)
                            .as_secs_f32();
                        self.draw_center_status(
                            params.view_w,
                            params.view_h,
                            s.status_loading_title,
                            msg,
                            false,
                            spin_sec,
                            hud_life_alpha,
                        );
                    }
                } else if params.show_loading_box && hud_life_alpha > 0.02 {
                    let s = params.lang.strings();
                    let msg = params.loading_name.unwrap_or(s.status_loading_default);
                    let spin_sec = params
                        .now
                        .duration_since(params.open_started_at)
                        .as_secs_f32();
                    self.draw_center_status(
                        params.view_w,
                        params.view_h,
                        s.status_loading_title,
                        msg,
                        false,
                        spin_sec,
                        hud_life_alpha,
                    );
                }

                return self.finish_and_present();
            }

            let bg_rgba = lerp_rgba(base_bg, [0.0, 0.0, 0.0, 1.0], eff_dim);
            self.brush
                .SetColor(&color_alpha(bg_rgba, backdrop_life_alpha));
            self.d2d_ctx.FillRectangle(
                &D2D_RECT_F {
                    left: 0.0,
                    top: 0.0,
                    right: params.view_w,
                    bottom: params.view_h,
                },
                &self.brush,
            );

            // 幻灯片纯黑幕下仅对两张均不透明的图启用光学 ADD 叠化；任一图含透明通道统一走 SOURCE_OVER 防过曝
            let in_has_alpha = params.image.map(|i| i.has_alpha).unwrap_or(false);
            let out_has_alpha = self
                .outgoing_slide
                .as_ref()
                .map(|o| o.has_alpha)
                .unwrap_or(false);
            let use_additive_dissolve = slide_active
                && params.slide_is_crossfade
                && eff_dim >= 0.99
                && self.outgoing_slide.is_some()
                && !in_has_alpha
                && !out_has_alpha;
            if use_additive_dissolve {
                self.d2d_ctx.SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
            }

            // ─── 图层 2a：离场旧图
            if slide_active {
                if let Some(out) = self.outgoing_slide.as_ref() {
                    let (out_dx, out_scale, out_opacity) = if params.slide_is_crossfade {
                        (0.0, 1.0, (1.0 - slide_eased) * img_life_alpha)
                    } else {
                        let dx = -params.slide_dir * slide_offset_span * slide_eased;
                        let op = (1.0 - params.slide_progress).powf(1.85) * img_life_alpha;
                        (dx, 1.0, op)
                    };
                    if out_opacity > 0.008 {
                        let out_dip_per_px = out.zoom / out.dpi_scale.max(0.5);
                        self.draw_image_surface(
                            &out.bitmap,
                            out.img_w,
                            out.img_h,
                            out.has_alpha,
                            out.rotation_deg,
                            out.flip_x,
                            out.flip_y,
                            out.zoom * out_scale,
                            out_dip_per_px * out_scale,
                            params.view_w * 0.5 + out.pan_x + out_dx,
                            params.view_h * 0.5 + out.pan_y,
                            out_opacity,
                            false,
                            eff_dim,
                            (1.0 - eff_dim).clamp(0.0, 1.0),
                            prefer_fast_linear,
                            params.dpi,
                        );
                    }
                }
            }

            // ─── 图层 2b：中央无框悬浮相片画布
            let (in_dx, in_slide_scale, in_opacity) = if slide_active {
                if params.slide_is_crossfade {
                    (0.0, 1.0, slide_eased * img_life_alpha)
                } else {
                    (
                        params.slide_dir * slide_offset_span * (1.0 - slide_eased),
                        1.0,
                        (0.28 + 0.72 * slide_eased) * img_life_alpha,
                    )
                }
            } else {
                (0.0, 1.0, img_life_alpha)
            };

            let (eff_zoom, raw_cx, raw_cy) = if let Some(snap) = params.close_snapshot {
                let z = snap.start_zoom + (snap.target_end_zoom - snap.start_zoom) * close_implode_k;
                let px = snap.start_pan_x * (1.0 - close_implode_k);
                let py = snap.start_pan_y * (1.0 - close_implode_k);
                (z, params.view_w * 0.5 + px, params.view_h * 0.5 + py)
            } else {
                let z = params.camera.zoom * open_scale_mul * in_slide_scale;
                let cx = params.view_w * 0.5 + params.camera.pan_x + in_dx;
                let cy = params.view_h * 0.5 + params.camera.pan_y;
                (z, cx, cy)
            };

            if let (Some(img), Some(bmp)) = (params.image, self.main_bitmap.as_ref()) {
                if in_opacity > 0.008 {
                    let dip_per_px = params.camera.dip_per_image_pixel(eff_zoom);
                    let allow_exact_1to1 = !slide_active
                        && !is_closing
                        && !params.is_slideshow
                        && !params.camera.is_camera_animating()
                        && (open_scale_mul - 1.0).abs() < 0.001;
                    self.draw_image_surface(
                        bmp,
                        img.width as f32,
                        img.height as f32,
                        img.has_alpha,
                        params.camera.visual_rotation_deg,
                        params.camera.visual_flip_x,
                        params.camera.visual_flip_y,
                        eff_zoom,
                        dip_per_px,
                        raw_cx,
                        raw_cy,
                        in_opacity,
                        allow_exact_1to1,
                        eff_dim,
                        (1.0 - eff_dim).clamp(0.0, 1.0),
                        prefer_fast_linear,
                        params.dpi,
                    );
                }
            } else if let Some(err) = effective_error {
                if use_additive_dissolve {
                    self.d2d_ctx
                        .SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
                }
                if hud_life_alpha > 0.02 {
                    let s = params.lang.strings();
                    let detail = s.format_decode_error(err);
                    self.draw_center_status(
                        params.view_w,
                        params.view_h,
                        s.status_cannot_open,
                        &detail,
                        true,
                        0.0,
                        hud_life_alpha,
                    );
                }
            } else if let Some(tp) = params.fallback_thumb_path {
                if let Some(ut) = self.thumb_bitmaps.get(tp) {
                    if in_opacity > 0.008 {
                        let ow = ut.orig_w as f32;
                        let oh = ut.orig_h as f32;
                        let bmp_sz = ut.bitmap.GetSize();
                        let ratio_w = ow / bmp_sz.width.max(1.0);
                        // 若用户已在缩略图占位期缩放/平移，则直接按当前相机绘制，保证手动姿态 100% 保留
                        let (z, cx, cy) = if !params.camera.is_fit_mode || params.close_snapshot.is_some() {
                            (eff_zoom, raw_cx, raw_cy)
                        } else {
                            let fit_mode = params.camera.fit_layout_mode;
                            let fit_z = ViewportCamera::compute_fit_zoom_for_mode(
                                ow,
                                oh,
                                params.camera.rotation,
                                params.view_w,
                                params.view_h,
                                params.camera.dpi_scale,
                                fit_mode,
                            );
                            let py = ViewportCamera::default_fit_pan_y(
                                ow,
                                oh,
                                params.camera.rotation,
                                fit_z,
                                params.view_h,
                                params.camera.dpi_scale,
                                fit_mode,
                            );
                            (
                                fit_z * open_scale_mul * in_slide_scale,
                                params.view_w * 0.5 + in_dx,
                                params.view_h * 0.5 + py * (1.0 - close_implode_k),
                            )
                        };
                        let dip_per_orig_px = params.camera.dip_per_image_pixel(z);
                        self.draw_image_surface(
                            &ut.bitmap,
                            bmp_sz.width.max(1.0),
                            bmp_sz.height.max(1.0),
                            false,
                            params.camera.visual_rotation_deg,
                            params.camera.visual_flip_x,
                            params.camera.visual_flip_y,
                            z,
                            dip_per_orig_px * ratio_w,
                            cx,
                            cy,
                            in_opacity,
                            false,
                            eff_dim,
                            0.0,
                            true,
                            params.dpi,
                        );
                    }
                } else if params.show_loading_box && hud_life_alpha > 0.02 {
                    if use_additive_dissolve {
                        self.d2d_ctx
                            .SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
                    }
                    let s = params.lang.strings();
                    let msg = params.loading_name.unwrap_or(s.status_loading_default);
                    let spin_sec = params
                        .now
                        .duration_since(params.open_started_at)
                        .as_secs_f32();
                    self.draw_center_status(
                        params.view_w,
                        params.view_h,
                        s.status_loading_title,
                        msg,
                        false,
                        spin_sec,
                        hud_life_alpha,
                    );
                }
            } else if params.show_loading_box && hud_life_alpha > 0.02 {
                if use_additive_dissolve {
                    self.d2d_ctx
                        .SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
                }
                let s = params.lang.strings();
                let msg = params.loading_name.unwrap_or(s.status_loading_default);
                let spin_sec = params
                    .now
                    .duration_since(params.open_started_at)
                    .as_secs_f32();
                self.draw_center_status(
                    params.view_w,
                    params.view_h,
                    s.status_loading_title,
                    msg,
                    false,
                    spin_sec,
                    hud_life_alpha,
                );
            }

            if use_additive_dissolve {
                self.d2d_ctx
                    .SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            }

            // ─── 图层 3：Ctrl + 左键框选局部特写框与右下角缩放迷你导航框
            let reg_cinema_fade =
                (1.0 - smoothstep_s_curve((eff_dim / 0.36).clamp(0.0, 1.0))).clamp(0.0, 1.0);
            let reg_hud_alpha = (hud_life_alpha * reg_cinema_fade).clamp(0.0, 1.0);

            if let Some(sel) = params.selection_box {
                if reg_hud_alpha > 0.05 {
                    self.draw_selection_box(sel);
                }
            }

            if reg_hud_alpha > 0.02 {
                if let (Some(outer), Some(inner), Some(img), Some(bmp)) = (
                    params.layout.minimap_rect,
                    params.layout.minimap_image_rect,
                    params.image,
                    self.main_bitmap.as_ref(),
                ) {
                    self.draw_minimap(
                        img,
                        bmp,
                        params.camera,
                        params.view_w,
                        params.view_h,
                        outer,
                        inner,
                        reg_hud_alpha,
                    );
                }
            }

            // ─── 图层 4：按 I 键呼出的左上角真 EXIF 玻璃卡
            if params.show_info && reg_hud_alpha > 0.02 {
                if let (Some(card_rc), Some(img)) = (params.layout.info_card_rect, params.image) {
                    self.draw_info_card(card_rc, img, params.dpi, params.lang, reg_hud_alpha);
                }
            }

            // ─── 图层 4b：原版 Picasa 招牌正中央悬浮缩放倍率胶囊
            if !is_closing && reg_hud_alpha > 0.02 && params.zoom_badge_alpha > 0.01 {
                let zoom_pct = (params.camera.target_zoom * 100.0).round().max(1.0) as i32;
                self.draw_zoom_badge(
                    params.view_w,
                    params.view_h,
                    zoom_pct,
                    (params.zoom_badge_alpha * reg_hud_alpha).clamp(0.0, 1.0),
                );
            }

            // ─── 图层 5：右上角标志性费茨定律 40 DIP 关闭徽章（仅浮层态绘制，小窗口态已有系统标题栏关闭钮故隐藏）
            if !params.is_windowed && reg_hud_alpha > 0.015 {
                self.draw_corner_close_button(params, reg_hud_alpha);
            }

            // ─── 图层 6：常规底部悬浮 HUD
            if reg_hud_alpha > 0.015 {
                let bottom_y_off =
                    (1.0 - reg_cinema_fade) * 14.0 + (1.0 - hud_close_fade) * 20.0;
                if bottom_y_off.abs() > 0.05 {
                    self.d2d_ctx
                        .SetTransform(&Matrix3x2::translation(0.0, bottom_y_off));
                }
                if !params.is_slideshow {
                    self.draw_bottom_status_text(params, reg_hud_alpha);
                }
                self.draw_anim_bar(params, reg_hud_alpha);
                self.draw_toolbar_with_integrated_filmstrip(params, reg_hud_alpha);
                if bottom_y_off.abs() > 0.05 {
                    self.d2d_ctx.SetTransform(&Matrix3x2::identity());
                }
            }

            // ─── 图层 7：瞬态提示条 Toast
            if !is_closing {
                if let Some(toast) = params.toast_text {
                    self.draw_toast(params.view_w, params.view_h, toast);
                }
            }

            self.finish_and_present()
        }
    }

    unsafe fn finish_and_present(&self) -> Result<(), RenderError> {
        self.d2d_ctx
            .EndDraw(None, None)
            .map_err(|e| classify_win_err("D2D EndDraw 失败", e))?;
        if let Some(sc) = self.swap_chain.as_ref() {
            let hr = sc.Present(1, Default::default());
            if hr.is_err() {
                if is_device_lost_hr(hr) {
                    return Err(RenderError::DeviceLost(format!(
                        "DXGI Present 设备丢失: {:?}",
                        hr
                    )));
                }
                return Err(RenderError::Other(format!("DXGI Present 失败: {:?}", hr)));
            }
        } else {
            self.d3d_ctx.Flush();
        }
        Ok(())
    }

    /// 基于精确 2D 盒式高斯积分贴图的 9 宫格硬件加速投影（物理等价 2D Gaussian Blur，
    /// 四角绝对圆润无任何台阶痕，且在 4K 120Hz 任意角度旋转与弹入坍缩全程每帧耗时 < 0.03ms）
    unsafe fn draw_gaussian_box_shadow_local(
        &self,
        left: f32,
        top: f32,
        right: f32,
        bottom: f32,
        blur_spread_dip: f32,
        offset_y_dip: f32,
        opacity: f32,
    ) {
        let op = opacity.clamp(0.0, 1.0);
        if op <= 0.005 {
            return;
        }
        // 纹理尺寸 192x192，其中外扩羽化带 56px，内缘过渡 16px，中心实心区 48x48
        const TEX_SZ: f32 = 192.0;
        const EDGE_SRC: f32 = 72.0; // 56px 外扩 + 16px 内收，刚好覆盖完整 erf 过渡带
        const OUTER_RATIO: f32 = 56.0 / 72.0;
        const INNER_RATIO: f32 = 16.0 / 72.0;

        let w = (right - left).max(1.0);
        let h = (bottom - top).max(1.0);
        let outer = blur_spread_dip.max(4.0);
        let max_inner_x = (w * 0.48).max(0.5);
        let max_inner_y = (h * 0.48).max(0.5);
        let inner_x = (outer * (INNER_RATIO / OUTER_RATIO)).min(max_inner_x);
        let inner_y = (outer * (INNER_RATIO / OUTER_RATIO)).min(max_inner_y);

        let dx = [
            left - outer,
            left + inner_x,
            right - inner_x,
            right + outer,
        ];
        let dy = [
            top + offset_y_dip - outer,
            top + offset_y_dip + inner_y,
            bottom + offset_y_dip - inner_y,
            bottom + offset_y_dip + outer,
        ];
        let sx = [0.0f32, EDGE_SRC, TEX_SZ - EDGE_SRC, TEX_SZ];
        let sy = [0.0f32, EDGE_SRC, TEX_SZ - EDGE_SRC, TEX_SZ];

        for row in 0..3 {
            if dy[row + 1] <= dy[row] + 0.01 {
                continue;
            }
            for col in 0..3 {
                if dx[col + 1] <= dx[col] + 0.01 {
                    continue;
                }
                let dst = D2D_RECT_F {
                    left: dx[col],
                    top: dy[row],
                    right: dx[col + 1],
                    bottom: dy[row + 1],
                };
                let src = D2D_RECT_F {
                    left: sx[col],
                    top: sy[row],
                    right: sx[col + 1],
                    bottom: sy[row + 1],
                };
                self.d2d_ctx.DrawBitmap(
                    &self.shadow_9slice,
                    Some(&dst),
                    op,
                    D2D1_INTERPOLATION_MODE_LINEAR,
                    Some(&src),
                    None,
                );
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn draw_image_surface(
        &self,
        bmp: &ID2D1Bitmap1,
        img_w: f32,
        img_h: f32,
        has_alpha: bool,
        rotation_deg: f32,
        visual_flip_x: f32,
        visual_flip_y: f32,
        zoom: f32,
        dip_per_px: f32,
        raw_cx: f32,
        raw_cy: f32,
        opacity: f32,
        allow_exact_1to1: bool,
        cinema_dim: f32,
        checker_fade: f32,
        prefer_fast_linear: bool,
        dpi: f32,
    ) {
        let bmp_sz = bmp.GetSize();
        let bmp_w = bmp_sz.width.max(1.0);
        let bmp_h = bmp_sz.height.max(1.0);
        let is_downsampled = (bmp_w - img_w).abs() > 0.5 || (bmp_h - img_h).abs() > 0.5;

        let dpi_scale = (dpi / 96.0).max(0.5);
        let is_exact_1to1 = allow_exact_1to1 && !is_downsampled && (zoom - 1.0).abs() < 0.005;
        let (cx, cy) = if is_exact_1to1 {
            let swapped = ((rotation_deg.round() as i32).rem_euclid(180) - 90).abs() < 2;
            let eff_w = if swapped { img_h } else { img_w };
            let eff_h = if swapped { img_w } else { img_h };
            let left_phys = (raw_cx * dpi_scale - eff_w * 0.5).round();
            let top_phys = (raw_cy * dpi_scale - eff_h * 0.5).round();
            (
                (left_phys + eff_w * 0.5) / dpi_scale,
                (top_phys + eff_h * 0.5) / dpi_scale,
            )
        } else {
            (raw_cx, raw_cy)
        };

        let half_w_dip = (img_w * dip_per_px * 0.5).max(0.5);
        let half_h_dip = (img_h * dip_per_px * 0.5).max(0.5);

        let fx = if visual_flip_x.abs() < 0.02 {
            0.02 * visual_flip_x.signum()
        } else {
            visual_flip_x
        };
        let fy = if visual_flip_y.abs() < 0.02 {
            0.02 * visual_flip_y.signum()
        } else {
            visual_flip_y
        };
        let dip_orient_mat = matrix_scale(fx, fy)
            * Matrix3x2::rotation(rotation_deg, 0.0, 0.0)
            * Matrix3x2::translation(cx, cy);

        // 原版 Picasa 纯净无白边悬浮质感：双层高斯 9 宫格落影紧贴照片真实边缘（柔和环境投影 + 贴边接触投影）
        let frame_vis = (1.0 - cinema_dim).clamp(0.0, 1.0);
        if frame_vis > 0.01 {
            let shadow_mat = matrix_scale(fx.abs(), fy.abs())
                * Matrix3x2::rotation(rotation_deg, 0.0, 0.0)
                * Matrix3x2::translation(cx, cy);
            self.d2d_ctx.SetTransform(&shadow_mat);
            let base_op = (opacity.powf(1.15) * frame_vis).clamp(0.0, 1.0);
            self.draw_gaussian_box_shadow_local(
                -half_w_dip,
                -half_h_dip,
                half_w_dip,
                half_h_dip,
                PHOTO_SHADOW_AMBIENT_SPREAD_DIP,
                PHOTO_SHADOW_AMBIENT_OFFSET_Y_DIP,
                PHOTO_SHADOW_AMBIENT_OPACITY * base_op,
            );
            self.draw_gaussian_box_shadow_local(
                -half_w_dip,
                -half_h_dip,
                half_w_dip,
                half_h_dip,
                PHOTO_SHADOW_CONTACT_SPREAD_DIP,
                PHOTO_SHADOW_CONTACT_OFFSET_Y_DIP,
                PHOTO_SHADOW_CONTACT_OPACITY * base_op,
            );
        }

        let tex_mat = matrix_scale(dip_per_px * fx, dip_per_px * fy)
            * Matrix3x2::rotation(rotation_deg, 0.0, 0.0)
            * Matrix3x2::translation(cx, cy);
        self.d2d_ctx.SetTransform(&tex_mat);

        let dst_px_rect = D2D_RECT_F {
            left: -img_w * 0.5,
            top: -img_h * 0.5,
            right: img_w * 0.5,
            bottom: img_h * 0.5,
        };

        let eff_checker_alpha = (0.94 * opacity * checker_fade.clamp(0.0, 1.0)).clamp(0.0, 1.0);
        if has_alpha && eff_checker_alpha > 0.01 {
            let inv_s = (1.0 / dip_per_px.max(0.01)).clamp(0.02, 50.0);
            self.checker_brush
                .SetTransform(&matrix_scale(inv_s, inv_s));
            self.checker_brush.SetOpacity(eff_checker_alpha);
            self.d2d_ctx
                .FillRectangle(&dst_px_rect, &self.checker_brush);
        }

        // src_rect 严格取实际上传的 GPU 纹理尺寸（兼容超长图降采样后按原始比例完整绘制）
        let src_rect = D2D_RECT_F {
            left: 0.0,
            top: 0.0,
            right: bmp_w,
            bottom: bmp_h,
        };
        let interp = if is_exact_1to1 || (!is_downsampled && zoom >= 4.0) {
            D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
        } else if prefer_fast_linear {
            D2D1_INTERPOLATION_MODE_LINEAR
        } else {
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
        };
        self.d2d_ctx.DrawBitmap(
            bmp,
            Some(&dst_px_rect),
            opacity.clamp(0.0, 1.0),
            interp,
            Some(&src_rect),
            None,
        );

        // 透明素材保持克制微亮轮廓线；不透明图片彻底无白边与外框线，干净贴合高斯边缘阴影
        if frame_vis > 0.01 && has_alpha {
            self.d2d_ctx.SetTransform(&dip_orient_mat);
            let border_rc = D2D_RECT_F {
                left: -half_w_dip + 0.5,
                top: -half_h_dip + 0.5,
                right: half_w_dip - 0.5,
                bottom: half_h_dip - 0.5,
            };
            self.brush.SetColor(&D2D1_COLOR_F {
                r: 1.0,
                g: 1.0,
                b: 1.0,
                a: 0.24 * opacity * frame_vis,
            });
            self.d2d_ctx
                .DrawRectangle(&border_rc, &self.brush, 1.2, None);
        }

        self.d2d_ctx.SetTransform(&Matrix3x2::identity());
    }

    /// 原版 Picasa 招牌正中央缩放倍率圆角胶囊（如 `100%` / `6398%`，高分屏等宽锐利数字，停止缩放后柔和淡出）
    unsafe fn draw_zoom_badge(&self, view_w: f32, view_h: f32, zoom_pct: i32, alpha: f32) {
        let label = format!("{zoom_pct}%");
        let box_w = (label.len() as f32 * 10.5 + 32.0).clamp(84.0, 168.0);
        let box_h = 36.0f32;
        let left = (view_w - box_w) * 0.5;
        let top = (view_h - box_h) * 0.5;

        // 柔和环境投影确保亮图与暗图上皆清晰浮起
        self.brush.SetColor(&color_alpha([0.0, 0.0, 0.0, 0.34], alpha));
        self.d2d_ctx.FillRoundedRectangle(
            &D2D1_ROUNDED_RECT {
                rect: D2D_RECT_F {
                    left: left - 2.0,
                    top: top + 1.0,
                    right: left + box_w + 2.0,
                    bottom: top + box_h + 4.0,
                },
                radiusX: 9.0,
                radiusY: 9.0,
            },
            &self.brush,
        );

        let rr = D2D1_ROUNDED_RECT {
            rect: D2D_RECT_F {
                left,
                top,
                right: left + box_w,
                bottom: top + box_h,
            },
            radiusX: 7.5,
            radiusY: 7.5,
        };
        self.brush.SetColor(&color_alpha([0.23, 0.24, 0.27, 0.86], alpha));
        self.d2d_ctx.FillRoundedRectangle(&rr, &self.brush);
        self.brush.SetColor(&color_alpha([1.0, 1.0, 1.0, 0.18], alpha));
        self.d2d_ctx
            .DrawRoundedRectangle(&rr, &self.brush, 1.0, None);

        let wt = to_wide(&label);
        self.brush.SetColor(&color_alpha([1.0, 1.0, 1.0, 0.98], alpha));
        self.d2d_ctx.DrawText(
            &wt[..wt.len() - 1],
            &self.fmt_zoom_badge,
            &rr.rect,
            &self.brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            Default::default(),
        );
    }

    unsafe fn draw_selection_box(&self, raw: RectF) {
        let r = raw.normalized();
        if r.width() < 2.0 || r.height() < 2.0 {
            return;
        }
        self.brush.SetColor(&color_alpha(COLOR_ACCENT, 0.16));
        self.d2d_ctx.FillRectangle(&to_d2d_rect(r), &self.brush);
        self.brush.SetColor(&color_alpha(COLOR_ACCENT, 0.92));
        self.d2d_ctx
            .DrawRectangle(&to_d2d_rect(r), &self.brush, 1.5, None);
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn draw_minimap(
        &self,
        img: &SharedImage,
        bmp: &ID2D1Bitmap1,
        camera: &ViewportCamera,
        view_w: f32,
        view_h: f32,
        outer: RectF,
        inner: RectF,
        alpha: f32,
    ) {
        let rr = D2D1_ROUNDED_RECT {
            rect: to_d2d_rect(outer),
            radiusX: 8.0,
            radiusY: 8.0,
        };
        self.brush.SetColor(&color_alpha(COLOR_PANEL_BG, alpha));
        self.d2d_ctx.FillRoundedRectangle(&rr, &self.brush);
        self.brush.SetColor(&color_alpha(COLOR_PANEL_BORDER, alpha));
        self.d2d_ctx
            .DrawRoundedRectangle(&rr, &self.brush, 1.0, None);

        let cx = inner.center_x();
        let cy = inner.center_y();
        let (eff_w, eff_h) = camera.effective_dims(img.width as f32, img.height as f32);
        let scale = (inner.width() / eff_w).min(inner.height() / eff_h);
        let fx = if camera.flip_x { -1.0 } else { 1.0 };
        let fy = if camera.flip_y { -1.0 } else { 1.0 };
        let mat = matrix_scale(scale * fx, scale * fy)
            * Matrix3x2::rotation(camera.rotation.degrees(), 0.0, 0.0)
            * Matrix3x2::translation(cx, cy);
        self.d2d_ctx.SetTransform(&mat);
        let dst = D2D_RECT_F {
            left: -(img.width as f32) * 0.5,
            top: -(img.height as f32) * 0.5,
            right: (img.width as f32) * 0.5,
            bottom: (img.height as f32) * 0.5,
        };
        self.d2d_ctx.DrawBitmap(
            bmp,
            Some(&dst),
            0.94 * alpha,
            D2D1_INTERPOLATION_MODE_LINEAR,
            None,
            None,
        );
        self.d2d_ctx.SetTransform(&Matrix3x2::identity());

        let norm = camera.normalized_visible_window(
            img.width as f32,
            img.height as f32,
            view_w,
            view_h,
        );
        let vp_rc = D2D_RECT_F {
            left: inner.left + norm.left * inner.width(),
            top: inner.top + norm.top * inner.height(),
            right: inner.left + norm.right * inner.width(),
            bottom: inner.top + norm.bottom * inner.height(),
        };
        self.brush.SetColor(&color_alpha(COLOR_ACCENT, 0.18 * alpha));
        self.d2d_ctx.FillRectangle(&vp_rc, &self.brush);
        self.brush.SetColor(&color_alpha(COLOR_ACCENT, alpha));
        self.d2d_ctx.DrawRectangle(&vp_rc, &self.brush, 1.6, None);
    }

    /// 底部正上方微文件名与按钮悬停提示共享浮标 (ss/statustext)
    unsafe fn draw_bottom_status_text(&self, params: &RenderFrameParams<'_>, alpha: f32) {
        let rc = params.layout.caption_rect;
        let tip_opt = params
            .layout
            .hovered_status_tip(params.hovered, params.lang);
        let status_line: String = if let Some(tip) = tip_opt {
            tip.to_string()
        } else if let HitTarget::FilmstripItem(h_idx) = params.hovered {
            if h_idx != params.current_index {
                params
                    .sibling_paths
                    .get(h_idx)
                    .and_then(|p| p.file_name())
                    .map(|s| s.to_string_lossy().to_string())
                    .unwrap_or_default()
            } else {
                self.format_default_status_text(params)
            }
        } else {
            self.format_default_status_text(params)
        };

        if status_line.is_empty() {
            return;
        }
        let wt = to_wide(&status_line);
        let is_tip_active = tip_opt.is_some();

        // 柔和双层微晕暗影，保障在亮白桌面/大理石照片底端依然清晰锐利可读
        for (ox, oy, a) in [(0.0f32, 1.4f32, 0.86f32), (0.0, 0.5, 0.52)] {
            self.brush.SetColor(&D2D1_COLOR_F {
                r: 0.0,
                g: 0.0,
                b: 0.0,
                a: a * alpha,
            });
            self.d2d_ctx.DrawText(
                &wt[..wt.len() - 1],
                &self.fmt_status_bold,
                &D2D_RECT_F {
                    left: rc.left + ox,
                    top: rc.top + oy,
                    right: rc.right + ox,
                    bottom: rc.bottom + oy,
                },
                &self.brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                Default::default(),
            );
        }

        let fg = if is_tip_active {
            [0.97, 0.98, 1.0, 1.0]
        } else {
            [0.84, 0.86, 0.89, 0.96]
        };
        self.brush.SetColor(&color_alpha(fg, alpha));
        self.d2d_ctx.DrawText(
            &wt[..wt.len() - 1],
            &self.fmt_status_bold,
            &to_d2d_rect(rc),
            &self.brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            Default::default(),
        );
    }

    fn format_default_status_text(&self, params: &RenderFrameParams<'_>) -> String {
        let name = params
            .sibling_paths
            .get(params.current_index)
            .and_then(|p| p.file_name())
            .map(|s| s.to_string_lossy().to_string())
            .unwrap_or_else(|| "Tools3000 Picasa Viewer".to_string());
        let total = params.sibling_paths.len().max(1);
        let idx = params.current_index + 1;

        if let Some(img) = params.image {
            let zoom_pct = (params.camera.zoom * 100.0).round() as i32;
            if !params.camera.is_fit_mode {
                if total > 1 {
                    format!("{} ({} × {})  ·  {}%  ·  {} / {}", name, img.width, img.height, zoom_pct, idx, total)
                } else {
                    format!("{} ({} × {})  ·  {}%", name, img.width, img.height, zoom_pct)
                }
            } else if total > 1 {
                format!("{} ({} × {})  ·  {} / {}", name, img.width, img.height, idx, total)
            } else {
                format!("{} ({} × {})", name, img.width, img.height)
            }
        } else if total > 1 {
            format!("{}  ·  {} / {}", name, idx, total)
        } else {
            name
        }
    }

    /// 右上角标志性费茨定律关闭徽章（1:1 复刻原版 Picasa 深炭灰无白框扇形 + 20 阶解析高斯模糊外阴影）
    unsafe fn draw_corner_close_button(&self, params: &RenderFrameParams<'_>, alpha: f32) {
        let h = params.close_hover_anim.clamp(0.0, 1.0);
        let r = params.layout.close_corner_radius + 3.0 * h;
        let origin = p2(params.view_w, 0.0);

        // 1. 真正像素级连续解析高斯外投影（ID2D1RadialGradientBrush 18 阶高斯核 exp(-u² * 4.5)，零台阶、零白边）
        // 阴影圆心相对扇形略微向左下偏移 (-2.2, +2.6)，精准复刻原版立体悬浮落影感！
        let shadow_spread = 28.0 + 6.0 * h;
        let shadow_r = r + shadow_spread;
        let shadow_center = p2(params.view_w - 2.2, 2.6);
        let inner_stop = ((r - 2.0) / shadow_r).clamp(0.25, 0.90);
        let shadow_rgb = lerp_rgba([0.0, 0.0, 0.0, 1.0], [0.22, 0.02, 0.03, 1.0], h * 0.55);
        let peak_a = 0.62 + 0.14 * h;

        let mut stops: Vec<(f32, [f32; 4])> = Vec::with_capacity(18);
        stops.push((0.0, [shadow_rgb[0], shadow_rgb[1], shadow_rgb[2], peak_a]));
        stops.push((
            inner_stop,
            [shadow_rgb[0], shadow_rgb[1], shadow_rgb[2], peak_a],
        ));
        const STEPS: usize = 16;
        for i in 1..=STEPS {
            let u = i as f32 / STEPS as f32;
            let pos = inner_stop + (1.0 - inner_stop) * u;
            // 高斯衰减核（兼顾近边深沉与远端丝滑消散至 0.0）
            let g = (-4.6 * u * u).exp() * (1.0 - u * u * 0.15);
            let a = if i == STEPS { 0.0 } else { (peak_a * g).max(0.0) };
            stops.push((pos, [shadow_rgb[0], shadow_rgb[1], shadow_rgb[2], a]));
        }
        if let Some(grad) = self.make_radial_gradient(
            shadow_center,
            p2(0.0, 0.0),
            shadow_r,
            shadow_r,
            &stops,
            alpha,
        ) {
            self.d2d_ctx.FillEllipse(
                &D2D1_ELLIPSE {
                    point: shadow_center,
                    radiusX: shadow_r,
                    radiusY: shadow_r,
                },
                &grad,
            );
        }

        // 2. 主体原版纯净深炭灰四分之一圆扇形（彻底去掉刺眼白边，悬停丝滑过渡至深绯红）
        let base_fill = lerp_rgba(
            [0.225, 0.232, 0.245, 0.92],
            [0.78, 0.19, 0.21, 0.96],
            h,
        );
        self.brush.SetColor(&color_alpha(base_fill, alpha));
        self.d2d_ctx.FillEllipse(
            &D2D1_ELLIPSE {
                point: origin,
                radiusX: r,
                radiusY: r,
            },
            &self.brush,
        );

        // 3. 极克制的深暗边缘过渡收口（无白色轮廓线，让扇形与高斯外阴影融为一体）
        let edge_col = lerp_rgba([0.08, 0.085, 0.095, 0.45], [0.55, 0.10, 0.12, 0.55], h);
        self.brush.SetColor(&color_alpha(edge_col, alpha));
        self.d2d_ctx.DrawEllipse(
            &D2D1_ELLIPSE {
                point: origin,
                radiusX: r - 0.4,
                radiusY: r - 0.4,
            },
            &self.brush,
            0.85,
            None,
        );

        // 4. 扇区光学视觉重心处的抗锯齿清脆白色 `×` 号（跨度 9.8 × 9.8 DIP，带柔和投影）
        let offset = 13.8 + 0.8 * h;
        let ccx = params.view_w - offset;
        let ccy = offset;
        let cr = 4.9 + 0.35 * h;

        self.brush
            .SetColor(&color_alpha([0.0, 0.0, 0.0, 0.52], alpha));
        self.d2d_ctx.DrawLine(
            p2(ccx - cr, ccy - cr + 1.0),
            p2(ccx + cr, ccy + cr + 1.0),
            &self.brush,
            2.1,
            &self.stroke_round,
        );
        self.d2d_ctx.DrawLine(
            p2(ccx + cr, ccy - cr + 1.0),
            p2(ccx - cr, ccy + cr + 1.0),
            &self.brush,
            2.1,
            &self.stroke_round,
        );

        let cross_col = lerp_rgba([0.94, 0.95, 0.97, 0.98], [1.0, 1.0, 1.0, 1.0], h);
        self.brush.SetColor(&color_alpha(cross_col, alpha));
        self.d2d_ctx.DrawLine(
            p2(ccx - cr, ccy - cr),
            p2(ccx + cr, ccy + cr),
            &self.brush,
            2.0,
            &self.stroke_round,
        );
        self.d2d_ctx.DrawLine(
            p2(ccx + cr, ccy - cr),
            p2(ccx - cr, ccy + cr),
            &self.brush,
            2.0,
            &self.stroke_round,
        );
    }

    unsafe fn draw_toolbar_with_integrated_filmstrip(
        &self,
        params: &RenderFrameParams<'_>,
        alpha: f32,
    ) {
        let bar = params.layout.toolbar_rect;

        // 1. 底部通透冷炭水晶底栏：复用预烘焙渐变刷（0 帧内 COM 分配）
        if let Some(bar_grad) = self.bar_grad_brush.as_ref() {
            bar_grad.SetStartPoint(p2(bar.left, bar.top));
            bar_grad.SetEndPoint(p2(bar.left, bar.bottom));
            bar_grad.SetOpacity(alpha.clamp(0.0, 1.0));
            self.d2d_ctx.FillRectangle(&to_d2d_rect(bar), bar_grad);
        } else {
            self.brush.SetColor(&color_alpha(COLOR_BAR_BG, alpha));
            self.d2d_ctx.FillRectangle(&to_d2d_rect(bar), &self.brush);
        }
        self.brush
            .SetColor(&color_alpha(COLOR_PANEL_TOP_SHEEN, alpha));
        self.d2d_ctx.DrawLine(
            p2(bar.left, bar.top + 0.5),
            p2(bar.right, bar.top + 0.5),
            &self.brush,
            1.0,
            None,
        );

        // 2. 背景层：无框缩略图胶片流（合并整条胶片带单次倒影渐变遮罩）
        self.draw_integrated_filmstrip(params, alpha);

        // 3. 前景层：先画左右两侧功能按钮与双翼金属银箭，最后压顶绘制正中央立体水晶播放球
        for btn in &params.layout.buttons {
            if !btn.emphasized_circle {
                self.draw_toolbar_button(btn, params, alpha);
            }
        }
        for btn in &params.layout.buttons {
            if btn.emphasized_circle {
                self.draw_toolbar_button(btn, params, alpha);
            }
        }
    }

    unsafe fn draw_integrated_filmstrip(&self, params: &RenderFrameParams<'_>, alpha: f32) {
        if !params.open_anim_armed {
            return;
        }
        let Some(strip_rc) = params.layout.filmstrip_rect else {
            return;
        };
        let strip_cx = strip_rc.center_x();
        let half_w = (strip_rc.width() * 0.5).max(1.0);
        let bar_bottom = params.layout.toolbar_rect.bottom;
        let since_open = params
            .now
            .duration_since(params.open_started_at)
            .as_secs_f32();
        let wave_elapsed = since_open - THUMB_WAVE_START_DELAY_SEC;

        // 先绘制非当前缩略图与倒影，最后压顶绘制当前选中放大高亮缩略图；
        // 每个 pass 的倒影区合并用预缓存的单支整条渐变刷一次覆写，彻底消除逐格 COM 渐变刷分配
        for pass_current in [false, true] {
            let mut band_left = f32::INFINITY;
            let mut band_right = f32::NEG_INFINITY;
            let mut band_top = f32::INFINITY;

            for item in &params.layout.filmstrip_items {
                if item.is_current != pass_current {
                    continue;
                }
                let pitch = (item.rect.width() + 2.0).max(1.0);
                let slot_dist = (item.rect.center_x() - strip_cx).abs() / pitch;

                let wave_delay = (slot_dist * THUMB_INITIAL_STAGGER_SEC).min(THUMB_MAX_STAGGER_SEC);
                let wave_t =
                    ((wave_elapsed - wave_delay) / THUMB_ITEM_GROW_DURATION_SEC).clamp(0.0, 1.0);

                let born_t = params
                    .sibling_paths
                    .get(item.index)
                    .and_then(|p| self.thumb_born_at.get(p))
                    .map(|born| {
                        (params.now.duration_since(*born).as_secs_f32()
                            / THUMB_ITEM_GROW_DURATION_SEC)
                            .clamp(0.0, 1.0)
                    })
                    .unwrap_or(0.0);

                let local_t = wave_t.min(born_t);
                if local_t <= 0.01 {
                    continue;
                }
                let grow_scale_eased = ease_out_back_soft(local_t);
                let grow_pos_eased = ease_out_cubic(local_t);
                let grow_alpha = (local_t / 0.22).clamp(0.0, 1.0);

                let hover_k = self
                    .thumb_hover_anim
                    .get(&item.index)
                    .copied()
                    .unwrap_or(0.0)
                    .clamp(0.0, 1.0);
                let emph_scale = if item.is_current {
                    1.12
                } else {
                    1.0 + 0.07 * hover_k
                };
                let lift_y = if item.is_current {
                    -2.5
                } else {
                    -3.2 * hover_k
                };

                let grow_scale = (0.06 + 0.94 * grow_scale_eased).max(0.04) * emph_scale;
                let cur_w = item.rect.width() * grow_scale;
                let cur_h = item.rect.height() * grow_scale;
                let cx_i = item.rect.center_x();
                let cy_i = item.rect.center_y() + (1.0 - grow_pos_eased) * 10.0 + lift_y;
                let draw_rect = RectF::new(
                    cx_i - cur_w * 0.5,
                    cy_i - cur_h * 0.5,
                    cx_i + cur_w * 0.5,
                    cy_i + cur_h * 0.5,
                );

                let dist_ratio = ((draw_rect.center_x() - strip_cx).abs() / half_w).clamp(0.0, 1.0);
                let edge_fade = if dist_ratio > 0.74 {
                    (1.0 - (dist_ratio - 0.74) / 0.26).clamp(0.08, 1.0)
                } else {
                    1.0
                };
                let item_alpha = alpha * edge_fade * grow_alpha;

                self.brush
                    .SetColor(&color_alpha([0.10, 0.104, 0.112, 0.74], item_alpha));
                self.d2d_ctx
                    .FillRectangle(&to_d2d_rect(draw_rect), &self.brush);

                if let Some(path) = params.sibling_paths.get(item.index) {
                    if let Some(ut) = self.thumb_bitmaps.get(path) {
                        let tbmp = &ut.bitmap;
                        let sz = tbmp.GetSize();
                        let target_aspect = draw_rect.width() / draw_rect.height().max(1.0);
                        let src_aspect = sz.width / sz.height.max(1.0);
                        let src = if src_aspect > target_aspect {
                            let cw = sz.height * target_aspect;
                            let sx = (sz.width - cw) * 0.5;
                            D2D_RECT_F {
                                left: sx,
                                top: 0.0,
                                right: sx + cw,
                                bottom: sz.height,
                            }
                        } else {
                            let ch = sz.width / target_aspect.max(0.01);
                            let sy = (sz.height - ch) * 0.5;
                            D2D_RECT_F {
                                left: 0.0,
                                top: sy,
                                right: sz.width,
                                bottom: sy + ch,
                            }
                        };
                        let dst = to_d2d_rect(draw_rect);
                        let bmp_bright = if item.is_current {
                            0.96
                        } else {
                            FILMSTRIP_DIM_OPACITY + (0.90 - FILMSTRIP_DIM_OPACITY) * hover_k
                        };
                        self.d2d_ctx.DrawBitmap(
                            tbmp,
                            Some(&dst),
                            item_alpha * bmp_bright,
                            D2D1_INTERPOLATION_MODE_LINEAR,
                            Some(&src),
                            None,
                        );

                        // 底部镜面倒影：仅绘制翻转缩略图，渐变遮罩合并到循环外整条单次绘制
                        let refl_top = draw_rect.bottom;
                        let refl_h = (bar_bottom - refl_top).max(0.0);
                        if refl_h > 2.0 {
                            let refl_clip = D2D_RECT_F {
                                left: draw_rect.left,
                                top: refl_top,
                                right: draw_rect.right,
                                bottom: bar_bottom,
                            };
                            self.d2d_ctx.PushAxisAlignedClip(
                                &refl_clip,
                                windows::Win32::Graphics::Direct2D::D2D1_ANTIALIAS_MODE_ALIASED,
                            );
                            let mut prev_mat = Matrix3x2::identity();
                            self.d2d_ctx.GetTransform(&mut prev_mat);
                            let flip_y = Matrix3x2 {
                                M11: 1.0,
                                M12: 0.0,
                                M21: 0.0,
                                M22: -1.0,
                                M31: 0.0,
                                M32: 2.0 * draw_rect.bottom,
                            };
                            self.d2d_ctx.SetTransform(&(flip_y * prev_mat));
                            self.d2d_ctx.DrawBitmap(
                                tbmp,
                                Some(&dst),
                                item_alpha * bmp_bright * 0.28,
                                D2D1_INTERPOLATION_MODE_LINEAR,
                                Some(&src),
                                None,
                            );
                            self.d2d_ctx.SetTransform(&prev_mat);
                            self.d2d_ctx.PopAxisAlignedClip();

                            band_left = band_left.min(draw_rect.left);
                            band_right = band_right.max(draw_rect.right);
                            band_top = band_top.min(refl_top);
                        }
                    } else if self.failed_thumbs.contains(path) {
                        self.brush
                            .SetColor(&color_alpha([0.05, 0.052, 0.058, 0.92], item_alpha));
                        self.d2d_ctx
                            .FillRectangle(&to_d2d_rect(draw_rect), &self.brush);
                        let line_half = (draw_rect.width() * 0.16).clamp(4.0, 9.0);
                        let lcx = draw_rect.center_x();
                        let lcy = draw_rect.center_y() - 3.0;
                        self.brush
                            .SetColor(&color_alpha([0.85, 0.24, 0.24, 0.78], item_alpha));
                        self.d2d_ctx.DrawLine(
                            p2(lcx - line_half, lcy),
                            p2(lcx + line_half, lcy),
                            &self.brush,
                            1.8,
                            &self.stroke_round,
                        );
                    }
                }

                if item.is_current {
                    self.brush
                        .SetColor(&color_alpha([1.0, 1.0, 1.0, 0.88], item_alpha));
                    self.d2d_ctx
                        .DrawRectangle(&to_d2d_rect(draw_rect), &self.brush, 1.75, None);
                } else if hover_k > 0.02 {
                    self.brush
                        .SetColor(&color_alpha([0.94, 0.96, 1.0, 0.65 * hover_k], item_alpha));
                    self.d2d_ctx
                        .DrawRectangle(&to_d2d_rect(draw_rect), &self.brush, 1.25, None);
                }
            }

            // 整条胶片带倒影区合并单次预缓存线性渐变遮罩（0 帧内 COM 渐变刷创建）
            if band_right > band_left && bar_bottom > band_top + 1.0 {
                if let Some(fade_grad) = self.refl_fade_brush.as_ref() {
                    fade_grad.SetStartPoint(p2(band_left, band_top));
                    fade_grad.SetEndPoint(p2(band_left, bar_bottom));
                    fade_grad.SetOpacity(alpha.clamp(0.0, 1.0));
                    let band_rc = D2D_RECT_F {
                        left: band_left,
                        top: band_top,
                        right: band_right,
                        bottom: bar_bottom,
                    };
                    self.d2d_ctx.FillRectangle(&band_rc, fade_grad);
                }
            }
        }
    }

    unsafe fn draw_toolbar_button(
        &self,
        btn: &ToolbarButtonLayout,
        params: &RenderFrameParams<'_>,
        alpha: f32,
    ) {
        let is_hover = params.hovered == HitTarget::ToolbarButton(btn.action);
        let cx = btn.rect.center_x();
        let cy = btn.rect.center_y();

        // 1. 正中央 44 DIP 立体无黑框水晶播放球（悬停微弹发光，无内缩灰环）
        if btn.emphasized_circle {
            let h = params.play_hover_anim.clamp(0.0, 1.0);
            let r = btn.rect.width() * 0.5 * (1.0 + 0.055 * h);

            // 1a. 柔和多层立体环境投影（自然浮压在左右双翼金属银箭之上）
            for (exp, y_off, a) in [(4.2f32, 1.8f32, 0.24f32), (1.8, 0.9, 0.42)] {
                self.brush
                    .SetColor(&color_alpha([0.0, 0.0, 0.0, a], alpha));
                self.d2d_ctx.FillEllipse(
                    &D2D1_ELLIPSE {
                        point: p2(cx, cy + y_off),
                        radiusX: r + exp,
                        radiusY: r + exp,
                    },
                    &self.brush,
                );
            }

            // 1b. 球体金属水晶双层渐变基底（顶部清透银白 -> 地平线深邃枪灰 -> 底沿柔和环境反光）
            let lift = 0.09 * h;
            let sphere_stops: [(f32, [f32; 4]); 6] = [
                (0.00, [0.80 + lift, 0.82 + lift, 0.85 + lift, 0.99]),
                (0.30, [0.62 + lift, 0.64 + lift, 0.67 + lift, 0.99]),
                (0.45, [0.26 + lift, 0.28 + lift, 0.31 + lift, 0.99]),
                (0.72, [0.33 + lift, 0.35 + lift, 0.39 + lift, 0.99]),
                (0.90, [0.54 + lift, 0.56 + lift, 0.60 + lift, 0.99]),
                (1.00, [0.74 + lift, 0.76 + lift, 0.80 + lift, 0.99]),
            ];
            if let Some(sgrad) = self.make_linear_gradient(
                p2(cx, cy - r),
                p2(cx, cy + r),
                &sphere_stops,
                alpha,
            ) {
                self.d2d_ctx.FillEllipse(
                    &D2D1_ELLIPSE {
                        point: p2(cx, cy),
                        radiusX: r,
                        radiusY: r,
                    },
                    &sgrad,
                );
            }

            // 1c. 紧贴球顶的无断层柔弧水晶玻璃高光冠（复用预烘焙单位球冠几何体，0 帧内路径分配）
            if let Some(cap) = self.geoms.crystal_cap_unit.as_ref() {
                let cap_stops: [(f32, [f32; 4]); 3] = [
                    (0.0, [1.0, 1.0, 1.0, 0.74 + 0.14 * h]),
                    (0.58, [0.95, 0.97, 1.0, 0.44 + 0.12 * h]),
                    (1.0, [0.90, 0.92, 0.96, 0.16 + 0.08 * h]),
                ];
                let cap_r = (r - 0.6).max(1.0);
                if let Some(cgrad) = self.make_linear_gradient(
                    p2(0.0, -1.0),
                    p2(0.0, -2.0 / cap_r),
                    &cap_stops,
                    alpha,
                ) {
                    let mut prev_mat = Matrix3x2::identity();
                    self.d2d_ctx.GetTransform(&mut prev_mat);
                    let mat = matrix_scale(cap_r, cap_r) * Matrix3x2::translation(cx, cy) * prev_mat;
                    self.d2d_ctx.SetTransform(&mat);
                    self.d2d_ctx.FillGeometry(cap, &cgrad, None);
                    self.d2d_ctx.SetTransform(&prev_mat);
                }
            }

            // 1d. 球缘细腻银白微光边（告别生硬死黑框！）
            self.brush
                .SetColor(&color_alpha([1.0, 1.0, 1.0, 0.46 + 0.24 * h], alpha));
            self.d2d_ctx.DrawEllipse(
                &D2D1_ELLIPSE {
                    point: p2(cx, cy),
                    radiusX: r - 0.5,
                    radiusY: r - 0.5,
                },
                &self.brush,
                1.05,
                None,
            );

            // 1e. 光学居中纯白立体播放三角 ▶
            let tri_r = 8.2 * (1.0 + 0.04 * h);
            self.brush
                .SetColor(&color_alpha([0.0, 0.0, 0.0, 0.42], alpha));
            self.draw_play_triangle(cx + 1.5, cy + 1.1, tri_r);
            self.brush
                .SetColor(&color_alpha([1.0, 1.0, 1.0, 1.0], alpha));
            self.draw_play_triangle(cx + 1.5, cy, tri_r);
            return;
        }

        // 2. 中央左右双翼拉丝金属导航大箭头
        if matches!(btn.action, ToolbarAction::PrevImage | ToolbarAction::NextImage) {
            let dir = if btn.action == ToolbarAction::NextImage {
                1.0f32
            } else {
                -1.0f32
            };
            let pod_cx = params.layout.center_pod_rect.center_x();
            self.draw_metallic_wing_arrow(pod_cx, cy, dir, is_hover, alpha);
            return;
        }

        // 3. 对称圆角玻璃金属功能按钮（+ / - / 1:1 / ↺ / ↻ / 水平翻转 / 垂直翻转）
        let inset = 0.75f32;
        let brr = D2D1_ROUNDED_RECT {
            rect: D2D_RECT_F {
                left: btn.rect.left + inset,
                top: btn.rect.top + inset,
                right: btn.rect.right - inset,
                bottom: btn.rect.bottom - inset,
            },
            radiusX: 5.5,
            radiusY: 5.5,
        };

        let fill_col = if btn.active {
            [0.21, 0.23, 0.27, 0.94]
        } else if is_hover {
            [0.17, 0.18, 0.21, 0.93]
        } else {
            [0.075, 0.078, 0.085, 0.88]
        };
        self.brush.SetColor(&color_alpha(fill_col, alpha));
        self.d2d_ctx.FillRoundedRectangle(&brr, &self.brush);

        let border_col = if btn.active {
            [0.86, 0.89, 0.95, 0.92]
        } else if is_hover {
            [0.78, 0.80, 0.85, 0.88]
        } else {
            [0.48, 0.50, 0.53, 0.64]
        };
        self.brush.SetColor(&color_alpha(border_col, alpha));
        self.d2d_ctx
            .DrawRoundedRectangle(&brr, &self.brush, 1.65, None);

        let icon_color = if btn.active || is_hover {
            [0.99, 0.99, 1.0, 1.0]
        } else {
            [0.76, 0.78, 0.81, 0.98]
        };
        self.brush.SetColor(&color_alpha(icon_color, alpha));
        self.draw_vector_icon(btn.action, btn.rect, btn.active, params.anim_speed, alpha);
    }

    unsafe fn draw_metallic_wing_arrow(
        &self,
        pod_cx: f32,
        cy: f32,
        dir: f32,
        is_hover: bool,
        alpha: f32,
    ) {
        let geom = if dir >= 0.0 {
            self.geoms.wing_right.as_ref()
        } else {
            self.geoms.wing_left.as_ref()
        };
        let Some(geom) = geom else {
            return;
        };

        let lift = if is_hover { 0.13f32 } else { 0.0 };
        let arrow_stops: [(f32, [f32; 4]); 5] = [
            (0.00, [0.78 + lift, 0.80 + lift, 0.83 + lift, 0.96]),
            (0.32, [0.55 + lift, 0.57 + lift, 0.60 + lift, 0.96]),
            (0.55, [0.46 + lift, 0.48 + lift, 0.51 + lift, 0.96]),
            (0.74, [0.52 + lift, 0.54 + lift, 0.57 + lift, 0.96]),
            (1.00, [0.76 + lift, 0.78 + lift, 0.81 + lift, 0.96]),
        ];
        let mut prev_mat = Matrix3x2::identity();
        self.d2d_ctx.GetTransform(&mut prev_mat);
        self.d2d_ctx
            .SetTransform(&(Matrix3x2::translation(pod_cx, cy) * prev_mat));
        if let Some(agrad) = self.make_linear_gradient(
            p2(0.0, -14.8),
            p2(0.0, 14.8),
            &arrow_stops,
            alpha,
        ) {
            self.d2d_ctx.FillGeometry(geom, &agrad, None);
        }

        let edge_col = if is_hover {
            [0.95, 0.97, 1.0, 0.45]
        } else {
            [0.16, 0.17, 0.19, 0.58]
        };
        self.brush.SetColor(&color_alpha(edge_col, alpha));
        self.d2d_ctx
            .DrawGeometry(geom, &self.brush, 1.0, &self.stroke_miter);
        self.d2d_ctx.SetTransform(&prev_mat);
    }

    unsafe fn draw_anim_bar(&self, params: &RenderFrameParams<'_>, alpha: f32) {
        let Some(bar) = params.layout.anim_bar_rect else {
            return;
        };
        let Some(img) = params.image else {
            return;
        };
        let rr = D2D1_ROUNDED_RECT {
            rect: to_d2d_rect(bar),
            radiusX: 8.0,
            radiusY: 8.0,
        };
        self.brush
            .SetColor(&color_alpha(COLOR_PANEL_BG, alpha * 0.92));
        self.d2d_ctx.FillRoundedRectangle(&rr, &self.brush);
        self.brush.SetColor(&color_alpha(COLOR_PANEL_BORDER, alpha));
        self.d2d_ctx
            .DrawRoundedRectangle(&rr, &self.brush, 1.0, None);

        if let Some(sc) = params.layout.anim_scrubber_rect {
            let cy = sc.center_y();
            let x0 = sc.left + 4.0;
            let x1 = sc.right - 4.0;
            let total = img.frames.len().max(1);
            let frac = if total > 1 {
                (params.frame_index as f32) / ((total - 1) as f32)
            } else {
                0.0
            };
            let kx = x0 + (x1 - x0) * frac.clamp(0.0, 1.0);
            self.brush
                .SetColor(&color_alpha([1.0, 1.0, 1.0, 0.20], alpha));
            self.d2d_ctx.FillRoundedRectangle(
                &D2D1_ROUNDED_RECT {
                    rect: D2D_RECT_F {
                        left: x0,
                        top: cy - 2.0,
                        right: x1,
                        bottom: cy + 2.0,
                    },
                    radiusX: 2.0,
                    radiusY: 2.0,
                },
                &self.brush,
            );
            self.brush.SetColor(&color_alpha(COLOR_ACCENT, alpha));
            self.d2d_ctx.FillRoundedRectangle(
                &D2D1_ROUNDED_RECT {
                    rect: D2D_RECT_F {
                        left: x0,
                        top: cy - 2.0,
                        right: kx.max(x0 + 2.0),
                        bottom: cy + 2.0,
                    },
                    radiusX: 2.0,
                    radiusY: 2.0,
                },
                &self.brush,
            );
            self.brush.SetColor(&color_alpha(COLOR_TEXT_PRIMARY, alpha));
            self.d2d_ctx.FillEllipse(
                &D2D1_ELLIPSE {
                    point: p2(kx, cy),
                    radiusX: 4.5,
                    radiusY: 4.5,
                },
                &self.brush,
            );
        }

        if let Some(lbl_rc) = params.layout.anim_frame_label_rect {
            let txt = format!("{}/{}", params.frame_index + 1, img.frames.len());
            let w = to_wide(&txt);
            self.brush
                .SetColor(&color_alpha(COLOR_TEXT_SECONDARY, alpha));
            self.d2d_ctx.DrawText(
                &w[..w.len() - 1],
                &self.fmt_mono_center,
                &to_d2d_rect(lbl_rc),
                &self.brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                Default::default(),
            );
        }

        for btn in &params.layout.anim_buttons {
            self.draw_toolbar_button(btn, params, alpha);
        }
    }

    unsafe fn draw_play_triangle(&self, cx: f32, cy: f32, r: f32) {
        if let Some(geom) = self.geoms.play_tri_unit.as_ref() {
            let mut prev_mat = Matrix3x2::identity();
            self.d2d_ctx.GetTransform(&mut prev_mat);
            self.d2d_ctx
                .SetTransform(&(matrix_scale(r, r) * Matrix3x2::translation(cx, cy) * prev_mat));
            self.d2d_ctx.FillGeometry(geom, &self.brush, None);
            self.d2d_ctx.SetTransform(&prev_mat);
        }
    }

    /// 经典旋转矢量图标（复用预烘焙几何体，0 帧内路径分配）
    unsafe fn draw_rotate_icon(&self, cx: f32, cy: f32, ccw: bool) {
        let (arc, head) = if ccw {
            (
                self.geoms.rot_ccw_arc.as_ref(),
                self.geoms.rot_ccw_head.as_ref(),
            )
        } else {
            (
                self.geoms.rot_cw_arc.as_ref(),
                self.geoms.rot_cw_head.as_ref(),
            )
        };
        let mut prev_mat = Matrix3x2::identity();
        self.d2d_ctx.GetTransform(&mut prev_mat);
        self.d2d_ctx
            .SetTransform(&(Matrix3x2::translation(cx, cy) * prev_mat));
        if let Some(a) = arc {
            self.d2d_ctx
                .DrawGeometry(a, &self.brush, 2.2, &self.stroke_flat);
        }
        if let Some(h) = head {
            self.d2d_ctx.FillGeometry(h, &self.brush, None);
        }
        self.d2d_ctx.SetTransform(&prev_mat);
    }

    /// 专业图像软件高辨识度「对称轴双三角镜像」水平/垂直翻转图标（复用预烘焙几何体）
    unsafe fn draw_flip_icon(&self, cx: f32, cy: f32, horizontal: bool, alpha: f32) {
        let cur_col = self.brush.GetColor();
        let mut prev_mat = Matrix3x2::identity();
        self.d2d_ctx.GetTransform(&mut prev_mat);
        self.d2d_ctx
            .SetTransform(&(Matrix3x2::translation(cx, cy) * prev_mat));
        if horizontal {
            self.d2d_ctx.DrawLine(
                p2(0.0, -6.4),
                p2(0.0, 6.4),
                &self.brush,
                1.35,
                &self.stroke_round,
            );
            if let Some(g) = self.geoms.flip_h_left.as_ref() {
                self.d2d_ctx.FillGeometry(g, &self.brush, None);
            }
            if let Some(g) = self.geoms.flip_h_right.as_ref() {
                self.brush.SetColor(&D2D1_COLOR_F {
                    r: cur_col.r,
                    g: cur_col.g,
                    b: cur_col.b,
                    a: cur_col.a * 0.25 * alpha.max(0.5),
                });
                self.d2d_ctx.FillGeometry(g, &self.brush, None);
                self.brush.SetColor(&cur_col);
                self.d2d_ctx
                    .DrawGeometry(g, &self.brush, 1.3, &self.stroke_round);
            }
        } else {
            self.d2d_ctx.DrawLine(
                p2(-6.8, 0.0),
                p2(6.8, 0.0),
                &self.brush,
                1.35,
                &self.stroke_round,
            );
            if let Some(g) = self.geoms.flip_v_top.as_ref() {
                self.d2d_ctx.FillGeometry(g, &self.brush, None);
            }
            if let Some(g) = self.geoms.flip_v_bot.as_ref() {
                self.brush.SetColor(&D2D1_COLOR_F {
                    r: cur_col.r,
                    g: cur_col.g,
                    b: cur_col.b,
                    a: cur_col.a * 0.25 * alpha.max(0.5),
                });
                self.d2d_ctx.FillGeometry(g, &self.brush, None);
                self.brush.SetColor(&cur_col);
                self.d2d_ctx
                    .DrawGeometry(g, &self.brush, 1.3, &self.stroke_round);
            }
        }
        self.d2d_ctx.SetTransform(&prev_mat);
    }

    unsafe fn draw_vector_icon(
        &self,
        action: ToolbarAction,
        rc: RectF,
        active: bool,
        anim_speed: f32,
        alpha: f32,
    ) {
        let cx = rc.center_x();
        let cy = rc.center_y();
        let sw = 1.85f32;
        match action {
            ToolbarAction::ZoomOut => {
                self.d2d_ctx.FillRoundedRectangle(
                    &D2D1_ROUNDED_RECT {
                        rect: D2D_RECT_F {
                            left: cx - 3.0,
                            top: cy - 0.95,
                            right: cx + 3.0,
                            bottom: cy + 1.15,
                        },
                        radiusX: 0.4,
                        radiusY: 0.4,
                    },
                    &self.brush,
                );
            }
            ToolbarAction::ZoomIn => {
                self.d2d_ctx.FillRoundedRectangle(
                    &D2D1_ROUNDED_RECT {
                        rect: D2D_RECT_F {
                            left: cx - 4.0,
                            top: cy - 1.0,
                            right: cx + 4.0,
                            bottom: cy + 1.0,
                        },
                        radiusX: 0.4,
                        radiusY: 0.4,
                    },
                    &self.brush,
                );
                self.d2d_ctx.FillRoundedRectangle(
                    &D2D1_ROUNDED_RECT {
                        rect: D2D_RECT_F {
                            left: cx - 1.0,
                            top: cy - 4.0,
                            right: cx + 1.0,
                            bottom: cy + 4.0,
                        },
                        radiusX: 0.4,
                        radiusY: 0.4,
                    },
                    &self.brush,
                );
            }
            ToolbarAction::ToggleActualSize => {
                let w = to_wide("1:1");
                self.d2d_ctx.DrawText(
                    &w[..w.len() - 1],
                    &self.fmt_btn_1to1,
                    &to_d2d_rect(rc),
                    &self.brush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    Default::default(),
                );
            }
            ToolbarAction::RotateCcw => {
                self.draw_rotate_icon(cx, cy, true);
            }
            ToolbarAction::RotateCw => {
                self.draw_rotate_icon(cx, cy, false);
            }
            ToolbarAction::FlipHorizontal => {
                self.draw_flip_icon(cx, cy, true, alpha);
            }
            ToolbarAction::FlipVertical => {
                self.draw_flip_icon(cx, cy, false, alpha);
            }
            ToolbarAction::AnimTogglePlay => {
                if active {
                    self.d2d_ctx.DrawLine(
                        p2(cx - 2.4, cy - 4.5),
                        p2(cx - 2.4, cy + 4.5),
                        &self.brush,
                        1.9,
                        &self.stroke_round,
                    );
                    self.d2d_ctx.DrawLine(
                        p2(cx + 2.4, cy - 4.5),
                        p2(cx + 2.4, cy + 4.5),
                        &self.brush,
                        1.9,
                        &self.stroke_round,
                    );
                } else {
                    self.draw_play_triangle(cx + 0.8, cy, 4.8);
                }
            }
            ToolbarAction::AnimPrevFrame => {
                self.d2d_ctx.DrawLine(
                    p2(cx - 3.8, cy - 4.4),
                    p2(cx - 3.8, cy + 4.4),
                    &self.brush,
                    sw,
                    &self.stroke_round,
                );
                self.d2d_ctx.DrawLine(
                    p2(cx + 3.2, cy - 4.4),
                    p2(cx - 1.3, cy),
                    &self.brush,
                    sw,
                    &self.stroke_round,
                );
                self.d2d_ctx.DrawLine(
                    p2(cx - 1.3, cy),
                    p2(cx + 3.2, cy + 4.4),
                    &self.brush,
                    sw,
                    &self.stroke_round,
                );
            }
            ToolbarAction::AnimNextFrame => {
                self.d2d_ctx.DrawLine(
                    p2(cx + 3.8, cy - 4.4),
                    p2(cx + 3.8, cy + 4.4),
                    &self.brush,
                    sw,
                    &self.stroke_round,
                );
                self.d2d_ctx.DrawLine(
                    p2(cx - 3.2, cy - 4.4),
                    p2(cx + 1.3, cy),
                    &self.brush,
                    sw,
                    &self.stroke_round,
                );
                self.d2d_ctx.DrawLine(
                    p2(cx + 1.3, cy),
                    p2(cx - 3.2, cy + 4.4),
                    &self.brush,
                    sw,
                    &self.stroke_round,
                );
            }
            ToolbarAction::AnimCycleSpeed => {
                let s = if (anim_speed - 0.25).abs() < 0.05 {
                    "0.25x".to_string()
                } else {
                    format!("{:.1}x", anim_speed)
                };
                let w = to_wide(&s);
                self.d2d_ctx.DrawText(
                    &w[..w.len() - 1],
                    &self.fmt_btn_1to1,
                    &to_d2d_rect(rc),
                    &self.brush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    Default::default(),
                );
            }
            ToolbarAction::SaveCurrentFramePng => {
                self.d2d_ctx.DrawLine(
                    p2(cx, cy - 4.5),
                    p2(cx, cy + 2.2),
                    &self.brush,
                    sw,
                    &self.stroke_round,
                );
                self.d2d_ctx.DrawLine(
                    p2(cx - 2.8, cy - 0.4),
                    p2(cx, cy + 2.4),
                    &self.brush,
                    sw,
                    &self.stroke_round,
                );
                self.d2d_ctx.DrawLine(
                    p2(cx + 2.8, cy - 0.4),
                    p2(cx, cy + 2.4),
                    &self.brush,
                    sw,
                    &self.stroke_round,
                );
                self.d2d_ctx.DrawLine(
                    p2(cx - 4.4, cy + 4.5),
                    p2(cx + 4.4, cy + 4.5),
                    &self.brush,
                    sw,
                    &self.stroke_round,
                );
            }
            _ => {}
        }
    }

    unsafe fn draw_info_card(
        &self,
        rc: RectF,
        img: &SharedImage,
        dpi: f32,
        lang: ViewerLanguage,
        alpha: f32,
    ) {
        let s = lang.strings();
        let rr = D2D1_ROUNDED_RECT {
            rect: to_d2d_rect(rc),
            radiusX: 10.0,
            radiusY: 10.0,
        };
        self.brush.SetColor(&color_alpha(COLOR_PANEL_BG, alpha));
        self.d2d_ctx.FillRoundedRectangle(&rr, &self.brush);
        self.brush.SetColor(&color_alpha(COLOR_PANEL_BORDER, alpha));
        self.d2d_ctx
            .DrawRoundedRectangle(&rr, &self.brush, 1.0, None);

        let name = img
            .path
            .file_name()
            .map(|st| st.to_string_lossy().to_string())
            .unwrap_or_else(|| s.info_fallback_title.to_string());
        let wname = to_wide(&name);
        self.brush.SetColor(&color_alpha(COLOR_TEXT_PRIMARY, alpha));
        self.d2d_ctx.DrawText(
            &wname[..wname.len() - 1],
            &self.fmt_title,
            &D2D_RECT_F {
                left: rc.left + 16.0,
                top: rc.top + 12.0,
                right: rc.right - 16.0,
                bottom: rc.top + 38.0,
            },
            &self.brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            Default::default(),
        );

        let mut rows: Vec<(String, String)> = Vec::new();
        rows.push((
            s.exif_file_size.to_string(),
            format_file_size(img.file_size_bytes),
        ));
        for field in &img.exif_summary {
            rows.push(s.format_exif_field(field));
        }
        rows.push((
            s.exif_display_scale.to_string(),
            format!(
                "{}% ({})",
                (dpi * 100.0 / 96.0).round() as u32,
                s.exif_1to1_suffix
            ),
        ));

        let mut y = rc.top + 48.0;
        for (k, v) in rows {
            if y + 22.0 > rc.bottom - 8.0 {
                break;
            }
            let wk = to_wide(&k);
            let wv = to_wide(&v);
            self.brush
                .SetColor(&color_alpha(COLOR_TEXT_SECONDARY, alpha));
            self.d2d_ctx.DrawText(
                &wk[..wk.len() - 1],
                &self.fmt_ui,
                &D2D_RECT_F {
                    left: rc.left + 16.0,
                    top: y,
                    right: rc.left + 136.0,
                    bottom: y + 22.5,
                },
                &self.brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                Default::default(),
            );
            self.brush.SetColor(&color_alpha(COLOR_TEXT_PRIMARY, alpha));
            self.d2d_ctx.DrawText(
                &wv[..wv.len() - 1],
                &self.fmt_mono,
                &D2D_RECT_F {
                    left: rc.left + 140.0,
                    top: y,
                    right: rc.right - 14.0,
                    bottom: y + 22.5,
                },
                &self.brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                Default::default(),
            );
            y += 24.5;
        }
    }

    unsafe fn draw_toast(&self, view_w: f32, _view_h: f32, text: &str) {
        let box_w = (text.chars().count() as f32 * 13.0 + 34.0).clamp(150.0, 440.0);
        let box_h = 32.0;
        let left = (view_w - box_w) * 0.5;
        let top = 46.0;
        let rr = D2D1_ROUNDED_RECT {
            rect: D2D_RECT_F {
                left,
                top,
                right: left + box_w,
                bottom: top + box_h,
            },
            radiusX: 7.0,
            radiusY: 7.0,
        };
        self.brush.SetColor(&color_f([0.09, 0.095, 0.108, 0.88]));
        self.d2d_ctx.FillRoundedRectangle(&rr, &self.brush);
        self.brush.SetColor(&color_f([0.58, 0.60, 0.64, 0.45]));
        self.d2d_ctx
            .DrawRoundedRectangle(&rr, &self.brush, 1.0, None);

        self.brush.SetColor(&color_f(COLOR_TEXT_PRIMARY));
        let wt = to_wide(text);
        self.d2d_ctx.DrawText(
            &wt[..wt.len() - 1],
            &self.fmt_ui_center,
            &rr.rect,
            &self.brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            Default::default(),
        );
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn draw_center_status(
        &self,
        view_w: f32,
        view_h: f32,
        title: &str,
        detail: &str,
        is_error: bool,
        spin_phase_sec: f32,
        alpha: f32,
    ) {
        let (box_w, box_h) = if is_error {
            (380.0f32.min(view_w - 40.0), 236.0f32)
        } else {
            (340.0f32.min(view_w - 40.0), 116.0f32)
        };
        let left = (view_w - box_w) * 0.5;
        let top = (view_h - box_h) * 0.5 - 24.0;
        let cx = left + box_w * 0.5;
        let cy = top + box_h * 0.5;

        // 柔和宽幅 2D 物理高斯投影（还原原版 Picasa 无效图片暗黑方卡立体悬浮感）
        self.d2d_ctx.SetTransform(&Matrix3x2::translation(cx, cy));
        self.draw_gaussian_box_shadow_local(
            -box_w * 0.5,
            -box_h * 0.5,
            box_w * 0.5,
            box_h * 0.5,
            30.0,
            6.0,
            0.58 * alpha,
        );
        self.d2d_ctx.SetTransform(&Matrix3x2::identity());

        let rr = D2D1_ROUNDED_RECT {
            rect: D2D_RECT_F {
                left,
                top,
                right: left + box_w,
                bottom: top + box_h,
            },
            radiusX: if is_error { 4.0 } else { 10.0 },
            radiusY: if is_error { 4.0 } else { 10.0 },
        };
        let bg = if is_error {
            [0.02, 0.02, 0.025, 0.96]
        } else {
            COLOR_PANEL_BG
        };
        self.brush.SetColor(&color_alpha(bg, alpha));
        self.d2d_ctx.FillRoundedRectangle(&rr, &self.brush);
        self.brush.SetColor(&color_alpha(
            if is_error {
                [1.0, 1.0, 1.0, 0.12]
            } else {
                COLOR_PANEL_BORDER
            },
            alpha,
        ));
        self.d2d_ctx
            .DrawRoundedRectangle(&rr, &self.brush, 1.0, None);

        if is_error {
            // 原版 Picasa 经典无效图片醒目珊瑚红居中标题 + 高分屏清晰详情说明
            self.brush
                .SetColor(&color_alpha([0.94, 0.27, 0.27, 1.0], alpha));
            let wt = to_wide(title);
            self.d2d_ctx.DrawText(
                &wt[..wt.len() - 1],
                &self.fmt_error_title,
                &D2D_RECT_F {
                    left: left + 18.0,
                    top: cy - 30.0,
                    right: left + box_w - 18.0,
                    bottom: cy + 4.0,
                },
                &self.brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                Default::default(),
            );
            self.brush
                .SetColor(&color_alpha([0.74, 0.76, 0.80, 0.92], alpha));
            let wd = to_wide(detail);
            self.d2d_ctx.DrawText(
                &wd[..wd.len() - 1],
                &self.fmt_error_detail,
                &D2D_RECT_F {
                    left: left + 20.0,
                    top: cy + 10.0,
                    right: left + box_w - 20.0,
                    bottom: top + box_h - 16.0,
                },
                &self.brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                Default::default(),
            );
        } else {
            // 加载态：纯矢量平滑旋转加载环 + 标题与文件名
            let spin_cy = top + 30.0;
            let spin_r = 12.0f32;
            self.brush
                .SetColor(&color_alpha([1.0, 1.0, 1.0, 0.14], alpha));
            self.d2d_ctx.DrawEllipse(
                &D2D1_ELLIPSE {
                    point: p2(cx, spin_cy),
                    radiusX: spin_r,
                    radiusY: spin_r,
                },
                &self.brush,
                2.2,
                None,
            );
            let angle = (spin_phase_sec * 6.0) % std::f32::consts::TAU;
            let a0 = angle;
            let a1 = angle + std::f32::consts::PI * 1.15;
            let p_start = p2(cx + spin_r * a0.cos(), spin_cy + spin_r * a0.sin());
            let p_end = p2(cx + spin_r * a1.cos(), spin_cy + spin_r * a1.sin());
            if let Some(arc) = self.build_arc_geometry(p_start, p_end, spin_r, true, true) {
                self.brush
                    .SetColor(&color_alpha([0.96, 0.97, 0.99, 0.94], alpha));
                self.d2d_ctx
                    .DrawGeometry(&arc, &self.brush, 2.3, &self.stroke_round);
            }

            self.brush.SetColor(&color_alpha(COLOR_TEXT_PRIMARY, alpha));
            let wt = to_wide(title);
            self.d2d_ctx.DrawText(
                &wt[..wt.len() - 1],
                &self.fmt_ui_center,
                &D2D_RECT_F {
                    left: left + 16.0,
                    top: top + 52.0,
                    right: left + box_w - 16.0,
                    bottom: top + 78.0,
                },
                &self.brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                Default::default(),
            );
            self.brush
                .SetColor(&color_alpha(COLOR_TEXT_SECONDARY, alpha));
            let wd = to_wide(detail);
            self.d2d_ctx.DrawText(
                &wd[..wd.len() - 1],
                &self.fmt_ui_center,
                &D2D_RECT_F {
                    left: left + 16.0,
                    top: top + 78.0,
                    right: left + box_w - 16.0,
                    bottom: top + box_h - 10.0,
                },
                &self.brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                Default::default(),
            );
        }
    }
}

unsafe fn create_gpu_checker_brush(d2d_ctx: &ID2D1DeviceContext) -> Result<ID2D1BitmapBrush1, String> {
    const SZ: usize = 20;
    const HALF: usize = 10;
    let mut pixels = [0u8; SZ * SZ * 4];
    for y in 0..SZ {
        for x in 0..SZ {
            let light = ((x / HALF) + (y / HALF)) % 2 == 1;
            let (b, g, r) = if light { (66u8, 60u8, 56u8) } else { (48u8, 43u8, 40u8) };
            let off = (y * SZ + x) * 4;
            pixels[off] = b;
            pixels[off + 1] = g;
            pixels[off + 2] = r;
            pixels[off + 3] = 255;
        }
    }
    let props = D2D1_BITMAP_PROPERTIES1 {
        pixelFormat: D2D1_PIXEL_FORMAT {
            format: DXGI_FORMAT_B8G8R8A8_UNORM,
            alphaMode: D2D1_ALPHA_MODE_PREMULTIPLIED,
        },
        dpiX: 96.0,
        dpiY: 96.0,
        bitmapOptions: D2D1_BITMAP_OPTIONS_NONE,
        ..Default::default()
    };
    let bmp = d2d_ctx
        .CreateBitmap(
            D2D_SIZE_U {
                width: SZ as u32,
                height: SZ as u32,
            },
            Some(pixels.as_ptr() as *const _),
            (SZ * 4) as u32,
            &props,
        )
        .map_err(|e| format!("创建棋盘格 GPU 位图失败: {e}"))?;

    let bb_props = D2D1_BITMAP_BRUSH_PROPERTIES1 {
        extendModeX: D2D1_EXTEND_MODE_WRAP,
        extendModeY: D2D1_EXTEND_MODE_WRAP,
        interpolationMode: D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
    };
    d2d_ctx
        .CreateBitmapBrush(&bmp, Some(&bb_props), None)
        .map_err(|e| format!("创建棋盘格 GPU 画刷失败: {e}"))
}

/// 高精度高斯误差函数 `erf(x)`（Abramowitz-Stegun 7.1.26，最大误差 < 1.5e-7），
/// 用于数学解析烘焙精确 2D 盒式高斯阴影贴图
#[inline]
fn erf_approx(x: f32) -> f32 {
    let sign = if x < 0.0 { -1.0f32 } else { 1.0f32 };
    let ax = x.abs();
    let t = 1.0 / (1.0 + 0.3275911 * ax);
    let poly = t
        * (0.254829592
            + t * (-0.284496736
                + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429))));
    sign * (1.0 - poly * (-ax * ax).exp())
}

/// 烘焙 192x192 精确解析 2D 高斯模糊阴影 9-Slice GPU 纹理（耗时 < 0.3ms，显存仅 144KB）
unsafe fn create_gaussian_shadow_9slice_bitmap(
    d2d_ctx: &ID2D1DeviceContext,
) -> Result<ID2D1Bitmap1, String> {
    const SZ: usize = 192;
    const BOX_MIN: f32 = 56.0;
    const BOX_MAX: f32 = 136.0; // 内核 80x80，外扩羽化 56px（> 3.5 * sigma），四角 100% 平滑衰减至 0
    const SIGMA: f32 = 16.0;
    let inv_sqrt2_sigma = 1.0 / (std::f32::consts::SQRT_2 * SIGMA);

    let mut lut_1d = [0.0f32; SZ];
    for i in 0..SZ {
        let p = i as f32 + 0.5;
        let v = 0.5
            * (erf_approx((p - BOX_MIN) * inv_sqrt2_sigma)
                - erf_approx((p - BOX_MAX) * inv_sqrt2_sigma));
        lut_1d[i] = v.clamp(0.0, 1.0);
    }

    let mut pixels = vec![0u8; SZ * SZ * 4];
    for y in 0..SZ {
        let ay = lut_1d[y];
        for x in 0..SZ {
            let a = (lut_1d[x] * ay).clamp(0.0, 1.0);
            // 边缘 2px 软归零防止双线性采样钳制出微弱黑边
            let edge_fade_x = (x.min(SZ - 1 - x) as f32 / 2.0).min(1.0);
            let edge_fade_y = (y.min(SZ - 1 - y) as f32 / 2.0).min(1.0);
            let alpha_u8 = ((a * edge_fade_x * edge_fade_y) * 255.0).round().clamp(0.0, 255.0) as u8;
            let off = (y * SZ + x) * 4;
            // 预乘纯黑 BGRA (0, 0, 0, alpha)
            pixels[off] = 0;
            pixels[off + 1] = 0;
            pixels[off + 2] = 0;
            pixels[off + 3] = alpha_u8;
        }
    }

    let props = D2D1_BITMAP_PROPERTIES1 {
        pixelFormat: D2D1_PIXEL_FORMAT {
            format: DXGI_FORMAT_B8G8R8A8_UNORM,
            alphaMode: D2D1_ALPHA_MODE_PREMULTIPLIED,
        },
        dpiX: 96.0,
        dpiY: 96.0,
        bitmapOptions: D2D1_BITMAP_OPTIONS_NONE,
        ..Default::default()
    };
    d2d_ctx
        .CreateBitmap(
            D2D_SIZE_U {
                width: SZ as u32,
                height: SZ as u32,
            },
            Some(pixels.as_ptr() as *const _),
            (SZ * 4) as u32,
            &props,
        )
        .map_err(|e| format!("创建 2D 高斯阴影 9-Slice GPU 位图失败: {e}"))
}

fn resolve_available_font<'a>(
    dwrite: &IDWriteFactory,
    preferred: &'a str,
    fallback: &'a str,
) -> &'a str {
    unsafe {
        let mut collection = None;
        if dwrite.GetSystemFontCollection(&mut collection, false).is_ok() {
            if let Some(col) = collection {
                let wpref = to_wide(preferred);
                let mut index = 0u32;
                let mut exists = windows::Win32::Foundation::BOOL(0);
                if col
                    .FindFamilyName(PCWSTR(wpref.as_ptr()), &mut index, &mut exists)
                    .is_ok()
                    && exists.as_bool()
                {
                    return preferred;
                }
            }
        }
    }
    fallback
}

fn create_text_format(
    dwrite: &IDWriteFactory,
    family: &str,
    size: f32,
    weight: windows::Win32::Graphics::DirectWrite::DWRITE_FONT_WEIGHT,
    center: bool,
    locale_tag: &str,
) -> Result<IDWriteTextFormat, String> {
    unsafe {
        let wfam = to_wide(family);
        let wloc = to_wide(locale_tag);
        let fmt = dwrite
            .CreateTextFormat(
                PCWSTR(wfam.as_ptr()),
                None,
                weight,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                size,
                PCWSTR(wloc.as_ptr()),
            )
            .map_err(|e| format!("CreateTextFormat 失败: {e}"))?;
        let _ = fmt.SetTextAlignment(if center {
            DWRITE_TEXT_ALIGNMENT_CENTER
        } else {
            DWRITE_TEXT_ALIGNMENT_LEADING
        });
        let _ = fmt.SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        let _ = fmt.SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        Ok(fmt)
    }
}

fn create_wrapped_text_format(
    dwrite: &IDWriteFactory,
    family: &str,
    size: f32,
    weight: windows::Win32::Graphics::DirectWrite::DWRITE_FONT_WEIGHT,
    locale_tag: &str,
) -> Result<IDWriteTextFormat, String> {
    unsafe {
        let wfam = to_wide(family);
        let wloc = to_wide(locale_tag);
        let fmt = dwrite
            .CreateTextFormat(
                PCWSTR(wfam.as_ptr()),
                None,
                weight,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                size,
                PCWSTR(wloc.as_ptr()),
            )
            .map_err(|e| format!("CreateTextFormat(wrap) 失败: {e}"))?;
        let _ = fmt.SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        let _ = fmt.SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        let _ = fmt.SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        Ok(fmt)
    }
}

unsafe fn create_linear_gradient_brush(
    d2d_ctx: &ID2D1DeviceContext,
    start: D2D_POINT_2F,
    end: D2D_POINT_2F,
    stops: &[(f32, [f32; 4])],
    alpha: f32,
) -> Option<ID2D1LinearGradientBrush> {
    let d2d_stops: Vec<D2D1_GRADIENT_STOP> = stops
        .iter()
        .map(|(pos, col)| D2D1_GRADIENT_STOP {
            position: *pos,
            color: color_alpha(*col, alpha),
        })
        .collect();
    let col = d2d_ctx
        .CreateGradientStopCollection(
            &d2d_stops,
            D2D1_COLOR_SPACE_SRGB,
            D2D1_COLOR_SPACE_SRGB,
            D2D1_BUFFER_PRECISION_8BPC_UNORM,
            D2D1_EXTEND_MODE_CLAMP,
            D2D1_COLOR_INTERPOLATION_MODE_STRAIGHT,
        )
        .ok()?;
    let props = D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES {
        startPoint: start,
        endPoint: end,
    };
    let bprops = D2D1_BRUSH_PROPERTIES {
        opacity: 1.0,
        transform: Matrix3x2::identity(),
    };
    d2d_ctx
        .CreateLinearGradientBrush(&props, Some(&bprops), &col)
        .ok()
}

unsafe fn build_polygon_geometry_with(
    factory: &ID2D1Factory1,
    pts: &[D2D_POINT_2F],
) -> Option<ID2D1PathGeometry> {
    if pts.len() < 3 {
        return None;
    }
    let geom = factory.CreatePathGeometry().ok()?;
    let sink = geom.Open().ok()?;
    sink.BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
    for p in &pts[1..] {
        sink.AddLine(*p);
    }
    sink.EndFigure(D2D1_FIGURE_END_CLOSED);
    sink.Close().ok()?;
    Some(geom.into())
}

unsafe fn build_arc_geometry_with(
    factory: &ID2D1Factory1,
    start: D2D_POINT_2F,
    end: D2D_POINT_2F,
    radius: f32,
    clockwise: bool,
    large_arc: bool,
) -> Option<ID2D1PathGeometry> {
    let geom = factory.CreatePathGeometry().ok()?;
    let sink = geom.Open().ok()?;
    sink.BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
    sink.AddArc(&D2D1_ARC_SEGMENT {
        point: end,
        size: D2D_SIZE_F {
            width: radius,
            height: radius,
        },
        rotationAngle: 0.0,
        sweepDirection: if clockwise {
            D2D1_SWEEP_DIRECTION_CLOCKWISE
        } else {
            D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE
        },
        arcSize: if large_arc {
            D2D1_ARC_SIZE_LARGE
        } else {
            D2D1_ARC_SIZE_SMALL
        },
    });
    sink.EndFigure(D2D1_FIGURE_END_OPEN);
    sink.Close().ok()?;
    Some(geom.into())
}

unsafe fn build_unit_crystal_cap(factory: &ID2D1Factory1) -> Option<ID2D1PathGeometry> {
    // 单位半径 r = 1.0，对应默认半径 ~21.4 DIP 的比例
    let y_off = -2.2f32 / 21.4f32;
    let dx = (1.0 - y_off * y_off).max(0.01).sqrt();
    let left_pt = p2(-dx, y_off);
    let right_pt = p2(dx, y_off);
    let geom = factory.CreatePathGeometry().ok()?;
    let sink = geom.Open().ok()?;
    sink.BeginFigure(left_pt, D2D1_FIGURE_BEGIN_FILLED);
    sink.AddArc(&D2D1_ARC_SEGMENT {
        point: right_pt,
        size: D2D_SIZE_F {
            width: 1.0,
            height: 1.0,
        },
        rotationAngle: 0.0,
        sweepDirection: D2D1_SWEEP_DIRECTION_CLOCKWISE,
        arcSize: D2D1_ARC_SIZE_SMALL,
    });
    sink.AddQuadraticBezier(&D2D1_QUADRATIC_BEZIER_SEGMENT {
        point1: p2(0.0, -6.5 / 21.4),
        point2: left_pt,
    });
    sink.EndFigure(D2D1_FIGURE_END_CLOSED);
    sink.Close().ok()?;
    Some(geom.into())
}

unsafe fn build_static_geometries(factory: &ID2D1Factory1) -> StaticGeometries {
    let play_tri_unit = build_polygon_geometry_with(
        factory,
        &[p2(-0.62, -0.84), p2(0.90, 0.0), p2(-0.62, 0.84)],
    );
    let crystal_cap_unit = build_unit_crystal_cap(factory);

    let make_wing = |dir: f32| {
        build_polygon_geometry_with(
            factory,
            &[
                p2(dir * 15.0, -6.2),
                p2(dir * 36.8, -6.2),
                p2(dir * 36.8, -14.8),
                p2(dir * 53.2, 0.0),
                p2(dir * 36.8, 14.8),
                p2(dir * 36.8, 6.2),
                p2(dir * 15.0, 6.2),
            ],
        )
    };
    let wing_left = make_wing(-1.0);
    let wing_right = make_wing(1.0);

    let r = 4.0f32;
    let oy = 2.0f32;
    let a0_ccw = 162.0f32.to_radians();
    let a0_cw = 18.0f32.to_radians();
    let a1 = 270.0f32.to_radians();
    let ed_y = oy + r * a1.sin();
    let rot_ccw_arc = build_arc_geometry_with(
        factory,
        p2(r * a0_ccw.cos(), oy + r * a0_ccw.sin()),
        p2(r * a1.cos(), ed_y),
        r,
        false,
        true,
    );
    let rot_ccw_head = build_polygon_geometry_with(
        factory,
        &[
            p2(-3.4, ed_y - 0.15),
            p2(1.2, ed_y - 3.6),
            p2(1.2, ed_y + 3.3),
        ],
    );
    let rot_cw_arc = build_arc_geometry_with(
        factory,
        p2(r * a0_cw.cos(), oy + r * a0_cw.sin()),
        p2(r * a1.cos(), ed_y),
        r,
        true,
        true,
    );
    let rot_cw_head = build_polygon_geometry_with(
        factory,
        &[
            p2(3.4, ed_y - 0.15),
            p2(-1.2, ed_y - 3.6),
            p2(-1.2, ed_y + 3.3),
        ],
    );

    let flip_h_left =
        build_polygon_geometry_with(factory, &[p2(-2.3, -5.0), p2(-7.8, 4.8), p2(-2.3, 4.8)]);
    let flip_h_right =
        build_polygon_geometry_with(factory, &[p2(2.3, -5.0), p2(7.8, 4.8), p2(2.3, 4.8)]);
    let flip_v_top =
        build_polygon_geometry_with(factory, &[p2(-5.2, -2.2), p2(5.0, -7.0), p2(5.0, -2.2)]);
    let flip_v_bot =
        build_polygon_geometry_with(factory, &[p2(-5.2, 2.2), p2(5.0, 7.0), p2(5.0, 2.2)]);

    StaticGeometries {
        play_tri_unit,
        crystal_cap_unit,
        wing_left,
        wing_right,
        rot_ccw_arc,
        rot_ccw_head,
        rot_cw_arc,
        rot_cw_head,
        flip_h_left,
        flip_h_right,
        flip_v_top,
        flip_v_bot,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{DecodedImage, FitLayoutMode, ImageFormatKind};
    use crate::ui_layout::LayoutInput;
    use std::sync::Arc;
    use std::time::Duration;

    fn solid_opaque_image(w: u32, h: u32, b: u8, g: u8, r: u8) -> SharedImage {
        let mut px = vec![0u8; (w * h * 4) as usize];
        for c in px.chunks_exact_mut(4) {
            c[0] = b;
            c[1] = g;
            c[2] = r;
            c[3] = 255;
        }
        Arc::new(DecodedImage {
            path: PathBuf::from("solid_test.png"),
            format_kind: ImageFormatKind::Static("PNG".to_string()),
            file_size_bytes: 1024,
            width: w,
            height: h,
            has_alpha: false,
            frames: vec![ImageFrame {
                width: w,
                height: h,
                delay_ms: 0,
                bgra_premul: px,
            }],
            total_duration_ms: 0,
            exif_summary: Vec::new(),
        })
    }

    #[test]
    fn opaque_image_renders_tight_gaussian_shadow_with_zero_white_border() {
        let view_w = 640u32;
        let view_h = 480u32;
        let mut renderer = ViewerRenderer::new_offscreen(view_w, view_h, 96.0, ViewerLanguage::ZhCN)
            .expect("离屏渲染器初始化成功");
        let img = solid_opaque_image(200, 200, 180, 90, 30);
        let mut cam = ViewportCamera::default();
        cam.reset_to_fit_with_mode(200.0, 200.0, view_w as f32, view_h as f32, FitLayoutMode::Overlay);

        let siblings = vec![img.path.clone()];
        let layout = ViewerUiLayout::compute(&LayoutInput {
            view_w: view_w as f32,
            view_h: view_h as f32,
            sibling_count: 1,
            current_index: 0,
            filmstrip_scroll_offset: 0.0,
            is_animated: false,
            anim_playing: false,
            is_slideshow: false,
            is_windowed: false,
            is_actual_zoom: false,
            is_flipped_h: false,
            is_flipped_v: false,
            show_info: false,
            show_minimap: false,
            img_eff_w: 200.0,
            img_eff_h: 200.0,
            exif_line_count: 3,
        });

        let open_at = Instant::now();
        let params = RenderFrameParams {
            now: open_at + Duration::from_secs(2),
            open_started_at: open_at,
            lang: ViewerLanguage::ZhCN,
            view_w: view_w as f32,
            view_h: view_h as f32,
            dpi: 96.0,
            backdrop_open_progress: 1.0,
            open_anim_armed: true,
            open_progress: 1.0,
            close_progress: 0.0,
            close_snapshot: None,
            slide_progress: 1.0,
            slide_dir: 0.0,
            slide_is_crossfade: false,
            slide_fast_repeat: false,
            cinema_dim: 0.0,
            slideshow_portal: None,
            close_hover_anim: 0.0,
            play_hover_anim: 0.0,
            backdrop: BackdropMode::PicasaGlass,
            simulated_desktop_bg: Some([0.65, 0.65, 0.65, 1.0]),
            camera: &cam,
            image: Some(&img),
            fallback_thumb_path: None,
            frame_index: 0,
            error: None,
            loading_name: None,
            show_loading_box: false,
            current_index: 0,
            sibling_paths: &siblings,
            is_slideshow: false,
            is_windowed: false,
            layout: &layout,
            hovered: HitTarget::None,
            anim_speed: 1.0,
            selection_box: None,
            toast_text: None,
            show_info: false,
            zoom_badge_alpha: 0.0,
        };
        renderer.render(&params).expect("渲染成功");
        let fb = renderer.readback_offscreen_frame().expect("回读帧成功");

        let rc = cam.image_screen_rect(200.0, 200.0, view_w as f32, view_h as f32);
        let mid_y = rc.center_y().round() as u32;
        let sample_px = |x: u32, y: u32| -> (u8, u8, u8) {
            let off = ((y * view_w + x) * 4) as usize;
            (fb.bgra_premul[off + 2], fb.bgra_premul[off + 1], fb.bgra_premul[off])
        };

        // 远离图片处的基准底色（背景 + 58% 雾灰遮罩）
        let (bg_r, bg_g, bg_b) = sample_px(20, mid_y);
        let bg_luma = (bg_r as u32 + bg_g as u32 + bg_b as u32) / 3;

        // 紧邻图片右边外侧 2px 处：必须无白色相纸框，且应被贴边高斯阴影柔和压暗
        let edge_out_x = (rc.right.ceil() as u32 + 2).min(view_w - 1);
        let (sr, sg, sb) = sample_px(edge_out_x, mid_y);
        let shadow_luma = (sr as u32 + sg as u32 + sb as u32) / 3;
        assert!(
            sr < 200 && sg < 200 && sb < 200,
            "图片外缘 2px 严禁出现白色边框，实际 RGB=({sr},{sg},{sb})"
        );
        assert!(
            shadow_luma < bg_luma,
            "图片外缘 2px 应存在柔和边缘阴影压暗，shadow_luma={shadow_luma} vs bg_luma={bg_luma}"
        );
    }

    #[test]
    fn windowed_render_uses_opaque_background_and_omits_corner_close_badge() {
        let view_w = 640u32;
        let view_h = 480u32;
        let mut renderer = ViewerRenderer::new_offscreen(view_w, view_h, 96.0, ViewerLanguage::ZhCN)
            .expect("离屏渲染器初始化成功");
        let img = solid_opaque_image(200, 200, 40, 120, 200);
        let mut cam = ViewportCamera::default();
        cam.reset_to_fit_with_mode(200.0, 200.0, view_w as f32, view_h as f32, FitLayoutMode::Windowed);

        let siblings = vec![img.path.clone()];
        let layout = ViewerUiLayout::compute(&LayoutInput {
            view_w: view_w as f32,
            view_h: view_h as f32,
            sibling_count: 1,
            current_index: 0,
            filmstrip_scroll_offset: 0.0,
            is_animated: false,
            anim_playing: false,
            is_slideshow: false,
            is_windowed: true,
            is_actual_zoom: false,
            is_flipped_h: false,
            is_flipped_v: false,
            show_info: false,
            show_minimap: false,
            img_eff_w: 200.0,
            img_eff_h: 200.0,
            exif_line_count: 3,
        });

        let open_at = Instant::now();
        let params = RenderFrameParams {
            now: open_at + Duration::from_secs(2),
            open_started_at: open_at,
            lang: ViewerLanguage::ZhCN,
            view_w: view_w as f32,
            view_h: view_h as f32,
            dpi: 96.0,
            backdrop_open_progress: 1.0,
            open_anim_armed: true,
            open_progress: 1.0,
            close_progress: 0.0,
            close_snapshot: None,
            slide_progress: 1.0,
            slide_dir: 0.0,
            slide_is_crossfade: false,
            slide_fast_repeat: false,
            cinema_dim: 0.0,
            slideshow_portal: None,
            close_hover_anim: 0.0,
            play_hover_anim: 0.0,
            backdrop: BackdropMode::PicasaGlass,
            simulated_desktop_bg: None,
            camera: &cam,
            image: Some(&img),
            fallback_thumb_path: None,
            frame_index: 0,
            error: None,
            loading_name: None,
            show_loading_box: false,
            current_index: 0,
            sibling_paths: &siblings,
            is_slideshow: false,
            is_windowed: true,
            layout: &layout,
            hovered: HitTarget::None,
            anim_speed: 1.0,
            selection_box: None,
            toast_text: None,
            show_info: false,
            zoom_badge_alpha: 0.0,
        };
        renderer.render(&params).expect("小窗渲染成功");
        let fb = renderer.readback_offscreen_frame().expect("回读帧成功");

        // 左上角与右上角像素必须均为完全不透明底色（Alpha = 255），且右上角无四分之一圆深灰/白叉关闭徽章
        let tl_off = ((14 * view_w + 14) * 4) as usize;
        let tr_off = ((14 * view_w + (view_w - 14)) * 4) as usize;
        assert_eq!(fb.bgra_premul[tl_off + 3], 255);
        assert_eq!(fb.bgra_premul[tr_off + 3], 255);
        assert_eq!(
            &fb.bgra_premul[tl_off..tl_off + 4],
            &fb.bgra_premul[tr_off..tr_off + 4],
            "小窗口模式右上角不得绘制浮层关闭徽章"
        );
    }
}
