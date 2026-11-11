// ─────────────────────────────────────────────────────────────────────────────
// PinGroupBarOverlay.cpp — 屏幕顶部雪瓷白微晶贴图分组切换与编组导览栏实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinGroupBarOverlay.h"
#include "capture/PinWindow.h"
#include "capture/CaptureHudPalette.h"
#include "capture/FloatingGlassBar.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"
#include "core/events/EventBus.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <windowsx.h>

namespace tools3000::capture {

using namespace Microsoft::WRL;

static constexpr const wchar_t* PIN_GROUP_BAR_CLASS = L"Tools3000_PinGroupBarOverlay";
static constexpr const wchar_t* PIN_GROUP_EDIT_POPUP_CLASS = L"Tools3000_PinGroupInlineEditPopup";
static constexpr UINT kMsgAsyncCommitInlineEdit = WM_APP + 0x45;

bool PinGroupBarOverlay::s_classRegistered = false;
bool PinGroupBarOverlay::s_editPopupClassRegistered = false;

PinGroupBarOverlay& PinGroupBarOverlay::instance() {
    static PinGroupBarOverlay inst;
    return inst;
}

PinGroupBarOverlay::~PinGroupBarOverlay() {
    shutdown();
}

std::wstring PinGroupBarOverlay::normalizeInlineRenameInput(int groupId, std::wstring_view rawInput) {
    return PinGroupPalette::normalizeInlineRenameInput(groupId, rawInput);
}

void PinGroupBarOverlay::destroyInlineEditorWindow() noexcept {
    if (m_editControlHwnd && IsWindow(m_editControlHwnd) && m_origEditWndProc) {
        SetWindowLongPtrW(m_editControlHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_origEditWndProc));
    }
    m_editControlHwnd = nullptr;
    m_origEditWndProc = nullptr;

    if (m_editPopupHwnd && IsWindow(m_editPopupHwnd)) {
        DestroyWindow(m_editPopupHwnd);
    }
    m_editPopupHwnd = nullptr;

    if (m_editFont) {
        DeleteObject(m_editFont);
        m_editFont = nullptr;
    }
    m_editFontScale = 0.0f;
}

void PinGroupBarOverlay::shutdown() {
    m_closingInlineEdit = true;
    m_editingGroupId = 0;
    m_editingScreenRect = {};
    destroyInlineEditorWindow();
    m_closingInlineEdit = false;

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_dcTarget.Reset();
    m_textFormat.Reset();
    m_chips.clear();
    m_createdGroups.fill(false);
    m_lastGroupClickTick = 0;
    m_lastClickedGroupId = 0;
}

bool PinGroupBarOverlay::ensureWindow() {
    if (m_hwnd && IsWindow(m_hwnd)) {
        return true;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!s_classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_HAND);
        wc.lpszClassName = PIN_GROUP_BAR_CLASS;
        RegisterClassExW(&wc);
        s_classRegistered = true;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        PIN_GROUP_BAR_CLASS,
        L"",
        WS_POPUP,
        0, 0, 10, 10,
        nullptr, nullptr, hInst, this);
    if (!m_hwnd) {
        return false;
    }

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetPropW(m_hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
    return true;
}

bool PinGroupBarOverlay::ensureInlineEditorPopup() {
    if (m_editPopupHwnd && IsWindow(m_editPopupHwnd) &&
        m_editControlHwnd && IsWindow(m_editControlHwnd)) {
        return true;
    }
    destroyInlineEditorWindow();
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        return false;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!s_editPopupClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = editPopupWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        wc.lpszClassName = PIN_GROUP_EDIT_POPUP_CLASS;
        RegisterClassExW(&wc);
        s_editPopupClassRegistered = true;
    }

    m_editPopupHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        PIN_GROUP_EDIT_POPUP_CLASS,
        L"",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 80, 20,
        m_hwnd,
        nullptr,
        hInst,
        this);
    if (!m_editPopupHwnd) {
        return false;
    }
    SetWindowLongPtrW(m_editPopupHwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetPropW(m_editPopupHwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));

    m_editControlHwnd = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
        0, 0, 80, 20,
        m_editPopupHwnd,
        nullptr,
        hInst,
        nullptr);
    if (!m_editControlHwnd) {
        DestroyWindow(m_editPopupHwnd);
        m_editPopupHwnd = nullptr;
        return false;
    }

    SetWindowLongPtrW(m_editControlHwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_origEditWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(m_editControlHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(editControlSubclassProc)));
    SendMessageW(
        m_editControlHwnd,
        EM_SETLIMITTEXT,
        static_cast<WPARAM>(PinGroupConstants::kMaxCustomGroupNameChars),
        0);
    SendMessageW(m_editControlHwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(2, 2));
    return true;
}

void PinGroupBarOverlay::updateInlineEditorFont(float scale) {
    if (!m_editControlHwnd || !IsWindow(m_editControlHwnd)) {
        return;
    }
    const float safeScale = std::clamp(scale > 0.0f ? scale : 1.0f, 1.0f, 4.0f);
    if (m_editFont && std::abs(m_editFontScale - safeScale) <= 0.005f) {
        return;
    }

    ensureResources(safeScale);
    const wchar_t* fontFamily = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
    const int fontPx = static_cast<int>(std::lround((std::max)(12.0f, 12.2f * safeScale)));
    HFONT newFont = CreateFontW(
        -fontPx,
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        fontFamily);
    if (!newFont) {
        return;
    }

    SendMessageW(m_editControlHwnd, WM_SETFONT, reinterpret_cast<WPARAM>(newFont), TRUE);
    if (m_editFont) {
        DeleteObject(m_editFont);
    }
    m_editFont = newFont;
    m_editFontScale = safeScale;
}

