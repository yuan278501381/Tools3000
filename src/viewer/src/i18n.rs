// ─────────────────────────────────────────────────────────────────────────────
// i18n.rs — 独立看图器 8 语种国际化单一事实源 (100% 单文件内嵌，自动跟随系统语言)
//
// 对齐 Tools3000 全语种矩阵：
//   1. zh-CN (简体中文)      2. zh-TW (繁體中文)      3. en-US (English / 全局兜底)
//   4. ja-JP (日本語)        5. ko-KR (한국어)        6. ru-RU (Русский)
//   7. pt-BR (Português BR)  8. es-ES (Español)
//
// 独立性保障：
// - 词典 100% 编译期静态内嵌于单文件 EXE，零外部词典文件与零宿主配置耦合；
// - 原生调用 Win32 `GetUserDefaultUILanguage()` 自动跟随 Windows 系统显示语言。
// ─────────────────────────────────────────────────────────────────────────────

use crate::types::BackdropMode;

#[link(name = "kernel32")]
extern "system" {
    fn GetUserDefaultUILanguage() -> u16;
}

/// 与 Tools3000 主程序 1:1 对齐的 8 大支持语种（自动跟随 Windows 系统显示语言）
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ViewerLanguage {
    ZhCN,
    ZhTW,
    EnUS,
    JaJP,
    KoKR,
    RuRU,
    PtBR,
    EsES,
}

impl ViewerLanguage {
    /// 规范 BCP-47 标签（供 DirectWrite 本地化字形整形使用）
    pub const fn tag(self) -> &'static str {
        match self {
            Self::ZhCN => "zh-CN",
            Self::ZhTW => "zh-TW",
            Self::EnUS => "en-US",
            Self::JaJP => "ja-JP",
            Self::KoKR => "ko-KR",
            Self::RuRU => "ru-RU",
            Self::PtBR => "pt-BR",
            Self::EsES => "es-ES",
        }
    }

    /// 针对中日韩繁西里尔字形优化的首选无衬线字体回退名（杜绝 CJK 异体字错乱）
    pub const fn preferred_cjk_fallback_font(self) -> &'static str {
        match self {
            Self::ZhCN => "Microsoft YaHei UI",
            Self::ZhTW => "Microsoft JhengHei UI",
            Self::JaJP => "Yu Gothic UI",
            Self::KoKR => "Malgun Gothic",
            Self::EnUS | Self::RuRU | Self::PtBR | Self::EsES => "Segoe UI",
        }
    }

    /// 解析任意 BCP-47 语言标签或简写（未知语种安全回退 EnUS）
    pub fn from_tag(raw: &str) -> Self {
        let lower: String = raw
            .trim()
            .chars()
            .map(|c| if c == '_' { '-' } else { c.to_ascii_lowercase() })
            .collect();
        if lower.starts_with("zh") {
            if lower.contains("tw")
                || lower.contains("hk")
                || lower.contains("mo")
                || lower.contains("hant")
            {
                return Self::ZhTW;
            }
            return Self::ZhCN;
        }
        if lower.starts_with("ja") {
            return Self::JaJP;
        }
        if lower.starts_with("ko") {
            return Self::KoKR;
        }
        if lower.starts_with("ru") {
            return Self::RuRU;
        }
        if lower.starts_with("pt") {
            return Self::PtBR;
        }
        if lower.starts_with("es") {
            return Self::EsES;
        }
        Self::EnUS
    }

    /// 通过 Win32 `GetUserDefaultUILanguage()` 独立探测并自动跟随当前操作系统显示语言
    pub fn detect_system() -> Self {
        let lang_id = unsafe { GetUserDefaultUILanguage() };
        Self::from_windows_langid(lang_id)
    }

    pub fn from_windows_langid(lang_id: u16) -> Self {
        let primary = lang_id & 0x03FF;
        let sublang = (lang_id >> 10) & 0x003F;
        match primary {
            0x04 => {
                // LANG_CHINESE: 简中 (0x02 大陆 / 0x04 新加坡) -> ZhCN，其余繁中 (台湾/香港/澳门) -> ZhTW
                if sublang == 0x02 || sublang == 0x04 {
                    Self::ZhCN
                } else {
                    Self::ZhTW
                }
            }
            0x11 => Self::JaJP, // LANG_JAPANESE
            0x12 => Self::KoKR, // LANG_KOREAN
            0x19 => Self::RuRU, // LANG_RUSSIAN
            0x16 => Self::PtBR, // LANG_PORTUGUESE
            0x0A => Self::EsES, // LANG_SPANISH
            _ => Self::EnUS,
        }
    }

    pub const fn strings(self) -> &'static ViewerStrings {
        match self {
            Self::ZhCN => &STRINGS_ZH_CN,
            Self::ZhTW => &STRINGS_ZH_TW,
            Self::EnUS => &STRINGS_EN_US,
            Self::JaJP => &STRINGS_JA_JP,
            Self::KoKR => &STRINGS_KO_KR,
            Self::RuRU => &STRINGS_RU_RU,
            Self::PtBR => &STRINGS_PT_BR,
            Self::EsES => &STRINGS_ES_ES,
        }
    }
}

/// 强类型全界面词典（Rust 编译期保证 8 语种 100% 字段对齐，0 悬空键）
pub struct ViewerStrings {
    // 启动与系统文件关联弹窗
    pub open_dialog_title: &'static str,
    pub open_filter_images: &'static str,
    pub open_filter_all: &'static str,
    pub no_viewable_images: &'static str,
    pub reg_ok_body: &'static str,
    pub reg_fail_title: &'static str,
    pub unreg_ok_body: &'static str,
    pub unreg_fail_title: &'static str,

    // 底栏共享状态文字提示 (ss/statustext)
    pub tip_zoom_in: &'static str,
    pub tip_zoom_out: &'static str,
    pub tip_actual_size: &'static str,
    pub tip_fit_window: &'static str,
    pub tip_play_slideshow: &'static str,
    pub tip_pause_slideshow: &'static str,
    pub tip_resume_slideshow: &'static str,
    pub tip_exit_slideshow: &'static str,
    pub tip_prev_image: &'static str,
    pub tip_next_image: &'static str,
    pub tip_rotate_ccw: &'static str,
    pub tip_rotate_cw: &'static str,
    pub tip_flip_h: &'static str,
    pub tip_flip_v: &'static str,
    pub tip_close: &'static str,
    pub tip_anim_play: &'static str,
    pub tip_anim_pause: &'static str,
    pub tip_anim_prev_frame: &'static str,
    pub tip_anim_next_frame: &'static str,
    pub tip_anim_speed: &'static str,
    pub tip_save_png: &'static str,
    pub tip_scrub_frames: &'static str,

    // 居中状态与加载卡片
    pub status_cannot_open: &'static str,
    pub status_loading_title: &'static str,
    pub status_loading_default: &'static str,

    // 左上角详情与 EXIF 面板
    pub info_fallback_title: &'static str,
    pub exif_file_size: &'static str,
    pub exif_format: &'static str,
    pub exif_dimensions: &'static str,
    pub exif_artboard: &'static str,
    pub exif_alpha: &'static str,
    pub exif_alpha_yes: &'static str,
    pub exif_alpha_no: &'static str,
    pub exif_frames: &'static str,
    pub exif_duration: &'static str,
    pub exif_orientation: &'static str,
    pub exif_camera: &'static str,
    pub exif_lens: &'static str,
    pub exif_date_taken: &'static str,
    pub exif_exposure: &'static str,
    pub exif_focal_length: &'static str,
    pub exif_display_scale: &'static str,
    pub exif_1to1_suffix: &'static str,

    // 另存为 PNG 对话框
    pub save_png_filter: &'static str,
    pub save_png_title: &'static str,

    // 右键菜单
    pub menu_actual_or_fit: &'static str,
    pub menu_fit_width: &'static str,
    pub menu_play_slideshow: &'static str,
    pub menu_stop_slideshow: &'static str,
    pub menu_fullscreen_overlay: &'static str,
    pub menu_rotate_cw: &'static str,
    pub menu_rotate_ccw: &'static str,
    pub menu_flip_h: &'static str,
    pub menu_flip_v: &'static str,
    pub menu_open_file: &'static str,
    pub menu_copy_image: &'static str,
    pub menu_copy_path: &'static str,
    pub menu_save_frame_png: &'static str,
    pub menu_save_image_png: &'static str,
    pub menu_set_wallpaper: &'static str,
    pub menu_cycle_backdrop: &'static str,
    pub menu_image_info: &'static str,
    pub menu_reveal_explorer: &'static str,
    pub menu_open_with: &'static str,
    pub menu_register_assoc: &'static str,
    pub menu_delete_recycle: &'static str,
    pub menu_close: &'static str,

