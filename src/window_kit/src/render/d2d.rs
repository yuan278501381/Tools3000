// ─────────────────────────────────────────────────────────────────────────────
// render/d2d.rs — 轻量 Direct2D 1.1 + DirectWrite + WIC 渲染核心
//                 支持两条硬件/透明管线：
//                 1) ID2D1DCRenderTarget + UpdateLayeredWindow（零常驻显存，用于透明边框与分区叠加层）
//                 2) ID2D1HwndRenderTarget（兼容 DwmRegisterThumbnail，用于 Alt+Tab 卡片）
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::constants::{FONT_MONO_FAMILY, FONT_UI_FALLBACK, FONT_UI_FAMILY};
use crate::core::win_utils::to_wide;
use std::ffi::c_void;
use std::mem::size_of;
use std::sync::OnceLock;
use windows::core::{Interface, PCWSTR};
use windows::Win32::Foundation::{HWND, POINT, RECT, SIZE};
use windows::Win32::Graphics::Direct2D::Common::{
    D2D1_ALPHA_MODE_PREMULTIPLIED, D2D1_COLOR_F, D2D1_PIXEL_FORMAT, D2D_RECT_F, D2D_SIZE_U,
};
use windows::Win32::Graphics::Direct2D::{
    D2D1CreateFactory, ID2D1Bitmap, ID2D1DCRenderTarget, ID2D1Factory, ID2D1HwndRenderTarget,
    ID2D1RenderTarget, D2D1_FACTORY_OPTIONS, D2D1_FACTORY_TYPE_SINGLE_THREADED,
    D2D1_HWND_RENDER_TARGET_PROPERTIES, D2D1_PRESENT_OPTIONS_NONE, D2D1_RENDER_TARGET_PROPERTIES,
    D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_ROUNDED_RECT,
    D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE,
};
use windows::Win32::Graphics::DirectWrite::{
    DWriteCreateFactory, IDWriteFactory, IDWriteTextFormat, IDWriteTextLayout,
    DWRITE_FACTORY_TYPE_SHARED, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL,
    DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_WEIGHT_SEMI_BOLD,
    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_TEXT_ALIGNMENT_CENTER,
    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_TRIMMING, DWRITE_TRIMMING_GRANULARITY_CHARACTER,
    DWRITE_WORD_WRAPPING_NO_WRAP,
};
use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_B8G8R8A8_UNORM;
use windows::Win32::Graphics::Gdi::{
    CreateCompatibleDC, CreateDIBSection, DeleteDC, DeleteObject, SelectObject, AC_SRC_ALPHA,
    AC_SRC_OVER, BITMAPINFO, BITMAPINFOHEADER, BI_RGB, BLENDFUNCTION, DIB_RGB_COLORS, HDC,
};
use windows::Win32::Graphics::Imaging::{
    CLSID_WICImagingFactory, GUID_WICPixelFormat32bppPBGRA, IWICImagingFactory,
    WICBitmapDitherTypeNone, WICBitmapPaletteTypeCustom,
};
use windows::Win32::System::Com::{CoCreateInstance, CLSCTX_INPROC_SERVER};
use windows::Win32::UI::WindowsAndMessaging::{UpdateLayeredWindow, HICON, ULW_ALPHA};

/// 全局共享无状态 D2D / DWrite / WIC 工厂（按需创建，体积小且跨覆盖层复用）
pub struct SharedGraphics {
    pub d2d_factory: ID2D1Factory,
    pub dwrite_factory: IDWriteFactory,
    pub wic_factory: Option<IWICImagingFactory>,
}

