#include "capture/StudioTimeline.h"
#include "common/AtomicFile.h"
#include "core/utils/WinUtils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <sapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <sstream>

namespace tools3000::capture {

namespace {

StudioCursorKind classifySystemCursor(HCURSOR hCur) noexcept {
    if (!hCur) return StudioCursorKind::Hidden;
    static const HCURSOR s_ibeam = LoadCursorW(nullptr, IDC_IBEAM);
    static const HCURSOR s_hand  = LoadCursorW(nullptr, IDC_HAND);
    static const HCURSOR s_cross = LoadCursorW(nullptr, IDC_CROSS);
    if (hCur == s_ibeam) return StudioCursorKind::IBeam;
    if (hCur == s_hand)  return StudioCursorKind::Hand;
    if (hCur == s_cross) return StudioCursorKind::Cross;
    return StudioCursorKind::Arrow;
}

std::string formatSrtTimestamp(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t hours = ms / 3600000;
    ms %= 3600000;
    const int64_t mins = ms / 60000;
    ms %= 60000;
    const int64_t secs = ms / 1000;
    const int64_t millis = ms % 1000;
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld",
                  static_cast<long long>(hours),
                  static_cast<long long>(mins),
                  static_cast<long long>(secs),
                  static_cast<long long>(millis));
    return buf;
}

} // namespace

double OneEuroFilter1D::smoothingAlpha(double dt, double cutoffHz) noexcept {
    if (dt <= 1e-6 || cutoffHz <= 1e-6) return 1.0;
    const double tau = 1.0 / (2.0 * std::numbers::pi * cutoffHz);
    return 1.0 / (1.0 + tau / dt);
}

double OneEuroFilter1D::filter(double value, double timestampSec) noexcept {
    if (!m_initialized) {
        m_prevValue = value;
        m_prevDeriv = 0.0;
        m_prevTimeSec = timestampSec;
        m_initialized = true;
        return value;
    }
    const double dt = timestampSec - m_prevTimeSec;
    if (dt <= 1e-5) {
        return m_prevValue;
    }
    m_prevTimeSec = timestampSec;

    const double rawDeriv = (value - m_prevValue) / dt;
    const double alphaD = smoothingAlpha(dt, m_dCutoff);
    const double hatDeriv = alphaD * rawDeriv + (1.0 - alphaD) * m_prevDeriv;
    m_prevDeriv = hatDeriv;

    const double cutoff = m_minCutoff + m_beta * std::abs(hatDeriv);
    const double alpha = smoothingAlpha(dt, cutoff);
    const double filtered = alpha * value + (1.0 - alpha) * m_prevValue;
    m_prevValue = filtered;
    return filtered;
}

double CriticallyDampedSpring1D::step(double target, double dtSec) noexcept {
    if (!m_initialized) {
        reset(target, 0.0);
        return m_pos;
    }
    if (dtSec <= 1e-6) {
        return m_pos;
    }
    const double dt = (std::min)(dtSec, 0.25);
    const double w = (std::max)(0.5, m_omega);
    const double wdt = w * dt;
    const double expTerm = std::exp(-wdt);
    const double delta = m_pos - target;
    const double nextPos = target + (delta * (1.0 + wdt) + m_vel * dt) * expTerm;
    const double nextVel = (m_vel - (delta * w + m_vel) * wdt) * expTerm;
    m_pos = nextPos;
    m_vel = nextVel;
    return m_pos;
}

