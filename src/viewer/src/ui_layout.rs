// ─────────────────────────────────────────────────────────────────────────────
// ui_layout.rs — 空间图层与高分屏 (High-DPI) 自适应布局引擎
//   1. 右上角费茨定律 40 DIP 关闭徽章（紧贴屏幕最右上角物理顶点 `(vw, 0)`）
//   2. 左上角极简元数据浮标 / 详细 EXIF 卡片
//   3. 中央无框悬浮相片画布（幻灯片放映态全屏纯黑零界面干扰，1:1 致敬原版 Picasa）
//   4. 底部一体化缩略图胶片流与对称水晶控制栏（含水平翻转 & 垂直翻转，精确遮盖缩略图底 1/3）
// ─────────────────────────────────────────────────────────────────────────────

use crate::constants::{
    ANIM_BAR_HEIGHT_DIP, ANIM_SCRUBBER_WIDTH_DIP, CORNER_CLOSE_RADIUS_DIP, FILMSTRIP_ITEM_GAP_DIP,
    FILMSTRIP_ITEM_SIZE_DIP, MINIMAP_MARGIN_DIP, MINIMAP_MAX_SIDE_DIP, PLAY_BUTTON_SIZE_DIP,
    TOOLBAR_ACTUAL_W_DIP, TOOLBAR_BUTTON_GAP_DIP, TOOLBAR_BUTTON_H_DIP, TOOLBAR_BUTTON_W_DIP,
    TOOLBAR_HEIGHT_DIP,
};
use crate::i18n::ViewerLanguage;
use crate::types::{RectF, ToolbarAction};

#[derive(Clone, Copy, Debug)]
pub struct ToolbarButtonLayout {
    pub action: ToolbarAction,
    pub rect: RectF,
    pub active: bool,
    pub emphasized_circle: bool,
}

#[derive(Clone, Copy, Debug)]
pub struct FilmstripItemLayout {
    pub index: usize,
    pub rect: RectF,
    pub is_current: bool,
}

#[derive(Clone, Debug, Default)]
pub struct ViewerUiLayout {
    pub view_w: f32,
    pub is_slideshow: bool,
    pub is_windowed: bool,
    pub close_rect: RectF,
    pub close_corner_radius: f32,

    // 左上角极简元数据浮标位置
    pub top_left_hud_rect: RectF,

    // 常规看图态底部 68 DIP 一体化通栏（50 DIP 高清缩略图 + 16.5 DIP 镜面倒影 + 对称水晶按钮簇）
    pub toolbar_rect: RectF,
    pub center_pod_rect: RectF,
    pub caption_rect: RectF,
    pub buttons: Vec<ToolbarButtonLayout>,

    // 一体化内嵌胶片缩略图带
    pub filmstrip_rect: Option<RectF>,
    pub filmstrip_items: Vec<FilmstripItemLayout>,

    // 动图专属控制胶囊与可拖拽帧进度条
    pub anim_bar_rect: Option<RectF>,
    pub anim_scrubber_rect: Option<RectF>,
    pub anim_frame_label_rect: Option<RectF>,
    pub anim_buttons: Vec<ToolbarButtonLayout>,

