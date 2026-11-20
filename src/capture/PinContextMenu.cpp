// ─────────────────────────────────────────────────────────────────────────────
// PinContextMenu.cpp — 贴图 Direct2D 微晶自绘右键复合菜单宿主与渲染引擎实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinContextMenu.h"
#include "core/i18n/UiI18n.h"
#include "capture/FloatingGlassBar.h"
#include "capture/PinInputController.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <windowsx.h>

namespace tools3000::capture {

using Microsoft::WRL::ComPtr;

namespace {
constexpr const wchar_t* kMenuRootWndClass = L"Tools3000_PinContextMenuRoot";
constexpr const wchar_t* kMenuSubWndClass  = L"Tools3000_PinContextMenuSub";

bool ptInRectF(const D2D1_RECT_F& r, float x, float y) noexcept {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}
} // namespace

bool PinContextMenu::s_rootClassRegistered = false;
bool PinContextMenu::s_subClassRegistered = false;

void PinContextMenu::PopupPlane::releaseSurface() noexcept {
    if (memDc) {
        if (oldBmp) {
            SelectObject(memDc, oldBmp);
            oldBmp = nullptr;
        }
        DeleteDC(memDc);
        memDc = nullptr;
    }
    if (dib) {
        DeleteObject(dib);
        dib = nullptr;
    }
    dibW = 0;
    dibH = 0;
}

PinContextMenu& PinContextMenu::instance() {
    static PinContextMenu inst;
    return inst;
}

PinContextMenu::~PinContextMenu() {
    shutdown();
}

void PinContextMenu::dismiss() noexcept {
    if (m_running) {
        m_running = false;
        if (m_planes[0].hwnd && IsWindow(m_planes[0].hwnd)) {
            PostMessageW(m_planes[0].hwnd, WM_NULL, 0, 0);
        }
    }
}

void PinContextMenu::shutdown() noexcept {
    dismiss();
    for (auto& p : m_planes) {
        if (p.hwnd && IsWindow(p.hwnd)) {
            KillTimer(p.hwnd, PinContextMenuMetrics::kSubmenuHoverTimerId);
            DestroyWindow(p.hwnd);
        }
        p.hwnd = nullptr;
        p.visible = false;
        p.releaseSurface();
    }
    m_headerLabelFmt.Reset();
    m_itemLabelFmt.Reset();
    m_itemLabelBoldFmt.Reset();
    m_shortcutFmt.Reset();
    m_badgeFmt.Reset();
    m_dcTarget.Reset();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
    m_cachedFontScale = 0.0f;
}

PinMenuCommand PinContextMenu::showContextMenu(HWND ownerHwnd,
                                               POINT anchorScreenPt,
                                               const PinContextMenuSnapshot& snapshot,
                                               std::function<void(float)> onLiveOpacityChange,
                                               std::function<bool()> isOwnerAlive) {
    const PinContextMenuData rootData = PinContextMenuBuilder::buildRootMenu(snapshot);
    return runModalMenu(ownerHwnd, anchorScreenPt, rootData, snapshot,
                        std::move(onLiveOpacityChange), std::move(isOwnerAlive));
}

PinMenuCommand PinContextMenu::showGroupMenu(HWND ownerHwnd,
                                             POINT anchorScreenPt,
                                             const PinContextMenuSnapshot& snapshot,
                                             std::function<bool()> isOwnerAlive) {
    const PinContextMenuData groupData = PinContextMenuBuilder::buildGroupOnlyMenu(snapshot);
    return runModalMenu(ownerHwnd, anchorScreenPt, groupData, snapshot,
                        nullptr, std::move(isOwnerAlive));
}

bool PinContextMenu::ensureGraphicsResources(float dpiScale, bool isChinese) {
    const float safeScale = std::clamp(dpiScale > 0.0f ? dpiScale : 1.0f, 1.0f, 4.0f);

    if (!m_d2dFactory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf()))) {
            return false;
        }
    }
    if (!m_dcTarget) {
        const auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, m_dcTarget.GetAddressOf()))) {
            return false;
        }
        m_dcTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!m_dwriteFactory) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
            return false;
        }
    }

    if (m_headerLabelFmt && m_itemLabelFmt && m_itemLabelBoldFmt &&
        m_shortcutFmt && m_badgeFmt &&
        std::abs(m_cachedFontScale - safeScale) <= 0.005f &&
        m_cachedFontChinese == isChinese) {
        return true;
    }

    m_headerLabelFmt.Reset();
    m_itemLabelFmt.Reset();
    m_itemLabelBoldFmt.Reset();
    m_shortcutFmt.Reset();
    m_badgeFmt.Reset();

    const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
    const wchar_t* locale = tools3000::core::trWCstrFor(isChinese, L"zh-CN", L"en-US");

    const float headerPx   = (std::max)(PinContextMenuMetrics::kMinUiFontPx, PinContextMenuMetrics::kHeaderLabelFontDip * safeScale);
    const float itemPx     = (std::max)(PinContextMenuMetrics::kMinUiFontPx, PinContextMenuMetrics::kItemLabelFontDip * safeScale);
    const float shortcutPx = (std::max)(PinContextMenuMetrics::kMinUiFontPx, PinContextMenuMetrics::kShortcutFontDip * safeScale);
    const float badgePx    = (std::max)(PinContextMenuMetrics::kMinUiFontPx, PinContextMenuMetrics::kBadgeFontDip * safeScale);

    auto createFmt = [&](float px, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align, IDWriteTextFormat** outFmt) -> bool {
        if (FAILED(m_dwriteFactory->CreateTextFormat(
                uiFont, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, px, locale, outFmt)) || !(*outFmt)) {
            return false;
        }
        (*outFmt)->SetTextAlignment(align);
        (*outFmt)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        (*outFmt)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };

    if (!createFmt(headerPx,   DWRITE_FONT_WEIGHT_MEDIUM,    DWRITE_TEXT_ALIGNMENT_CENTER,   m_headerLabelFmt.GetAddressOf()) ||
        !createFmt(itemPx,     DWRITE_FONT_WEIGHT_MEDIUM,    DWRITE_TEXT_ALIGNMENT_LEADING,  m_itemLabelFmt.GetAddressOf()) ||
        !createFmt(itemPx,     DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,  m_itemLabelBoldFmt.GetAddressOf()) ||
        !createFmt(shortcutPx, DWRITE_FONT_WEIGHT_MEDIUM,    DWRITE_TEXT_ALIGNMENT_TRAILING, m_shortcutFmt.GetAddressOf()) ||
        !createFmt(badgePx,    DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,   m_badgeFmt.GetAddressOf())) {
        return false;
    }

    m_cachedFontScale = safeScale;
    m_cachedFontChinese = isChinese;
    return true;
}

float PinContextMenu::measureTextWidth(const std::wstring& text, float fontPx) {
    if (text.empty() || !m_dwriteFactory) return 0.0f;
    IDWriteTextFormat* fmt = m_itemLabelFmt.Get();
    if (std::abs(fontPx - (std::max)(PinContextMenuMetrics::kMinUiFontPx, PinContextMenuMetrics::kShortcutFontDip * m_cachedFontScale)) < 0.25f &&
        m_shortcutFmt) {
        fmt = m_shortcutFmt.Get();
    }
    if (!fmt) return std::ceil(static_cast<float>(text.size()) * fontPx * 0.8f);

    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()),
            fmt, 2000.0f, 80.0f, layout.GetAddressOf())) && layout) {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) {
            return std::ceil(metrics.widthIncludingTrailingWhitespace);
        }
    }
    return std::ceil(static_cast<float>(text.size()) * fontPx * 0.8f);
}

