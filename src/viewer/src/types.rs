// ─────────────────────────────────────────────────────────────────────────────
// types.rs — 独立看图器核心领域数据模型与生命周期状态契约（与外部宿主 100% 解耦）
// ─────────────────────────────────────────────────────────────────────────────

use std::path::PathBuf;
use std::sync::Arc;

/// 单帧已解码图像（统一预乘 BGRA8 格式，可直接零拷贝上传 Direct2D ID2D1Bitmap1）
#[derive(Clone)]
pub struct ImageFrame {
    pub width: u32,
    pub height: u32,
    pub delay_ms: u32,
    /// 预乘 Alpha BGRA8 像素（长度 = width * height * 4）
    pub bgra_premul: Vec<u8>,
}

impl ImageFrame {
    pub fn byte_len(&self) -> usize {
        self.bgra_premul.len()
    }
}

/// 结构化图像格式标签（与界面语言解耦，由 i18n 模块渲染 8 语种地道文案）
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ImageFormatKind {
    Static(String),
    AnimatedGif,
    AnimatedWebP,
    AnimatedApng,
    SvgVector,
}

impl ImageFormatKind {
    /// 供日志与命令行自测使用的无语言耦合标识
    pub fn short_tag(&self) -> &str {
        match self {
            Self::Static(s) => s.as_str(),
            Self::AnimatedGif => "GIF (Animated)",
            Self::AnimatedWebP => "WebP (Animated)",
            Self::AnimatedApng => "APNG (Animated)",
            Self::SvgVector => "SVG",
        }
    }
}

/// 结构化 EXIF / 图像详情条目（杜绝解码器内硬编码单语种字符串污染）
#[derive(Clone, Debug, PartialEq)]
pub enum ExifField {
    Format(ImageFormatKind),
    Dimensions(u32, u32),
    Artboard {
        orig_w: u32,
        orig_h: u32,
        out_w: u32,
        out_h: u32,
    },
    Alpha(bool),
    FrameCount {
        count: usize,
        best_selected: bool,
        duration_sec: Option<f64>,
    },
    DurationSec(f64),
    OrientationAuto(u16),
    Camera(String),
    Lens(String),
    DateTaken(String),
    Exposure(String),
    FocalLengthMm(f64),
}