void PinGroupBarOverlay::repositionInlineEditor() {
    if (!m_editPopupHwnd || !IsWindow(m_editPopupHwnd) ||
        !m_editControlHwnd || !IsWindow(m_editControlHwnd)) {
        return;
    }
    const int popW = (std::max)(24, static_cast<int>(m_editingScreenRect.right - m_editingScreenRect.left));
    const int popH = (std::max)(16, static_cast<int>(m_editingScreenRect.bottom - m_editingScreenRect.top));

    int editH = popH;
    if (HDC hdc = GetDC(m_editPopupHwnd)) {
        HGDIOBJ oldObj = m_editFont ? SelectObject(hdc, m_editFont) : nullptr;
        TEXTMETRICW tm{};
        if (GetTextMetricsW(hdc, &tm) && tm.tmHeight > 0) {
            editH = (std::min)(popH, static_cast<int>(tm.tmHeight + 2));
        }
        if (oldObj) {
            SelectObject(hdc, oldObj);
        }
        ReleaseDC(m_editPopupHwnd, hdc);
    }
    const int editY = (std::max)(0, (popH - editH) / 2);

    SetWindowPos(
        m_editPopupHwnd,
        HWND_TOPMOST,
        m_editingScreenRect.left,
        m_editingScreenRect.top,
        popW,
        popH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(
        m_editControlHwnd,
        nullptr,
        0,
        editY,
        popW,
        editH,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(m_editControlHwnd, nullptr, TRUE);
}

bool PinGroupBarOverlay::isGroupCurrentlyDisplayed(int groupId) const noexcept {
    if (groupId < PinGroupConstants::kMinGroupId || groupId > PinGroupConstants::kMaxGroupId) {
        return false;
    }
    const size_t idx = static_cast<size_t>(groupId);
    return m_groupCounts[idx] > 0 || m_createdGroups[idx] || m_activeFilter == groupId;
}

bool PinGroupBarOverlay::beginInlineRename(int groupId) {
    const int norm = PinGroupPalette::normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) {
        return false;
    }
    if (!m_hwnd || !IsWindow(m_hwnd) || !isVisible() || !isGroupCurrentlyDisplayed(norm)) {
        return false;
    }

    if (m_editingGroupId != 0 && m_editingGroupId != norm) {
        commitInlineRename(true);
        if (!m_hwnd || !IsWindow(m_hwnd) || !isVisible() || !isGroupCurrentlyDisplayed(norm)) {
            return false;
        }
    }

    if (m_editingGroupId == norm && isInlineEditing()) {
        SetForegroundWindow(m_editPopupHwnd);
        SetFocus(m_editControlHwnd);
        SendMessageW(m_editControlHwnd, EM_SETSEL, 0, -1);
        return true;
    }

    m_editingGroupId = norm;
    rebuildAndRender();

    if (m_editingScreenRect.right <= m_editingScreenRect.left ||
        m_editingScreenRect.bottom <= m_editingScreenRect.top) {
        m_editingGroupId = 0;
        rebuildAndRender();
        return false;
    }

    if (!ensureInlineEditorPopup()) {
        m_editingGroupId = 0;
        rebuildAndRender();
        return false;
    }

    updateInlineEditorFont(m_dpiScale);
    const bool zh = tools3000::core::isUiLanguageChinese();
    std::wstring seedText = PinGroupPalette::customName(norm);
    if (seedText.empty()) {
        seedText = PinGroupPalette::shortLabel(norm, zh);
    }
    SetWindowTextW(m_editControlHwnd, seedText.c_str());
    repositionInlineEditor();

    ShowWindow(m_editPopupHwnd, SW_SHOW);
    SetForegroundWindow(m_editPopupHwnd);
    SetFocus(m_editControlHwnd);
    SendMessageW(m_editControlHwnd, EM_SETSEL, 0, -1);
    return true;
}

void PinGroupBarOverlay::commitInlineRename(bool save) {
    if (m_closingInlineEdit || m_editingGroupId == 0) {
        return;
    }
    m_closingInlineEdit = true;

    const int targetGroup = m_editingGroupId;
    std::wstring normalizedName;
    bool hasCapturedInput = false;
    if (save && m_editControlHwnd && IsWindow(m_editControlHwnd)) {
        const int len = GetWindowTextLengthW(m_editControlHwnd);
        std::wstring rawBuf;
        if (len > 0) {
            rawBuf.resize(static_cast<size_t>(len) + 1, L'\0');
            const int copied = GetWindowTextW(m_editControlHwnd, rawBuf.data(), len + 1);
            rawBuf.resize(static_cast<size_t>((std::max)(0, copied)));
        }
        normalizedName = normalizeInlineRenameInput(targetGroup, rawBuf);
        hasCapturedInput = true;
    }

    m_editingGroupId = 0;
    m_editingScreenRect = {};
    destroyInlineEditorWindow();
    m_closingInlineEdit = false;

    if (save && hasCapturedInput && targetGroup >= PinGroupConstants::kMinGroupId) {
        PinGroupPalette::setGroupName(targetGroup, normalizedName);
        PinWindow::refreshAllVisualsAndGroupBar();
    } else if (m_hwnd && IsWindow(m_hwnd) && isVisible()) {
        rebuildAndRender();
    }
}

bool PinGroupBarOverlay::tryConsumeGroupDoubleClick(POINT screenPt, int hitGroupId) {
    const ULONGLONG nowTick = GetTickCount64();
    if (m_lastClickedGroupId >= PinGroupConstants::kMinGroupId &&
        m_lastClickedGroupId <= PinGroupConstants::kMaxGroupId &&
        m_lastGroupClickTick > 0 &&
        (nowTick - m_lastGroupClickTick) <= static_cast<ULONGLONG>(GetDoubleClickTime())) {
        const int tolX = (std::max)(GetSystemMetrics(SM_CXDOUBLECLK) / 2, 8);
        const int tolY = (std::max)(GetSystemMetrics(SM_CYDOUBLECLK) / 2, 8);
        const int dx = std::abs(screenPt.x - m_lastGroupClickScreenPt.x);
        const int dy = std::abs(screenPt.y - m_lastGroupClickScreenPt.y);
        if (dx <= tolX && dy <= tolY &&
            (hitGroupId <= 0 || hitGroupId == m_lastClickedGroupId || dx <= 6)) {
            const int targetGroup = m_lastClickedGroupId;
            m_lastGroupClickTick = 0;
            m_lastClickedGroupId = 0;
            if (m_activeFilter != targetGroup) {
                PinWindow::setActiveGroupFilter(targetGroup);
            }
            beginInlineRename(targetGroup);
            return true;
        }
    }
    return false;
}

bool PinGroupBarOverlay::ensureResources(float scale) {
    const float safeScale = std::clamp(scale > 0.0f ? scale : 1.0f, 1.0f, 4.0f);
    const bool isZh = tools3000::core::isUiLanguageChinese();

    if (!m_d2dFactory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf()))) {
            return false;
        }
    }
    if (!m_dcTarget) {
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, m_dcTarget.GetAddressOf()))) {
            return false;
        }
    }
    if (!m_dwriteFactory) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
            return false;
        }
    }

    if (!m_textFormat || std::abs(m_cachedFontScale - safeScale) > 0.005f || m_cachedChinese != isZh) {
        m_textFormat.Reset();
        const wchar_t* fontFamily = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
        const float fontPx = (std::max)(12.0f, 12.2f * safeScale);
        if (FAILED(m_dwriteFactory->CreateTextFormat(
                fontFamily, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                fontPx, isZh ? L"zh-CN" : L"en-US", m_textFormat.GetAddressOf()))) {
            return false;
        }
        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_cachedFontScale = safeScale;
        m_cachedChinese = isZh;
    }

    return true;
}