    // 文件关联与默认看图器配置窗口 (8 语种全覆盖)
    pub assoc_dlg_title: &'static str,
    pub assoc_dlg_subtitle: &'static str,
    pub assoc_icon_caption: &'static str,
    pub assoc_btn_select_all: &'static str,
    pub assoc_btn_common: &'static str,
    pub assoc_btn_apply_default: &'static str,
    pub assoc_btn_apply_only: &'static str,
    pub assoc_btn_remove_all: &'static str,
    pub assoc_btn_close: &'static str,
    pub assoc_unreg_done: &'static str,

    // 轻量瞬态反馈 Toast 与幕布名
    pub toast_single_image_no_slideshow: &'static str,
    pub toast_anim_resumed: &'static str,
    pub toast_anim_paused: &'static str,
    pub toast_anim_speed_prefix: &'static str,
    pub toast_saved_png_prefix: &'static str,
    pub toast_save_png_failed_prefix: &'static str,
    pub toast_copied_image: &'static str,
    pub toast_copy_failed: &'static str,
    pub toast_copied_path: &'static str,
    pub toast_wallpaper_ok: &'static str,
    pub toast_wallpaper_fail: &'static str,
    pub toast_backdrop_prefix: &'static str,
    pub backdrop_glass: &'static str,
    pub backdrop_dark: &'static str,
    pub backdrop_light: &'static str,
    pub toast_fullscreen_on: &'static str,
    pub toast_workarea_on: &'static str,
    pub toast_registered_ok: &'static str,
    pub toast_register_failed_prefix: &'static str,
    pub toast_moved_to_recycle: &'static str,
    pub toast_recycle_cancelled: &'static str,
}

impl ViewerStrings {
    pub const fn backdrop_label(&self, mode: BackdropMode) -> &'static str {
        match mode {
            BackdropMode::PicasaGlass => self.backdrop_glass,
            BackdropMode::SolidDark => self.backdrop_dark,
            BackdropMode::SoftLight => self.backdrop_light,
        }
    }

    /// 将底层解码器产出的 EXIF 键值对动态映射为当前激活语种展示文案
    pub fn localize_exif_pair(&self, raw_key: &str, raw_val: &str) -> (String, String) {
        let key = match raw_key {
            "格式" => self.exif_format,
            "尺寸" => self.exif_dimensions,
            "画板尺寸" => self.exif_artboard,
            "透明通道" => self.exif_alpha,
            "帧数" | "图像帧数" | "动画帧数" => self.exif_frames,
            "总时长" => self.exif_duration,
            "EXIF 方向" => self.exif_orientation,
            "相机型号" => self.exif_camera,
            "镜头" => self.exif_lens,
            "拍摄时间" => self.exif_date_taken,
            "曝光参数" => self.exif_exposure,
            "焦距" => self.exif_focal_length,
            _ => raw_key,
        };

        let val = match raw_val {
            "含 Alpha" => self.exif_alpha_yes.to_string(),
            "不透明" => self.exif_alpha_no.to_string(),
            "GIF 动图" => "Animated GIF".to_string(),
            "APNG 动图" => "Animated PNG (APNG)".to_string(),
            "SVG 矢量图" => "SVG Vector".to_string(),
            other => {
                if let Some(num) = other.strip_suffix(" 帧") {
                    format!("{num}")
                } else if let Some(rest) = other.strip_prefix("已自动纠偏 (") {
                    format!("Auto ({}", rest)
                } else {
                    other.replace(" 帧（已选最优清晰帧）", "").replace(" 帧（", " (").replace('）', ")")
                }
            }
        };
        (key.to_string(), val)
    }
}

// ─── 1. 简体中文 (zh-CN) ───────────────────────────────────────────────────────
pub const STRINGS_ZH_CN: ViewerStrings = ViewerStrings {
    open_dialog_title: "选择要浏览的图片 - Tools3000 Picasa Viewer",
    open_filter_images: "所有支持的图片 (JPG/PNG/WebP/GIF/BMP/ICO/TIFF/SVG/TGA/QOI/AVIF/HEIC/RAW)",
    open_filter_all: "所有文件",
    no_viewable_images: "目标路径不存在可浏览的图像文件",
    reg_ok_body: "已成功注册为当前用户的图片打开方式。\n可在右键菜单「打开方式」或 Windows「默认应用」中设为默认看图器。",
    reg_fail_title: "注册失败",
    unreg_ok_body: "已移除当前用户的图片文件关联注册。",
    unreg_fail_title: "卸载关联失败",

    tip_zoom_in: "放大 (+)",
    tip_zoom_out: "缩小 (-)",
    tip_actual_size: "实际大小 1:1 (1)",
    tip_fit_window: "适应窗口大小 (0)",
    tip_play_slideshow: "播放幻灯片 (F5)",
    tip_pause_slideshow: "暂停放映 (Space)",
    tip_resume_slideshow: "继续放映 (Space)",
    tip_exit_slideshow: "退出幻灯片放映 (Esc)",
    tip_prev_image: "上一张 (←)",
    tip_next_image: "下一张 (→)",
    tip_rotate_ccw: "逆时针旋转 (Ctrl+Shift+R)",
    tip_rotate_cw: "顺时针旋转 (Ctrl+R)",
    tip_flip_h: "水平翻转 (H)",
    tip_flip_v: "垂直翻转 (V)",
    tip_close: "关闭 (Esc)",
    tip_anim_play: "播放动图 (Space)",
    tip_anim_pause: "暂停动图 (Space)",
    tip_anim_prev_frame: "上一帧 (,)",
    tip_anim_next_frame: "下一帧 (.)",
    tip_anim_speed: "切换动图倍速 (S)",
    tip_save_png: "导出当前帧 PNG (Ctrl+S)",
    tip_scrub_frames: "拖动浏览任意动图帧",

    status_cannot_open: "无法打开图像",
    status_loading_title: "正在载入",
    status_loading_default: "正在载入图像...",

    info_fallback_title: "图像详情",
    exif_file_size: "文件体积",
    exif_format: "图像格式",
    exif_dimensions: "原始尺寸",
    exif_artboard: "矢量画板",
    exif_alpha: "透明通道",
    exif_alpha_yes: "含 Alpha",
    exif_alpha_no: "不透明",
    exif_frames: "图像帧数",
    exif_duration: "动画时长",
    exif_orientation: "EXIF 方向",
    exif_camera: "相机型号",
    exif_lens: "镜头型号",
    exif_date_taken: "拍摄时间",
    exif_exposure: "曝光参数",
    exif_focal_length: "焦距",
    exif_display_scale: "屏幕缩放",
    exif_1to1_suffix: "物理 1:1",

    save_png_filter: "PNG 无损图像 (*.png)",
    save_png_title: "另存当前画面为 PNG",

    menu_actual_or_fit: "实际大小 1:1 / 适应窗口\t1",
    menu_fit_width: "适应宽度阅读长图\tW",
    menu_play_slideshow: "播放幻灯片\tF5",
    menu_stop_slideshow: "停止幻灯片放映\tF5",
    menu_fullscreen_overlay: "满屏通透悬浮模式\tF11",
    menu_rotate_cw: "顺时针旋转 90°\tCtrl+R",
    menu_rotate_ccw: "逆时针旋转 90°\tCtrl+Shift+R",
    menu_flip_h: "水平翻转\tH",
    menu_flip_v: "垂直翻转\tV",
    menu_open_file: "打开本地图片...\tO",
    menu_copy_image: "复制图片\tCtrl+C",
    menu_copy_path: "复制文件路径\tCtrl+Shift+C",
    menu_save_frame_png: "导出当前动图帧为 PNG...\tCtrl+S",
    menu_save_image_png: "另存当前画面为 PNG...\tCtrl+S",
    menu_set_wallpaper: "设为桌面壁纸",
    menu_cycle_backdrop: "切换画布幕布背景\tB",
    menu_image_info: "照片详情与 EXIF\tI",
    menu_reveal_explorer: "在资源管理器中定位\tF",
    menu_open_with: "打开方式...",
    menu_register_assoc: "设为默认看图器 / 文件关联...\tCtrl+M",
    menu_delete_recycle: "移入回收站\tDel",
    menu_close: "关闭\tEsc",

    assoc_dlg_title: "设为默认图片查看器与文件关联 - Tools3000 Picasa Viewer",
    assoc_dlg_subtitle: "勾选希望用本软件打开的图片格式。关联生效后，文件夹中的图片文件图标将变为经典的 Picasa 拍立得相纸图标：",
    assoc_icon_caption: "关联后图片文件图标",
    assoc_btn_select_all: "全选",
    assoc_btn_common: "常用格式",
    assoc_btn_apply_default: "应用关联并打开系统默认设置",
    assoc_btn_apply_only: "仅写入关联与图标",
    assoc_btn_remove_all: "清除关联",
    assoc_btn_close: "关闭",
    assoc_unreg_done: "已清除本软件的图片文件关联",

    toast_single_image_no_slideshow: "当前目录仅 1 张图片，无法播放幻灯片",
    toast_anim_resumed: "动图继续播放",
    toast_anim_paused: "动图已暂停",
    toast_anim_speed_prefix: "动图倍速: ",
    toast_saved_png_prefix: "已导出 PNG: ",
    toast_save_png_failed_prefix: "导出 PNG 失败: ",
    toast_copied_image: "已复制图像至剪贴板",
    toast_copy_failed: "写入剪贴板失败",
    toast_copied_path: "已复制文件完整路径",
    toast_wallpaper_ok: "已设为桌面壁纸",
    toast_wallpaper_fail: "设置桌面壁纸失败",
    toast_backdrop_prefix: "幕布背景: ",
    backdrop_glass: "Picasa 透明浮层",
    backdrop_dark: "深邃沉浸暗房",
    backdrop_light: "明亮画廊审图",
    toast_fullscreen_on: "满屏通透悬浮模式 (F11)",
    toast_workarea_on: "保留任务栏工作区模式 (F11)",
    toast_registered_ok: "已注册至系统「打开方式」列表",
    toast_register_failed_prefix: "注册失败: ",
    toast_moved_to_recycle: "已安全移入回收站",
    toast_recycle_cancelled: "移入回收站已取消",
};

