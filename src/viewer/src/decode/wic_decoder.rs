// ─────────────────────────────────────────────────────────────────────────────
// decode/wic_decoder.rs — Windows 原生 WIC 硬件/SIMD 解码器 + 完整真实 EXIF 元数据
// 零额外体积通吃 JPG / PNG / BMP / ICO / TIFF / AVIF / HEIC / RAW
// ─────────────────────────────────────────────────────────────────────────────

use crate::decode::{try_alloc_zeroed_buffer, validate_image_dimensions};
use crate::types::{
    DecodeError, DecodedImage, ExifField, ImageFormatKind, ImageFrame, ThumbnailBitmap,
};
use std::cell::RefCell;
use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::path::Path;
use windows::core::{PCWSTR, PROPVARIANT};
use windows::Win32::Foundation::GENERIC_READ;
use windows::Win32::Graphics::Imaging::{
    CLSID_WICImagingFactory, GUID_WICPixelFormat32bppPBGRA, IWICBitmapFrameDecode,
    IWICBitmapSource, IWICImagingFactory, IWICMetadataQueryReader, WICBitmapDitherTypeNone,
    WICBitmapInterpolationModeFant, WICBitmapPaletteTypeCustom,
    WICBitmapTransformFlipHorizontal, WICBitmapTransformFlipVertical,
    WICBitmapTransformOptions, WICBitmapTransformRotate180, WICBitmapTransformRotate270,
    WICBitmapTransformRotate90, WICDecodeMetadataCacheOnDemand,
};
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CoUninitialize, CLSCTX_INPROC_SERVER, COINIT_MULTITHREADED,
};

/// 线程级 WIC 工厂与 COM 生命周期 RAII 守卫：
/// 后台解码线程仅初始化一次 COM 与 IWICImagingFactory 并复用，线程退出时先释放 COM 对象再 CoUninitialize
struct WicThreadContext {
    factory: Option<IWICImagingFactory>,
    com_inited: bool,
}

impl WicThreadContext {
    fn new() -> Self {
        let com_inited = unsafe { CoInitializeEx(None, COINIT_MULTITHREADED).is_ok() };
        let factory =
            unsafe { CoCreateInstance(&CLSID_WICImagingFactory, None, CLSCTX_INPROC_SERVER).ok() };
        Self {
            factory,
            com_inited,
        }
    }

    fn get_or_create_factory(&mut self) -> Result<IWICImagingFactory, DecodeError> {
        if let Some(f) = self.factory.as_ref() {
            return Ok(f.clone());
        }
        let created: IWICImagingFactory =
            unsafe { CoCreateInstance(&CLSID_WICImagingFactory, None, CLSCTX_INPROC_SERVER) }
                .map_err(|e| DecodeError::WicFailure(e.to_string()))?;
        self.factory = Some(created.clone());
        Ok(created)
    }
}

impl Drop for WicThreadContext {
    fn drop(&mut self) {
        // 严格顺序：必须在 CoUninitialize 之前释放全部 COM 接口指针
        self.factory = None;
        if self.com_inited {
            unsafe {
                CoUninitialize();
            }
        }
    }
}

thread_local! {
    static THREAD_WIC: RefCell<WicThreadContext> = RefCell::new(WicThreadContext::new());
}

fn get_thread_wic_factory() -> Result<IWICImagingFactory, DecodeError> {
    THREAD_WIC.with(|cell| cell.borrow_mut().get_or_create_factory())
}

unsafe fn query_prop(reader: &IWICMetadataQueryReader, queries: &[&str]) -> Option<PROPVARIANT> {
    for q in queries {
        let wide: Vec<u16> = q.encode_utf16().chain(Some(0)).collect();
        let mut val = PROPVARIANT::default();
        if reader
            .GetMetadataByName(PCWSTR(wide.as_ptr()), &mut val)
            .is_ok()
            && !val.is_empty()
        {
            return Some(val);
        }
    }
    None
}

unsafe fn query_string(reader: &IWICMetadataQueryReader, queries: &[&str]) -> Option<String> {
    let val = query_prop(reader, queries)?;
    let s = val.to_string();
    let trimmed = s.trim().trim_end_matches('\0').trim();
    if trimmed.is_empty() {
        None
    } else {
        Some(trimmed.to_string())
    }
}