    // 右下角迷你导航框与左上角 EXIF 浮卡
    pub minimap_rect: Option<RectF>,
    pub minimap_image_rect: Option<RectF>,
    pub info_card_rect: Option<RectF>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HitTarget {
    None,
    ToolbarButton(ToolbarAction),
    AnimScrubber,
    ToolbarBackground,
    FilmstripItem(usize),
    FilmstripBackground,
    Minimap,
    ImageCanvas,
    EmptyBackdrop,
}

pub struct LayoutInput {
    pub view_w: f32,
    pub view_h: f32,
    pub sibling_count: usize,
    pub current_index: usize,
    pub filmstrip_scroll_offset: f32,
    pub is_animated: bool,
    pub anim_playing: bool,
    pub is_slideshow: bool,
    pub is_windowed: bool,
    pub is_actual_zoom: bool,
    pub is_flipped_h: bool,
    pub is_flipped_v: bool,
    pub show_info: bool,
    pub show_minimap: bool,
    pub img_eff_w: f32,
    pub img_eff_h: f32,
    pub exif_line_count: usize,
}

impl ViewerUiLayout {
    pub fn compute(input: &LayoutInput) -> Self {
        let mut layout = Self::default();
        let vw = input.view_w.max(480.0);
        let vh = input.view_h.max(320.0);
        let cx = vw * 0.5;
        layout.view_w = vw;
        layout.is_slideshow = input.is_slideshow;
        layout.is_windowed = input.is_windowed;

        // 1. 右上角费茨定律关闭徽章：圆心严格位于屏幕最右上角物理顶点 `(vw, 0)`，半径 40 DIP
        layout.close_corner_radius = CORNER_CLOSE_RADIUS_DIP;
        layout.close_rect = RectF::new(
            vw - CORNER_CLOSE_RADIUS_DIP,
            0.0,
            vw,
            CORNER_CLOSE_RADIUS_DIP,
        );

        // 2. 左上角极简元数据浮标（无死板底框，位于 (18, 14)）
        layout.top_left_hud_rect = RectF::new(18.0, 14.0, (vw - 88.0).max(200.0), 40.0);

        // 3. 最底部：高分屏自适应 66 DIP 一体化全宽暗色底栏
        let bar_top = vh - TOOLBAR_HEIGHT_DIP;
        layout.toolbar_rect = RectF::new(0.0, bar_top, vw, vh);

        // 3a. 胶片缩略图流：48 DIP 高度，紧贴底栏顶边下方 1.5 DIP，当前图居中
        if input.sibling_count > 1 {
            let strip_w = (vw * 0.78).min(1160.0).max(280.0);
            let strip_left = (vw - strip_w) * 0.5;
            let strip_rect = RectF::new(strip_left, bar_top + 1.0, strip_left + strip_w, vh);
            layout.filmstrip_rect = Some(strip_rect);

            let side = FILMSTRIP_ITEM_SIZE_DIP;
            let pitch = side + FILMSTRIP_ITEM_GAP_DIP;
            let item_top = bar_top + 1.5;
            let current_anchor = input.current_index as f32 + input.filmstrip_scroll_offset;

            let max_visible_half = ((strip_w * 0.5) / pitch).ceil() as isize + 1;
            let anchor_i = current_anchor.round() as isize;
            let start_i = (anchor_i - max_visible_half).max(0) as usize;
            let end_i = ((anchor_i + max_visible_half + 1) as usize).min(input.sibling_count);

            for idx in start_i..end_i {
                let delta = idx as f32 - current_anchor;
                let icx = cx + delta * pitch;
                let half = side * 0.5;
                let left = icx - half;
                let right = icx + half;
                if right < strip_left + 2.0 || left > strip_left + strip_w - 2.0 {
                    continue;
                }
                layout.filmstrip_items.push(FilmstripItemLayout {
                    index: idx,
                    rect: RectF::new(left, item_top, right, item_top + side),
                    is_current: idx == input.current_index,
                });
            }
        }

        // 3b. 前景对称主控按钮簇（按钮精确遮盖 50 DIP 缩略图底部 16.5 DIP = 33.0% 黄金 1/3）：
        //     [+] [-] [1:1]   |   ◀ (▶ 44 DIP 水晶播放圆球) ▶   |   [↺] [↻] [水平翻转] [垂直翻转]
        let bw = TOOLBAR_BUTTON_W_DIP;
        let actual_bw = TOOLBAR_ACTUAL_W_DIP;
        let bh = TOOLBAR_BUTTON_H_DIP;
        let gap = TOOLBAR_BUTTON_GAP_DIP;
        let btn_cy = bar_top + 48.0;
        let btn_top = btn_cy - bh * 0.5;
        let btn_bottom = btn_cy + bh * 0.5;

        let play_sz = PLAY_BUTTON_SIZE_DIP;
        let play_r = play_sz * 0.5;
        let play_rect = RectF::new(cx - play_r, btn_cy - play_r, cx + play_r, btn_cy + play_r);

        let show_nav = input.sibling_count > 1;
        let wing_outer = 54.0f32;
        let wing_inner = 16.5f32;
        let wing_half_h = 15.2f32;
        let pod_left = if show_nav { cx - wing_outer } else { cx - play_r };
        let pod_right = if show_nav { cx + wing_outer } else { cx + play_r };
        layout.center_pod_rect =
            RectF::new(pod_left, btn_cy - wing_half_h, pod_right, btn_cy + wing_half_h);

        // 中央三联：先注册中央立体水晶圆球（保证圆球半径内命中优先级最高）
        layout.buttons.push(ToolbarButtonLayout {
            action: ToolbarAction::ToggleSlideshow,
            rect: play_rect,
            active: input.is_slideshow,
            emphasized_circle: true,
        });
        if show_nav {
            layout.buttons.push(ToolbarButtonLayout {
                action: ToolbarAction::PrevImage,
                rect: RectF::new(
                    cx - wing_outer,
                    btn_cy - wing_half_h,
                    cx - wing_inner,
                    btn_cy + wing_half_h,
                ),
                active: false,
                emphasized_circle: false,
            });
            layout.buttons.push(ToolbarButtonLayout {
                action: ToolbarAction::NextImage,
                rect: RectF::new(
                    cx + wing_inner,
                    btn_cy - wing_half_h,
                    cx + wing_outer,
                    btn_cy + wing_half_h,
                ),
                active: false,
                emphasized_circle: false,
            });
        }

        // 左侧 3 键：[+] [-] [1:1]
        let actual_right = layout.center_pod_rect.left - 12.0;
        let actual_left = actual_right - actual_bw;
        layout.buttons.push(ToolbarButtonLayout {
            action: ToolbarAction::ToggleActualSize,
            rect: RectF::new(actual_left, btn_top, actual_right, btn_bottom),
            active: input.is_actual_zoom,
            emphasized_circle: false,
        });

        let zout_right = actual_left - gap;
        let zout_left = zout_right - bw;
        layout.buttons.push(ToolbarButtonLayout {
            action: ToolbarAction::ZoomOut,
            rect: RectF::new(zout_left, btn_top, zout_right, btn_bottom),
            active: false,
            emphasized_circle: false,
        });

        let zin_right = zout_left - gap;
        let zin_left = zin_right - bw;
        layout.buttons.push(ToolbarButtonLayout {
            action: ToolbarAction::ZoomIn,
            rect: RectF::new(zin_left, btn_top, zin_right, btn_bottom),
            active: false,
            emphasized_circle: false,
        });

        // 右侧 4 键：[↺ 逆时针旋转] [↻ 顺时针旋转] [水平翻转] [垂直翻转]
        let rot_ccw_left = layout.center_pod_rect.right + 12.0;
        layout.buttons.push(ToolbarButtonLayout {
            action: ToolbarAction::RotateCcw,
            rect: RectF::new(rot_ccw_left, btn_top, rot_ccw_left + bw, btn_bottom),
            active: false,
            emphasized_circle: false,
        });

        let rot_cw_left = rot_ccw_left + bw + gap;
        layout.buttons.push(ToolbarButtonLayout {
            action: ToolbarAction::RotateCw,
            rect: RectF::new(rot_cw_left, btn_top, rot_cw_left + bw, btn_bottom),
            active: false,
            emphasized_circle: false,
        });

        let flip_h_left = rot_cw_left + bw + gap + 3.0;
        layout.buttons.push(ToolbarButtonLayout {
            action: ToolbarAction::FlipHorizontal,
            rect: RectF::new(flip_h_left, btn_top, flip_h_left + bw, btn_bottom),
            active: input.is_flipped_h,
            emphasized_circle: false,
        });

        let flip_v_left = flip_h_left + bw + gap;
        layout.buttons.push(ToolbarButtonLayout {
            action: ToolbarAction::FlipVertical,
            rect: RectF::new(flip_v_left, btn_top, flip_v_left + bw, btn_bottom),
            active: input.is_flipped_v,
            emphasized_circle: false,
        });

        // 4. 底栏正上方：动图专属轻量胶囊 + 共享微文件名/操作提示文字 (ss/statustext)
        // （幻灯片放映态全屏纯黑净空，不渲染底部栏与文件名）
        let mut stack_top = bar_top - 6.0;

        if input.is_animated && !input.is_slideshow {
            let ab = 24.0f32;
            let ag = 4.5f32;
            let frame_lbl_w = 56.0f32;
            let speed_btn_w = 42.0f32;
            let anim_w = 11.0 * 2.0
                + ab * 3.0
                + ag * 2.0
                + 8.0
                + ANIM_SCRUBBER_WIDTH_DIP
                + 8.0
                + frame_lbl_w
                + 6.0
                + speed_btn_w
                + ag
                + ab;
            let anim_left = (vw - anim_w) * 0.5;
            let anim_top = stack_top - ANIM_BAR_HEIGHT_DIP;
            layout.anim_bar_rect =
                Some(RectF::new(anim_left, anim_top, anim_left + anim_w, stack_top));

            let bt = anim_top + (ANIM_BAR_HEIGHT_DIP - ab) * 0.5;
            let mut ax = anim_left + 11.0;

            layout.anim_buttons.push(ToolbarButtonLayout {
                action: ToolbarAction::AnimTogglePlay,
                rect: RectF::new(ax, bt, ax + ab, bt + ab),
                active: input.anim_playing,
                emphasized_circle: false,
            });
            ax += ab + ag;

            layout.anim_buttons.push(ToolbarButtonLayout {
                action: ToolbarAction::AnimPrevFrame,
                rect: RectF::new(ax, bt, ax + ab, bt + ab),
                active: false,
                emphasized_circle: false,
            });
            ax += ab + ag;

            layout.anim_buttons.push(ToolbarButtonLayout {
                action: ToolbarAction::AnimNextFrame,
                rect: RectF::new(ax, bt, ax + ab, bt + ab),
                active: false,
                emphasized_circle: false,
            });
            ax += ab + 8.0;

            layout.anim_scrubber_rect = Some(RectF::new(
                ax,
                anim_top + 4.0,
                ax + ANIM_SCRUBBER_WIDTH_DIP,
                stack_top - 4.0,
            ));
            ax += ANIM_SCRUBBER_WIDTH_DIP + 8.0;

            layout.anim_frame_label_rect =
                Some(RectF::new(ax, anim_top, ax + frame_lbl_w, stack_top));
            ax += frame_lbl_w + 6.0;

            layout.anim_buttons.push(ToolbarButtonLayout {
                action: ToolbarAction::AnimCycleSpeed,
                rect: RectF::new(ax, bt, ax + speed_btn_w, bt + ab),
                active: false,
                emphasized_circle: false,
            });
            ax += speed_btn_w + ag;

            layout.anim_buttons.push(ToolbarButtonLayout {
                action: ToolbarAction::SaveCurrentFramePng,
                rect: RectF::new(ax, bt, ax + ab, bt + ab),
                active: false,
                emphasized_circle: false,
            });
            stack_top = anim_top - 5.0;
        }

        let cap_w = (vw - 80.0).clamp(240.0, 920.0);
        layout.caption_rect = RectF::new(
            (vw - cap_w) * 0.5,
            stack_top - 25.0,
            (vw + cap_w) * 0.5,
            stack_top,
        );

        // 5. 右下角 Picasa 缩放迷你导航框
        if input.show_minimap && input.img_eff_w > 0.0 && input.img_eff_h > 0.0 {
            let aspect = input.img_eff_w / input.img_eff_h;
            let (mw, mh) = if aspect >= 1.0 {
                (MINIMAP_MAX_SIDE_DIP, (MINIMAP_MAX_SIDE_DIP / aspect).max(48.0))
            } else {
                ((MINIMAP_MAX_SIDE_DIP * aspect).max(48.0), MINIMAP_MAX_SIDE_DIP)
            };
            let pad = 7.0;
            let box_w = mw + pad * 2.0;
            let box_h = mh + pad * 2.0;
            let right = vw - MINIMAP_MARGIN_DIP;
            let bottom = bar_top - 16.0;
            let outer = RectF::new(right - box_w, bottom - box_h, right, bottom);
            let inner = RectF::new(
                outer.left + pad,
                outer.top + pad,
                outer.right - pad,
                outer.bottom - pad,
            );
            layout.minimap_rect = Some(outer);
            layout.minimap_image_rect = Some(inner);
        }

        // 6. 左上角真 EXIF 相机详情玻璃卡
        if input.show_info {
            let rows = input.exif_line_count.max(3).min(12) as f32;
            let card_h = 54.0 + rows * 24.5 + 14.0;
            layout.info_card_rect = Some(RectF::new(18.0, 18.0, 392.0, 18.0 + card_h));
        }

        layout
    }