// ─── 2. 繁體中文 (zh-TW) ───────────────────────────────────────────────────────
pub const STRINGS_ZH_TW: ViewerStrings = ViewerStrings {
    open_dialog_title: "選擇要瀏覽的圖片 - Tools3000 Picasa Viewer",
    open_filter_images: "所有支援的圖片 (JPG/PNG/WebP/GIF/BMP/ICO/TIFF/SVG/TGA/QOI/AVIF/HEIC/RAW)",
    open_filter_all: "所有檔案",
    no_viewable_images: "目標路徑不存在可瀏覽的影像檔案",
    reg_ok_body: "已成功註冊為目前使用者的圖片開啟方式。\n可在右鍵選單「開啟檔案」或 Windows「預設應用程式」中設為預設看圖程式。",
    reg_fail_title: "註冊失敗",
    unreg_ok_body: "已移除目前使用者的圖片檔案關聯註冊。",
    unreg_fail_title: "解除關聯失敗",

    tip_zoom_in: "放大 (+)",
    tip_zoom_out: "縮小 (-)",
    tip_actual_size: "實際大小 1:1 (1)",
    tip_fit_window: "符合視窗大小 (0)",
    tip_play_slideshow: "播放投影片 (F5)",
    tip_pause_slideshow: "暫停放映 (Space)",
    tip_resume_slideshow: "繼續放映 (Space)",
    tip_exit_slideshow: "離開投影片放映 (Esc)",
    tip_prev_image: "上一張 (←)",
    tip_next_image: "下一張 (→)",
    tip_rotate_ccw: "逆時針旋轉 (Ctrl+Shift+R)",
    tip_rotate_cw: "順時針旋轉 (Ctrl+R)",
    tip_flip_h: "水平翻轉 (H)",
    tip_flip_v: "垂直翻轉 (V)",
    tip_close: "關閉 (Esc)",
    tip_anim_play: "播放動圖 (Space)",
    tip_anim_pause: "暫停動圖 (Space)",
    tip_anim_prev_frame: "上一影格 (,)",
    tip_anim_next_frame: "下一影格 (.)",
    tip_anim_speed: "切換動圖倍速 (S)",
    tip_save_png: "匯出目前影格 PNG (Ctrl+S)",
    tip_scrub_frames: "拖曳瀏覽任意動圖影格",

    status_cannot_open: "無法開啟影像",
    status_loading_title: "正在載入",
    status_loading_default: "正在載入影像...",

    info_fallback_title: "影像詳細資訊",
    exif_file_size: "檔案大小",
    exif_format: "影像格式",
    exif_dimensions: "原始尺寸",
    exif_artboard: "向量畫板",
    exif_alpha: "透明色版",
    exif_alpha_yes: "含 Alpha",
    exif_alpha_no: "不透明",
    exif_frames: "影像影格數",
    exif_duration: "動畫長度",
    exif_orientation: "EXIF 方向",
    exif_camera: "相機型號",
    exif_lens: "鏡頭型號",
    exif_date_taken: "拍攝時間",
    exif_exposure: "曝光參數",
    exif_focal_length: "焦距",
    exif_display_scale: "螢幕縮放",
    exif_1to1_suffix: "實體 1:1",

    save_png_filter: "PNG 無損影像 (*.png)",
    save_png_title: "另存目前畫面為 PNG",

    menu_actual_or_fit: "實際大小 1:1 / 符合視窗\t1",
    menu_fit_width: "符合寬度閱讀長圖\tW",
    menu_play_slideshow: "播放投影片\tF5",
    menu_stop_slideshow: "停止投影片放映\tF5",
    menu_fullscreen_overlay: "全螢幕通透懸浮模式\tF11",
    menu_rotate_cw: "順時針旋轉 90°\tCtrl+R",
    menu_rotate_ccw: "逆時針旋轉 90°\tCtrl+Shift+R",
    menu_flip_h: "水平翻轉\tH",
    menu_flip_v: "垂直翻轉\tV",
    menu_open_file: "開啟本機圖片...\tO",
    menu_copy_image: "複製圖片\tCtrl+C",
    menu_copy_path: "複製檔案路徑\tCtrl+Shift+C",
    menu_save_frame_png: "匯出目前動圖影格為 PNG...\tCtrl+S",
    menu_save_image_png: "另存目前畫面為 PNG...\tCtrl+S",
    menu_set_wallpaper: "設為桌面背景",
    menu_cycle_backdrop: "切換畫布背景模式\tB",
    menu_image_info: "相片資訊與 EXIF\tI",
    menu_reveal_explorer: "在檔案總管中顯示\tF",
    menu_open_with: "開啟檔案...",
    menu_register_assoc: "設為預設看圖程式 / 檔案關聯...\tCtrl+M",
    menu_delete_recycle: "移至資源回收筒\tDel",
    menu_close: "關閉\tEsc",

    assoc_dlg_title: "設為預設圖片檢視器與檔案關聯 - Tools3000 Picasa Viewer",
    assoc_dlg_subtitle: "勾選希望由本軟體開啟的圖片格式。關聯生效後，資料夾中的圖片檔案圖示將變為經典的 Picasa 拍立得相紙圖示：",
    assoc_icon_caption: "關聯後圖片檔案圖示",
    assoc_btn_select_all: "全選",
    assoc_btn_common: "常用格式",
    assoc_btn_apply_default: "套用關聯並開啟系統預設設定",
    assoc_btn_apply_only: "僅寫入關聯與圖示",
    assoc_btn_remove_all: "清除關聯",
    assoc_btn_close: "關閉",
    assoc_unreg_done: "已清除本軟體的圖片檔案關聯",

    toast_single_image_no_slideshow: "目前資料夾僅 1 張圖片，無法播放投影片",
    toast_anim_resumed: "動圖繼續播放",
    toast_anim_paused: "動圖已暫停",
    toast_anim_speed_prefix: "動圖倍速: ",
    toast_saved_png_prefix: "已匯出 PNG: ",
    toast_save_png_failed_prefix: "匯出 PNG 失敗: ",
    toast_copied_image: "已複製影像至剪貼簿",
    toast_copy_failed: "寫入剪貼簿失敗",
    toast_copied_path: "已複製檔案完整路徑",
    toast_wallpaper_ok: "已設為桌面背景",
    toast_wallpaper_fail: "設定桌面背景失敗",
    toast_backdrop_prefix: "畫布背景: ",
    backdrop_glass: "Picasa 透明浮層",
    backdrop_dark: "深邃暗房",
    backdrop_light: "明亮藝廊",
    toast_fullscreen_on: "全螢幕通透懸浮模式 (F11)",
    toast_workarea_on: "保留工作列模式 (F11)",
    toast_registered_ok: "已註冊至系統「開啟檔案」清單",
    toast_register_failed_prefix: "註冊失敗: ",
    toast_moved_to_recycle: "已安全移至資源回收筒",
    toast_recycle_cancelled: "已取消移至資源回收筒",
};