impl SharedGraphics {
    pub fn new() -> Result<Self, String> {
        unsafe {
            let options = D2D1_FACTORY_OPTIONS::default();
            let d2d_factory: ID2D1Factory =
                D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, Some(&options))
                    .map_err(|e| format!("D2D1CreateFactory failed: {e}"))?;
            let dwrite_factory: IDWriteFactory = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED)
                .map_err(|e| format!("DWriteCreateFactory failed: {e}"))?;
            let wic_factory: Option<IWICImagingFactory> =
                CoCreateInstance(&CLSID_WICImagingFactory, None, CLSCTX_INPROC_SERVER).ok();
            Ok(Self {
                d2d_factory,
                dwrite_factory,
                wic_factory,
            })
        }
    }

    pub fn create_dc_target(&self) -> Result<ID2D1DCRenderTarget, String> {
        let props = D2D1_RENDER_TARGET_PROPERTIES {
            r#type: D2D1_RENDER_TARGET_TYPE_DEFAULT,
            pixelFormat: D2D1_PIXEL_FORMAT {
                format: DXGI_FORMAT_B8G8R8A8_UNORM,
                alphaMode: D2D1_ALPHA_MODE_PREMULTIPLIED,
            },
            dpiX: 96.0,
            dpiY: 96.0,
            usage: D2D1_RENDER_TARGET_USAGE_NONE,
            minLevel: Default::default(),
        };
        unsafe {
            self.d2d_factory
                .CreateDCRenderTarget(&props)
                .map_err(|e| format!("CreateDCRenderTarget failed: {e}"))
        }
    }

    pub fn create_hwnd_target(
        &self,
        hwnd: HWND,
        width_px: u32,
        height_px: u32,
        dpi: u32,
    ) -> Result<ID2D1HwndRenderTarget, String> {
        let dpi_f = (dpi.max(96)) as f32;
        let props = D2D1_RENDER_TARGET_PROPERTIES {
            r#type: D2D1_RENDER_TARGET_TYPE_DEFAULT,
            pixelFormat: D2D1_PIXEL_FORMAT {
                format: DXGI_FORMAT_B8G8R8A8_UNORM,
                alphaMode: D2D1_ALPHA_MODE_PREMULTIPLIED,
            },
            dpiX: dpi_f,
            dpiY: dpi_f,
            usage: D2D1_RENDER_TARGET_USAGE_NONE,
            minLevel: Default::default(),
        };
        let hwnd_props = D2D1_HWND_RENDER_TARGET_PROPERTIES {
            hwnd,
            pixelSize: D2D_SIZE_U {
                width: width_px.max(1),
                height: height_px.max(1),
            },
            presentOptions: D2D1_PRESENT_OPTIONS_NONE,
        };
        unsafe {
            let rt = self
                .d2d_factory
                .CreateHwndRenderTarget(&props, &hwnd_props)
                .map_err(|e| format!("CreateHwndRenderTarget failed: {e}"))?;
            rt.SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            Ok(rt)
        }
    }

    pub fn create_text_format(
        &self,
        family: &str,
        size_dip: f32,
        bold: bool,
        semi_bold: bool,
        centered: bool,
    ) -> Result<IDWriteTextFormat, String> {
        let fam = to_wide(family);
        let locale = to_wide("zh-CN");
        let weight = if bold {
            DWRITE_FONT_WEIGHT_BOLD
        } else if semi_bold {
            DWRITE_FONT_WEIGHT_SEMI_BOLD
        } else {
            DWRITE_FONT_WEIGHT_MEDIUM
        };
        unsafe {
            let fmt = self
                .dwrite_factory
                .CreateTextFormat(
                    PCWSTR(fam.as_ptr()),
                    None,
                    weight,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    size_dip.max(12.0),
                    PCWSTR(locale.as_ptr()),
                )
                .map_err(|e| format!("CreateTextFormat failed: {e}"))?;
            let _ = fmt.SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            let _ = fmt.SetTextAlignment(if centered {
                DWRITE_TEXT_ALIGNMENT_CENTER
            } else {
                DWRITE_TEXT_ALIGNMENT_LEADING
            });
            let _ = fmt.SetParagraphAlignment(if centered {
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER
            } else {
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR
            });
            Ok(fmt)
        }
    }

    /// 跨 Win10 / Server 2019 / Server 2022 / Win11 探测并缓存本机可用最佳界面字体族
    fn resolve_ui_font_family(&self) -> &'static str {
        static CACHED: OnceLock<&'static str> = OnceLock::new();
        CACHED.get_or_init(|| {
            let candidates = [FONT_UI_FAMILY, FONT_UI_FALLBACK, "Segoe UI"];
            if let Ok(Some(col)) = unsafe {
                let mut col = None;
                self.dwrite_factory
                    .GetSystemFontCollection(&mut col, false)
                    .map(|()| col)
            } {
                for &fam in &candidates {
                    let wide = to_wide(fam);
                    let mut idx = 0u32;
                    let mut exists = windows::Win32::Foundation::BOOL(0);
                    if unsafe {
                        col.FindFamilyName(PCWSTR(wide.as_ptr()), &mut idx, &mut exists)
                            .is_ok()
                    } && exists.as_bool()
                    {
                        return fam;
                    }
                }
            }
            FONT_UI_FALLBACK
        })
    }

    /// 跨 Win10 / Server 2019 / Server 2022 / Win11 探测并缓存本机可用最佳等宽字体族
    fn resolve_mono_font_family(&self) -> &'static str {
        static CACHED: OnceLock<&'static str> = OnceLock::new();
        CACHED.get_or_init(|| {
            let candidates = [FONT_MONO_FAMILY, "Cascadia Code", "Consolas"];
            if let Ok(Some(col)) = unsafe {
                let mut col = None;
                self.dwrite_factory
                    .GetSystemFontCollection(&mut col, false)
                    .map(|()| col)
            } {
                for &fam in &candidates {
                    let wide = to_wide(fam);
                    let mut idx = 0u32;
                    let mut exists = windows::Win32::Foundation::BOOL(0);
                    if unsafe {
                        col.FindFamilyName(PCWSTR(wide.as_ptr()), &mut idx, &mut exists)
                            .is_ok()
                    } && exists.as_bool()
                    {
                        return fam;
                    }
                }
            }
            "Consolas"
        })
    }

    pub fn ui_text_format(&self, size_dip: f32, bold: bool, centered: bool) -> Result<IDWriteTextFormat, String> {
        let fam = self.resolve_ui_font_family();
        self.create_text_format(fam, size_dip, bold, !bold, centered)
            .or_else(|_| self.create_text_format(FONT_UI_FALLBACK, size_dip, bold, !bold, centered))
    }

    pub fn mono_text_format(&self, size_dip: f32, bold: bool, centered: bool) -> Result<IDWriteTextFormat, String> {
        let fam = self.resolve_mono_font_family();
        self.create_text_format(fam, size_dip, bold, false, centered)
            .or_else(|_| self.create_text_format("Consolas", size_dip, bold, false, centered))
    }

    /// 创建支持省略号自动截断与局部高亮着色的 DirectWrite TextLayout
    pub fn create_trimmed_text_layout(
        &self,
        text: &str,
        format: &IDWriteTextFormat,
        max_width: f32,
        max_height: f32,
    ) -> Option<IDWriteTextLayout> {
        let wide: Vec<u16> = text.encode_utf16().collect();
        unsafe {
            let layout = self
                .dwrite_factory
                .CreateTextLayout(&wide, format, max_width.max(4.0), max_height.max(4.0))
                .ok()?;
            if let Ok(ellipsis) = self.dwrite_factory.CreateEllipsisTrimmingSign(format) {
                let trim = DWRITE_TRIMMING {
                    granularity: DWRITE_TRIMMING_GRANULARITY_CHARACTER,
                    delimiter: 0,
                    delimiterCount: 0,
                };
                let _ = layout.SetTrimming(&trim, &ellipsis);
            }
            Some(layout)
        }
    }

    /// 将 Win32 HICON 通过 WIC 转换为预乘 BGRA ID2D1Bitmap（单次转换，绝不泄漏中间对象）
    pub fn hicon_to_d2d_bitmap(
        &self,
        rt: &ID2D1RenderTarget,
        hicon: HICON,
    ) -> Option<ID2D1Bitmap> {
        if hicon.is_invalid() {
            return None;
        }
        let wic = self.wic_factory.as_ref()?;
        unsafe {
            let wic_bmp = wic.CreateBitmapFromHICON(hicon).ok()?;
            let converter = wic.CreateFormatConverter().ok()?;
            converter
                .Initialize(
                    &wic_bmp,
                    &GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone,
                    None,
                    0.0,
                    WICBitmapPaletteTypeCustom,
                )
                .ok()?;
            rt.CreateBitmapFromWicBitmap(&converter, None).ok()
        }
    }
}

