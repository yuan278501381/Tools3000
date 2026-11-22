// ─────────────────────────────────────────────────────────────────────────────
// viewport.rs — 2D 物理相机：光标锚点无级阻尼缩放 · 惯性甩动滑行 · 橡皮筋边界回弹
//               连续 90° 丝滑旋转 · 屏幕感知水平/垂直 3D 镜像翻转 · 高分屏物理 1:1
//
// 坐标契约（全面兼容 Windows 100% ~ 250%+ 混合多屏缩放）：
// - `view_w` / `view_h` / `pan_x` / `pan_y` / `cursor_x` / `cursor_y` 均为逻辑 DIP
// - `img_w` / `img_h` 为图像原始物理像素尺寸
// - `zoom == 1.0`（100%）严格等于 1 图像像素 = 1 显示器物理像素，绝不被系统拉伸发虚
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{
    FIT_BOTTOM_SAFE_PAD_DIP, FIT_SCREEN_MARGIN_RATIO, FIT_TOP_SAFE_PAD_DIP, FLIP_ANIM_DAMPING,
    KINETIC_FRICTION_DAMPING, MAX_ZOOM, MIN_ZOOM, ROTATION_ANIM_DAMPING, RUBBER_BAND_MAX_DIP,
    RUBBER_BAND_SPRING_DAMPING, SLIDESHOW_MARGIN_RATIO, SLIDESHOW_ZOOM_ANIM_DAMPING,
    WINDOWED_FIT_MARGIN_RATIO, ZOOM_ANIM_DAMPING, ZOOM_STEP_FACTOR,
};
use crate::physics::{exp_decay_factor, exp_smooth, rubber_band_offset};
use crate::types::{FitLayoutMode, RectF, RotationQuarter};

/// 二维物理相机状态机：分离 current 与 target 实现 60~240Hz 帧率无关指数阻尼动画
#[derive(Clone, Copy, Debug)]
pub struct ViewportCamera {
    pub zoom: f32,
    pub target_zoom: f32,
    pub pan_x: f32,
    pub pan_y: f32,
    pub target_pan_x: f32,
    pub target_pan_y: f32,
    /// 惯性甩动速度向量 (DIP / sec)
    pub vel_x: f32,
    pub vel_y: f32,
    /// 拖拽橡皮筋原始未钳制坐标
    raw_drag_pan_x: f32,
    raw_drag_pan_y: f32,
    pub is_dragging: bool,

    pub rotation: RotationQuarter,
    pub visual_rotation_deg: f32,
    pub target_rotation_deg: f32,

    /// 图像局部坐标系水平/垂直镜像标记及连续翻转插值 (-1.0 .. +1.0)
    pub flip_x: bool,
    pub flip_y: bool,
    pub visual_flip_x: f32,
    pub visual_flip_y: f32,

    pub is_fit_mode: bool,
    /// 当前 Fit 布局模式（浮层 / 标准小窗 / 全屏幻灯片）
    pub fit_layout_mode: FitLayoutMode,
    /// 浮层态未放大自由拖拽定位标记（1:1 复刻原版 Picasa 在屏幕内自由挪动照片并停留在松手位置）
    pub free_drag_pan: bool,
    /// 光标锚点缩放自由偏移标记：允许图像在尚未充满屏幕时依然严格锁定鼠标下像素点，
    /// 不被强制拍回屏幕正中，跨过填满屏幕临界点全程零跳变
    pub cursor_zoom_free_pan: bool,
    /// 进出幻灯片放映过渡期启用柔和扩图阻尼（与 1.00s 影院熄灯同频）
    slideshow_transition_damping: bool,
    /// 当前显示器物理缩放因子（96 DPI = 1.0，144 DPI = 1.5，192 DPI = 2.0）
    pub dpi_scale: f32,
}

impl Default for ViewportCamera {
    fn default() -> Self {
        Self {
            zoom: 1.0,
            target_zoom: 1.0,
            pan_x: 0.0,
            pan_y: 0.0,
            target_pan_x: 0.0,
            target_pan_y: 0.0,
            vel_x: 0.0,
            vel_y: 0.0,
            raw_drag_pan_x: 0.0,
            raw_drag_pan_y: 0.0,
            is_dragging: false,
            rotation: RotationQuarter::Deg0,
            visual_rotation_deg: 0.0,
            target_rotation_deg: 0.0,
            flip_x: false,
            flip_y: false,
            visual_flip_x: 1.0,
            visual_flip_y: 1.0,
            is_fit_mode: true,
            fit_layout_mode: FitLayoutMode::Overlay,
            free_drag_pan: false,
            cursor_zoom_free_pan: false,
            slideshow_transition_damping: false,
            dpi_scale: 1.0,
        }
    }
}

impl ViewportCamera {
    pub fn set_dpi_scale(&mut self, dpi_scale: f32) {
        self.dpi_scale = dpi_scale.clamp(0.5, 4.0);
    }

    pub fn set_fit_layout_mode(&mut self, mode: FitLayoutMode) {
        self.fit_layout_mode = mode;
        if mode != FitLayoutMode::Overlay {
            self.free_drag_pan = false;
        }
    }

    /// 单个图像物理像素在当前缩放倍率下对应的逻辑 DIP 跨度
    #[inline]
    pub fn dip_per_image_pixel(&self, zoom: f32) -> f32 {
        zoom / self.dpi_scale.max(0.5)
    }

    /// 计算图片按当前旋转角度与显示器 DPI 缩放适应视口的理想物理倍率
    pub fn compute_fit_zoom(
        img_w: f32,
        img_h: f32,
        rotation: RotationQuarter,
        view_w: f32,
        view_h: f32,
        dpi_scale: f32,
    ) -> f32 {
        Self::compute_fit_zoom_for_mode(
            img_w,
            img_h,
            rotation,
            view_w,
            view_h,
            dpi_scale,
            FitLayoutMode::Overlay,
        )
    }

    /// 支持浮层看图（底部留出 HUD 呼吸空间）、标准小窗（紧凑满幅居中）与幻灯片放映（100% 全屏）三种 Fit 计算
    pub fn compute_fit_zoom_for_mode(
        img_w: f32,
        img_h: f32,
        rotation: RotationQuarter,
        view_w: f32,
        view_h: f32,
        dpi_scale: f32,
        mode: FitLayoutMode,
    ) -> f32 {
        if img_w <= 0.0 || img_h <= 0.0 || view_w <= 0.0 || view_h <= 0.0 {
            return 1.0;
        }
        let scale = dpi_scale.clamp(0.5, 4.0);
        let (eff_w, eff_h) = if rotation.swaps_axes() {
            (img_h, img_w)
        } else {
            (img_w, img_h)
        };
        let (avail_w_dip, avail_h_dip) = match mode {
            FitLayoutMode::Slideshow => (
                (view_w * SLIDESHOW_MARGIN_RATIO).max(64.0),
                (view_h * SLIDESHOW_MARGIN_RATIO).max(64.0),
            ),
            FitLayoutMode::Windowed => (
                (view_w * WINDOWED_FIT_MARGIN_RATIO).max(64.0),
                (view_h * WINDOWED_FIT_MARGIN_RATIO).max(64.0),
            ),
            FitLayoutMode::Overlay => (
                (view_w * FIT_SCREEN_MARGIN_RATIO).max(64.0),
                (view_h - FIT_TOP_SAFE_PAD_DIP - FIT_BOTTOM_SAFE_PAD_DIP)
                    .min(view_h * FIT_SCREEN_MARGIN_RATIO)
                    .max(64.0),
            ),
        };
        let avail_w_phys = avail_w_dip * scale;
        let avail_h_phys = avail_h_dip * scale;
        let fit = (avail_w_phys / eff_w).min(avail_h_phys / eff_h);
        fit.min(1.0).clamp(MIN_ZOOM, MAX_ZOOM)
    }

