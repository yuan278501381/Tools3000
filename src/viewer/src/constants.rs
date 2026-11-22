// ─────────────────────────────────────────────────────────────────────────────
// constants.rs — 全局不可变配置常量与高分屏 (High-DPI) 黄金视觉令牌（单一事实源）
// ─────────────────────────────────────────────────────────────────────────────

pub const APP_WINDOW_CLASS: &str = "Tools3000_Picasa_Viewer_Wnd";
pub const ASSOC_WINDOW_CLASS: &str = "Tools3000_Picasa_Assoc_D2D_Dlg";
pub const APP_WINDOW_TITLE: &str = "Tools3000 Picasa Viewer";
pub const APP_PROG_ID: &str = "Tools3000.PicasaViewer";
/// 单实例 WM_COPYDATA 跨进程传图魔数（"T3KV"）
pub const IPC_COPYDATA_OPEN_PATH: usize = 0x5433_4B56;

// 缩放边界与帧率无关物理阻尼参数（支持 60Hz / 120Hz / 144Hz / 165Hz 一致手感，最高 6400% 像素级审图）
// 对照原版 Picasa 录屏实测滚轮长尾衰减曲线 (exp(-k*0.133) ≈ 0.36..0.48) 校准对数无级丝滑阻尼
pub const MIN_ZOOM: f32 = 0.05;
pub const MAX_ZOOM: f32 = 64.0;
pub const ZOOM_STEP_FACTOR: f32 = 1.22;
pub const ZOOM_ANIM_DAMPING: f32 = 7.6;
pub const ROTATION_ANIM_DAMPING: f32 = 14.5;
pub const FLIP_ANIM_DAMPING: f32 = 16.0;
pub const KINETIC_FRICTION_DAMPING: f32 = 7.5;
pub const RUBBER_BAND_SPRING_DAMPING: f32 = 22.0;
pub const RUBBER_BAND_MAX_DIP: f32 = 88.0;

pub const FIT_SCREEN_MARGIN_RATIO: f32 = 0.94;
pub const FIT_BOTTOM_SAFE_PAD_DIP: f32 = 98.0;
pub const FIT_TOP_SAFE_PAD_DIP: f32 = 26.0;
pub const SLIDESHOW_MARGIN_RATIO: f32 = 1.00;

// 照片真实边缘双层高斯投影令牌（原版 Picasa 纯净无白边悬浮落影：宽环境阴影 + 贴边接触阴影）
pub const PHOTO_SHADOW_AMBIENT_SPREAD_DIP: f32 = 30.0;
pub const PHOTO_SHADOW_AMBIENT_OFFSET_Y_DIP: f32 = 6.0;
pub const PHOTO_SHADOW_AMBIENT_OPACITY: f32 = 0.58;
pub const PHOTO_SHADOW_CONTACT_SPREAD_DIP: f32 = 9.0;
pub const PHOTO_SHADOW_CONTACT_OFFSET_Y_DIP: f32 = 1.5;
pub const PHOTO_SHADOW_CONTACT_OPACITY: f32 = 0.42;

// 标准可缩放小窗口 (Windowed) 尺寸约束与紧凑满幅参数
pub const WINDOWED_MIN_CLIENT_W_DIP: f32 = 540.0;
pub const WINDOWED_MIN_CLIENT_H_DIP: f32 = 420.0;
pub const WINDOWED_TRACK_MIN_W_DIP: f32 = 480.0;
pub const WINDOWED_TRACK_MIN_H_DIP: f32 = 360.0;
pub const WINDOWED_MAX_WORKAREA_RATIO: f32 = 0.86;
pub const WINDOWED_FIT_MARGIN_RATIO: f32 = 0.985;

// 原版 Picasa 灵魂动效节奏（硬件 VSync 满血锁定 60~240Hz，对照原版录屏逐帧实测像素曲线精准校准）
pub const BACKDROP_OPEN_DURATION_SEC: f32 = 0.14;       // 140ms 全屏轻盈雾灰遮罩与底栏底座利落就位
pub const OPEN_ANIM_DURATION_SEC: f32 = 0.42;           // 420ms Opening 实体相纸从中心 0.15x 舒展弹开至 1.0x
pub const CLOSE_ANIM_DURATION_SEC: f32 = 0.34;          // 340ms Closing 两段式退场：前 238ms 实体照片清晰向心收拢，后 115ms 灰幕散去
pub const CLOSE_HUD_FADEOUT_RATIO: f32 = 0.16;          // 前 ~55ms 底部 HUD 与右上角关闭钮利落收隐
pub const CLOSE_IMAGE_IMPLODE_RATIO: f32 = 0.70;        // 前 70% (~238ms) 完整呈现照片向中心吸入过程
pub const CLOSE_BACKDROP_START_RATIO: f32 = 0.66;       // 66% 起照片收尾消隐，雾灰遮罩随之柔和散开
pub const OPEN_START_SCALE_RATIO: f32 = 0.15;
pub const CLOSE_END_SCALE_RATIO: f32 = 0.10;