bool PinContextMenu::ensurePlaneWindow(int level, HWND ownerHwnd) {
    if (level < 0 || level >= PinContextMenuMetrics::kMaxCascadeDepth) return false;
    auto& plane = m_planes[static_cast<size_t>(level)];
    plane.level = level;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const bool isRoot = (level == 0);
    const wchar_t* clsName = isRoot ? kMenuRootWndClass : kMenuSubWndClass;

    if (isRoot && !s_rootClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = planeWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kMenuRootWndClass;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            s_rootClassRegistered = true;
        }
    } else if (!isRoot && !s_subClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = planeWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kMenuSubWndClass;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            s_subClassRegistered = true;
        }
    }

    if (plane.hwnd && IsWindow(plane.hwnd)) {
        SetWindowLongPtrW(plane.hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerHwnd));
        return true;
    }

    const DWORD exStyle = isRoot
        ? (WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW)
        : (WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);

    plane.hwnd = CreateWindowExW(
        exStyle, clsName, L"", WS_POPUP,
        0, 0, 16, 16,
        ownerHwnd, nullptr, hInst, this);
    if (!plane.hwnd) return false;

    SetWindowLongPtrW(plane.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetPropW(plane.hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
    return true;
}

void PinContextMenu::openRootPlane(POINT anchorScreenPt, float maxWorkAreaH) {
    if (!m_activeRootData) return;
    auto& root = m_planes[0];
    root.hasHeaderBar = m_activeRootData->hasHeaderBar;
    root.hasOpacitySlider = m_activeRootData->hasOpacitySlider;
    root.items = &m_activeRootData->items;
    root.hoveredHeaderIndex = -1;
    root.pressedHeaderIndex = -1;
    root.hoveredItemIndex = -1;
    root.pressedItemIndex = -1;
    root.openSubmenuParentIndex = -1;
    root.hoverSliderThumb = false;
    root.hoverSliderPctPill = false;
    root.scrollOffsetY = 0.0f;

    root.layout = PinContextMenuLayoutEngine::computePanelLayout(
        root.hasHeaderBar,
        root.hasHeaderBar ? &m_activeRootData->headerButtons : nullptr,
        root.hasOpacitySlider,
        m_activeRootData->opacityLabel,
        *root.items,
        m_dpiScale,
        maxWorkAreaH,
        [this](const std::wstring& txt, float px) { return measureTextWidth(txt, px); });

    const float shadowPx = std::ceil(PinContextMenuMetrics::kShadowMarginDip * m_dpiScale);
    root.windowPos = PinContextMenuLayoutEngine::placeRootWindow(
        anchorScreenPt, root.layout.windowSize, m_workArea, shadowPx);
    root.visible = true;

    renderPlane(0);
    SetWindowPos(root.hwnd, HWND_TOPMOST,
                 root.windowPos.x, root.windowPos.y,
                 root.layout.windowSize.cx, root.layout.windowSize.cy,
                 SWP_SHOWWINDOW);
    SetActiveWindow(root.hwnd);
    SetFocus(root.hwnd);
}

void PinContextMenu::openChildPlane(int childLevel,
                                    int parentItemIndex,
                                    const std::vector<PinMenuItem>& submenuItems) {
    if (childLevel <= 0 || childLevel >= PinContextMenuMetrics::kMaxCascadeDepth) return;
    closePlanesFrom(childLevel);

    auto& parent = m_planes[static_cast<size_t>(childLevel - 1)];
    if (!parent.visible || parentItemIndex < 0 ||
        static_cast<size_t>(parentItemIndex) >= parent.layout.itemRects.size()) {
        return;
    }
    if (!ensurePlaneWindow(childLevel, m_planes[0].hwnd)) return;

    parent.openSubmenuParentIndex = parentItemIndex;
    renderPlane(childLevel - 1);

    auto& child = m_planes[static_cast<size_t>(childLevel)];
    child.hasHeaderBar = false;
    child.hasOpacitySlider = false;
    child.items = &submenuItems;
    child.hoveredHeaderIndex = -1;
    child.pressedHeaderIndex = -1;
    child.hoveredItemIndex = -1;
    child.pressedItemIndex = -1;
    child.openSubmenuParentIndex = -1;
    child.scrollOffsetY = 0.0f;

    const float maxWorkH = static_cast<float>((std::max)(200L, m_workArea.bottom - m_workArea.top));
    child.layout = PinContextMenuLayoutEngine::computePanelLayout(
        false, nullptr, false, {},
        submenuItems, m_dpiScale, maxWorkH,
        [this](const std::wstring& txt, float px) { return measureTextWidth(txt, px); });

    const RECT parentPanelScreen{
        parent.windowPos.x + static_cast<LONG>(std::lround(parent.layout.panelRect.left)),
        parent.windowPos.y + static_cast<LONG>(std::lround(parent.layout.panelRect.top)),
        parent.windowPos.x + static_cast<LONG>(std::lround(parent.layout.panelRect.right)),
        parent.windowPos.y + static_cast<LONG>(std::lround(parent.layout.panelRect.bottom)),
    };
    const float anchorTopScreenY =
        static_cast<float>(parent.windowPos.y) +
        parent.layout.itemRects[static_cast<size_t>(parentItemIndex)].top -
        parent.scrollOffsetY;

    const float shadowPx = std::ceil(PinContextMenuMetrics::kShadowMarginDip * m_dpiScale);
    const float overlapPx = std::round(PinContextMenuMetrics::kSubmenuOverlapDip * m_dpiScale);
    child.windowPos = PinContextMenuLayoutEngine::placeSubmenuWindow(
        parentPanelScreen, anchorTopScreenY, child.layout.windowSize,
        m_workArea, shadowPx, overlapPx);
    child.visible = true;

    renderPlane(childLevel);
    SetWindowPos(child.hwnd, HWND_TOPMOST,
                 child.windowPos.x, child.windowPos.y,
                 child.layout.windowSize.cx, child.layout.windowSize.cy,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void PinContextMenu::closePlanesFrom(int startLevel) noexcept {
    const int safeStart = (std::max)(0, startLevel);
    for (int lv = PinContextMenuMetrics::kMaxCascadeDepth - 1; lv >= safeStart; --lv) {
        auto& p = m_planes[static_cast<size_t>(lv)];
        if (p.hwnd && IsWindow(p.hwnd)) {
            KillTimer(p.hwnd, PinContextMenuMetrics::kSubmenuHoverTimerId);
            if (p.visible) {
                ShowWindow(p.hwnd, SW_HIDE);
            }
        }
        p.visible = false;
        p.items = nullptr;
        p.hoveredHeaderIndex = -1;
        p.pressedHeaderIndex = -1;
        p.hoveredItemIndex = -1;
        p.pressedItemIndex = -1;
        p.openSubmenuParentIndex = -1;
        p.scrollOffsetY = 0.0f;
    }
    if (safeStart > 0) {
        auto& parent = m_planes[static_cast<size_t>(safeStart - 1)];
        if (parent.openSubmenuParentIndex != -1) {
            parent.openSubmenuParentIndex = -1;
            if (parent.visible) {
                renderPlane(safeStart - 1);
            }
        }
    }
}

void PinContextMenu::hideAllPlanes() noexcept {
    closePlanesFrom(0);
    for (auto& p : m_planes) {
        p.releaseSurface();
    }
}

void PinContextMenu::drawPlaneContents(ID2D1RenderTarget* rt,
                                       const PopupPlane& plane,
                                       bool isDark,
                                       float dpiScale) {
    if (!rt) return;
    const float s = std::clamp(dpiScale > 0.0f ? dpiScale : 1.0f, 1.0f, 4.0f);
    const auto pal = FloatingGlassBar::resolveThemePalette(isDark);

    // 1. 微晶玻璃浮岛底板（含多层柔和阴影 + 1px 顶部微晶高光边）
    FloatingGlassBar::drawGlassPanel(
        rt, plane.layout.panelRect, PinContextMenuMetrics::kPanelCornerRadiusDip * s, pal.isDark);

    ComPtr<ID2D1SolidColorBrush> textPrimaryBr, textSecondaryBr, textMutedBr;
    ComPtr<ID2D1SolidColorBrush> accentBr, accentSoftBr, accentBorderBr, accentTextBr;
    ComPtr<ID2D1SolidColorBrush> hoverBr, dividerBr, dangerBgBr, dangerTextBr, whiteBr;

    rt->CreateSolidColorBrush(pal.textPrimary, textPrimaryBr.GetAddressOf());
    rt->CreateSolidColorBrush(pal.textSecondary, textSecondaryBr.GetAddressOf());
    rt->CreateSolidColorBrush(pal.textMuted, textMutedBr.GetAddressOf());
    rt->CreateSolidColorBrush(pal.accent, accentBr.GetAddressOf());
    rt->CreateSolidColorBrush(pal.accentSoftBg, accentSoftBr.GetAddressOf());
    rt->CreateSolidColorBrush(pal.accentBorder, accentBorderBr.GetAddressOf());
    rt->CreateSolidColorBrush(pal.accentText, accentTextBr.GetAddressOf());
    rt->CreateSolidColorBrush(pal.hoverBg, hoverBr.GetAddressOf());
    rt->CreateSolidColorBrush(pal.divider, dividerBr.GetAddressOf());
    rt->CreateSolidColorBrush(pal.dangerSoftBg, dangerBgBr.GetAddressOf());
    rt->CreateSolidColorBrush(pal.dangerText, dangerTextBr.GetAddressOf());
    rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), whiteBr.GetAddressOf());

    auto drawHorizontalRule = [&](float yPos) {
        if (!dividerBr) return;
        const float lx = plane.layout.panelRect.left + 12.0f * s;
        const float rx = plane.layout.panelRect.right - 12.0f * s;
        const float alignedY = std::floor(yPos) + 0.5f;
        rt->DrawLine(D2D1::Point2F(lx, alignedY), D2D1::Point2F(rx, alignedY), dividerBr.Get(), 1.0f);
    };

    // 2. 顶部 6 枚高频图文快捷栏
    if (plane.hasHeaderBar && m_activeRootData) {
        for (int i = 0; i < PinContextMenuMetrics::kHeaderActionCount; ++i) {
            const auto& btn = m_activeRootData->headerButtons[static_cast<size_t>(i)];
            const D2D1_RECT_F rc = plane.layout.headerButtonRects[static_cast<size_t>(i)];
            const bool hovered = (plane.hoveredHeaderIndex == i);
            const bool active = btn.isActive;

            if (hovered || active) {
                FloatingGlassBar::drawButtonPill(
                    rt, rc, PinContextMenuMetrics::kHeaderBtnRadiusDip * s,
                    hovered, active, false, false, 1.0f, pal.isDark);
            }

            ID2D1Brush* fgBrush = active
                ? (accentTextBr ? accentTextBr.Get() : textPrimaryBr.Get())
                : textPrimaryBr.Get();

            const float iconSz = PinContextMenuMetrics::kHeaderIconSizeDip * s;
            const float cx = (rc.left + rc.right) * 0.5f;
            const float iconTop = rc.top + 4.0f * s;
            const D2D1_RECT_F iconRc = D2D1::RectF(
                cx - iconSz * 0.5f, iconTop, cx + iconSz * 0.5f, iconTop + iconSz);
            CaptureVectorIcons::renderIcon(rt, m_d2dFactory.Get(), btn.icon, iconRc, fgBrush, s);

            if (m_headerLabelFmt && fgBrush) {
                const D2D1_RECT_F lblRc = D2D1::RectF(
                    rc.left + 1.0f, iconRc.bottom, rc.right - 1.0f, rc.bottom - 3.5f * s);
                rt->DrawTextW(btn.shortLabel.c_str(), static_cast<UINT32>(btn.shortLabel.size()),
                              m_headerLabelFmt.Get(), lblRc, fgBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        const float sepY = plane.layout.headerButtonRects[0].bottom +
                           PinContextMenuMetrics::kSeparatorHeightDip * s * 0.5f;
        drawHorizontalRule(sepY);
    }

    // 3. 连续无级不透明度滑块行
    if (plane.hasOpacitySlider && m_activeRootData) {
        if (m_itemLabelFmt && textSecondaryBr) {
            rt->DrawTextW(m_activeRootData->opacityLabel.c_str(),
                          static_cast<UINT32>(m_activeRootData->opacityLabel.size()),
                          m_itemLabelFmt.Get(), plane.layout.sliderLabelRect,
                          textSecondaryBr.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        const D2D1_RECT_F tr = plane.layout.sliderTrackRect;
        const float trRadius = (tr.bottom - tr.top) * 0.5f;
        ComPtr<ID2D1SolidColorBrush> trackBgBr;
        rt->CreateSolidColorBrush(
            pal.isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f)
                       : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.12f),
            trackBgBr.GetAddressOf());
        if (trackBgBr) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(tr, trRadius, trRadius), trackBgBr.Get());
        }

        const float thumbX = PinContextMenuLayoutEngine::sliderXFromOpacity(m_currentOpacity, tr);
        const float midY = (tr.top + tr.bottom) * 0.5f;
        if (accentBr && thumbX > tr.left + 0.5f) {
            const D2D1_RECT_F fillRc = D2D1::RectF(tr.left, tr.top, thumbX, tr.bottom);
            rt->FillRoundedRectangle(D2D1::RoundedRect(fillRc, trRadius, trRadius), accentBr.Get());
        }

        const float thumbR = ((plane.hoverSliderThumb || m_draggingSlider)
            ? PinContextMenuMetrics::kSliderThumbActiveRadiusDip
            : PinContextMenuMetrics::kSliderThumbRadiusDip) * s;
        if (whiteBr) {
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, midY), thumbR, thumbR), whiteBr.Get());
        }
        if (accentBr) {
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, midY), thumbR - 0.8f * s, thumbR - 0.8f * s),
                            accentBr.Get(), 2.0f * s);
        }

        const int pctInt = static_cast<int>(std::lround(m_currentOpacity * 100.0f));
        const std::wstring pctStr = std::format(L"{}%", pctInt);
        const D2D1_RECT_F pctRc = plane.layout.sliderPctPillRect;
        if (plane.hoverSliderPctPill || pctInt != 100) {
            FloatingGlassBar::drawButtonPill(
                rt, pctRc, 5.0f * s, plane.hoverSliderPctPill, pctInt != 100,
                false, false, 0.92f, pal.isDark);
        }
        if (m_badgeFmt) {
            ID2D1Brush* pctBr = (pctInt != 100 && accentTextBr) ? accentTextBr.Get() : textSecondaryBr.Get();
            if (pctBr) {
                rt->DrawTextW(pctStr.c_str(), static_cast<UINT32>(pctStr.size()),
                              m_badgeFmt.Get(), pctRc, pctBr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        const float sepY = plane.layout.sliderRowRect.bottom +
                           PinContextMenuMetrics::kSeparatorHeightDip * s * 0.5f;
        drawHorizontalRule(sepY);
    }

    // 4. 列表区（带裁剪视口与平滑滚动支持）
    if (plane.items && !plane.items->empty()) {
        rt->PushAxisAlignedClip(plane.layout.itemsViewportRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        for (size_t i = 0; i < plane.items->size() && i < plane.layout.itemRects.size(); ++i) {
            const auto& item = (*plane.items)[i];
            D2D1_RECT_F rowRc = plane.layout.itemRects[i];
            rowRc.top -= plane.scrollOffsetY;
            rowRc.bottom -= plane.scrollOffsetY;

            if (rowRc.bottom < plane.layout.itemsViewportRect.top ||
                rowRc.top > plane.layout.itemsViewportRect.bottom) {
                continue;
            }

            if (item.isSeparator) {
                drawHorizontalRule((rowRc.top + rowRc.bottom) * 0.5f);
                continue;
            }

            const bool isHovered = (static_cast<int>(i) == plane.hoveredItemIndex) ||
                                   (static_cast<int>(i) == plane.openSubmenuParentIndex);
            const bool isDangerHover = isHovered && item.isDanger;

            if (isHovered) {
                FloatingGlassBar::drawButtonPill(
                    rt, rowRc, PinContextMenuMetrics::kItemCornerRadiusDip * s,
                    true, false, item.isDanger, false, 1.0f, pal.isDark);
            }

            // 左侧槽位：彩色分组圆点 / 选中态微晶勾选徽章 / 矢量功能图标
            const float leadCenterX = rowRc.left + PinContextMenuMetrics::kItemIconLeadWidthDip * s * 0.5f + 2.0f * s;
            const float rowCenterY = (rowRc.top + rowRc.bottom) * 0.5f;

            if (item.hasColorDot) {
                ComPtr<ID2D1SolidColorBrush> dotBr;
                D2D1_COLOR_F dc = item.dotColor;
                dc.a = 0.96f;
                rt->CreateSolidColorBrush(dc, dotBr.GetAddressOf());
                if (item.isChecked && dotBr) {
                    D2D1_COLOR_F halo = dc;
                    halo.a = pal.isDark ? 0.30f : 0.22f;
                    ComPtr<ID2D1SolidColorBrush> haloBr;
                    rt->CreateSolidColorBrush(halo, haloBr.GetAddressOf());
                    if (haloBr) {
                        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(leadCenterX, rowCenterY), 7.2f * s, 7.2f * s), haloBr.Get());
                    }
                    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(leadCenterX, rowCenterY), 6.2f * s, 6.2f * s), dotBr.Get(), 1.3f * s);
                }
                if (dotBr) {
                    const float dotR = item.isChecked ? (3.8f * s) : (4.3f * s);
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(leadCenterX, rowCenterY), dotR, dotR), dotBr.Get());
                }
            } else if (item.isChecked) {
                const float chkBoxHalf = 7.8f * s;
                const D2D1_RECT_F chkBox = D2D1::RectF(
                    leadCenterX - chkBoxHalf, rowCenterY - chkBoxHalf,
                    leadCenterX + chkBoxHalf, rowCenterY + chkBoxHalf);
                if (accentSoftBr) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(chkBox, 4.2f * s, 4.2f * s), accentSoftBr.Get());
                }
                if (accentBorderBr) {
                    rt->DrawRoundedRectangle(D2D1::RoundedRect(chkBox, 4.2f * s, 4.2f * s), accentBorderBr.Get(), 1.0f);
                }
                ID2D1Brush* chkFg = accentTextBr ? accentTextBr.Get() : textPrimaryBr.Get();
                const float icHalf = 6.2f * s;
                const D2D1_RECT_F icRc = D2D1::RectF(
                    leadCenterX - icHalf, rowCenterY - icHalf,
                    leadCenterX + icHalf, rowCenterY + icHalf);
                CaptureVectorIcons::renderIcon(rt, m_d2dFactory.Get(), CaptureIconId::ActionConfirm, icRc, chkFg, s * 0.78f);
            } else if (item.icon != CaptureIconId::None) {
                const float icHalf = PinContextMenuMetrics::kItemIconSizeDip * s * 0.5f;
                const D2D1_RECT_F icRc = D2D1::RectF(
                    leadCenterX - icHalf, rowCenterY - icHalf,
                    leadCenterX + icHalf, rowCenterY + icHalf);
                ID2D1Brush* icBr = isDangerHover
                    ? (dangerTextBr ? dangerTextBr.Get() : textPrimaryBr.Get())
                    : (isHovered ? textPrimaryBr.Get() : textSecondaryBr.Get());
                CaptureVectorIcons::renderIcon(rt, m_d2dFactory.Get(), item.icon, icRc, icBr, s * 0.94f);
            }

            // 右侧尾部：子菜单箭头 + 单一主快捷键对齐文本
            float rightCursorX = rowRc.right - PinContextMenuMetrics::kItemRightPadDip * s;
            if (item.hasSubmenu()) {
                const float chevSz = 16.0f * s;
                const D2D1_RECT_F chevRc = D2D1::RectF(
                    rightCursorX - chevSz + 3.0f * s, rowCenterY - chevSz * 0.5f,
                    rightCursorX + 3.0f * s, rowCenterY + chevSz * 0.5f);
                ID2D1Brush* chevBr = isHovered ? textPrimaryBr.Get() : textMutedBr.Get();
                CaptureVectorIcons::renderIcon(rt, m_d2dFactory.Get(), CaptureIconId::UiChevronRight, chevRc, chevBr, s * 0.84f);
                rightCursorX -= PinContextMenuMetrics::kItemChevronWidthDip * s;
            }

            if (!item.shortcutText.empty() && m_shortcutFmt) {
                const float scW = measureTextWidth(
                    item.shortcutText,
                    (std::max)(PinContextMenuMetrics::kMinUiFontPx, PinContextMenuMetrics::kShortcutFontDip * s));
                const D2D1_RECT_F scRc = D2D1::RectF(
                    (std::max)(rowRc.left + 40.0f * s, rightCursorX - scW - 4.0f * s),
                    rowRc.top, rightCursorX, rowRc.bottom);
                ID2D1Brush* scBr = isDangerHover ? dangerTextBr.Get() : textMutedBr.Get();
                if (scBr) {
                    rt->DrawTextW(item.shortcutText.c_str(), static_cast<UINT32>(item.shortcutText.size()),
                                  m_shortcutFmt.Get(), scRc, scBr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                rightCursorX = scRc.left - 8.0f * s;
            }

            // 主标题与内联微晶状态徽章（如 125% / 反色 / 4 张）
            const float labelLeft = rowRc.left + PinContextMenuMetrics::kItemIconLeadWidthDip * s + 4.0f * s;
            const float labelMaxRight = (std::max)(labelLeft + 24.0f * s, rightCursorX);
            ID2D1Brush* lblBr = isDangerHover
                ? (dangerTextBr ? dangerTextBr.Get() : textPrimaryBr.Get())
                : (item.isChecked && accentTextBr ? accentTextBr.Get() : textPrimaryBr.Get());
            IDWriteTextFormat* lblFmt = (item.isChecked && m_itemLabelBoldFmt) ? m_itemLabelBoldFmt.Get() : m_itemLabelFmt.Get();

            float badgeWidthTotal = 0.0f;
            float measuredBadgeTextW = 0.0f;
            if (!item.statusBadge.empty()) {
                measuredBadgeTextW = measureTextWidth(
                    item.statusBadge,
                    (std::max)(PinContextMenuMetrics::kMinUiFontPx, PinContextMenuMetrics::kBadgeFontDip * s));
                badgeWidthTotal = measuredBadgeTextW + PinContextMenuMetrics::kStatusBadgePadXDip * 2.0f * s + 6.0f * s;
            }

            const D2D1_RECT_F lblRc = D2D1::RectF(
                labelLeft, rowRc.top,
                (std::max)(labelLeft + 16.0f * s, labelMaxRight - badgeWidthTotal),
                rowRc.bottom);
            if (lblFmt && lblBr) {
                rt->DrawTextW(item.label.c_str(), static_cast<UINT32>(item.label.size()),
                              lblFmt, lblRc, lblBr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }

            if (!item.statusBadge.empty() && m_badgeFmt) {
                const float rawLblW = measureTextWidth(
                    item.label,
                    (std::max)(PinContextMenuMetrics::kMinUiFontPx, PinContextMenuMetrics::kItemLabelFontDip * s));
                const float badgeLeft = (std::min)(
                    labelLeft + rawLblW + 6.0f * s,
                    labelMaxRight - (measuredBadgeTextW + PinContextMenuMetrics::kStatusBadgePadXDip * 2.0f * s));
                const float badgeRight = badgeLeft + measuredBadgeTextW + PinContextMenuMetrics::kStatusBadgePadXDip * 2.0f * s;
                const float badgeHalfH = PinContextMenuMetrics::kStatusBadgeHeightDip * s * 0.5f;
                if (badgeRight <= labelMaxRight + 2.0f * s && badgeLeft >= labelLeft + 12.0f * s) {
                    const D2D1_RECT_F bRc = D2D1::RectF(
                        badgeLeft, rowCenterY - badgeHalfH, badgeRight, rowCenterY + badgeHalfH);
                    if (accentSoftBr) {
                        rt->FillRoundedRectangle(D2D1::RoundedRect(bRc, badgeHalfH * 0.65f, badgeHalfH * 0.65f), accentSoftBr.Get());
                    }
                    ID2D1Brush* bTxtBr = accentTextBr ? accentTextBr.Get() : textSecondaryBr.Get();
                    if (bTxtBr) {
                        rt->DrawTextW(item.statusBadge.c_str(), static_cast<UINT32>(item.statusBadge.size()),
                                      m_badgeFmt.Get(), bRc, bTxtBr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
            }
        }

        // 超长子菜单微晶滚动指示条
        if (plane.layout.maxScrollOffsetY > 0.5f && textMutedBr) {
            const float vpTop = plane.layout.itemsViewportRect.top + 2.0f * s;
            const float vpBot = plane.layout.itemsViewportRect.bottom - 2.0f * s;
            const float vpH = (std::max)(12.0f * s, vpBot - vpTop);
            const float thumbRatio = std::clamp(vpH / (std::max)(1.0f, plane.layout.totalItemsContentHeight), 0.15f, 0.90f);
            const float sbH = (std::max)(16.0f * s, vpH * thumbRatio);
            const float scrollT = std::clamp(plane.scrollOffsetY / plane.layout.maxScrollOffsetY, 0.0f, 1.0f);
            const float sbTop = vpTop + (vpH - sbH) * scrollT;
            const D2D1_RECT_F sbRc = D2D1::RectF(
                plane.layout.panelRect.right - 4.5f * s, sbTop,
                plane.layout.panelRect.right - 2.0f * s, sbTop + sbH);
            rt->FillRoundedRectangle(D2D1::RoundedRect(sbRc, 1.25f * s, 1.25f * s), textMutedBr.Get());
        }

        rt->PopAxisAlignedClip();
    }
}

void PinContextMenu::renderPlane(int level) {
    if (level < 0 || level >= PinContextMenuMetrics::kMaxCascadeDepth) return;
    auto& plane = m_planes[static_cast<size_t>(level)];
    if (!plane.hwnd || !IsWindow(plane.hwnd)) return;

    const int w = (std::max)(1, static_cast<int>(plane.layout.windowSize.cx));
    const int h = (std::max)(1, static_cast<int>(plane.layout.windowSize.cy));
    if (!ensureGraphicsResources(m_dpiScale, m_activeSnapshot.isChinese)) return;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return;

    if (!plane.memDc || !plane.dib || plane.dibW != w || plane.dibH != h) {
        plane.releaseSurface();
        plane.memDc = CreateCompatibleDC(screenDc);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        plane.dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!plane.memDc || !plane.dib || !bits) {
            plane.releaseSurface();
            ReleaseDC(nullptr, screenDc);
            return;
        }
        plane.oldBmp = SelectObject(plane.memDc, plane.dib);
        plane.dibW = w;
        plane.dibH = h;
    }

    RECT bindRc{0, 0, w, h};
    if (SUCCEEDED(m_dcTarget->BindDC(plane.memDc, &bindRc))) {
        m_dcTarget->BeginDraw();
        m_dcTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        drawPlaneContents(m_dcTarget.Get(), plane, FloatingGlassBar::isEffectiveDarkMode(), m_dpiScale);
        const HRESULT hrEnd = m_dcTarget->EndDraw();
        if (hrEnd == D2DERR_RECREATE_TARGET) {
            m_dcTarget.Reset();
        }
    }

    POINT ptDst = plane.windowPos;
    SIZE szWnd{w, h};
    POINT ptSrc{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(plane.hwnd, screenDc, &ptDst, &szWnd, plane.memDc, &ptSrc, 0, &blend, ULW_ALPHA);

    ReleaseDC(nullptr, screenDc);
}

cv::Mat PinContextMenu::renderPreviewBitmap(const PinContextMenuData& menuData,
                                            float dpiScale,
                                            int hoveredHeaderBtn,
                                            int hoveredItemIdx,
                                            bool forceDarkTheme) {
    const float safeScale = std::clamp(dpiScale > 0.0f ? dpiScale : 1.0f, 1.0f, 4.0f);
    if (!ensureGraphicsResources(safeScale, true)) return {};

    const PinContextMenuData* prevRoot = m_activeRootData;
    const float prevOp = m_currentOpacity;
    m_activeRootData = &menuData;
    m_currentOpacity = menuData.initialOpacity;

    PopupPlane previewPlane{};
    previewPlane.level = 0;
    previewPlane.hasHeaderBar = menuData.hasHeaderBar;
    previewPlane.hasOpacitySlider = menuData.hasOpacitySlider;
    previewPlane.items = &menuData.items;
    previewPlane.hoveredHeaderIndex = hoveredHeaderBtn;
    previewPlane.hoveredItemIndex = hoveredItemIdx;
    previewPlane.layout = PinContextMenuLayoutEngine::computePanelLayout(
        menuData.hasHeaderBar,
        menuData.hasHeaderBar ? &menuData.headerButtons : nullptr,
        menuData.hasOpacitySlider,
        menuData.opacityLabel,
        menuData.items,
        safeScale,
        1400.0f * safeScale,
        [this](const std::wstring& txt, float px) { return measureTextWidth(txt, px); });

    const int w = (std::max)(1, static_cast<int>(previewPlane.layout.windowSize.cx));
    const int h = (std::max)(1, static_cast<int>(previewPlane.layout.windowSize.cy));

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        m_activeRootData = prevRoot;
        m_currentOpacity = prevOp;
        return {};
    }
    HDC memDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);

    cv::Mat out;
    if (memDc && dib && bits) {
        HGDIOBJ old = SelectObject(memDc, dib);
        RECT bindRc{0, 0, w, h};
        if (SUCCEEDED(m_dcTarget->BindDC(memDc, &bindRc))) {
            m_dcTarget->BeginDraw();
            m_dcTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
            drawPlaneContents(m_dcTarget.Get(), previewPlane, forceDarkTheme, safeScale);
            m_dcTarget->EndDraw();
            GdiFlush();
            cv::Mat wrapped(h, w, CV_8UC4, bits);
            out = wrapped.clone();
        }
        SelectObject(memDc, old);
    }
    if (dib) DeleteObject(dib);
    if (memDc) DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);

    m_activeRootData = prevRoot;
    m_currentOpacity = prevOp;
    return out;
}