std::vector<StudioZoomSegment> StudioTimelineAnalyzer::buildAutoZoomSegments(
    const StudioTimelineData& timeline,
    float defaultZoom,
    bool includeTypingFocus) {
    struct RawAnchor {
        int64_t ptsMs = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    const float clampedZoom = std::clamp(defaultZoom, 1.25f, 2.80f);
    const float w = static_cast<float>((std::max)(1, timeline.sourceWidth));
    const float h = static_cast<float>((std::max)(1, timeline.sourceHeight));
    const float flyByDistThresh = (std::max)(280.0f, w * 0.17f);
    const bool hasRichCursorTrack = timeline.cursorSamples.size() >= 16;

    auto isTransientFlyByClick = [&](const StudioClickEvent& clk, size_t clkIdx) -> bool {
        if (!hasRichCursorTrack) return false;
        // 若紧接着 650ms 内在附近有连续操作，说明是目标区域双击/连击，保留放大
        if (clkIdx + 1 < timeline.clickEvents.size()) {
            const auto& nxt = timeline.clickEvents[clkIdx + 1];
            if (nxt.ptsMs - clk.ptsMs <= 650 &&
                std::hypot(nxt.x - clk.x, nxt.y - clk.y) < flyByDistThresh * 0.75f) {
                return false;
            }
        }
        // 检查点击后 220ms~260ms 鼠标是否已高速飞掠离开目标区域（如顺手关弹窗/点遮罩）
        const int64_t probeMs = clk.ptsMs + 230;
        auto it = std::lower_bound(
            timeline.cursorSamples.begin(), timeline.cursorSamples.end(), probeMs,
            [](const StudioCursorSample& s, int64_t target) { return s.ptsMs < target; });
        if (it != timeline.cursorSamples.end() && it->ptsMs <= clk.ptsMs + 360) {
            const float dist = std::hypot(it->x - clk.x, it->y - clk.y);
            if (dist > flyByDistThresh) {
                return true;
            }
        }
        return false;
    };

    std::vector<RawAnchor> anchors;
    anchors.reserve(timeline.clickEvents.size() + (includeTypingFocus ? timeline.focusEvents.size() : 0));
    for (size_t i = 0; i < timeline.clickEvents.size(); ++i) {
        const auto& clk = timeline.clickEvents[i];
        if (clk.x >= 0.0f && clk.x <= w && clk.y >= 0.0f && clk.y <= h) {
            if (isTransientFlyByClick(clk, i)) {
                continue;
            }
            anchors.push_back({clk.ptsMs, clk.x, clk.y});
        }
    }
    if (includeTypingFocus) {
        for (const auto& foc : timeline.focusEvents) {
            if (foc.x >= 0.0f && foc.x <= w && foc.y >= 0.0f && foc.y <= h) {
                anchors.push_back({foc.ptsMs, foc.x, foc.y});
            }
        }
    }

    if (anchors.empty()) {
        return {};
    }

    std::sort(anchors.begin(), anchors.end(), [](const RawAnchor& a, const RawAnchor& b) {
        return a.ptsMs < b.ptsMs;
    });

    // 过滤 1.4s 内跨全屏来回乱点的零碎扫荡动作 (连续 >=3 次跨屏大跳跃 > 32% 屏宽)
    std::vector<RawAnchor> cleanAnchors;
    cleanAnchors.reserve(anchors.size());
    const float scatterJumpThresh = (std::max)(340.0f, w * 0.32f);
    for (size_t i = 0; i < anchors.size(); ++i) {
        if (i > 0 && i + 1 < anchors.size()) {
            const auto& prev = anchors[i - 1];
            const auto& cur = anchors[i];
            const auto& nxt = anchors[i + 1];
            if (nxt.ptsMs - prev.ptsMs <= 1400 &&
                std::hypot(cur.x - prev.x, cur.y - prev.y) > scatterJumpThresh &&
                std::hypot(nxt.x - cur.x, nxt.y - cur.y) > scatterJumpThresh) {
                continue; // 剔除夹在中间的零碎跨屏跳跃点，防镜头剧烈横甩
            }
        }
        cleanAnchors.push_back(anchors[i]);
    }
    if (cleanAnchors.empty()) {
        return {};
    }

    constexpr int64_t kLeadInMs = 220;
    constexpr int64_t kHoldTailMs = 1400;
    constexpr int64_t kMergeGapMs = 1050; // 2.67s 内的相邻操作平滑桥接摇移，杜绝频繁“放大-缩回-再放大”抽搐
    const int64_t maxDur = (std::max<int64_t>)(timeline.durationMs, cleanAnchors.back().ptsMs + kHoldTailMs);

    std::vector<StudioZoomSegment> segments;
    for (const auto& a : cleanAnchors) {
        const int64_t segStart = (std::max<int64_t>)(0, a.ptsMs - kLeadInMs);
        const int64_t segEnd = (std::min<int64_t>)(maxDur, a.ptsMs + kHoldTailMs);
        if (segEnd <= segStart + 120) continue;

        if (!segments.empty() && segStart <= segments.back().endMs + kMergeGapMs) {
            segments.back().endMs = (std::max)(segments.back().endMs, segEnd);
            if (std::hypot(a.x - segments.back().focusX, a.y - segments.back().focusY) < w * 0.28f) {
                segments.back().focusX = segments.back().focusX * 0.40f + a.x * 0.60f;
                segments.back().focusY = segments.back().focusY * 0.40f + a.y * 0.60f;
            }
        } else {
            StudioZoomSegment seg;
            seg.startMs = segStart;
            seg.endMs = segEnd;
            seg.focusX = a.x;
            seg.focusY = a.y;
            seg.targetZoom = clampedZoom;
            seg.enabled = true;
            seg.manualInserted = false;
            segments.push_back(seg);
        }
    }

    return segments;
}

std::vector<StudioEvaluatedFrame> StudioTimelineAnalyzer::bakeEvaluatedFrames(
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    int targetFps,
    int totalFrames) {
    const int safeFps = std::clamp(targetFps, 1, 120);
    const int safeCount = (std::max)(0, totalFrames);
    std::vector<StudioEvaluatedFrame> out(static_cast<size_t>(safeCount));
    if (safeCount == 0) return out;

    const float srcW = static_cast<float>((std::max)(1, timeline.sourceWidth));
    const float srcH = static_cast<float>((std::max)(1, timeline.sourceHeight));
    const float centerDefaultX = srcW * 0.5f;
    const float centerDefaultY = srcH * 0.5f;

    // 高响应低相位滞后参数 (minCutoff=3.6Hz, beta=0.140)：慢速消抖丝滑，高速移动零拖尾
    OneEuroFilter2D cursorFilter(3.60, 0.140, 1.0);
    CriticallyDampedSpring1D springX(6.2);
    CriticallyDampedSpring1D springY(6.2);
    CriticallyDampedSpring1D springZoom(5.6);
    CriticallyDampedSpring1D springYaw(4.8);
    CriticallyDampedSpring1D springPitch(4.8);
    springX.reset(centerDefaultX);
    springY.reset(centerDefaultY);
    springZoom.reset(1.0);
    springYaw.reset(0.0);
    springPitch.reset(0.0);

    float anchorFocusX = centerDefaultX;
    float anchorFocusY = centerDefaultY;
    size_t sampleIdx = 0;
    size_t clickSnapCursor = 0;
    size_t clickZoomCursor = 0;
    size_t focusZoomCursor = 0;
    size_t clickRippleCursor = 0;
    size_t captionCursor = 0;

    // 针对 3~4 小时超长录屏：确保事件表严格单调有序，以双指针游标将 O(F * C) 嵌套扫描降为 O(F + C) 亚毫秒级线性推进
    const auto ensureSortedClicks = [&]() -> std::vector<StudioClickEvent> {
        if (std::is_sorted(timeline.clickEvents.begin(), timeline.clickEvents.end(),
                           [](const StudioClickEvent& a, const StudioClickEvent& b) { return a.ptsMs < b.ptsMs; })) {
            return {};
        }
        auto copy = timeline.clickEvents;
        std::sort(copy.begin(), copy.end(),
                  [](const StudioClickEvent& a, const StudioClickEvent& b) { return a.ptsMs < b.ptsMs; });
        return copy;
    }();
    const auto& clicksRef = ensureSortedClicks.empty() ? timeline.clickEvents : ensureSortedClicks;

    const auto ensureSortedFocus = [&]() -> std::vector<StudioFocusEvent> {
        if (std::is_sorted(timeline.focusEvents.begin(), timeline.focusEvents.end(),
                           [](const StudioFocusEvent& a, const StudioFocusEvent& b) { return a.ptsMs < b.ptsMs; })) {
            return {};
        }
        auto copy = timeline.focusEvents;
        std::sort(copy.begin(), copy.end(),
                  [](const StudioFocusEvent& a, const StudioFocusEvent& b) { return a.ptsMs < b.ptsMs; });
        return copy;
    }();
    const auto& focusRef = ensureSortedFocus.empty() ? timeline.focusEvents : ensureSortedFocus;

    const double dtSec = 1.0 / static_cast<double>(safeFps);

    for (int f = 0; f < safeCount; ++f) {
        const double tSec = static_cast<double>(f) * dtSec;
        const int64_t ptsMs = static_cast<int64_t>(std::llround(tSec * 1000.0));

        // 1. 插值获取当前时刻原始光标位置与光标形状
        float rawCurX = centerDefaultX;
        float rawCurY = centerDefaultY;
        StudioCursorKind curKind = StudioCursorKind::Arrow;
        if (!timeline.cursorSamples.empty()) {
            while (sampleIdx + 1 < timeline.cursorSamples.size() &&
                   timeline.cursorSamples[sampleIdx + 1].ptsMs <= ptsMs) {
                ++sampleIdx;
            }
            const auto& s0 = timeline.cursorSamples[sampleIdx];
            curKind = s0.kind;
            if (sampleIdx + 1 < timeline.cursorSamples.size()) {
                const auto& s1 = timeline.cursorSamples[sampleIdx + 1];
                const int64_t span = s1.ptsMs - s0.ptsMs;
                if (span > 0 && ptsMs >= s0.ptsMs) {
                    const float alpha = std::clamp(
                        static_cast<float>(ptsMs - s0.ptsMs) / static_cast<float>(span), 0.0f, 1.0f);
                    rawCurX = s0.x + (s1.x - s0.x) * alpha;
                    rawCurY = s0.y + (s1.y - s0.y) * alpha;
                } else {
                    rawCurX = s0.x;
                    rawCurY = s0.y;
                }
            } else {
                rawCurX = s0.x;
                rawCurY = s0.y;
            }
        }
        rawCurX = std::clamp(rawCurX, 0.0f, srcW);
        rawCurY = std::clamp(rawCurY, 0.0f, srcH);

        float smoothCurX = rawCurX;
        float smoothCurY = rawCurY;
        if (config.smoothCursorEnabled) {
            cursorFilter.filter(rawCurX, rawCurY, tSec, smoothCurX, smoothCurY);
            // Click-Snap 点击瞬间零误差磁吸收敛：在点击时刻 [-75ms, +135ms] 窗口内将平滑光标无痕收敛至真实点击点
            float bestSnapW = 0.0f;
            float snapTargetX = smoothCurX;
            float snapTargetY = smoothCurY;
            while (clickSnapCursor < clicksRef.size() && clicksRef[clickSnapCursor].ptsMs < ptsMs - 135) {
                ++clickSnapCursor;
            }
            for (size_t ci = clickSnapCursor; ci < clicksRef.size(); ++ci) {
                const auto& clk = clicksRef[ci];
                if (clk.ptsMs > ptsMs + 75) break;
                const int64_t dtMs = ptsMs - clk.ptsMs;
                if (dtMs >= -75 && dtMs <= 135) {
                    float w = 0.0f;
                    if (dtMs >= -18 && dtMs <= 45) {
                        w = 1.0f;
                    } else if (dtMs < -18) {
                        const float u = static_cast<float>(dtMs + 75) / 57.0f;
                        w = 0.5f - 0.5f * std::cos(u * 3.14159265f);
                    } else {
                        const float u = static_cast<float>(135 - dtMs) / 90.0f;
                        w = 0.5f - 0.5f * std::cos(u * 3.14159265f);
                    }
                    if (w > bestSnapW) {
                        bestSnapW = w;
                        snapTargetX = std::clamp(clk.x, 0.0f, srcW);
                        snapTargetY = std::clamp(clk.y, 0.0f, srcH);
                    }
                }
            }
            if (bestSnapW > 0.001f) {
                smoothCurX = smoothCurX + (snapTargetX - smoothCurX) * bestSnapW;
                smoothCurY = smoothCurY + (snapTargetY - smoothCurY) * bestSnapW;
                if (bestSnapW >= 0.999f) {
                    cursorFilter.snapTo(snapTargetX, snapTargetY, tSec);
                }
            }
            smoothCurX = std::clamp(smoothCurX, 0.0f, srcW);
            smoothCurY = std::clamp(smoothCurY, 0.0f, srcH);
        }

        // 2. 判定当前是否命中启用的 Auto-Zoom 区间
        float desiredZoom = 1.0f;
        float desiredCenterX = centerDefaultX;
        float desiredCenterY = centerDefaultY;
        float desiredYawDeg = 0.0f;
        float desiredPitchDeg = 0.0f;

        if (config.autoZoomEnabled && config.zoomMode != StudioZoomMode::None) {
            const StudioZoomSegment* activeSeg = nullptr;
            for (const auto& seg : timeline.zoomSegments) {
                if (seg.enabled && ptsMs >= seg.startMs && ptsMs <= seg.endMs) {
                    activeSeg = &seg;
                    break;
                }
            }
            if (activeSeg) {
                desiredZoom = std::clamp(
                    activeSeg->manualInserted ? activeSeg->targetZoom : config.defaultZoomFactor,
                    1.25f, 2.80f);
                // 查找该放大区间内最近的点击或焦点事件；手动插入的运镜块严格锚定用户设置的焦点
                float refX = activeSeg->focusX;
                float refY = activeSeg->focusY;
                if (!activeSeg->manualInserted) {
                    while (clickZoomCursor < clicksRef.size() && clicksRef[clickZoomCursor].ptsMs < activeSeg->startMs) {
                        ++clickZoomCursor;
                    }
                    while (clickZoomCursor > 0 && clicksRef[clickZoomCursor - 1].ptsMs >= activeSeg->startMs) {
                        --clickZoomCursor;
                    }
                    for (size_t ci = clickZoomCursor; ci < clicksRef.size(); ++ci) {
                        const auto& clk = clicksRef[ci];
                        if (clk.ptsMs > ptsMs + 180) break;
                        if (clk.ptsMs >= activeSeg->startMs) {
                            refX = clk.x;
                            refY = clk.y;
                        }
                    }
                    if (config.typingAutoZoom) {
                        while (focusZoomCursor < focusRef.size() && focusRef[focusZoomCursor].ptsMs < activeSeg->startMs) {
                            ++focusZoomCursor;
                        }
                        while (focusZoomCursor > 0 && focusRef[focusZoomCursor - 1].ptsMs >= activeSeg->startMs) {
                            --focusZoomCursor;
                        }
                        for (size_t fi = focusZoomCursor; fi < focusRef.size(); ++fi) {
                            const auto& foc = focusRef[fi];
                            if (foc.ptsMs > ptsMs + 120) break;
                            if (foc.ptsMs >= activeSeg->startMs) {
                                refX = foc.x;
                                refY = foc.y;
                            }
                        }
                    }
                }

                // 舒适区死区融合：光标在视口中心 38% 范围内不牵引镜头，超出时温和牵引
                const float viewHalfW = (srcW / desiredZoom) * 0.5f;
                const float viewHalfH = (srcH / desiredZoom) * 0.5f;
                const float deadX = viewHalfW * 0.38f;
                const float deadY = viewHalfH * 0.38f;

                anchorFocusX = refX;
                anchorFocusY = refY;
                if (!activeSeg->manualInserted) {
                    if (std::abs(smoothCurX - anchorFocusX) > deadX) {
                        const float sign = (smoothCurX > anchorFocusX) ? 1.0f : -1.0f;
                        anchorFocusX += (std::abs(smoothCurX - anchorFocusX) - deadX) * sign;
                    }
                    if (std::abs(smoothCurY - anchorFocusY) > deadY) {
                        const float sign = (smoothCurY > anchorFocusY) ? 1.0f : -1.0f;
                        anchorFocusY += (std::abs(smoothCurY - anchorFocusY) - deadY) * sign;
                    }
                }

                // 提前对目标中心做视口边界钳制，避免弹簧向屏幕外无效蓄力
                desiredCenterX = std::clamp(anchorFocusX, viewHalfW, (std::max)(viewHalfW, srcW - viewHalfW));
                desiredCenterY = std::clamp(anchorFocusY, viewHalfH, (std::max)(viewHalfH, srcH - viewHalfH));

                if (config.zoomMode == StudioZoomMode::Motion3D) {
                    const float normDx = std::clamp((anchorFocusX - centerDefaultX) / (srcW * 0.45f), -1.0f, 1.0f);
                    const float normDy = std::clamp((anchorFocusY - centerDefaultY) / (srcH * 0.45f), -1.0f, 1.0f);
                    if (std::abs(normDx) < 0.14f && std::abs(normDy) < 0.14f) {
                        desiredYawDeg = 3.4f;
                        desiredPitchDeg = -1.6f;
                    } else {
                        desiredYawDeg = -normDx * 6.0f;
                        desiredPitchDeg = normDy * 4.0f;
                    }
                }
            }
        }

        // 3. 临界阻尼弹簧步进
        const float curZoom = (f == 0)
            ? 1.0f
            : static_cast<float>(std::clamp(springZoom.step(desiredZoom, dtSec), 1.0, 3.0));
        const float curCx = (f == 0)
            ? centerDefaultX
            : static_cast<float>(springX.step(desiredCenterX, dtSec));
        const float curCy = (f == 0)
            ? centerDefaultY
            : static_cast<float>(springY.step(desiredCenterY, dtSec));
        const float curYaw = (f == 0)
            ? 0.0f
            : static_cast<float>(std::clamp(springYaw.step(desiredYawDeg, dtSec), -8.0, 8.0));
        const float curPitch = (f == 0)
            ? 0.0f
            : static_cast<float>(std::clamp(springPitch.step(desiredPitchDeg, dtSec), -6.0, 6.0));

        const float cropW = std::clamp(srcW / curZoom, 16.0f, srcW);
        const float cropH = std::clamp(srcH / curZoom, 16.0f, srcH);
        const float cropX = std::clamp(curCx - cropW * 0.5f, 0.0f, (std::max)(0.0f, srcW - cropW));
        const float cropY = std::clamp(curCy - cropH * 0.5f, 0.0f, (std::max)(0.0f, srcH - cropH));

        // 4. Screen Studio 级二阶物理触感按压 (Tactile Squish + Micro-Tilt) 与 SDF 光学波纹进度计算
        float clickScale = 1.0f;
        float clickTiltDeg = 0.0f;
        float rippleProgress = -1.0f;
        float rippleSrcX = smoothCurX;
        float rippleSrcY = smoothCurY;
        bool rippleRight = false;

        constexpr int64_t kTactileDurationMs = 260;
        const int64_t rippleDurationMs =
            (config.clickEffectStyle == StudioClickEffectStyle::DualRipple) ? 430
            : ((config.clickEffectStyle == StudioClickEffectStyle::SoftTouch) ? 290 : 310);
        const int64_t maxLookbackMs = (std::max)(kTactileDurationMs, rippleDurationMs);

        while (clickRippleCursor < clicksRef.size() && clicksRef[clickRippleCursor].ptsMs < ptsMs - maxLookbackMs) {
            ++clickRippleCursor;
        }
        for (size_t ci = clickRippleCursor; ci < clicksRef.size(); ++ci) {
            const auto& clk = clicksRef[ci];
            if (clk.ptsMs > ptsMs) break;
            const int64_t elapsed = ptsMs - clk.ptsMs;
            if (elapsed < 0 || elapsed > maxLookbackMs) {
                continue;
            }
            if (elapsed <= kTactileDurationMs) {
                const float u = static_cast<float>(elapsed) / static_cast<float>(kTactileDurationMs);
                if (u <= 0.24f) {
                    // 前 62ms 快速干脆点按下沉至 0.79x，同时围绕尖端 (0,0) 轻叩 -3.4 deg
                    const float k = u / 0.24f;
                    const float inv = 1.0f - k;
                    const float easeDown = 1.0f - inv * inv * inv;
                    clickScale = 1.0f - 0.21f * easeDown;
                    clickTiltDeg = -3.4f * easeDown;
                } else {
                    // 62ms..260ms 二阶欠阻尼弹簧清脆回弹 (0.79x -> ~1.045x -> 1.00x)
                    const float k = (u - 0.24f) / 0.76f;
                    clickScale = 1.0f - 0.21f * std::exp(-5.2f * k) * std::cos(4.4f * k);
                    clickTiltDeg = -3.4f * std::exp(-6.0f * k) * std::cos(3.2f * k);
                }
            }
            if (elapsed <= rippleDurationMs) {
                rippleProgress = static_cast<float>(elapsed) / static_cast<float>(rippleDurationMs);
                rippleSrcX = std::clamp(clk.x, 0.0f, srcW);
                rippleSrcY = std::clamp(clk.y, 0.0f, srcH);
                rippleRight = clk.isRight;
            }
        }

        // 5. 匹配当前时间点的字幕
        std::string captionText;
        if (config.autoCaptionsEnabled && !timeline.captions.empty()) {
            while (captionCursor + 1 < timeline.captions.size() &&
                   timeline.captions[captionCursor].endMs < ptsMs) {
                ++captionCursor;
            }
            for (size_t k = captionCursor; k < timeline.captions.size(); ++k) {
                const auto& cap = timeline.captions[k];
                if (cap.startMs > ptsMs) break;
                if (ptsMs >= cap.startMs && ptsMs <= cap.endMs) {
                    captionText = cap.text;
                    break;
                }
            }
        }

        StudioEvaluatedFrame& ef = out[static_cast<size_t>(f)];
        ef.ptsMs = ptsMs;
        ef.sourceWidth = srcW;
        ef.sourceHeight = srcH;
        ef.camera.centerX = cropX + cropW * 0.5f;
        ef.camera.centerY = cropY + cropH * 0.5f;
        ef.camera.zoom = curZoom;
        ef.camera.cropX = cropX;
        ef.camera.cropY = cropY;
        ef.camera.cropW = cropW;
        ef.camera.cropH = cropH;
        ef.camera.tiltYawDeg = (std::abs(curYaw) > 0.03f) ? curYaw : 0.0f;
        ef.camera.tiltPitchDeg = (std::abs(curPitch) > 0.03f) ? curPitch : 0.0f;
        if (f > 0) {
            const auto& prevCam = out[static_cast<size_t>(f - 1)].camera;
            const float panDist = std::hypot(ef.camera.centerX - prevCam.centerX, ef.camera.centerY - prevCam.centerY);
            const float zoomDist = std::abs(ef.camera.zoom - prevCam.zoom) * srcW * 0.35f;
            ef.cameraVelocityPx = panDist + zoomDist;
        } else {
            ef.cameraVelocityPx = 0.0f;
        }
        ef.smoothCursorX = smoothCurX;
        ef.smoothCursorY = smoothCurY;
        ef.cursorKind = curKind;
        ef.clickScale = clickScale;
        ef.clickTiltDeg = clickTiltDeg;
        const bool wantWave = config.showClickRipple &&
                              (config.clickEffectStyle != StudioClickEffectStyle::TactileOnly);
        ef.rippleProgress = wantWave ? rippleProgress : -1.0f;
        ef.rippleSrcX = rippleSrcX;
        ef.rippleSrcY = rippleSrcY;
        ef.rippleIsRight = rippleRight;
        ef.activeCaption = std::move(captionText);
    }

    return out;
}

std::vector<StudioCutSegment> StudioTimelineAnalyzer::normalizeCutSegments(
    const std::vector<StudioCutSegment>& cuts,
    int64_t totalDurationMs) {
    if (cuts.empty() || totalDurationMs <= 0) return {};
    std::vector<StudioCutSegment> sorted;
    sorted.reserve(cuts.size());
    for (const auto& c : cuts) {
        if (!c.enabled) continue;
        const int64_t s = std::clamp<int64_t>(c.startMs, 0, totalDurationMs);
        const int64_t e = std::clamp<int64_t>(c.endMs, 0, totalDurationMs);
        if (e > s + 20) {
            sorted.push_back({s, e, c.speedFactor, true});
        }
    }
    if (sorted.empty()) return {};
    std::sort(sorted.begin(), sorted.end(), [](const StudioCutSegment& a, const StudioCutSegment& b) {
        return a.startMs < b.startMs;
    });
    std::vector<StudioCutSegment> merged;
    merged.push_back(sorted.front());
    for (size_t i = 1; i < sorted.size(); ++i) {
        const bool bothHardCut = (sorted[i].speedFactor <= 0.05f && merged.back().speedFactor <= 0.05f);
        if (sorted[i].startMs < merged.back().endMs) {
            if (bothHardCut) {
                merged.back().endMs = (std::max)(merged.back().endMs, sorted[i].endMs);
            } else {
                StudioCutSegment clipped = sorted[i];
                clipped.startMs = merged.back().endMs;
                if (clipped.endMs > clipped.startMs + 20) {
                    merged.push_back(clipped);
                }
            }
        } else if (bothHardCut && sorted[i].startMs <= merged.back().endMs + 10) {
            merged.back().endMs = (std::max)(merged.back().endMs, sorted[i].endMs);
        } else {
            merged.push_back(sorted[i]);
        }
    }
    int64_t effectiveDur = totalDurationMs;
    for (const auto& m : merged) {
        const int64_t span = m.endMs - m.startMs;
        if (m.speedFactor <= 0.05f) {
            effectiveDur -= span;
        } else if (m.speedFactor > 1.01f) {
            effectiveDur -= static_cast<int64_t>(std::llround(span * (1.0 - 1.0 / m.speedFactor)));
        }
    }
    if (effectiveDur < 200) {
        if (totalDurationMs <= 200) return {};
        while (!merged.empty() && effectiveDur < 200) {
            const auto back = merged.back();
            merged.pop_back();
            const int64_t span = back.endMs - back.startMs;
            if (back.speedFactor <= 0.05f) effectiveDur += span;
            else if (back.speedFactor > 1.01f) effectiveDur += static_cast<int64_t>(std::llround(span * (1.0 - 1.0 / back.speedFactor)));
        }
    }
    return merged;
}

bool StudioTimelineAnalyzer::isTimeCutOut(
    const std::vector<StudioCutSegment>& normalizedCuts,
    int64_t srcPtsMs) noexcept {
    for (const auto& c : normalizedCuts) {
        if (srcPtsMs < c.startMs) return false;
        if (srcPtsMs >= c.startMs && srcPtsMs < c.endMs && c.speedFactor <= 0.05f) return true;
    }
    return false;
}

int64_t StudioTimelineAnalyzer::mapSourceToKeptMs(
    const std::vector<StudioCutSegment>& normalizedCuts,
    int64_t srcPtsMs) noexcept {
    double removedBefore = 0.0;
    for (const auto& c : normalizedCuts) {
        if (srcPtsMs <= c.startMs) break;
        const int64_t overlapEnd = (std::min)(srcPtsMs, c.endMs);
        const double span = static_cast<double>(overlapEnd - c.startMs);
        if (c.speedFactor <= 0.05f) {
            removedBefore += span;
        } else if (c.speedFactor > 1.01f) {
            removedBefore += span * (1.0 - 1.0 / static_cast<double>(c.speedFactor));
        }
        if (srcPtsMs < c.endMs) break;
    }
    return (std::max<int64_t>)(0, srcPtsMs - static_cast<int64_t>(std::llround(removedBefore)));
}

int64_t StudioTimelineAnalyzer::computeKeptDurationMs(
    int64_t totalDurationMs,
    const std::vector<StudioCutSegment>& cuts) noexcept {
    if (totalDurationMs <= 0) return 0;
    const auto norm = normalizeCutSegments(cuts, totalDurationMs);
    return (std::max<int64_t>)(100, mapSourceToKeptMs(norm, totalDurationMs));
}

std::vector<StudioVideoClip> StudioTimelineAnalyzer::buildClipsFromCutSegments(
    int64_t rawTotalDurationMs,
    const std::vector<StudioCutSegment>& cuts) {
    const int64_t total = (std::max<int64_t>)(100, rawTotalDurationMs);
    const auto norm = normalizeCutSegments(cuts, total);
    if (norm.empty()) {
        return {{0, total, 1.0f}};
    }
    std::vector<StudioVideoClip> clips;
    int64_t cursor = 0;
    for (const auto& c : norm) {
        if (c.startMs > cursor + 20) {
            clips.push_back({cursor, c.startMs, 1.0f});
        }
        if (c.speedFactor > 0.05f) {
            clips.push_back({c.startMs, c.endMs, c.speedFactor});
        }
        cursor = (std::max)(cursor, c.endMs);
    }
    if (cursor + 20 < total) {
        clips.push_back({cursor, total, 1.0f});
    }
    if (clips.empty()) {
        clips.push_back({0, total, 1.0f});
    }
    return clips;
}

int StudioTimelineAnalyzer::splitClipAtSourceMs(
    std::vector<StudioVideoClip>& clips,
    int64_t splitSrcMs,
    int64_t minPieceMs) {
    for (size_t i = 0; i < clips.size(); ++i) {
        const auto& c = clips[i];
        if (splitSrcMs >= c.srcStartMs + minPieceMs && splitSrcMs <= c.srcEndMs - minPieceMs) {
            StudioVideoClip rightPart{splitSrcMs, c.srcEndMs, c.speed};
            clips[i].srcEndMs = splitSrcMs;
            clips.insert(clips.begin() + static_cast<std::ptrdiff_t>(i + 1), rightPart);
            return static_cast<int>(i + 1);
        }
    }
    return -1;
}

int64_t StudioTimelineAnalyzer::computeClipsKeptDurationMs(
    const std::vector<StudioVideoClip>& clips) noexcept {
    int64_t sum = 0;
    for (const auto& c : clips) {
        if (c.srcEndMs > c.srcStartMs) {
            sum += c.keptDurationMs();
        }
    }
    return (std::max<int64_t>)(50, sum);
}

int64_t StudioTimelineAnalyzer::mapKeptMsToSourceMs(
    const std::vector<StudioVideoClip>& clips,
    int64_t keptMs,
    int* outClipIndex) noexcept {
    if (clips.empty()) {
        if (outClipIndex) *outClipIndex = -1;
        return (std::max<int64_t>)(0, keptMs);
    }
    if (keptMs <= 0) {
        if (outClipIndex) *outClipIndex = 0;
        return clips.front().srcStartMs;
    }
    int64_t accKept = 0;
    for (size_t i = 0; i < clips.size(); ++i) {
        const auto& c = clips[i];
        const int64_t kDur = c.keptDurationMs();
        if (keptMs <= accKept + kDur || i + 1 == clips.size()) {
            if (outClipIndex) *outClipIndex = static_cast<int>(i);
            const double localKept = static_cast<double>(std::clamp<int64_t>(keptMs - accKept, 0, kDur));
            const double sp = (c.speed > 0.1f) ? static_cast<double>(c.speed) : 1.0;
            const int64_t mapped = c.srcStartMs + static_cast<int64_t>(std::llround(localKept * sp));
            return std::clamp<int64_t>(mapped, c.srcStartMs, c.srcEndMs);
        }
        accKept += kDur;
    }
    if (outClipIndex) *outClipIndex = static_cast<int>(clips.size()) - 1;
    return clips.back().srcEndMs;
}

int64_t StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(
    const std::vector<StudioVideoClip>& clips,
    int64_t srcMs) noexcept {
    if (clips.empty()) return (std::max<int64_t>)(0, srcMs);
    int64_t accKept = 0;
    for (const auto& c : clips) {
        if (srcMs <= c.srcStartMs) {
            return accKept;
        }
        if (srcMs <= c.srcEndMs) {
            const double sp = (c.speed > 0.1f) ? static_cast<double>(c.speed) : 1.0;
            const int64_t deltaKept = static_cast<int64_t>(std::llround(static_cast<double>(srcMs - c.srcStartMs) / sp));
            return accKept + std::clamp<int64_t>(deltaKept, 0, c.keptDurationMs());
        }
        accKept += c.keptDurationMs();
    }
    return accKept;
}

std::vector<StudioCutSegment> StudioTimelineAnalyzer::buildCutSegmentsFromClips(
    const std::vector<StudioVideoClip>& clips,
    int64_t rawTotalDurationMs) {
    const int64_t total = (std::max<int64_t>)(100, rawTotalDurationMs);
    if (clips.empty()) return {};

    std::vector<StudioCutSegment> out;
    int64_t cursor = 0;
    for (const auto& c : clips) {
        const int64_t s = std::clamp<int64_t>(c.srcStartMs, 0, total);
        const int64_t e = std::clamp<int64_t>(c.srcEndMs, s, total);
        if (s > cursor + 15) {
            out.push_back({cursor, s, 0.0f, true});
        }
        if (e > s + 15 && std::abs(c.speed - 1.0f) > 0.03f) {
            out.push_back({s, e, c.speed, true});
        }
        cursor = (std::max)(cursor, e);
    }
    if (cursor + 15 < total) {
        out.push_back({cursor, total, 0.0f, true});
    }
    return out;
}

std::string StudioTimelineAnalyzer::formatSrtContent(
    const std::vector<StudioCaptionSegment>& captions,
    const std::vector<StudioCutSegment>& cuts) {
    int64_t maxEnd = 0;
    for (const auto& seg : captions) maxEnd = (std::max)(maxEnd, seg.endMs);
    for (const auto& c : cuts) maxEnd = (std::max)(maxEnd, c.endMs + 1000);
    const auto normCuts = normalizeCutSegments(cuts, maxEnd);

    std::ostringstream oss;
    size_t idx = 1;
    for (const auto& seg : captions) {
        if (seg.text.empty() || seg.endMs <= seg.startMs) continue;
        const int64_t mappedStart = mapSourceToKeptMs(normCuts, seg.startMs);
        const int64_t mappedEnd = mapSourceToKeptMs(normCuts, seg.endMs);
        if (mappedEnd <= mappedStart + 80) continue; // 完全落在被剪除片段内的字幕自动剥离
        oss << idx++ << "\r\n"
            << formatSrtTimestamp(mappedStart) << " --> " << formatSrtTimestamp(mappedEnd) << "\r\n"
            << seg.text << "\r\n\r\n";
    }
    return oss.str();
}

bool StudioTimelineAnalyzer::exportSrtFile(
    const std::wstring& srtPath,
    const std::vector<StudioCaptionSegment>& captions,
    const std::vector<StudioCutSegment>& cuts) {
    if (srtPath.empty() || captions.empty()) return false;
    const std::string content = formatSrtContent(captions, cuts);
    if (content.empty()) return false;
    // UTF-8 BOM + content so Windows media players & Premiere recognize Chinese captions cleanly
    std::string withBom = "\xEF\xBB\xBF" + content;
    return tools3000::common::atomicWriteBinaryFileWithFlush(
        srtPath,
        reinterpret_cast<const uint8_t*>(withBom.data()),
        withBom.size());
}

namespace {

const wchar_t* timelineAnnotationKindName(StudioAnnotationKind k, bool isZh, bool compact) noexcept {
    switch (k) {
        case StudioAnnotationKind::TextPlain:
            return isZh ? (compact ? L"文字" : L"纯文字") : (compact ? L"Text" : L"Plain Text");
        case StudioAnnotationKind::Text:
            return isZh ? (compact ? L"标签" : L"文字标签") : (compact ? L"Label" : L"Filled Label");
        case StudioAnnotationKind::TextBubble:
            return isZh ? (compact ? L"气泡" : L"对话气泡") : (compact ? L"Bubble" : L"Speech Bubble");
        case StudioAnnotationKind::TextDashed:
            return isZh ? (compact ? L"虚线框" : L"虚线文本框") : (compact ? L"Dashed" : L"Dashed Box");
        case StudioAnnotationKind::TextNeon:
            return isZh ? (compact ? L"发光框" : L"发光文本框") : (compact ? L"Glow" : L"Glow Callout");
        case StudioAnnotationKind::StepBadge:
            return isZh ? (compact ? L"步骤" : L"步骤序号") : (compact ? L"Step" : L"Step Number");
        case StudioAnnotationKind::GlowLine:
            return isZh ? (compact ? L"划线" : L"荧光划线") : (compact ? L"Line" : L"Highlighter");
        case StudioAnnotationKind::DashedLine:
            return isZh ? L"虚线" : (compact ? L"Dashed" : L"Dashed Line");
        case StudioAnnotationKind::Arrow:
            return isZh ? L"箭头" : L"Arrow";
        case StudioAnnotationKind::Rectangle:
            return isZh ? (compact ? L"矩形" : L"矩形框") : (compact ? L"Rect" : L"Rectangle");
        case StudioAnnotationKind::RoundedBox:
            return isZh ? (compact ? L"圆角" : L"圆角框") : (compact ? L"Round" : L"Rounded Box");
        case StudioAnnotationKind::CircleRing:
            return isZh ? L"圆环" : L"Circle";
        case StudioAnnotationKind::SketchCircle:
            return isZh ? (compact ? L"重点圈" : L"手绘重点圈") : (compact ? L"Marker" : L"Marker Circle");
        case StudioAnnotationKind::BoldArrow:
            return isZh ? (compact ? L"大箭头" : L"醒目大箭头") : (compact ? L"Arrow" : L"Bold Arrow");
        case StudioAnnotationKind::HandPointer:
            return isZh ? (compact ? L"手势" : L"点击手势") : (compact ? L"Hand" : L"Click Hand");
        case StudioAnnotationKind::Spotlight:
            return isZh ? (compact ? L"聚光灯" : L"聚光灯高亮") : (compact ? L"Spot" : L"Spotlight");
        case StudioAnnotationKind::BlurMask:
            return isZh ? (compact ? L"马赛克" : L"马赛克打码") : (compact ? L"Mosaic" : L"Mosaic Blur");
        case StudioAnnotationKind::Magnifier:
            return isZh ? (compact ? L"放大镜" : L"局部放大镜") : (compact ? L"Loupe" : L"Magnifier");
        default:
            return isZh ? L"标注" : L"Mark";
    }
}

bool isTimelineTextLikeKind(StudioAnnotationKind k) noexcept {
    return k == StudioAnnotationKind::Text ||
           k == StudioAnnotationKind::TextPlain ||
           k == StudioAnnotationKind::TextBubble ||
           k == StudioAnnotationKind::TextDashed ||
           k == StudioAnnotationKind::TextNeon ||
           k == StudioAnnotationKind::StepBadge;
}

} // namespace

int StudioTimelineAnalyzer::resequenceAutoStepBadges(
    std::vector<StudioAnnotationItem>& annotations,
    const std::vector<StudioVideoClip>& clips,
    std::vector<int>* outStepOrderRanks) {
    if (outStepOrderRanks) {
        outStepOrderRanks->assign(annotations.size(), 0);
    }

    struct StepOrderRef {
        size_t index = 0;
        int64_t keptStartMs = 0;
    };
    std::vector<StepOrderRef> activeSteps;
    activeSteps.reserve(annotations.size());

    for (size_t i = 0; i < annotations.size(); ++i) {
        const auto& ann = annotations[i];
        if (!ann.enabled || ann.kind != StudioAnnotationKind::StepBadge) continue;
        const int64_t ks = clips.empty()
            ? (std::max<int64_t>)(0, ann.startMs)
            : mapSourceMsToKeptMsViaClips(clips, ann.startMs);
        const int64_t ke = clips.empty()
            ? (std::max<int64_t>)(0, ann.endMs)
            : mapSourceMsToKeptMsViaClips(clips, ann.endMs);
        if (ke <= ks + 30) continue;
        activeSteps.push_back({i, ks});
    }

    std::stable_sort(activeSteps.begin(), activeSteps.end(), [](const StepOrderRef& a, const StepOrderRef& b) {
        if (a.keptStartMs != b.keptStartMs) return a.keptStartMs < b.keptStartMs;
        return a.index < b.index;
    });

    int seq = 1;
    for (const auto& ref : activeSteps) {
        auto& ann = annotations[ref.index];
        if (!ann.customStepText) {
            ann.text = std::to_string(seq);
        }
        if (outStepOrderRanks) {
            (*outStepOrderRanks)[ref.index] = seq;
        }
        ++seq;
    }
    return seq;
}

std::wstring StudioTimelineAnalyzer::formatTimelineAnnotationLabel(
    const StudioAnnotationItem& ann,
    int stepOrderRank,
    float availWidthPx,
    bool forTooltip,
    bool isZh) {
    const std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(ann.text);
    if (ann.kind == StudioAnnotationKind::StepBadge) {
        const int rank = (stepOrderRank > 0) ? stepOrderRank : 1;
        const std::wstring badgeShown = wText.empty() ? std::to_wstring(rank) : wText;
        if (forTooltip) {
            wchar_t buf[96]{};
            if (ann.customStepText && badgeShown != std::to_wstring(rank)) {
                std::swprintf(buf, std::size(buf),
                              isZh ? L"步骤序号 · 第 %d 个 (显示 %ls)" : L"Step #%d (%ls)",
                              rank, badgeShown.c_str());
            } else {
                std::swprintf(buf, std::size(buf),
                              isZh ? L"步骤序号 · 第 %d 个" : L"Step #%d",
                              rank);
            }
            return buf;
        }
        if (availWidthPx >= 64.0f) {
            wchar_t buf[64]{};
            if (ann.customStepText && badgeShown != std::to_wstring(rank)) {
                std::swprintf(buf, std::size(buf), isZh ? L"步骤 %d·%ls" : L"Step %d·%ls", rank, badgeShown.c_str());
            } else {
                std::swprintf(buf, std::size(buf), isZh ? L"步骤 %d" : L"Step %d", rank);
            }
            return buf;
        }
        return L"#" + badgeShown;
    }

    if (isTimelineTextLikeKind(ann.kind)) {
        const wchar_t* fullKind = timelineAnnotationKindName(ann.kind, isZh, false);
        const wchar_t* shortKind = timelineAnnotationKindName(ann.kind, isZh, true);
        if (forTooltip) {
            return wText.empty() ? std::wstring(fullKind) : (std::wstring(fullKind) + L": " + wText);
        }
        if (availWidthPx >= 92.0f && !wText.empty()) {
            return std::wstring(shortKind) + L": " + wText;
        }
        if (!wText.empty()) {
            return wText;
        }
        return shortKind;
    }

    return timelineAnnotationKindName(ann.kind, isZh, !forTooltip && availWidthPx < 74.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// StudioTimelineRecorder — 250Hz 轨迹焦点采集与本地 SAPI 离线语音字幕线程
// ─────────────────────────────────────────────────────────────────────────────

int64_t StudioTimelineRecorder::currentElapsedTimelineMs() const noexcept {
    std::lock_guard lock(m_clockMutex);
    const auto now = std::chrono::steady_clock::now();
    const auto activeNow = m_paused.load(std::memory_order_acquire) ? m_pauseStartTick : now;
    const auto rawMs = std::chrono::duration_cast<std::chrono::milliseconds>(activeNow - m_startTick).count();
    return (std::max<int64_t>)(0, rawMs - m_accumulatedPausedMs);
}

void StudioTimelineRecorder::start(
    int regionX,
    int regionY,
    int width,
    int height,
    int fps,
    bool captureSpeechCaptions,
    std::chrono::steady_clock::time_point epochOverride) {
    stop();

    m_regionX = regionX;
    m_regionY = regionY;
    m_width = (std::max)(1, width);
    m_height = (std::max)(1, height);
    m_fps = std::clamp(fps, 1, 120);
    m_enableSpeech = captureSpeechCaptions;

    {
        std::lock_guard lock(m_clockMutex);
        m_startTick = (epochOverride != std::chrono::steady_clock::time_point{})
            ? epochOverride
            : std::chrono::steady_clock::now();
        m_pauseStartTick = m_startTick;
        m_accumulatedPausedMs = 0;
    }
    {
        std::lock_guard lock(m_dataMutex);
        m_data = StudioTimelineData{};
        m_data.regionX = m_regionX;
        m_data.regionY = m_regionY;
        m_data.sourceWidth = m_width;
        m_data.sourceHeight = m_height;
        m_data.fps = m_fps;
        m_data.cursorSamples.reserve(16384);
    }

    m_latestTypingPtsMs.store(-1, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_typingStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_typingWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_samplerThread = std::thread(&StudioTimelineRecorder::samplingLoop, this);
    if (m_typingStopEvent && m_typingWakeEvent) {
        m_typingFocusThread = std::thread(&StudioTimelineRecorder::typingFocusLoop, this);
    }

    if (m_enableSpeech) {
        m_speechStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (m_speechStopEvent) {
            m_speechThread = std::thread(&StudioTimelineRecorder::speechRecognitionLoop, this);
        }
    }
}

void StudioTimelineRecorder::setPaused(bool paused) noexcept {
    std::lock_guard lock(m_clockMutex);
    const bool wasPaused = m_paused.load(std::memory_order_acquire);
    if (paused == wasPaused) return;
    const auto now = std::chrono::steady_clock::now();
    if (paused) {
        m_pauseStartTick = now;
        m_paused.store(true, std::memory_order_release);
    } else {
        m_accumulatedPausedMs +=
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_pauseStartTick).count();
        m_paused.store(false, std::memory_order_release);
    }
}

void StudioTimelineRecorder::stop() {
    m_running.store(false, std::memory_order_release);
    if (m_typingStopEvent) {
        SetEvent(m_typingStopEvent);
    }
    if (m_speechStopEvent) {
        SetEvent(m_speechStopEvent);
    }
    if (m_samplerThread.joinable()) {
        m_samplerThread.join();
    }
    if (m_typingFocusThread.joinable()) {
        m_typingFocusThread.join();
    }
    if (m_speechThread.joinable()) {
        m_speechThread.join();
    }
    if (m_typingStopEvent) {
        CloseHandle(m_typingStopEvent);
        m_typingStopEvent = nullptr;
    }
    if (m_typingWakeEvent) {
        CloseHandle(m_typingWakeEvent);
        m_typingWakeEvent = nullptr;
    }
    if (m_speechStopEvent) {
        CloseHandle(m_speechStopEvent);
        m_speechStopEvent = nullptr;
    }
}

StudioTimelineData StudioTimelineRecorder::takeTimelineData(
    int64_t finalDurationMs,
    float defaultZoom,
    bool includeTypingFocus) {
    stop();
    std::lock_guard lock(m_dataMutex);
    m_data.durationMs = (std::max<int64_t>)(finalDurationMs,
        m_data.cursorSamples.empty() ? 0 : m_data.cursorSamples.back().ptsMs);
    m_data.zoomSegments = StudioTimelineAnalyzer::buildAutoZoomSegments(
        m_data, defaultZoom, includeTypingFocus);
    return std::move(m_data);
}

void StudioTimelineRecorder::samplingLoop() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool prevLeft = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool prevRight = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const DWORD selfPid = GetCurrentProcessId();

    struct PendingClickTracker {
        bool active = false;
        int64_t downPts = 0;
        float downX = 0.0f;
        float downY = 0.0f;
        float maxDistSq = 0.0f;
    };
    PendingClickTracker leftPress{}, rightPress{};

    POINT prevCaretScreen{LONG_MIN, LONG_MIN};
    int caretPollTick = 0;

    // 供零阻塞检测真实键盘文字输入（排除 Ctrl/Alt/Win 组合快捷键）的扫描键表
    static constexpr int kTypingVkList[] = {
        'A','B','C','D','E','F','G','H','I','J','K','L','M',
        'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
        '0','1','2','3','4','5','6','7','8','9',
        VK_SPACE, VK_BACK, VK_RETURN,
        VK_NUMPAD0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4,
        VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9,
        VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
        VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7
    };
    std::array<bool, std::size(kTypingVkList)> prevTypingDown{};
    for (size_t ki = 0; ki < prevTypingDown.size(); ++ki) {
        prevTypingDown[ki] = (GetAsyncKeyState(kTypingVkList[ki]) & 0x8000) != 0;
    }
    int64_t prevKeystrokePts = -5000;
    int sustainedBurstCount = 0;

    while (m_running.load(std::memory_order_acquire)) {
        if (m_paused.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            prevLeft = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            prevRight = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            leftPress.active = false;
            rightPress.active = false;
            continue;
        }

        const int64_t pts = currentElapsedTimelineMs();
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        POINT pt{};
        StudioCursorKind kind = StudioCursorKind::Arrow;
        const bool hasCi = (GetCursorInfo(&ci) != FALSE);
        if (hasCi) {
            kind = (ci.flags & CURSOR_SHOWING) ? classifySystemCursor(ci.hCursor) : StudioCursorKind::Hidden;
        }
        // 优先使用 GetPhysicalCursorPos 确保在任何多屏混合高 DPI 缩放下严格获取真实物理桌面像素坐标
        if (!GetPhysicalCursorPos(&pt)) {
            if (hasCi) {
                pt = ci.ptScreenPos;
            } else {
                GetCursorPos(&pt);
            }
        }

        const float relX = static_cast<float>(pt.x - m_regionX);
        const float relY = static_cast<float>(pt.y - m_regionY);
        const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

        auto isOnSelfRecordingOverlay = [&]() -> bool {
            HWND hAt = WindowFromPoint(pt);
            if (!hAt) return false;
            DWORD wndPid = 0;
            GetWindowThreadProcessId(hAt, &wndPid);
            if (wndPid != selfPid) return false;
            wchar_t cls[64]{};
            if (GetClassNameW(hAt, cls, 64) > 0) {
                // 豁免点击录制指示器、倒计时卡片或快捷键浮层的自身控制操作
                if (wcsstr(cls, L"Recording") != nullptr || wcsstr(cls, L"ShortcutHint") != nullptr) {
                    return true;
                }
            }
            return false;
        };

        const bool inBounds = (relX >= 0.0f && relX <= static_cast<float>(m_width) &&
                               relY >= 0.0f && relY <= static_cast<float>(m_height));

        if (leftDown && !prevLeft) {
            if (inBounds && !isOnSelfRecordingOverlay()) {
                leftPress = {true, pts, relX, relY, 0.0f};
            } else {
                leftPress.active = false;
            }
        } else if (leftDown && leftPress.active) {
            const float dx = relX - leftPress.downX;
            const float dy = relY - leftPress.downY;
            leftPress.maxDistSq = (std::max)(leftPress.maxDistSq, dx * dx + dy * dy);
        }

        if (rightDown && !prevRight) {
            if (inBounds && !isOnSelfRecordingOverlay()) {
                rightPress = {true, pts, relX, relY, 0.0f};
            } else {
                rightPress.active = false;
            }
        } else if (rightDown && rightPress.active) {
            const float dx = relX - rightPress.downX;
            const float dy = relY - rightPress.downY;
            rightPress.maxDistSq = (std::max)(rightPress.maxDistSq, dx * dx + dy * dy);
        }

        {
            std::lock_guard lock(m_dataMutex);
            // 3~4 小时超长录屏内存保护：过滤静止冗余点并对超长轨迹保拐点流式抽稀封顶 (<= 131072 点，恒定 < 3MB)
            bool shouldPushSample = true;
            if (!m_data.cursorSamples.empty()) {
                const auto& prevS = m_data.cursorSamples.back();
                const float moveSq = (relX - prevS.x) * (relX - prevS.x) + (relY - prevS.y) * (relY - prevS.y);
                if (moveSq < 0.25f &&
                    prevS.kind == kind &&
                    prevS.leftDown == leftDown &&
                    prevS.rightDown == rightDown &&
                    (pts - prevS.ptsMs) < 180) {
                    shouldPushSample = false;
                }
            }
            if (shouldPushSample) {
                if (m_data.cursorSamples.size() >= 131072) {
                    size_t dst = 0;
                    for (size_t i = 0; i < m_data.cursorSamples.size(); ++i) {
                        const bool keep = ((i % 2) == 0) ||
                            (i + 1 == m_data.cursorSamples.size()) ||
                            (i > 0 && (m_data.cursorSamples[i].leftDown != m_data.cursorSamples[i - 1].leftDown ||
                                       m_data.cursorSamples[i].rightDown != m_data.cursorSamples[i - 1].rightDown));
                        if (keep) {
                            m_data.cursorSamples[dst++] = m_data.cursorSamples[i];
                        }
                    }
                    m_data.cursorSamples.resize(dst);
                }
                m_data.cursorSamples.push_back({pts, relX, relY, kind, leftDown, rightDown});
            }
            // 高分屏 (200%+ DPI) 适配：允许按压轻滑位移 <= 32px 且按压时长 <= 680ms 判定为真实意图点击
            constexpr float kMaxClickDriftSq = 32.0f * 32.0f;
            constexpr int64_t kMaxClickHoldMs = 680;
            if (!leftDown && prevLeft && leftPress.active) {
                if (leftPress.maxDistSq <= kMaxClickDriftSq && (pts - leftPress.downPts) <= kMaxClickHoldMs) {
                    m_data.clickEvents.push_back({leftPress.downPts, leftPress.downX, leftPress.downY, false});
                }
                leftPress.active = false;
            }
            if (!rightDown && prevRight && rightPress.active) {
                if (rightPress.maxDistSq <= kMaxClickDriftSq && (pts - rightPress.downPts) <= kMaxClickHoldMs) {
                    m_data.clickEvents.push_back({rightPress.downPts, rightPress.downX, rightPress.downY, true});
                }
                rightPress.active = false;
            }
        }
        prevLeft = leftDown;
        prevRight = rightDown;

        // 1) 每 20ms 非阻塞检测真实连续打字按键边沿，唤醒独立 MTA UIA 聚焦线程定位现代编辑器/浏览器/IM 打字焦点
        if ((++caretPollTick % 10) == 0) {
            const bool shortcutModHeld =
                ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
                ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) ||
                ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
            bool anyNewKeystroke = false;
            for (size_t ki = 0; ki < prevTypingDown.size(); ++ki) {
                const bool down = (GetAsyncKeyState(kTypingVkList[ki]) & 0x8000) != 0;
                if (down && !prevTypingDown[ki] && !shortcutModHeld) {
                    anyNewKeystroke = true;
                }
                prevTypingDown[ki] = down;
            }
            if (anyNewKeystroke) {
                if (pts - prevKeystrokePts <= 1800) {
                    ++sustainedBurstCount;
                } else {
                    sustainedBurstCount = 1;
                }
                prevKeystrokePts = pts;
                // 连续敲击 >= 2 键确认属于真实文字输入（过滤单按空格暂停视频等孤立键）
                if (sustainedBurstCount >= 2) {
                    m_latestTypingPtsMs.store(pts, std::memory_order_release);
                    if (m_typingWakeEvent) {
                        SetEvent(m_typingWakeEvent);
                    }
                }
            }
        }

        // 2) 每 60ms 轻量探测传统 Win32 插入符位置（兼容经典记事本/ERP 输入框）
        if ((caretPollTick % 30) == 0) {
            HWND fg = GetForegroundWindow();
            if (fg) {
                const DWORD tid = GetWindowThreadProcessId(fg, nullptr);
                GUITHREADINFO gti{};
                gti.cbSize = sizeof(gti);
                if (tid != 0 && GetGUIThreadInfo(tid, &gti) && gti.hwndCaret &&
                    (gti.rcCaret.right > gti.rcCaret.left || gti.rcCaret.bottom > gti.rcCaret.top)) {
                    POINT caretPt{
                        (gti.rcCaret.left + gti.rcCaret.right) / 2,
                        (gti.rcCaret.top + gti.rcCaret.bottom) / 2
                    };
                    if (ClientToScreen(gti.hwndCaret, &caretPt)) {
                        const float cx = static_cast<float>(caretPt.x - m_regionX);
                        const float cy = static_cast<float>(caretPt.y - m_regionY);
                        if (cx >= 0.0f && cx <= static_cast<float>(m_width) &&
                            cy >= 0.0f && cy <= static_cast<float>(m_height) &&
                            (std::abs(caretPt.x - prevCaretScreen.x) > 8 ||
                             std::abs(caretPt.y - prevCaretScreen.y) > 8)) {
                            prevCaretScreen = caretPt;
                            std::lock_guard lock(m_dataMutex);
                            m_data.focusEvents.push_back({pts, cx, cy});
                        }
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2)); // ~500Hz 高精度采样
    }
}

void StudioTimelineRecorder::typingFocusLoop() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hrCo);

    IUIAutomation* uia = nullptr;
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                     __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));

    HANDLE handles[2] = {m_typingStopEvent, m_typingWakeEvent};
    int64_t lastEmittedPts = -1000;

    while (m_running.load(std::memory_order_acquire)) {
        const DWORD wr = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0 || !m_running.load(std::memory_order_acquire)) {
            break;
        }
        if (m_paused.load(std::memory_order_acquire)) {
            continue;
        }
        const int64_t pts = m_latestTypingPtsMs.load(std::memory_order_acquire);
        if (pts < 0 || (pts - lastEmittedPts) < 280) {
            continue; // 280ms 节流合并连续打字聚焦锚点，保持平滑连续放大不抖动
        }

        bool resolved = false;
        float focusRelX = 0.0f;
        float focusRelY = 0.0f;

        auto acceptScreenPoint = [&](double sx, double sy) -> bool {
            const float rx = static_cast<float>(sx - static_cast<double>(m_regionX));
            const float ry = static_cast<float>(sy - static_cast<double>(m_regionY));
            if (rx >= 0.0f && rx <= static_cast<float>(m_width) &&
                ry >= 0.0f && ry <= static_cast<float>(m_height)) {
                focusRelX = rx;
                focusRelY = ry;
                return true;
            }
            return false;
        };

        // 第 1 级 & 第 2 级：UI Automation TextPattern 精确闪烁光标坐标 / 焦点输入框包围盒
        // 通杀 VS Code / Antigravity / Chrome / Edge / Word / 微信 / 飞书 / WinUI 3
        if (uia) {
            IUIAutomationElement* focusedEl = nullptr;
            if (SUCCEEDED(uia->GetFocusedElement(&focusedEl)) && focusedEl) {
                IUIAutomationTextPattern* textPat = nullptr;
                if (SUCCEEDED(focusedEl->GetCurrentPatternAs(
                        UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
                        reinterpret_cast<void**>(&textPat))) && textPat) {
                    IUIAutomationTextRangeArray* selArr = nullptr;
                    if (SUCCEEDED(textPat->GetSelection(&selArr)) && selArr) {
                        int selLen = 0;
                        if (SUCCEEDED(selArr->get_Length(&selLen)) && selLen > 0) {
                            IUIAutomationTextRange* range0 = nullptr;
                            if (SUCCEEDED(selArr->GetElement(0, &range0)) && range0) {
                                SAFEARRAY* rectSa = nullptr;
                                if (SUCCEEDED(range0->GetBoundingRectangles(&rectSa)) && rectSa) {
                                    LONG lBound = 0, uBound = -1;
                                    SafeArrayGetLBound(rectSa, 1, &lBound);
                                    SafeArrayGetUBound(rectSa, 1, &uBound);
                                    if (uBound - lBound + 1 >= 4) {
                                        double* vals = nullptr;
                                        if (SUCCEEDED(SafeArrayAccessData(rectSa, reinterpret_cast<void**>(&vals))) && vals) {
                                            const double rl = vals[0], rt = vals[1], rw = vals[2], rh = vals[3];
                                            SafeArrayUnaccessData(rectSa);
                                            if ((rw >= 0.0 && rh > 1.0) &&
                                                acceptScreenPoint(rl + (std::min)(rw * 0.5, 24.0), rt + rh * 0.5)) {
                                                resolved = true;
                                            }
                                        }
                                    }
                                    SafeArrayDestroy(rectSa);
                                }
                                range0->Release();
                            }
                        }
                        selArr->Release();
                    }
                    textPat->Release();
                }

                // 若该输入控件未暴露 TextPattern（如部分聊天框/单行 input），读取焦点控件自身边界框
                if (!resolved) {
                    RECT brc{};
                    if (SUCCEEDED(focusedEl->get_CurrentBoundingRectangle(&brc))) {
                        const double bw = static_cast<double>(brc.right - brc.left);
                        const double bh = static_cast<double>(brc.bottom - brc.top);
                        const double regionArea = static_cast<double>(m_width) * static_cast<double>(m_height);
                        // 仅当焦点控件确实是具体输入框/编辑子区（面积 <= 选区 52%）时才取其输入起始视点
                        if (bw >= 12.0 && bh >= 10.0 && (bw * bh) <= regionArea * 0.52) {
                            const double targetX = static_cast<double>(brc.left) + (std::min)(bw * 0.35, 160.0);
                            const double targetY = static_cast<double>(brc.top) + (std::min)(bh * 0.5, 120.0);
                            if (acceptScreenPoint(targetX, targetY)) {
                                resolved = true;
                            }
                        }
                    }
                }
                focusedEl->Release();
            }
        }

        // 第 3 级兜底（无 UIA 的自绘 Canvas / 终端 / 远控编辑器）：
        // 若用户刚在选区内点击定位了输入框（8s 内）或鼠标正呈 IBeam 编辑指针停留在选区内并持续打字，锁定该编辑点持续放大
        if (!resolved) {
            std::lock_guard lock(m_dataMutex);
            if (!m_data.clickEvents.empty()) {
                const auto& lastClk = m_data.clickEvents.back();
                if (pts - lastClk.ptsMs <= 8000 &&
                    lastClk.x >= 0.0f && lastClk.x <= static_cast<float>(m_width) &&
                    lastClk.y >= 0.0f && lastClk.y <= static_cast<float>(m_height)) {
                    focusRelX = lastClk.x;
                    focusRelY = lastClk.y;
                    resolved = true;
                }
            }
            if (!resolved && !m_data.cursorSamples.empty()) {
                const auto& lastCur = m_data.cursorSamples.back();
                if (lastCur.kind == StudioCursorKind::IBeam &&
                    lastCur.x >= 0.0f && lastCur.x <= static_cast<float>(m_width) &&
                    lastCur.y >= 0.0f && lastCur.y <= static_cast<float>(m_height)) {
                    focusRelX = lastCur.x;
                    focusRelY = lastCur.y;
                    resolved = true;
                }
            }
            if (resolved) {
                m_data.focusEvents.push_back({pts, focusRelX, focusRelY});
                lastEmittedPts = pts;
                continue;
            }
        }

        if (resolved) {
            std::lock_guard lock(m_dataMutex);
            m_data.focusEvents.push_back({pts, focusRelX, focusRelY});
            lastEmittedPts = pts;
        }
    }

    if (uia) {
        uia->Release();
    }
    if (needUninit) {
        CoUninitialize();
    }
}