// 下方胶片缩略图接力涟漪绽放：待主图即将落定时，从正中央当前图向左右两翼次第弹大盛开
pub const THUMB_WAVE_START_DELAY_SEC: f32 = 0.24;       // 主图弹开至 240ms 接力引爆底部缩略图涟漪
pub const THUMB_ITEM_GROW_DURATION_SEC: f32 = 0.26;     // 单张缩略图 0.06x -> 1.0x 弹性绽放时长
pub const THUMB_INITIAL_STAGGER_SEC: f32 = 0.044;       // 每向外一格 44ms 清晰涟漪步进
pub const THUMB_MAX_STAGGER_SEC: f32 = 0.56;            // 覆盖整排可见胶片的最长起跑延迟
pub const SLIDE_TRANSITION_DURATION_SEC: f32 = 0.165;   // 165ms 常规翻图丝滑微位移交叉淡化
pub const SLIDE_FAST_REPEAT_DURATION_SEC: f32 = 0.075;  // 长按方向键高速连切零拖影收敛时长
pub const SLIDE_MICRO_OFFSET_DIP: f32 = 26.0;           // 26 DIP 柔和方向推进感，肉眼清脆灵动
pub const SLIDE_FAST_OFFSET_DIP: f32 = 14.0;

pub const SLIDESHOW_ENTER_DIM_DURATION_SEC: f32 = 1.00; // 1.00s 原版实测影院熄灯叠化 + 照片无白框平滑扩图至满屏
pub const SLIDESHOW_EXIT_DIM_DURATION_SEC: f32 = 0.30;  // 0.30s 原版实测利落亮灯退回工作区看图态
pub const SLIDESHOW_CROSSFADE_DURATION_SEC: f32 = 1.05; // 1.05s 幻灯片光学能量守恒双图层柔和交叉叠化 (Additive Cross-dissolve)
pub const SLIDESHOW_ZOOM_ANIM_DAMPING: f32 = 4.6;       // 进出幻灯片平滑扩图/回缩专用柔和阻尼（与 1.0s 熄灯同频）
pub const FILMSTRIP_SCROLL_DAMPING: f32 = 16.0;
pub const HOVER_ANIM_DAMPING: f32 = 16.5;
pub const SLIDESHOW_INTERVAL_MS: u64 = 3200;            // 幻灯片每张沉稳清晰展示 3.2s 后开始 1.05s 光学叠化
pub const LOADING_INDICATOR_DELAY_MS: u64 = 120;
pub const ZOOM_BADGE_HOLD_MS: u64 = 850;                // 正中央缩放倍率胶囊停留时长
pub const ZOOM_BADGE_FADE_MS: u64 = 220;                // 正中央缩放倍率胶囊平滑淡出时长

// 按钮按住自动连发节奏
pub const AUTOREPEAT_INITIAL_DELAY_MS: u64 = 320;
pub const AUTOREPEAT_INTERVAL_MS: u64 = 85;

// 紧凑缓存与后台预解码预算（对标原版 Picasa 极致轻量内存：36MB 全图缓存 + 单图预解上限 18MB + 128px 锐利缩略图）
pub const MAX_DECODE_CACHE_BYTES: usize = 36 * 1024 * 1024;
pub const MAX_DECODE_CACHE_ENTRIES: usize = 3;
pub const MAX_PRELOAD_SINGLE_ENTRY_BYTES: usize = 18 * 1024 * 1024;
pub const MAX_THUMB_CACHE_ENTRIES: usize = 48;
pub const THUMB_TARGET_SIZE: u32 = 128;
pub const PRELOAD_RADIUS: usize = 1;
pub const WORKER_THREAD_COUNT: usize = 2;

// 解码安全防护上限（约 1.34 亿像素 / 512MB 单帧，严防畸形文件头触发 panic="abort" OOM）
pub const MAX_DECODE_PIXELS: u64 = 134_217_728;
pub const MAX_DECODE_DIMENSION: u32 = 131_072;
pub const MAX_SVG_DECOMPRESSED_BYTES: usize = 32 * 1024 * 1024;

// 动图播放控制参数
pub const DEFAULT_FRAME_DELAY_MS: u32 = 100;
pub const MIN_FRAME_DELAY_MS: u32 = 16;
pub const MAX_ANIM_FRAMES: usize = 600;
pub const MAX_ANIM_TOTAL_BYTES: usize = 192 * 1024 * 1024;
pub const ANIM_SPEED_PRESETS: [f32; 5] = [0.25, 0.5, 1.0, 1.5, 2.0];