/// 解析 WIC EXIF 有理数（VT_UI8 = 低32位分子 / 高32位分母，或 VT_R8 / VT_R4 / VT_UI4）
unsafe fn query_rational_f64(reader: &IWICMetadataQueryReader, queries: &[&str]) -> Option<f64> {
    let val = query_prop(reader, queries)?;
    if let Ok(raw) = u64::try_from(&val) {
        let num = (raw & 0xFFFF_FFFF) as u32 as f64;
        let den = ((raw >> 32) & 0xFFFF_FFFF) as u32 as f64;
        if den > 0.0 {
            return Some(num / den);
        }
        return Some(num);
    }
    if let Ok(f) = f64::try_from(&val) {
        return Some(f);
    }
    if let Ok(n) = u32::try_from(&val) {
        return Some(n as f64);
    }
    if let Ok(n) = u16::try_from(&val) {
        return Some(n as f64);
    }
    None
}

fn read_exif_orientation(frame: &IWICBitmapFrameDecode) -> u16 {
    unsafe {
        let Ok(reader) = frame.GetMetadataQueryReader() else {
            return 1;
        };
        let queries = [
            "app1/ifd/{ushort=274}",
            "/app1/ifd/{ushort=274}",
            "/ifd/{ushort=274}",
            "System.Photo.Orientation",
        ];
        if let Some(val) = query_prop(&reader, &queries) {
            if let Ok(v) = u16::try_from(&val) {
                if (1..=8).contains(&v) {
                    return v;
                }
            }
        }
    }
    1
}

/// 读取专业摄影 EXIF 元数据（相机型号、镜头、拍摄时间、光圈、快门、ISO、焦距）
fn read_camera_exif_fields(frame: &IWICBitmapFrameDecode) -> Vec<ExifField> {
    let mut out = Vec::new();
    unsafe {
        let Ok(reader) = frame.GetMetadataQueryReader() else {
            return out;
        };

        let make = query_string(
            &reader,
            &[
                "System.Photo.CameraManufacturer",
                "/app1/ifd/{ushort=271}",
                "/ifd/{ushort=271}",
            ],
        );
        let model = query_string(
            &reader,
            &[
                "System.Photo.CameraModel",
                "/app1/ifd/{ushort=272}",
                "/ifd/{ushort=272}",
            ],
        );
        let camera = match (make, model) {
            (Some(mk), Some(md)) => {
                if md.to_ascii_lowercase().starts_with(&mk.to_ascii_lowercase()) {
                    md
                } else {
                    format!("{mk} {md}")
                }
            }
            (None, Some(md)) => md,
            (Some(mk), None) => mk,
            (None, None) => String::new(),
        };
        if !camera.is_empty() {
            out.push(ExifField::Camera(camera));
        }

        if let Some(lens) = query_string(
            &reader,
            &[
                "System.Photo.LensModel",
                "/app1/ifd/exif/{ushort=42036}",
                "/ifd/exif/{ushort=42036}",
            ],
        ) {
            out.push(ExifField::Lens(lens));
        }

        if let Some(date) = query_string(
            &reader,
            &[
                "/app1/ifd/exif/{ushort=36867}",
                "/ifd/exif/{ushort=36867}",
                "/app1/ifd/{ushort=306}",
                "System.Photo.DateTaken",
            ],
        ) {
            out.push(ExifField::DateTaken(date));
        }

        let mut exposure_parts = Vec::new();
        if let Some(fnum) = query_rational_f64(
            &reader,
            &[
                "/app1/ifd/exif/{ushort=33437}",
                "/ifd/exif/{ushort=33437}",
                "System.Photo.FNumber",
            ],
        ) {
            if fnum > 0.1 && fnum < 128.0 {
                exposure_parts.push(format!("f/{fnum:.1}"));
            }
        }
        if let Some(exp) = query_rational_f64(
            &reader,
            &[
                "/app1/ifd/exif/{ushort=33434}",
                "/ifd/exif/{ushort=33434}",
                "System.Photo.ExposureTime",
            ],
        ) {
            if exp > 0.0 {
                if exp < 0.5 {
                    let denom = (1.0 / exp).round() as u32;
                    exposure_parts.push(format!("1/{denom}s"));
                } else {
                    exposure_parts.push(format!("{exp:.1}s"));
                }
            }
        }
        if let Some(iso) = query_rational_f64(
            &reader,
            &[
                "/app1/ifd/exif/{ushort=34855}",
                "/ifd/exif/{ushort=34855}",
                "System.Photo.ISOSpeed",
            ],
        ) {
            if (10.0..=1_000_000.0).contains(&iso) {
                exposure_parts.push(format!("ISO {}", iso.round() as u32));
            }
        }
        if !exposure_parts.is_empty() {
            out.push(ExifField::Exposure(exposure_parts.join(" · ")));
        }

        if let Some(fl) = query_rational_f64(
            &reader,
            &[
                "/app1/ifd/exif/{ushort=37386}",
                "/ifd/exif/{ushort=37386}",
                "System.Photo.FocalLength",
            ],
        ) {
            if fl > 0.5 && fl < 5000.0 {
                out.push(ExifField::FocalLengthMm(fl));
            }
        }
    }
    out
}