    /// 浮层态将垂直中心微偏上方呼吸区避让底栏；标准小窗态与幻灯片态严格正中
    pub fn default_fit_pan_y(
        img_w: f32,
        img_h: f32,
        rotation: RotationQuarter,
        fit_zoom: f32,
        view_h: f32,
        dpi_scale: f32,
        mode: FitLayoutMode,
    ) -> f32 {
        if matches!(mode, FitLayoutMode::Slideshow | FitLayoutMode::Windowed) {
            return 0.0;
        }
        let eff_h = if rotation.swaps_axes() { img_w } else { img_h };
        let disp_h = eff_h * (fit_zoom / dpi_scale.max(0.5));
        let safe_center_y = (FIT_TOP_SAFE_PAD_DIP + (view_h - FIT_BOTTOM_SAFE_PAD_DIP)) * 0.5;
        let natural_bottom = view_h * 0.5 + disp_h * 0.5;
        if natural_bottom > view_h - FIT_BOTTOM_SAFE_PAD_DIP + 10.0 {
            safe_center_y - view_h * 0.5
        } else {
            // 小图微偏上 -14 DIP，视觉重心更轻盈舒展
            -14.0f32.min((view_h * 0.04).max(0.0))
        }
    }

    /// 重置为适应屏幕居中状态（单元测试快捷入口）
    #[cfg(test)]
    pub fn reset_to_fit(&mut self, img_w: f32, img_h: f32, view_w: f32, view_h: f32) {
        self.reset_to_fit_with_mode(img_w, img_h, view_w, view_h, FitLayoutMode::Overlay);
    }

    pub fn reset_to_fit_with_mode(
        &mut self,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
        mode: FitLayoutMode,
    ) {
        self.rotation = RotationQuarter::Deg0;
        self.visual_rotation_deg = 0.0;
        self.target_rotation_deg = 0.0;
        self.flip_x = false;
        self.flip_y = false;
        self.visual_flip_x = 1.0;
        self.visual_flip_y = 1.0;
        self.vel_x = 0.0;
        self.vel_y = 0.0;
        self.is_dragging = false;
        self.fit_layout_mode = mode;

        let fit = Self::compute_fit_zoom_for_mode(
            img_w,
            img_h,
            self.rotation,
            view_w,
            view_h,
            self.dpi_scale,
            mode,
        );
        let py = Self::default_fit_pan_y(
            img_w,
            img_h,
            self.rotation,
            fit,
            view_h,
            self.dpi_scale,
            mode,
        );
        self.zoom = fit;
        self.target_zoom = fit;
        self.pan_x = 0.0;
        self.pan_y = py;
        self.target_pan_x = 0.0;
        self.target_pan_y = py;
        self.raw_drag_pan_x = 0.0;
        self.raw_drag_pan_y = py;
        self.is_fit_mode = true;
        self.free_drag_pan = false;
        self.cursor_zoom_free_pan = false;
        self.slideshow_transition_damping = false;
    }

    /// 进出幻灯片全屏模式或浮层尺寸变化时，平滑过渡至对应 Fit 最佳视口而不打断当前旋转/翻转态
    pub fn animate_to_fit_mode(
        &mut self,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
        mode: FitLayoutMode,
    ) {
        self.fit_layout_mode = mode;
        let fit = Self::compute_fit_zoom_for_mode(
            img_w,
            img_h,
            self.rotation,
            view_w,
            view_h,
            self.dpi_scale,
            mode,
        );
        let py = Self::default_fit_pan_y(
            img_w,
            img_h,
            self.rotation,
            fit,
            view_h,
            self.dpi_scale,
            mode,
        );
        self.vel_x = 0.0;
        self.vel_y = 0.0;
        self.target_zoom = fit;
        self.target_pan_x = 0.0;
        self.target_pan_y = py;
        self.is_fit_mode = true;
        self.free_drag_pan = false;
        self.cursor_zoom_free_pan = false;
        self.slideshow_transition_damping = true;
    }

    /// 直接无跳变就位目标 Fit 几何态（用于进出幻灯片整屏叠化及小窗交互缩放 1:1 跟手重排）
    pub fn snap_to_fit_mode(
        &mut self,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
        mode: FitLayoutMode,
    ) {
        self.fit_layout_mode = mode;
        let fit = Self::compute_fit_zoom_for_mode(
            img_w,
            img_h,
            self.rotation,
            view_w,
            view_h,
            self.dpi_scale,
            mode,
        );
        let py = Self::default_fit_pan_y(
            img_w,
            img_h,
            self.rotation,
            fit,
            view_h,
            self.dpi_scale,
            mode,
        );
        self.vel_x = 0.0;
        self.vel_y = 0.0;
        self.zoom = fit;
        self.target_zoom = fit;
        self.pan_x = 0.0;
        self.pan_y = py;
        self.target_pan_x = 0.0;
        self.target_pan_y = py;
        self.raw_drag_pan_x = 0.0;
        self.raw_drag_pan_y = py;
        self.is_fit_mode = true;
        self.free_drag_pan = false;
        self.cursor_zoom_free_pan = false;
        self.slideshow_transition_damping = false;
    }

    /// 窗口 DPI 或分辨率变化时，保持现有旋转与翻转状态立即重新适配 Fit 模式
    #[allow(dead_code)]
    pub fn refit_preserving_rotation(&mut self, img_w: f32, img_h: f32, view_w: f32, view_h: f32) {
        let mode = self.fit_layout_mode;
        self.snap_to_fit_mode(img_w, img_h, view_w, view_h, mode);
    }

    /// 顺时针 / 逆时针 90° 连续丝滑旋转，同步联动重算宽高互换后的新适应倍率
    pub fn rotate_step(
        &mut self,
        clockwise: bool,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
        mode: FitLayoutMode,
    ) {
        self.fit_layout_mode = mode;
        self.vel_x = 0.0;
        self.vel_y = 0.0;
        self.slideshow_transition_damping = false;
        if clockwise {
            self.rotation = self.rotation.rotate_cw();
            self.target_rotation_deg += 90.0;
        } else {
            self.rotation = self.rotation.rotate_ccw();
            self.target_rotation_deg -= 90.0;
        }
        if self.is_fit_mode {
            let fit = Self::compute_fit_zoom_for_mode(
                img_w,
                img_h,
                self.rotation,
                view_w,
                view_h,
                self.dpi_scale,
                mode,
            );
            let py = Self::default_fit_pan_y(
                img_w,
                img_h,
                self.rotation,
                fit,
                view_h,
                self.dpi_scale,
                mode,
            );
            self.target_zoom = fit;
            self.target_pan_x = 0.0;
            self.target_pan_y = py;
            self.free_drag_pan = false;
            self.cursor_zoom_free_pan = false;
        } else {
            self.cursor_zoom_free_pan = false;
            self.clamp_target_pan(img_w, img_h, view_w, view_h);
        }
    }

    /// 屏幕视觉水平翻转（左右照镜子）：无论当前旋转到了 0°/90°/180°/270°，始终沿屏幕竖直中轴左右翻面
    pub fn toggle_flip_horizontal(&mut self) {
        if self.rotation.swaps_axes() {
            self.flip_y = !self.flip_y;
        } else {
            self.flip_x = !self.flip_x;
        }
    }

    /// 屏幕视觉垂直翻转（上下倒影）：无论当前旋转到了 0°/90°/180°/270°，始终沿屏幕水平中轴上下翻面
    pub fn toggle_flip_vertical(&mut self) {
        if self.rotation.swaps_axes() {
            self.flip_x = !self.flip_x;
        } else {
            self.flip_y = !self.flip_y;
        }
    }