// SVG 矢量栅格化最大边长上限
pub const SVG_TARGET_LONG_SIDE: u32 = 1800;

// 高分屏黄金 DIP 悬浮底栏（68 DIP 通栏 = 50 DIP 高清胶片缩略图 + 16.5 DIP 镜面倒影，前景按钮精确遮盖缩略图底 1/3）
pub const TOOLBAR_HEIGHT_DIP: f32 = 68.0;
pub const TOOLBAR_BUTTON_W_DIP: f32 = 36.0;
pub const TOOLBAR_ACTUAL_W_DIP: f32 = 42.0;
pub const TOOLBAR_BUTTON_H_DIP: f32 = 26.0;
pub const PLAY_BUTTON_SIZE_DIP: f32 = 44.0;
pub const TOOLBAR_BUTTON_GAP_DIP: f32 = 4.5;

pub const ANIM_BAR_HEIGHT_DIP: f32 = 32.0;
pub const ANIM_SCRUBBER_WIDTH_DIP: f32 = 156.0;

pub const FILMSTRIP_ITEM_SIZE_DIP: f32 = 50.0;
pub const FILMSTRIP_ITEM_GAP_DIP: f32 = 2.0;
pub const FILMSTRIP_DIM_OPACITY: f32 = 0.56;

pub const MINIMAP_MAX_SIDE_DIP: f32 = 168.0;
pub const MINIMAP_MARGIN_DIP: f32 = 18.0;

// 右上角标志性费茨定律关闭徽章（紧贴屏幕最右上角顶点，40 DIP 半径高分屏清晰好点，悬停微扩发光）
pub const CORNER_CLOSE_RADIUS_DIP: f32 = 40.0;

pub const TOAST_DURATION_MS: u64 = 1600;

// 支持的图片扩展名清单
pub const SUPPORTED_EXTENSIONS: &[&str] = &[
    "jpg", "jpeg", "jpe", "jfif", "pjpeg",
    "png", "apng",
    "webp",
    "gif",
    "bmp", "dib", "rle",
    "ico", "cur",
    "tif", "tiff",
    "svg", "svgz",
    "avif", "heic", "heif", "hif",
    "wdp", "hdp", "jxr",
    "tga", "qoi", "ppm", "pgm", "pbm", "dds",
    "dng", "cr2", "cr3", "nef", "arw", "orf", "rw2", "pef", "raf", "srw",
];

// 现代无衬线与等宽字体栈（遵循全局字体排版规范，字号 >= 12px，字重 >= 500）
pub const FONT_UI_FAMILY: &str = "Segoe UI Variable Text";
pub const FONT_MONO_FAMILY: &str = "Cascadia Code";

// Picasa 招牌轻盈雾灰半透明遮罩（约 58% 不透明度 `rgba(24, 25, 27, 0.58)`，真实柔和透出底层桌面与文件夹！）
pub const COLOR_ACCENT: [f32; 4] = [0.96, 0.44, 0.25, 1.0];
pub const COLOR_BACKDROP_GLASS: [f32; 4] = [0.095, 0.098, 0.106, 0.58];
pub const COLOR_BACKDROP_SOLID_DARK: [f32; 4] = [0.03, 0.032, 0.038, 0.97];
pub const COLOR_BACKDROP_LIGHT: [f32; 4] = [0.94, 0.95, 0.965, 0.94];
pub const COLOR_WINDOWED_BG_DARK: [f32; 4] = [0.11, 0.115, 0.125, 1.0];
pub const COLOR_WINDOWED_BG_SOLID_DARK: [f32; 4] = [0.03, 0.032, 0.038, 1.0];
pub const COLOR_WINDOWED_BG_LIGHT: [f32; 4] = [0.945, 0.953, 0.965, 1.0];
pub const COLOR_BAR_BG: [f32; 4] = [0.088, 0.092, 0.100, 0.88];
pub const COLOR_PANEL_BG: [f32; 4] = [0.085, 0.092, 0.11, 0.90];
pub const COLOR_PANEL_TOP_SHEEN: [f32; 4] = [1.0, 1.0, 1.0, 0.18];
pub const COLOR_PANEL_BORDER: [f32; 4] = [1.0, 1.0, 1.0, 0.17];
pub const COLOR_TEXT_PRIMARY: [f32; 4] = [0.96, 0.97, 0.98, 1.0];
pub const COLOR_TEXT_SECONDARY: [f32; 4] = [0.78, 0.80, 0.84, 1.0];