fn exif_orientation_to_wic_transform(orientation: u16) -> WICBitmapTransformOptions {
    match orientation {
        2 => WICBitmapTransformFlipHorizontal,
        3 => WICBitmapTransformRotate180,
        4 => WICBitmapTransformFlipVertical,
        5 => WICBitmapTransformOptions(
            WICBitmapTransformRotate90.0 | WICBitmapTransformFlipHorizontal.0,
        ),
        6 => WICBitmapTransformRotate90,
        7 => WICBitmapTransformOptions(
            WICBitmapTransformRotate270.0 | WICBitmapTransformFlipHorizontal.0,
        ),
        8 => WICBitmapTransformRotate270,
        _ => WICBitmapTransformOptions(0),
    }
}

fn apply_orientation(
    factory: &IWICImagingFactory,
    source: IWICBitmapSource,
    orientation: u16,
) -> IWICBitmapSource {
    let opt = exif_orientation_to_wic_transform(orientation);
    if opt.0 == 0 {
        return source;
    }
    unsafe {
        if let Ok(rotator) = factory.CreateBitmapFlipRotator() {
            if rotator.Initialize(&source, opt).is_ok() {
                if let Ok(as_src) = windows::core::Interface::cast::<IWICBitmapSource>(&rotator) {
                    return as_src;
                }
            }
        }
    }
    source
}

fn convert_source_to_pbgra(
    factory: &IWICImagingFactory,
    source: &IWICBitmapSource,
) -> Result<(u32, u32, Vec<u8>, bool), DecodeError> {
    unsafe {
        let converter = factory
            .CreateFormatConverter()
            .map_err(|e| DecodeError::WicFailure(e.to_string()))?;
        converter
            .Initialize(
                source,
                &GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone,
                None,
                0.0,
                WICBitmapPaletteTypeCustom,
            )
            .map_err(|e| DecodeError::WicFailure(e.to_string()))?;

        let mut w = 0u32;
        let mut h = 0u32;
        converter
            .GetSize(&mut w, &mut h)
            .map_err(|e| DecodeError::WicFailure(e.to_string()))?;
        let total_pixels = validate_image_dimensions(w, h, "WIC")?;

        let stride = w
            .checked_mul(4)
            .ok_or(DecodeError::DimensionsTooLarge {
                width: w,
                height: h,
            })?;
        let total_len = total_pixels
            .checked_mul(4)
            .ok_or(DecodeError::OutOfMemory)?;
        let mut pixels = try_alloc_zeroed_buffer(total_len)?;
        converter
            .CopyPixels(std::ptr::null(), stride, &mut pixels)
            .map_err(|e| DecodeError::WicFailure(e.to_string()))?;

        let has_alpha = pixels.chunks_exact(4).any(|px| px[3] < 255);
        Ok((w, h, pixels, has_alpha))
    }
}