void StudioTimelineRecorder::speechRecognitionLoop() {
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hrCo);

    ISpRecognizer* recognizer = nullptr;
    ISpObjectTokenCategory* audioCategory = nullptr;
    ISpObjectToken* defaultAudioIn = nullptr;
    ISpRecoContext* recoCtx = nullptr;
    ISpRecoGrammar* grammar = nullptr;

    auto cleanupCom = [&]() {
        if (grammar) { grammar->Release(); grammar = nullptr; }
        if (recoCtx) { recoCtx->Release(); recoCtx = nullptr; }
        if (defaultAudioIn) { defaultAudioIn->Release(); defaultAudioIn = nullptr; }
        if (audioCategory) { audioCategory->Release(); audioCategory = nullptr; }
        if (recognizer) { recognizer->Release(); recognizer = nullptr; }
        if (needUninit) CoUninitialize();
    };

    if (FAILED(CoCreateInstance(__uuidof(SpInprocRecognizer), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(ISpRecognizer), reinterpret_cast<void**>(&recognizer))) || !recognizer) {
        cleanupCom();
        return;
    }

    if (FAILED(CoCreateInstance(__uuidof(SpObjectTokenCategory), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(ISpObjectTokenCategory), reinterpret_cast<void**>(&audioCategory))) ||
        !audioCategory ||
        FAILED(audioCategory->SetId(SPCAT_AUDIOIN, FALSE))) {
        cleanupCom();
        return;
    }

    WCHAR* defaultTokenId = nullptr;
    if (FAILED(audioCategory->GetDefaultTokenId(&defaultTokenId)) || !defaultTokenId) {
        cleanupCom();
        return;
    }
    HRESULT hrTok = CoCreateInstance(__uuidof(SpObjectToken), nullptr, CLSCTX_INPROC_SERVER,
                                     __uuidof(ISpObjectToken), reinterpret_cast<void**>(&defaultAudioIn));
    if (SUCCEEDED(hrTok) && defaultAudioIn) {
        hrTok = defaultAudioIn->SetId(nullptr, defaultTokenId, FALSE);
    }
    CoTaskMemFree(defaultTokenId);
    if (FAILED(hrTok) || !defaultAudioIn || FAILED(recognizer->SetInput(defaultAudioIn, TRUE))) {
        cleanupCom();
        return;
    }

    if (FAILED(recognizer->CreateRecoContext(&recoCtx)) || !recoCtx) {
        cleanupCom();
        return;
    }

    const ULONGLONG interest = SPFEI(SPEI_SOUND_START) | SPFEI(SPEI_RECOGNITION);
    if (FAILED(recoCtx->SetInterest(interest, interest)) ||
        FAILED(recoCtx->SetNotifyWin32Event())) {
        cleanupCom();
        return;
    }

    HANDLE recoEvent = recoCtx->GetNotifyEventHandle();
    if (!recoEvent || recoEvent == INVALID_HANDLE_VALUE ||
        FAILED(recoCtx->CreateGrammar(1, &grammar)) || !grammar ||
        FAILED(grammar->LoadDictation(nullptr, SPLO_STATIC)) ||
        FAILED(grammar->SetDictationState(SPRS_ACTIVE))) {
        cleanupCom();
        return;
    }

    int64_t currentUtteranceStartMs = -1;
    HANDLE handles[2] = {m_speechStopEvent, recoEvent};

    while (m_running.load(std::memory_order_acquire)) {
        const DWORD waitRes = WaitForMultipleObjects(2, handles, FALSE, 250);
        if (waitRes == WAIT_OBJECT_0 || !m_running.load(std::memory_order_acquire)) {
            break;
        }
        if (waitRes != WAIT_OBJECT_0 + 1) {
            continue;
        }

        SPEVENT evt{};
        ULONG fetched = 0;
        while (SUCCEEDED(recoCtx->GetEvents(1, &evt, &fetched)) && fetched > 0) {
            if (evt.eEventId == SPEI_SOUND_START) {
                currentUtteranceStartMs = currentElapsedTimelineMs();
            } else if (evt.eEventId == SPEI_RECOGNITION && evt.lParam != 0) {
                auto* recoResult = reinterpret_cast<ISpRecoResult*>(evt.lParam);
                WCHAR* recognizedWide = nullptr;
                if (SUCCEEDED(recoResult->GetText(
                        static_cast<ULONG>(SP_GETWHOLEPHRASE),
                        static_cast<ULONG>(SP_GETWHOLEPHRASE),
                        TRUE, &recognizedWide, nullptr)) && recognizedWide) {
                    std::string utf8 = tools3000::core::WinUtils::wstringToUtf8(recognizedWide);
                    CoTaskMemFree(recognizedWide);
                    if (!utf8.empty() && !m_paused.load(std::memory_order_acquire)) {
                        const int64_t endMs = currentElapsedTimelineMs();
                        const int64_t estDuration = (std::max<int64_t>)(900, static_cast<int64_t>(utf8.size()) * 90);
                        const int64_t startMs = (currentUtteranceStartMs >= 0 && currentUtteranceStartMs < endMs)
                            ? currentUtteranceStartMs
                            : (std::max<int64_t>)(0, endMs - estDuration);
                        std::lock_guard lock(m_dataMutex);
                        m_data.captions.push_back({startMs, (std::max)(startMs + 600, endMs + 250), std::move(utf8)});
                    }
                }
                currentUtteranceStartMs = -1;
            }
            if (evt.elParamType == SPET_LPARAM_IS_OBJECT && evt.lParam != 0) {
                reinterpret_cast<IUnknown*>(evt.lParam)->Release();
            } else if (evt.elParamType == SPET_LPARAM_IS_POINTER && evt.lParam != 0) {
                CoTaskMemFree(reinterpret_cast<void*>(evt.lParam));
            } else if (evt.elParamType == SPET_LPARAM_IS_STRING && evt.lParam != 0) {
                CoTaskMemFree(reinterpret_cast<void*>(evt.lParam));
            }
        }
    }

    // 立即将 SAPI 识别器置为 SPRST_INACTIVE_WITH_PURGE 丢弃未决音频缓冲，避免等待尾静音解码超时导致停止卡顿 10s+
    if (recognizer) {
        recognizer->SetRecoState(SPRST_INACTIVE_WITH_PURGE);
    }
    if (grammar) {
        grammar->SetDictationState(SPRS_INACTIVE);
    }
    cleanupCom();
}

