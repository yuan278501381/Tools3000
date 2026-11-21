// ─────────────────────────────────────────────────────────────────────────────
// renderer.rs — Direct3D11 + DirectComposition + Direct2D 1.1 + DirectWrite
//               硬件加速矢量自绘引擎（高分屏 100%~250%+ 原生超清 · 6 大物理动效）
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{
    CLOSE_HUD_FADEOUT_RATIO, COLOR_ACCENT, COLOR_BACKDROP_GLASS, COLOR_BACKDROP_LIGHT,
    COLOR_BACKDROP_SOLID_DARK, COLOR_BAR_BG, COLOR_PANEL_BG, COLOR_PANEL_BORDER,
    COLOR_PANEL_TOP_SHEEN, COLOR_TEXT_PRIMARY, COLOR_TEXT_SECONDARY, FILMSTRIP_DIM_OPACITY,
    FONT_MONO_FAMILY, FONT_UI_FAMILY, OPEN_START_SCALE_RATIO, SLIDE_FAST_OFFSET_DIP,
    SLIDE_MICRO_OFFSET_DIP, THUMB_INITIAL_STAGGER_SEC, THUMB_ITEM_GROW_DURATION_SEC,
};
use crate::i18n::ViewerLanguage;
use crate::physics::{
    close_implode_curve, ease_out_cubic, ease_out_quart, ease_out_quint, smoothstep_s_curve,
    KenBurnsTrack,
};
use crate::types::{
    format_file_size, BackdropMode, CloseImplodeSnapshot, ImageFrame, RectF, SharedImage,
    ThumbnailBitmap, ToolbarAction,
};
use crate::ui_layout::{HitTarget, ToolbarButtonLayout, ViewerUiLayout};
use crate::viewport::ViewportCamera;
use std::collections::HashMap;
use std::path::PathBuf;
use std::time::Instant;
use windows::core::{Interface, PCWSTR};
use windows::Foundation::Numerics::Matrix3x2;
use windows::Win32::Foundation::HWND;
use windows::Win32::Graphics::Direct2D::Common::{
    D2D1_ALPHA_MODE_PREMULTIPLIED, D2D1_COLOR_F, D2D1_FIGURE_BEGIN_FILLED,
    D2D1_FIGURE_BEGIN_HOLLOW, D2D1_FIGURE_END_CLOSED, D2D1_FIGURE_END_OPEN, D2D1_GRADIENT_STOP,
    D2D1_PIXEL_FORMAT, D2D_POINT_2F, D2D_RECT_F, D2D_SIZE_F, D2D_SIZE_U,
};
use windows::Win32::Graphics::Direct2D::{
    D2D1CreateFactory, ID2D1Bitmap1, ID2D1BitmapBrush1, ID2D1Device, ID2D1DeviceContext,
    ID2D1Factory1, ID2D1LinearGradientBrush, ID2D1PathGeometry, ID2D1SolidColorBrush,
    ID2D1StrokeStyle, D2D1_ARC_SEGMENT, D2D1_ARC_SIZE_LARGE, D2D1_ARC_SIZE_SMALL,
    D2D1_BITMAP_BRUSH_PROPERTIES1, D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1_BITMAP_OPTIONS_NONE,
    D2D1_BITMAP_OPTIONS_TARGET, D2D1_BITMAP_PROPERTIES1, D2D1_BRUSH_PROPERTIES,
    D2D1_BUFFER_PRECISION_8BPC_UNORM, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND,
    D2D1_COLOR_INTERPOLATION_MODE_STRAIGHT, D2D1_COLOR_SPACE_SRGB, D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
    D2D1_DRAW_TEXT_OPTIONS_CLIP, D2D1_ELLIPSE, D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_WRAP,
    D2D1_FACTORY_OPTIONS, D2D1_FACTORY_TYPE_SINGLE_THREADED,
    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, D2D1_INTERPOLATION_MODE_LINEAR,
    D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES,
    D2D1_LINE_JOIN_MITER, D2D1_LINE_JOIN_ROUND, D2D1_QUADRATIC_BEZIER_SEGMENT, D2D1_ROUNDED_RECT,
    D2D1_STROKE_STYLE_PROPERTIES1, D2D1_SWEEP_DIRECTION_CLOCKWISE,
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
    DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_WORD_WRAPPING_NO_WRAP,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_ALPHA_MODE_PREMULTIPLIED, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SAMPLE_DESC,
};
use windows::Win32::Graphics::Dxgi::{
    IDXGIDevice, IDXGIFactory2, IDXGISurface, IDXGISwapChain1, DXGI_SCALING_STRETCH,
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

/// 离场旧图快照（含幻灯片 Ken Burns 连续推镜轨迹，保证 900ms 叠化退场期间推镜不急停）
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
    ken_burns: Option<KenBurnsTrack>,
}

struct UploadedThumb {
    bitmap: ID2D1Bitmap1,
    orig_w: u32,
    orig_h: u32,
}