// ─── 3. English (en-US) ────────────────────────────────────────────────────────
pub const STRINGS_EN_US: ViewerStrings = ViewerStrings {
    open_dialog_title: "Select Image - Tools3000 Picasa Viewer",
    open_filter_images: "All Supported Images (JPG/PNG/WebP/GIF/BMP/ICO/TIFF/SVG/TGA/QOI/AVIF/HEIC/RAW)",
    open_filter_all: "All Files",
    no_viewable_images: "No viewable image files found at target path",
    reg_ok_body: "Registered in Open With for the current user.\nYou can set Tools3000 Picasa Viewer as default in Windows Default Apps.",
    reg_fail_title: "Registration Failed",
    unreg_ok_body: "Removed user image file associations.",
    unreg_fail_title: "Unregister Failed",

    tip_zoom_in: "Zoom In (+)",
    tip_zoom_out: "Zoom Out (-)",
    tip_actual_size: "Actual Size 1:1 (1)",
    tip_fit_window: "Fit to Window (0)",
    tip_play_slideshow: "Play Slideshow (F5)",
    tip_pause_slideshow: "Pause Slideshow (Space)",
    tip_resume_slideshow: "Resume Slideshow (Space)",
    tip_exit_slideshow: "Exit Slideshow (Esc)",
    tip_prev_image: "Previous (Left)",
    tip_next_image: "Next (Right)",
    tip_rotate_ccw: "Rotate Counterclockwise (Ctrl+Shift+R)",
    tip_rotate_cw: "Rotate Clockwise (Ctrl+R)",
    tip_flip_h: "Flip Horizontal (H)",
    tip_flip_v: "Flip Vertical (V)",
    tip_close: "Close (Esc)",
    tip_anim_play: "Play Animation (Space)",
    tip_anim_pause: "Pause Animation (Space)",
    tip_anim_prev_frame: "Previous Frame (,)",
    tip_anim_next_frame: "Next Frame (.)",
    tip_anim_speed: "Animation Speed (S)",
    tip_save_png: "Export Frame as PNG (Ctrl+S)",
    tip_scrub_frames: "Drag to scrub animation frames",

    status_cannot_open: "Cannot Open Image",
    status_loading_title: "Loading",
    status_loading_default: "Loading image...",

    info_fallback_title: "Image Details",
    exif_file_size: "File Size",
    exif_format: "Format",
    exif_dimensions: "Dimensions",
    exif_artboard: "Artboard",
    exif_alpha: "Alpha",
    exif_alpha_yes: "Alpha Channel",
    exif_alpha_no: "Opaque",
    exif_frames: "Frames",
    exif_duration: "Duration",
    exif_orientation: "Orientation",
    exif_camera: "Camera",
    exif_lens: "Lens",
    exif_date_taken: "Date Taken",
    exif_exposure: "Exposure",
    exif_focal_length: "Focal Length",
    exif_display_scale: "DPI Scale",
    exif_1to1_suffix: "1:1 Pixel",

    save_png_filter: "Lossless PNG Image (*.png)",
    save_png_title: "Save Current View as PNG",

    menu_actual_or_fit: "Actual Size 1:1 / Fit Window\t1",
    menu_fit_width: "Fit to Width (Tall Image)\tW",
    menu_play_slideshow: "Play Slideshow\tF5",
    menu_stop_slideshow: "Stop Slideshow\tF5",
    menu_fullscreen_overlay: "Fullscreen Overlay\tF11",
    menu_rotate_cw: "Rotate Clockwise 90°\tCtrl+R",
    menu_rotate_ccw: "Rotate Counterclockwise 90°\tCtrl+Shift+R",
    menu_flip_h: "Flip Horizontal\tH",
    menu_flip_v: "Flip Vertical\tV",
    menu_open_file: "Open Image File...\tO",
    menu_copy_image: "Copy Image\tCtrl+C",
    menu_copy_path: "Copy File Path\tCtrl+Shift+C",
    menu_save_frame_png: "Export Current Frame as PNG...\tCtrl+S",
    menu_save_image_png: "Save Current Image as PNG...\tCtrl+S",
    menu_set_wallpaper: "Set as Desktop Wallpaper",
    menu_cycle_backdrop: "Cycle Backdrop Style\tB",
    menu_image_info: "Photo Info & EXIF\tI",
    menu_reveal_explorer: "Show in File Explorer\tF",
    menu_open_with: "Open With...",
    menu_register_assoc: "Set as Default Viewer / File Types...\tCtrl+M",
    menu_delete_recycle: "Move to Recycle Bin\tDel",
    menu_close: "Close\tEsc",

    assoc_dlg_title: "Default Image Viewer & File Associations - Tools3000 Picasa Viewer",
    assoc_dlg_subtitle: "Select the image formats to open with this viewer. Associated files in Explorer will display the classic Picasa Polaroid photo icon:",
    assoc_icon_caption: "Associated File Icon",
    assoc_btn_select_all: "Select All",
    assoc_btn_common: "Common Only",
    assoc_btn_apply_default: "Apply & Open System Default Apps",
    assoc_btn_apply_only: "Apply Icons & Associations Only",
    assoc_btn_remove_all: "Unregister",
    assoc_btn_close: "Close",
    assoc_unreg_done: "Removed image file associations",

    toast_single_image_no_slideshow: "Only 1 image in folder, slideshow unavailable",
    toast_anim_resumed: "Animation resumed",
    toast_anim_paused: "Animation paused",
    toast_anim_speed_prefix: "Playback speed: ",
    toast_saved_png_prefix: "Saved PNG: ",
    toast_save_png_failed_prefix: "Save PNG failed: ",
    toast_copied_image: "Image copied to clipboard",
    toast_copy_failed: "Failed to write clipboard",
    toast_copied_path: "File path copied to clipboard",
    toast_wallpaper_ok: "Set as desktop wallpaper",
    toast_wallpaper_fail: "Failed to set desktop wallpaper",
    toast_backdrop_prefix: "Backdrop: ",
    backdrop_glass: "Picasa Translucent",
    backdrop_dark: "Darkroom Black",
    backdrop_light: "Studio Light",
    toast_fullscreen_on: "Fullscreen Overlay Mode (F11)",
    toast_workarea_on: "Work Area Mode with Taskbar (F11)",
    toast_registered_ok: "Registered in Windows Open With",
    toast_register_failed_prefix: "Registration failed: ",
    toast_moved_to_recycle: "Moved to Recycle Bin",
    toast_recycle_cancelled: "Move to Recycle Bin cancelled",
};

// ─── 4. 日本語 (ja-JP) ─────────────────────────────────────────────────────────
pub const STRINGS_JA_JP: ViewerStrings = ViewerStrings {
    open_dialog_title: "画像を選択 - Tools3000 Picasa Viewer",
    open_filter_images: "対応しているすべての画像 (JPG/PNG/WebP/GIF/BMP/ICO/TIFF/SVG/TGA/QOI/AVIF/HEIC/RAW)",
    open_filter_all: "すべてのファイル",
    no_viewable_images: "表示可能な画像ファイルが見つかりません",
    reg_ok_body: "現在のユーザーの「プログラムから開く」に登録しました。\nWindowsの「既定のアプリ」から既定のビューアーに設定できます。",
    reg_fail_title: "登録に失敗しました",
    unreg_ok_body: "画像ファイルの関連付けを解除しました。",
    unreg_fail_title: "解除に失敗しました",

    tip_zoom_in: "拡大 (+)",
    tip_zoom_out: "縮小 (-)",
    tip_actual_size: "実際のサイズ 1:1 (1)",
    tip_fit_window: "ウィンドウに合わせる (0)",
    tip_play_slideshow: "スライドショー再生 (F5)",
    tip_pause_slideshow: "スライドショー一時停止 (Space)",
    tip_resume_slideshow: "スライドショー再開 (Space)",
    tip_exit_slideshow: "スライドショー終了 (Esc)",
    tip_prev_image: "前の画像 (←)",
    tip_next_image: "次の画像 (→)",
    tip_rotate_ccw: "反時計回りに回転 (Ctrl+Shift+R)",
    tip_rotate_cw: "時計回りに回転 (Ctrl+R)",
    tip_flip_h: "左右反転 (H)",
    tip_flip_v: "上下反転 (V)",
    tip_close: "閉じる (Esc)",
    tip_anim_play: "アニメーション再生 (Space)",
    tip_anim_pause: "アニメーション一時停止 (Space)",
    tip_anim_prev_frame: "前のフレーム (,)",
    tip_anim_next_frame: "次のフレーム (.)",
    tip_anim_speed: "再生速度を切替 (S)",
    tip_save_png: "現在のフレームをPNG出力 (Ctrl+S)",
    tip_scrub_frames: "ドラッグしてフレームを移動",

    status_cannot_open: "画像を開けません",
    status_loading_title: "読み込み中",
    status_loading_default: "画像を読み込んでいます...",

    info_fallback_title: "画像の詳細",
    exif_file_size: "ファイルサイズ",
    exif_format: "形式",
    exif_dimensions: "サイズ",
    exif_artboard: "アートボード",
    exif_alpha: "透過",
    exif_alpha_yes: "アルファあり",
    exif_alpha_no: "不透明",
    exif_frames: "フレーム数",
    exif_duration: "再生時間",
    exif_orientation: "EXIF 向き",
    exif_camera: "カメラ",
    exif_lens: "レンズ",
    exif_date_taken: "撮影日時",
    exif_exposure: "露出",
    exif_focal_length: "焦点距離",
    exif_display_scale: "表示スケール",
    exif_1to1_suffix: "物理 1:1",

    save_png_filter: "PNG 可逆画像 (*.png)",
    save_png_title: "現在の表示を PNG として保存",

    menu_actual_or_fit: "実際のサイズ 1:1 / 画面に合わせる\t1",
    menu_fit_width: "幅に合わせて表示 (縦長画像)\tW",
    menu_play_slideshow: "スライドショーを再生\tF5",
    menu_stop_slideshow: "スライドショーを停止\tF5",
    menu_fullscreen_overlay: "全画面透過オーバーレイ\tF11",
    menu_rotate_cw: "時計回りに 90° 回転\tCtrl+R",
    menu_rotate_ccw: "反時計回りに 90° 回転\tCtrl+Shift+R",
    menu_flip_h: "左右反転\tH",
    menu_flip_v: "上下反転\tV",
    menu_open_file: "画像ファイルを開く...\tO",
    menu_copy_image: "画像をコピー\tCtrl+C",
    menu_copy_path: "ファイルパスをコピー\tCtrl+Shift+C",
    menu_save_frame_png: "現在のフレームを PNG で保存...\tCtrl+S",
    menu_save_image_png: "PNG として別名で保存...\tCtrl+S",
    menu_set_wallpaper: "デスクトップの背景に設定",
    menu_cycle_backdrop: "背景スタイルを切替\tB",
    menu_image_info: "写真情報と EXIF\tI",
    menu_reveal_explorer: "エクスプローラーで表示\tF",
    menu_open_with: "プログラムから開く...",
    menu_register_assoc: "既定のビューアー設定 / 関連付け...\tCtrl+M",
    menu_delete_recycle: "ごみ箱へ移動\tDel",
    menu_close: "閉じる\tEsc",

    assoc_dlg_title: "既定の画像ビューアーとファイルの関連付け - Tools3000 Picasa Viewer",
    assoc_dlg_subtitle: "このビューアーで開く画像形式を選択してください。関連付け後、エクスプローラーの画像アイコンが定番の Picasa ポラロイドアイコンに変わります：",
    assoc_icon_caption: "関連付け後の画像アイコン",
    assoc_btn_select_all: "すべて選択",
    assoc_btn_common: "標準形式のみ",
    assoc_btn_apply_default: "適用してシステムの既定アプリを開く",
    assoc_btn_apply_only: "関連付けとアイコンのみ適用",
    assoc_btn_remove_all: "関連付け解除",
    assoc_btn_close: "閉じる",
    assoc_unreg_done: "画像ファイルの関連付けを解除しました",

    toast_single_image_no_slideshow: "フォルダ内に画像が1枚のみのため再生できません",
    toast_anim_resumed: "アニメーションを再開しました",
    toast_anim_paused: "アニメーションを一時停止しました",
    toast_anim_speed_prefix: "再生速度: ",
    toast_saved_png_prefix: "PNG を保存しました: ",
    toast_save_png_failed_prefix: "PNG 保存失敗: ",
    toast_copied_image: "画像をクリップボードにコピーしました",
    toast_copy_failed: "クリップボードへの書き込みに失敗しました",
    toast_copied_path: "ファイルパスをコピーしました",
    toast_wallpaper_ok: "デスクトップの背景に設定しました",
    toast_wallpaper_fail: "壁紙の設定に失敗しました",
    toast_backdrop_prefix: "背景モード: ",
    backdrop_glass: "Picasa 透過レイヤー",
    backdrop_dark: "ダークルーム",
    backdrop_light: "ライトギャラリー",
    toast_fullscreen_on: "全画面透過モード (F11)",
    toast_workarea_on: "タスクバー表示モード (F11)",
    toast_registered_ok: "「プログラムから開く」に登録しました",
    toast_register_failed_prefix: "登録失敗: ",
    toast_moved_to_recycle: "ごみ箱へ移動しました",
    toast_recycle_cancelled: "ごみ箱への移動をキャンセルしました",
};

