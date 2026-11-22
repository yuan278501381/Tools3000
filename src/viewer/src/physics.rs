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

/// 五次缓出曲线 `1 - (1 - t)^5`
#[allow(dead_code)]
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

/// 二次缓出曲线 `1 - (1 - t)^2`
#[inline]
pub fn ease_out_quad(t: f32) -> f32 {
    let u = 1.0 - t.clamp(0.0, 1.0);
    1.0 - u * u
}

/// 原版 Picasa 主图打开舒展曲线 `1 - (1 - t)^2.15`：中段从容均匀、收尾优雅贴合，彻底告别暴冲吞帧感
#[inline]
pub fn ease_out_picasa_open(t: f32) -> f32 {
    let u = 1.0 - t.clamp(0.0, 1.0);
    1.0 - u.powf(2.15)
}

/// 轻微 Q 弹过冲缓出曲线（用于下方胶片缩略图从槽位依次绽放，最大过冲 ~3.5% 灵动克制）
#[inline]
pub fn ease_out_back_soft(t: f32) -> f32 {
    const C1: f32 = 0.85;
    const C3: f32 = C1 + 1.0;
    let x = t.clamp(0.0, 1.0) - 1.0;
    1.0 + C3 * x * x * x + C1 * x * x
}

/// 三次缓出曲线 `1 - (1 - t)^3`
#[inline]
pub fn ease_out_cubic(t: f32) -> f32 {
    let u = 1.0 - t.clamp(0.0, 1.0);
    1.0 - u * u * u
}

/// 关闭向心坍缩复合曲线：兼具首帧干脆跟手初速与中后段加速向中心收拢张力（对照原版录屏实测 #501->#502 52% 收束点校准）
#[inline]
pub fn close_implode_curve(t: f32) -> f32 {
    let x = t.clamp(0.0, 1.0);
    0.34 * x + 0.66 * x * x
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
        assert!((ease_out_picasa_open(0.0) - 0.0).abs() < 1e-6);
        assert!((ease_out_picasa_open(1.0) - 1.0).abs() < 1e-6);
        let mid_open = ease_out_picasa_open(0.33);
        assert!(mid_open > 0.50 && mid_open < 0.65);
        assert!((ease_out_back_soft(0.0) - 0.0).abs() < 1e-5);
        assert!((ease_out_back_soft(1.0) - 1.0).abs() < 1e-5);
        assert!(ease_out_back_soft(0.75) > 1.0 && ease_out_back_soft(0.75) < 1.05);
        assert!((close_implode_curve(0.0) - 0.0).abs() < 1e-6);
        assert!((close_implode_curve(1.0) - 1.0).abs() < 1e-6);
        assert!(close_implode_curve(0.55) > 0.35 && close_implode_curve(0.55) < 0.45);
    }
}