pub struct RenderFrameParams<'a> {
    pub now: Instant,
    pub open_started_at: Instant,
    pub lang: ViewerLanguage,
    pub view_w: f32,
    pub view_h: f32,
    pub dpi: f32,
    pub open_progress: f32,
    pub close_progress: f32,
    pub close_snapshot: Option<CloseImplodeSnapshot>,
    pub slide_progress: f32,
    pub slide_dir: f32,
    pub slide_is_crossfade: bool,
    pub slide_fast_repeat: bool,
    pub cinema_dim: f32,
    pub ken_burns: Option<KenBurnsTrack>,
    pub close_hover_anim: f32,
    pub play_hover_anim: f32,
    pub backdrop: BackdropMode,
    /// 仅在 `--dump-ui` 离线核验时模拟半透明下方的桌面底色
    pub simulated_desktop_bg: Option<[f32; 4]>,
    pub camera: &'a ViewportCamera,
    pub image: Option<&'a SharedImage>,
    pub fallback_thumb_path: Option<&'a PathBuf>,
    pub frame_index: usize,
    pub error_text: Option<&'a str>,
    pub loading_name: Option<&'a str>,
    pub show_loading_box: bool,
    pub current_index: usize,
    pub sibling_paths: &'a [PathBuf],
    pub is_slideshow: bool,
    pub slideshow_paused: bool,
    pub layout: &'a ViewerUiLayout,
    pub ui_opacity: f32,
    pub hovered: HitTarget,
    pub anim_speed: f32,
    pub selection_box: Option<RectF>,
    pub toast_text: Option<&'a str>,
    pub show_info: bool,
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
    stroke_round: ID2D1StrokeStyle,
    stroke_flat: ID2D1StrokeStyle,
    stroke_miter: ID2D1StrokeStyle,
    fmt_ui: IDWriteTextFormat,
    fmt_ui_center: IDWriteTextFormat,
    fmt_hud_top: IDWriteTextFormat,
    fmt_status_bold: IDWriteTextFormat,
    fmt_mono: IDWriteTextFormat,
    fmt_mono_center: IDWriteTextFormat,
    fmt_btn_1to1: IDWriteTextFormat,
    fmt_title: IDWriteTextFormat,

    phys_w: u32,
    phys_h: u32,
    dpi: f32,

    uploaded_key: Option<(PathBuf, usize)>,
    main_bitmap: Option<ID2D1Bitmap1>,
    outgoing_slide: Option<OutgoingSlide>,
    thumb_bitmaps: HashMap<PathBuf, UploadedThumb>,
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

            // 高分屏清晰排版：字号全部 >= 12.5 DIP，字重 >= 500 Medium
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
            let fmt_hud_top = create_text_format(
                &dwrite,
                ui_family,
                13.0,
                DWRITE_FONT_WEIGHT_MEDIUM,
                false,
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
                stroke_round,
                stroke_flat,
                stroke_miter,
                fmt_ui,
                fmt_ui_center,
                fmt_hud_top,
                fmt_status_bold,
                fmt_mono,
                fmt_mono_center,
                fmt_btn_1to1,
                fmt_title,
                phys_w: w,
                phys_h: h,
                dpi,
                uploaded_key: None,
                main_bitmap: None,
                outgoing_slide: None,
                thumb_bitmaps: HashMap::new(),
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

    pub fn resize(&mut self, phys_w: u32, phys_h: u32, dpi: f32) -> Result<(), String> {
        let w = phys_w.max(1);
        let h = phys_h.max(1);
        let dpi_changed = (dpi - self.dpi).abs() > 0.5;
        if w == self.phys_w && h == self.phys_h {
            if dpi_changed {
                self.dpi = dpi;
                self.bind_render_target(dpi)?;
            }
            return Ok(());
        }
        unsafe {
            self.d2d_ctx.SetTarget(None);
            self.target_bitmap = None;
            if let Some(sc) = self.swap_chain.as_ref() {
                sc.ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, Default::default())
                    .map_err(|e| format!("ResizeBuffers 失败: {e}"))?;
            }
        }
        self.phys_w = w;
        self.phys_h = h;
        self.dpi = dpi;
        self.bind_render_target(dpi)?;
        if let Some(dc) = self.dcomp_device.as_ref() {
            unsafe {
                let _ = dc.Commit();
            }
        }
        Ok(())
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

    pub fn ensure_thumb_uploaded(&mut self, path: &PathBuf, thumb: &ThumbnailBitmap) {
        if self.thumb_bitmaps.contains_key(path) {
            return;
        }
        if self.thumb_bitmaps.len() > 180 {
            self.thumb_bitmaps.clear();
        }
        if let Ok(bmp) = self.create_gpu_bitmap(thumb.width, thumb.height, &thumb.bgra_premul) {
            self.thumb_bitmaps.insert(
                path.clone(),
                UploadedThumb {
                    bitmap: bmp,
                    orig_w: thumb.orig_width.max(1),
                    orig_h: thumb.orig_height.max(1),
                },
            );
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
    }

    pub fn clear_main_bitmap(&mut self) {
        self.uploaded_key = None;
        self.main_bitmap = None;
    }

    /// 零拷贝接管当前 GPU 主图纹理作为翻图微位移或幻灯片 900ms 交叉叠化的离场旧图
    pub fn capture_outgoing_for_slide(
        &mut self,
        camera: &ViewportCamera,
        img: &SharedImage,
        ken_burns: Option<KenBurnsTrack>,
    ) {
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
                ken_burns,
            });
        }
    }

    /// 叠化期间继续推进离场旧图的 Ken Burns 微推镜，杜绝离场图突然急停卡顿
    pub fn advance_outgoing_ken_burns(&mut self, dt_sec: f32) {
        if let Some(out) = self.outgoing_slide.as_mut() {
            if let Some(kb) = out.ken_burns.as_mut() {
                kb.advance(dt_sec);
            }
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
        if now.duration_since(open_started_at).as_secs_f32() < 0.70 {
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
        let props = D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES {
            startPoint: start,
            endPoint: end,
        };
        let bprops = D2D1_BRUSH_PROPERTIES {
            opacity: 1.0,
            transform: Matrix3x2::identity(),
        };
        self.d2d_ctx
            .CreateLinearGradientBrush(&props, Some(&bprops), &col)
            .ok()
    }

    unsafe fn build_polygon_geometry(&self, pts: &[D2D_POINT_2F]) -> Option<ID2D1PathGeometry> {
        if pts.len() < 3 {
            return None;
        }
        let geom = self.d2d_factory.CreatePathGeometry().ok()?;
        let sink = geom.Open().ok()?;
        sink.BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
        for p in &pts[1..] {
            sink.AddLine(*p);
        }
        sink.EndFigure(D2D1_FIGURE_END_CLOSED);
        sink.Close().ok()?;
        Some(geom.into())
    }

    unsafe fn build_arc_geometry(
        &self,
        start: D2D_POINT_2F,
        end: D2D_POINT_2F,
        radius: f32,
        clockwise: bool,
        large_arc: bool,
    ) -> Option<ID2D1PathGeometry> {
        let geom = self.d2d_factory.CreatePathGeometry().ok()?;
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

    /// 构建中央水晶播放球的柔弧球冠高光层（紧贴上缘柔和过渡，消除硬切夹层圈）
    unsafe fn build_crystal_cap_geometry(
        &self,
        cx: f32,
        cy: f32,
        r: f32,
    ) -> Option<ID2D1PathGeometry> {
        let y_off = -2.2f32;
        let dx = (r * r - y_off * y_off).max(1.0).sqrt();
        let left_pt = p2(cx - dx, cy + y_off);
        let right_pt = p2(cx + dx, cy + y_off);
        let geom = self.d2d_factory.CreatePathGeometry().ok()?;
        let sink = geom.Open().ok()?;
        sink.BeginFigure(left_pt, D2D1_FIGURE_BEGIN_FILLED);
        sink.AddArc(&D2D1_ARC_SEGMENT {
            point: right_pt,
            size: D2D_SIZE_F {
                width: r,
                height: r,
            },
            rotationAngle: 0.0,
            sweepDirection: D2D1_SWEEP_DIRECTION_CLOCKWISE,
            arcSize: D2D1_ARC_SIZE_SMALL,
        });
        sink.AddQuadraticBezier(&D2D1_QUADRATIC_BEZIER_SEGMENT {
            point1: p2(cx, cy - 6.5),
            point2: left_pt,
        });
        sink.EndFigure(D2D1_FIGURE_END_CLOSED);
        sink.Close().ok()?;
        Some(geom.into())
    }

    fn ensure_main_frame_uploaded(&mut self, path: &PathBuf, frame_idx: usize, frame: &ImageFrame) {
        let need_upload = match &self.uploaded_key {
            Some((p, idx)) => p != path || *idx != frame_idx || self.main_bitmap.is_none(),
            None => true,
        };
        if !need_upload {
            return;
        }
        if let Ok(bmp) = self.create_gpu_bitmap(frame.width, frame.height, &frame.bgra_premul) {
            self.main_bitmap = Some(bmp);
            self.uploaded_key = Some((path.clone(), frame_idx));
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
                .map_err(|e| format!("上传 GPU 位图失败: {e}"))
        }
    }

    pub fn render(&mut self, params: &RenderFrameParams<'_>) -> Result<(), String> {
        if let Some(img) = params.image {
            let safe_idx = params.frame_index.min(img.frames.len().saturating_sub(1));
            self.ensure_main_frame_uploaded(&img.path, safe_idx, &img.frames[safe_idx]);
        }

        // ─── 1. 打开弹入 (`Opening`) 与向心坍缩关闭 (`Closing`) 物理参数解算 ────────────
        // Opening: 195ms 内主图从视口中心 `0.35 * fit_scale` 按 EaseOutQuint 清脆弹开至适应视口，遮罩 0 -> 0.78
        let open_eased = ease_out_quint(params.open_progress);
        let open_img_alpha = ease_out_cubic((params.open_progress * 1.45).clamp(0.0, 1.0));
        let open_scale_mul = OPEN_START_SCALE_RATIO + (1.0 - OPEN_START_SCALE_RATIO) * open_eased;

        // Closing: 175ms 内前 75ms HUD 快速消隐，主图无论放大/偏离/旋转均沿加速收束曲线向屏幕中心坍缩至 0.28 * fit_scale
        let is_closing = params.close_progress > 0.0001 || params.close_snapshot.is_some();
        let close_t = params.close_progress.clamp(0.0, 1.0);
        let close_implode_k = close_implode_curve(close_t);
        let close_backdrop_fade = if is_closing {
            1.0 - ease_out_cubic(close_t)
        } else {
            1.0
        };
        let close_img_fade = if is_closing {
            (1.0 - close_t).powf(1.32)
        } else {
            1.0
        };
        // 前 ~75ms 底部三段式 HUD、左上角浮标与右上角关闭徽章率先快速淡出
        let hud_close_fade = if is_closing {
            let h = (1.0 - close_t / CLOSE_HUD_FADEOUT_RATIO).clamp(0.0, 1.0);
            h * h
        } else {
            1.0
        };

        let backdrop_life_alpha = (open_eased * close_backdrop_fade).clamp(0.0, 1.0);
        let img_life_alpha = (open_img_alpha * close_img_fade).clamp(0.0, 1.0);
        let hud_life_alpha = (open_eased * hud_close_fade).clamp(0.0, 1.0);

        // ─── 2. 切图微位移交叉淡化 vs 幻灯片 900ms S 曲线双图层叠化 ──────────────────
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

        unsafe {
            self.d2d_ctx.BeginDraw();
            self.d2d_ctx.SetTransform(&Matrix3x2::identity());

            // ─── 图层 1：全屏半透明桌面穿透遮罩（常规约 78% 暗色透桌面，幻灯片 350ms 沉入 #000000 纯黑放映厅）
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

            let base_bg = match params.backdrop {
                BackdropMode::PicasaGlass => COLOR_BACKDROP_GLASS,
                BackdropMode::SolidDark => COLOR_BACKDROP_SOLID_DARK,
                BackdropMode::SoftLight => COLOR_BACKDROP_LIGHT,
            };
            let eff_dim = params
                .close_snapshot
                .map(|s| s.start_cinema_dim)
                .unwrap_or(params.cinema_dim);
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

            // ─── 图层 2a：离场旧图（幻灯片 Ken Burns + 900ms 交叉叠化 / 切图 140ms 微位移淡出）
            if slide_active {
                if let Some(out) = self.outgoing_slide.as_ref() {
                    let (kb_scale, kb_ox, kb_oy) =
                        out.ken_burns.map(|k| k.sample()).unwrap_or((1.0, 0.0, 0.0));
                    let (out_dx, out_scale, out_opacity) = if params.slide_is_crossfade {
                        (kb_ox, kb_scale, (1.0 - slide_eased) * img_life_alpha)
                    } else {
                        let dx = -params.slide_dir * slide_offset_span * slide_eased;
                        let s = 1.0 - 0.035 * slide_eased;
                        let op = (1.0 - params.slide_progress).powf(1.85) * img_life_alpha;
                        (dx, s, op)
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
                            params.view_h * 0.5 + out.pan_y + kb_oy,
                            out_opacity,
                            false,
                            eff_dim > 0.65,
                            params.dpi,
                        );
                    }
                }
            }

            // ─── 图层 2b：中央无框悬浮相片画布（含打开弹入 / Ken Burns / 微位移切入 / 旋转翻转 / 向心坍缩）
            let (in_dx, in_slide_scale, in_opacity) = if slide_active {
                if params.slide_is_crossfade {
                    (0.0, 1.0, slide_eased * img_life_alpha)
                } else {
                    (
                        params.slide_dir * slide_offset_span * (1.0 - slide_eased),
                        0.965 + 0.035 * slide_eased,
                        (0.18 + 0.82 * slide_eased) * img_life_alpha,
                    )
                }
            } else {
                (0.0, 1.0, img_life_alpha)
            };

            let (kb_scale, kb_ox, kb_oy) = if params.is_slideshow && !is_closing {
                params
                    .ken_burns
                    .map(|k| k.sample())
                    .unwrap_or((1.0, 0.0, 0.0))
            } else {
                (1.0, 0.0, 0.0)
            };
            let kb_blend = params.cinema_dim.clamp(0.0, 1.0);
            let eff_kb_scale = 1.0 + (kb_scale - 1.0) * kb_blend;
            let eff_kb_ox = kb_ox * kb_blend;
            let eff_kb_oy = kb_oy * kb_blend;

            // 解算当前帧主图的真实缩放与中心坐标（Closing 态强制向屏幕正中心坍缩至 0.28 * fit_zoom）
            let (eff_zoom, raw_cx, raw_cy) = if let Some(snap) = params.close_snapshot {
                let z = snap.start_zoom + (snap.target_end_zoom - snap.start_zoom) * close_implode_k;
                let px = snap.start_pan_x * (1.0 - close_implode_k);
                let py = snap.start_pan_y * (1.0 - close_implode_k);
                (z, params.view_w * 0.5 + px, params.view_h * 0.5 + py)
            } else {
                let z = params.camera.zoom * open_scale_mul * in_slide_scale * eff_kb_scale;
                let cx = params.view_w * 0.5 + params.camera.pan_x + in_dx + eff_kb_ox;
                let cy = params.view_h * 0.5 + params.camera.pan_y + eff_kb_oy;
                (z, cx, cy)
            };

            if let (Some(img), Some(bmp)) = (params.image, self.main_bitmap.as_ref()) {
                if in_opacity > 0.008 {
                    let dip_per_px = params.camera.dip_per_image_pixel(eff_zoom);
                    let allow_exact_1to1 = !slide_active
                        && !is_closing
                        && !params.is_slideshow
                        && !params.camera.is_transform_animating()
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
                        eff_dim > 0.65,
                        params.dpi,
                    );
                }
            } else if let Some(err) = params.error_text {
                if hud_life_alpha > 0.02 {
                    let s = params.lang.strings();
                    self.draw_center_status(params.view_w, params.view_h, s.status_cannot_open, err);
                }
            } else if let Some(tp) = params.fallback_thumb_path {
                // 高速连切大图尚未解码完成时：按原图真实宽高计算完全一致的 Fit 矩形将缩略图放大顶上，
                // 保证 120fps 弹入/切图动画不断档，原图解码完成后无缝高清替换零跳变！
                if let Some(ut) = self.thumb_bitmaps.get(tp) {
                    if in_opacity > 0.008 {
                        let ow = ut.orig_w as f32;
                        let oh = ut.orig_h as f32;
                        let fit_z = ViewportCamera::compute_fit_zoom_for_mode(
                            ow,
                            oh,
                            params.camera.rotation,
                            params.view_w,
                            params.view_h,
                            params.camera.dpi_scale,
                            params.is_slideshow,
                        );
                        let py = ViewportCamera::default_fit_pan_y(
                            ow,
                            oh,
                            params.camera.rotation,
                            fit_z,
                            params.view_h,
                            params.camera.dpi_scale,
                            params.is_slideshow,
                        );
                        let z = if let Some(snap) = params.close_snapshot {
                            snap.start_zoom
                                + (snap.target_end_zoom - snap.start_zoom) * close_implode_k
                        } else {
                            fit_z * open_scale_mul * in_slide_scale * eff_kb_scale
                        };
                        let cx = params.view_w * 0.5 + in_dx + eff_kb_ox;
                        let cy = params.view_h * 0.5 + py * (1.0 - close_implode_k) + eff_kb_oy;
                        let dip_per_orig_px = params.camera.dip_per_image_pixel(z);
                        // 将缩略图缩放到与原图完全相同的屏幕 DIP 尺寸
                        let ratio_w = ow / (ut.bitmap.GetSize().width.max(1.0));
                        self.draw_image_surface(
                            &ut.bitmap,
                            ut.bitmap.GetSize().width.max(1.0),
                            ut.bitmap.GetSize().height.max(1.0),
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
                            eff_dim > 0.65,
                            params.dpi,
                        );
                    }
                } else if params.show_loading_box && hud_life_alpha > 0.02 {
                    let s = params.lang.strings();
                    let msg = params.loading_name.unwrap_or(s.status_loading_default);
                    self.draw_center_status(params.view_w, params.view_h, s.status_loading_title, msg);
                }
            } else if params.show_loading_box && hud_life_alpha > 0.02 {
                let s = params.lang.strings();
                let msg = params.loading_name.unwrap_or(s.status_loading_default);
                self.draw_center_status(params.view_w, params.view_h, s.status_loading_title, msg);
            }

            // ─── 图层 3：Ctrl + 左键框选局部特写框与右下角缩放迷你导航框
            if let Some(sel) = params.selection_box {
                if hud_life_alpha > 0.05 {
                    self.draw_selection_box(sel);
                }
            }

            if hud_life_alpha > 0.02 && !params.is_slideshow {
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
                        hud_life_alpha,
                    );
                }
            }

            let ui_vis = params.ui_opacity.clamp(0.0, 1.0);
            let hud_alpha = (ui_vis * hud_life_alpha).clamp(0.0, 1.0);

            // ─── 图层 4：左上角极简元数据浮标 / 详细 EXIF 玻璃卡
            if params.show_info && hud_life_alpha > 0.02 {
                if let (Some(card_rc), Some(img)) = (params.layout.info_card_rect, params.image) {
                    self.draw_info_card(card_rc, img, params.dpi, params.lang, hud_life_alpha);
                }
            } else if !params.is_slideshow && hud_alpha > 0.015 {
                self.draw_top_left_metadata_hud(params, hud_alpha);
            }

            // ─── 图层 5：右上角标志性费茨定律 40 DIP 关闭徽章（2.5s 静止净空至 0.0，微动或甩角瞬间唤醒）
            if !params.is_slideshow {
                let close_hover =
                    params.hovered == HitTarget::ToolbarButton(ToolbarAction::CloseViewer);
                let corner_alpha = if close_hover {
                    hud_life_alpha
                } else {
                    hud_alpha
                };
                if corner_alpha > 0.015 {
                    self.draw_corner_close_button(params, corner_alpha);
                }
            }

            // ─── 图层 6：底部悬浮 HUD（常规看图为胶片流+对称水晶控制栏；幻灯片放映为极简迷你水晶播控胶囊）
            if params.is_slideshow {
                if hud_alpha > 0.015 {
                    let y_off = (1.0 - ui_vis) * 14.0 + (1.0 - hud_close_fade) * 18.0;
                    if y_off.abs() > 0.05 {
                        self.d2d_ctx
                            .SetTransform(&Matrix3x2::translation(0.0, y_off));
                    }
                    self.draw_bottom_status_text(params, hud_alpha);
                    self.draw_slideshow_mini_bar(params, hud_alpha);
                    if y_off.abs() > 0.05 {
                        self.d2d_ctx.SetTransform(&Matrix3x2::identity());
                    }
                }
            } else if hud_alpha > 0.015 {
                let bottom_y_off = (1.0 - open_eased) * 16.0
                    + (1.0 - hud_close_fade) * 26.0
                    + (1.0 - ui_vis) * 12.0;
                if bottom_y_off.abs() > 0.05 {
                    self.d2d_ctx
                        .SetTransform(&Matrix3x2::translation(0.0, bottom_y_off));
                }
                self.draw_bottom_status_text(params, hud_alpha);
                self.draw_anim_bar(params, hud_alpha);
                self.draw_toolbar_with_integrated_filmstrip(params, hud_alpha);
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

            self.d2d_ctx
                .EndDraw(None, None)
                .map_err(|e| format!("D2D EndDraw 失败: {e}"))?;
            if let Some(sc) = self.swap_chain.as_ref() {
                let _ = sc.Present(1, Default::default());
            } else {
                self.d3d_ctx.Flush();
            }
        }
        Ok(())
    }

    /// 在 DIP 尺度旋转/翻转局部空间内绘制多层同心半透明柔和立体投影（旋转动画全程丝滑跟随零闪烁）
    unsafe fn draw_photo_drop_shadow_local(&self, half_w: f32, half_h: f32, opacity: f32) {
        let layers = [
            (18.0f32, 6.5f32, 0.042f32, 9.0f32),
            (11.5, 4.2, 0.068, 6.5),
            (6.5, 2.4, 0.105, 4.2),
            (3.0, 1.2, 0.155, 2.4),
            (1.2, 0.5, 0.210, 1.2),
        ];
        for (expand, y_off, a, rad) in layers {
            self.brush.SetColor(&D2D1_COLOR_F {
                r: 0.0,
                g: 0.0,
                b: 0.0,
                a: a * opacity,
            });
            let rr = D2D1_ROUNDED_RECT {
                rect: D2D_RECT_F {
                    left: -half_w - expand,
                    top: -half_h - expand * 0.72 + y_off,
                    right: half_w + expand,
                    bottom: half_h + expand * 0.92 + y_off,
                },
                radiusX: rad,
                radiusY: rad,
            };
            self.d2d_ctx.FillRoundedRectangle(&rr, &self.brush);
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
        in_cinema_black: bool,
        dpi: f32,
    ) {
        let dpi_scale = (dpi / 96.0).max(0.5);
        let is_exact_1to1 = allow_exact_1to1 && (zoom - 1.0).abs() < 0.005;
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

        // 先切入 DIP 尺度「镜像 + 任意角度旋转 + 中心平移」矩阵，保证阴影与 1.5px 微白相纸描边全程同步旋转不闪烁
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

        if !in_cinema_black {
            let shadow_mat = matrix_scale(fx.abs(), fy.abs())
                * Matrix3x2::rotation(rotation_deg, 0.0, 0.0)
                * Matrix3x2::translation(cx, cy);
            self.d2d_ctx.SetTransform(&shadow_mat);
            self.draw_photo_drop_shadow_local(half_w_dip, half_h_dip, opacity.powf(1.25));
        }

        // 位图矩阵：像素空间 -> 逻辑 DIP -> 镜像翻转 -> 任意角度旋转 -> 屏幕中心
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

        if has_alpha {
            let inv_s = (1.0 / dip_per_px.max(0.01)).clamp(0.02, 50.0);
            self.checker_brush
                .SetTransform(&matrix_scale(inv_s, inv_s));
            self.checker_brush.SetOpacity(0.94 * opacity);
            self.d2d_ctx
                .FillRectangle(&dst_px_rect, &self.checker_brush);
        }

        let src_rect = D2D_RECT_F {
            left: 0.0,
            top: 0.0,
            right: img_w,
            bottom: img_h,
        };
        // 放大超过 2.0x 查看像素细节或 1:1 模式切换清晰像素采样；缩小时用高质量立方抗锯齿消除摩尔纹
        let interp = if zoom > 2.0 || is_exact_1to1 {
            D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
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

        // 非全黑影院态下外包 1.4 DIP 极细微白相纸内描边（随旋转/翻转矩阵同步贴合）
        if !in_cinema_black {
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
                a: 0.22 * opacity,
            });
            self.d2d_ctx
                .DrawRectangle(&border_rc, &self.brush, 1.35, None);
        }

        self.d2d_ctx.SetTransform(&Matrix3x2::identity());
    }

    /// 左上角极简元数据浮标：位于 `(18.0, 14.0)`，无死板底框，带 1px 黑色偏移投影
    /// 格式：`文件名.jpg  |  3840×2160  |  86%  |  5 / 42`
    unsafe fn draw_top_left_metadata_hud(&self, params: &RenderFrameParams<'_>, alpha: f32) {
        let Some(name) = params
            .sibling_paths
            .get(params.current_index)
            .and_then(|p| p.file_name())
            .map(|s| s.to_string_lossy().to_string())
        else {
            return;
        };

        let line = if let Some(img) = params.image {
            let zoom_pct = (params.camera.zoom * 100.0).round() as i32;
            format!(
                "{}   |   {}×{}   |   {}%   |   {} / {}",
                name,
                img.width,
                img.height,
                zoom_pct,
                params.current_index + 1,
                params.sibling_paths.len().max(1)
            )
        } else {
            format!(
                "{}   |   {} / {}",
                name,
                params.current_index + 1,
                params.sibling_paths.len().max(1)
            )
        };

        let wt = to_wide(&line);
        let rc = params.layout.top_left_hud_rect;

        // 1px 黑色柔和偏移投影保障在亮色桌面壁纸上依然清晰可读
        self.brush.SetColor(&D2D1_COLOR_F {
            r: 0.0,
            g: 0.0,
            b: 0.0,
            a: 0.78 * alpha,
        });
        self.d2d_ctx.DrawText(
            &wt[..wt.len() - 1],
            &self.fmt_hud_top,
            &D2D_RECT_F {
                left: rc.left + 1.0,
                top: rc.top + 1.2,
                right: rc.right + 1.0,
                bottom: rc.bottom + 1.2,
            },
            &self.brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            Default::default(),
        );

        self.brush
            .SetColor(&color_alpha([0.90, 0.92, 0.95, 0.92], alpha));
        self.d2d_ctx.DrawText(
            &wt[..wt.len() - 1],
            &self.fmt_hud_top,
            &to_d2d_rect(rc),
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

        self.brush.SetColor(&D2D1_COLOR_F {
            r: 0.0,
            g: 0.0,
            b: 0.0,
            a: 0.78 * alpha,
        });
        self.d2d_ctx.DrawText(
            &wt[..wt.len() - 1],
            &self.fmt_status_bold,
            &D2D_RECT_F {
                left: rc.left,
                top: rc.top + 1.3,
                right: rc.right,
                bottom: rc.bottom + 1.3,
            },
            &self.brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            Default::default(),
        );

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

        if let Some(img) = params.image {
            let zoom_pct = (params.camera.zoom * 100.0).round() as i32;
            if !params.camera.is_fit_mode {
                format!("{} ({} × {})  ·  {}%", name, img.width, img.height, zoom_pct)
            } else {
                format!("{} ({} × {})", name, img.width, img.height)
            }
        } else {
            name
        }
    }

    /// 右上角标志性费茨定律关闭徽章（紧贴屏幕最右上角物理顶点 `(view_w, 0)`，40 DIP 半径，悬停平滑弹扩发光）
    unsafe fn draw_corner_close_button(&self, params: &RenderFrameParams<'_>, alpha: f32) {
        let h = params.close_hover_anim.clamp(0.0, 1.0);
        let r = params.layout.close_corner_radius + 3.5 * h;
        let origin = p2(params.view_w, 0.0);

        // 1. 外围柔和多层投影与悬停微红光晕
        for (expand, a_base, a_red) in [(7.5f32, 0.06f32, 0.16f32), (3.5, 0.14, 0.28)] {
            let glow_col = lerp_rgba(
                [0.0, 0.0, 0.0, a_base],
                [0.88, 0.20, 0.22, a_red],
                h,
            );
            self.brush.SetColor(&color_alpha(glow_col, alpha));
            self.d2d_ctx.FillEllipse(
                &D2D1_ELLIPSE {
                    point: origin,
                    radiusX: r + expand,
                    radiusY: r + expand,
                },
                &self.brush,
            );
        }

        // 2. 主体深灰磨砂半透明四分之一圆扇形，悬停平滑过渡至高质感深绯红玻璃态
        let base_fill = lerp_rgba(
            [0.125, 0.132, 0.150, 0.72],
            [0.78, 0.21, 0.23, 0.90],
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

        // 3. 左下切弧细腻玻璃微亮描边
        let rim_col = lerp_rgba(
            [1.0, 1.0, 1.0, 0.18],
            [1.0, 0.88, 0.88, 0.48],
            h,
        );
        self.brush.SetColor(&color_alpha(rim_col, alpha));
        self.d2d_ctx.DrawEllipse(
            &D2D1_ELLIPSE {
                point: origin,
                radiusX: r - 0.5,
                radiusY: r - 0.5,
            },
            &self.brush,
            1.15,
            None,
        );

        // 4. 扇区光学视觉重心处的抗锯齿清脆白色 `×` 号（跨度 9.8 × 9.8 DIP，带 1px 暗影）
        let offset = 13.4 + 0.8 * h;
        let ccx = params.view_w - offset;
        let ccy = offset;
        let cr = 4.8 + 0.35 * h;

        self.brush
            .SetColor(&color_alpha([0.0, 0.0, 0.0, 0.45], alpha));
        self.d2d_ctx.DrawLine(
            p2(ccx - cr, ccy - cr + 0.9),
            p2(ccx + cr, ccy + cr + 0.9),
            &self.brush,
            2.0,
            &self.stroke_round,
        );
        self.d2d_ctx.DrawLine(
            p2(ccx + cr, ccy - cr + 0.9),
            p2(ccx - cr, ccy + cr + 0.9),
            &self.brush,
            2.0,
            &self.stroke_round,
        );

        let cross_col = lerp_rgba([0.92, 0.93, 0.96, 0.95], [1.0, 1.0, 1.0, 1.0], h);
        self.brush.SetColor(&color_alpha(cross_col, alpha));
        self.d2d_ctx.DrawLine(
            p2(ccx - cr, ccy - cr),
            p2(ccx + cr, ccy + cr),
            &self.brush,
            1.95,
            &self.stroke_round,
        );
        self.d2d_ctx.DrawLine(
            p2(ccx + cr, ccy - cr),
            p2(ccx - cr, ccy + cr),
            &self.brush,
            1.95,
            &self.stroke_round,
        );
    }

    /// 幻灯片放映态专属极简半透明水晶迷你播控胶囊：`[ ◀ ]  [ ⏸/▶ ]  [ ▶ ]  |  [ × ]`
    unsafe fn draw_slideshow_mini_bar(&self, params: &RenderFrameParams<'_>, alpha: f32) {
        let Some(bar) = params.layout.slideshow_mini_bar_rect else {
            return;
        };

        // 柔和底影 + 黑曜石半透明水晶胶囊 + 顶部 1px 亮银高光内边
        for (exp, a) in [(6.0f32, 0.14f32), (2.5, 0.26)] {
            self.brush
                .SetColor(&color_alpha([0.0, 0.0, 0.0, a], alpha));
            self.d2d_ctx.FillRoundedRectangle(
                &D2D1_ROUNDED_RECT {
                    rect: D2D_RECT_F {
                        left: bar.left - exp,
                        top: bar.top - exp * 0.5 + 2.0,
                        right: bar.right + exp,
                        bottom: bar.bottom + exp + 2.0,
                    },
                    radiusX: 24.0,
                    radiusY: 24.0,
                },
                &self.brush,
            );
        }

        let rr = D2D1_ROUNDED_RECT {
            rect: to_d2d_rect(bar),
            radiusX: bar.height() * 0.5,
            radiusY: bar.height() * 0.5,
        };
        if let Some(grad) = self.make_linear_gradient(
            p2(bar.left, bar.top),
            p2(bar.left, bar.bottom),
            &[
                (0.0, [0.18, 0.19, 0.22, 0.86]),
                (0.5, [0.11, 0.12, 0.14, 0.90]),
                (1.0, [0.07, 0.075, 0.09, 0.94]),
            ],
            alpha,
        ) {
            self.d2d_ctx.FillRoundedRectangle(&rr, &grad);
        } else {
            self.brush.SetColor(&color_alpha(COLOR_PANEL_BG, alpha));
            self.d2d_ctx.FillRoundedRectangle(&rr, &self.brush);
        }
        self.brush
            .SetColor(&color_alpha([1.0, 1.0, 1.0, 0.22], alpha));
        self.d2d_ctx
            .DrawRoundedRectangle(&rr, &self.brush, 1.0, None);

        // 分隔竖线（位于第 3 枚与第 4 枚退出按钮之间）
        if params.layout.slideshow_mini_buttons.len() == 4 {
            let b2 = params.layout.slideshow_mini_buttons[2].rect;
            let b3 = params.layout.slideshow_mini_buttons[3].rect;
            let sx = (b2.right + b3.left) * 0.5;
            self.brush
                .SetColor(&color_alpha([1.0, 1.0, 1.0, 0.16], alpha));
            self.d2d_ctx.DrawLine(
                p2(sx, bar.top + 10.0),
                p2(sx, bar.bottom - 10.0),
                &self.brush,
                1.0,
                None,
            );
        }

        for btn in &params.layout.slideshow_mini_buttons {
            let is_hover = params.hovered == HitTarget::ToolbarButton(btn.action);
            let cx = btn.rect.center_x();
            let cy = btn.rect.center_y();
            let r = btn.rect.width() * 0.5;

            if is_hover {
                let hcol = if btn.action == ToolbarAction::ExitSlideshow {
                    [0.80, 0.24, 0.26, 0.72]
                } else {
                    [1.0, 1.0, 1.0, 0.18]
                };
                self.brush.SetColor(&color_alpha(hcol, alpha));
                self.d2d_ctx.FillEllipse(
                    &D2D1_ELLIPSE {
                        point: p2(cx, cy),
                        radiusX: r,
                        radiusY: r,
                    },
                    &self.brush,
                );
            }

            let fg = if is_hover {
                [1.0, 1.0, 1.0, 1.0]
            } else {
                [0.86, 0.88, 0.92, 0.96]
            };
            self.brush.SetColor(&color_alpha(fg, alpha));

            match btn.action {
                ToolbarAction::PrevImage => {
                    self.d2d_ctx.DrawLine(
                        p2(cx + 2.4, cy - 5.2),
                        p2(cx - 3.0, cy),
                        &self.brush,
                        1.9,
                        &self.stroke_round,
                    );
                    self.d2d_ctx.DrawLine(
                        p2(cx - 3.0, cy),
                        p2(cx + 2.4, cy + 5.2),
                        &self.brush,
                        1.9,
                        &self.stroke_round,
                    );
                }
                ToolbarAction::NextImage => {
                    self.d2d_ctx.DrawLine(
                        p2(cx - 2.4, cy - 5.2),
                        p2(cx + 3.0, cy),
                        &self.brush,
                        1.9,
                        &self.stroke_round,
                    );
                    self.d2d_ctx.DrawLine(
                        p2(cx + 3.0, cy),
                        p2(cx - 2.4, cy + 5.2),
                        &self.brush,
                        1.9,
                        &self.stroke_round,
                    );
                }
                ToolbarAction::SlideshowTogglePause => {
                    if !params.slideshow_paused {
                        for x_off in [-2.8f32, 2.8f32] {
                            self.d2d_ctx.FillRoundedRectangle(
                                &D2D1_ROUNDED_RECT {
                                    rect: D2D_RECT_F {
                                        left: cx + x_off - 1.3,
                                        top: cy - 5.2,
                                        right: cx + x_off + 1.3,
                                        bottom: cy + 5.2,
                                    },
                                    radiusX: 0.7,
                                    radiusY: 0.7,
                                },
                                &self.brush,
                            );
                        }
                    } else {
                        self.draw_play_triangle(cx + 1.1, cy, 5.8);
                    }
                }
                ToolbarAction::ExitSlideshow => {
                    let cr = 4.4f32;
                    self.d2d_ctx.DrawLine(
                        p2(cx - cr, cy - cr),
                        p2(cx + cr, cy + cr),
                        &self.brush,
                        1.9,
                        &self.stroke_round,
                    );
                    self.d2d_ctx.DrawLine(
                        p2(cx + cr, cy - cr),
                        p2(cx - cr, cy + cr),
                        &self.brush,
                        1.9,
                        &self.stroke_round,
                    );
                }
                _ => {}
            }
        }
    }

    unsafe fn draw_toolbar_with_integrated_filmstrip(
        &self,
        params: &RenderFrameParams<'_>,
        alpha: f32,
    ) {
        let bar = params.layout.toolbar_rect;

        // 1. 底部 66 DIP 通透冷炭水晶底栏 + 顶部 1px 微亮玻璃内高光边
        if let Some(bar_grad) = self.make_linear_gradient(
            p2(bar.left, bar.top),
            p2(bar.left, bar.bottom),
            &[
                (0.0, [0.115, 0.120, 0.130, 0.82]),
                (0.68, [0.082, 0.086, 0.094, 0.87]),
                (1.0, [0.055, 0.058, 0.066, 0.92]),
            ],
            alpha,
        ) {
            self.d2d_ctx.FillRectangle(&to_d2d_rect(bar), &bar_grad);
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

        // 2. 背景层：无框缩略图胶片流（当前图居中放大亮白框，悬停上浮提亮，底部柔和镜面倒影）
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

        // 先绘制非当前缩略图，最后压顶绘制当前选中放大高亮缩略图，确保立体层次分明
        for pass_current in [false, true] {
            for item in &params.layout.filmstrip_items {
                if item.is_current != pass_current {
                    continue;
                }
                let pitch = (item.rect.width() + 2.0).max(1.0);
                let slot_dist = (item.rect.center_x() - strip_cx).abs() / pitch;

                let wave_delay = (slot_dist * THUMB_INITIAL_STAGGER_SEC).min(0.28);
                let wave_t =
                    ((since_open - wave_delay) / THUMB_ITEM_GROW_DURATION_SEC).clamp(0.0, 1.0);

                let born_t = params
                    .sibling_paths
                    .get(item.index)
                    .and_then(|p| self.thumb_born_at.get(p))
                    .map(|born| {
                        (params.now.duration_since(*born).as_secs_f32()
                            / THUMB_ITEM_GROW_DURATION_SEC)
                            .clamp(0.0, 1.0)
                    })
                    .unwrap_or(wave_t);

                let local_t = wave_t.min(born_t);
                if local_t <= 0.01 {
                    continue;
                }
                let grow_eased = ease_out_cubic(local_t);

                // 当前图放大 1.12x 微抬，悬停平滑上浮放大
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

                let grow_scale = (0.28 + 0.72 * grow_eased) * emph_scale;
                let cur_w = item.rect.width() * grow_scale;
                let cur_h = item.rect.height() * grow_scale;
                let cx_i = item.rect.center_x();
                let cy_i = item.rect.center_y() + (1.0 - grow_eased) * 6.0 + lift_y;
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
                let item_alpha = alpha * edge_fade * grow_eased;

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

                        // 底部镜面倒影
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
                            if let Some(fade_grad) = self.make_linear_gradient(
                                p2(refl_clip.left, refl_clip.top),
                                p2(refl_clip.left, refl_clip.bottom),
                                &[
                                    (0.0, [0.06, 0.064, 0.07, 0.25]),
                                    (0.65, [0.055, 0.058, 0.064, 0.80]),
                                    (1.0, [0.055, 0.058, 0.064, 0.96]),
                                ],
                                alpha,
                            ) {
                                self.d2d_ctx.FillRectangle(&refl_clip, &fade_grad);
                            }
                            self.d2d_ctx.PopAxisAlignedClip();
                        }
                    }
                }

                // 当前照片亮白高亮边框；悬停照片柔和提亮描边
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

            // 1c. 紧贴球顶的无断层柔弧水晶玻璃高光冠
            if let Some(cap) = self.build_crystal_cap_geometry(cx, cy, r - 0.6) {
                let cap_stops: [(f32, [f32; 4]); 3] = [
                    (0.0, [1.0, 1.0, 1.0, 0.74 + 0.14 * h]),
                    (0.58, [0.95, 0.97, 1.0, 0.44 + 0.12 * h]),
                    (1.0, [0.90, 0.92, 0.96, 0.16 + 0.08 * h]),
                ];
                if let Some(cgrad) = self.make_linear_gradient(
                    p2(cx, cy - r),
                    p2(cx, cy - 2.0),
                    &cap_stops,
                    alpha,
                ) {
                    self.d2d_ctx.FillGeometry(&cap, &cgrad, None);
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
        let pts = [
            p2(pod_cx + dir * 15.0, cy - 6.2),
            p2(pod_cx + dir * 36.8, cy - 6.2),
            p2(pod_cx + dir * 36.8, cy - 14.8),
            p2(pod_cx + dir * 53.2, cy),
            p2(pod_cx + dir * 36.8, cy + 14.8),
            p2(pod_cx + dir * 36.8, cy + 6.2),
            p2(pod_cx + dir * 15.0, cy + 6.2),
        ];
        let Some(geom) = self.build_polygon_geometry(&pts) else {
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
        if let Some(agrad) = self.make_linear_gradient(
            p2(pod_cx, cy - 14.8),
            p2(pod_cx, cy + 14.8),
            &arrow_stops,
            alpha,
        ) {
            self.d2d_ctx.FillGeometry(&geom, &agrad, None);
        }

        let edge_col = if is_hover {
            [0.95, 0.97, 1.0, 0.45]
        } else {
            [0.16, 0.17, 0.19, 0.58]
        };
        self.brush.SetColor(&color_alpha(edge_col, alpha));
        self.d2d_ctx
            .DrawGeometry(&geom, &self.brush, 1.0, &self.stroke_miter);
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
        let pts = [
            p2(cx - r * 0.62, cy - r * 0.84),
            p2(cx + r * 0.90, cy),
            p2(cx - r * 0.62, cy + r * 0.84),
        ];
        if let Some(geom) = self.build_polygon_geometry(&pts) {
            self.d2d_ctx.FillGeometry(&geom, &self.brush, None);
        }
    }

    /// 经典旋转矢量图标（适配高分屏 35x25 按钮比例）
    unsafe fn draw_rotate_icon(&self, cx: f32, cy: f32, ccw: bool) {
        let r = 4.0f32;
        let oy = cy + 2.0;

        let (start, end, clockwise, arrow_pts) = if ccw {
            let a0 = 162.0f32.to_radians();
            let a1 = 270.0f32.to_radians();
            let st = p2(cx + r * a0.cos(), oy + r * a0.sin());
            let ed = p2(cx + r * a1.cos(), oy + r * a1.sin());
            let tri = [
                p2(cx - 3.4, ed.y - 0.15),
                p2(cx + 1.2, ed.y - 3.6),
                p2(cx + 1.2, ed.y + 3.3),
            ];
            (st, ed, false, tri)
        } else {
            let a0 = 18.0f32.to_radians();
            let a1 = 270.0f32.to_radians();
            let st = p2(cx + r * a0.cos(), oy + r * a0.sin());
            let ed = p2(cx + r * a1.cos(), oy + r * a1.sin());
            let tri = [
                p2(cx + 3.4, ed.y - 0.15),
                p2(cx - 1.2, ed.y - 3.6),
                p2(cx - 1.2, ed.y + 3.3),
            ];
            (st, ed, true, tri)
        };

        if let Some(arc) = self.build_arc_geometry(start, end, r, clockwise, true) {
            self.d2d_ctx
                .DrawGeometry(&arc, &self.brush, 2.2, &self.stroke_flat);
        }
        if let Some(head) = self.build_polygon_geometry(&arrow_pts) {
            self.d2d_ctx.FillGeometry(&head, &self.brush, None);
        }
    }

    /// 专业图像软件高辨识度「对称轴双三角镜像」水平/垂直翻转图标
    unsafe fn draw_flip_icon(&self, cx: f32, cy: f32, horizontal: bool, alpha: f32) {
        let cur_col = self.brush.GetColor();
        if horizontal {
            // 竖直镜像中轴线
            self.d2d_ctx.DrawLine(
                p2(cx, cy - 6.4),
                p2(cx, cy + 6.4),
                &self.brush,
                1.35,
                &self.stroke_round,
            );
            // 左半实心直角镜像翼
            let left_tri = [
                p2(cx - 2.3, cy - 5.0),
                p2(cx - 7.8, cy + 4.8),
                p2(cx - 2.3, cy + 4.8),
            ];
            if let Some(g) = self.build_polygon_geometry(&left_tri) {
                self.d2d_ctx.FillGeometry(&g, &self.brush, None);
            }
            // 右半对称描边半透明镜像翼
            let right_tri = [
                p2(cx + 2.3, cy - 5.0),
                p2(cx + 7.8, cy + 4.8),
                p2(cx + 2.3, cy + 4.8),
            ];
            if let Some(g) = self.build_polygon_geometry(&right_tri) {
                self.brush.SetColor(&D2D1_COLOR_F {
                    r: cur_col.r,
                    g: cur_col.g,
                    b: cur_col.b,
                    a: cur_col.a * 0.25 * alpha.max(0.5),
                });
                self.d2d_ctx.FillGeometry(&g, &self.brush, None);
                self.brush.SetColor(&cur_col);
                self.d2d_ctx
                    .DrawGeometry(&g, &self.brush, 1.3, &self.stroke_round);
            }
        } else {
            // 水平镜像中轴线
            self.d2d_ctx.DrawLine(
                p2(cx - 6.8, cy),
                p2(cx + 6.8, cy),
                &self.brush,
                1.35,
                &self.stroke_round,
            );
            // 上半实心直角镜像翼
            let top_tri = [
                p2(cx - 5.2, cy - 2.2),
                p2(cx + 5.0, cy - 7.0),
                p2(cx + 5.0, cy - 2.2),
            ];
            if let Some(g) = self.build_polygon_geometry(&top_tri) {
                self.d2d_ctx.FillGeometry(&g, &self.brush, None);
            }
            // 下半对称描边半透明镜像翼
            let bot_tri = [
                p2(cx - 5.2, cy + 2.2),
                p2(cx + 5.0, cy + 7.0),
                p2(cx + 5.0, cy + 2.2),
            ];
            if let Some(g) = self.build_polygon_geometry(&bot_tri) {
                self.brush.SetColor(&D2D1_COLOR_F {
                    r: cur_col.r,
                    g: cur_col.g,
                    b: cur_col.b,
                    a: cur_col.a * 0.25 * alpha.max(0.5),
                });
                self.d2d_ctx.FillGeometry(&g, &self.brush, None);
                self.brush.SetColor(&cur_col);
                self.d2d_ctx
                    .DrawGeometry(&g, &self.brush, 1.3, &self.stroke_round);
            }
        }
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
        for (rk, rv) in &img.exif_summary {
            rows.push(s.localize_exif_pair(rk, rv));
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

    unsafe fn draw_center_status(&self, view_w: f32, view_h: f32, title: &str, detail: &str) {
        let box_w = 400.0f32.min(view_w - 40.0);
        let box_h = 92.0f32;
        let left = (view_w - box_w) * 0.5;
        let top = (view_h - box_h) * 0.5;
        let rr = D2D1_ROUNDED_RECT {
            rect: D2D_RECT_F {
                left,
                top,
                right: left + box_w,
                bottom: top + box_h,
            },
            radiusX: 9.0,
            radiusY: 9.0,
        };
        self.brush.SetColor(&color_f(COLOR_PANEL_BG));
        self.d2d_ctx.FillRoundedRectangle(&rr, &self.brush);
        self.brush.SetColor(&color_f(COLOR_PANEL_BORDER));
        self.d2d_ctx
            .DrawRoundedRectangle(&rr, &self.brush, 1.0, None);

        self.brush.SetColor(&color_f(COLOR_TEXT_PRIMARY));
        let wt = to_wide(title);
        self.d2d_ctx.DrawText(
            &wt[..wt.len() - 1],
            &self.fmt_ui_center,
            &D2D_RECT_F {
                left: left + 16.0,
                top: top + 14.0,
                right: left + box_w - 16.0,
                bottom: top + 42.0,
            },
            &self.brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            Default::default(),
        );
        self.brush.SetColor(&color_f(COLOR_TEXT_SECONDARY));
        let wd = to_wide(detail);
        self.d2d_ctx.DrawText(
            &wd[..wd.len() - 1],
            &self.fmt_ui_center,
            &D2D_RECT_F {
                left: left + 16.0,
                top: top + 44.0,
                right: left + box_w - 16.0,
                bottom: top + box_h - 12.0,
            },
            &self.brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            Default::default(),
        );
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
