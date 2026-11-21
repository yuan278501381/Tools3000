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

/// 完整已解码图像（静态图含 1 帧，动态 GIF/WebP/APNG 含多帧）
pub struct DecodedImage {
    pub path: PathBuf,
    pub format_label: String,
    pub file_size_bytes: u64,
    pub width: u32,
    pub height: u32,
    pub has_alpha: bool,
    pub frames: Vec<ImageFrame>,
    pub total_duration_ms: u64,
    pub exif_summary: Vec<(String, String)>,
}

impl DecodedImage {
    pub fn is_animated(&self) -> bool {
        self.frames.len() > 1
    }

    pub fn memory_bytes(&self) -> usize {
        self.frames.iter().map(|f| f.byte_len()).sum()
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

/// 向心坍缩退出 (`Closing`) 初始快照：
/// 无论关闭前照片处于 1000% 放大、边缘平移还是旋转状态，均能从当前视觉姿态精准向屏幕正中央坍缩至 `0.28 * fit_zoom`
#[derive(Clone, Copy, Debug)]
pub struct CloseImplodeSnapshot {
    pub start_zoom: f32,
    pub target_end_zoom: f32,
    pub start_pan_x: f32,
    pub start_pan_y: f32,
    pub start_cinema_dim: f32,
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
    ExitSlideshow,
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

/// RGBA8 直通像素原地转换为 Direct2D 所需的预乘 Alpha BGRA8
pub fn rgba_straight_to_bgra_premul(rgba: &[u8]) -> (Vec<u8>, bool) {
    let pixel_count = rgba.len() / 4;
    let mut out = vec![0u8; pixel_count * 4];
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
    (out, has_alpha)
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