    /// 当前屏幕空间是否处于水平镜像激活态
    pub fn is_screen_flipped_h(&self) -> bool {
        if self.rotation.swaps_axes() {
            self.flip_y
        } else {
            self.flip_x
        }
    }

    /// 当前屏幕空间是否处于垂直镜像激活态
    pub fn is_screen_flipped_v(&self) -> bool {
        if self.rotation.swaps_axes() {
            self.flip_x
        } else {
            self.flip_y
        }
    }

    /// 长截图阅读利器（W 键）：一键横向适应安全阅读宽度并置顶对齐
    pub fn fit_to_width(&mut self, img_w: f32, img_h: f32, view_w: f32, view_h: f32) {
        if img_w <= 0.0 || img_h <= 0.0 || view_w <= 0.0 || view_h <= 0.0 {
            return;
        }
        self.vel_x = 0.0;
        self.vel_y = 0.0;
        self.free_drag_pan = false;
        self.cursor_zoom_free_pan = false;
        self.slideshow_transition_damping = false;
        let (eff_w, eff_h) = self.effective_dims(img_w, img_h);
        let avail_w_phys = (view_w * 0.90).max(120.0) * self.dpi_scale.max(0.5);
        let target_z = (avail_w_phys / eff_w).min(2.0).clamp(MIN_ZOOM, MAX_ZOOM);
        self.target_zoom = target_z;
        self.target_pan_x = 0.0;
        let scaled_h_dip = eff_h * self.dip_per_image_pixel(target_z);
        if scaled_h_dip > view_h {
            self.target_pan_y = (scaled_h_dip - view_h) * 0.5;
        } else {
            self.target_pan_y = 0.0;
        }
        self.is_fit_mode = false;
        self.clamp_target_pan(img_w, img_h, view_w, view_h);
    }

    /// 全倍率光标像素锁定数学公式（True Cursor-Anchored Logarithmic Zoom）：
    /// - 放大 (`wheel_steps > 0`)：无论图片当前是小于屏幕的 Fit 态、竖图、自由拖拽后的位置还是大图，
    ///   均严格锁定鼠标指针正下方的图像像素点零漂移，仅用宽松可见安全包络防止图像滑出视野；
    /// - 缩小 (`wheel_steps < 0`)：若用户通过自由拖拽放置过照片 (`free_drag_pan`)，缩小同样保持光标像素锚点零漂移；
    ///   否则大于 Fit 倍率时沿光标平滑回缩，到达 Fit 倍率时精准归回屏幕正中。
    pub fn zoom_at_point(
        &mut self,
        wheel_steps: f32,
        cursor_x: f32,
        cursor_y: f32,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
    ) {
        let old_zoom = self.target_zoom.max(MIN_ZOOM);
        let factor = ZOOM_STEP_FACTOR.powf(wheel_steps);
        let new_zoom = (old_zoom * factor).clamp(MIN_ZOOM, MAX_ZOOM);
        if (new_zoom - old_zoom).abs() < 1e-5 {
            return;
        }

        self.vel_x = 0.0;
        self.vel_y = 0.0;
        self.slideshow_transition_damping = false;

        let mode = self.fit_layout_mode;
        let fit = Self::compute_fit_zoom_for_mode(
            img_w,
            img_h,
            self.rotation,
            view_w,
            view_h,
            self.dpi_scale,
            mode,
        );
        let fit_py = Self::default_fit_pan_y(
            img_w,
            img_h,
            self.rotation,
            fit,
            view_h,
            self.dpi_scale,
            mode,
        );

        let cx = view_w * 0.5;
        let cy = view_h * 0.5;
        let rel_x = cursor_x - cx - self.target_pan_x;
        let rel_y = cursor_y - cy - self.target_pan_y;
        let ratio = new_zoom / old_zoom;
        let cand_pan_x = cursor_x - cx - rel_x * ratio;
        let cand_pan_y = cursor_y - cy - rel_y * ratio;

        self.target_zoom = new_zoom;
        self.is_fit_mode = false;

        if new_zoom > old_zoom || self.free_drag_pan {
            // 放大或自由拖拽定格后继续滚轮缩放：100% 锁定鼠标光标下的像素点，仅用宽松可见包络防飞屏
            let (env_x, env_y) = self.cursor_zoom_envelope(img_w, img_h, view_w, view_h);
            self.target_pan_x = cand_pan_x.clamp(-env_x, env_x);
            self.target_pan_y = cand_pan_y.clamp(-env_y, env_y);
            self.cursor_zoom_free_pan = true;
        } else {
            // 缩小：缩回 Fit 及以下时平滑归中；在 Fit 以上缩小时，贴边区内严格锚定光标，
            // 超出贴边区的自由漂移余量随接近 Fit 的比例无缝收敛回正中
            if new_zoom <= fit * 1.005 {
                let small_py = Self::default_fit_pan_y(
                    img_w,
                    img_h,
                    self.rotation,
                    new_zoom,
                    view_h,
                    self.dpi_scale,
                    mode,
                );
                self.target_pan_x = 0.0;
                self.target_pan_y = small_py;
                self.cursor_zoom_free_pan = false;
            } else {
                let (max_c_x, max_c_y) = self.max_pan_bounds(img_w, img_h, view_w, view_h);
                let (env_x, env_y) = self.cursor_zoom_envelope(img_w, img_h, view_w, view_h);
                let pull = ((new_zoom - fit) / (old_zoom - fit).max(1e-4)).clamp(0.0, 1.0);

                let base_x = cand_pan_x.clamp(-max_c_x, max_c_x);
                let next_x = base_x + (cand_pan_x - base_x) * pull;

                let (min_cy, max_cy) = if max_c_y <= 0.0 {
                    (fit_py, fit_py)
                } else {
                    (-max_c_y, max_c_y)
                };
                let base_y = cand_pan_y.clamp(min_cy, max_cy);
                let next_y = base_y + (cand_pan_y - base_y) * pull;

                self.target_pan_x = next_x.clamp(-env_x, env_x);
                self.target_pan_y = next_y.clamp(-env_y, env_y);
                let still_free_x = (self.target_pan_x - base_x).abs() > 0.5;
                let still_free_y = (self.target_pan_y - base_y).abs() > 0.5;
                self.cursor_zoom_free_pan = still_free_x || still_free_y;
            }
        }
    }

    /// 双击或点 1:1 / 0 键：若当前处于放大或自由拖拽偏离居中态则平滑恢复居中 Fit，否则以双击点为锚点放大至 1:1 物理像素
    pub fn toggle_actual_or_fit(
        &mut self,
        anchor_x: f32,
        anchor_y: f32,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
    ) {
        let mode = self.fit_layout_mode;
        let fit = Self::compute_fit_zoom_for_mode(
            img_w,
            img_h,
            self.rotation,
            view_w,
            view_h,
            self.dpi_scale,
            mode,
        );
        let fit_py = Self::default_fit_pan_y(
            img_w,
            img_h,
            self.rotation,
            fit,
            view_h,
            self.dpi_scale,
            mode,
        );
        let is_off_center_free_drag = self.free_drag_pan
            && (self.target_pan_x.abs() > 2.0 || (self.target_pan_y - fit_py).abs() > 2.0);
        if !is_off_center_free_drag && (self.is_fit_mode || (self.target_zoom - fit).abs() < 0.02) {
            let target = if fit < 0.95 { 1.0 } else { 2.0 };
            let steps = (target / self.target_zoom.max(MIN_ZOOM)).ln() / ZOOM_STEP_FACTOR.ln();
            self.zoom_at_point(steps, anchor_x, anchor_y, img_w, img_h, view_w, view_h);
        } else {
            self.vel_x = 0.0;
            self.vel_y = 0.0;
            self.slideshow_transition_damping = false;
            self.target_zoom = fit;
            self.target_pan_x = 0.0;
            self.target_pan_y = fit_py;
            self.is_fit_mode = true;
            self.free_drag_pan = false;
            self.cursor_zoom_free_pan = false;
        }
    }