    /// 共享状态提示文字（自动跟随 8 国系统语言）
    pub fn hovered_status_tip(&self, target: HitTarget, lang: ViewerLanguage) -> Option<&'static str> {
        let s = lang.strings();
        match target {
            HitTarget::ToolbarButton(ToolbarAction::CloseViewer) => Some(s.tip_close),
            HitTarget::ToolbarButton(act) => {
                let is_active = self
                    .buttons
                    .iter()
                    .chain(self.anim_buttons.iter())
                    .find(|b| b.action == act)
                    .map(|b| b.active)
                    .unwrap_or(false);
                match act {
                    ToolbarAction::ZoomIn => Some(s.tip_zoom_in),
                    ToolbarAction::ZoomOut => Some(s.tip_zoom_out),
                    ToolbarAction::ToggleActualSize => Some(if is_active {
                        s.tip_fit_window
                    } else {
                        s.tip_actual_size
                    }),
                    ToolbarAction::ToggleSlideshow => Some(if is_active {
                        s.tip_pause_slideshow
                    } else {
                        s.tip_play_slideshow
                    }),
                    ToolbarAction::PrevImage => Some(s.tip_prev_image),
                    ToolbarAction::NextImage => Some(s.tip_next_image),
                    ToolbarAction::RotateCcw => Some(s.tip_rotate_ccw),
                    ToolbarAction::RotateCw => Some(s.tip_rotate_cw),
                    ToolbarAction::FlipHorizontal => Some(s.tip_flip_h),
                    ToolbarAction::FlipVertical => Some(s.tip_flip_v),
                    ToolbarAction::AnimTogglePlay => Some(if is_active {
                        s.tip_anim_pause
                    } else {
                        s.tip_anim_play
                    }),
                    ToolbarAction::AnimPrevFrame => Some(s.tip_anim_prev_frame),
                    ToolbarAction::AnimNextFrame => Some(s.tip_anim_next_frame),
                    ToolbarAction::AnimCycleSpeed => Some(s.tip_anim_speed),
                    ToolbarAction::SaveCurrentFramePng => Some(s.tip_save_png),
                    _ => None,
                }
            }
            HitTarget::AnimScrubber => Some(s.tip_scrub_frames),
            _ => None,
        }
    }

