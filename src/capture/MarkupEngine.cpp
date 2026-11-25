// ─────────────────────────────────────────────────────────────────────────────
// MarkupEngine.cpp — 截图标注引擎核心（元素生命周期、撤销栈、渲染缓存、通用手柄与注册表）
// ─────────────────────────────────────────────────────────────────────────────

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture/MarkupEngineInternal.h"
#include "capture/TextAnnotationHelper.h"
#include "capture/TextReplaceEngine.h"
#include "capture/MarkupCurveGeometry.h"
#include "capture/CaptureInteractionPolicy.h"
#include "capture/CaptureTextEditor.h"
#include "capture/CornerRadiusHelper.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/TraceId.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

namespace tools3000::capture {

float sampleCanvasRegionLuminance(const cv::Mat& canvas, const cv::Rect& roiRect) {
    if (canvas.empty() || (canvas.channels() != 3 && canvas.channels() != 4)) {
        return 0.50f;
    }
    const cv::Rect bounds(0, 0, canvas.cols, canvas.rows);
    cv::Rect clipped = roiRect & bounds;
    if (clipped.width <= 0 || clipped.height <= 0) {
        if (roiRect.width > 0 && roiRect.height > 0) {
            const int w = std::min(roiRect.width, canvas.cols);
            const int h = std::min(roiRect.height, canvas.rows);
            const int cx = std::clamp(roiRect.x, 0, std::max(0, canvas.cols - w));
            const int cy = std::clamp(roiRect.y, 0, std::max(0, canvas.rows - h));
            clipped = cv::Rect(cx, cy, w, h) & bounds;
        }
        if (clipped.width <= 0 || clipped.height <= 0) {
            clipped = bounds;
        }
    }
    const cv::Scalar meanVal = cv::mean(canvas(clipped));
    const float b = static_cast<float>(meanVal[0]);
    const float g = static_cast<float>(meanVal[1]);
    const float r = static_cast<float>(meanVal[2]);
    const float lum = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
    return std::clamp(lum, 0.0f, 1.0f);
}

MarkupColor computeAdaptiveTextOutlineColor(const MarkupColor& textColor, float bgLuminance) {
    const float textLum = (0.299f * static_cast<float>(textColor.r) +
                           0.587f * static_cast<float>(textColor.g) +
                           0.114f * static_cast<float>(textColor.b)) / 255.0f;
    const MarkupColor darkOutline{15, 17, 23, 250};
    const MarkupColor lightOutline{255, 255, 255, 250};

    // 1. 极深色文字（如极客黑 #1E293B / 纯黑 #000000，亮度 < 0.22）：
    //    若使用深色描边会导致描边与字芯完全融为一团黑影，且在深色背景上彻底不可见，因此恒定采用纯白高反差描边
    if (textLum < 0.22f) {
        return lightOutline;
    }

    // 2. 极亮浅色/纯白文字（如纯白 #FFFFFF 或高明度浅粉彩，亮度 > 0.78）：
    //    若使用白色描边会导致文字边缘发胖糊化，且在浅色背景上不可见，因此恒定采用曜石黑高反差描边
    if (textLum > 0.78f) {
        return darkOutline;
    }

    // 3. 彩色及中等亮度文字（红/橙/黄/绿/蓝/紫等，0.22 <= textLum <= 0.78）：
    //    严格基于底层背景亮度自动反色：深色背景 (bgLuminance < 0.50f) 配纯白高光描边，浅色背景配曜石黑沉稳描边
    return (bgLuminance < 0.50f) ? lightOutline : darkOutline;
}

MarkupColor resolveTextOutlineColor(const MarkupColor& textColor,
                                    const MarkupColor& outlineColor,
                                    const cv::Mat& canvas,
                                    const cv::Rect& textRect,
                                    bool withBackdrop) {
    if (!outlineColor.isAuto()) {
        const uint8_t a = outlineColor.a > 0 ? outlineColor.a : 250;
        return MarkupColor{outlineColor.r, outlineColor.g, outlineColor.b, a};
    }

    float bgLum = sampleCanvasRegionLuminance(canvas, textRect);
    if (withBackdrop) {
        const float textLum = (0.299f * static_cast<float>(textColor.r) +
                               0.587f * static_cast<float>(textColor.g) +
                               0.114f * static_cast<float>(textColor.b)) / 255.0f;
        const float capsuleLum = (textLum > 0.45f) ? 0.08f : 0.96f;
        const float capsuleAlpha = (textLum > 0.45f) ? (190.0f / 255.0f) : (200.0f / 255.0f);
        bgLum = bgLum * (1.0f - capsuleAlpha) + capsuleLum * capsuleAlpha;
    }
    return computeAdaptiveTextOutlineColor(textColor, bgLum);
}

} // namespace tools3000::capture