    /// Ctrl + 左键框选局部区域精准放大并丝滑居中特写
    pub fn zoom_to_screen_rect(
        &mut self,
        sel: RectF,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
    ) {
        let s = sel.normalized();
        if s.width() < 16.0 || s.height() < 16.0 {
            return;
        }
        let img_rc = self.image_screen_rect(img_w, img_h, view_w, view_h);
        if img_rc.width() < 1.0 || img_rc.height() < 1.0 {
            return;
        }
        self.vel_x = 0.0;
        self.vel_y = 0.0;
        self.free_drag_pan = false;
        self.cursor_zoom_free_pan = false;
        self.slideshow_transition_damping = false;
        let u = ((s.center_x() - img_rc.left) / img_rc.width()).clamp(0.0, 1.0);
        let v = ((s.center_y() - img_rc.top) / img_rc.height()).clamp(0.0, 1.0);
        let factor = ((view_w * 0.86) / s.width())
            .min((view_h * 0.84) / s.height())
            .max(1.0);
        let next_zoom = (self.zoom * factor).clamp(MIN_ZOOM, MAX_ZOOM);
        self.target_zoom = next_zoom;
        let (eff_w, eff_h) = self.effective_dims(img_w, img_h);
        let dip_per_px = self.dip_per_image_pixel(next_zoom);
        self.target_pan_x = (0.5 - u) * eff_w * dip_per_px;
        self.target_pan_y = (0.5 - v) * eff_h * dip_per_px;
        self.is_fit_mode = false;
        self.clamp_target_pan(img_w, img_h, view_w, view_h);
    }

    /// 开始左键拖拽平移：冻结当前缩放与位置并清零残余惯性，防止阻尼缩放中途拖拽导致边界错位
    pub fn begin_drag(&mut self) {
        self.is_dragging = true;
        self.target_zoom = self.zoom;
        self.target_pan_x = self.pan_x;
        self.target_pan_y = self.pan_y;
        self.vel_x = 0.0;
        self.vel_y = 0.0;
        self.slideshow_transition_damping = false;
        self.raw_drag_pan_x = self.pan_x;
        self.raw_drag_pan_y = self.pan_y;
    }

    /// 拖拽实时跟手平移 + 速度向量指数采样 + 越界橡皮筋弹性阻尼
    /// - 浮层态 (`Overlay`) 下即使图片未放大 (`scaled <= view`) 也允许 1:1 自由拖拽至视口任意位置，
    ///   仅以宽松防丢失包络 (`min(scaled, view, 120.0)` DIP 留边) 柔和限位，松手即停留在放置处；
    /// - 放大超过视口 (`scaled > view`) 的轴沿用经典贴边边界 `max_pan_bounds`。
    #[allow(clippy::too_many_arguments)]
    pub fn pan_by_drag(
        &mut self,
        dx: f32,
        dy: f32,
        dt_sec: f32,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
    ) {
        if !self.is_dragging {
            self.begin_drag();
        }
        self.raw_drag_pan_x += dx;
        self.raw_drag_pan_y += dy;

        let (max_c_x, max_c_y) = self.max_pan_bounds(img_w, img_h, view_w, view_h);
        let allow_free_drag = self.fit_layout_mode == FitLayoutMode::Overlay;
        if allow_free_drag && (max_c_x <= 0.5 || max_c_y <= 0.5) {
            self.free_drag_pan = true;
            // 拖拽接管自由位置后清除纯缩放自由标记，让松手边界严格遵循自由拖拽规则
            self.cursor_zoom_free_pan = false;
        } else if max_c_x > 0.5 && max_c_y > 0.5 {
            self.free_drag_pan = false;
            self.cursor_zoom_free_pan = false;
        }

        let (eff_max_x, eff_max_y) = if self.free_drag_pan {
            let (env_x, env_y) = self.cursor_zoom_envelope(img_w, img_h, view_w, view_h);
            (
                if max_c_x > 0.5 { max_c_x } else { env_x },
                if max_c_y > 0.5 { max_c_y } else { env_y },
            )
        } else if self.cursor_zoom_free_pan && !self.is_fit_mode {
            let (inner_x, inner_y) = self.inner_viewport_pan_bounds(img_w, img_h, view_w, view_h);
            (
                if max_c_x > 0.5 { max_c_x } else { inner_x },
                if max_c_y > 0.5 { max_c_y } else { inner_y },
            )
        } else {
            (max_c_x, max_c_y)
        };

        let eff_x = apply_rubber_band_clamp(self.raw_drag_pan_x, eff_max_x, RUBBER_BAND_MAX_DIP);
        let eff_y = apply_rubber_band_clamp(self.raw_drag_pan_y, eff_max_y, RUBBER_BAND_MAX_DIP);

        let safe_dt = dt_sec.clamp(0.002, 0.05);
        let inst_vx = ((eff_x - self.pan_x) / safe_dt).clamp(-4800.0, 4800.0);
        let inst_vy = ((eff_y - self.pan_y) / safe_dt).clamp(-4800.0, 4800.0);
        self.vel_x = self.vel_x * 0.35 + inst_vx * 0.65;
        self.vel_y = self.vel_y * 0.35 + inst_vy * 0.65;

        self.pan_x = eff_x;
        self.pan_y = eff_y;
        self.target_pan_x = eff_x;
        self.target_pan_y = eff_y;
        if eff_max_x > 0.5 || eff_max_y > 0.5 {
            self.is_fit_mode = false;
        }
    }

    /// 结束左键拖拽：若松手瞬间仍在快速滑动则释放惯性滑行，若已停顿则仅触发越界弹簧回弹
    pub fn end_drag(&mut self, pointer_stalled: bool) {
        self.is_dragging = false;
        if pointer_stalled || self.vel_x.hypot(self.vel_y) < 45.0 {
            self.vel_x = 0.0;
            self.vel_y = 0.0;
        }
    }

    /// 点击或拖拽右下角迷你导航框，将指定归一化图像位置 (u, v in [0,1]) 居中
    pub fn focus_normalized_point(
        &mut self,
        norm_x: f32,
        norm_y: f32,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
    ) {
        self.vel_x = 0.0;
        self.vel_y = 0.0;
        self.free_drag_pan = false;
        self.cursor_zoom_free_pan = false;
        self.slideshow_transition_damping = false;
        let (eff_w, eff_h) = self.effective_dims(img_w, img_h);
        let dip_per_px = self.dip_per_image_pixel(self.target_zoom);
        let scaled_w = eff_w * dip_per_px;
        let scaled_h = eff_h * dip_per_px;
        let u = norm_x.clamp(0.0, 1.0);
        let v = norm_y.clamp(0.0, 1.0);
        self.target_pan_x = (0.5 - u) * scaled_w;
        self.target_pan_y = (0.5 - v) * scaled_h;
        self.clamp_target_pan(img_w, img_h, view_w, view_h);
        self.pan_x = self.target_pan_x;
        self.pan_y = self.target_pan_y;
        self.is_fit_mode = false;
    }

    pub fn max_pan_bounds(&self, img_w: f32, img_h: f32, view_w: f32, view_h: f32) -> (f32, f32) {
        let (eff_w, eff_h) = self.effective_dims(img_w, img_h);
        let dip_per_px = self.dip_per_image_pixel(self.target_zoom);
        let scaled_w = eff_w * dip_per_px;
        let scaled_h = eff_h * dip_per_px;
        let max_pan_x = ((scaled_w - view_w) * 0.5).max(0.0);
        let max_pan_y = ((scaled_h - view_h) * 0.5).max(0.0);
        (max_pan_x, max_pan_y)
    }