/// 结构化解码/上传错误（支持 8 语种完整本地化展示，杜绝非中文环境泄露汉字）
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DecodeError {
    ReadMetadata(String),
    OpenFile(String),
    ReadFile { format: &'static str, detail: String },
    ZeroDimensions { format: &'static str },
    DimensionsTooLarge { width: u32, height: u32 },
    OutOfMemory,
    InvalidHeader { format: &'static str },
    TruncatedData { format: &'static str },
    UnsupportedFormat { format: &'static str, detail: String },
    NoValidFrames { format: &'static str },
    WicFailure(String),
    SvgParseFailure(String),
    SvgTooLarge,
    DecodeFailed { format: &'static str, detail: String },
    GpuUploadFailed(String),
}

impl std::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::ReadMetadata(d) => write!(f, "Failed to read file metadata: {d}"),
            Self::OpenFile(d) => write!(f, "Failed to open image file: {d}"),
            Self::ReadFile { format, detail } => write!(f, "Failed to read {format}: {detail}"),
            Self::ZeroDimensions { format } => write!(f, "{format} image has zero dimensions"),
            Self::DimensionsTooLarge { width, height } => {
                write!(f, "Image dimensions exceed safety limit ({width} x {height})")
            }
            Self::OutOfMemory => write!(f, "Insufficient memory to decode this image"),
            Self::InvalidHeader { format } => write!(f, "Invalid {format} file header"),
            Self::TruncatedData { format } => write!(f, "Truncated or corrupted {format} pixel data"),
            Self::UnsupportedFormat { format, detail } => {
                if detail.is_empty() {
                    write!(f, "Unsupported {format} sub-format")
                } else {
                    write!(f, "Unsupported {format} sub-format: {detail}")
                }
            }
            Self::NoValidFrames { format } => write!(f, "{format} contains no valid frames"),
            Self::WicFailure(d) => write!(f, "Windows Imaging Codec error: {d}"),
            Self::SvgParseFailure(d) => write!(f, "Failed to parse SVG vector document: {d}"),
            Self::SvgTooLarge => write!(f, "Decompressed SVG size exceeds safety limit"),
            Self::DecodeFailed { format, detail } => write!(f, "Failed to decode {format}: {detail}"),
            Self::GpuUploadFailed(d) => write!(f, "GPU texture upload failed: {d}"),
        }
    }
}

/// 完整已解码图像（静态图含 1 帧，动态 GIF/WebP/APNG 含多帧）
pub struct DecodedImage {
    pub path: PathBuf,
    pub format_kind: ImageFormatKind,
    pub file_size_bytes: u64,
    pub width: u32,
    pub height: u32,
    pub has_alpha: bool,
    pub frames: Vec<ImageFrame>,
    pub total_duration_ms: u64,
    pub exif_summary: Vec<ExifField>,
}

impl DecodedImage {
    pub fn is_animated(&self) -> bool {
        self.frames.len() > 1
    }

    pub fn memory_bytes(&self) -> usize {
        self.frames.iter().map(|f| f.byte_len()).sum()
    }

    /// 判断指定帧是否仍持有 CPU 像素缓冲（静态图上传 GPU 后可释放 CPU 像素实现零双份驻留）
    pub fn has_cpu_pixels(&self, frame_idx: usize) -> bool {
        self.frames
            .get(frame_idx)
            .map(|f| {
                let expected = (f.width as usize)
                    .saturating_mul(f.height as usize)
                    .saturating_mul(4);
                expected > 0 && f.bgra_premul.len() >= expected
            })
            .unwrap_or(false)
    }

    /// 生成仅保留完整图像元信息（尺寸/透明通道/EXIF/格式/文件大小）的轻量对象，释放 CPU 像素内存
    pub fn clone_metadata_without_pixels(&self) -> Self {
        Self {
            path: self.path.clone(),
            format_kind: self.format_kind.clone(),
            file_size_bytes: self.file_size_bytes,
            width: self.width,
            height: self.height,
            has_alpha: self.has_alpha,
            frames: self
                .frames
                .iter()
                .map(|f| ImageFrame {
                    width: f.width,
                    height: f.height,
                    delay_ms: f.delay_ms,
                    bgra_premul: Vec::new(),
                })
                .collect(),
            total_duration_ms: self.total_duration_ms,
            exif_summary: self.exif_summary.clone(),
        }
    }
}

pub type SharedImage = Arc<DecodedImage>;

/// 胶片缩略图（预乘 BGRA8 + 保留原图真实像素宽高，使大图未解码完成时的占位放大与原图 100% 零跳变）
#[derive(Clone)]
pub struct ThumbnailBitmap {
    pub width: u32,
    pub height: u32,
    pub orig_width: u32,
    pub orig_height: u32,
    pub bgra_premul: Vec<u8>,
}

/// 应用四态生命周期状态机：`Opening -> Viewing <-> Slideshow -> Closing`
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LifecyclePhase {
    Opening,
    Viewing,
    Slideshow,
    Closing,
}

/// 窗口形态：经典半透明桌面浮层 vs 标准可缩放独立小窗口（1:1 复刻原版 Picasa 自动切窗体验）
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WindowMode {
    Overlay,
    Windowed,
}

/// 视口适配布局策略：决定默认 Fit 边距、居中基线与自由拖拽边界
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FitLayoutMode {
    /// 浮层看图态：四周留呼吸边距 + 底部工具栏安全区 + 允许未放大时自由拖拽定位
    Overlay,
    /// 标准小窗口态：紧凑满幅居中（贴近客户区边缘）+ 窗口缩放实时重排
    Windowed,
    /// 全屏幻灯片态：100% 纯净满屏居中
    Slideshow,
}

/// 向心坍缩退出 (`Closing`) 初始快照：
/// 无论关闭前照片处于 1000% 放大、边缘平移还是旋转状态，均能从当前视觉姿态精准向屏幕正中央坍缩至 `0.10 * fit_zoom`
#[derive(Clone, Copy, Debug)]
pub struct CloseImplodeSnapshot {
    pub start_zoom: f32,
    pub target_end_zoom: f32,
    pub start_pan_x: f32,
    pub start_pan_y: f32,
    pub start_cinema_dim: f32,
}

/// 进出幻灯片放映整屏双场景叠化快照（1:1 复刻原版 Picasa `1.00s` 熄灯叠化与 `0.30s` 亮灯叠化）：
/// 记录离场场景的几何姿态，使常规带白框原位图与全屏纯黑满屏图实现精确光学线性交叉叠化 `lerp(Viewing, Slideshow, a)`
#[derive(Clone, Copy, Debug)]
pub struct SlideshowPortalSnapshot {
    pub zoom: f32,
    pub pan_x: f32,
    pub pan_y: f32,
    pub rotation_deg: f32,
    pub flip_x: f32,
    pub flip_y: f32,
}

/// 画布背景幕布模式（按 B 循环切换）
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BackdropMode {
    /// Picasa 招牌通透半透明幕布（桌面与文件夹隐约可见）
    PicasaGlass,
    /// 纯黑沉浸暗房
    SolidDark,
    /// 柔白亮色审图底板（适合深色线条透明 PNG/SVG）
    SoftLight,
}

impl BackdropMode {
    pub fn next(self) -> Self {
        match self {
            Self::PicasaGlass => Self::SolidDark,
            Self::SolidDark => Self::SoftLight,
            Self::SoftLight => Self::PicasaGlass,
        }
    }
}

/// 顺时针 90 度步进旋转状态（纯 GPU 矩阵变换 + 复制/保存时按需无损应用）
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum RotationQuarter {
    #[default]
    Deg0 = 0,
    Deg90 = 1,
    Deg180 = 2,
    Deg270 = 3,
}

impl RotationQuarter {
    pub fn rotate_cw(self) -> Self {
        match self {
            Self::Deg0 => Self::Deg90,
            Self::Deg90 => Self::Deg180,
            Self::Deg180 => Self::Deg270,
            Self::Deg270 => Self::Deg0,
        }
    }

    pub fn rotate_ccw(self) -> Self {
        match self {
            Self::Deg0 => Self::Deg270,
            Self::Deg90 => Self::Deg0,
            Self::Deg180 => Self::Deg90,
            Self::Deg270 => Self::Deg180,
        }
    }

    pub fn degrees(self) -> f32 {
        match self {
            Self::Deg0 => 0.0,
            Self::Deg90 => 90.0,
            Self::Deg180 => 180.0,
            Self::Deg270 => 270.0,
        }
    }

    pub fn swaps_axes(self) -> bool {
        matches!(self, Self::Deg90 | Self::Deg270)
    }
}

/// 控制栏、幻灯片迷你播控胶囊与右键菜单动作标识
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ToolbarAction {
    PrevImage,
    NextImage,
    ToggleSlideshow,
    SlideshowTogglePause,
    ZoomIn,
    ZoomOut,
    ToggleActualSize,
    FitToWidth,
    RotateCcw,
    RotateCw,
    FlipHorizontal,
    FlipVertical,
    OpenImageFile,
    AnimTogglePlay,
    AnimPrevFrame,
    AnimNextFrame,
    AnimCycleSpeed,
    SaveCurrentFramePng,
    CopyImage,
    CopyPath,
    SetAsWallpaper,
    ToggleInfo,
    CycleBackdrop,
    ToggleFullscreen,
    RevealInExplorer,
    OpenWithDialog,
    RegisterDefaultApp,
    DeleteToRecycleBin,
    CloseViewer,
}

/// 逻辑像素矩形 (DIP)
#[derive(Clone, Copy, Debug, Default)]
pub struct RectF {
    pub left: f32,
    pub top: f32,
    pub right: f32,
    pub bottom: f32,
}

impl RectF {
    pub const fn new(left: f32, top: f32, right: f32, bottom: f32) -> Self {
        Self {
            left,
            top,
            right,
            bottom,
        }
    }

    pub fn width(self) -> f32 {
        (self.right - self.left).max(0.0)
    }

    pub fn height(self) -> f32 {
        (self.bottom - self.top).max(0.0)
    }

    pub fn center_x(self) -> f32 {
        (self.left + self.right) * 0.5
    }

    pub fn center_y(self) -> f32 {
        (self.top + self.bottom) * 0.5
    }

    pub fn contains(self, x: f32, y: f32) -> bool {
        x >= self.left && x <= self.right && y >= self.top && y <= self.bottom
    }

    pub fn normalized(self) -> Self {
        Self {
            left: self.left.min(self.right),
            top: self.top.min(self.bottom),
            right: self.left.max(self.right),
            bottom: self.top.max(self.bottom),
        }
    }
}

/// RGBA8 直通像素安全转换为 Direct2D 所需的预乘 Alpha BGRA8（使用 try_reserve_exact 防 OOM Abort）
pub fn try_rgba_straight_to_bgra_premul(rgba: &[u8]) -> Result<(Vec<u8>, bool), DecodeError> {
    let pixel_count = rgba.len() / 4;
    let byte_len = pixel_count
        .checked_mul(4)
        .ok_or(DecodeError::OutOfMemory)?;
    let mut out = Vec::new();
    out.try_reserve_exact(byte_len)
        .map_err(|_| DecodeError::OutOfMemory)?;
    out.resize(byte_len, 0);
    let mut has_alpha = false;
    for i in 0..pixel_count {
        let s = i * 4;
        let r = rgba[s] as u32;
        let g = rgba[s + 1] as u32;
        let b = rgba[s + 2] as u32;
        let a = rgba[s + 3] as u32;
        if a < 255 {
            has_alpha = true;
            out[s] = ((b * a + 127) / 255) as u8;
            out[s + 1] = ((g * a + 127) / 255) as u8;
            out[s + 2] = ((r * a + 127) / 255) as u8;
            out[s + 3] = a as u8;
        } else {
            out[s] = b as u8;
            out[s + 1] = g as u8;
            out[s + 2] = r as u8;
            out[s + 3] = 255;
        }
    }
    Ok((out, has_alpha))
}

/// 格式化人类易读字节大小
pub fn format_file_size(bytes: u64) -> String {
    const KB: f64 = 1024.0;
    const MB: f64 = 1024.0 * 1024.0;
    const GB: f64 = 1024.0 * 1024.0 * 1024.0;
    let b = bytes as f64;
    if b >= GB {
        format!("{:.2} GB", b / GB)
    } else if b >= MB {
        format!("{:.2} MB", b / MB)
    } else if b >= KB {
        format!("{:.1} KB", b / KB)
    } else {
        format!("{} B", bytes)
    }
}