float StudioTimelineAnalyzer::sampleWaveformRangePeak(
    const std::vector<float>& peaks,
    int64_t totalDurationMs,
    int64_t rangeStartMs,
    int64_t rangeEndMs,
    float referencePeak,
    float noiseFloor) noexcept {
    if (peaks.empty() || totalDurationMs <= 0) {
        return 0.0f;
    }
    const int64_t sMs = std::clamp<int64_t>(rangeStartMs, 0, totalDurationMs);
    const int64_t eMs = std::clamp<int64_t>((std::max)(sMs + 1, rangeEndMs), 0, totalDurationMs);
    const size_t n = peaks.size();
    const size_t i0 = std::min<size_t>(
        n - 1,
        static_cast<size_t>((static_cast<double>(sMs) / static_cast<double>(totalDurationMs)) * static_cast<double>(n)));
    const size_t i1 = std::min<size_t>(
        n - 1,
        static_cast<size_t>(std::ceil((static_cast<double>(eMs) / static_cast<double>(totalDurationMs)) * static_cast<double>(n) - 1e-6)));

    const size_t endIdx = (std::max)(i0, i1);
    float maxVal = 0.0f;
    for (size_t i = i0; i <= endIdx; ++i) {
        maxVal = (std::max)(maxVal, peaks[i]);
    }

    const float safeFloor = std::clamp(noiseFloor, 0.0f, 0.25f);
    if (maxVal <= safeFloor + 0.004f) {
        return 0.0f;
    }

    const float clean = maxVal - safeFloor;
    const float effSpan = (std::max)(0.18f, referencePeak - safeFloor);
    const float norm = std::clamp(clean / effSpan, 0.0f, 1.0f);
    return std::clamp(std::pow(norm, 0.92f), 0.0f, 1.0f);
}