float PinGroupBarOverlay::measureChipText(const std::wstring& text, float scale) {
    if (text.empty()) return 0.0f;
    if (ensureResources(scale) && m_dwriteFactory && m_textFormat) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()),
                m_textFormat.Get(), 1600.0f * scale, 80.0f * scale,
                layout.GetAddressOf())) && layout) {
            DWRITE_TEXT_METRICS m{};
            if (SUCCEEDED(layout->GetMetrics(&m))) {
                return std::ceil(m.widthIncludingTrailingWhitespace);
            }
        }
    }
    return std::ceil(static_cast<float>(text.size()) * 11.5f * scale);
}

void PinGroupBarOverlay::refresh(
    size_t totalCount,
    size_t ungroupedCount,
    const std::array<size_t, PinGroupConstants::kMaxGroupId + 1>& groupCounts,
    const std::array<bool, PinGroupConstants::kMaxGroupId + 1>& createdGroups,
    int activeFilter,
    bool allHidden) {

    m_totalCount = totalCount;
    m_ungroupedCount = ungroupedCount;
    m_groupCounts = groupCounts;
    m_createdGroups = createdGroups;
    m_activeFilter = PinGroupPalette::normalizeFilter(activeFilter);

    bool hasAnyGrouped = false;
    for (int g = PinGroupConstants::kMinGroupId; g <= PinGroupConstants::kMaxGroupId; ++g) {
        if (groupCounts[static_cast<size_t>(g)] > 0 || createdGroups[static_cast<size_t>(g)]) {
            hasAnyGrouped = true;
            break;
        }
    }

    const bool shouldShow = !allHidden && totalCount > 0 &&
                            (hasAnyGrouped || m_activeFilter != PinGroupConstants::kFilterAllGroups);
    if (!shouldShow) {
        if (m_editingGroupId != 0) {
            m_closingInlineEdit = true;
            m_editingGroupId = 0;
            m_editingScreenRect = {};
            destroyInlineEditorWindow();
            m_closingInlineEdit = false;
        }
        if (m_hwnd && IsWindow(m_hwnd)) {
            ShowWindow(m_hwnd, SW_HIDE);
        }
        m_chips.clear();
        m_hoveredChipIndex = -1;
        return;
    }

    if (m_editingGroupId != 0 && !isGroupCurrentlyDisplayed(m_editingGroupId)) {
        m_closingInlineEdit = true;
        m_editingGroupId = 0;
        m_editingScreenRect = {};
        destroyInlineEditorWindow();
        m_closingInlineEdit = false;
    }

    if (!ensureWindow()) {
        return;
    }

    rebuildAndRender();
}