pub fn rgba(r: f32, g: f32, b: f32, a: f32) -> D2D1_COLOR_F {
    D2D1_COLOR_F { r, g, b, a }
}

pub fn rgb_a(rgb: [f32; 3], a: f32) -> D2D1_COLOR_F {
    D2D1_COLOR_F {
        r: rgb[0],
        g: rgb[1],
        b: rgb[2],
        a,
    }
}

pub fn rect_f(left: f32, top: f32, right: f32, bottom: f32) -> D2D_RECT_F {
    D2D_RECT_F {
        left,
        top,
        right,
        bottom,
    }
}

pub fn rounded_rect(r: D2D_RECT_F, radius: f32) -> D2D1_ROUNDED_RECT {
    D2D1_ROUNDED_RECT {
        rect: r,
        radiusX: radius,
        radiusY: radius,
    }
}

/// 三次缓出曲线（桌面级自然减速手感）
#[inline]
pub fn ease_out_cubic(t: f32) -> f32 {
    let x = t.clamp(0.0, 1.0);
    1.0 - (1.0 - x).powi(3)
}

/// 线性插值
#[inline]
pub fn lerp_f32(a: f32, b: f32, t: f32) -> f32 {
    a + (b - a) * t.clamp(0.0, 1.0)
}

/// 帧率无关指数阻尼逼近（60Hz / 120Hz / 144Hz 屏幕体感完全一致）
#[inline]
pub fn exp_damp_f32(current: f32, target: f32, dt_ms: f32, half_life_ms: f32) -> f32 {
    if (current - target).abs() < 0.001 || half_life_ms <= 0.0 {
        return target;
    }
    let alpha = 1.0 - 0.5_f32.powf(dt_ms.max(1.0) / half_life_ms);
    current + (target - current) * alpha
}

