// ─────────────────────────────────────────────────────────────────────────────
// CornerRadiusHelper.cpp — 跨选区/美化外壳/矩形标注通用圆角手柄交互框架实现
// ─────────────────────────────────────────────────────────────────────────────

#include "CornerRadiusHelper.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <format>
#include <unordered_map>

namespace tools3000::capture {

bool CornerRadiusHelper::canShowHandles(float width, float height, float scale, float minDimension) {
    const float threshold = minDimension * (scale > 0.0f ? scale : 1.0f);
    return width >= threshold && height >= threshold;
}

float CornerRadiusHelper::calculateOffset(float cornerRadius, float width, float height, float scale,
                                         float minOffsetLogical, float baseMinLogical) {
    const float s = (scale > 0.0f ? scale : 1.0f);
    const float minDimension = (std::min)(width, height);
    const float maxOffset = minDimension * 0.40f;
    const float minOffset = minOffsetLogical * s;
    const float baseOffset = (std::max)(baseMinLogical, cornerRadius + 8.0f) * s;

    if (maxOffset < minOffset) {
        return (std::max)(1.0f, maxOffset);
    }
    return std::clamp(baseOffset, minOffset, maxOffset);
}

std::vector<CornerHandle> CornerRadiusHelper::getCornerHandles(
    float left, float top, float right, float bottom,
    float cornerRadius, float scale, bool singlePrimaryCorner)
{
    const float w = right - left;
    const float h = bottom - top;
    const float offset = calculateOffset(cornerRadius, w, h, scale);

    std::vector<CornerHandle> handles;
    handles.reserve(singlePrimaryCorner ? 1 : 4);

    // 0: 左上角 LT (主控制手柄)
    handles.push_back({
        left + offset,
        top + offset,
        0.70710678f,
        0.70710678f,
        0
    });

    if (singlePrimaryCorner) {
        return handles;
    }

    // 1: 右上角 RT
    handles.push_back({
        right - offset,
        top + offset,
        -0.70710678f,
        0.70710678f,
        1
    });

    // 2: 右下角 RB
    handles.push_back({
        right - offset,
        bottom - offset,
        -0.70710678f,
        -0.70710678f,
        2
    });

    // 3: 左下角 LB
    handles.push_back({
        left + offset,
        bottom - offset,
        0.70710678f,
        -0.70710678f,
        3
    });

    return handles;
}

CornerProximityState CornerRadiusHelper::evaluateProximity(
    const std::vector<CornerHandle>& handles,
    float cursorX, float cursorY,
    float scale,
    bool isDragging,
    int dragCornerIndex,
    float dragStartX,
    float dragStartY) noexcept
{
    CornerProximityState result{};
    if (handles.empty()) {
        return result;
    }

    const float s = (scale > 0.0f ? scale : 1.0f);
    const int count = static_cast<int>(handles.size());

    if (isDragging) {
        if (dragCornerIndex >= 0 && dragCornerIndex < count) {
            result.activeCornerIndex = dragCornerIndex;
        } else {
            float minD = 1e9f;
            for (int i = 0; i < count; ++i) {
                const float d = std::hypot(dragStartX - handles[i].x, dragStartY - handles[i].y);
                if (d < minD) {
                    minD = d;
                    result.activeCornerIndex = i;
                }
            }
        }
        result.visibilityAlpha = 1.0f;
        result.isDirectHover = true;
        return result;
    }

    const float revealR = kProximityRevealRadiusLogical * s;
    const float directR = kDirectHoverRadiusLogical * s;

    float minD = 1e9f;
    int nearestIdx = -1;
    for (int i = 0; i < count; ++i) {
        const float d = std::hypot(cursorX - handles[i].x, cursorY - handles[i].y);
        if (d <= revealR && d < minD) {
            minD = d;
            nearestIdx = i;
        }
    }

    if (nearestIdx < 0) {
        return result; // 远离角部：100% 隐形零噪点
    }

    result.activeCornerIndex = nearestIdx;
    if (minD <= directR) {
        result.visibilityAlpha = 1.0f;
        result.isDirectHover = true;
    } else {
        // 平滑三次 Hermite (smoothstep) 近场渐显曲线，越靠近角部越清晰温润
        const float t = std::clamp((revealR - minD) / (std::max)(1.0f, revealR - directR), 0.0f, 1.0f);
        const float smooth = t * t * (3.0f - 2.0f * t);
        result.visibilityAlpha = 0.22f + 0.78f * smooth;
        result.isDirectHover = false;
    }
    return result;
}

int CornerRadiusHelper::hitTestHandles(
    float left, float top, float right, float bottom,
    float cornerRadius, float px, float py,
    float scale, float hitRadius, bool /*singlePrimaryCorner*/)
{
    // 精准收敛命中热区至四角珍珠微晶锚点本身，彻底消除导轨与胶囊占用选区腹地引发的误触
    const auto handles = getCornerHandles(left, top, right, bottom, cornerRadius, scale, false);
    const float s = (scale > 0.0f ? scale : 1.0f);
    const float effectiveHitR = (std::max)(kDirectHoverRadiusLogical * s, hitRadius);

    int bestIdx = -1;
    float bestDist = effectiveHitR;
    for (const auto& hnd : handles) {
        const float dist = std::hypot(px - hnd.x, py - hnd.y);
        if (dist <= bestDist) {
            bestDist = dist;
            bestIdx = hnd.cornerIndex;
        }
    }
    return bestIdx;
}

float CornerRadiusHelper::calculateDraggedRadius(
    float left, float top, float right, float bottom,
    float dragStartX, float dragStartY,
    float startRadius,
    float currentCursorX, float currentCursorY,
    float maxRadiusLimit,
    int cornerIndex,
    float scale)
{
    const float s = (scale > 0.0f ? scale : 1.0f);
    const float w = right - left;
    const float h = bottom - top;
    const float cx = (left + right) * 0.5f;
    const float cy = (top + bottom) * 0.5f;

    float maxR = ((std::min)(w, h) * 0.5f) / s;
    if (maxRadiusLimit > 0.0f && maxRadiusLimit < maxR) {
        maxR = maxRadiusLimit;
    }

    float signX = 1.0f;
    float signY = 1.0f;
    if (cornerIndex == 0) {
        signX = 1.0f;
        signY = 1.0f;
    } else if (cornerIndex == 1) {
        signX = -1.0f;
        signY = 1.0f;
    } else if (cornerIndex == 2) {
        signX = -1.0f;
        signY = -1.0f;
    } else if (cornerIndex == 3) {
        signX = 1.0f;
        signY = -1.0f;
    } else {
        signX = (dragStartX < cx) ? 1.0f : -1.0f;
        signY = (dragStartY < cy) ? 1.0f : -1.0f;
    }

    const float dx = (currentCursorX - dragStartX) * signX;
    const float dy = (currentCursorY - dragStartY) * signY;
    const float delta = ((dx + dy) * 0.5f) / s;

    return std::clamp(startRadius + delta, 0.0f, maxR);
}

float CornerRadiusHelper::applyIncrementalDrag(
    float currentRadius,
    float dx, float dy,
    float signX, float signY,
    float maxRadius)
{
    const float delta = (dx * signX + dy * signY) * 0.5f;
    return std::clamp(currentRadius + delta, 0.0f, maxRadius);
}

std::wstring CornerRadiusHelper::getToastMessage(CornerRadiusTarget target, float radius, bool isZh) {
    const int rVal = static_cast<int>(std::round(radius));
    switch (target) {
        case CornerRadiusTarget::Selection:
            return isZh ? std::format(L"选区圆角: {} px", rVal) : std::format(L"Selection Radius: {} px", rVal);
        case CornerRadiusTarget::BeautifyShell:
            return isZh ? std::format(L"美化外壳圆角: {} px", rVal) : std::format(L"Beauty Shell Radius: {} px", rVal);
        case CornerRadiusTarget::RectangleMarkup:
            return isZh ? std::format(L"矩形圆角: {} px", rVal) : std::format(L"Corner Radius: {} px", rVal);
        default:
            return isZh ? std::format(L"圆角半径: {} px", rVal) : std::format(L"Radius: {} px", rVal);
    }
}

HCURSOR CornerRadiusHelper::getLiveCursor(int cornerIndex, float dpiScale, bool isDragging) {
    const int normCorner = (cornerIndex >= 0 && cornerIndex <= 3) ? cornerIndex : 0;
    const bool isNesw = (normCorner == 1 || normCorner == 3);
    const float monScale = std::clamp(dpiScale > 0.0f ? dpiScale : 1.0f, 1.0f, 4.0f);

    // 同步感知 Windows 辅助功能“鼠标指针大小”倍率 (HKCU\Software\Microsoft\Accessibility\CursorSize, 1..15)
    float userCursorMult = 1.0f;
    DWORD regCursorSize = 1;
    DWORD regCb = sizeof(regCursorSize);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"CursorSize",
                     RRF_RT_REG_DWORD, nullptr, &regCursorSize, &regCb) == ERROR_SUCCESS &&
        regCursorSize > 1) {
        userCursorMult = std::clamp(1.0f + static_cast<float>(regCursorSize - 1) * 0.25f, 1.0f, 2.5f);
    }

    const float effScale = std::clamp(monScale * userCursorMult, 1.0f, 4.0f);
    const int scaleBucket = static_cast<int>(std::round(effScale * 100.0f));
    const int cacheKey = scaleBucket * 8 + normCorner * 2 + (isDragging ? 1 : 0);

    static std::unordered_map<int, HCURSOR> s_liveCornerCursors;
    if (auto it = s_liveCornerCursors.find(cacheKey); it != s_liveCornerCursors.end()) {
        return it->second;
    }

    int sz = std::clamp(static_cast<int>(std::round(48.0f * effScale)), 48, 160);
    if (sz % 2 != 0) ++sz;
    const int mid = sz / 2;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz; // Top-down 32-bit Premultiplied ARGB DIB
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    uint32_t* pixels = nullptr;
    HDC hdcScreen = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmColor = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);

    if (!memDC || !hbmColor || !pixels) {
        if (hbmColor) DeleteObject(hbmColor);
        if (memDC) DeleteDC(memDC);
        HCURSOR fallback = LoadCursor(nullptr, isNesw ? IDC_SIZENESW : IDC_SIZENWSE);
        s_liveCornerCursors[cacheKey] = fallback;
        return fallback;
    }

    std::memset(pixels, 0, static_cast<size_t>(sz) * sz * sizeof(uint32_t));
    HGDIOBJ oldBmp = SelectObject(memDC, hbmColor);

    bool renderedByD2D = false;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;
    if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf())) && d2dFactory) {
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt;
        RECT bindRect{ 0, 0, sz, sz };
        if (SUCCEEDED(d2dFactory->CreateDCRenderTarget(&rtProps, rt.GetAddressOf())) &&
            rt && SUCCEEDED(rt->BindDC(memDC, &bindRect))) {
            rt->SetDpi(96.0f, 96.0f);
            rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStroke;
            D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND);
            d2dFactory->CreateStrokeStyle(sp, nullptr, 0, roundStroke.GetAddressOf());

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadowBrush, whiteBrush, darkBrush, irisBrush;
            rt->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.28f), shadowBrush.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.99f), whiteBrush.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.96f), darkBrush.GetAddressOf());
            rt->CreateSolidColorBrush(
                isDragging
                    ? D2D1::ColorF(139.0f / 255.0f, 92.0f / 255.0f, 246.0f / 255.0f, 0.99f)
                    : D2D1::ColorF(124.0f / 255.0f, 58.0f / 255.0f, 237.0f / 255.0f, 0.97f),
                irisBrush.GetAddressOf());

            const float cx = static_cast<float>(mid);
            const float cy = static_cast<float>(mid);
            const float s = effScale;

            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

            // 悬停态与拖拽态共享 22px 黄金视觉体量（与 Windows 原生 IDC_SIZENWSE 1:1 比例协调，拖拽时不缩水跳变）
            float inX = 0.70710678f;
            float inY = 0.70710678f;
            float outDeg = 225.0f;
            switch (normCorner) {
                case 0: inX =  0.70710678f; inY =  0.70710678f; outDeg = 225.0f; break; // LT
                case 1: inX = -0.70710678f; inY =  0.70710678f; outDeg = 315.0f; break; // RT
                case 2: inX = -0.70710678f; inY = -0.70710678f; outDeg =  45.0f; break; // RB
                default:inX =  0.70710678f; inY = -0.70710678f; outDeg = 135.0f; break; // LB
            }
            const float perpX = -inY;
            const float perpY =  inX;

            // A. 构造舒展饱满的 45 度圆头双向缩放箭头（总轴长 ~16.8px * s，翼展饱满清晰）
            const float arrowShift = (isDragging ? 2.2f : 2.6f) * s;
            const float arrowCx = cx + inX * arrowShift;
            const float arrowCy = cy + inY * arrowShift;
            const float halfSpan = (isDragging ? 7.8f : 8.4f) * s;
            const float wingLen = 4.4f * s;
            const float wingSpread = 3.6f * s;
            const D2D1_POINT_2F pOut = D2D1::Point2F(arrowCx - inX * halfSpan, arrowCy - inY * halfSpan);
            const D2D1_POINT_2F pIn  = D2D1::Point2F(arrowCx + inX * halfSpan, arrowCy + inY * halfSpan);

            Microsoft::WRL::ComPtr<ID2D1PathGeometry> arrowGeo;
            if (SUCCEEDED(d2dFactory->CreatePathGeometry(arrowGeo.GetAddressOf())) && arrowGeo) {
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(arrowGeo->Open(sink.GetAddressOf())) && sink) {
                    sink->BeginFigure(pOut, D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(pIn);
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);

                    sink->BeginFigure(
                        D2D1::Point2F(pOut.x + inX * wingLen + perpX * wingSpread,
                                      pOut.y + inY * wingLen + perpY * wingSpread),
                        D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(pOut);
                    sink->AddLine(
                        D2D1::Point2F(pOut.x + inX * wingLen - perpX * wingSpread,
                                      pOut.y + inY * wingLen - perpY * wingSpread));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);

                    sink->BeginFigure(
                        D2D1::Point2F(pIn.x - inX * wingLen + perpX * wingSpread,
                                      pIn.y - inY * wingLen + perpY * wingSpread),
                        D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(pIn);
                    sink->AddLine(
                        D2D1::Point2F(pIn.x - inX * wingLen - perpX * wingSpread,
                                      pIn.y - inY * wingLen - perpY * wingSpread));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                }
            }

            // B. 构造外角侧 78 度平滑圆角曲率导弧路径（与箭头外端留出 ~4.6px 清透呼吸间隙）
            const float arcCenterX = cx + inX * 1.4f * s;
            const float arcCenterY = cy + inY * 1.4f * s;
            const float arcR = (isDragging ? 11.4f : 11.8f) * s;
            constexpr float kDegToRad = 3.1415926535f / 180.0f;
            const float radStart = (outDeg - 39.0f) * kDegToRad;
            const float radEnd   = (outDeg + 39.0f) * kDegToRad;
            const D2D1_POINT_2F arcStart = D2D1::Point2F(
                arcCenterX + arcR * std::cos(radStart),
                arcCenterY + arcR * std::sin(radStart));
            const D2D1_POINT_2F arcEnd = D2D1::Point2F(
                arcCenterX + arcR * std::cos(radEnd),
                arcCenterY + arcR * std::sin(radEnd));

            Microsoft::WRL::ComPtr<ID2D1PathGeometry> arcGeo;
            if (SUCCEEDED(d2dFactory->CreatePathGeometry(arcGeo.GetAddressOf())) && arcGeo) {
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(arcGeo->Open(sink.GetAddressOf())) && sink) {
                    sink->BeginFigure(arcStart, D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc(D2D1::ArcSegment(
                        arcEnd, D2D1::SizeF(arcR, arcR), 0.0f,
                        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                }
            }

            // 1) 柔和向下微阴影层
            if (shadowBrush) {
                rt->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, 1.0f * s));
                if (arrowGeo) rt->DrawGeometry(arrowGeo.Get(), shadowBrush.Get(), 4.5f * s, roundStroke.Get());
                if (arcGeo)   rt->DrawGeometry(arcGeo.Get(), shadowBrush.Get(), 4.7f * s, roundStroke.Get());
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
            }
            // 2) 高反差纯白瓷感外护边
            if (whiteBrush) {
                if (arrowGeo) rt->DrawGeometry(arrowGeo.Get(), whiteBrush.Get(), 3.8f * s, roundStroke.Get());
                if (arcGeo)   rt->DrawGeometry(arcGeo.Get(), whiteBrush.Get(), 4.0f * s, roundStroke.Get());
            }
            // 3) 内芯线：悬停态深曜岩箭头 + 紫晶弧；拖拽激活态整枚指针联动泛起紫晶微光
            ID2D1SolidColorBrush* arrowCore = isDragging ? irisBrush.Get() : darkBrush.Get();
            if (arrowCore && arrowGeo) {
                rt->DrawGeometry(arrowGeo.Get(), arrowCore, 1.85f * s, roundStroke.Get());
            }
            if (irisBrush && arcGeo) {
                rt->DrawGeometry(arcGeo.Get(), irisBrush.Get(), 2.15f * s, roundStroke.Get());
            }

            renderedByD2D = SUCCEEDED(rt->EndDraw());
        }
    }

    SelectObject(memDC, oldBmp);
    DeleteDC(memDC);

    if (!renderedByD2D) {
        DeleteObject(hbmColor);
        HCURSOR fallback = LoadCursor(nullptr, isNesw ? IDC_SIZENESW : IDC_SIZENWSE);
        s_liveCornerCursors[cacheKey] = fallback;
        return fallback;
    }

    // 32-bit 预乘 Alpha 硬件光标：AND 掩码全 0 由 DWM 直接按逐像素 Alpha 通道丝滑合成抗锯齿边缘
    const int maskStride = ((sz + 15) / 16) * 2;
    std::vector<uint8_t> maskBits(static_cast<size_t>(maskStride) * sz, 0x00);
    HBITMAP hbmMask = CreateBitmap(sz, sz, 1, 1, maskBits.data());

    ICONINFO ii = {};
    ii.fIcon = FALSE;
    ii.xHotspot = static_cast<DWORD>(mid);
    ii.yHotspot = static_cast<DWORD>(mid);
    ii.hbmMask = hbmMask;
    ii.hbmColor = hbmColor;

    HCURSOR cur = CreateIconIndirect(&ii);
    if (hbmMask) DeleteObject(hbmMask);
    DeleteObject(hbmColor);

    if (!cur) cur = LoadCursor(nullptr, isNesw ? IDC_SIZENESW : IDC_SIZENWSE);
    s_liveCornerCursors[cacheKey] = cur;
    return cur;
}

} // namespace tools3000::capture