StudioWaveformEnvelopeConfig StudioTimelineAnalyzer::analyzeWaveformProfile(
    const std::vector<float>& peaks) noexcept {
    StudioWaveformEnvelopeConfig cfg{};
    if (peaks.empty()) return cfg;

    std::vector<float> active;
    active.reserve(peaks.size());
    for (float v : peaks) {
        if (v >= 0.014f) {
            active.push_back(v);
        }
    }
    if (active.empty()) {
        return cfg;
    }

    std::sort(active.begin(), active.end());
    const size_t n = active.size();
    const float actP12 = active[std::min<size_t>(n - 1, static_cast<size_t>(std::floor((n - 1) * 0.12)))];
    const float actP98 = active[std::min<size_t>(n - 1, static_cast<size_t>(std::floor((n - 1) * 0.98)))];
    if (actP98 < 0.014f) {
        return cfg;
    }

    cfg.hasAudio = true;
    if (actP98 < 0.115f && (actP98 - actP12) < 0.022f) {
        // 恒定微弱麦克风风扇/电流本底底噪：压在零轴微澜，杜绝平顶毛毛虫
        cfg.noiseFloor = actP12 * 0.88f;
        cfg.activePedestal = cfg.noiseFloor;
        cfg.referencePeak = (std::max)(0.38f, actP98 * 5.0f);
        cfg.gamma = 1.15f;
    } else if (actP12 > 0.13f && (actP12 / (std::max)(0.05f, actP98)) > 0.40f) {
        // 重度母带响度压限的系统音乐/视频伴奏：剥离稳态高底座并展开鼓点与重音动态峰谷
        cfg.noiseFloor = 0.012f;
        cfg.activePedestal = actP12 * 0.80f;
        cfg.referencePeak = (std::max)(cfg.activePedestal + 0.10f, actP98);
        cfg.gamma = 1.35f;
    } else {
        // 自然抑扬顿挫麦克风人声讲解或高动态音效
        cfg.noiseFloor = std::clamp(actP12 * 0.55f, 0.008f, actP98 * 0.24f);
        cfg.activePedestal = cfg.noiseFloor;
        cfg.referencePeak = (std::max)(0.20f, actP98);
        cfg.gamma = 1.06f;
    }
    return cfg;
}