bool PinContextMenu::isMenuFamilyHwnd(HWND hwnd) const noexcept {
    if (!hwnd) return false;
    for (const auto& p : m_planes) {
        if (p.visible && p.hwnd == hwnd) return true;
    }
    return false;
}

int PinContextMenu::findPlaneLevel(HWND hwnd) const noexcept {
    if (!hwnd) return -1;
    for (int i = 0; i < PinContextMenuMetrics::kMaxCascadeDepth; ++i) {
        if (m_planes[static_cast<size_t>(i)].hwnd == hwnd) return i;
    }
    return -1;
}

int PinContextMenu::deepestVisibleLevel() const noexcept {
    for (int i = PinContextMenuMetrics::kMaxCascadeDepth - 1; i >= 0; --i) {
        if (m_planes[static_cast<size_t>(i)].visible) return i;
    }
    return 0;
}

int PinContextMenu::hitTestHeaderButton(const PopupPlane& plane, float x, float y) const noexcept {
    if (!plane.hasHeaderBar) return -1;
    for (int i = 0; i < PinContextMenuMetrics::kHeaderActionCount; ++i) {
        if (ptInRectF(plane.layout.headerButtonRects[static_cast<size_t>(i)], x, y)) {
            return i;
        }
    }
    return -1;
}

