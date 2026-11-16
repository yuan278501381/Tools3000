#include "capture/StudioTimeline.h"
#include "common/AtomicFile.h"
#include "core/utils/WinUtils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <sapi.h>

#include <algorithm>
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
    float defaultZoom) {
    struct RawAnchor {
        int64_t ptsMs = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    const float clampedZoom = std::clamp(defaultZoom, 1.25f, 2.80f);
    const float w = static_cast<float>((std::max)(1, timeline.sourceWidth));
    const float h = static_cast<float>((std::max)(1, timeline.sourceHeight));

    std::vector<RawAnchor> anchors;
    anchors.reserve(timeline.clickEvents.size() + timeline.focusEvents.size());
    for (const auto& clk : timeline.clickEvents) {
        if (clk.x >= 0.0f && clk.x <= w && clk.y >= 0.0f && clk.y <= h) {
            anchors.push_back({clk.ptsMs, clk.x, clk.y});
        }
    }
    for (const auto& foc : timeline.focusEvents) {
        if (foc.x >= 0.0f && foc.x <= w && foc.y >= 0.0f && foc.y <= h) {
            anchors.push_back({foc.ptsMs, foc.x, foc.y});
        }
    }

    if (anchors.empty()) {
        return {};
    }

    std::sort(anchors.begin(), anchors.end(), [](const RawAnchor& a, const RawAnchor& b) {
        return a.ptsMs < b.ptsMs;
    });

    constexpr int64_t kLeadInMs = 240;
    constexpr int64_t kHoldTailMs = 1500;
    constexpr int64_t kMergeGapMs = 650;
    const int64_t maxDur = (std::max<int64_t>)(timeline.durationMs, anchors.back().ptsMs + kHoldTailMs);

    std::vector<StudioZoomSegment> segments;
    for (const auto& a : anchors) {
        const int64_t segStart = (std::max<int64_t>)(0, a.ptsMs - kLeadInMs);
        const int64_t segEnd = (std::min<int64_t>)(maxDur, a.ptsMs + kHoldTailMs);
        if (segEnd <= segStart + 120) continue;

        if (!segments.empty() && segStart <= segments.back().endMs + kMergeGapMs) {
            segments.back().endMs = (std::max)(segments.back().endMs, segEnd);
            // 若新锚点与首个锚点距离较近，平滑融合焦点
            segments.back().focusX = segments.back().focusX * 0.35f + a.x * 0.65f;
            segments.back().focusY = segments.back().focusY * 0.35f + a.y * 0.65f;
        } else {
            StudioZoomSegment seg;
            seg.startMs = segStart;
            seg.endMs = segEnd;
            seg.focusX = a.x;
            seg.focusY = a.y;
            seg.targetZoom = clampedZoom;
            seg.enabled = true;
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

    OneEuroFilter2D cursorFilter(1.40, 0.020, 1.0);
    CriticallyDampedSpring1D springX(6.4);
    CriticallyDampedSpring1D springY(6.4);
    CriticallyDampedSpring1D springZoom(5.8);
    springX.reset(centerDefaultX);
    springY.reset(centerDefaultY);
    springZoom.reset(1.0);

    float anchorFocusX = centerDefaultX;
    float anchorFocusY = centerDefaultY;
    size_t sampleIdx = 0;

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
            smoothCurX = std::clamp(smoothCurX, 0.0f, srcW);
            smoothCurY = std::clamp(smoothCurY, 0.0f, srcH);
        }

        // 2. 判定当前是否命中启用的 Auto-Zoom 区间
        float desiredZoom = 1.0f;
        float desiredCenterX = centerDefaultX;
        float desiredCenterY = centerDefaultY;

        if (config.autoZoomEnabled) {
            const StudioZoomSegment* activeSeg = nullptr;
            for (const auto& seg : timeline.zoomSegments) {
                if (seg.enabled && ptsMs >= seg.startMs && ptsMs <= seg.endMs) {
                    activeSeg = &seg;
                    break;
                }
            }
            if (activeSeg) {
                desiredZoom = std::clamp(config.defaultZoomFactor, 1.25f, 2.80f);
                // 查找该放大区间内最近的点击或焦点事件；若鼠标大幅移出舒适死区则平滑跟移
                float refX = activeSeg->focusX;
                float refY = activeSeg->focusY;
                for (const auto& clk : timeline.clickEvents) {
                    if (clk.ptsMs >= activeSeg->startMs && clk.ptsMs <= ptsMs + 180) {
                        refX = clk.x;
                        refY = clk.y;
                    }
                }
                for (const auto& foc : timeline.focusEvents) {
                    if (foc.ptsMs >= activeSeg->startMs && foc.ptsMs <= ptsMs + 120) {
                        refX = foc.x;
                        refY = foc.y;
                    }
                }

                // 舒适区死区融合：光标在视口中心 32% 范围内不牵引镜头，超出时温和牵引
                const float viewHalfW = (srcW / desiredZoom) * 0.5f;
                const float viewHalfH = (srcH / desiredZoom) * 0.5f;
                const float deadX = viewHalfW * 0.34f;
                const float deadY = viewHalfH * 0.34f;

                anchorFocusX = refX;
                anchorFocusY = refY;
                if (std::abs(smoothCurX - anchorFocusX) > deadX) {
                    const float sign = (smoothCurX > anchorFocusX) ? 1.0f : -1.0f;
                    anchorFocusX += (std::abs(smoothCurX - anchorFocusX) - deadX) * sign;
                }
                if (std::abs(smoothCurY - anchorFocusY) > deadY) {
                    const float sign = (smoothCurY > anchorFocusY) ? 1.0f : -1.0f;
                    anchorFocusY += (std::abs(smoothCurY - anchorFocusY) - deadY) * sign;
                }

                // 提前对目标中心做视口边界钳制，避免弹簧向屏幕外无效蓄力
                desiredCenterX = std::clamp(anchorFocusX, viewHalfW, (std::max)(viewHalfW, srcW - viewHalfW));
                desiredCenterY = std::clamp(anchorFocusY, viewHalfH, (std::max)(viewHalfH, srcH - viewHalfH));
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

        const float cropW = std::clamp(srcW / curZoom, 16.0f, srcW);
        const float cropH = std::clamp(srcH / curZoom, 16.0f, srcH);
        const float cropX = std::clamp(curCx - cropW * 0.5f, 0.0f, (std::max)(0.0f, srcW - cropW));
        const float cropY = std::clamp(curCy - cropH * 0.5f, 0.0f, (std::max)(0.0f, srcH - cropH));

        // 4. 点击微缩放弹跳与水波纹进度计算
        float clickScale = 1.0f;
        float rippleProgress = -1.0f;
        bool rippleRight = false;
        constexpr int64_t kRippleDurationMs = 380;
        for (const auto& clk : timeline.clickEvents) {
            const int64_t elapsed = ptsMs - clk.ptsMs;
            if (elapsed >= 0 && elapsed <= kRippleDurationMs) {
                const float p = static_cast<float>(elapsed) / static_cast<float>(kRippleDurationMs);
                rippleProgress = p;
                rippleRight = clk.isRight;
                // 前 28% 快速按压下沉至 0.82x，随后回弹至 1.12x 再平复到 1.0x
                if (p < 0.28f) {
                    const float k = p / 0.28f;
                    clickScale = 1.0f - 0.18f * std::sin(k * 1.5707963f);
                } else {
                    const float k = (p - 0.28f) / 0.72f;
                    clickScale = 1.0f + 0.12f * std::sin(k * 3.1415926f) * (1.0f - k);
                }
            }
        }

        // 5. 匹配当前时间点的字幕
        std::string captionText;
        if (config.autoCaptionsEnabled) {
            for (const auto& cap : timeline.captions) {
                if (ptsMs >= cap.startMs && ptsMs <= cap.endMs) {
                    captionText = cap.text;
                    break;
                }
            }
        }

        StudioEvaluatedFrame& ef = out[static_cast<size_t>(f)];
        ef.camera.centerX = cropX + cropW * 0.5f;
        ef.camera.centerY = cropY + cropH * 0.5f;
        ef.camera.zoom = curZoom;
        ef.camera.cropX = cropX;
        ef.camera.cropY = cropY;
        ef.camera.cropW = cropW;
        ef.camera.cropH = cropH;
        ef.smoothCursorX = smoothCurX;
        ef.smoothCursorY = smoothCurY;
        ef.cursorKind = curKind;
        ef.clickScale = clickScale;
        ef.rippleProgress = config.showClickRipple ? rippleProgress : -1.0f;
        ef.rippleIsRight = rippleRight;
        ef.activeCaption = std::move(captionText);
    }

    return out;
}

std::string StudioTimelineAnalyzer::formatSrtContent(const std::vector<StudioCaptionSegment>& captions) {
    std::ostringstream oss;
    size_t idx = 1;
    for (const auto& seg : captions) {
        if (seg.text.empty() || seg.endMs <= seg.startMs) continue;
        oss << idx++ << "\r\n"
            << formatSrtTimestamp(seg.startMs) << " --> " << formatSrtTimestamp(seg.endMs) << "\r\n"
            << seg.text << "\r\n\r\n";
    }
    return oss.str();
}

bool StudioTimelineAnalyzer::exportSrtFile(
    const std::wstring& srtPath,
    const std::vector<StudioCaptionSegment>& captions) {
    if (srtPath.empty() || captions.empty()) return false;
    const std::string content = formatSrtContent(captions);
    if (content.empty()) return false;
    // UTF-8 BOM + content so Windows media players & Premiere recognize Chinese captions cleanly
    std::string withBom = "\xEF\xBB\xBF" + content;
    return tools3000::common::atomicWriteBinaryFileWithFlush(
        srtPath,
        reinterpret_cast<const uint8_t*>(withBom.data()),
        withBom.size());
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
    int regionX, int regionY, int width, int height, int fps, bool captureSpeechCaptions) {
    stop();

    m_regionX = regionX;
    m_regionY = regionY;
    m_width = (std::max)(1, width);
    m_height = (std::max)(1, height);
    m_fps = std::clamp(fps, 1, 120);
    m_enableSpeech = captureSpeechCaptions;

    {
        std::lock_guard lock(m_clockMutex);
        m_startTick = std::chrono::steady_clock::now();
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

    m_paused.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_samplerThread = std::thread(&StudioTimelineRecorder::samplingLoop, this);

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
    if (m_speechStopEvent) {
        SetEvent(m_speechStopEvent);
    }
    if (m_samplerThread.joinable()) {
        m_samplerThread.join();
    }
    if (m_speechThread.joinable()) {
        m_speechThread.join();
    }
    if (m_speechStopEvent) {
        CloseHandle(m_speechStopEvent);
        m_speechStopEvent = nullptr;
    }
}

StudioTimelineData StudioTimelineRecorder::takeTimelineData(int64_t finalDurationMs, float defaultZoom) {
    stop();
    std::lock_guard lock(m_dataMutex);
    m_data.durationMs = (std::max<int64_t>)(finalDurationMs,
        m_data.cursorSamples.empty() ? 0 : m_data.cursorSamples.back().ptsMs);
    m_data.zoomSegments = StudioTimelineAnalyzer::buildAutoZoomSegments(m_data, defaultZoom);
    return std::move(m_data);
}

void StudioTimelineRecorder::samplingLoop() {
    bool prevLeft = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool prevRight = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    POINT prevCaretScreen{LONG_MIN, LONG_MIN};
    int caretPollTick = 0;

    while (m_running.load(std::memory_order_acquire)) {
        if (m_paused.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            prevLeft = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            prevRight = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            continue;
        }

        const int64_t pts = currentElapsedTimelineMs();
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        POINT pt{};
        StudioCursorKind kind = StudioCursorKind::Arrow;
        if (GetCursorInfo(&ci)) {
            pt = ci.ptScreenPos;
            kind = (ci.flags & CURSOR_SHOWING) ? classifySystemCursor(ci.hCursor) : StudioCursorKind::Hidden;
        } else {
            GetCursorPos(&pt);
        }

        const float relX = static_cast<float>(pt.x - m_regionX);
        const float relY = static_cast<float>(pt.y - m_regionY);
        const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

        {
            std::lock_guard lock(m_dataMutex);
            m_data.cursorSamples.push_back({pts, relX, relY, kind, leftDown, rightDown});
            if ((leftDown && !prevLeft) || (rightDown && !prevRight)) {
                if (relX >= 0.0f && relX <= static_cast<float>(m_width) &&
                    relY >= 0.0f && relY <= static_cast<float>(m_height)) {
                    m_data.clickEvents.push_back({pts, relX, relY, rightDown && !leftDown});
                }
            }
        }
        prevLeft = leftDown;
        prevRight = rightDown;

        // 每 60ms 低开销探测一次键盘打字光标焦点位置，若插入符发生实质移动则记录打字聚焦点
        if ((++caretPollTick % 15) == 0) {
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

        std::this_thread::sleep_for(std::chrono::milliseconds(4)); // ~250Hz 采样
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

    if (grammar) {
        grammar->SetDictationState(SPRS_INACTIVE);
    }
    cleanupCom();
}

} // namespace tools3000::capture