    /// 图像小于视口时的内切安全平移范围（保证照片边缘不越出视口边界）
    fn inner_viewport_pan_bounds(
        &self,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
    ) -> (f32, f32) {
        let (eff_w, eff_h) = self.effective_dims(img_w, img_h);
        let dip_per_px = self.dip_per_image_pixel(self.target_zoom);
        let scaled_w = eff_w * dip_per_px;
        let scaled_h = eff_h * dip_per_px;
        (
            ((view_w - scaled_w) * 0.5).max(0.0),
            ((view_h - scaled_h) * 0.5).max(0.0),
        )
    }

    /// 光标锚点缩放宽松可见安全包络：任意倍率下保证图像至少保留 120 DIP（或自身全宽）留在屏幕可见区内，绝不跑飞
    pub fn cursor_zoom_envelope(
        &self,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
    ) -> (f32, f32) {
        let (eff_w, eff_h) = self.effective_dims(img_w, img_h);
        let dip_per_px = self.dip_per_image_pixel(self.target_zoom);
        let scaled_w = eff_w * dip_per_px;
        let scaled_h = eff_h * dip_per_px;
        let keep_x = scaled_w.min(view_w).min(120.0);
        let keep_y = scaled_h.min(view_h).min(120.0);
        let env_x = ((scaled_w + view_w) * 0.5 - keep_x).max(0.0);
        let env_y = ((scaled_h + view_h) * 0.5 - keep_y).max(0.0);
        (env_x, env_y)
    }

    /// 限制目标平移边界：光标自由锚定态与自由拖拽态使用防飞屏可见包络；常规态/Fit 态归中或贴边
    pub fn clamp_target_pan(&mut self, img_w: f32, img_h: f32, view_w: f32, view_h: f32) {
        if self.cursor_zoom_free_pan && !self.is_fit_mode {
            let (env_x, env_y) = self.cursor_zoom_envelope(img_w, img_h, view_w, view_h);
            self.target_pan_x = self.target_pan_x.clamp(-env_x, env_x);
            self.target_pan_y = self.target_pan_y.clamp(-env_y, env_y);
            return;
        }
        let (max_pan_x, max_pan_y) = self.max_pan_bounds(img_w, img_h, view_w, view_h);
        if self.free_drag_pan && !self.is_fit_mode {
            let (env_x, env_y) = self.cursor_zoom_envelope(img_w, img_h, view_w, view_h);
            let bx = if max_pan_x > 0.5 { max_pan_x } else { env_x };
            let by = if max_pan_y > 0.5 { max_pan_y } else { env_y };
            self.target_pan_x = self.target_pan_x.clamp(-bx, bx);
            self.target_pan_y = self.target_pan_y.clamp(-by, by);
            return;
        }
        self.target_pan_x = self.target_pan_x.clamp(-max_pan_x, max_pan_x);
        if max_pan_y <= 0.0 && self.is_fit_mode {
            let py = Self::default_fit_pan_y(
                img_w,
                img_h,
                self.rotation,
                self.target_zoom,
                view_h,
                self.dpi_scale,
                self.fit_layout_mode,
            );
            self.target_pan_y = py;
        } else {
            self.target_pan_y = self.target_pan_y.clamp(-max_pan_y, max_pan_y);
        }
    }

    /// 推进帧率无关物理动画（含阻尼缩放、惯性甩动滑行、橡皮筋回弹、90° 连续旋转与 3D 镜像翻转）
    pub fn step_animation(
        &mut self,
        dt_sec: f32,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
    ) -> bool {
        let dt = dt_sec.clamp(0.001, 0.05);

        // 1. 松手后的惯性衰减滑行与越界弹簧回弹 (Rubber-banding)
        if !self.is_dragging && img_w > 0.0 && img_h > 0.0 {
            let has_kinetic_vel = self.vel_x.abs() > 2.0 || self.vel_y.abs() > 2.0;
            let (max_x, mut min_y, mut max_y) = if self.cursor_zoom_free_pan && !self.is_fit_mode {
                if has_kinetic_vel {
                    let (mx, my) = self.max_pan_bounds(img_w, img_h, view_w, view_h);
                    let (ix, iy) = if self.free_drag_pan {
                        self.cursor_zoom_envelope(img_w, img_h, view_w, view_h)
                    } else {
                        self.inner_viewport_pan_bounds(img_w, img_h, view_w, view_h)
                    };
                    let bx = if mx > 0.5 { mx } else { ix };
                    let by = if my > 0.5 { my } else { iy };
                    (bx, -by, by)
                } else {
                    // 光标锚点缩放收敛期间绝不偷拉回中，仅用防飞屏安全包络保护
                    let (ex, ey) = self.cursor_zoom_envelope(img_w, img_h, view_w, view_h);
                    (ex, -ey, ey)
                }
            } else if self.free_drag_pan && !self.is_fit_mode {
                // 浮层态自由拖拽：未超视口轴停留在用户放置位置，仅越过防丢失包络时柔和橡皮筋回弹
                let (mx, my) = self.max_pan_bounds(img_w, img_h, view_w, view_h);
                let (ex, ey) = self.cursor_zoom_envelope(img_w, img_h, view_w, view_h);
                let bx = if mx > 0.5 { mx } else { ex };
                let by = if my > 0.5 { my } else { ey };
                (bx, -by, by)
            } else {
                let (mx, my) = self.max_pan_bounds(img_w, img_h, view_w, view_h);
                if my <= 0.0 && self.is_fit_mode {
                    let py = Self::default_fit_pan_y(
                        img_w,
                        img_h,
                        self.rotation,
                        self.target_zoom,
                        view_h,
                        self.dpi_scale,
                        self.fit_layout_mode,
                    );
                    (mx, py, py)
                } else {
                    (mx, -my, my)
                }
            };
            if min_y > max_y {
                std::mem::swap(&mut min_y, &mut max_y);
            }

            if has_kinetic_vel {
                self.target_pan_x += self.vel_x * dt;
                self.target_pan_y += self.vel_y * dt;

                let over_x = self.target_pan_x < -max_x || self.target_pan_x > max_x;
                let over_y = self.target_pan_y < min_y || self.target_pan_y > max_y;
                let friction_x = if over_x {
                    KINETIC_FRICTION_DAMPING * 3.8
                } else {
                    KINETIC_FRICTION_DAMPING
                };
                let friction_y = if over_y {
                    KINETIC_FRICTION_DAMPING * 3.8
                } else {
                    KINETIC_FRICTION_DAMPING
                };
                self.vel_x *= (-friction_x * dt).exp();
                self.vel_y *= (-friction_y * dt).exp();
                if self.vel_x.abs() <= 2.0 {
                    self.vel_x = 0.0;
                }
                if self.vel_y.abs() <= 2.0 {
                    self.vel_y = 0.0;
                }
            }

            // 弹簧回弹至合法视口边界
            let spring_f = exp_decay_factor(RUBBER_BAND_SPRING_DAMPING, dt);
            if self.target_pan_x < -max_x {
                self.target_pan_x += (-max_x - self.target_pan_x) * spring_f;
            } else if self.target_pan_x > max_x {
                self.target_pan_x += (max_x - self.target_pan_x) * spring_f;
            }
            if self.target_pan_y < min_y {
                self.target_pan_y += (min_y - self.target_pan_y) * spring_f;
            } else if self.target_pan_y > max_y {
                self.target_pan_y += (max_y - self.target_pan_y) * spring_f;
            }
        }

        // 2. 对数感知等速无级阻尼缩放 + 精确共轴平移 + 90° 连续旋转与 3D 镜像翻转
        // 关键物理性质：
        // - 在对数空间插值 ln(zoom)，使人眼感知的缩放速率从 50% -> 200% -> 800% 绝对均匀丝滑（消除线性前冲后慢）；
        // - pan 使用精确几何同步比例 alpha = (zoom_next - zoom) / (target_zoom - zoom)，
        //   保证整个 120Hz/240Hz 无级丝滑缩放动画的每一中间帧，鼠标指针正下方的图像像素严格 0.00px 漂移！
        let zoom_damping = if self.slideshow_transition_damping {
            SLIDESHOW_ZOOM_ANIM_DAMPING
        } else {
            ZOOM_ANIM_DAMPING
        };
        let zoom_f = exp_decay_factor(zoom_damping, dt);
        let zoom_diff = self.target_zoom - self.zoom;
        if zoom_diff.abs() > 1e-5 {
            let cur_ln = self.zoom.max(MIN_ZOOM).ln();
            let tgt_ln = self.target_zoom.max(MIN_ZOOM).ln();
            let next_zoom = (cur_ln + (tgt_ln - cur_ln) * zoom_f).exp();
            if !self.is_dragging {
                let alpha = ((next_zoom - self.zoom) / zoom_diff).clamp(0.0, 1.0);
                self.pan_x += (self.target_pan_x - self.pan_x) * alpha;
                self.pan_y += (self.target_pan_y - self.pan_y) * alpha;
            }
            self.zoom = next_zoom;
        } else {
            self.zoom = self.target_zoom;
            if !self.is_dragging {
                self.pan_x = exp_smooth(self.pan_x, self.target_pan_x, zoom_damping, dt);
                self.pan_y = exp_smooth(self.pan_y, self.target_pan_y, zoom_damping, dt);
            }
        }
        self.visual_rotation_deg = exp_smooth(
            self.visual_rotation_deg,
            self.target_rotation_deg,
            ROTATION_ANIM_DAMPING,
            dt,
        );

        let target_fx = if self.flip_x { -1.0 } else { 1.0 };
        let target_fy = if self.flip_y { -1.0 } else { 1.0 };
        self.visual_flip_x = exp_smooth(self.visual_flip_x, target_fx, FLIP_ANIM_DAMPING, dt);
        self.visual_flip_y = exp_smooth(self.visual_flip_y, target_fy, FLIP_ANIM_DAMPING, dt);

        let dz = (self.target_zoom - self.zoom).abs();
        let dx = (self.target_pan_x - self.pan_x).abs();
        let dy = (self.target_pan_y - self.pan_y).abs();
        let dr = (self.target_rotation_deg - self.visual_rotation_deg).abs();
        let dfx = (target_fx - self.visual_flip_x).abs();
        let dfy = (target_fy - self.visual_flip_y).abs();
        let has_vel = self.vel_x.abs() > 2.0 || self.vel_y.abs() > 2.0;

        if !has_vel && dz < 0.0005 && dx < 0.16 && dy < 0.16 && dr < 0.10 && dfx < 0.004 && dfy < 0.004 {
            self.zoom = self.target_zoom;
            if !self.is_dragging {
                self.pan_x = self.target_pan_x;
                self.pan_y = self.target_pan_y;
            }
            self.visual_rotation_deg = self.target_rotation_deg;
            self.visual_flip_x = target_fx;
            self.visual_flip_y = target_fy;
            self.slideshow_transition_damping = false;
            return false;
        }
        true
    }