namespace tools3000::capture {

using namespace markup_detail;

namespace {
    std::mutex g_gdiPlusMutex;
    ULONG_PTR g_gdiPlusToken = 0;
} // namespace

namespace markup_detail {
    std::vector<cv::Point> generateSmoothSpline(const std::vector<cv::Point>& pts, int stepsPerSegment) {
        if (pts.size() < 3) return pts;

        std::vector<cv::Point> cleanPts;
        cleanPts.reserve(pts.size());
        for (const auto& p : pts) {
            if (cleanPts.empty() || cleanPts.back() != p) {
                cleanPts.push_back(p);
            }
        }
        if (cleanPts.size() < 3) return pts;

        std::vector<cv::Point> extended(cleanPts.size() + 2);
        extended[0] = cleanPts[0] - (cleanPts[1] - cleanPts[0]);
        for (size_t i = 0; i < cleanPts.size(); ++i) extended[i + 1] = cleanPts[i];
        extended[extended.size() - 1] = cleanPts.back() + (cleanPts.back() - cleanPts[cleanPts.size() - 2]);

        std::vector<cv::Point> smoothPts;
        smoothPts.reserve((cleanPts.size() - 1) * stepsPerSegment + 1);

        for (size_t i = 1; i < extended.size() - 2; ++i) {
            cv::Point2f p0 = extended[i - 1], p1 = extended[i], p2 = extended[i + 1], p3 = extended[i + 2];
            const int limit = (i == extended.size() - 3) ? (stepsPerSegment + 1) : stepsPerSegment;
            for (int t_i = 0; t_i < limit; ++t_i) {
                float t = static_cast<float>(t_i) / stepsPerSegment;
                float t2 = t * t;
                float t3 = t2 * t;
                float x = 0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t + (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 + (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3);
                float y = 0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t + (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 + (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);
                cv::Point pt(static_cast<int>(std::round(x)), static_cast<int>(std::round(y)));
                if (smoothPts.empty() || smoothPts.back() != pt) {
                    smoothPts.push_back(pt);
                }
            }
        }
        if (smoothPts.size() == 1) {
            smoothPts.push_back(cleanPts.back());
        }
        return smoothPts;
    }

    void ensureGdiplusStartup() {
        std::lock_guard lock(g_gdiPlusMutex);
        if (g_gdiPlusToken != 0) return;
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
            g_gdiPlusToken = token;
        }
    }


    bool isGdiplusStarted() noexcept {
        std::lock_guard lock(g_gdiPlusMutex);
        return g_gdiPlusToken != 0;
    }
} // namespace markup_detail

cv::Mat applyRoundedCorners(const cv::Mat& src, float radius) {
    if (src.empty() || radius <= 0.5f) {
        return src.clone();
    }

    const int w = src.cols;
    const int h = src.rows;
    const float r = (std::min)({radius, static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f});
    if (r <= 0.5f) {
        return src.clone();
    }

    cv::Mat dst;
    if (src.channels() == 4) {
        dst = src.clone();
    } else if (src.channels() == 3) {
        cv::cvtColor(src, dst, cv::COLOR_BGR2BGRA);
    } else {
        return src.clone();
    }

    const int intR = static_cast<int>(std::ceil(r));

    auto applyCorner = [&](int startX, int endX, int startY, int endY, float centerX, float centerY) {
        for (int y = startY; y < endY; ++y) {
            auto* row = dst.ptr<cv::Vec4b>(y);
            const float py = y + 0.5f;
            for (int x = startX; x < endX; ++x) {
                const float px = x + 0.5f;
                const float d = std::hypot(px - centerX, py - centerY);
                if (d > r + 0.5f) {
                    row[x][3] = 0;
                } else if (d > r - 0.5f) {
                    const float coverage = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
                    row[x][3] = static_cast<uint8_t>(row[x][3] * coverage);
                }
            }
        }
    };

    // Top-Left corner
    applyCorner(0, intR, 0, intR, r, r);
    // Top-Right corner
    applyCorner(w - intR, w, 0, intR, w - r, r);
    // Bottom-Left corner
    applyCorner(0, intR, h - intR, h, r, h - r);
    // Bottom-Right corner
    applyCorner(w - intR, w, h - intR, h, w - r, h - r);

    return dst;
}

bool initializeMarkupTextRenderer() {
    std::lock_guard lock(g_gdiPlusMutex);
    if (g_gdiPlusToken != 0) return true;

    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    const auto status = Gdiplus::GdiplusStartup(&token, &input, nullptr);
    if (status != Gdiplus::Ok) {
        LOG_ERROR("GDI+ 文本渲染器初始化失败, status={}", static_cast<int>(status));
        return false;
    }
    g_gdiPlusToken = token;
    LOG_DEBUG("GDI+ 文本渲染器已初始化");
    return true;
}

void shutdownMarkupTextRenderer() {
    ULONG_PTR token = 0;
    {
        std::lock_guard lock(g_gdiPlusMutex);
        token = g_gdiPlusToken;
        g_gdiPlusToken = 0;
    }
    if (token != 0) {
        // Deliberately before FreeLibrary and without the mutex: GDI+ waits for
        // its helper thread during shutdown.
        Gdiplus::GdiplusShutdown(token);
        LOG_DEBUG("GDI+ 文本渲染器已关闭");
    }
}

static std::atomic<uint32_t> g_elementIdCounter{1};

// ─────────────────────────────────────────────────────────────────────────────
// MarkupElement 矢量操作
// ─────────────────────────────────────────────────────────────────────────────

cv::Rect MarkupElement::getBoundingBox() const {
    if (const auto* handler = MarkupToolRegistry::instance().getHandler(tool)) {
        return handler->getBoundingBox(*this);
    }
    return cv::Rect();
}

HitArea MarkupElement::hitTestEx(cv::Point pt, int padding) const {
    if (const auto* handler = MarkupToolRegistry::instance().getHandler(tool)) {
        return handler->hitTest(*this, pt, padding);
    }
    return HitArea::None;
}

void MarkupElement::moveBy(int dx, int dy) {
    startPt.x += dx; startPt.y += dy;
    endPt.x += dx; endPt.y += dy;
    for (auto& pt : penPoints) {
        pt.x += dx; pt.y += dy;
    }
    for (auto& rc : textSpans) {
        rc.x += dx; rc.y += dy;
    }
}

void MarkupElement::resize(int dx, int dy, HitArea handle) {
    if (const auto* handler = MarkupToolRegistry::instance().getHandler(tool)) {
        handler->resize(*this, dx, dy, handle);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 底图管理
// ─────────────────────────────────────────────────────────────────────────────

int MarkupEngine::currentNumber(NumberFormat format) const noexcept {
    return m_nextNumbers[numberFormatSlot(format)];
}

void MarkupEngine::resetNumber(int start, NumberFormat format) noexcept {
    m_nextNumbers[numberFormatSlot(format)] = (std::max)(1, start);
}

void MarkupEngine::resetAllNumbers() noexcept {
    m_nextNumbers.fill(1);
}

void MarkupEngine::recalculateNextNumbers() noexcept {
    std::array<int, static_cast<size_t>(NumberFormat::COUNT)> maxByFormat{};
    for (const auto& elem : m_elements) {
        if (elem && elem->tool == MarkupTool::Number) {
            const size_t slot = numberFormatSlot(elem->numberFormat);
            maxByFormat[slot] = (std::max)(maxByFormat[slot], elem->numberValue);
        }
    }
    for (size_t i = 0; i < m_nextNumbers.size(); ++i) {
        m_nextNumbers[i] = maxByFormat[i] + 1;
    }
}

bool MarkupEngine::elementEquivalent(const MarkupElement& a, const MarkupElement& b) noexcept {
    if (a.id != b.id ||
        a.tool != b.tool ||
        a.color != b.color ||
        std::abs(a.thickness - b.thickness) > 1e-3f ||
        a.lineStyle != b.lineStyle ||
        a.fill != b.fill ||
        std::abs(a.cornerRadius - b.cornerRadius) > 1e-3f ||
        a.arrowStyle != b.arrowStyle ||
        a.smoothCurve != b.smoothCurve ||
        a.isMultiPoint != b.isMultiPoint ||
        a.hasCustomCaps != b.hasCustomCaps ||
        a.startCap != b.startCap ||
        a.endCap != b.endCap ||
        a.startPt != b.startPt ||
        a.endPt != b.endPt ||
        a.text != b.text ||
        std::abs(a.fontSize - b.fontSize) > 1e-3f ||
        std::abs(a.textMaxWidth - b.textMaxWidth) > 0.5f ||
        a.hasCustomTextWidth != b.hasCustomTextWidth ||
        std::abs(a.customTextBoxHeight - b.customTextBoxHeight) > 0.5f ||
        a.textOutline != b.textOutline ||
        a.textOutlineColor != b.textOutlineColor ||
        std::abs(a.outlineThickness - b.outlineThickness) > 1e-3f ||
        a.hasCustomOutline != b.hasCustomOutline ||
        a.numberValue != b.numberValue ||
        std::abs(a.dpiScale - b.dpiScale) > 1e-3f ||
        a.numberShape != b.numberShape ||
        a.numberFormat != b.numberFormat ||
        a.hasLeaderArrow != b.hasLeaderArrow ||
        a.mosaicBlockSize != b.mosaicBlockSize ||
        std::abs(a.magnifierScale - b.magnifierScale) > 1e-3f ||
        a.magnifierRadius != b.magnifierRadius ||
        a.magnifierRadiusY != b.magnifierRadiusY ||
        a.magnifierCalloutStyle != b.magnifierCalloutStyle ||
        std::abs(a.spotlightDimAlpha - b.spotlightDimAlpha) > 1e-3f ||
        a.spotlightEllipse != b.spotlightEllipse ||
        a.spotlightShowBorder != b.spotlightShowBorder ||
        a.watermarkText != b.watermarkText ||
        std::abs(a.watermarkOpacity - b.watermarkOpacity) > 1e-3f ||
        std::abs(a.watermarkAngle - b.watermarkAngle) > 1e-3f ||
        a.watermarkSpacing != b.watermarkSpacing ||
        a.inpaintRadius != b.inpaintRadius ||
        a.penPoints.size() != b.penPoints.size() ||
        a.textSpans.size() != b.textSpans.size()) {
        return false;
    }
    for (size_t i = 0; i < a.penPoints.size(); ++i) {
        if (a.penPoints[i] != b.penPoints[i]) return false;
    }
    for (size_t i = 0; i < a.textSpans.size(); ++i) {
        if (a.textSpans[i] != b.textSpans[i]) return false;
    }
    return true;
}

bool MarkupEngine::elementsEquivalent(const std::vector<MarkupElement>& a,
                                      const std::vector<MarkupElement>& b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!elementEquivalent(a[i], b[i])) return false;
    }
    return true;
}

MarkupEngine::MarkupSnapshot MarkupEngine::captureSnapshot() const {
    MarkupSnapshot snap;
    snap.elements.reserve(m_elements.size());
    for (const auto& elem : m_elements) {
        if (!elem) continue;
        MarkupElement copy = *elem;
        copy.isActive = false;
        copy.isEditing = false;
        snap.elements.push_back(std::move(copy));
    }
    snap.nextNumbers = m_nextNumbers;
    return snap;
}

void MarkupEngine::pushUndoSnapshot() {
    MarkupSnapshot cur = captureSnapshot();
    if (!m_undoHistory.empty() &&
        m_undoHistory.back().nextNumbers == cur.nextNumbers &&
        elementsEquivalent(m_undoHistory.back().elements, cur.elements)) {
        m_redoHistory.clear();
        return;
    }
    if (m_undoHistory.size() >= kMaxUndoSteps) {
        m_undoHistory.pop_front();
    }
    m_undoHistory.push_back(std::move(cur));
    m_redoHistory.clear();
}

void MarkupEngine::commitUndoSnapshot(MarkupSnapshot&& beforeState) {
    const MarkupSnapshot cur = captureSnapshot();
    if (beforeState.nextNumbers == cur.nextNumbers &&
        elementsEquivalent(beforeState.elements, cur.elements)) {
        return;
    }
    if (m_undoHistory.size() >= kMaxUndoSteps) {
        m_undoHistory.pop_front();
    }
    m_undoHistory.push_back(std::move(beforeState));
    m_redoHistory.clear();
}

void MarkupEngine::restoreSnapshot(const MarkupSnapshot& snapshot) {
    m_elements.clear();
    m_elements.reserve(snapshot.elements.size());
    uint32_t maxId = 0;
    for (const auto& item : snapshot.elements) {
        auto copy = std::make_unique<MarkupElement>(item);
        copy->isActive = false;
        copy->isEditing = false;
        if (copy->id == 0) {
            copy->id = g_elementIdCounter++;
        }
        maxId = (std::max)(maxId, copy->id);
        m_elements.push_back(std::move(copy));
    }
    uint32_t curCounter = g_elementIdCounter.load(std::memory_order_relaxed);
    while (curCounter <= maxId &&
           !g_elementIdCounter.compare_exchange_weak(curCounter, maxId + 1, std::memory_order_relaxed)) {}

    m_nextNumbers = snapshot.nextNumbers;
    m_underlayCache.release();
    m_underlayElements.clear();
}

std::vector<MarkupElement> MarkupEngine::exportElements() const {
    return captureSnapshot().elements;
}

void MarkupEngine::importElements(const std::vector<MarkupElement>& elems) {
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_elements.clear();
    m_elements.reserve(elems.size());

    uint32_t maxId = 0;
    for (const auto& src : elems) {
        auto copy = std::make_unique<MarkupElement>(src);
        copy->isActive = false;
        copy->isEditing = false;
        if (copy->id == 0) {
            copy->id = g_elementIdCounter++;
        }
        maxId = (std::max)(maxId, copy->id);
        m_elements.push_back(std::move(copy));
    }
    uint32_t curCounter = g_elementIdCounter.load(std::memory_order_relaxed);
    while (curCounter <= maxId &&
           !g_elementIdCounter.compare_exchange_weak(curCounter, maxId + 1, std::memory_order_relaxed)) {}

    recalculateNextNumbers();
    m_underlayCache.release();
    m_underlayElements.clear();
}

void MarkupEngine::setBaseImage(const cv::Mat& image) {
    m_baseImage = image.clone();
    m_elements.clear();
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_filterRoiCache.clear();
    m_underlayCache.release();
    m_underlayElements.clear();
    ++m_baseRevision;
    resetAllNumbers();
    LOG_DEBUG("标注引擎: 设置底图 {}x{}", image.cols, image.rows);
}

void MarkupEngine::updateBaseImage(const cv::Mat& image) {
    // 仅替换底图，保留标注/撤销栈/序号状态（选区二次调整后重裁用）
    m_baseImage = image.clone();
    if (m_baseImage.cols > 0) {
        for (auto& e : m_elements) {
            if (e && e->tool == MarkupTool::Text && !e->hasCustomTextWidth) {
                e->textMaxWidth = CaptureTextEditor::computeAvailableBoxWidth(
                    m_baseImage.cols, e->startPt.x, static_cast<int>(e->fontSize));
                e->textRenderSize = cv::Size(0, 0);
            }
        }
    }
    m_filterRoiCache.clear();
    m_underlayCache.release();
    m_underlayElements.clear();
    ++m_baseRevision;
}

void MarkupEngine::translateAll(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    for (auto& e : m_elements) {
        if (e) {
            e->moveBy(dx, dy);
            if (e->tool == MarkupTool::Text && !e->hasCustomTextWidth && m_baseImage.cols > 0) {
                e->textMaxWidth = CaptureTextEditor::computeAvailableBoxWidth(
                    m_baseImage.cols, e->startPt.x, static_cast<int>(e->fontSize));
                e->textRenderSize = cv::Size(0, 0);
            }
        }
    }
    for (auto& snap : m_undoHistory) {
        for (auto& e : snap.elements) e.moveBy(dx, dy);
    }
    for (auto& snap : m_redoHistory) {
        for (auto& e : snap.elements) e.moveBy(dx, dy);
    }
    m_filterRoiCache.clear();
    m_underlayCache.release();
    m_underlayElements.clear();
}

cv::Mat MarkupEngine::getCompositeImage(bool includeActiveHandles) const {
    if (m_baseImage.empty()) return {};
    if (m_elements.empty()) return m_baseImage.clone();

    auto isStageCForeground = [](MarkupTool t) noexcept {
        return t != MarkupTool::Mosaic &&
               t != MarkupTool::Blur &&
               t != MarkupTool::Inpaint &&
               t != MarkupTool::Spotlight;
    };

    const size_t total = m_elements.size();
    if (total >= 2 && m_elements.back() && isStageCForeground(m_elements.back()->tool)) {
        const size_t prefixCount = total - 1;
        bool cacheHit = (!m_underlayCache.empty() &&
                         m_underlayBaseRev == m_baseRevision &&
                         m_underlayCache.size() == m_baseImage.size() &&
                         m_underlayCache.type() == m_baseImage.type() &&
                         m_underlayElements.size() == prefixCount);
        if (cacheHit) {
            for (size_t i = 0; i < prefixCount; ++i) {
                if (!m_elements[i] || !elementEquivalent(*m_elements[i], m_underlayElements[i])) {
                    cacheHit = false;
                    break;
                }
            }
        }

        if (!cacheHit) {
            cv::Mat prefixCanvas = m_baseImage.clone();
            renderSubset(prefixCanvas, prefixCount, false);
            m_underlayCache = prefixCanvas;
            m_underlayBaseRev = m_baseRevision;
            m_underlayElements.clear();
            m_underlayElements.reserve(prefixCount);
            for (size_t i = 0; i < prefixCount; ++i) {
                if (m_elements[i]) {
                    MarkupElement spec = *m_elements[i];
                    spec.isActive = false;
                    spec.isEditing = false;
                    m_underlayElements.push_back(std::move(spec));
                }
            }
        }

        cv::Mat result = m_underlayCache.clone();
        renderElement(result, *m_elements.back(), false);
        if (includeActiveHandles) {
            for (const auto& elem : m_elements) {
                if (elem && elem->isActive) {
                    if (const auto* handler = MarkupToolRegistry::instance().getHandler(elem->tool)) {
                        handler->renderActiveHandles(result, *elem);
                    }
                }
            }
        }
        return result;
    }

    cv::Mat result = m_baseImage.clone();
    renderAll(result, includeActiveHandles);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 元素操作
// ─────────────────────────────────────────────────────────────────────────────

MarkupElement* MarkupEngine::addElement(std::unique_ptr<MarkupElement> element) {
    if (!element) return nullptr;
    pushUndoSnapshot();
    if (element->id == 0) {
        element->id = g_elementIdCounter++;
    }
    m_elements.push_back(std::move(element));
    return m_elements.back().get();
}

bool MarkupEngine::undo() {
    if (m_undoHistory.empty()) return false;

    MarkupSnapshot current = captureSnapshot();
    if (m_redoHistory.size() >= kMaxUndoSteps) {
        m_redoHistory.pop_front();
    }
    m_redoHistory.push_back(std::move(current));

    MarkupSnapshot prev = std::move(m_undoHistory.back());
    m_undoHistory.pop_back();
    restoreSnapshot(prev);

    LOG_DEBUG("标注引擎: 撤销, 剩余元素数={}", m_elements.size());
    return true;
}

bool MarkupEngine::redo() {
    if (m_redoHistory.empty()) return false;

    MarkupSnapshot current = captureSnapshot();
    if (m_undoHistory.size() >= kMaxUndoSteps) {
        m_undoHistory.pop_front();
    }
    m_undoHistory.push_back(std::move(current));

    MarkupSnapshot next = std::move(m_redoHistory.back());
    m_redoHistory.pop_back();
    restoreSnapshot(next);

    LOG_DEBUG("标注引擎: 重做, 剩余元素数={}", m_elements.size());
    return true;
}

void MarkupEngine::clearAll() {
    m_elements.clear();
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_filterRoiCache.clear();
    m_underlayCache.release();
    m_underlayElements.clear();
    resetAllNumbers();
}

void MarkupEngine::resetSession() {
    clearAll();
    m_baseImage.release();
    m_underlayCache.release();
    m_filterRoiCache.clear();
    m_elements.shrink_to_fit();
    m_underlayElements.shrink_to_fit();
    ++m_baseRevision;
}

bool MarkupEngine::clearElementsUndoable() {
    if (m_elements.empty()) {
        resetAllNumbers();
        return false;
    }
    pushUndoSnapshot();
    m_elements.clear();
    m_underlayCache.release();
    m_underlayElements.clear();
    resetAllNumbers();
    return true;
}

void MarkupEngine::removeElement(uint32_t id) {
    const auto found = std::find_if(m_elements.begin(), m_elements.end(),
        [id](const std::unique_ptr<MarkupElement>& e) { return e && e->id == id; });
    if (found == m_elements.end()) return;

    pushUndoSnapshot();
    m_elements.erase(std::remove_if(m_elements.begin(), m_elements.end(),
        [id](const std::unique_ptr<MarkupElement>& e) { return e && e->id == id; }),
        m_elements.end());
    m_filterRoiCache.erase(id);
    m_underlayCache.release();
    m_underlayElements.clear();

    recalculateNextNumbers();
}

HitResult MarkupEngine::getElementAtEx(cv::Point pt, int padding) const {
    // 倒序查找，优先命中上层元素
    for (auto it = m_elements.rbegin(); it != m_elements.rend(); ++it) {
        HitArea area = (*it)->hitTestEx(pt, padding);
        if (area != HitArea::None) {
            return { it->get(), area };
        }
    }
    return { nullptr, HitArea::None };
}

MarkupElement* MarkupEngine::getElementById(uint32_t id) const {
    for (const auto& elem : m_elements) {
        if (elem->id == id) return elem.get();
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// 工具快捷方法
// ─────────────────────────────────────────────────────────────────────────────

MarkupElement* MarkupEngine::drawRectangle(cv::Point p1, cv::Point p2, MarkupColor color, float thickness) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Rectangle;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->color = color;
    elem->thickness = thickness;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawLine(cv::Point p1, cv::Point p2, MarkupColor color, float thickness, LineStyle style) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Line;
    elem->startPt = p1;
    elem->endPt = p2;
    elem->color = color;
    elem->thickness = thickness;
    elem->lineStyle = style;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawPolyline(const std::vector<cv::Point>& points, MarkupColor color,
                                          float thickness, LineStyle style, bool smoothCurve) {
    if (points.size() < 2) return nullptr;
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Line;
    elem->isMultiPoint = true;
    elem->penPoints = points;
    elem->startPt = points.front();
    elem->endPt = points.back();
    elem->color = color;
    elem->thickness = thickness;
    elem->lineStyle = style;
    elem->smoothCurve = smoothCurve;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawArrow(cv::Point from, cv::Point to, MarkupColor color, float thickness) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Arrow;
    elem->startPt = from;
    elem->endPt = to;
    elem->color = color;
    elem->thickness = thickness;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawEllipse(cv::Point p1, cv::Point p2, MarkupColor color, float thickness) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Ellipse;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->color = color;
    elem->thickness = thickness;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawPenStroke(const std::vector<cv::Point>& points, MarkupColor color, float thickness) {
    if (points.size() < 2) return nullptr;
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Pen;
    if (points.size() >= 3) {
        elem->penPoints = generateSmoothSpline(points, 8);
    } else {
        elem->penPoints = points;
    }
    elem->color = color;
    elem->thickness = thickness;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawHighlight(cv::Point p1, cv::Point p2, MarkupColor color) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Highlight;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->color = color;
    elem->color.a = 210;  // 正片叠底荧光笔默认高鲜明度
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawTextHighlightSpans(const std::vector<cv::Rect>& spans, MarkupColor color) {
    std::vector<cv::Rect> valid;
    valid.reserve(spans.size());
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (const auto& r : spans) {
        if (r.width <= 0 || r.height <= 0) continue;
        valid.push_back(r);
        minX = (std::min)(minX, r.x);
        minY = (std::min)(minY, r.y);
        maxX = (std::max)(maxX, r.x + r.width);
        maxY = (std::max)(maxY, r.y + r.height);
    }
    if (valid.empty()) return nullptr;

    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Highlight;
    elem->textSpans = std::move(valid);
    elem->startPt = cv::Point(minX, minY);
    elem->endPt = cv::Point(maxX, maxY);
    elem->color = color;
    elem->color.a = 220;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawTextUnderlineSpans(const std::vector<cv::Rect>& spans, MarkupColor color,
                                                    LineStyle style, float thickness) {
    std::vector<cv::Rect> valid;
    valid.reserve(spans.size());
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    int sumH = 0;
    for (const auto& r : spans) {
        if (r.width <= 0 || r.height <= 0) continue;
        valid.push_back(r);
        minX = (std::min)(minX, r.x);
        minY = (std::min)(minY, r.y);
        maxX = (std::max)(maxX, r.x + r.width);
        maxY = (std::max)(maxY, r.y + r.height + 5);
        sumH += r.height;
    }
    if (valid.empty()) return nullptr;

    const float avgH = static_cast<float>(sumH) / static_cast<float>(valid.size());
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Line;
    elem->textSpans = std::move(valid);
    elem->startPt = cv::Point(minX, minY);
    elem->endPt = cv::Point(maxX, maxY);
    elem->color = color;
    elem->color.a = 255;
    elem->lineStyle = style;
    elem->thickness = (thickness > 0.5f)
        ? thickness
        : std::clamp(avgH * 0.095f, 1.8f, 2.85f);
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::applyMosaic(cv::Point p1, cv::Point p2, int blockSize) {
    blockSize = (std::max)(2, blockSize);
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Mosaic;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->mosaicBlockSize = blockSize;
    elem->thickness = static_cast<float>((std::max)(1, blockSize / 2));
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::applyBlur(cv::Point p1, cv::Point p2, int kernelSize) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Blur;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->mosaicBlockSize = kernelSize;
    elem->thickness = static_cast<float>((std::max)(1, kernelSize / 3));
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::addText(cv::Point position, const std::string& text, MarkupColor color, float fontSize,
                                     bool textOutline, MarkupColor outlineColor) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Text;
    elem->startPt = position;
    elem->endPt = position;
    elem->text = text;
    elem->color = color;
    elem->fontSize = fontSize;
    if (m_baseImage.cols > 0) {
        elem->textMaxWidth = CaptureTextEditor::computeAvailableBoxWidth(
            m_baseImage.cols, position.x, static_cast<int>(fontSize));
    }
    elem->textOutline = textOutline;
    elem->textOutlineColor = outlineColor;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawNumberMark(cv::Point position, MarkupColor color, float dpiScale,
                                            NumberBadgeShape shape, NumberFormat format) {
    const size_t slot = numberFormatSlot(format);
    const int assigned = m_nextNumbers[slot];
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Number;
    elem->startPt = position;
    elem->endPt = position;
    elem->color = color;
    elem->numberValue = assigned;
    elem->dpiScale = dpiScale;
    elem->numberShape = shape;
    elem->numberFormat = format;
    MarkupElement* added = addElement(std::move(elem));
    m_nextNumbers[slot] = assigned + 1;
    return added;
}

int MarkupEngine::addNumberMark(cv::Point position, MarkupColor color, float dpiScale,
                                NumberBadgeShape shape, NumberFormat format) {
    auto* elem = drawNumberMark(position, color, dpiScale, shape, format);
    return elem ? elem->numberValue : 0;
}

MarkupElement* MarkupEngine::addMagnifier(cv::Point center, float scale, int radius) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Magnifier;
    elem->startPt = center;
    elem->endPt = center;
    elem->color = MarkupColor::White();
    elem->thickness = 3.0f;
    elem->fill = true; // 默认展示精致倍率微晶胶囊，可通过填充开关切换显隐
    elem->magnifierScale = std::clamp(scale, 1.1f, 8.0f);
    elem->magnifierRadius = std::clamp(radius, 20, 500);
    elem->magnifierRadiusY = elem->magnifierRadius;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::addSpotlight(cv::Point p1, cv::Point p2, MarkupColor color, float dimAlpha, bool ellipse) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Spotlight;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->color = color;
    elem->spotlightDimAlpha = dimAlpha;
    elem->spotlightEllipse = ellipse;
    LOG_DEBUG("标注引擎: 添加聚光灯 ({},{})→({},{}) dimAlpha={:.2f} ellipse={}",
              p1.x, p1.y, p2.x, p2.y, dimAlpha, ellipse);
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::addWatermark(cv::Point p1, cv::Point p2, const std::string& text, float opacity, float angle) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Watermark;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->watermarkText = text;
    elem->watermarkOpacity = opacity;
    elem->watermarkAngle = angle;
    LOG_DEBUG("标注引擎: 添加水印 ({},{})→({},{}) text='{}' opacity={:.2f} angle={:.1f}",
              p1.x, p1.y, p2.x, p2.y, text, opacity, angle);
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::applyInpaint(cv::Point p1, cv::Point p2, int radius) {
    radius = std::clamp(radius, 1, 32);
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Inpaint;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->inpaintRadius = radius;
    elem->thickness = static_cast<float>(radius);
    LOG_DEBUG("标注引擎: 添加智能消除 ({},{})→({},{}) radius={}",
              p1.x, p1.y, p2.x, p2.y, radius);
    return addElement(std::move(elem));
}

// ─────────────────────────────────────────────────────────────────────────────
// 统一 8 方向控制手柄几何与通用包围盒处理引擎 (HandleGeometry & DefaultBoxHandler)
// ─────────────────────────────────────────────────────────────────────────────

std::array<HandleGeometry::HandlePoint, 8> HandleGeometry::getBoxHandles(const cv::Rect& bbox) {
    const int x = bbox.x;
    const int y = bbox.y;
    const int w = bbox.width;
    const int h = bbox.height;
    const int cx = x + w / 2;
    const int cy = y + h / 2;

    return {{
        { cv::Point(x, y),         HitArea::LT },
        { cv::Point(cx, y),        HitArea::T  },
        { cv::Point(x + w, y),     HitArea::RT },
        { cv::Point(x + w, cy),    HitArea::R  },
        { cv::Point(x + w, y + h), HitArea::RB },
        { cv::Point(cx, y + h),    HitArea::B  },
        { cv::Point(x, y + h),     HitArea::LB },
        { cv::Point(x, cy),        HitArea::L  }
    }};
}

HitArea HandleGeometry::hitTestHandles(const cv::Rect& bbox, cv::Point pt, int handleHalfSize) {
    auto handles = getBoxHandles(bbox);
    const int hw = (std::max)(1, handleHalfSize);

    // 优先检测 4 个角手柄
    static constexpr std::array<size_t, 4> kCorners = {0, 2, 4, 6};
    for (size_t idx : kCorners) {
        const auto& hp = handles[idx];
        cv::Rect hrect(hp.point.x - hw, hp.point.y - hw, hw * 2, hw * 2);
        if (hrect.contains(pt)) return hp.area;
    }

    // 其次检测 4 个边中点手柄
    static constexpr std::array<size_t, 4> kEdges = {1, 3, 5, 7};
    for (size_t idx : kEdges) {
        const auto& hp = handles[idx];
        cv::Rect hrect(hp.point.x - hw, hp.point.y - hw, hw * 2, hw * 2);
        if (hrect.contains(pt)) return hp.area;
    }

    return HitArea::None;
}

void HandleGeometry::renderBoxHandles(cv::Mat& canvas, const cv::Rect& bbox,
                                     const cv::Scalar& activeColor,
                                     int handleHalfSize) {
    cv::rectangle(canvas, bbox, activeColor, 1, cv::LINE_AA);

    const int hw = (std::max)(2, handleHalfSize);
    auto handles = getBoxHandles(bbox);

    for (const auto& hp : handles) {
        cv::Rect hrect(hp.point.x - hw, hp.point.y - hw, hw * 2, hw * 2);
        cv::rectangle(canvas, hrect, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
        cv::rectangle(canvas, hrect, activeColor, 1, cv::LINE_AA);
    }
}

cv::Rect HandleGeometry::computeResizedRect(const cv::Rect& originalRect, int dx, int dy, HitArea handle,
                                           int minW, int minH) {
    int l = originalRect.x;
    int t = originalRect.y;
    int r = originalRect.x + originalRect.width;
    int b = originalRect.y + originalRect.height;

    switch (handle) {
        case HitArea::LT: l += dx; t += dy; break;
        case HitArea::T:  t += dy; break;
        case HitArea::RT: r += dx; t += dy; break;
        case HitArea::R:  r += dx; break;
        case HitArea::RB: r += dx; b += dy; break;
        case HitArea::B:  b += dy; break;
        case HitArea::LB: l += dx; b += dy; break;
        case HitArea::L:  l += dx; break;
        default: break;
    }

    if (r - l < minW) {
        if (handle == HitArea::L || handle == HitArea::LT || handle == HitArea::LB) {
            l = r - minW;
        } else {
            r = l + minW;
        }
    }
    if (b - t < minH) {
        if (handle == HitArea::T || handle == HitArea::LT || handle == HitArea::RT) {
            t = b - minH;
        } else {
            b = t + minH;
        }
    }

    return cv::Rect(l, t, r - l, b - t);
}

int HandleGeometry::computeScalarDelta(int dx, int dy, HitArea handle) {
    switch (handle) {
        case HitArea::R:  return dx;
        case HitArea::L:  return -dx;
        case HitArea::B:  return dy;
        case HitArea::T:  return -dy;
        case HitArea::RB: return static_cast<int>(std::round((dx + dy) * 0.5f));
        case HitArea::LT: return static_cast<int>(std::round((-dx - dy) * 0.5f));
        case HitArea::RT: return static_cast<int>(std::round((dx - dy) * 0.5f));
        case HitArea::LB: return static_cast<int>(std::round((-dx + dy) * 0.5f));
        default: return 0;
    }
}

cv::Point HandleGeometry::computeAnchoredOrigin(const cv::Point& currentOrigin, int dW, int dH, HitArea handle) {
    cv::Point origin = currentOrigin;
    switch (handle) {
        case HitArea::R:
            // 右边拉伸：左边固定，垂直中心绝对锚定
            origin.y -= dH / 2;
            break;
        case HitArea::L:
            // 左边拉伸：右边固定，垂直中心绝对锚定
            origin.x -= dW;
            origin.y -= dH / 2;
            break;
        case HitArea::B:
            // 下边拉伸：顶边固定，水平中心绝对锚定
            origin.x -= dW / 2;
            break;
        case HitArea::T:
            // 上边拉伸：底边固定，水平中心绝对锚定
            origin.x -= dW / 2;
            origin.y -= dH;
            break;
        case HitArea::RB:
            // 右下角拉伸：左上角固定
            break;
        case HitArea::LT:
            // 左上角拉伸：右下角固定
            origin.x -= dW;
            origin.y -= dH;
            break;
        case HitArea::RT:
            // 右上角拉伸：左下角固定
            origin.y -= dH;
            break;
        case HitArea::LB:
            // 左下角拉伸：右上角固定
            origin.x -= dW;
            break;
        default:
            break;
    }
    return origin;
}

cv::Rect DefaultBoxHandler::getBoundingBox(const MarkupElement& element) const {
    int x1 = (std::min)(element.startPt.x, element.endPt.x);
    int y1 = (std::min)(element.startPt.y, element.endPt.y);
    int x2 = (std::max)(element.startPt.x, element.endPt.x);
    int y2 = (std::max)(element.startPt.y, element.endPt.y);
    return cv::Rect(x1, y1, x2 - x1, y2 - y1);
}

HitArea DefaultBoxHandler::hitTestHandles(const MarkupElement& element, cv::Point pt, int padding) const {
    if (!element.isActive) return HitArea::None;
    cv::Rect bbox = getBoundingBox(element);
    int hw = (std::max)(padding, HandleGeometry::kDefaultHandleHalfSize);
    return HandleGeometry::hitTestHandles(bbox, pt, hw);
}

HitArea DefaultBoxHandler::hitTest(const MarkupElement& element, cv::Point pt, int padding) const {
    cv::Rect bbox = getBoundingBox(element);
    if (element.isActive) {
        HitArea h = hitTestHandles(element, pt, padding);
        if (h != HitArea::None) return h;
    }
    int p = (std::max)(padding, 6);
    cv::Rect expandedBox(bbox.x - p, bbox.y - p, bbox.width + p * 2, bbox.height + p * 2);
    if (expandedBox.contains(pt)) return HitArea::Body;
    return HitArea::None;
}

void DefaultBoxHandler::resize(MarkupElement& element, int dx, int dy, HitArea handle) const {
    cv::Rect origBox = getBoundingBox(element);
    cv::Rect newBox = HandleGeometry::computeResizedRect(origBox, dx, dy, handle,
                                                        HandleGeometry::kDefaultMinBoxSize,
                                                        HandleGeometry::kDefaultMinBoxSize);
    element.startPt = cv::Point(newBox.x, newBox.y);
    element.endPt = cv::Point(newBox.x + newBox.width, newBox.y + newBox.height);
    LOG_DEBUG("MarkupEngine: DefaultBoxHandler::resize tool={}, handle={}, dx={}, dy={}, bounds=[{},{},{},{}]",
              static_cast<int>(element.tool), static_cast<int>(handle), dx, dy,
              newBox.x, newBox.y, newBox.x + newBox.width, newBox.y + newBox.height);
}

void DefaultBoxHandler::renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const {
    cv::Rect bbox = getBoundingBox(element);
    HandleGeometry::renderBoxHandles(canvas, bbox);
}

// ─────────────────────────────────────────────────────────────────────────────
// 渲染
// ─────────────────────────────────────────────────────────────────────────────

MarkupToolRegistry& MarkupToolRegistry::instance() {
    static MarkupToolRegistry s_instance;
    return s_instance;
}

MarkupToolRegistry::MarkupToolRegistry() {
    markup_detail::registerShapeMarkupHandlers(*this);
    markup_detail::registerEffectMarkupHandlers(*this);
}

void MarkupToolRegistry::registerHandler(MarkupTool tool, std::shared_ptr<IMarkupToolHandler> handler) {
    m_handlers[tool] = std::move(handler);
}

const IMarkupToolHandler* MarkupToolRegistry::getHandler(MarkupTool tool) const {
    auto it = m_handlers.find(tool);
    if (it != m_handlers.end()) {
        return it->second.get();
    }
    return nullptr;
}

namespace {

uint64_t computeSampledRoiChecksum(const cv::Mat& canvas, const cv::Rect& sampleRect) noexcept {
    if (canvas.empty() || sampleRect.width <= 0 || sampleRect.height <= 0) return 0;
    uint64_t hash = 14695981039346656037ULL;
    const int stepY = (std::max)(1, sampleRect.height / 16);
    const int stepX = (std::max)(1, sampleRect.width / 16);
    const int ch = canvas.channels();
    for (int y = sampleRect.y; y < sampleRect.y + sampleRect.height; y += stepY) {
        const uchar* row = canvas.ptr<uchar>(y);
        for (int x = sampleRect.x; x < sampleRect.x + sampleRect.width; x += stepX) {
            const uchar* px = row + static_cast<size_t>(x) * static_cast<size_t>(ch);
            for (int c = 0; c < ch; ++c) {
                hash ^= static_cast<uint64_t>(px[c]);
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

} // namespace

void MarkupEngine::renderFilterElementCached(cv::Mat& canvas, const MarkupElement& elem) const {
    const auto* handler = MarkupToolRegistry::instance().getHandler(elem.tool);
    if (!handler || canvas.empty()) return;

    const int x1 = (std::max)(0, (std::min)(elem.startPt.x, elem.endPt.x));
    const int y1 = (std::max)(0, (std::min)(elem.startPt.y, elem.endPt.y));
    const int x2 = (std::min)(canvas.cols, (std::max)(elem.startPt.x, elem.endPt.x));
    const int y2 = (std::min)(canvas.rows, (std::max)(elem.startPt.y, elem.endPt.y));
    if (x2 <= x1 || y2 <= y1) return;

    const cv::Rect dstRoi(x1, y1, x2 - x1, y2 - y1);
    const int paramVal = (elem.tool == MarkupTool::Inpaint) ? elem.inpaintRadius : elem.mosaicBlockSize;

    cv::Rect sampleRoi = dstRoi;
    if (elem.tool == MarkupTool::Inpaint) {
        const double safeRadius = std::clamp(static_cast<double>(elem.inpaintRadius), 1.0, 32.0);
        const int pad = (std::max)(16, static_cast<int>(std::ceil(safeRadius * 3.0)));
        sampleRoi = cv::Rect(x1 - pad, y1 - pad, (x2 - x1) + pad * 2, (y2 - y1) + pad * 2) &
                    cv::Rect(0, 0, canvas.cols, canvas.rows);
    }

    if (elem.id != 0) {
        const uint64_t checksum = computeSampledRoiChecksum(canvas, sampleRoi);
        auto it = m_filterRoiCache.find(elem.id);
        if (it != m_filterRoiCache.end()) {
            const auto& entry = it->second;
            if (entry.tool == elem.tool &&
                entry.dstRoi == dstRoi &&
                entry.paramValue == paramVal &&
                entry.channels == canvas.channels() &&
                entry.inputChecksum == checksum &&
                entry.resultPatch.size() == dstRoi.size() &&
                entry.resultPatch.type() == canvas.type()) {
                entry.resultPatch.copyTo(canvas(dstRoi));
                return;
            }
        }

        handler->render(canvas, elem);

        if (m_filterRoiCache.size() > 64) {
            m_filterRoiCache.clear();
        }
        FilterRoiCacheEntry updated;
        updated.tool = elem.tool;
        updated.dstRoi = dstRoi;
        updated.paramValue = paramVal;
        updated.channels = canvas.channels();
        updated.inputChecksum = checksum;
        updated.resultPatch = canvas(dstRoi).clone();
        m_filterRoiCache[elem.id] = std::move(updated);
        return;
    }

    handler->render(canvas, elem);
}

void MarkupEngine::renderSubset(cv::Mat& canvas, size_t count, bool includeActiveHandles) const {
    if (canvas.empty() || m_elements.empty() || count == 0) return;
    const size_t limit = (std::min)(count, m_elements.size());

    auto isBaseFilterTool = [](MarkupTool t) noexcept {
        return t == MarkupTool::Mosaic || t == MarkupTool::Blur || t == MarkupTool::Inpaint;
    };

    std::vector<const MarkupElement*> spotlights;
    for (size_t i = 0; i < limit; ++i) {
        const auto& elem = m_elements[i];
        if (!elem) continue;
        if (elem->tool == MarkupTool::Spotlight) {
            spotlights.push_back(elem.get());
        } else if (isBaseFilterTool(elem->tool)) {
            renderFilterElementCached(canvas, *elem);
        }
    }

    // 聚光灯联合遮罩层：多区域求亮区并集一次性弱化背景，且保证后续前景箭头/文字/序号始终明亮清晰
    if (!spotlights.empty()) {
        markup_detail::renderUnifiedSpotlights(canvas, spotlights);
    }

    auto syncTextWrapWidthForCanvas = [](const cv::Mat& targetCanvas, const MarkupElement& element) {
        if (element.tool != MarkupTool::Text || element.hasCustomTextWidth || targetCanvas.cols <= 0) return;
        const float maxW = CaptureTextEditor::computeAvailableBoxWidth(
            targetCanvas.cols, element.startPt.x, static_cast<int>(element.fontSize));
        if (std::abs(element.textMaxWidth - maxW) > 0.5f) {
            auto& mut = const_cast<MarkupElement&>(element);
            mut.textMaxWidth = maxW;
            mut.textRenderSize = cv::Size(0, 0);
        }
    };

    auto renderWithColorOpacity = [&](cv::Mat& targetCanvas, const IMarkupToolHandler* handler, const MarkupElement& element) {
        if (!handler || targetCanvas.empty()) return;
        // 必须在半透明 ROI 局部裁剪之前按完整画布宽度同步换行宽度，杜绝局部 ROI 宽度误导软换行
        syncTextWrapWidthForCanvas(targetCanvas, element);
        if (element.color.a == 0 || element.color.a >= 255 ||
            element.tool == MarkupTool::Highlight ||
            element.tool == MarkupTool::Magnifier ||
            element.tool == MarkupTool::Watermark ||
            element.tool == MarkupTool::Spotlight ||
            element.tool == MarkupTool::Mosaic ||
            element.tool == MarkupTool::Blur ||
            element.tool == MarkupTool::Inpaint) {
            handler->render(targetCanvas, element);
            return;
        }
        const cv::Rect rawBox = handler->getBoundingBox(element);
        const int pad = (std::max)(48, static_cast<int>(std::ceil(element.thickness * 8.0f + element.fontSize * 1.5f)));
        const cv::Rect expanded(rawBox.x - pad, rawBox.y - pad, rawBox.width + pad * 2, rawBox.height + pad * 2);
        const cv::Rect roi = expanded & cv::Rect(0, 0, targetCanvas.cols, targetCanvas.rows);
        if (roi.width <= 0 || roi.height <= 0) {
            return;
        }
        const double alpha = std::clamp(static_cast<double>(element.color.a) / 255.0, 0.05, 1.0);
        cv::Mat dstRoi = targetCanvas(roi);
        cv::Mat layerRoi = dstRoi.clone();
        MarkupElement localElem = element;
        localElem.moveBy(-roi.x, -roi.y);
        handler->render(layerRoi, localElem);
        if (element.tool == MarkupTool::Text && localElem.textRenderSize.width > 0 && localElem.textRenderSize.height > 0) {
            const_cast<MarkupElement&>(element).textRenderSize = localElem.textRenderSize;
            const_cast<MarkupElement&>(element).endPt =
                cv::Point(element.startPt.x + localElem.textRenderSize.width,
                          element.startPt.y + localElem.textRenderSize.height);
        }
        cv::addWeighted(layerRoi, alpha, dstRoi, 1.0 - alpha, 0.0, dstRoi);
    };

    for (size_t i = 0; i < limit; ++i) {
        const auto& elem = m_elements[i];
        if (!elem || elem->tool == MarkupTool::Spotlight || isBaseFilterTool(elem->tool)) continue;
        if (const auto* handler = MarkupToolRegistry::instance().getHandler(elem->tool)) {
            renderWithColorOpacity(canvas, handler, *elem);
        }
    }

    if (includeActiveHandles) {
        for (size_t i = 0; i < limit; ++i) {
            const auto& elem = m_elements[i];
            if (elem && elem->isActive) {
                if (const auto* handler = MarkupToolRegistry::instance().getHandler(elem->tool)) {
                    handler->renderActiveHandles(canvas, *elem);
                }
            }
        }
    }
}

void MarkupEngine::renderAll(cv::Mat& canvas, bool includeActiveHandles) const {
    renderSubset(canvas, m_elements.size(), includeActiveHandles);
}

void MarkupEngine::renderElement(cv::Mat& canvas, const MarkupElement& element, bool includeActiveHandles) const {
    if (const auto* handler = MarkupToolRegistry::instance().getHandler(element.tool)) {
        if (element.tool == MarkupTool::Text && !element.hasCustomTextWidth && canvas.cols > 0) {
            const float maxW = CaptureTextEditor::computeAvailableBoxWidth(
                canvas.cols, element.startPt.x, static_cast<int>(element.fontSize));
            if (std::abs(element.textMaxWidth - maxW) > 0.5f) {
                auto& mut = const_cast<MarkupElement&>(element);
                mut.textMaxWidth = maxW;
                mut.textRenderSize = cv::Size(0, 0);
            }
        }
        if (element.color.a > 0 && element.color.a < 255 &&
            element.tool != MarkupTool::Highlight &&
            element.tool != MarkupTool::Magnifier &&
            element.tool != MarkupTool::Watermark &&
            element.tool != MarkupTool::Spotlight &&
            element.tool != MarkupTool::Mosaic &&
            element.tool != MarkupTool::Blur &&
            element.tool != MarkupTool::Inpaint) {
            const cv::Rect rawBox = handler->getBoundingBox(element);
            const int pad = (std::max)(48, static_cast<int>(std::ceil(element.thickness * 8.0f + element.fontSize * 1.5f)));
            const cv::Rect expanded(rawBox.x - pad, rawBox.y - pad, rawBox.width + pad * 2, rawBox.height + pad * 2);
            const cv::Rect roi = expanded & cv::Rect(0, 0, canvas.cols, canvas.rows);
            if (roi.width > 0 && roi.height > 0) {
                const double alpha = std::clamp(static_cast<double>(element.color.a) / 255.0, 0.05, 1.0);
                cv::Mat dstRoi = canvas(roi);
                cv::Mat layerRoi = dstRoi.clone();
                MarkupElement localElem = element;
                localElem.moveBy(-roi.x, -roi.y);
                handler->render(layerRoi, localElem);
                if (element.tool == MarkupTool::Text && localElem.textRenderSize.width > 0 && localElem.textRenderSize.height > 0) {
                    const_cast<MarkupElement&>(element).textRenderSize = localElem.textRenderSize;
                    const_cast<MarkupElement&>(element).endPt =
                        cv::Point(element.startPt.x + localElem.textRenderSize.width,
                                  element.startPt.y + localElem.textRenderSize.height);
                }
                cv::addWeighted(layerRoi, alpha, dstRoi, 1.0 - alpha, 0.0, dstRoi);
            }
        } else if (element.tool == MarkupTool::Mosaic ||
                   element.tool == MarkupTool::Blur ||
                   element.tool == MarkupTool::Inpaint) {
            renderFilterElementCached(canvas, element);
        } else {
            handler->render(canvas, element);
        }
        if (includeActiveHandles && element.isActive) {
            handler->renderActiveHandles(canvas, element);
        }
    }
}

void MarkupEngine::renderTextHighlightPreview(cv::Mat& canvas,
                                              const std::vector<cv::Rect>& spans,
                                              MarkupColor color) {
    renderTextSpansHighlightImpl(canvas, spans, color);
}

void MarkupEngine::renderTextUnderlinePreview(cv::Mat& canvas,
                                              const std::vector<cv::Rect>& spans,
                                              MarkupColor color,
                                              LineStyle style,
                                              float thickness) {
    renderTextSpansUnderlineImpl(canvas, spans, color, style, thickness);
}

}  // namespace tools3000::capture