// ─── 5. 한국어 (ko-KR) ─────────────────────────────────────────────────────────
pub const STRINGS_KO_KR: ViewerStrings = ViewerStrings {
    open_dialog_title: "볼 이미지 선택 - Tools3000 Picasa Viewer",
    open_filter_images: "지원되는 모든 이미지 (JPG/PNG/WebP/GIF/BMP/ICO/TIFF/SVG/TGA/QOI/AVIF/HEIC/RAW)",
    open_filter_all: "모든 파일",
    no_viewable_images: "대상 경로에 표시할 수 있는 이미지 파일이 없습니다",
    reg_ok_body: "현재 사용자의 '연결 프로그램'에 등록되었습니다.\nWindows 기본 앱 설정에서 기본 뷰어로 지정할 수 있습니다.",
    reg_fail_title: "등록 실패",
    unreg_ok_body: "이미지 파일 연결 등록을 제거했습니다.",
    unreg_fail_title: "등록 해제 실패",

    tip_zoom_in: "확대 (+)",
    tip_zoom_out: "축소 (-)",
    tip_actual_size: "실제 크기 1:1 (1)",
    tip_fit_window: "창 크기에 맞춤 (0)",
    tip_play_slideshow: "슬라이드쇼 재생 (F5)",
    tip_pause_slideshow: "슬라이드쇼 일시정지 (Space)",
    tip_resume_slideshow: "슬라이드쇼 계속 (Space)",
    tip_exit_slideshow: "슬라이드쇼 종료 (Esc)",
    tip_prev_image: "이전 사진 (←)",
    tip_next_image: "다음 사진 (→)",
    tip_rotate_ccw: "반시계 방향 회전 (Ctrl+Shift+R)",
    tip_rotate_cw: "시계 방향 회전 (Ctrl+R)",
    tip_flip_h: "좌우 반전 (H)",
    tip_flip_v: "상하 반전 (V)",
    tip_close: "닫기 (Esc)",
    tip_anim_play: "움짤 재생 (Space)",
    tip_anim_pause: "움짤 일시정지 (Space)",
    tip_anim_prev_frame: "이전 프레임 (,)",
    tip_anim_next_frame: "다음 프레임 (.)",
    tip_anim_speed: "재생 배속 변경 (S)",
    tip_save_png: "현재 프레임 PNG 저장 (Ctrl+S)",
    tip_scrub_frames: "드래그하여 프레임 탐색",

    status_cannot_open: "이미지를 열 수 없음",
    status_loading_title: "불러오는 중",
    status_loading_default: "이미지를 불러오는 중...",

    info_fallback_title: "이미지 정보",
    exif_file_size: "파일 크기",
    exif_format: "형식",
    exif_dimensions: "해상도",
    exif_artboard: "아트보드",
    exif_alpha: "투명 채널",
    exif_alpha_yes: "알파 포함",
    exif_alpha_no: "불투명",
    exif_frames: "프레임 수",
    exif_duration: "재생 시간",
    exif_orientation: "EXIF 방향",
    exif_camera: "카메라",
    exif_lens: "렌즈",
    exif_date_taken: "촬영 일시",
    exif_exposure: "노출 정보",
    exif_focal_length: "초점 거리",
    exif_display_scale: "화면 배율",
    exif_1to1_suffix: "물리 1:1",

    save_png_filter: "무손실 PNG 이미지 (*.png)",
    save_png_title: "현재 화면을 PNG로 저장",

    menu_actual_or_fit: "실제 크기 1:1 / 창에 맞춤\t1",
    menu_fit_width: "가로 폭에 맞춤 (긴 이미지)\tW",
    menu_play_slideshow: "슬라이드쇼 재생\tF5",
    menu_stop_slideshow: "슬라이드쇼 중지\tF5",
    menu_fullscreen_overlay: "전체 화면 투명 오버레이\tF11",
    menu_rotate_cw: "시계 방향 90° 회전\tCtrl+R",
    menu_rotate_ccw: "반시계 방향 90° 회전\tCtrl+Shift+R",
    menu_flip_h: "좌우 반전\tH",
    menu_flip_v: "상하 반전\tV",
    menu_open_file: "이미지 파일 열기...\tO",
    menu_copy_image: "이미지 복사\tCtrl+C",
    menu_copy_path: "파일 경로 복사\tCtrl+Shift+C",
    menu_save_frame_png: "현재 프레임을 PNG로 내보내기...\tCtrl+S",
    menu_save_image_png: "현재 이미지를 PNG로 저장...\tCtrl+S",
    menu_set_wallpaper: "바탕 화면 배경으로 설정",
    menu_cycle_backdrop: "배경 스타일 전환\tB",
    menu_image_info: "사진 상세 정보 및 EXIF\tI",
    menu_reveal_explorer: "파일 탐색기에서 표시\tF",
    menu_open_with: "연결 프로그램...",
    menu_register_assoc: "기본 뷰어 설정 / 파일 연결...\tCtrl+M",
    menu_delete_recycle: "휴지통으로 이동\tDel",
    menu_close: "닫기\tEsc",

    assoc_dlg_title: "기본 이미지 뷰어 및 파일 연결 설정 - Tools3000 Picasa Viewer",
    assoc_dlg_subtitle: "이 뷰어로 열 이미지 형식을 선택하세요. 연결이 적용되면 탐색기의 이미지 아이콘이 클래식 Picasa 폴라로이드 사진 아이콘으로 변경됩니다:",
    assoc_icon_caption: "연결된 이미지 파일 아이콘",
    assoc_btn_select_all: "전체 선택",
    assoc_btn_common: "기본 형식",
    assoc_btn_apply_default: "적용 및 시스템 기본 앱 설정 열기",
    assoc_btn_apply_only: "파일 연결 및 아이콘만 적용",
    assoc_btn_remove_all: "연결 해제",
    assoc_btn_close: "닫기",
    assoc_unreg_done: "이미지 파일 연결을 해제했습니다",

    toast_single_image_no_slideshow: "폴더에 사진이 1장뿐이어서 슬라이드쇼를 시작할 수 없습니다",
    toast_anim_resumed: "재생을 재개했습니다",
    toast_anim_paused: "일시정지됨",
    toast_anim_speed_prefix: "재생 배속: ",
    toast_saved_png_prefix: "PNG 저장 완료: ",
    toast_save_png_failed_prefix: "PNG 저장 실패: ",
    toast_copied_image: "이미지를 클립보드에 복사했습니다",
    toast_copy_failed: "클립보드 복사 실패",
    toast_copied_path: "파일 경로를 복사했습니다",
    toast_wallpaper_ok: "바탕 화면 배경으로 설정했습니다",
    toast_wallpaper_fail: "바탕 화면 설정 실패",
    toast_backdrop_prefix: "배경 모드: ",
    backdrop_glass: "Picasa 반투명 오버레이",
    backdrop_dark: "암실 블랙",
    backdrop_light: "라이트 갤러리",
    toast_fullscreen_on: "전체 화면 오버레이 모드 (F11)",
    toast_workarea_on: "작업 표시줄 유지 모드 (F11)",
    toast_registered_ok: "'연결 프로그램'에 등록되었습니다",
    toast_register_failed_prefix: "등록 실패: ",
    toast_moved_to_recycle: "휴지통으로 이동했습니다",
    toast_recycle_cancelled: "휴지통 이동이 취소되었습니다",
};