    /// 当前相机是否正处于缩放/平移/甩动/旋转运动期（运动期启用硬件单周期 LINEAR 插值跑满 120~240Hz，静止立刻切回高质量立方锐化）
    pub fn is_camera_animating(&self) -> bool {
        self.is_dragging
            || (self.target_zoom - self.zoom).abs() > 0.0012
            || (self.target_pan_x - self.pan_x).abs() > 0.35
            || (self.target_pan_y - self.pan_y).abs() > 0.35
            || self.vel_x.abs() > 2.0
            || self.vel_y.abs() > 2.0
            || self.is_transform_animating()
    }

    pub fn is_transform_animating(&self) -> bool {
        let target_fx = if self.flip_x { -1.0 } else { 1.0 };
        let target_fy = if self.flip_y { -1.0 } else { 1.0 };
        (self.target_rotation_deg - self.visual_rotation_deg).abs() > 0.15
            || (target_fx - self.visual_flip_x).abs() > 0.01
            || (target_fy - self.visual_flip_y).abs() > 0.01
    }

    pub fn effective_dims(&self, img_w: f32, img_h: f32) -> (f32, f32) {
        if self.rotation.swaps_axes() {
            (img_h, img_w)
        } else {
            (img_w, img_h)
        }
    }

    /// 计算当前绘制在视口中的外接逻辑 DIP 矩形（含缩放、DPI 与平移）
    pub fn image_screen_rect(&self, img_w: f32, img_h: f32, view_w: f32, view_h: f32) -> RectF {
        let (eff_w, eff_h) = self.effective_dims(img_w, img_h);
        let dip_per_px = self.dip_per_image_pixel(self.zoom);
        let half_w = eff_w * dip_per_px * 0.5;
        let half_h = eff_h * dip_per_px * 0.5;
        let cx = view_w * 0.5 + self.pan_x;
        let cy = view_h * 0.5 + self.pan_y;
        RectF::new(cx - half_w, cy - half_h, cx + half_w, cy + half_h)
    }

    /// 是否超出屏幕需要展示右下角 Picasa 迷你导航框
    pub fn should_show_minimap(&self, img_w: f32, img_h: f32, view_w: f32, view_h: f32) -> bool {
        let rc = self.image_screen_rect(img_w, img_h, view_w, view_h);
        rc.width() > view_w + 8.0 || rc.height() > view_h + 8.0
    }

    /// 计算可见视口在全图中的归一化窗口矩形 [0..1]
    pub fn normalized_visible_window(
        &self,
        img_w: f32,
        img_h: f32,
        view_w: f32,
        view_h: f32,
    ) -> RectF {
        let rc = self.image_screen_rect(img_w, img_h, view_w, view_h);
        let w = rc.width().max(1.0);
        let h = rc.height().max(1.0);
        let u0 = ((0.0 - rc.left) / w).clamp(0.0, 1.0);
        let v0 = ((0.0 - rc.top) / h).clamp(0.0, 1.0);
        let u1 = ((view_w - rc.left) / w).clamp(0.0, 1.0);
        let v1 = ((view_h - rc.top) / h).clamp(0.0, 1.0);
        RectF::new(u0, v0, u1, v1)
    }
}