bool PinContextMenu::hitTestSliderTrackArea(const PopupPlane& plane, float x, float y) const noexcept {
    if (!plane.hasOpacitySlider) return false;
    const float pad = 9.0f * m_dpiScale;
    const D2D1_RECT_F hitRc = D2D1::RectF(
        plane.layout.sliderTrackRect.left - pad,
        plane.layout.sliderRowRect.top,
        plane.layout.sliderTrackRect.right + pad,
        plane.layout.sliderRowRect.bottom);
    return ptInRectF(hitRc, x, y);
}

bool PinContextMenu::hitTestSliderPctPill(const PopupPlane& plane, float x, float y) const noexcept {
    if (!plane.hasOpacitySlider) return false;
    return ptInRectF(plane.layout.sliderPctPillRect, x, y);
}

int PinContextMenu::hitTestMenuItem(const PopupPlane& plane, float x, float y) const noexcept {
    if (!plane.items || !ptInRectF(plane.layout.itemsViewportRect, x, y)) return -1;
    const float contentY = y + plane.scrollOffsetY;
    for (size_t i = 0; i < plane.items->size() && i < plane.layout.itemRects.size(); ++i) {
        if (!(*plane.items)[i].isSelectable()) continue;
        const auto& r = plane.layout.itemRects[i];
        if (x >= r.left && x <= r.right && contentY >= r.top && contentY <= r.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void PinContextMenu::ensureItemVisibleInScroll(PopupPlane& plane, int itemIndex) noexcept {
    if (plane.layout.maxScrollOffsetY <= 0.5f || itemIndex < 0 ||
        static_cast<size_t>(itemIndex) >= plane.layout.itemRects.size()) {
        return;
    }
    const auto& r = plane.layout.itemRects[static_cast<size_t>(itemIndex)];
    const float vpTop = plane.layout.itemsViewportRect.top;
    const float vpBot = plane.layout.itemsViewportRect.bottom;
    if (r.top - plane.scrollOffsetY < vpTop) {
        plane.scrollOffsetY = std::clamp(r.top - vpTop, 0.0f, plane.layout.maxScrollOffsetY);
    } else if (r.bottom - plane.scrollOffsetY > vpBot) {
        plane.scrollOffsetY = std::clamp(r.bottom - vpBot, 0.0f, plane.layout.maxScrollOffsetY);
    }
}

void PinContextMenu::applyLiveOpacity(float newOpacity) {
    const float clamped = std::clamp(
        newOpacity, PinInteractionConstants::kMinPinOpacity, PinInteractionConstants::kMaxPinOpacity);
    if (std::abs(clamped - m_currentOpacity) <= 0.002f) return;
    m_currentOpacity = clamped;
    renderPlane(0);
    if (m_onLiveOpacityChange) {
        m_onLiveOpacityChange(clamped);
    }
}

LRESULT PinContextMenu::handlePlaneMessage(int level, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& plane = m_planes[static_cast<size_t>(level)];

    switch (msg) {
        case WM_MOUSEACTIVATE:
            return (level == 0) ? MA_ACTIVATE : MA_NOACTIVATE;

        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_KEYMENU || (wParam & 0xFFF0) == SC_CONTEXTHELP) {
                return 0;
            }
            break;

        case WM_SETCURSOR: {
            if (m_draggingSlider || plane.hoverSliderThumb || plane.hoverSliderPctPill ||
                plane.hoveredHeaderIndex >= 0 || plane.hoveredItemIndex >= 0) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }

        case WM_ACTIVATEAPP: {
            if (wParam == FALSE && m_running) {
                dismiss();
            }
            return 0;
        }

        case WM_KILLFOCUS: {
            if (level == 0 && m_running) {
                HWND nextFocus = reinterpret_cast<HWND>(wParam);
                if (!isMenuFamilyHwnd(nextFocus)) {
                    dismiss();
                }
            }
            return 0;
        }

        case WM_CANCELMODE: {
            if (m_draggingSlider) {
                m_draggingSlider = false;
                ReleaseCapture();
            }
            if (m_running) dismiss();
            return 0;
        }

        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, HOVER_DEFAULT};
            TrackMouseEvent(&tme);

            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));

            if (level == 0 && m_draggingSlider) {
                applyLiveOpacity(PinContextMenuLayoutEngine::opacityFromSliderX(mx, plane.layout.sliderTrackRect));
                return 0;
            }

            // 鼠标已进入子菜单：取消父菜单挂起的子菜单切换定时器，防止斜向划过时误关当前子菜单
            if (level > 0) {
                auto& parent = m_planes[static_cast<size_t>(level - 1)];
                if (parent.hwnd) {
                    KillTimer(parent.hwnd, PinContextMenuMetrics::kSubmenuHoverTimerId);
                }
                if (parent.openSubmenuParentIndex >= 0 &&
                    parent.hoveredItemIndex != parent.openSubmenuParentIndex) {
                    parent.hoveredItemIndex = parent.openSubmenuParentIndex;
                    renderPlane(level - 1);
                }
            }

            const int hitHdr = hitTestHeaderButton(plane, mx, my);
            const bool hitPct = hitTestSliderPctPill(plane, mx, my);
            const bool hitTrk = !hitPct && hitTestSliderTrackArea(plane, mx, my);
            const int hitItem = (hitHdr < 0 && !hitPct && !hitTrk) ? hitTestMenuItem(plane, mx, my) : -1;

            const bool visualChanged =
                (hitHdr != plane.hoveredHeaderIndex) ||
                (hitPct != plane.hoverSliderPctPill) ||
                (hitTrk != plane.hoverSliderThumb) ||
                (hitItem != plane.hoveredItemIndex);

            plane.hoveredHeaderIndex = hitHdr;
            plane.hoverSliderPctPill = hitPct;
            plane.hoverSliderThumb = hitTrk;
            plane.hoveredItemIndex = hitItem;

            if (visualChanged) {
                renderPlane(level);
            }

            // 处理级联子菜单展开/收起（带 140ms 斜向移动宽限）
            if (plane.items && level + 1 < PinContextMenuMetrics::kMaxCascadeDepth) {
                if (plane.openSubmenuParentIndex < 0) {
                    if (hitItem >= 0 && (*plane.items)[static_cast<size_t>(hitItem)].hasSubmenu()) {
                        openChildPlane(level + 1, hitItem, (*plane.items)[static_cast<size_t>(hitItem)].submenu);
                    }
                } else if (hitItem != plane.openSubmenuParentIndex) {
                    m_pendingHoverLevel = level;
                    m_pendingHoverItemIndex = hitItem;
                    SetTimer(hwnd, PinContextMenuMetrics::kSubmenuHoverTimerId,
                             PinContextMenuMetrics::kSubmenuHoverDelayMs, nullptr);
                } else {
                    KillTimer(hwnd, PinContextMenuMetrics::kSubmenuHoverTimerId);
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (!m_draggingSlider) {
                bool changed = false;
                if (plane.hoveredHeaderIndex != -1) { plane.hoveredHeaderIndex = -1; changed = true; }
                if (plane.hoverSliderThumb)         { plane.hoverSliderThumb = false; changed = true; }
                if (plane.hoverSliderPctPill)       { plane.hoverSliderPctPill = false; changed = true; }
                if (plane.hoveredItemIndex != plane.openSubmenuParentIndex) {
                    plane.hoveredItemIndex = plane.openSubmenuParentIndex;
                    changed = true;
                }
                if (changed && plane.visible) {
                    renderPlane(level);
                }
            }
            return 0;
        }

        case WM_TIMER: {
            if (wParam == PinContextMenuMetrics::kSubmenuHoverTimerId) {
                KillTimer(hwnd, PinContextMenuMetrics::kSubmenuHoverTimerId);
                if (!m_running || m_pendingHoverLevel != level || !plane.visible || !plane.items) {
                    return 0;
                }
                const int targetIdx = m_pendingHoverItemIndex;
                if (targetIdx >= 0 && static_cast<size_t>(targetIdx) < plane.items->size() &&
                    (*plane.items)[static_cast<size_t>(targetIdx)].hasSubmenu()) {
                    openChildPlane(level + 1, targetIdx, (*plane.items)[static_cast<size_t>(targetIdx)].submenu);
                } else {
                    closePlanesFrom(level + 1);
                }
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta == 0) return 0;

            POINT scrPt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            // 将滚轮优先路由给光标当前指向的菜单层
            int targetLv = level;
            for (int lv = PinContextMenuMetrics::kMaxCascadeDepth - 1; lv >= 0; --lv) {
                const auto& p = m_planes[static_cast<size_t>(lv)];
                if (!p.visible) continue;
                RECT wr{p.windowPos.x, p.windowPos.y,
                        p.windowPos.x + p.layout.windowSize.cx,
                        p.windowPos.y + p.layout.windowSize.cy};
                if (PtInRect(&wr, scrPt)) {
                    targetLv = lv;
                    break;
                }
            }
            auto& targetPlane = m_planes[static_cast<size_t>(targetLv)];
            const float localX = static_cast<float>(scrPt.x - targetPlane.windowPos.x);
            const float localY = static_cast<float>(scrPt.y - targetPlane.windowPos.y);

            if (targetPlane.hasOpacitySlider && ptInRectF(targetPlane.layout.sliderRowRect, localX, localY)) {
                const float step = (delta > 0 ? 1.0f : -1.0f) * PinContextMenuMetrics::kSliderWheelStep;
                applyLiveOpacity(m_currentOpacity + step);
                return 0;
            }
            if (targetPlane.layout.maxScrollOffsetY > 0.5f) {
                const float stepPx = (delta > 0 ? -1.0f : 1.0f) * PinContextMenuMetrics::kScrollStepDip * m_dpiScale;
                const float nextScroll = std::clamp(
                    targetPlane.scrollOffsetY + stepPx, 0.0f, targetPlane.layout.maxScrollOffsetY);
                if (std::abs(nextScroll - targetPlane.scrollOffsetY) > 0.2f) {
                    targetPlane.scrollOffsetY = nextScroll;
                    renderPlane(targetLv);
                }
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));
            m_hasPointerDownInside = true;

            if (msg == WM_LBUTTONDOWN && level == 0) {
                if (hitTestSliderPctPill(plane, mx, my)) {
                    applyLiveOpacity(1.0f);
                    return 0;
                }
                if (hitTestSliderTrackArea(plane, mx, my)) {
                    m_draggingSlider = true;
                    SetCapture(hwnd);
                    applyLiveOpacity(PinContextMenuLayoutEngine::opacityFromSliderX(mx, plane.layout.sliderTrackRect));
                    return 0;
                }
            }

            plane.pressedHeaderIndex = hitTestHeaderButton(plane, mx, my);
            plane.pressedItemIndex = (plane.pressedHeaderIndex < 0) ? hitTestMenuItem(plane, mx, my) : -1;
            return 0;
        }

        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));

            if (msg == WM_LBUTTONUP && m_draggingSlider) {
                m_draggingSlider = false;
                ReleaseCapture();
                renderPlane(0);
                return 0;
            }

            if (!m_hasPointerDownInside) {
                return 0;
            }

            const int hitHdr = hitTestHeaderButton(plane, mx, my);
            if (hitHdr >= 0 && hitHdr == plane.pressedHeaderIndex && m_activeRootData) {
                const bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const auto& btn = m_activeRootData->headerButtons[static_cast<size_t>(hitHdr)];
                m_selectedCommand = shiftHeld && btn.shiftCommand.isValid()
                    ? btn.shiftCommand
                    : btn.primaryCommand;
                dismiss();
                return 0;
            }

            const int hitItem = hitTestMenuItem(plane, mx, my);
            if (hitItem >= 0 && hitItem == plane.pressedItemIndex && plane.items) {
                const auto& item = (*plane.items)[static_cast<size_t>(hitItem)];
                if (item.hasSubmenu()) {
                    if (level + 1 < PinContextMenuMetrics::kMaxCascadeDepth) {
                        openChildPlane(level + 1, hitItem, item.submenu);
                    }
                } else if (item.command.isValid()) {
                    m_selectedCommand = item.command;
                    dismiss();
                }
            }
            plane.pressedHeaderIndex = -1;
            plane.pressedItemIndex = -1;
            return 0;
        }

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool alt = (msg == WM_SYSKEYDOWN) || ((GetKeyState(VK_MENU) & 0x8000) != 0);

            if (wParam == VK_ESCAPE) {
                dismiss();
                return 0;
            }

            const int activeLv = deepestVisibleLevel();
            auto& activePlane = m_planes[static_cast<size_t>(activeLv)];

            if (wParam == VK_UP || wParam == VK_DOWN) {
                if (activePlane.items) {
                    const int nextIdx = PinContextMenuLayoutEngine::findNextSelectableIndex(
                        *activePlane.items, activePlane.hoveredItemIndex, (wParam == VK_DOWN) ? 1 : -1);
                    if (nextIdx >= 0) {
                        activePlane.hoveredHeaderIndex = -1;
                        activePlane.hoveredItemIndex = nextIdx;
                        ensureItemVisibleInScroll(activePlane, nextIdx);
                        renderPlane(activeLv);
                    }
                }
                return 0;
            }

            if (wParam == VK_RIGHT) {
                if (activePlane.items && activePlane.hoveredItemIndex >= 0 &&
                    static_cast<size_t>(activePlane.hoveredItemIndex) < activePlane.items->size()) {
                    const auto& cur = (*activePlane.items)[static_cast<size_t>(activePlane.hoveredItemIndex)];
                    if (cur.hasSubmenu() && activeLv + 1 < PinContextMenuMetrics::kMaxCascadeDepth) {
                        openChildPlane(activeLv + 1, activePlane.hoveredItemIndex, cur.submenu);
                        auto& sub = m_planes[static_cast<size_t>(activeLv + 1)];
                        if (sub.items) {
                            sub.hoveredItemIndex = PinContextMenuLayoutEngine::findNextSelectableIndex(*sub.items, -1, 1);
                            renderPlane(activeLv + 1);
                        }
                    }
                }
                return 0;
            }

            if (wParam == VK_LEFT) {
                if (activeLv > 0) {
                    closePlanesFrom(activeLv);
                }
                return 0;
            }

            if (wParam == VK_OEM_4 || wParam == VK_OEM_6) { // '[' / ']' 实时微调不透明度滑块
                if (m_planes[0].hasOpacitySlider && !ctrl && !alt) {
                    const float step = (wParam == VK_OEM_6 ? 1.0f : -1.0f) * PinContextMenuMetrics::kSliderWheelStep;
                    applyLiveOpacity(m_currentOpacity + step);
                    return 0;
                }
            }

            if (wParam == VK_RETURN || wParam == VK_SPACE) {
                if (activePlane.items && activePlane.hoveredItemIndex >= 0 &&
                    static_cast<size_t>(activePlane.hoveredItemIndex) < activePlane.items->size()) {
                    const auto& cur = (*activePlane.items)[static_cast<size_t>(activePlane.hoveredItemIndex)];
                    if (cur.hasSubmenu() && activeLv + 1 < PinContextMenuMetrics::kMaxCascadeDepth) {
                        openChildPlane(activeLv + 1, activePlane.hoveredItemIndex, cur.submenu);
                        auto& sub = m_planes[static_cast<size_t>(activeLv + 1)];
                        if (sub.items) {
                            sub.hoveredItemIndex = PinContextMenuLayoutEngine::findNextSelectableIndex(*sub.items, -1, 1);
                            renderPlane(activeLv + 1);
                        }
                    } else if (cur.command.isValid()) {
                        m_selectedCommand = cur.command;
                        dismiss();
                    }
                }
                return 0;
            }

            const PinMenuCommand directCmd =
                PinContextMenuBuilder::resolveDirectKeyShortcut(wParam, ctrl, shift, alt, m_activeSnapshot);
            if (directCmd.isValid()) {
                m_selectedCommand = directCmd;
                dismiss();
                return 0;
            }
            if (alt) return 0;
            break;
        }

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK PinContextMenu::planeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        if (cs && cs->lpCreateParams) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
    }
    auto* self = reinterpret_cast<PinContextMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        const int lv = self->findPlaneLevel(hwnd);
        if (lv >= 0) {
            return self->handlePlaneMessage(lv, hwnd, msg, wParam, lParam);
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

PinMenuCommand PinContextMenu::runModalMenu(HWND ownerHwnd,
                                            POINT anchorScreenPt,
                                            const PinContextMenuData& rootData,
                                            const PinContextMenuSnapshot& snapshot,
                                            std::function<void(float)> onLiveOpacityChange,
                                            std::function<bool()> isOwnerAlive) {
    if (m_running || !ownerHwnd || !IsWindow(ownerHwnd)) {
        return {};
    }

    HMONITOR hMon = MonitorFromPoint(anchorScreenPt, MONITOR_DEFAULTTONEAREST);
    m_workArea = tools3000::core::dpi::workArea(hMon);
    m_dpiScale = std::clamp(tools3000::core::dpi::scaleForMonitor(hMon), 1.0f, 4.0f);
    m_activeRootData = &rootData;
    m_activeSnapshot = snapshot;
    m_currentOpacity = rootData.initialOpacity;
    m_onLiveOpacityChange = std::move(onLiveOpacityChange);
    m_selectedCommand = {};
    m_draggingSlider = false;
    m_hasPointerDownInside = false;
    m_pendingHoverLevel = -1;
    m_pendingHoverItemIndex = -1;

    if (!ensureGraphicsResources(m_dpiScale, snapshot.isChinese) ||
        !ensurePlaneWindow(0, ownerHwnd)) {
        m_activeRootData = nullptr;
        m_onLiveOpacityChange = nullptr;
        return {};
    }

    m_running = true;
    const float maxWorkH = static_cast<float>((std::max)(200L, m_workArea.bottom - m_workArea.top));
    openRootPlane(anchorScreenPt, maxWorkH);

    while (m_running) {
        if (isOwnerAlive && !isOwnerAlive()) {
            m_running = false;
            break;
        }
        MSG msg{};
        const BOOL gm = GetMessageW(&msg, nullptr, 0, 0);
        if (gm <= 0) {
            if (gm == 0) {
                PostQuitMessage(static_cast<int>(msg.wParam));
            }
            m_running = false;
            break;
        }

        if (!m_running) break;

        if ((msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
             msg.message == WM_MBUTTONDOWN || msg.message == WM_NCLBUTTONDOWN ||
             msg.message == WM_NCRBUTTONDOWN || msg.message == WM_NCMBUTTONDOWN) &&
            !m_draggingSlider && !isMenuFamilyHwnd(msg.hwnd)) {
            m_running = false;
            // 左键点击外部安全吞掉以收起菜单（防止误触底下贴图的关闭按钮）；
            // 右键点击外部则收起当前菜单后保留后续 WM_RBUTTONUP 在新坐标无缝重开菜单
            break;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (m_draggingSlider) {
        m_draggingSlider = false;
        ReleaseCapture();
    }
    hideAllPlanes();
    m_activeRootData = nullptr;
    m_onLiveOpacityChange = nullptr;

    if (ownerHwnd && IsWindow(ownerHwnd) && m_selectedCommand.action != PinMenuAction::ClosePin &&
        m_selectedCommand.action != PinMenuAction::CloseAllPins &&
        m_selectedCommand.action != PinMenuAction::GroupCloseCurrent &&
        m_selectedCommand.action != PinMenuAction::GroupRenameCurrent &&
        m_selectedCommand.action != PinMenuAction::EditMarkup) {
        SetFocus(ownerHwnd);
    }

    return m_selectedCommand;
}

} // namespace tools3000::capture