// ─── 6. Русский (ru-RU) ────────────────────────────────────────────────────────
pub const STRINGS_RU_RU: ViewerStrings = ViewerStrings {
    open_dialog_title: "Выберите изображение - Tools3000 Picasa Viewer",
    open_filter_images: "Все поддерживаемые изображения (JPG/PNG/WebP/GIF/BMP/ICO/TIFF/SVG/TGA/QOI/AVIF/HEIC/RAW)",
    open_filter_all: "Все файлы",
    no_viewable_images: "В указанном пути нет поддерживаемых изображений",
    reg_ok_body: "Программа добавлена в меню «Открыть с помощью».\nВы можете назначить её просмотрщиком по умолчанию в параметрах Windows.",
    reg_fail_title: "Ошибка регистрации",
    unreg_ok_body: "Ассоциации файлов изображений удалены.",
    unreg_fail_title: "Ошибка удаления ассоциаций",

    tip_zoom_in: "Увеличить (+)",
    tip_zoom_out: "Уменьшить (-)",
    tip_actual_size: "Реальный размер 1:1 (1)",
    tip_fit_window: "Вписать в окно (0)",
    tip_play_slideshow: "Слайд-шоу (F5)",
    tip_pause_slideshow: "Пауза слайд-шоу (Space)",
    tip_resume_slideshow: "Продолжить слайд-шоу (Space)",
    tip_exit_slideshow: "Выйти из слайд-шоу (Esc)",
    tip_prev_image: "Предыдущее (←)",
    tip_next_image: "Следующее (→)",
    tip_rotate_ccw: "Повернуть против часовой (Ctrl+Shift+R)",
    tip_rotate_cw: "Повернуть по часовой (Ctrl+R)",
    tip_flip_h: "Отразить по горизонтали (H)",
    tip_flip_v: "Отразить по вертикали (V)",
    tip_close: "Закрыть (Esc)",
    tip_anim_play: "Воспроизвести анимацию (Space)",
    tip_anim_pause: "Пауза анимации (Space)",
    tip_anim_prev_frame: "Предыдущий кадр (,)",
    tip_anim_next_frame: "Следующий кадр (.)",
    tip_anim_speed: "Скорость анимации (S)",
    tip_save_png: "Сохранить кадр в PNG (Ctrl+S)",
    tip_scrub_frames: "Перетащите для перемотки кадров",

    status_cannot_open: "Не удалось открыть изображение",
    status_loading_title: "Загрузка",
    status_loading_default: "Загрузка изображения...",

    info_fallback_title: "Сведения об изображении",
    exif_file_size: "Размер файла",
    exif_format: "Формат",
    exif_dimensions: "Разрешение",
    exif_artboard: "Холст",
    exif_alpha: "Прозрачность",
    exif_alpha_yes: "Альфа-канал",
    exif_alpha_no: "Непрозрачное",
    exif_frames: "Кадры",
    exif_duration: "Длительность",
    exif_orientation: "Ориентация",
    exif_camera: "Камера",
    exif_lens: "Объектив",
    exif_date_taken: "Дата съёмки",
    exif_exposure: "Экспозиция",
    exif_focal_length: "Фокусное расст.",
    exif_display_scale: "Масштаб экрана",
    exif_1to1_suffix: "1:1 пикс.",

    save_png_filter: "Изображение PNG без потерь (*.png)",
    save_png_title: "Сохранить текущее изображение как PNG",

    menu_actual_or_fit: "Реальный размер 1:1 / Вписать\t1",
    menu_fit_width: "По ширине экрана (длинные скриншоты)\tW",
    menu_play_slideshow: "Запустить слайд-шоу\tF5",
    menu_stop_slideshow: "Остановить слайд-шоу\tF5",
    menu_fullscreen_overlay: "Полноэкранный прозрачный режим\tF11",
    menu_rotate_cw: "Повернуть на 90° по часовой\tCtrl+R",
    menu_rotate_ccw: "Повернуть на 90° против часовой\tCtrl+Shift+R",
    menu_flip_h: "Отразить по горизонтали\tH",
    menu_flip_v: "Отразить по вертикали\tV",
    menu_open_file: "Открыть изображение...\tO",
    menu_copy_image: "Копировать изображение\tCtrl+C",
    menu_copy_path: "Копировать путь к файлу\tCtrl+Shift+C",
    menu_save_frame_png: "Сохранить текущий кадр как PNG...\tCtrl+S",
    menu_save_image_png: "Сохранить как PNG...\tCtrl+S",
    menu_set_wallpaper: "Сделать фоном рабочего стола",
    menu_cycle_backdrop: "Сменить стиль подложки\tB",
    menu_image_info: "Свойства фото и EXIF\tI",
    menu_reveal_explorer: "Показать в Проводнике\tF",
    menu_open_with: "Открыть с помощью...",
    menu_register_assoc: "Просмотрщик по умолчанию / Ассоциации...\tCtrl+M",
    menu_delete_recycle: "Удалить в Корзину\tDel",
    menu_close: "Закрыть\tEsc",

    assoc_dlg_title: "Просмотрщик по умолчанию и ассоциации файлов - Tools3000 Picasa Viewer",
    assoc_dlg_subtitle: "Выберите форматы изображений для открытия в этой программе. После применения файлы получат классический значок фотографии Picasa:",
    assoc_icon_caption: "Значок связанных файлов",
    assoc_btn_select_all: "Выбрать все",
    assoc_btn_common: "Основные",
    assoc_btn_apply_default: "Применить и открыть параметры по умолчанию",
    assoc_btn_apply_only: "Только применить значки и типы",
    assoc_btn_remove_all: "Сбросить",
    assoc_btn_close: "Закрыть",
    assoc_unreg_done: "Ассоциации файлов изображений удалены",

    toast_single_image_no_slideshow: "В папке только 1 файл — слайд-шоу недоступно",
    toast_anim_resumed: "Анимация продолжена",
    toast_anim_paused: "Анимация на паузе",
    toast_anim_speed_prefix: "Скорость: ",
    toast_saved_png_prefix: "Сохранено в PNG: ",
    toast_save_png_failed_prefix: "Ошибка сохранения PNG: ",
    toast_copied_image: "Изображение скопировано в буфер обмена",
    toast_copy_failed: "Не удалось скопировать в буфер обмена",
    toast_copied_path: "Путь к файлу скопирован",
    toast_wallpaper_ok: "Установлено как фон рабочего стола",
    toast_wallpaper_fail: "Не удалось установить фон рабочего стола",
    toast_backdrop_prefix: "Подложка: ",
    backdrop_glass: "Прозрачный слой Picasa",
    backdrop_dark: "Тёмная комната",
    backdrop_light: "Светлая студия",
    toast_fullscreen_on: "Полноэкранный режим (F11)",
    toast_workarea_on: "Режим рабочей области с панелью задач (F11)",
    toast_registered_ok: "Добавлено в меню «Открыть с помощью»",
    toast_register_failed_prefix: "Ошибка регистрации: ",
    toast_moved_to_recycle: "Перемещено в Корзину",
    toast_recycle_cancelled: "Удаление отменено",
};