pub fn decode_via_wic(path: &Path, file_size: u64) -> Result<DecodedImage, DecodeError> {
    let factory = get_thread_wic_factory()?;
    let wide_path: Vec<u16> = OsStr::new(path).encode_wide().chain(Some(0)).collect();

    unsafe {
        let decoder = factory
            .CreateDecoderFromFilename(
                PCWSTR(wide_path.as_ptr()),
                None,
                GENERIC_READ,
                WICDecodeMetadataCacheOnDemand,
            )
            .map_err(|e| DecodeError::WicFailure(e.to_string()))?;

        let ext = path
            .extension()
            .and_then(|e| e.to_str())
            .map(|s| s.to_ascii_uppercase())
            .unwrap_or_else(|| "IMAGE".to_string());

        let frame_count = decoder.GetFrameCount().unwrap_or(1).max(1);
        // ICO / CUR 常包含 16~256px 多档分辨率，自动择优挑选最大高清帧
        let best_frame_idx = if frame_count > 1 && (ext == "ICO" || ext == "CUR") {
            let mut best_idx = 0u32;
            let mut best_area = 0u64;
            for i in 0..frame_count {
                if let Ok(f) = decoder.GetFrame(i) {
                    let mut fw = 0u32;
                    let mut fh = 0u32;
                    if f.GetSize(&mut fw, &mut fh).is_ok() {
                        let area = (fw as u64) * (fh as u64);
                        if area >= best_area {
                            best_area = area;
                            best_idx = i;
                        }
                    }
                }
            }
            best_idx
        } else {
            0
        };

        let frame = decoder
            .GetFrame(best_frame_idx)
            .map_err(|e| DecodeError::WicFailure(e.to_string()))?;

        let orientation = read_exif_orientation(&frame);
        let camera_exif = read_camera_exif_fields(&frame);
        let base_source: IWICBitmapSource = windows::core::Interface::cast(&frame)
            .map_err(|e| DecodeError::WicFailure(e.to_string()))?;
        let oriented = apply_orientation(&factory, base_source, orientation);
        let (w, h, bgra_premul, has_alpha) = convert_source_to_pbgra(&factory, &oriented)?;

        let format_kind = ImageFormatKind::Static(ext);
        let mut exif = vec![
            ExifField::Format(format_kind.clone()),
            ExifField::Dimensions(w, h),
            ExifField::Alpha(has_alpha),
        ];
        if frame_count > 1 {
            exif.push(ExifField::FrameCount {
                count: frame_count as usize,
                best_selected: true,
                duration_sec: None,
            });
        }
        if orientation != 1 {
            exif.push(ExifField::OrientationAuto(orientation));
        }
        exif.extend(camera_exif);

        Ok(DecodedImage {
            path: path.to_path_buf(),
            format_kind,
            file_size_bytes: file_size,
            width: w,
            height: h,
            has_alpha,
            frames: vec![ImageFrame {
                width: w,
                height: h,
                delay_ms: 0,
                bgra_premul,
            }],
            total_duration_ms: 0,
            exif_summary: exif,
        })
    }
}

pub fn decode_thumbnail_via_wic(
    path: &Path,
    max_side: u32,
) -> Result<ThumbnailBitmap, DecodeError> {
    let factory = get_thread_wic_factory()?;
    let wide_path: Vec<u16> = OsStr::new(path).encode_wide().chain(Some(0)).collect();

    unsafe {
        let decoder = factory
            .CreateDecoderFromFilename(
                PCWSTR(wide_path.as_ptr()),
                None,
                GENERIC_READ,
                WICDecodeMetadataCacheOnDemand,
            )
            .map_err(|e| DecodeError::WicFailure(e.to_string()))?;
        let frame = decoder
            .GetFrame(0)
            .map_err(|e| DecodeError::WicFailure(e.to_string()))?;
        let orientation = read_exif_orientation(&frame);
        let mut src_w = 0u32;
        let mut src_h = 0u32;
        frame
            .GetSize(&mut src_w, &mut src_h)
            .map_err(|e| DecodeError::WicFailure(e.to_string()))?;
        let _ = validate_image_dimensions(src_w, src_h, "WIC")?;

        let scale = (max_side as f32 / src_w.max(src_h) as f32).min(1.0);
        let dst_w = ((src_w as f32 * scale).round() as u32).max(1);
        let dst_h = ((src_h as f32 * scale).round() as u32).max(1);

        let base_source: IWICBitmapSource = windows::core::Interface::cast(&frame)
            .map_err(|e| DecodeError::WicFailure(e.to_string()))?;
        let scaled: IWICBitmapSource = if dst_w < src_w || dst_h < src_h {
            let scaler = factory
                .CreateBitmapScaler()
                .map_err(|e| DecodeError::WicFailure(e.to_string()))?;
            scaler
                .Initialize(&base_source, dst_w, dst_h, WICBitmapInterpolationModeFant)
                .map_err(|e| DecodeError::WicFailure(e.to_string()))?;
            windows::core::Interface::cast(&scaler)
                .map_err(|e| DecodeError::WicFailure(e.to_string()))?
        } else {
            base_source
        };

        let oriented = apply_orientation(&factory, scaled, orientation);
        let (w, h, bgra_premul, _) = convert_source_to_pbgra(&factory, &oriented)?;
        let (orig_w, orig_h) = if matches!(orientation, 5..=8) {
            (src_h, src_w)
        } else {
            (src_w, src_h)
        };
        Ok(ThumbnailBitmap {
            width: w,
            height: h,
            orig_width: orig_w,
            orig_height: orig_h,
            bgra_premul,
        })
    }
}