void StudioTimelineAnalyzer::sampleSculptedWaveformColumn(
    const std::vector<float>& peaks,
    int64_t totalDurationMs,
    int64_t rangeStartMs,
    int64_t rangeEndMs,
    const StudioWaveformEnvelopeConfig& profile,
    float& outBodyEnergy,
    float& outPeakCrest) noexcept {
    outBodyEnergy = 0.0f;
    outPeakCrest = 0.0f;
    if (!profile.hasAudio || peaks.empty() || totalDurationMs <= 0) {
        return;
    }

    const int64_t sMs = std::clamp<int64_t>(rangeStartMs, 0, totalDurationMs);
    const int64_t eMs = std::clamp<int64_t>((std::max)(sMs + 1, rangeEndMs), 0, totalDurationMs);
    const size_t n = peaks.size();
    const size_t i0 = std::min<size_t>(
        n - 1,
        static_cast<size_t>((static_cast<double>(sMs) / static_cast<double>(totalDurationMs)) * static_cast<double>(n)));
    const size_t i1 = std::min<size_t>(
        n - 1,
        static_cast<size_t>(std::ceil((static_cast<double>(eMs) / static_cast<double>(totalDurationMs)) * static_cast<double>(n) - 1e-6)));
    const size_t endIdx = (std::max)(i0, i1);

    float maxVal = 0.0f;
    double sumSq = 0.0;
    size_t count = 0;
    for (size_t i = i0; i <= endIdx; ++i) {
        const float v = peaks[i];
        maxVal = (std::max)(maxVal, v);
        sumSq += static_cast<double>(v) * static_cast<double>(v);
        ++count;
    }
    if (maxVal <= profile.noiseFloor + 0.004f) {
        return;
    }

    const float rmsVal = (count > 0) ? static_cast<float>(std::sqrt(sumSq / static_cast<double>(count))) : maxVal;
    const bool compressedMusic = (profile.activePedestal > profile.noiseFloor + 0.02f);
    const float span = (std::max)(0.10f, profile.referencePeak - profile.activePedestal);

    const float rawBodyNorm = std::clamp((rmsVal - profile.activePedestal) / span, 0.0f, 1.0f);
    const float shapedBody = std::pow(rawBodyNorm, profile.gamma);
    outBodyEnergy = compressedMusic
        ? std::clamp(0.10f + 0.90f * shapedBody, 0.09f, 1.0f)
        : std::clamp(shapedBody, 0.0f, 1.0f);

    const float rawPeakNorm = std::clamp((maxVal - profile.activePedestal) / span, 0.0f, 1.0f);
    const float shapedPeak = std::pow(rawPeakNorm, (std::max)(0.85f, profile.gamma * 0.82f));
    const float candCrest = compressedMusic
        ? std::clamp(0.16f + 0.84f * shapedPeak, outBodyEnergy, 1.0f)
        : std::clamp(shapedPeak, outBodyEnergy, 1.0f);
    outPeakCrest = (std::min)(candCrest, outBodyEnergy + 0.26f);
}