// ─── 7. Português - Brasil (pt-BR) ─────────────────────────────────────────────
pub const STRINGS_PT_BR: ViewerStrings = ViewerStrings {
    open_dialog_title: "Selecionar imagem - Tools3000 Picasa Viewer",
    open_filter_images: "Todas as imagens suportadas (JPG/PNG/WebP/GIF/BMP/ICO/TIFF/SVG/TGA/QOI/AVIF/HEIC/RAW)",
    open_filter_all: "Todos os arquivos",
    no_viewable_images: "Nenhum arquivo de imagem encontrado no caminho",
    reg_ok_body: "Registrado em \"Abrir com\" para o usuário atual.\nVocê pode defini-lo como visualizador padrão nas Configurações do Windows.",
    reg_fail_title: "Falha no registro",
    unreg_ok_body: "Associações de arquivos de imagem removidas.",
    unreg_fail_title: "Falha ao remover associação",

    tip_zoom_in: "Aumentar zoom (+)",
    tip_zoom_out: "Diminuir zoom (-)",
    tip_actual_size: "Tamanho real 1:1 (1)",
    tip_fit_window: "Ajustar à janela (0)",
    tip_play_slideshow: "Iniciar apresentação (F5)",
    tip_pause_slideshow: "Pausar apresentação (Space)",
    tip_resume_slideshow: "Retomar apresentação (Space)",
    tip_exit_slideshow: "Sair da apresentação (Esc)",
    tip_prev_image: "Anterior (←)",
    tip_next_image: "Próxima (→)",
    tip_rotate_ccw: "Girar no sentido anti-horário (Ctrl+Shift+R)",
    tip_rotate_cw: "Girar no sentido horário (Ctrl+R)",
    tip_flip_h: "Inverter horizontalmente (H)",
    tip_flip_v: "Inverter verticalmente (V)",
    tip_close: "Fechar (Esc)",
    tip_anim_play: "Reproduzir animação (Space)",
    tip_anim_pause: "Pausar animação (Space)",
    tip_anim_prev_frame: "Quadro anterior (,)",
    tip_anim_next_frame: "Próximo quadro (.)",
    tip_anim_speed: "Velocidade da animação (S)",
    tip_save_png: "Exportar quadro como PNG (Ctrl+S)",
    tip_scrub_frames: "Arraste para navegar pelos quadros",

    status_cannot_open: "Não foi possível abrir a imagem",
    status_loading_title: "Carregando",
    status_loading_default: "Carregando imagem...",

    info_fallback_title: "Detalhes da imagem",
    exif_file_size: "Tamanho",
    exif_format: "Formato",
    exif_dimensions: "Dimensões",
    exif_artboard: "Prancheta",
    exif_alpha: "Transparência",
    exif_alpha_yes: "Canal Alfa",
    exif_alpha_no: "Opaco",
    exif_frames: "Quadros",
    exif_duration: "Duração",
    exif_orientation: "Orientação",
    exif_camera: "Câmera",
    exif_lens: "Lente",
    exif_date_taken: "Data da foto",
    exif_exposure: "Exposição",
    exif_focal_length: "Dist. focal",
    exif_display_scale: "Escala de tela",
    exif_1to1_suffix: "1:1 real",

    save_png_filter: "Imagem PNG sem perdas (*.png)",
    save_png_title: "Salvar visualização atual como PNG",

    menu_actual_or_fit: "Tamanho real 1:1 / Ajustar\t1",
    menu_fit_width: "Ajustar à largura (imagem longa)\tW",
    menu_play_slideshow: "Reproduzir apresentação\tF5",
    menu_stop_slideshow: "Parar apresentação\tF5",
    menu_fullscreen_overlay: "Modo tela cheia translúcido\tF11",
    menu_rotate_cw: "Girar 90° no sentido horário\tCtrl+R",
    menu_rotate_ccw: "Girar 90° no sentido anti-horário\tCtrl+Shift+R",
    menu_flip_h: "Inverter horizontalmente\tH",
    menu_flip_v: "Inverter verticalmente\tV",
    menu_open_file: "Abrir arquivo de imagem...\tO",
    menu_copy_image: "Copiar imagem\tCtrl+C",
    menu_copy_path: "Copiar caminho do arquivo\tCtrl+Shift+C",
    menu_save_frame_png: "Exportar quadro atual como PNG...\tCtrl+S",
    menu_save_image_png: "Salvar imagem como PNG...\tCtrl+S",
    menu_set_wallpaper: "Definir como plano de fundo",
    menu_cycle_backdrop: "Alternar fundo da tela\tB",
    menu_image_info: "Informações da foto e EXIF\tI",
    menu_reveal_explorer: "Mostrar no Explorador de Arquivos\tF",
    menu_open_with: "Abrir com...",
    menu_register_assoc: "Definir como padrão / Tipos de arquivo...\tCtrl+M",
    menu_delete_recycle: "Mover para a Lixeira\tDel",
    menu_close: "Fechar\tEsc",

    assoc_dlg_title: "Visualizador padrão e associações de arquivos - Tools3000 Picasa Viewer",
    assoc_dlg_subtitle: "Selecione os formatos de imagem para abrir neste visualizador. Os arquivos exibirão o clássico ícone Polaroid do Picasa:",
    assoc_icon_caption: "Ícone de arquivo associado",
    assoc_btn_select_all: "Marcar tudo",
    assoc_btn_common: "Formatos comuns",
    assoc_btn_apply_default: "Aplicar e abrir Apps Padrão do Windows",
    assoc_btn_apply_only: "Aplicar apenas ícones e tipos",
    assoc_btn_remove_all: "Remover",
    assoc_btn_close: "Fechar",
    assoc_unreg_done: "Associações de arquivos removidas",

    toast_single_image_no_slideshow: "Apenas 1 imagem na pasta, apresentação indisponível",
    toast_anim_resumed: "Animação retomada",
    toast_anim_paused: "Animação pausada",
    toast_anim_speed_prefix: "Velocidade: ",
    toast_saved_png_prefix: "PNG salvo: ",
    toast_save_png_failed_prefix: "Falha ao salvar PNG: ",
    toast_copied_image: "Imagem copiada para a área de transferência",
    toast_copy_failed: "Falha ao copiar para a área de transferência",
    toast_copied_path: "Caminho do arquivo copiado",
    toast_wallpaper_ok: "Definido como plano de fundo",
    toast_wallpaper_fail: "Falha ao definir plano de fundo",
    toast_backdrop_prefix: "Fundo: ",
    backdrop_glass: "Translúcido Picasa",
    backdrop_dark: "Câmara Escura",
    backdrop_light: "Galeria Clara",
    toast_fullscreen_on: "Modo tela cheia translúcido (F11)",
    toast_workarea_on: "Modo área de trabalho com barra (F11)",
    toast_registered_ok: "Registrado em \"Abrir com\" do Windows",
    toast_register_failed_prefix: "Falha no registro: ",
    toast_moved_to_recycle: "Movido para a Lixeira",
    toast_recycle_cancelled: "Exclusão cancelada",
};

