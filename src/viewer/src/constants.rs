// ─────────────────────────────────────────────────────────────────────────────
// constants.rs — 全局不可变配置常量与高分屏 (High-DPI) 黄金视觉令牌（单一事实源）
// ─────────────────────────────────────────────────────────────────────────────

pub const APP_WINDOW_CLASS: &str = "Tools3000_Picasa_Viewer_Wnd";
pub const APP_WINDOW_TITLE: &str = "Tools3000 Picasa Viewer";
pub const APP_PROG_ID: &str = "Tools3000.PicasaViewer";

// 缩放边界与帧率无关物理阻尼参数（支持 60Hz / 120Hz / 144Hz / 165Hz 一致手感）
pub const MIN_ZOOM: f32 = 0.05;
pub const MAX_ZOOM: f32 = 32.0;
pub const ZOOM_STEP_FACTOR: f32 = 1.18;
pub const ZOOM_ANIM_DAMPING: f32 = 20.0;
pub const ROTATION_ANIM_DAMPING: f32 = 18.5;
pub const FLIP_ANIM_DAMPING: f32 = 22.0;
pub const KINETIC_FRICTION_DAMPING: f32 = 7.5;
pub const RUBBER_BAND_SPRING_DAMPING: f32 = 22.0;
pub const RUBBER_BAND_MAX_DIP: f32 = 88.0;

pub const FIT_SCREEN_MARGIN_RATIO: f32 = 0.93;
pub const FIT_BOTTOM_SAFE_PAD_DIP: f32 = 94.0;
pub const FIT_TOP_SAFE_PAD_DIP: f32 = 34.0;
pub const SLIDESHOW_MARGIN_RATIO: f32 = 0.96;

// 6 大灵魂物理动效核心时长（秒 / 毫秒）
pub const OPEN_ANIM_DURATION_SEC: f32 = 0.195;          // 195ms Opening EaseOutQuint 中心弹开 (0.35x -> 1.0x)
pub const CLOSE_ANIM_DURATION_SEC: f32 = 0.175;         // 175ms Closing 向心坍缩至 0.28x 延迟退出
pub const CLOSE_HUD_FADEOUT_RATIO: f32 = 0.44;          // 前 ~75ms 底部 HUD 与顶角浮标率先快速消隐
pub const OPEN_START_SCALE_RATIO: f32 = 0.35;
pub const CLOSE_END_SCALE_RATIO: f32 = 0.28;

pub const THUMB_ITEM_GROW_DURATION_SEC: f32 = 0.24;
pub const THUMB_INITIAL_STAGGER_SEC: f32 = 0.022;
pub const SLIDE_TRANSITION_DURATION_SEC: f32 = 0.14;    // 140ms 微位移交叉淡化切图
pub const SLIDE_FAST_REPEAT_DURATION_SEC: f32 = 0.085;  // 长按方向键高速连切收敛时长
pub const SLIDE_MICRO_OFFSET_DIP: f32 = 38.0;           // 切图优雅微位移（告别夸张全屏横飞）
pub const SLIDE_FAST_OFFSET_DIP: f32 = 18.0;

pub const SLIDESHOW_CROSSFADE_DURATION_SEC: f32 = 0.90; // 900ms 电影级双图层交叉柔和叠化 (Cross-dissolve)
pub const CINEMA_DIM_DAMPING: f32 = 9.5;                // ~350ms 指数阻尼暗房沉降进入 #000000 纯黑放映厅
pub const FILMSTRIP_SCROLL_DAMPING: f32 = 17.5;
pub const HOVER_ANIM_DAMPING: f32 = 18.0;
pub const SLIDESHOW_INTERVAL_MS: u64 = 3500;            // 每张展示 3.5s 配合 Ken Burns 缓慢推拉
pub const LOADING_INDICATOR_DELAY_MS: u64 = 120;

// 按钮按住自动连发节奏
pub const AUTOREPEAT_INITIAL_DELAY_MS: u64 = 320;
pub const AUTOREPEAT_INTERVAL_MS: u64 = 85;

// 缓存与后台预解码预算（严格防爆内存，同时提升 4K 200%+ 高分屏缩略图清晰度）
pub const MAX_DECODE_CACHE_BYTES: usize = 256 * 1024 * 1024;
pub const MAX_DECODE_CACHE_ENTRIES: usize = 11;
pub const MAX_THUMB_CACHE_ENTRIES: usize = 180;
pub const THUMB_TARGET_SIZE: u32 = 192;
pub const PRELOAD_RADIUS: usize = 2;
pub const WORKER_THREAD_COUNT: usize = 2;