void PinGroupBarOverlay::rebuildAndRender(int retryDepth) {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;

    // 跟随当前活跃贴图或鼠标光标所在显示器工作区顶部居中锚定（告别死锁主屏）
    HMONITOR mon = nullptr;
    HWND fgHwnd = GetForegroundWindow();
    if (auto latestPin = PinWindow::latestInstance();
        latestPin && latestPin->isAlive() && latestPin->hwnd() &&
        (latestPin->isFocused() || latestPin->hwnd() == fgHwnd)) {
        mon = MonitorFromWindow(latestPin->hwnd(), MONITOR_DEFAULTTONEAREST);
    }
    if (!mon) {
        POINT cursorPt{};
        if (GetCursorPos(&cursorPt)) {
            mon = MonitorFromPoint(cursorPt, MONITOR_DEFAULTTONEAREST);
        }
    }
    if (!mon) {
        POINT fallbackPt{GetSystemMetrics(SM_CXSCREEN) / 2, 16};
        mon = MonitorFromPoint(fallbackPt, MONITOR_DEFAULTTOPRIMARY);
    }
    const RECT work = tools3000::core::dpi::workArea(mon);
    const float scale = std::clamp(tools3000::core::dpi::scaleForMonitor(mon), 1.0f, 4.0f);
    m_dpiScale = scale;

    if (!ensureResources(scale)) return;

    const bool zh = tools3000::core::isUiLanguageChinese();
    m_chips.clear();

    // 1. "全部" 胶囊
    {
        BarChip allChip;
        allChip.kind = ChipKind::FilterGroup;
        allChip.groupParam = PinGroupConstants::kFilterAllGroups;
        allChip.label = zh ? std::format(L"全部 {}", m_totalCount) : std::format(L"All {}", m_totalCount);
        allChip.isActive = (m_activeFilter == PinGroupConstants::kFilterAllGroups);
        m_chips.push_back(std::move(allChip));
    }

    // 2. 各活跃分组胶囊 (1..32)
    bool hasFreeGroupSlot = false;
    for (int g = PinGroupConstants::kMinGroupId; g <= PinGroupConstants::kMaxGroupId; ++g) {
        const size_t cnt = m_groupCounts[static_cast<size_t>(g)];
        const bool isCreated = m_createdGroups[static_cast<size_t>(g)];
        if (cnt == 0 && !isCreated) {
            hasFreeGroupSlot = true;
        }
        if (cnt == 0 && !isCreated && m_activeFilter != g) continue;
        BarChip gChip;
        gChip.kind = ChipKind::FilterGroup;
        gChip.groupParam = g;
        gChip.hasDot = true;
        gChip.dotColor = PinGroupPalette::accentColor(g, 1.0f);
        gChip.label = std::format(L"{} · {}", PinGroupPalette::shortLabel(g, zh), cnt);
        gChip.isActive = (m_activeFilter == g);
        m_chips.push_back(std::move(gChip));
    }

    // 3. 若同时存在已分组与未分组贴图，提供 "未分组" 过滤胶囊
    if (m_ungroupedCount > 0 || m_activeFilter == PinGroupConstants::kUngroupedId) {
        BarChip unChip;
        unChip.kind = ChipKind::FilterGroup;
        unChip.groupParam = PinGroupConstants::kUngroupedId;
        unChip.label = zh ? std::format(L"未分组 {}", m_ungroupedCount)
                          : std::format(L"Ungrouped {}", m_ungroupedCount);
        unChip.isActive = (m_activeFilter == PinGroupConstants::kUngroupedId);
        m_chips.push_back(std::move(unChip));
    }

    // 4. 快捷新建组、一键编组与当前组管理动作
    bool addedSeparator = false;
    if (hasFreeGroupSlot) {
        BarChip newChip;
        newChip.kind = ChipKind::CreateNewGroup;
        newChip.label = zh ? L"+ 新组" : L"+ Group";
        newChip.isSeparatorBefore = !addedSeparator;
        addedSeparator = true;
        m_chips.push_back(std::move(newChip));
    }

    if (m_ungroupedCount >= 2) {
        BarChip autoChip;
        autoChip.kind = ChipKind::AutoGroupUngrouped;
        autoChip.label = zh ? L"一键编组" : L"Auto Group";
        autoChip.isSeparatorBefore = !addedSeparator;
        addedSeparator = true;
        m_chips.push_back(std::move(autoChip));
    }

    if (m_activeFilter >= PinGroupConstants::kMinGroupId &&
        m_activeFilter <= PinGroupConstants::kMaxGroupId) {
        {
            BarChip renChip;
            renChip.kind = ChipKind::RenameActiveGroup;
            renChip.groupParam = m_activeFilter;
            renChip.label = zh ? L"改名" : L"Rename";
            renChip.isActive = (m_editingGroupId == m_activeFilter);
            renChip.isSeparatorBefore = !addedSeparator;
            addedSeparator = true;
            m_chips.push_back(std::move(renChip));
        }

        if (m_groupCounts[static_cast<size_t>(m_activeFilter)] >= 2) {
            BarChip arrChip;
            arrChip.kind = ChipKind::ArrangeActiveGroup;
            arrChip.groupParam = m_activeFilter;
            arrChip.label = zh ? L"整理本组" : L"Arrange";
            arrChip.isSeparatorBefore = !addedSeparator;
            addedSeparator = true;
            m_chips.push_back(std::move(arrChip));
        }

        BarChip disChip;
        disChip.kind = ChipKind::DissolveActiveGroup;
        disChip.groupParam = m_activeFilter;
        disChip.label = zh ? L"移除分组" : L"Ungroup";
        disChip.isSeparatorBefore = !addedSeparator;
        addedSeparator = true;
        m_chips.push_back(std::move(disChip));
    }

    // 计算几何尺寸（留出柔和底投影边距 shadowMargin）
    const float shadowMargin = std::ceil(6.0f * scale);
    const float barPadX = 8.0f * scale;
    const float barPadY = 5.0f * scale;
    const float chipH = 26.0f * scale;
    const float chipGap = 4.0f * scale;
    const float sepExtra = 10.0f * scale;
    const float dotAllowance = 13.0f * scale;

    float curX = shadowMargin + barPadX;
    const float chipTop = shadowMargin + barPadY;
    for (size_t i = 0; i < m_chips.size(); ++i) {
        if (i > 0) {
            curX += m_chips[i].isSeparatorBefore ? sepExtra : chipGap;
        }
        const bool isEditingThisGroup =
            (m_chips[i].kind == ChipKind::FilterGroup &&
             m_chips[i].groupParam >= PinGroupConstants::kMinGroupId &&
             m_chips[i].groupParam == m_editingGroupId);
        float chipW = 0.0f;
        if (isEditingThisGroup) {
            const std::wstring seedLabel = PinGroupPalette::shortLabel(m_chips[i].groupParam, zh);
            const float seedW = measureChipText(seedLabel, scale);
            const float editorBoxW = std::clamp(seedW + 28.0f * scale, 96.0f * scale, 220.0f * scale);
            chipW = std::ceil(dotAllowance + editorBoxW + 15.0f * scale);
        } else {
            const float textW = measureChipText(m_chips[i].label, scale);
            chipW = std::ceil(textW + (m_chips[i].hasDot ? dotAllowance : 0.0f) + 18.0f * scale);
        }
        m_chips[i].rect = D2D1::RectF(curX, chipTop, curX + chipW, chipTop + chipH);
        curX += chipW;
    }

    const int workW = (std::max)(64, static_cast<int>(work.right - work.left));
    const int maxWinW = (std::max)(64, workW - static_cast<int>(std::lround(16.0f * scale)));
    const float panelLeft = shadowMargin;
    const float panelTop = shadowMargin;
    const int naturalWinW = static_cast<int>(std::ceil(curX + barPadX + shadowMargin));
    const int winW = (std::min)(naturalWinW, maxWinW);
    const float panelRight = (std::max)(panelLeft + 24.0f, static_cast<float>(winW) - shadowMargin);
    const float panelBottom = chipTop + chipH + barPadY;
    const int winH = static_cast<int>(std::ceil(panelBottom + shadowMargin));

    const int winX = work.left + (workW - winW) / 2;
    const int winY = work.top + static_cast<int>(std::lround(10.0f * scale));

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return;
    HDC memDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = winW;
    bmi.bmiHeader.biHeight = -winH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return;
    }

    bool foundEditingChip = false;
    RECT localEditRect{};

    HGDIOBJ oldBmp = SelectObject(memDc, dib);
    RECT bindRect{0, 0, winW, winH};
    const HRESULT bindHr = m_dcTarget->BindDC(memDc, &bindRect);
    HRESULT endHr = E_FAIL;
    if (SUCCEEDED(bindHr)) {
        m_dcTarget->BeginDraw();
        m_dcTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        const auto pal = FloatingGlassBar::resolveThemePalette();
        const D2D1_RECT_F panelRect = D2D1::RectF(panelLeft, panelTop, panelRight, panelBottom);
        FloatingGlassBar::drawGlassPanel(m_dcTarget.Get(), panelRect, 11.0f * scale, pal.isDark);

        const D2D1_RECT_F contentClip = D2D1::RectF(
            panelLeft + 2.0f * scale, panelTop,
            (std::max)(panelLeft + 4.0f * scale, panelRight - 2.0f * scale), panelBottom);
        m_dcTarget->PushAxisAlignedClip(contentClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        ComPtr<ID2D1SolidColorBrush> primaryBrush, activeBgBrush, activeBorderBrush, hoverBgBrush, dangerHoverBrush, dangerTextBrush;
        m_dcTarget->CreateSolidColorBrush(pal.textPrimary, primaryBrush.GetAddressOf());
        m_dcTarget->CreateSolidColorBrush(pal.accentSubtle, activeBgBrush.GetAddressOf());
        m_dcTarget->CreateSolidColorBrush(pal.accentBorder, activeBorderBrush.GetAddressOf());
        m_dcTarget->CreateSolidColorBrush(pal.hoverBg, hoverBgBrush.GetAddressOf());
        m_dcTarget->CreateSolidColorBrush(D2D1::ColorF(244.0f / 255.0f, 63.0f / 255.0f, 94.0f / 255.0f, pal.isDark ? 0.18f : 0.11f), dangerHoverBrush.GetAddressOf());
        m_dcTarget->CreateSolidColorBrush(pal.isDark ? D2D1::ColorF(1.0f, 0.42f, 0.52f, 0.98f)
                                                     : D2D1::ColorF(225.0f / 255.0f, 29.0f / 255.0f, 72.0f / 255.0f, 0.96f), dangerTextBrush.GetAddressOf());

        for (size_t i = 0; i < m_chips.size(); ++i) {
            const auto& chip = m_chips[i];
            if (chip.rect.left >= contentClip.right) break;
            if (chip.isSeparatorBefore && i > 0) {
                const float sepX = (m_chips[i - 1].rect.right + chip.rect.left) * 0.5f;
                FloatingGlassBar::drawSeparator(
                    m_dcTarget.Get(), sepX, chip.rect.top + 4.0f * scale, chip.rect.bottom - 4.0f * scale, 1.0f, pal.isDark);
            }

            const bool hovered = (static_cast<int>(i) == m_hoveredChipIndex);
            const bool isDissolve = (chip.kind == ChipKind::DissolveActiveGroup);
            const bool isEditingThisGroup =
                (chip.kind == ChipKind::FilterGroup &&
                 chip.groupParam >= PinGroupConstants::kMinGroupId &&
                 chip.groupParam == m_editingGroupId);
            const auto chipRound = D2D1::RoundedRect(chip.rect, 7.0f * scale, 7.0f * scale);

            if (isEditingThisGroup) {
                ComPtr<ID2D1SolidColorBrush> editSurfaceBg, editGlowBorder, editCrispBorder;
                D2D1_COLOR_F glowCol = chip.dotColor;
                glowCol.a = pal.isDark ? 0.28f : 0.20f;
                D2D1_COLOR_F bdCol = chip.dotColor;
                bdCol.a = 0.90f;
                m_dcTarget->CreateSolidColorBrush(
                    pal.isDark ? D2D1::ColorF(30.0f / 255.0f, 35.0f / 255.0f, 46.0f / 255.0f, 1.0f)
                               : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                    editSurfaceBg.GetAddressOf());
                m_dcTarget->CreateSolidColorBrush(glowCol, editGlowBorder.GetAddressOf());
                m_dcTarget->CreateSolidColorBrush(bdCol, editCrispBorder.GetAddressOf());
                if (editGlowBorder) {
                    const D2D1_RECT_F outerRc = D2D1::RectF(
                        chip.rect.left - 1.0f * scale, chip.rect.top - 1.0f * scale,
                        chip.rect.right + 1.0f * scale, chip.rect.bottom + 1.0f * scale);
                    m_dcTarget->DrawRoundedRectangle(
                        D2D1::RoundedRect(outerRc, 8.0f * scale, 8.0f * scale),
                        editGlowBorder.Get(), 2.0f * scale);
                }
                if (editSurfaceBg) m_dcTarget->FillRoundedRectangle(chipRound, editSurfaceBg.Get());
                if (editCrispBorder) m_dcTarget->DrawRoundedRectangle(chipRound, editCrispBorder.Get(), 1.4f * scale);
            } else if (chip.isActive) {
                if (chip.hasDot) {
                    ComPtr<ID2D1SolidColorBrush> groupTintBg, groupTintBorder;
                    D2D1_COLOR_F bgCol = chip.dotColor;
                    bgCol.a = 0.15f;
                    D2D1_COLOR_F bdCol = chip.dotColor;
                    bdCol.a = 0.52f;
                    m_dcTarget->CreateSolidColorBrush(bgCol, groupTintBg.GetAddressOf());
                    m_dcTarget->CreateSolidColorBrush(bdCol, groupTintBorder.GetAddressOf());
                    if (groupTintBg) m_dcTarget->FillRoundedRectangle(chipRound, groupTintBg.Get());
                    if (groupTintBorder) m_dcTarget->DrawRoundedRectangle(chipRound, groupTintBorder.Get(), 1.1f * scale);
                } else {
                    if (activeBgBrush) m_dcTarget->FillRoundedRectangle(chipRound, activeBgBrush.Get());
                    if (activeBorderBrush) m_dcTarget->DrawRoundedRectangle(chipRound, activeBorderBrush.Get(), 1.0f * scale);
                }
            } else if (hovered) {
                auto* hBr = isDissolve ? dangerHoverBrush.Get() : hoverBgBrush.Get();
                if (hBr) m_dcTarget->FillRoundedRectangle(chipRound, hBr);
            }

            float textLeft = chip.rect.left + 8.0f * scale;
            if (chip.hasDot) {
                const float dotR = 3.8f * scale;
                const float dotCx = chip.rect.left + 9.5f * scale + dotR;
                const float dotCy = (chip.rect.top + chip.rect.bottom) * 0.5f;
                ComPtr<ID2D1SolidColorBrush> dotBrush;
                m_dcTarget->CreateSolidColorBrush(chip.dotColor, dotBrush.GetAddressOf());
                if (dotBrush) {
                    m_dcTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotCx, dotCy), dotR, dotR), dotBrush.Get());
                }
                textLeft = dotCx + dotR + 4.5f * scale;
            }

            if (isEditingThisGroup) {
                const float clampedRight = (std::min)(chip.rect.right, contentClip.right);
                if (clampedRight - textLeft >= 28.0f * scale) {
                    foundEditingChip = true;
                    localEditRect.left = static_cast<LONG>(std::ceil(textLeft));
                    localEditRect.top = static_cast<LONG>(std::ceil(chip.rect.top + 3.0f * scale));
                    localEditRect.right = static_cast<LONG>(std::floor(clampedRight - 6.0f * scale));
                    localEditRect.bottom = static_cast<LONG>(std::floor(chip.rect.bottom - 3.0f * scale));
                }
                continue;
            }

            ID2D1SolidColorBrush* txBrush = (isDissolve && hovered && dangerTextBrush)
                ? dangerTextBrush.Get()
                : primaryBrush.Get();
            if (txBrush && m_textFormat) {
                const D2D1_RECT_F textRect = D2D1::RectF(
                    textLeft, chip.rect.top,
                    (std::max)(textLeft, chip.rect.right - 7.0f * scale), chip.rect.bottom);
                m_dcTarget->DrawTextW(
                    chip.label.c_str(), static_cast<UINT32>(chip.label.size()),
                    m_textFormat.Get(), textRect, txBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        m_dcTarget->PopAxisAlignedClip();
        endHr = m_dcTarget->EndDraw();
    }

    if (endHr == D2DERR_RECREATE_TARGET) {
        m_dcTarget.Reset();
        SelectObject(memDc, oldBmp);
        DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        if (retryDepth < 1) {
            rebuildAndRender(retryDepth + 1);
        }
        return;
    }

    if (SUCCEEDED(bindHr) && SUCCEEDED(endHr)) {
        POINT ptSrc{0, 0};
        POINT ptDst{winX, winY};
        SIZE wndSize{winW, winH};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UpdateLayeredWindow(m_hwnd, screenDc, &ptDst, &wndSize, memDc, &ptSrc, 0, &blend, ULW_ALPHA);
        SetWindowPos(m_hwnd, HWND_TOPMOST, winX, winY, winW, winH, SWP_NOACTIVATE | SWP_SHOWWINDOW);

        if (foundEditingChip) {
            m_editingScreenRect = RECT{
                winX + localEditRect.left,
                winY + localEditRect.top,
                winX + localEditRect.right,
                winY + localEditRect.bottom
            };
            if (m_editPopupHwnd && IsWindow(m_editPopupHwnd)) {
                updateInlineEditorFont(scale);
                repositionInlineEditor();
            }
        } else {
            m_editingScreenRect = {};
        }
    }

    SelectObject(memDc, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

void PinGroupBarOverlay::handleChipClick(const BarChip& chip) {
    switch (chip.kind) {
        case ChipKind::FilterGroup: {
            if (chip.groupParam != PinGroupConstants::kFilterAllGroups &&
                m_activeFilter == chip.groupParam) {
                PinWindow::setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
            } else {
                PinWindow::setActiveGroupFilter(chip.groupParam);
            }
            break;
        }
        case ChipKind::CreateNewGroup: {
            PinWindow::createAndActivateNewGroup(nullptr);
            break;
        }
        case ChipKind::AutoGroupUngrouped: {
            PinWindow::groupAllUngrouped();
            break;
        }
        case ChipKind::RenameActiveGroup: {
            beginInlineRename(chip.groupParam);
            break;
        }
        case ChipKind::ArrangeActiveGroup: {
            PinWindow::arrangeGroup(chip.groupParam);
            break;
        }
        case ChipKind::DissolveActiveGroup: {
            PinWindow::dissolveGroup(chip.groupParam);
            break;
        }
    }
}

void PinGroupBarOverlay::handleGroupContextMenu(int groupId, POINT screenPt) {
    if (groupId < PinGroupConstants::kMinGroupId || groupId > PinGroupConstants::kMaxGroupId) return;
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    if (m_editingGroupId != 0) {
        commitInlineRename(true);
        if (!m_hwnd || !IsWindow(m_hwnd) || !isVisible()) return;
    }

    const bool zh = tools3000::core::isUiLanguageChinese();
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const std::wstring renameLabel = zh ? L"重命名本组..." : L"Rename Group...";
    const std::wstring onlyLabel = zh
        ? std::format(L"仅查看{}", PinGroupPalette::shortLabel(groupId, true))
        : std::format(L"View Only {}", PinGroupPalette::shortLabel(groupId, false));
    const std::wstring arrangeLabel = zh ? L"网格整理本组贴图" : L"Arrange Group in Grid";
    const std::wstring dissolveLabel = zh ? L"移除本组标记（保留贴图）" : L"Ungroup All (Keep Pins)";
    const std::wstring closeGroupLabel = zh ? L"关闭本组全部贴图" : L"Close All Pins in Group";

    AppendMenuW(menu, MF_STRING, 5, renameLabel.c_str());
    AppendMenuW(menu, MF_STRING, 1, onlyLabel.c_str());
    AppendMenuW(menu, MF_STRING, 2, arrangeLabel.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, dissolveLabel.c_str());
    HMENU colorSubMenu = CreatePopupMenu();
    if (colorSubMenu) {
        const std::string activeHex = PinGroupPalette::accentHex(groupId);
        for (int slot = 1; slot <= PinGroupConstants::kPaletteColorCount; ++slot) {
            const char* pHex = PinGroupPalette::presetPaletteHexes()[static_cast<size_t>(slot - 1)];
            const std::wstring cLabel = std::format(
                L"{}  ({})",
                PinGroupPalette::presetColorName(slot, zh),
                tools3000::core::WinUtils::utf8ToWstring(pHex));
            const UINT cFlags = ((activeHex == pHex) ? MF_CHECKED : MF_UNCHECKED) | MF_STRING;
            AppendMenuW(colorSubMenu, cFlags, static_cast<UINT_PTR>(100 + slot), cLabel.c_str());
        }
        if (PinGroupPalette::hasCustomization(groupId)) {
            AppendMenuW(colorSubMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(colorSubMenu, MF_STRING, 100, zh ? L"恢复默认名称与颜色" : L"Reset Name & Color");
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(colorSubMenu),
                    zh ? L"切换本组颜色" : L"Group Color");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, 4, closeGroupLabel.c_str());

    // 临时剥离 WS_EX_NOACTIVATE，确保点击弹出菜单外部区域时 Win32 能正常取消菜单
    const LONG_PTR prevExStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (prevExStyle & WS_EX_NOACTIVATE) {
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, prevExStyle & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
    }
    SetForegroundWindow(m_hwnd);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screenPt.x, screenPt.y, 0, m_hwnd, nullptr);
    if (m_hwnd && IsWindow(m_hwnd)) {
        PostMessageW(m_hwnd, WM_NULL, 0, 0);
        if (prevExStyle & WS_EX_NOACTIVATE) {
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, prevExStyle);
        }
    }
    DestroyMenu(menu);

    switch (cmd) {
        case 1: PinWindow::setActiveGroupFilter(groupId); break;
        case 2: PinWindow::arrangeGroup(groupId); break;
        case 3: PinWindow::dissolveGroup(groupId); break;
        case 4: PinWindow::closeGroup(groupId); break;
        case 5: beginInlineRename(groupId); break;
        case 100:
            PinGroupPalette::resetGroupCustomization(groupId);
            PinWindow::refreshAllVisualsAndGroupBar();
            break;
        default:
            if (cmd >= 101 && cmd <= 100 + PinGroupConstants::kPaletteColorCount) {
                const char* chosenHex = PinGroupPalette::presetPaletteHexes()[static_cast<size_t>(cmd - 101)];
                PinGroupPalette::setGroupColor(groupId, chosenHex);
                PinWindow::refreshAllVisualsAndGroupBar();
            }
            break;
    }
}

LRESULT CALLBACK PinGroupBarOverlay::editPopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PinGroupBarOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    static HBRUSH s_darkEditBrush = CreateSolidBrush(RGB(30, 35, 46));
    const bool isDark = FloatingGlassBar::isEffectiveDarkMode();
    switch (msg) {
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            if (hdc) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect(hdc, &rc, isDark && s_darkEditBrush
                                       ? s_darkEditBrush
                                       : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            }
            return 1;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            if (hdc) {
                SetTextColor(hdc, isDark ? RGB(248, 250, 252) : RGB(15, 23, 42));
                SetBkColor(hdc, isDark ? RGB(30, 35, 46) : RGB(255, 255, 255));
            }
            return reinterpret_cast<LRESULT>(
                isDark && s_darkEditBrush ? s_darkEditBrush : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        }
        case WM_SETFOCUS: {
            if (self && self->m_editControlHwnd && IsWindow(self->m_editControlHwnd)) {
                SetFocus(self->m_editControlHwnd);
                return 0;
            }
            break;
        }
        case WM_NCDESTROY: {
            if (self && self->m_editPopupHwnd == hwnd) {
                self->m_editPopupHwnd = nullptr;
            }
            break;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK PinGroupBarOverlay::editControlSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PinGroupBarOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    WNDPROC origProc = self ? self->m_origEditWndProc : nullptr;

    switch (msg) {
        case WM_GETDLGCODE: {
            const LRESULT base = origProc ? CallWindowProcW(origProc, hwnd, msg, wParam, lParam)
                                          : DefWindowProcW(hwnd, msg, wParam, lParam);
            return base | DLGC_WANTALLKEYS;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_RETURN) {
                if (self) {
                    self->commitInlineRename(true);
                }
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                if (self) {
                    self->commitInlineRename(false);
                }
                return 0;
            }
            if (wParam == 'A' && GetKeyState(VK_CONTROL) < 0) {
                SendMessageW(hwnd, EM_SETSEL, 0, -1);
                return 0;
            }
            break;
        }
        case WM_CHAR: {
            if (wParam == L'\r' || wParam == L'\n' || wParam == 0x1B || wParam == 0x01) {
                return 0;
            }
            break;
        }
        case WM_KILLFOCUS: {
            if (self && !self->m_closingInlineEdit && self->m_editingGroupId != 0) {
                HWND nextFocus = reinterpret_cast<HWND>(wParam);
                if (nextFocus != hwnd && nextFocus != self->m_editPopupHwnd) {
                    if (self->m_hwnd && IsWindow(self->m_hwnd)) {
                        PostMessageW(self->m_hwnd, kMsgAsyncCommitInlineEdit, 1, 0);
                    }
                }
            }
            break;
        }
        default:
            break;
    }

    return origProc ? CallWindowProcW(origProc, hwnd, msg, wParam, lParam)
                    : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK PinGroupBarOverlay::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PinGroupBarOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case kMsgAsyncCommitInlineEdit: {
            if (self && self->m_editingGroupId != 0) {
                self->commitInlineRename(wParam != 0);
            }
            return 0;
        }

        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE: {
            if (self && self->isVisible()) {
                self->rebuildAndRender();
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!self) break;
            TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);

            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));
            int hit = -1;
            for (size_t i = 0; i < self->m_chips.size(); ++i) {
                const auto& r = self->m_chips[i].rect;
                if (mx >= r.left && mx <= r.right && my >= r.top && my <= r.bottom) {
                    hit = static_cast<int>(i);
                    break;
                }
            }
            if (hit != self->m_hoveredChipIndex) {
                self->m_hoveredChipIndex = hit;
                self->rebuildAndRender();
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (self && self->m_hoveredChipIndex != -1) {
                self->m_hoveredChipIndex = -1;
                self->rebuildAndRender();
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (!self) break;
            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));
            POINT screenPt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &screenPt);

            std::optional<BarChip> clickedCopy;
            int hitGroupId = 0;
            for (const auto& chip : self->m_chips) {
                if (mx >= chip.rect.left && mx <= chip.rect.right &&
                    my >= chip.rect.top && my <= chip.rect.bottom) {
                    clickedCopy = chip;
                    if (chip.kind == ChipKind::FilterGroup &&
                        chip.groupParam >= PinGroupConstants::kMinGroupId) {
                        hitGroupId = chip.groupParam;
                    }
                    break;
                }
            }

            // 优先识别原地双击分组胶囊（抗居中宽度重排位移）
            if (self->tryConsumeGroupDoubleClick(screenPt, hitGroupId)) {
                return 0;
            }

            if (self->m_editingGroupId != 0) {
                // 若正在编辑某组且点击了同一胶囊（如圆点边缘）或再次点「改名」，保持当前输入焦点不中断
                if (hitGroupId == self->m_editingGroupId ||
                    (clickedCopy.has_value() &&
                     clickedCopy->kind == ChipKind::RenameActiveGroup &&
                     clickedCopy->groupParam == self->m_editingGroupId)) {
                    if (self->m_editControlHwnd && IsWindow(self->m_editControlHwnd)) {
                        SetForegroundWindow(self->m_editPopupHwnd);
                        SetFocus(self->m_editControlHwnd);
                    }
                    return 0;
                }
                self->commitInlineRename(true);
            }

            if (clickedCopy.has_value()) {
                if (hitGroupId >= PinGroupConstants::kMinGroupId) {
                    self->m_lastGroupClickTick = GetTickCount64();
                    self->m_lastGroupClickScreenPt = screenPt;
                    self->m_lastClickedGroupId = hitGroupId;
                } else {
                    self->m_lastGroupClickTick = 0;
                    self->m_lastClickedGroupId = 0;
                }
                self->handleChipClick(*clickedCopy);
            } else {
                self->m_lastGroupClickTick = 0;
                self->m_lastClickedGroupId = 0;
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            if (!self) break;
            const float mx = static_cast<float>(GET_X_LPARAM(lParam));
            const float my = static_cast<float>(GET_Y_LPARAM(lParam));
            for (const auto& chip : self->m_chips) {
                if (chip.kind == ChipKind::FilterGroup &&
                    chip.groupParam >= PinGroupConstants::kMinGroupId &&
                    mx >= chip.rect.left && mx <= chip.rect.right &&
                    my >= chip.rect.top && my <= chip.rect.bottom) {
                    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    ClientToScreen(hwnd, &pt);
                    const int targetGroup = chip.groupParam;
                    self->handleGroupContextMenu(targetGroup, pt);
                    break;
                }
            }
            return 0;
        }

        case WM_NCDESTROY: {
            if (self && self->m_hwnd == hwnd) {
                self->m_hwnd = nullptr;
            }
            break;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::capture