/// 零重绘、零内存分配更新分层窗口整体透明度（复用已有 DWM 分层表面，适合 4K 144Hz 丝滑淡入）
pub fn update_layered_window_alpha_only(hwnd: HWND, alpha: u8) {
    let blend = BLENDFUNCTION {
        BlendOp: AC_SRC_OVER as u8,
        BlendFlags: 0,
        SourceConstantAlpha: alpha,
        AlphaFormat: AC_SRC_ALPHA as u8,
    };
    unsafe {
        let _ = UpdateLayeredWindow(
            hwnd,
            HDC::default(),
            None,
            None,
            HDC::default(),
            None,
            windows::Win32::Foundation::COLORREF(0),
            Some(&blend),
            ULW_ALPHA,
        );
    }
}

/// 使用 Direct2D DCRenderTarget 绘制逐像素透明 BGRA 表面，并通过 UpdateLayeredWindow 原子提交。
/// 绘制完毕立即销毁临时 DIB 与内存 DC，保障零 GDI 句柄常驻泄漏！
pub fn render_layered_window_d2d<F>(
    gfx: &SharedGraphics,
    hwnd: HWND,
    screen_x: i32,
    screen_y: i32,
    width: i32,
    height: i32,
    draw_fn: F,
) -> Result<(), String>
where
    F: FnOnce(&ID2D1RenderTarget),
{
    render_layered_window_d2d_alpha(gfx, hwnd, screen_x, screen_y, width, height, 255, draw_fn)
}

pub fn render_layered_window_d2d_alpha<F>(
    gfx: &SharedGraphics,
    hwnd: HWND,
    screen_x: i32,
    screen_y: i32,
    width: i32,
    height: i32,
    source_alpha: u8,
    draw_fn: F,
) -> Result<(), String>
where
    F: FnOnce(&ID2D1RenderTarget),
{
    if width <= 0 || height <= 0 {
        return Ok(());
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
                biHeight: -height, // Top-down DIB
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
        let bind_rect = RECT {
            left: 0,
            top: 0,
            right: width,
            bottom: height,
        };

        let bind_res = dc_rt.BindDC(mem_dc, &bind_rect);
        let mut draw_err: Option<String> = None;
        if let Err(e) = bind_res {
            draw_err = Some(format!("BindDC failed: {e}"));
        } else {
            let rt: ID2D1RenderTarget = dc_rt.cast().map_err(|e| format!("cast rt failed: {e}"))?;
            rt.SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt.BeginDraw();
            rt.Clear(Some(&rgba(0.0, 0.0, 0.0, 0.0)));
            draw_fn(&rt);
            if let Err(e) = rt.EndDraw(None, None) {
                draw_err = Some(format!("EndDraw failed: {e}"));
            }
        }

        if draw_err.is_none() {
            let dst_pt = POINT {
                x: screen_x,
                y: screen_y,
            };
            let size = SIZE {
                cx: width,
                cy: height,
            };
            let src_pt = POINT { x: 0, y: 0 };
            let blend = BLENDFUNCTION {
                BlendOp: AC_SRC_OVER as u8,
                BlendFlags: 0,
                SourceConstantAlpha: source_alpha,
                AlphaFormat: AC_SRC_ALPHA as u8,
            };
            let _ = UpdateLayeredWindow(
                hwnd,
                HDC::default(),
                Some(&dst_pt),
                Some(&size),
                mem_dc,
                Some(&src_pt),
                windows::Win32::Foundation::COLORREF(0),
                Some(&blend),
                ULW_ALPHA,
            );
        }

        // 立即归还 GDI 对象，杜绝任何句柄残留
        let _ = SelectObject(mem_dc, old_obj);
        let _ = DeleteObject(dib);
        let _ = DeleteDC(mem_dc);

        if let Some(err) = draw_err {
            Err(err)
        } else {
            Ok(())
        }
    }
}