fn apply_rubber_band_clamp(raw: f32, max_bound: f32, rubber_limit: f32) -> f32 {
    if raw < -max_bound {
        -max_bound + rubber_band_offset(raw + max_bound, rubber_limit)
    } else if raw > max_bound {
        max_bound + rubber_band_offset(raw - max_bound, rubber_limit)
    } else {
        raw
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn small_image_stays_sharp_at_100_percent() {
        let fit = ViewportCamera::compute_fit_zoom(
            400.0,
            300.0,
            RotationQuarter::Deg0,
            1920.0,
            1080.0,
            1.0,
        );
        assert!((fit - 1.0).abs() < 1e-4);
    }

    #[test]
    fn large_4k_image_fits_inside_1080p_with_margin() {
        let fit = ViewportCamera::compute_fit_zoom(
            3840.0,
            2160.0,
            RotationQuarter::Deg0,
            1920.0,
            1080.0,
            1.0,
        );
        assert!(fit > 0.35 && fit < 0.46);
    }

    #[test]
    fn high_dpi_200_percent_renders_1to1_physical_pixels() {
        let mut cam = ViewportCamera::default();
        cam.set_dpi_scale(2.0);
        cam.reset_to_fit(1200.0, 800.0, 1920.0, 1080.0);
        assert!((cam.zoom - 1.0).abs() < 1e-4);
        let rc = cam.image_screen_rect(1200.0, 800.0, 1920.0, 1080.0);
        assert!((rc.width() - 600.0).abs() < 0.5);
        assert!((rc.height() - 400.0).abs() < 0.5);

        cam.reset_to_fit(3840.0, 2160.0, 1920.0, 1080.0);
        assert!(cam.zoom > 0.80 && cam.zoom < 0.96);
    }

    #[test]
    fn continuous_rotation_and_screen_flip_work_seamlessly() {
        let mut cam = ViewportCamera::default();
        cam.reset_to_fit(1600.0, 900.0, 1920.0, 1080.0);
        for _ in 0..4 {
            cam.rotate_step(true, 1600.0, 900.0, 1920.0, 1080.0, FitLayoutMode::Overlay);
        }
        assert_eq!(cam.rotation, RotationQuarter::Deg0);
        assert!((cam.target_rotation_deg - 360.0).abs() < 1e-3);

        // 旋转 90° 后点击水平翻转应映射为局部 Y 翻转，保证屏幕视觉左右翻面
        cam.rotate_step(true, 1600.0, 900.0, 1920.0, 1080.0, FitLayoutMode::Overlay);
        assert_eq!(cam.rotation, RotationQuarter::Deg90);
        cam.toggle_flip_horizontal();
        assert!(cam.is_screen_flipped_h());
        assert!(!cam.is_screen_flipped_v());
        assert!(cam.flip_y);

        for _ in 0..60 {
            cam.step_animation(0.016, 1600.0, 900.0, 1920.0, 1080.0);
        }
        assert!((cam.visual_flip_y - (-1.0)).abs() < 0.02);
    }

    #[test]
    fn kinetic_pan_and_rubber_band_settle_inside_bounds() {
        let mut cam = ViewportCamera::default();
        cam.reset_to_fit(4000.0, 3000.0, 1280.0, 720.0);
        cam.zoom_at_point(6.0, 640.0, 360.0, 4000.0, 3000.0, 1280.0, 720.0);
        cam.zoom = cam.target_zoom;
        cam.begin_drag();
        for _ in 0..8 {
            cam.pan_by_drag(220.0, 0.0, 0.016, 4000.0, 3000.0, 1280.0, 720.0);
        }
        cam.end_drag(false);
        let (max_x, _) = cam.max_pan_bounds(4000.0, 3000.0, 1280.0, 720.0);
        // 拖拽越界瞬间存在橡皮筋弹性余量
        assert!(cam.pan_x > max_x);
        // 物理推进收敛后自动回弹贴齐合法边界
        for _ in 0..90 {
            cam.step_animation(0.016, 4000.0, 3000.0, 1280.0, 720.0);
        }
        assert!((cam.pan_x - max_x).abs() < 0.5);
    }

    #[test]
    fn cursor_anchor_zoom_preserves_image_point_under_cursor() {
        let mut cam = ViewportCamera::default();
        cam.set_dpi_scale(1.5);
        cam.reset_to_fit(4000.0, 3000.0, 1600.0, 900.0);
        cam.zoom_at_point(8.0, 800.0, 450.0, 4000.0, 3000.0, 1600.0, 900.0);
        cam.zoom = cam.target_zoom;
        cam.pan_x = cam.target_pan_x;
        cam.pan_y = cam.target_pan_y;

        let cursor_x = 920.0;
        let cursor_y = 510.0;
        let before_img_x =
            (cursor_x - 800.0 - cam.target_pan_x) / cam.dip_per_image_pixel(cam.target_zoom);
        let before_img_y =
            (cursor_y - 450.0 - cam.target_pan_y) / cam.dip_per_image_pixel(cam.target_zoom);

        cam.zoom_at_point(2.0, cursor_x, cursor_y, 4000.0, 3000.0, 1600.0, 900.0);
        let after_img_x =
            (cursor_x - 800.0 - cam.target_pan_x) / cam.dip_per_image_pixel(cam.target_zoom);
        let after_img_y =
            (cursor_y - 450.0 - cam.target_pan_y) / cam.dip_per_image_pixel(cam.target_zoom);

        assert!((before_img_x - after_img_x).abs() < 0.5);
        assert!((before_img_y - after_img_y).abs() < 0.5);
        assert!(cam.should_show_minimap(4000.0, 3000.0, 1600.0, 900.0));
    }

    #[test]
    fn cursor_anchor_zoom_works_from_first_wheel_step_even_when_smaller_than_screen() {
        // 复现并永久防回退用户反馈场景：
        // 竖图在宽屏下初次打开远小于屏幕宽度 (scaled_w < view_w)，鼠标放在左下角执行滚轮放大，
        // 第一格滚轮开始就必须严格以鼠标光标下的图像像素为锚点缩放，且跨越满屏临界点零跳变！
        for dpi_scale in [1.0f32, 1.5, 2.0] {
            let mut cam = ViewportCamera::default();
            cam.set_dpi_scale(dpi_scale);
            let img_w = 1200.0;
            let img_h = 1600.0;
            let view_w = 1920.0;
            let view_h = 1080.0;
            cam.reset_to_fit(img_w, img_h, view_w, view_h);

            let rc0 = cam.image_screen_rect(img_w, img_h, view_w, view_h);
            assert!(rc0.width() < view_w * 0.6, "初始应为明显小于屏宽的竖图");

            // 鼠标指向照片偏右下角 (u=0.82, v=0.78)
            let cursor_x = rc0.left + rc0.width() * 0.82;
            let cursor_y = rc0.top + rc0.height() * 0.78;
            let initial_img_x = (cursor_x - view_w * 0.5 - cam.target_pan_x)
                / cam.dip_per_image_pixel(cam.target_zoom);
            let initial_img_y = (cursor_y - view_h * 0.5 - cam.target_pan_y)
                / cam.dip_per_image_pixel(cam.target_zoom);

            // 连续向上滚轮 14 格，涵盖「小于屏宽 -> 跨越满屏临界 -> 大于屏宽」完整过程
            for step in 1..=14 {
                cam.zoom_at_point(1.0, cursor_x, cursor_y, img_w, img_h, view_w, view_h);
                // 推进若干动画帧，确认 step_animation 既不把图片偷拉回中，中间帧也不发生锚点漂移
                for _ in 0..8 {
                    cam.step_animation(0.016, img_w, img_h, view_w, view_h);
                    let anim_img_x = (cursor_x - view_w * 0.5 - cam.pan_x)
                        / cam.dip_per_image_pixel(cam.zoom);
                    let anim_img_y = (cursor_y - view_h * 0.5 - cam.pan_y)
                        / cam.dip_per_image_pixel(cam.zoom);
                    assert!(
                        (anim_img_x - initial_img_x).abs() < 0.75
                            && (anim_img_y - initial_img_y).abs() < 0.75,
                        "dpi={dpi_scale} step={step}: 动画中间帧光标像素漂移 ({anim_img_x},{anim_img_y}) vs ({initial_img_x},{initial_img_y})"
                    );
                }
                let after_img_x = (cursor_x - view_w * 0.5 - cam.target_pan_x)
                    / cam.dip_per_image_pixel(cam.target_zoom);
                let after_img_y = (cursor_y - view_h * 0.5 - cam.target_pan_y)
                    / cam.dip_per_image_pixel(cam.target_zoom);
                assert!(
                    (after_img_x - initial_img_x).abs() < 0.5
                        && (after_img_y - initial_img_y).abs() < 0.5,
                    "dpi={dpi_scale} step={step}: 目标光标像素漂移 ({after_img_x},{after_img_y}) vs ({initial_img_x},{initial_img_y})"
                );
            }
            // 第 1 格以后就应当有明显的反向位移，而非被清零归中
            assert!(cam.target_pan_x.abs() > 100.0);

            // 连续向下滚轮缩回 Fit，确认平滑归回屏幕正中而不歪在角落
            for _ in 0..16 {
                cam.zoom_at_point(-1.0, cursor_x, cursor_y, img_w, img_h, view_w, view_h);
            }
            for _ in 0..90 {
                cam.step_animation(0.016, img_w, img_h, view_w, view_h);
            }
            assert!(
                cam.pan_x.abs() < 0.5,
                "dpi={dpi_scale}: 缩回 Fit 以下应平滑居中，实际 pan_x={}",
                cam.pan_x
            );
        }
    }

    #[test]
    fn begin_drag_freezes_inflight_zoom_target_preventing_bounds_mismatch() {
        let mut cam = ViewportCamera::default();
        cam.reset_to_fit(4000.0, 3000.0, 1280.0, 720.0);
        cam.zoom_at_point(8.0, 640.0, 360.0, 4000.0, 3000.0, 1280.0, 720.0);
        // 仅推进 2 帧，zoom 远未追上 target_zoom
        cam.step_animation(0.016, 4000.0, 3000.0, 1280.0, 720.0);
        cam.step_animation(0.016, 4000.0, 3000.0, 1280.0, 720.0);
        assert!((cam.target_zoom - cam.zoom).abs() > 0.1);

        let frozen_zoom = cam.zoom;
        cam.begin_drag();
        assert!((cam.target_zoom - frozen_zoom).abs() < 1e-6);
        assert!((cam.target_pan_x - cam.pan_x).abs() < 1e-6);
        assert!((cam.target_pan_y - cam.pan_y).abs() < 1e-6);
    }

    #[test]
    fn overlay_free_drag_moves_unzoomed_photo_1to1_and_stays_where_dropped() {
        let mut cam = ViewportCamera::default();
        let (img_w, img_h) = (1024.0, 1024.0);
        let (view_w, view_h) = (1920.0, 1032.0);
        cam.reset_to_fit_with_mode(img_w, img_h, view_w, view_h, FitLayoutMode::Overlay);
        let start_x = cam.pan_x;
        let start_y = cam.pan_y;

        // 在未放大 Fit 态下拖动 (+240, -90) DIP，必须 1:1 跟手且松手后停留在放置处
        cam.begin_drag();
        cam.pan_by_drag(120.0, -45.0, 0.016, img_w, img_h, view_w, view_h);
        cam.pan_by_drag(120.0, -45.0, 0.016, img_w, img_h, view_w, view_h);
        cam.end_drag(true);

        assert!(cam.free_drag_pan);
        assert!((cam.pan_x - (start_x + 240.0)).abs() < 0.5);
        assert!((cam.pan_y - (start_y - 90.0)).abs() < 0.5);

        for _ in 0..90 {
            cam.step_animation(0.016, img_w, img_h, view_w, view_h);
        }
        // 收敛后绝不自动弹回正中
        assert!((cam.pan_x - (start_x + 240.0)).abs() < 1.0);
        assert!((cam.pan_y - (start_y - 90.0)).abs() < 1.0);

        // 在自由拖拽后的新位置继续滚轮缩放，鼠标光标下图像像素零漂移
        let rc = cam.image_screen_rect(img_w, img_h, view_w, view_h);
        let cursor_x = rc.left + rc.width() * 0.35;
        let cursor_y = rc.top + rc.height() * 0.65;
        let before_ix = (cursor_x - view_w * 0.5 - cam.target_pan_x)
            / cam.dip_per_image_pixel(cam.target_zoom);
        let before_iy = (cursor_y - view_h * 0.5 - cam.target_pan_y)
            / cam.dip_per_image_pixel(cam.target_zoom);
        cam.zoom_at_point(2.0, cursor_x, cursor_y, img_w, img_h, view_w, view_h);
        let after_ix = (cursor_x - view_w * 0.5 - cam.target_pan_x)
            / cam.dip_per_image_pixel(cam.target_zoom);
        let after_iy = (cursor_y - view_h * 0.5 - cam.target_pan_y)
            / cam.dip_per_image_pixel(cam.target_zoom);
        assert!((after_ix - before_ix).abs() < 0.5);
        assert!((after_iy - before_iy).abs() < 0.5);

        // 双击或按 0/1 恢复居中 Fit
        cam.toggle_actual_or_fit(cursor_x, cursor_y, img_w, img_h, view_w, view_h);
        for _ in 0..120 {
            cam.step_animation(0.016, img_w, img_h, view_w, view_h);
        }
        assert!(cam.is_fit_mode);
        assert!(!cam.free_drag_pan);
        assert!(cam.pan_x.abs() < 0.5);
    }

    #[test]
    fn overlay_free_drag_beyond_viewport_clamps_to_safe_visible_edge() {
        let mut cam = ViewportCamera::default();
        let (img_w, img_h) = (800.0, 600.0);
        let (view_w, view_h) = (1920.0, 1080.0);
        cam.reset_to_fit_with_mode(img_w, img_h, view_w, view_h, FitLayoutMode::Overlay);

        cam.begin_drag();
        for _ in 0..20 {
            cam.pan_by_drag(250.0, 0.0, 0.016, img_w, img_h, view_w, view_h);
        }
        cam.end_drag(false);
        for _ in 0..120 {
            cam.step_animation(0.016, img_w, img_h, view_w, view_h);
        }
        let rc = cam.image_screen_rect(img_w, img_h, view_w, view_h);
        let visible_width = (rc.right.min(view_w) - rc.left.max(0.0)).max(0.0);
        assert!(
            visible_width >= 119.0,
            "越界回弹后至少应保留 120 DIP 可抓边缘在视口内，实际={visible_width}"
        );
    }

    #[test]
    fn toggle_actual_or_fit_recenters_unzoomed_free_dragged_photo() {
        let mut cam = ViewportCamera::default();
        let (img_w, img_h) = (1024.0, 1024.0);
        let (view_w, view_h) = (1920.0, 1032.0);
        cam.reset_to_fit_with_mode(img_w, img_h, view_w, view_h, FitLayoutMode::Overlay);
        let fit_zoom = cam.zoom;

        // 仅自由拖动但不改变缩放倍率
        cam.begin_drag();
        cam.pan_by_drag(-320.0, 80.0, 0.016, img_w, img_h, view_w, view_h);
        cam.end_drag(true);
        assert!(cam.free_drag_pan);
        assert!((cam.target_zoom - fit_zoom).abs() < 1e-4);

        // 双击 / 按 0/1 应恢复居中 Fit 而非继续放大
        cam.toggle_actual_or_fit(600.0, 500.0, img_w, img_h, view_w, view_h);
        assert!(cam.is_fit_mode);
        assert!(!cam.free_drag_pan);
        assert!(cam.target_pan_x.abs() < 0.01);
        assert!((cam.target_zoom - fit_zoom).abs() < 1e-4);
    }

    #[test]
    fn windowed_fit_mode_is_compact_full_bleed_and_centered() {
        let mut cam = ViewportCamera::default();
        cam.reset_to_fit_with_mode(1600.0, 1200.0, 800.0, 600.0, FitLayoutMode::Windowed);
        let rc = cam.image_screen_rect(1600.0, 1200.0, 800.0, 600.0);
        assert!((rc.center_x() - 400.0).abs() < 0.5);
        assert!((rc.center_y() - 300.0).abs() < 0.5);
        assert!(
            rc.width() > 780.0 && rc.height() > 585.0,
            "小窗口 Fit 应紧凑满幅贴合客户区，实际={}x{}",
            rc.width(),
            rc.height()
        );
    }
}