std::vector<TimelineSnapAnchor> StudioTimelineAnalyzer::collectTimelineSnapAnchors(
    const std::vector<StudioVideoClip>& clips,
    const std::vector<StudioZoomSegment>& zoomSegments,
    const std::vector<StudioAnnotationItem>& annotations,
    int64_t playheadKeptMs,
    int excludeZoomIdx,
    int excludeAnnIdx,
    bool includePlayhead) {
    std::vector<TimelineSnapAnchor> out;
    out.reserve(clips.size() + zoomSegments.size() * 2 + annotations.size() * 2 + 4);

    // 1. 视频切片首尾与全部拆分接缝点（最高视觉/剪辑优先级）
    int64_t accKept = 0;
    out.push_back({0, TimelineSnapTargetKind::ClipSplit});
    for (const auto& c : clips) {
        accKept += c.keptDurationMs();
        out.push_back({accKept, TimelineSnapTargetKind::ClipSplit});
    }

    // 2. 当前主播放头
    if (includePlayhead && playheadKeptMs >= 0) {
        out.push_back({std::clamp<int64_t>(playheadKeptMs, 0, (std::max<int64_t>)(0, accKept)),
                       TimelineSnapTargetKind::Playhead});
    }

    // 3. 其他运镜缩放块的起止边界
    for (size_t i = 0; i < zoomSegments.size(); ++i) {
        if (static_cast<int>(i) == excludeZoomIdx) continue;
        const int64_t ks = mapSourceMsToKeptMsViaClips(clips, zoomSegments[i].startMs);
        const int64_t ke = mapSourceMsToKeptMsViaClips(clips, zoomSegments[i].endMs);
        if (ke <= ks + 30) continue;
        out.push_back({ks, TimelineSnapTargetKind::ZoomBoundary});
        out.push_back({ke, TimelineSnapTargetKind::ZoomBoundary});
    }

    // 4. 其他标注/字幕块的起止边界
    for (size_t i = 0; i < annotations.size(); ++i) {
        if (static_cast<int>(i) == excludeAnnIdx) continue;
        const int64_t ks = mapSourceMsToKeptMsViaClips(clips, annotations[i].startMs);
        const int64_t ke = mapSourceMsToKeptMsViaClips(clips, annotations[i].endMs);
        if (ke <= ks + 30) continue;
        out.push_back({ks, TimelineSnapTargetKind::AnnBoundary});
        out.push_back({ke, TimelineSnapTargetKind::AnnBoundary});
    }

    // 按 keptMs 升序排列，同位置优先保留枚举值更小（ClipSplit > Playhead > Zoom > Ann）的锚点
    std::sort(out.begin(), out.end(), [](const TimelineSnapAnchor& a, const TimelineSnapAnchor& b) {
        if (a.keptMs != b.keptMs) return a.keptMs < b.keptMs;
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    });
    std::vector<TimelineSnapAnchor> dedup;
    dedup.reserve(out.size());
    for (const auto& a : out) {
        if (!dedup.empty() && std::abs(dedup.back().keptMs - a.keptMs) <= 4) {
            if (static_cast<int>(a.kind) < static_cast<int>(dedup.back().kind)) {
                dedup.back() = a;
            }
        } else {
            dedup.push_back(a);
        }
    }
    return dedup;
}