// ─── 8. Español (es-ES) ────────────────────────────────────────────────────────
pub const STRINGS_ES_ES: ViewerStrings = ViewerStrings {
    open_dialog_title: "Seleccionar imagen - Tools3000 Picasa Viewer",
    open_filter_images: "Todas las imágenes compatibles (JPG/PNG/WebP/GIF/BMP/ICO/TIFF/SVG/TGA/QOI/AVIF/HEIC/RAW)",
    open_filter_all: "Todos los archivos",
    no_viewable_images: "No se encontraron archivos de imagen en la ruta",
    reg_ok_body: "Registrado correctamente en «Abrir con» para el usuario actual.\nPuede establecerlo como visor predeterminado en Configuración de Windows.",
    reg_fail_title: "Error de registro",
    unreg_ok_body: "Se eliminaron las asociaciones de archivos de imagen.",
    unreg_fail_title: "Error al eliminar asociación",

    tip_zoom_in: "Acercar (+)",
    tip_zoom_out: "Alejar (-)",
    tip_actual_size: "Tamaño real 1:1 (1)",
    tip_fit_window: "Ajustar a la ventana (0)",
    tip_play_slideshow: "Reproducir presentación (F5)",
    tip_pause_slideshow: "Pausar presentación (Space)",
    tip_resume_slideshow: "Reanudar presentación (Space)",
    tip_exit_slideshow: "Salir de la presentación (Esc)",
    tip_prev_image: "Anterior (←)",
    tip_next_image: "Siguiente (→)",
    tip_rotate_ccw: "Girar a la izquierda (Ctrl+Shift+R)",
    tip_rotate_cw: "Girar a la derecha (Ctrl+R)",
    tip_flip_h: "Voltear horizontalmente (H)",
    tip_flip_v: "Voltear verticalmente (V)",
    tip_close: "Cerrar (Esc)",
    tip_anim_play: "Reproducir animación (Space)",
    tip_anim_pause: "Pausar animación (Space)",
    tip_anim_prev_frame: "Fotograma anterior (,)",
    tip_anim_next_frame: "Fotograma siguiente (.)",
    tip_anim_speed: "Velocidad de animación (S)",
    tip_save_png: "Exportar fotograma como PNG (Ctrl+S)",
    tip_scrub_frames: "Arrastre para recorrer los fotogramas",

    status_cannot_open: "No se puede abrir la imagen",
    status_loading_title: "Cargando",
    status_loading_default: "Cargando imagen...",

    info_fallback_title: "Detalles de la imagen",
    exif_file_size: "Tamaño",
    exif_format: "Formato",
    exif_dimensions: "Dimensiones",
    exif_artboard: "Mesa de trabajo",
    exif_alpha: "Transparencia",
    exif_alpha_yes: "Canal Alfa",
    exif_alpha_no: "Opaco",
    exif_frames: "Fotogramas",
    exif_duration: "Duración",
    exif_orientation: "Orientación",
    exif_camera: "Cámara",
    exif_lens: "Objetivo",
    exif_date_taken: "Fecha de captura",
    exif_exposure: "Exposición",
    exif_focal_length: "Dist. focal",
    exif_display_scale: "Escala pantalla",
    exif_1to1_suffix: "1:1 físico",

    save_png_filter: "Imagen PNG sin pérdida (*.png)",
    save_png_title: "Guardar vista actual como PNG",

    menu_actual_or_fit: "Tamaño real 1:1 / Ajustar\t1",
    menu_fit_width: "Ajustar al ancho (imagen larga)\tW",
    menu_play_slideshow: "Iniciar presentación\tF5",
    menu_stop_slideshow: "Detener presentación\tF5",
    menu_fullscreen_overlay: "Superposición a pantalla completa\tF11",
    menu_rotate_cw: "Girar 90° a la derecha\tCtrl+R",
    menu_rotate_ccw: "Girar 90° a la izquierda\tCtrl+Shift+R",
    menu_flip_h: "Voltear horizontalmente\tH",
    menu_flip_v: "Voltear verticalmente\tV",
    menu_open_file: "Abrir archivo de imagen...\tO",
    menu_copy_image: "Copiar imagen\tCtrl+C",
    menu_copy_path: "Copiar ruta del archivo\tCtrl+Shift+C",
    menu_save_frame_png: "Exportar fotograma actual como PNG...\tCtrl+S",
    menu_save_image_png: "Guardar imagen como PNG...\tCtrl+S",
    menu_set_wallpaper: "Establecer como fondo de escritorio",
    menu_cycle_backdrop: "Cambiar estilo de fondo\tB",
    menu_image_info: "Información de foto y EXIF\tI",
    menu_reveal_explorer: "Mostrar en el Explorador\tF",
    menu_open_with: "Abrir con...",
    menu_register_assoc: "Establecer como predeterminado / Tipos...\tCtrl+M",
    menu_delete_recycle: "Mover a la papelera\tDel",
    menu_close: "Cerrar\tEsc",

    assoc_dlg_title: "Visor predeterminado y asociación de archivos - Tools3000 Picasa Viewer",
    assoc_dlg_subtitle: "Seleccione los formatos de imagen que abrirá este visor. Los archivos mostrarán el clásico icono Polaroid de Picasa:",
    assoc_icon_caption: "Icono de archivo asociado",
    assoc_btn_select_all: "Seleccionar todo",
    assoc_btn_common: "Comunes",
    assoc_btn_apply_default: "Aplicar y abrir Aplicaciones predeterminadas",
    assoc_btn_apply_only: "Aplicar solo iconos y tipos",
    assoc_btn_remove_all: "Quitar asociación",
    assoc_btn_close: "Cerrar",
    assoc_unreg_done: "Se eliminaron las asociaciones de imagen",

    toast_single_image_no_slideshow: "Solo hay 1 imagen en la carpeta, presentación no disponible",
    toast_anim_resumed: "Animación reanudada",
    toast_anim_paused: "Animación en pausa",
    toast_anim_speed_prefix: "Velocidad: ",
    toast_saved_png_prefix: "PNG guardado: ",
    toast_save_png_failed_prefix: "Error al guardar PNG: ",
    toast_copied_image: "Imagen copiada al portapapeles",
    toast_copy_failed: "Error al copiar al portapapeles",
    toast_copied_path: "Ruta del archivo copiada",
    toast_wallpaper_ok: "Establecido como fondo de escritorio",
    toast_wallpaper_fail: "No se pudo establecer el fondo de escritorio",
    toast_backdrop_prefix: "Fondo: ",
    backdrop_glass: "Superposición Picasa",
    backdrop_dark: "Cuarto Oscuro",
    backdrop_light: "Estudio Claro",
    toast_fullscreen_on: "Modo pantalla completa (F11)",
    toast_workarea_on: "Modo área de trabajo con barra (F11)",
    toast_registered_ok: "Registrado en «Abrir con» de Windows",
    toast_register_failed_prefix: "Error de registro: ",
    toast_moved_to_recycle: "Movido a la papelera de reciclaje",
    toast_recycle_cancelled: "Eliminación cancelada",
};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_all_8_supported_locales_matching_tools3000() {
        assert_eq!(ViewerLanguage::from_tag("zh-CN"), ViewerLanguage::ZhCN);
        assert_eq!(ViewerLanguage::from_tag("zh_SG"), ViewerLanguage::ZhCN);
        assert_eq!(ViewerLanguage::from_tag("zh-TW"), ViewerLanguage::ZhTW);
        assert_eq!(ViewerLanguage::from_tag("zh-HK"), ViewerLanguage::ZhTW);
        assert_eq!(ViewerLanguage::from_tag("zh-Hant"), ViewerLanguage::ZhTW);
        assert_eq!(ViewerLanguage::from_tag("en-US"), ViewerLanguage::EnUS);
        assert_eq!(ViewerLanguage::from_tag("fr-FR"), ViewerLanguage::EnUS); // 未知语种安全回退 EnUS
        assert_eq!(ViewerLanguage::from_tag("ja-JP"), ViewerLanguage::JaJP);
        assert_eq!(ViewerLanguage::from_tag("ko-KR"), ViewerLanguage::KoKR);
        assert_eq!(ViewerLanguage::from_tag("ru-RU"), ViewerLanguage::RuRU);
        assert_eq!(ViewerLanguage::from_tag("pt-BR"), ViewerLanguage::PtBR);
        assert_eq!(ViewerLanguage::from_tag("es-ES"), ViewerLanguage::EsES);
        assert_eq!(ViewerLanguage::from_tag("es-419"), ViewerLanguage::EsES);
    }

    #[test]
    fn maps_windows_langid_accurately() {
        assert_eq!(ViewerLanguage::from_windows_langid(0x0804), ViewerLanguage::ZhCN);
        assert_eq!(ViewerLanguage::from_windows_langid(0x0404), ViewerLanguage::ZhTW);
        assert_eq!(ViewerLanguage::from_windows_langid(0x0C04), ViewerLanguage::ZhTW);
        assert_eq!(ViewerLanguage::from_windows_langid(0x0409), ViewerLanguage::EnUS);
        assert_eq!(ViewerLanguage::from_windows_langid(0x0411), ViewerLanguage::JaJP);
        assert_eq!(ViewerLanguage::from_windows_langid(0x0412), ViewerLanguage::KoKR);
        assert_eq!(ViewerLanguage::from_windows_langid(0x0419), ViewerLanguage::RuRU);
        assert_eq!(ViewerLanguage::from_windows_langid(0x0416), ViewerLanguage::PtBR);
        assert_eq!(ViewerLanguage::from_windows_langid(0x0C0A), ViewerLanguage::EsES);
    }

    #[test]
    fn non_cjk_locales_contain_zero_han_character_pollution() {
        let non_cjk = [
            ViewerLanguage::EnUS,
            ViewerLanguage::RuRU,
            ViewerLanguage::PtBR,
            ViewerLanguage::EsES,
            ViewerLanguage::KoKR,
        ];
        for lang in non_cjk {
            let s = lang.strings();
            let sample_fields = [
                s.open_dialog_title,
                s.tip_zoom_in,
                s.tip_zoom_out,
                s.tip_actual_size,
                s.tip_play_slideshow,
                s.tip_rotate_ccw,
                s.tip_rotate_cw,
                s.tip_flip_h,
                s.tip_flip_v,
                s.tip_resume_slideshow,
                s.tip_exit_slideshow,
                s.tip_close,
                s.menu_actual_or_fit,
                s.menu_flip_h,
                s.menu_flip_v,
                s.menu_open_file,
                s.menu_copy_image,
                s.menu_set_wallpaper,
                s.menu_delete_recycle,
                s.exif_file_size,
                s.exif_camera,
                s.backdrop_glass,
            ];
            for text in sample_fields {
                let has_han = text.chars().any(|c| ('\u{4E00}'..='\u{9FFF}').contains(&c));
                assert!(
                    !has_han,
                    "Locale {} contains unexpected Han character in {:?}",
                    lang.tag(),
                    text
                );
            }
        }
    }
}