    pub fn hit_test(&self, x: f32, y: f32, image_rect: RectF, ui_visible: bool) -> HitTarget {
        // 右上角费茨定律关闭徽章：仅浮层看图态显示，小窗口态已有系统标题栏关闭钮故彻底隐藏不拦截命中
        if !self.is_slideshow && !self.is_windowed {
            let dx_corner = x - self.view_w;
            let dy_corner = y;
            let hit_r = self.close_corner_radius + 4.0;
            if x >= self.view_w - hit_r
                && y >= 0.0
                && y <= hit_r
                && (dx_corner * dx_corner + dy_corner * dy_corner) <= hit_r * hit_r
            {
                return HitTarget::ToolbarButton(ToolbarAction::CloseViewer);
            }
        }

        if ui_visible && !self.is_slideshow {
            // 常规看图态：1. 中央水晶播放圆球严格优先命中
            for btn in &self.buttons {
                if btn.emphasized_circle {
                    let r = btn.rect.width() * 0.5;
                    let dx = x - btn.rect.center_x();
                    let dy = y - btn.rect.center_y();
                    if dx * dx + dy * dy <= (r + 1.5) * (r + 1.5) {
                        return HitTarget::ToolbarButton(btn.action);
                    }
                }
            }
            // 2. 其余前景操作按钮（含旋转、水平翻转、垂直翻转、缩放）
            for btn in &self.buttons {
                if !btn.emphasized_circle && btn.rect.contains(x, y) {
                    return HitTarget::ToolbarButton(btn.action);
                }
            }
            // 3. 背景层一体化缩略图胶片流
            for item in &self.filmstrip_items {
                if item.rect.contains(x, y) {
                    return HitTarget::FilmstripItem(item.index);
                }
            }
            if let Some(rc) = self.filmstrip_rect {
                if rc.contains(x, y) {
                    return HitTarget::FilmstripBackground;
                }
            }
            if self.toolbar_rect.contains(x, y) {
                return HitTarget::ToolbarBackground;
            }
            if let Some(rc) = self.anim_scrubber_rect {
                if rc.contains(x, y) {
                    return HitTarget::AnimScrubber;
                }
            }
            for btn in &self.anim_buttons {
                if btn.rect.contains(x, y) {
                    return HitTarget::ToolbarButton(btn.action);
                }
            }
            if let Some(rc) = self.anim_bar_rect {
                if rc.contains(x, y) {
                    return HitTarget::ToolbarBackground;
                }
            }
        }

        if !self.is_slideshow {
            if let Some(rc) = self.minimap_rect {
                if rc.contains(x, y) {
                    return HitTarget::Minimap;
                }
            }
        }

        if image_rect.contains(x, y) {
            HitTarget::ImageCanvas
        } else {
            HitTarget::EmptyBackdrop
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_input() -> LayoutInput {
        LayoutInput {
            view_w: 1920.0,
            view_h: 1080.0,
            sibling_count: 8,
            current_index: 3,
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
            img_eff_w: 1200.0,
            img_eff_h: 800.0,
            exif_line_count: 4,
        }
    }

    #[test]
    fn fitts_law_close_badge_hits_top_right_vertex_even_when_ui_idle() {
        let layout = ViewerUiLayout::compute(&sample_input());
        let img_rc = RectF::new(400.0, 120.0, 1520.0, 920.0);
        // 盲甩到屏幕最右上角顶点 `(1919, 0)` 必中
        assert_eq!(
            layout.hit_test(1919.0, 0.0, img_rc, false),
            HitTarget::ToolbarButton(ToolbarAction::CloseViewer)
        );
        assert_eq!(
            layout.hit_test(1920.0 - 24.0, 24.0, img_rc, false),
            HitTarget::ToolbarButton(ToolbarAction::CloseViewer)
        );
        // 超出 44 DIP 扇形半径外不误触
        assert_eq!(
            layout.hit_test(1920.0 - 52.0, 52.0, img_rc, true),
            HitTarget::EmptyBackdrop
        );
    }

    #[test]
    fn toolbar_includes_flip_buttons_and_maintains_one_third_overlap() {
        let input = sample_input();
        let layout = ViewerUiLayout::compute(&input);
        let vw = input.view_w;

        let rot_cw = layout
            .buttons
            .iter()
            .find(|b| b.action == ToolbarAction::RotateCw)
            .expect("应存在顺时针旋转按钮");
        let flip_h = layout
            .buttons
            .iter()
            .find(|b| b.action == ToolbarAction::FlipHorizontal)
            .expect("旋转右侧应存在水平翻转按钮");
        let flip_v = layout
            .buttons
            .iter()
            .find(|b| b.action == ToolbarAction::FlipVertical)
            .expect("旋转右侧应存在垂直翻转按钮");
        assert!(flip_h.rect.left > rot_cw.rect.right);
        assert!(flip_v.rect.left > flip_h.rect.right);

        let cur = layout
            .filmstrip_items
            .iter()
            .find(|it| it.is_current)
            .expect("多图时应存在当前缩略图");
        assert!((cur.rect.center_x() - vw * 0.5).abs() < 0.01);

        let zoom_btn = layout
            .buttons
            .iter()
            .find(|b| b.action == ToolbarAction::ZoomIn)
            .expect("应存在放大按钮");
        let overlap = (cur.rect.bottom - zoom_btn.rect.top).max(0.0);
        let ratio = overlap / cur.rect.height();
        assert!(
            (0.28..=0.38).contains(&ratio),
            "按钮遮盖缩略图比例应严格接近 1/3 (33.3%)，实际为 {:.3}",
            ratio
        );
    }

    #[test]
    fn slideshow_mode_is_pure_canvas_without_bottom_bar() {
        let mut input = sample_input();
        input.is_slideshow = true;
        let layout = ViewerUiLayout::compute(&input);
        let img_rc = RectF::new(240.0, 0.0, 1680.0, 1080.0);
        // 幻灯片放映态绝对纯净（1:1 致敬原版 Picasa），底部正中不渲染任何播控条或文件名
        assert_eq!(
            layout.hit_test(960.0, 1038.0, img_rc, true),
            HitTarget::ImageCanvas
        );
        assert_eq!(
            layout.hit_test(1919.0, 0.0, img_rc, true),
            HitTarget::EmptyBackdrop
        );
    }

    #[test]
    fn windowed_mode_hides_corner_close_hit_zone_while_preserving_bottom_toolbar() {
        let mut input = sample_input();
        input.view_w = 800.0;
        input.view_h = 600.0;
        input.is_windowed = true;
        let layout = ViewerUiLayout::compute(&input);
        let img_rc = RectF::new(10.0, 10.0, 790.0, 590.0);

        // 右上角顶点不触发浮层四分之一圆关闭钮，返回空白画布；扇形半径内的图片区域正常命中图片
        assert_eq!(
            layout.hit_test(799.0, 1.0, img_rc, true),
            HitTarget::EmptyBackdrop
        );
        assert_eq!(
            layout.hit_test(785.0, 15.0, img_rc, true),
            HitTarget::ImageCanvas
        );
        // 底部控制栏仍完整命中
        let play_btn = layout
            .buttons
            .iter()
            .find(|b| b.action == ToolbarAction::ToggleSlideshow)
            .expect("小窗口模式应保留底部播放钮");
        assert_eq!(
            layout.hit_test(play_btn.rect.center_x(), play_btn.rect.center_y(), img_rc, true),
            HitTarget::ToolbarButton(ToolbarAction::ToggleSlideshow)
        );
    }
}