TimelineSnapResult StudioTimelineAnalyzer::snapTimelineRangeDrag(
    int64_t rawStartMs,
    int64_t rawEndMs,
    int dragEdgeMode,
    const std::vector<TimelineSnapAnchor>& anchors,
    int64_t snapWindowMs,
    int64_t minSpanMs,
    int64_t maxTotalMs) noexcept {
    TimelineSnapResult res{};
    const int64_t total = (std::max<int64_t>)(minSpanMs, maxTotalMs);
    const int64_t len = (std::max<int64_t>)(minSpanMs, rawEndMs - rawStartMs);

    if (dragEdgeMode == 0) {
        res.snappedStartMs = std::clamp<int64_t>(rawStartMs, 0, (std::max<int64_t>)(0, total - len));
        res.snappedEndMs = res.snappedStartMs + len;
    } else if (dragEdgeMode == 1) {
        res.snappedEndMs = std::clamp<int64_t>(rawEndMs, minSpanMs, total);
        res.snappedStartMs = std::clamp<int64_t>(rawStartMs, 0, res.snappedEndMs - minSpanMs);
    } else {
        res.snappedStartMs = std::clamp<int64_t>(rawStartMs, 0, total - minSpanMs);
        res.snappedEndMs = std::clamp<int64_t>(rawEndMs, res.snappedStartMs + minSpanMs, total);
    }

    if (snapWindowMs <= 0 || anchors.empty()) {
        return res;
    }

    auto priorityBonusMs = [](TimelineSnapTargetKind k) noexcept -> int64_t {
        // 近似等距时赋予视频拆分点与主播放头 12ms / 6ms 优先权，切缝手感干脆不跳动
        switch (k) {
            case TimelineSnapTargetKind::ClipSplit:    return 12;
            case TimelineSnapTargetKind::Playhead:     return 6;
            case TimelineSnapTargetKind::ZoomBoundary: return 2;
            default:                                   return 0;
        }
    };

    int64_t bestScore = LLONG_MAX;
    TimelineSnapResult best = res;

    auto tryCandidate = [&](int64_t candStart, int64_t candEnd, int64_t rawDiff, const TimelineSnapAnchor& anchor) {
        if (rawDiff > snapWindowMs) return;
        if (candStart < 0 || candEnd > total || (candEnd - candStart) < minSpanMs) return;
        const int64_t score = rawDiff - priorityBonusMs(anchor.kind);
        if (score < bestScore) {
            bestScore = score;
            best.snapped = true;
            best.snappedStartMs = candStart;
            best.snappedEndMs = candEnd;
            best.snapLineKeptMs = anchor.keptMs;
            best.targetKind = anchor.kind;
        }
    };

    for (const auto& anchor : anchors) {
        if (dragEdgeMode == 0) {
            // 整块移动：同时检测左缘与右缘吸附，保持块长度 100% 不变
            const int64_t diffL = std::abs(res.snappedStartMs - anchor.keptMs);
            if (diffL <= snapWindowMs) {
                tryCandidate(anchor.keptMs, anchor.keptMs + len, diffL, anchor);
            }
            const int64_t diffR = std::abs(res.snappedEndMs - anchor.keptMs);
            if (diffR <= snapWindowMs) {
                tryCandidate(anchor.keptMs - len, anchor.keptMs, diffR, anchor);
            }
        } else if (dragEdgeMode == 1) {
            const int64_t diffL = std::abs(res.snappedStartMs - anchor.keptMs);
            if (diffL <= snapWindowMs) {
                tryCandidate(anchor.keptMs, res.snappedEndMs, diffL, anchor);
            }
        } else {
            const int64_t diffR = std::abs(res.snappedEndMs - anchor.keptMs);
            if (diffR <= snapWindowMs) {
                tryCandidate(res.snappedStartMs, anchor.keptMs, diffR, anchor);
            }
        }
    }
    return best;
}

TimelineSnapResult StudioTimelineAnalyzer::snapTimelinePoint(
    int64_t rawKeptMs,
    const std::vector<TimelineSnapAnchor>& anchors,
    int64_t snapWindowMs,
    int64_t maxTotalMs) noexcept {
    TimelineSnapResult res{};
    const int64_t total = (std::max<int64_t>)(0, maxTotalMs);
    const int64_t clamped = std::clamp<int64_t>(rawKeptMs, 0, total);
    res.snappedStartMs = clamped;
    res.snappedEndMs = clamped;
    if (snapWindowMs <= 0 || anchors.empty()) return res;

    int64_t bestScore = LLONG_MAX;
    for (const auto& anchor : anchors) {
        if (anchor.keptMs < 0 || anchor.keptMs > total) continue;
        const int64_t diff = std::abs(clamped - anchor.keptMs);
        if (diff > snapWindowMs) continue;
        const int64_t bonus = (anchor.kind == TimelineSnapTargetKind::ClipSplit) ? 10 : 0;
        const int64_t score = diff - bonus;
        if (score < bestScore) {
            bestScore = score;
            res.snapped = true;
            res.snappedStartMs = anchor.keptMs;
            res.snappedEndMs = anchor.keptMs;
            res.snapLineKeptMs = anchor.keptMs;
            res.targetKind = anchor.kind;
        }
    }
    return res;
}

int64_t StudioTimelineAnalyzer::findNextTimelineKeyframeMs(
    int64_t currentKeptMs,
    int direction,
    const std::vector<TimelineSnapAnchor>& anchors) noexcept {
    constexpr int64_t kEpsMs = 28;
    if (direction > 0) {
        int64_t best = -1;
        for (const auto& a : anchors) {
            if (a.kind == TimelineSnapTargetKind::Playhead) continue;
            if (a.keptMs > currentKeptMs + kEpsMs) {
                if (best < 0 || a.keptMs < best) best = a.keptMs;
            }
        }
        return best;
    } else if (direction < 0) {
        int64_t best = -1;
        for (const auto& a : anchors) {
            if (a.kind == TimelineSnapTargetKind::Playhead) continue;
            if (a.keptMs + kEpsMs < currentKeptMs) {
                if (best < 0 || a.keptMs > best) best = a.keptMs;
            }
        }
        return best;
    }
    return -1;
}

} // namespace tools3000::capture