// 动图播放控制参数
pub const DEFAULT_FRAME_DELAY_MS: u32 = 100;
pub const MIN_FRAME_DELAY_MS: u32 = 16;
pub const MAX_ANIM_FRAMES: usize = 600;
pub const MAX_ANIM_TOTAL_BYTES: usize = 192 * 1024 * 1024;
pub const ANIM_SPEED_PRESETS: [f32; 5] = [0.25, 0.5, 1.0, 1.5, 2.0];

// SVG 矢量栅格化最大边长上限
pub const SVG_TARGET_LONG_SIDE: u32 = 1800;

// 高分屏黄金 DIP 悬浮底栏（66 DIP 通栏 = 48 DIP 高清胶片缩略图 + 16.5 DIP 镜面倒影，前景按钮精确遮盖缩略图底 1/3）
pub const TOOLBAR_HEIGHT_DIP: f32 = 66.0;
pub const TOOLBAR_BUTTON_W_DIP: f32 = 35.0;
pub const TOOLBAR_ACTUAL_W_DIP: f32 = 40.0;
pub const TOOLBAR_BUTTON_H_DIP: f32 = 25.0;
pub const PLAY_BUTTON_SIZE_DIP: f32 = 44.0;
pub const TOOLBAR_BUTTON_GAP_DIP: f32 = 4.5;

// 幻灯片放映专属极简迷你水晶播控胶囊
pub const SLIDESHOW_MINI_BAR_H_DIP: f32 = 42.0;
pub const SLIDESHOW_MINI_BTN_SIZE_DIP: f32 = 30.0;
pub const SLIDESHOW_MINI_BOTTOM_MARGIN_DIP: f32 = 22.0;

pub const ANIM_BAR_HEIGHT_DIP: f32 = 32.0;
pub const ANIM_SCRUBBER_WIDTH_DIP: f32 = 156.0;

pub const FILMSTRIP_ITEM_SIZE_DIP: f32 = 48.0;
pub const FILMSTRIP_ITEM_GAP_DIP: f32 = 2.0;
pub const FILMSTRIP_DIM_OPACITY: f32 = 0.52;

pub const MINIMAP_MAX_SIDE_DIP: f32 = 168.0;
pub const MINIMAP_MARGIN_DIP: f32 = 18.0;

// 右上角标志性费茨定律关闭徽章（紧贴屏幕最右上角顶点，40 DIP 半径高分屏清晰好点，悬停微扩发光）
pub const CORNER_CLOSE_RADIUS_DIP: f32 = 40.0;

// 2.5 秒静止自动净空模式 (Auto-Hide Idle HUD)
pub const UI_AUTOHIDE_MS: u64 = 2500;
pub const SLIDESHOW_AUTOHIDE_MS: u64 = 2000;
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

// Picasa 半透明暗房遮罩（约 78% 不透明度 `rgba(22, 22, 25, 0.78)`，真实透出底层操作系统桌面！）
pub const COLOR_ACCENT: [f32; 4] = [0.96, 0.44, 0.25, 1.0];
pub const COLOR_BACKDROP_GLASS: [f32; 4] = [0.086, 0.086, 0.098, 0.78];
pub const COLOR_BACKDROP_SOLID_DARK: [f32; 4] = [0.03, 0.032, 0.038, 0.97];
pub const COLOR_BACKDROP_LIGHT: [f32; 4] = [0.94, 0.95, 0.965, 0.94];
pub const COLOR_BAR_BG: [f32; 4] = [0.085, 0.088, 0.098, 0.86];
pub const COLOR_PANEL_BG: [f32; 4] = [0.085, 0.092, 0.11, 0.90];
pub const COLOR_PANEL_TOP_SHEEN: [f32; 4] = [1.0, 1.0, 1.0, 0.18];
pub const COLOR_PANEL_BORDER: [f32; 4] = [1.0, 1.0, 1.0, 0.17];
pub const COLOR_TEXT_PRIMARY: [f32; 4] = [0.96, 0.97, 0.98, 1.0];
pub const COLOR_TEXT_SECONDARY: [f32; 4] = [0.78, 0.80, 0.84, 1.0];
