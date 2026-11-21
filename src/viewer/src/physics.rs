// ─────────────────────────────────────────────────────────────────────────────
// physics.rs — 帧率无关 (dt-independent) 指数阻尼物理动画器、缓动曲线与电影级推镜引擎
//
// 设计准则：
// - 所有连续动画均采用 `factor = 1.0 - (-speed * dt).exp()` 帧率无关指数衰减公式，
//   在 60Hz / 120Hz / 144Hz / 165Hz / 240Hz 显示器上保持完全一致的物理阻尼手感；
// - 任意时刻均可无缝打断并转向新目标值，零跳变、零硬切。
// ─────────────────────────────────────────────────────────────────────────────

/// 计算帧率无关的指数阻尼插值因子 `1 - exp(-speed * dt)`
#[inline]
pub fn exp_decay_factor(speed: f32, dt_sec: f32) -> f32 {
    let dt = dt_sec.clamp(0.0005, 0.05);
    (1.0 - (-speed * dt).exp()).clamp(0.0, 1.0)
}

/// 对标量执行一帧指数阻尼平滑趋近，返回更新后的当前值
#[inline]
pub fn exp_smooth(current: f32, target: f32, speed: f32, dt_sec: f32) -> f32 {
    let f = exp_decay_factor(speed, dt_sec);
    current + (target - current) * f
}

/// 五次缓出曲线 `1 - (1 - t)^5`：用于图片打开 (`Opening`) 从视口中心 `0.35x` 清脆弹开
#[inline]
pub fn ease_out_quint(t: f32) -> f32 {
    let u = 1.0 - t.clamp(0.0, 1.0);
    1.0 - u * u * u * u * u
}

/// 四次缓出曲线 `1 - (1 - t)^4`：用于切图微位移与 UI 唤醒
#[inline]
pub fn ease_out_quart(t: f32) -> f32 {
    let u = 1.0 - t.clamp(0.0, 1.0);
    1.0 - u * u * u * u
}

/// 三次缓出曲线 `1 - (1 - t)^3`：用于缩略图波浪生长
#[inline]
pub fn ease_out_cubic(t: f32) -> f32 {
    let u = 1.0 - t.clamp(0.0, 1.0);
    1.0 - u * u * u
}

/// 三次缓入曲线 `t^3`：用于向心加速收束
#[inline]
pub fn ease_in_cubic(t: f32) -> f32 {
    let x = t.clamp(0.0, 1.0);
    x * x * x
}

/// 关闭向心坍缩复合曲线：兼具首帧零延迟跟手初速与后段 `EaseInCubic` 向中心吸入张力
#[inline]
pub fn close_implode_curve(t: f32) -> f32 {
    let x = t.clamp(0.0, 1.0);
    0.22 * x + 0.78 * ease_in_cubic(x)
}

/// 三次平滑 S 曲线 `3t^2 - 2t^3`：用于幻灯片 900ms 双图层柔和交叉叠化 (Cross-dissolve)
#[inline]
pub fn smoothstep_s_curve(t: f32) -> f32 {
    let x = t.clamp(0.0, 1.0);
    x * x * (3.0 - 2.0 * x)
}

/// 橡皮筋弹性阻尼函数：当拖拽越过边界 `over` 时，施加渐近衰减阻力（最大弹性拉伸 `limit` DIP）
#[inline]
pub fn rubber_band_offset(over: f32, limit: f32) -> f32 {
    if over.abs() < 1e-4 || limit <= 0.0 {
        return 0.0;
    }
    let sign = over.signum();
    let x = over.abs() * 0.36;
    sign * (limit * x) / (limit + x)
}

/// 电影级幻灯片 Ken Burns 微推拉平移轨迹（帧率无关演进，切图叠化期间离场图层仍保持惯性不急停）
#[derive(Clone, Copy, Debug)]
pub struct KenBurnsTrack {
    pub elapsed_sec: f32,
    pub duration_sec: f32,
    pub start_scale: f32,
    pub end_scale: f32,
    pub start_offset_dip: (f32, f32),
    pub end_offset_dip: (f32, f32),
}

impl Default for KenBurnsTrack {
    fn default() -> Self {
        Self::for_slide_index(0, 4.4)
    }
}

impl KenBurnsTrack {
    /// 根据图片索引生成确定性且柔和交替的电影推镜轨迹（幅度克制在 1.00x ~ 1.068x，优雅不眩晕）
    pub fn for_slide_index(index: usize, total_span_sec: f32) -> Self {
        let span = total_span_sec.max(1.0);
        match index % 4 {
            0 => Self {
                elapsed_sec: 0.0,
                duration_sec: span,
                start_scale: 1.000,
                end_scale: 1.068,
                start_offset_dip: (-9.0, -5.0),
                end_offset_dip: (9.0, 5.0),
            },
            1 => Self {
                elapsed_sec: 0.0,
                duration_sec: span,
                start_scale: 1.064,
                end_scale: 1.000,
                start_offset_dip: (8.0, -5.0),
                end_offset_dip: (-8.0, 5.0),
            },
            2 => Self {
                elapsed_sec: 0.0,
                duration_sec: span,
                start_scale: 1.000,
                end_scale: 1.066,
                start_offset_dip: (8.0, 6.0),
                end_offset_dip: (-8.0, -6.0),
            },
            _ => Self {
                elapsed_sec: 0.0,
                duration_sec: span,
                start_scale: 1.062,
                end_scale: 1.000,
                start_offset_dip: (-8.0, 5.0),
                end_offset_dip: (8.0, -5.0),
            },
        }
    }

    #[inline]
    pub fn advance(&mut self, dt_sec: f32) {
        self.elapsed_sec = (self.elapsed_sec + dt_sec.clamp(0.0, 0.1)).min(self.duration_sec * 1.35);
    }

    /// 返回当前时刻的 `(scale_mul, offset_x_dip, offset_y_dip)`
    #[inline]
    pub fn sample(&self) -> (f32, f32, f32) {
        let t = (self.elapsed_sec / self.duration_sec.max(0.1)).clamp(0.0, 1.25);
        let scale = self.start_scale + (self.end_scale - self.start_scale) * t;
        let ox = self.start_offset_dip.0 + (self.end_offset_dip.0 - self.start_offset_dip.0) * t;
        let oy = self.start_offset_dip.1 + (self.end_offset_dip.1 - self.start_offset_dip.1) * t;
        (scale, ox, oy)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frame_rate_independent_damping_matches_across_60hz_and_120hz() {
        let mut v60 = 0.0f32;
        for _ in 0..30 {
            v60 = exp_smooth(v60, 100.0, 16.0, 1.0 / 60.0);
        }
        let mut v120 = 0.0f32;
        for _ in 0..60 {
            v120 = exp_smooth(v120, 100.0, 16.0, 1.0 / 120.0);
        }
        assert!(
            (v60 - v120).abs() < 0.05,
            "60Hz ({v60}) 与 120Hz ({v120}) 相同物理时长后应高度一致"
        );
    }

    #[test]
    fn easing_boundaries_are_exact() {
        assert!((ease_out_quint(0.0) - 0.0).abs() < 1e-6);
        assert!((ease_out_quint(1.0) - 1.0).abs() < 1e-6);
        assert!(ease_out_quint(0.35) > 0.85);
        assert!((close_implode_curve(0.0) - 0.0).abs() < 1e-6);
        assert!((close_implode_curve(1.0) - 1.0).abs() < 1e-6);
    }
}
