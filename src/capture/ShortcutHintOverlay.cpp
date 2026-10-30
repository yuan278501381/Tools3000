#include "capture/ShortcutHintOverlay.h"
#include "core/accessibility/OverlayAnnouncement.h"
#include "core/accessibility/OverlayUiaProvider.h"
#include "capture/ShortcutHintStyle.h"

#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace tools3000::capture {

namespace {
constexpr const wchar_t* WindowClass = L"Tools3000_ShortcutHintOverlay";
constexpr UINT_PTR kWatchdogTimerId = 1;

bool useChineseLabels() {
    const auto language = tools3000::core::ConfigManager::instance().get<std::string>(
        "/general/language", "auto");
    return language == "zh-CN" ||
           (language == "auto" && tools3000::core::WinUtils::isSystemLanguageChinese());
}

std::wstring configuredShortcut(const char* name, const wchar_t* fallback) {
    for (const auto& entry : tools3000::core::HotkeyManager::instance().getAllHotkeys()) {
        if (entry.name == name) {
            const auto text = entry.def.toString();
            return text.empty() ? std::wstring{} : tools3000::core::WinUtils::utf8ToWstring(text);
        }
    }
    return fallback;
}

bool highContrastEnabled() {
    HIGHCONTRASTW highContrast{sizeof(highContrast)};
    return SystemParametersInfoW(
               SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) &&
           (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

D2D1_COLOR_F systemColor(int colorIndex, float alpha = 1.0f) {
    const COLORREF color = GetSysColor(colorIndex);
    return D2D1::ColorF(
        GetRValue(color) / 255.0f,
        GetGValue(color) / 255.0f,
        GetBValue(color) / 255.0f,
        alpha);
}
}  // namespace

ShortcutHintOverlay& ShortcutHintOverlay::instance() {
    static ShortcutHintOverlay overlay;
    return overlay;
}

bool ShortcutHintOverlay::checkOwnerActive(ShortcutHintContext context) const {
    if (m_ownerActiveChecker) {
        return m_ownerActiveChecker(context);
    }
    switch (context) {
        case ShortcutHintContext::CaptureSelecting:
        case ShortcutHintContext::CaptureDragging:
        case ShortcutHintContext::CaptureSelected:
        case ShortcutHintContext::CaptureMarking:
        case ShortcutHintContext::CaptureMarkingNumber:
        case ShortcutHintContext::CaptureEditingText:
        case ShortcutHintContext::RecordSelecting: {
            HWND captureHwnd = FindWindowW(L"Tools3000_CaptureOverlay", nullptr);
            return captureHwnd != nullptr && IsWindowVisible(captureHwnd) != FALSE;
        }
        case ShortcutHintContext::ScrollCapture: {
            HWND scrollHwnd = FindWindowW(L"Tools3000_ScrollCaptureOverlay", nullptr);
            return scrollHwnd != nullptr && IsWindowVisible(scrollHwnd) != FALSE;
        }
        case ShortcutHintContext::Recording:
        case ShortcutHintContext::RecordingPaused: {
            HWND recHwnd = FindWindowW(L"Tools3000_RecordingIndicator", nullptr);
            return recHwnd != nullptr && IsWindowVisible(recHwnd) != FALSE;
        }
        default:
            return false;
    }
}

bool ShortcutHintOverlay::isVisible() const {
    return m_hasContext && m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd);
}

RECT ShortcutHintOverlay::getBounds() const {
    if (!isVisible()) return {};
    RECT r{};
    GetWindowRect(m_hwnd, &r);
    return r;
}

std::vector<ShortcutHintItem>
ShortcutHintOverlay::itemsFor(ShortcutHintContext context) const {
    const bool zh = useChineseLabels();
    switch (context) {
        case ShortcutHintContext::CaptureSelecting:
            return zh
                ? std::vector<ShortcutHintItem>{{L"拖拽", L"框选区域"}, {L"单击", L"智能选窗"},
                                                {L"Space", L"按住平移"}, {L"WASD", L"1px微调"},
                                                {L"C", L"复制颜色"}, {L"Ctrl+A", L"全屏"}, {L"Esc", L"退出"}}
                : std::vector<ShortcutHintItem>{{L"Drag", L"Select area"}, {L"Click", L"Auto window"},
                                                {L"Space", L"Hold to pan"}, {L"WASD", L"1px nudge"},
                                                {L"C", L"Copy color"}, {L"Ctrl+A", L"Full screen"}, {L"Esc", L"Exit"}};
        case ShortcutHintContext::CaptureDragging:
            return zh
                ? std::vector<ShortcutHintItem>{{L"W/A/S/D", L"移动鼠标光标"}, {L"Shift / Tab", L"切换吸附 / 辅助微调"},
                                                {L"Space", L"按住平移选区"}, {L"Esc", L"取消"}}
                : std::vector<ShortcutHintItem>{{L"W/A/S/D", L"Move cursor"}, {L"Shift / Tab", L"Toggle snap / nudge"},
                                                {L"Space", L"Hold to pan"}, {L"Esc", L"Cancel"}};
        case ShortcutHintContext::CaptureSelected:
            return zh
                ? std::vector<ShortcutHintItem>{{L"Enter", L"完成并复制"}, {L"Ctrl+T", L"贴图置顶"},
                                                {L"Ctrl+S", L"保存"}, {L"Ctrl+C", L"复制"},
                                                {L"WASD", L"1px微调移动"}, {L"Shift+WASD", L"尺寸微调"},
                                                {L"[ / ]", L"圆角调节"}, {L"R / A / T / N", L"标注工具"},
                                                {L"Esc", L"取消"}}
                : std::vector<ShortcutHintItem>{{L"Enter", L"Copy & finish"}, {L"Ctrl+T", L"Pin to screen"},
                                                {L"Ctrl+S", L"Save"}, {L"Ctrl+C", L"Copy"},
                                                {L"WASD", L"1px nudge"}, {L"Shift+WASD", L"Resize"},
                                                {L"[ / ]", L"Corner radius"}, {L"R / A / T / N", L"Markup tools"},
                                                {L"Esc", L"Cancel"}};
        case ShortcutHintContext::CaptureMarking:
            return zh
                ? std::vector<ShortcutHintItem>{{L"滚轮 / Ctrl+滚轮", L"粗细 / 不透明度"}, {L"Shift", L"约束水平/垂直/45°"},
                                                {L"Ctrl+Z", L"撤销"}, {L"Ctrl+Shift+Z", L"全部清空"}, {L"Esc", L"取消当前标注"}}
                : std::vector<ShortcutHintItem>{{L"Wheel / Ctrl+Wheel", L"Stroke / Opacity"}, {L"Shift", L"Snap 0°/45°/90°"},
                                                {L"Ctrl+Z", L"Undo"}, {L"Ctrl+Shift+Z", L"Clear all"}, {L"Esc", L"Cancel markup"}};
        case ShortcutHintContext::CaptureMarkingNumber:
            return zh
                ? std::vector<ShortcutHintItem>{{L"单击", L"放置序号"}, {L"按住拖拽", L"引出指示箭头"},
                                                {L"[ / ]", L"调整起始号"}, {L"Ctrl+Z", L"撤销"}, {L"Esc", L"完成"}}
                : std::vector<ShortcutHintItem>{{L"Click", L"Place badge"}, {L"Drag", L"Leader arrow"},
                                                {L"[ / ]", L"Number - / +"}, {L"Ctrl+Z", L"Undo"}, {L"Esc", L"Finish"}};
        case ShortcutHintContext::CaptureEditingText:
            return zh
                ? std::vector<ShortcutHintItem>{{L"Enter / 点击空白", L"提交文字"}, {L"Shift+Enter", L"换行"},
                                                {L"Esc", L"放弃编辑"}}
                : std::vector<ShortcutHintItem>{{L"Enter / Click out", L"Submit text"}, {L"Shift+Enter", L"New line"},
                                                {L"Esc", L"Cancel"}};
        case ShortcutHintContext::RecordSelecting:
            return zh
                ? std::vector<ShortcutHintItem>{{L"拖拽", L"选择录制区域"}, {L"Space", L"按住平移"},
                                                {L"Enter", L"确认区域"}, {L"Ctrl+A", L"全屏"}, {L"Esc", L"取消"}}
                : std::vector<ShortcutHintItem>{{L"Drag", L"Select area"}, {L"Space", L"Hold to pan"},
                                                {L"Enter", L"Confirm area"}, {L"Ctrl+A", L"Full screen"}, {L"Esc", L"Cancel"}};
        case ShortcutHintContext::ScrollCapture:
            return zh
                ? std::vector<ShortcutHintItem>{{L"自动滚动", L"保持页面静止"}, {L"Esc", L"完成并保存"}}
                : std::vector<ShortcutHintItem>{{L"Auto scroll", L"Keep content still"}, {L"Esc", L"Finish and save"}};
        case ShortcutHintContext::Recording:
        case ShortcutHintContext::RecordingPaused: {
            std::vector<ShortcutHintItem> items;
            if (const auto pause = configuredShortcut("Record Pause", L"Ctrl+Shift+P"); !pause.empty()) {
                items.push_back({pause, zh
                    ? (context == ShortcutHintContext::RecordingPaused ? L"继续录制" : L"暂停录制")
                    : (context == ShortcutHintContext::RecordingPaused ? L"Resume" : L"Pause")});
            }
            if (const auto stop = configuredShortcut("Record", L"Ctrl+Shift+R"); !stop.empty()) {
                items.push_back({stop, zh ? L"停止并保存" : L"Stop and save"});
            }
            if (const auto mute = configuredShortcut("Mute Microphone", L""); !mute.empty()) {
                items.push_back({mute, zh ? L"麦克风静音" : L"Mute microphone"});
            }
            return items;
        }
    }
    return {};
}

bool ShortcutHintOverlay::createWindow() {
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ShortcutHintOverlay::wndProc), &m_module)) {
        return false;
    }
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = wndProc;
    windowClass.hInstance = m_module;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = WindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        m_module = nullptr;
        return false;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WindowClass, L"", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, windowClass.hInstance, this);
    if (!m_hwnd) return false;
    // 零隐私防截策略 (Zero WDA_EXCLUDEFROMCAPTURE)：保证快捷键指引 HUD 能够被 Windows 截图管线正常截取
    SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
    return true;
}

bool ShortcutHintOverlay::createResources(float scale) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 m_d2dFactory.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.ReleaseAndGetAddressOf())))) return false;

    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(m_d2dFactory->CreateDCRenderTarget(
            &properties, m_renderTarget.ReleaseAndGetAddressOf()))) return false;

    const float keySize = ShortcutHintStyle::BaseKeyFont * scale;
    const float labelSize = ShortcutHintStyle::BaseLabelFont * scale;
    if (FAILED(m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, keySize, L"zh-CN", m_keyFormat.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, labelSize, L"zh-CN", m_labelFormat.ReleaseAndGetAddressOf()))) return false;
    m_keyFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_keyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_labelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_labelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    if (highContrastEnabled()) {
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOW), &m_panelBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOWTEXT), &m_borderBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_HIGHLIGHT), &m_keyBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOWTEXT), &m_keyBorderBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_HIGHLIGHTTEXT), &m_keyTextBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOWTEXT), &m_labelBrush);
        m_renderTarget->CreateSolidColorBrush(systemColor(COLOR_WINDOWTEXT), &m_sheenBrush);
    } else {
        // 黑曜石磨砂半透明深色底衬 rgba(16, 18, 24, 0.95) (极客沉浸无边界质感)
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(16.0f / 255.0f, 18.0f / 255.0f, 24.0f / 255.0f, 0.95f), &m_panelBrush);
        // 高对比模式备用边框 (非高反差模式下不绘制外框)
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f), &m_borderBrush);
        // 纯白高品质实体微晶按键 <kbd> 背景 rgba(255, 255, 255, 0.96)
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), &m_keyBrush);
        // 按键微徽章描边 rgba(0, 0, 0, 0.08)
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f), &m_keyBorderBrush);
        // 纯黑加粗文字 rgba(0, 0, 0, 0.95)
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.95f), &m_keyTextBrush);
        // 说明标签纯白高对比度文字 rgba(238, 238, 238, 0.95)
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(238.0f / 255.0f, 238.0f / 255.0f, 238.0f / 255.0f, 0.95f), &m_labelBrush);
        // 顶部 1px 极微弱黑曜石微晶高光棱线 rgba(255, 255, 255, 0.08)
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), &m_sheenBrush);
    }
    return m_panelBrush && m_keyBrush && m_keyBorderBrush && m_keyTextBrush && m_labelBrush;
}

float ShortcutHintOverlay::measureText(const std::wstring& text, IDWriteTextFormat* format) const {
    if (!m_dwriteFactory || !format || text.empty()) return 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
    if (FAILED(m_dwriteFactory->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), format,
            2048.0f, 128.0f, &textLayout))) return 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    return SUCCEEDED(textLayout->GetMetrics(&metrics)) ? std::ceil(metrics.widthIncludingTrailingWhitespace) : 0.0f;
}

bool ShortcutHintOverlay::layout(const std::vector<ShortcutHintItem>& items,
                                 int /*workWidth*/, float scale, int& width, int& height) {
    m_items.clear();
    if (items.empty()) return false;
    const float padX = ShortcutHintStyle::BaseHorizontalPadding * scale;
    const float padY = ShortcutHintStyle::BaseVerticalPadding * scale;
    const float keyPad = ShortcutHintStyle::BaseKeyHorizontalPadding * scale;
    const float keyHeight = ShortcutHintStyle::BaseKeyHeight * scale;
    const float labelGap = ShortcutHintStyle::BaseLabelGap * scale;
    const float rowGap = ShortcutHintStyle::BaseRowGap * scale;
    const float shadowPad = ShortcutHintStyle::BaseShadowPadding * scale;

    // 1. 遍历测量每项：统一按键列宽与说明文字宽度
    float maxKeyWidth = 0.0f;
    float maxLabelWidth = 0.0f;
    struct ItemMetric {
        ShortcutHintItem item;
        float keyW;
        float labelW;
    };
    std::vector<ItemMetric> metrics;
    metrics.reserve(items.size());

    for (const auto& it : items) {
        const float kw = std::max(30.0f * scale,
            measureText(it.key, m_keyFormat.Get()) + keyPad * 2.0f);
        const float lw = measureText(it.label, m_labelFormat.Get());
        if (kw > maxKeyWidth) maxKeyWidth = kw;
        if (lw > maxLabelWidth) maxLabelWidth = lw;
        metrics.push_back({it, kw, lw});
    }

    const float cardW = padX * 2.0f + maxKeyWidth + labelGap + maxLabelWidth + 6.0f * scale;
    const float cardH = padY * 2.0f + static_cast<float>(items.size()) * keyHeight + static_cast<float>(items.size() - 1) * rowGap;

    m_cardRect = D2D1::RectF(shadowPad, shadowPad, shadowPad + cardW, shadowPad + cardH);

    // 2. 垂直竖排布局，左侧统一为按键列，右侧严格对齐说明列
    float curY = m_cardRect.top + padY;
    const float labelStartX = m_cardRect.left + padX + maxKeyWidth + labelGap;
    for (const auto& m : metrics) {
        m_items.push_back({m.item, m_cardRect.left + padX, curY, m.keyW, m.labelW, labelStartX});
        curY += keyHeight + rowGap;
    }

    const float totalW = cardW + shadowPad * 2.0f;
    const float totalH = cardH + shadowPad * 2.0f;

    width = static_cast<int>(std::ceil(totalW));
    height = static_cast<int>(std::ceil(totalH));
    return width > 0 && height > 0;
}

POINT ShortcutHintOverlay::computeOptimalPosition(int width, int height, const RECT& workArea, float scale,
                                                 const std::vector<RECT>& avoidRects) {
    const float safeScale = (scale > 0.0f) ? scale : 1.0f;
    const int margin = static_cast<int>(ShortcutHintStyle::BaseScreenMargin * safeScale);
    const int workLeft = static_cast<int>(workArea.left);
    const int workTop = static_cast<int>(workArea.top);
    const int workRight = static_cast<int>(workArea.right);
    const int workBottom = static_cast<int>(workArea.bottom);
    const int workWidth = workRight - workLeft;
    const int workHeight = workBottom - workTop;
    const int minX = workLeft + margin;
    const int maxX = (std::max)(minX, workRight - width - margin);
    const int minY = workTop + margin;
    const int maxY = (std::max)(minY, workBottom - height - margin);

    // 默认首选左下角 (PixPin 标杆交互规范)
    POINT defaultPos{
        std::clamp(workLeft + margin, minX, maxX),
        std::clamp(workBottom - height - margin, minY, maxY)
    };
    if (avoidRects.empty()) {
        return defaultPos;
    }

    // 候选布局点位（按视觉稳定度与路径最短排序）：
    // 1. 左下角 (默认最优)
    // 2. 左上角 (同侧垂直翻转)
    // 3. 右下角 (同底边水平翻转)
    // 4. 右上角 (对角翻转)
    // 5. 底部居中
    // 6. 顶部居中
    // 7. 左侧居中
    // 8. 右侧居中
    const int centerX = std::clamp(workLeft + std::max(margin, (workWidth - width) / 2), minX, maxX);
    const int centerY = std::clamp(workTop + std::max(margin, (workHeight - height) / 2), minY, maxY);
    std::vector<POINT> candidates = {
        {minX, maxY},       // 1. 左下角 (默认最优)
        {minX, minY},       // 2. 左上角
        {maxX, maxY},       // 3. 右下角
        {maxX, minY},       // 4. 右上角
        {centerX, maxY},    // 5. 底部居中
        {centerX, minY},    // 6. 顶部居中
        {minX, centerY},    // 7. 左侧居中
        {maxX, centerY},    // 8. 右侧居中
    };

    auto calcIntersectionArea = [](const RECT& r1, const RECT& r2) -> long long {
        int left = std::max(r1.left, r2.left);
        int top = std::max(r1.top, r2.top);
        int right = std::min(r1.right, r2.right);
        int bottom = std::min(r1.bottom, r2.bottom);
        if (left < right && top < bottom) {
            return static_cast<long long>(right - left) * (bottom - top);
        }
        return 0;
    };

    const int pad = std::max(8, static_cast<int>(std::lround(10.0f * safeScale)));
    const RECT cornerBL{minX, maxY, minX + width, maxY + height};
    const RECT cornerTL{minX, minY, minX + width, minY + height};
    const RECT cornerBR{maxX, maxY, maxX + width, maxY + height};
    const RECT cornerTR{maxX, minY, maxX + width, minY + height};

    // 将障碍物分离为「局部可避让障碍物（光标安全区、放大镜、工具栏、中小型窗口等）」与「全域全屏/最大化背景选区」：
    // 彻底解决最大化窗口选区与局部鼠标/放大镜/工具栏同时存在时，大矩形面积掩盖局部碰撞导致左下角不躲避的问题。
    std::vector<RECT> localObstacles;
    std::vector<RECT> globalObstacles;
    localObstacles.reserve(avoidRects.size());
    for (const auto& avoid : avoidRects) {
        if (avoid.right <= avoid.left || avoid.bottom <= avoid.top) continue;
        RECT paddedAvoid{avoid.left - pad, avoid.top - pad, avoid.right + pad, avoid.bottom + pad};
        const long long aBL = calcIntersectionArea(cornerBL, paddedAvoid);
        const long long aTL = calcIntersectionArea(cornerTL, paddedAvoid);
        const long long aBR = calcIntersectionArea(cornerBR, paddedAvoid);
        const long long aTR = calcIntersectionArea(cornerTR, paddedAvoid);
        const long long minCorner = std::min({aBL, aTL, aBR, aTR});
        const long long maxCorner = std::max({aBL, aTL, aBR, aTR});
        if (minCorner > 0 && minCorner * 2 >= maxCorner) {
            globalObstacles.push_back(paddedAvoid);
        } else {
            localObstacles.push_back(paddedAvoid);
        }
    }

    // 若仅存在覆盖全屏四角的全屏/最大化选区且无任何局部障碍物（光标/放大镜/工具栏未进入左下角），固守左下角避免无意义跳动
    if (localObstacles.empty()) {
        return defaultPos;
    }

    auto computeLocalCollision = [&](const RECT& candRect) -> long long {
        long long total = 0;
        for (const auto& obs : localObstacles) {
            total += calcIntersectionArea(candRect, obs);
        }
        return total;
    };

    auto computeGlobalCollision = [&](const RECT& candRect) -> long long {
        long long total = 0;
        for (const auto& obs : globalObstacles) {
            total += calcIntersectionArea(candRect, obs);
        }
        return total;
    };

    const RECT defaultRect{defaultPos.x, defaultPos.y, defaultPos.x + width, defaultPos.y + height};
    const long long defaultLocalCollision = computeLocalCollision(defaultRect);
    if (defaultLocalCollision == 0) {
        return defaultPos;
    }

    POINT bestPos = defaultPos;
    long long bestLocalCollision = defaultLocalCollision;
    long long bestGlobalCollision = computeGlobalCollision(defaultRect);
    int bestGroup = 0;
    long long bestMetric = 0;
    bool hasAlternative = false;

    auto considerCandidate = [&](const POINT& cand, int group, long long metric) {
        const RECT candRect{cand.x, cand.y, cand.x + width, cand.y + height};
        const long long localCol = computeLocalCollision(candRect);
        const long long globalCol = computeGlobalCollision(candRect);
        if (!hasAlternative ||
            std::tie(localCol, globalCol, group, metric) <
                std::tie(bestLocalCollision, bestGlobalCollision, bestGroup, bestMetric)) {
            hasAlternative = true;
            bestLocalCollision = localCol;
            bestGlobalCollision = globalCol;
            bestGroup = group;
            bestMetric = metric;
            bestPos = cand;
        }
    };

    for (size_t i = 1; i < candidates.size(); ++i) {
        considerCandidate(candidates[i], 0, static_cast<long long>(i));
    }

    // 若 8 大标准锚点均存在局部碰撞，动态探测局部障碍物四周邻接候选点，按碰撞面积与距左下角曼哈顿距离择优
    if (bestLocalCollision > 0) {
        std::vector<POINT> dynamicCandidates;
        dynamicCandidates.reserve(localObstacles.size() * 8);
        for (const auto& obs : localObstacles) {
            int rx = std::clamp(static_cast<int>(obs.right + margin), minX, maxX);
            dynamicCandidates.push_back({rx, maxY});
            dynamicCandidates.push_back({rx, minY});

            int lx = std::clamp(static_cast<int>(obs.left - width - margin), minX, maxX);
            dynamicCandidates.push_back({lx, maxY});
            dynamicCandidates.push_back({lx, minY});

            int ty = std::clamp(static_cast<int>(obs.top - height - margin), minY, maxY);
            dynamicCandidates.push_back({minX, ty});
            dynamicCandidates.push_back({maxX, ty});

            int by = std::clamp(static_cast<int>(obs.bottom + margin), minY, maxY);
            dynamicCandidates.push_back({minX, by});
            dynamicCandidates.push_back({maxX, by});
        }

        for (const auto& cand : dynamicCandidates) {
            const long long manhattanDist =
                static_cast<long long>(std::abs(cand.x - defaultPos.x)) +
                static_cast<long long>(std::abs(cand.y - defaultPos.y));
            considerCandidate(cand, 1, manhattanDist);
        }
    }

    if (bestLocalCollision >= defaultLocalCollision) {
        return defaultPos;
    }

    return bestPos;
}

std::vector<RECT> ShortcutHintOverlay::effectiveAvoidRects() const {
    std::vector<RECT> combined = m_avoidRects;
    if (m_hasTrackedCursor && m_trackedCursorPos.x != LONG_MIN && m_trackedCursorPos.y != LONG_MIN) {
        const int pad = std::max(44, static_cast<int>(std::lround(60.0f * m_scale)));
        combined.push_back(RECT{
            m_trackedCursorPos.x - pad,
            m_trackedCursorPos.y - pad,
            m_trackedCursorPos.x + pad,
            m_trackedCursorPos.y + pad
        });
    }
    return combined;
}

void ShortcutHintOverlay::syncPosition() {
    if (!m_hwnd || !IsWindowVisible(m_hwnd) || m_width <= 0 || m_height <= 0) return;
    const auto combined = effectiveAvoidRects();
    POINT pos = computeOptimalPosition(m_width, m_height, m_workArea, m_scale, combined);
    RECT curRect{};
    GetWindowRect(m_hwnd, &curRect);

    // 迟滞防抖 (Hysteresis Anti-Jitter)：
    // 当卡片当前已从默认左下角避让至其他锚点，而新计算的 pos 准备跳回默认左下角时，
    // 检查默认左下角外扩迟滞带 (24px * scale) 是否仍与局部障碍物相交；若仍在迟滞带内则维持当前避让位，彻底杜绝临界鼠标微动引发的乒乓跳闪。
    const float safeScale = (m_scale > 0.0f) ? m_scale : 1.0f;
    const int margin = static_cast<int>(ShortcutHintStyle::BaseScreenMargin * safeScale);
    const int minX = static_cast<int>(m_workArea.left) + margin;
    const int maxX = (std::max)(minX, static_cast<int>(m_workArea.right) - m_width - margin);
    const int minY = static_cast<int>(m_workArea.top) + margin;
    const int maxY = (std::max)(minY, static_cast<int>(m_workArea.bottom) - m_height - margin);
    const POINT defaultPos{
        std::clamp(static_cast<int>(m_workArea.left) + margin, minX, maxX),
        std::clamp(static_cast<int>(m_workArea.bottom) - m_height - margin, minY, maxY)
    };
    const bool currentlyDisplaced = (curRect.left != defaultPos.x || curRect.top != defaultPos.y);
    if (currentlyDisplaced && pos.x == defaultPos.x && pos.y == defaultPos.y && !combined.empty()) {
        const int hystPad = std::max(20, static_cast<int>(std::lround(28.0f * safeScale)));
        const RECT hystDefaultRect{
            defaultPos.x - hystPad,
            defaultPos.y - hystPad,
            defaultPos.x + m_width + hystPad,
            defaultPos.y + m_height + hystPad
        };
        const RECT cornerBL{minX, maxY, minX + m_width, maxY + m_height};
        const RECT cornerTL{minX, minY, minX + m_width, minY + m_height};
        const RECT cornerBR{maxX, maxY, maxX + m_width, maxY + m_height};
        const RECT cornerTR{maxX, minY, maxX + m_width, minY + m_height};
        auto areaOf = [](const RECT& a, const RECT& b) -> long long {
            int l = (std::max)(a.left, b.left);
            int t = (std::max)(a.top, b.top);
            int r = (std::min)(a.right, b.right);
            int bt = (std::min)(a.bottom, b.bottom);
            return (l < r && t < bt) ? static_cast<long long>(r - l) * (bt - t) : 0;
        };
        bool stillNearLocalObstacle = false;
        for (const auto& avoid : combined) {
            if (avoid.right <= avoid.left || avoid.bottom <= avoid.top) continue;
            const long long aBL = areaOf(cornerBL, avoid);
            const long long aTL = areaOf(cornerTL, avoid);
            const long long aBR = areaOf(cornerBR, avoid);
            const long long aTR = areaOf(cornerTR, avoid);
            const long long minC = std::min({aBL, aTL, aBR, aTR});
            const long long maxC = std::max({aBL, aTL, aBR, aTR});
            if (minC > 0 && minC * 2 >= maxC) continue; // 忽略全屏背景矩形
            if (areaOf(hystDefaultRect, avoid) > 0) {
                stillNearLocalObstacle = true;
                break;
            }
        }
        if (stillNearLocalObstacle) {
            RECT stayRect{curRect.left, curRect.top, curRect.left + m_width, curRect.top + m_height};
            bool stayClear = true;
            for (const auto& avoid : combined) {
                if (avoid.right <= avoid.left || avoid.bottom <= avoid.top) continue;
                const long long aBL = areaOf(cornerBL, avoid);
                const long long aTL = areaOf(cornerTL, avoid);
                const long long aBR = areaOf(cornerBR, avoid);
                const long long aTR = areaOf(cornerTR, avoid);
                const long long minC = std::min({aBL, aTL, aBR, aTR});
                const long long maxC = std::max({aBL, aTL, aBR, aTR});
                if (minC > 0 && minC * 2 >= maxC) continue;
                if (areaOf(stayRect, avoid) > 0) {
                    stayClear = false;
                    break;
                }
            }
            if (stayClear) {
                return;
            }
        }
    }

    if (curRect.left != pos.x || curRect.top != pos.y) {
        SetWindowPos(m_hwnd, nullptr, pos.x, pos.y, 0, 0,
                     SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    }
}

void ShortcutHintOverlay::updateAvoidance(const std::vector<RECT>& avoidRects) {
    if (!m_hwnd || !IsWindowVisible(m_hwnd)) return;
    if (GetWindowThreadProcessId(m_hwnd, nullptr) != GetCurrentThreadId()) {
        if (tools3000::core::MainThreadDispatcher::instance().isInitialized()) {
            tools3000::core::MainThreadDispatcher::instance().post([avoidRects]() {
                ShortcutHintOverlay::instance().updateAvoidance(avoidRects);
            });
        }
        return;
    }
    const bool ownerActiveNow = checkOwnerActive(m_context);
    if (!m_observedOwnerActive && ownerActiveNow) {
        m_observedOwnerActive = true;
    } else if ((m_ownerActiveChecker != nullptr || m_observedOwnerActive) && !ownerActiveNow) {
        hide();
        return;
    }
    m_avoidRects = avoidRects;
    m_hasTrackedCursor = false;
    GetCursorPos(&m_initialCursorPos);
    syncPosition();
}

void ShortcutHintOverlay::show(ShortcutHintContext context, POINT anchor,
                              const std::vector<RECT>& avoidRects) {
    if ((m_hwnd && IsWindow(m_hwnd) && GetWindowThreadProcessId(m_hwnd, nullptr) != GetCurrentThreadId()) ||
        (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
         !tools3000::core::MainThreadDispatcher::instance().isOwnerThread())) {
        tools3000::core::MainThreadDispatcher::instance().post([context, anchor, avoidRects]() {
            ShortcutHintOverlay::instance().show(context, anchor, avoidRects);
        });
        return;
    }
    if (!tools3000::core::ConfigManager::instance().get<bool>(
            "/capture/showShortcutHints", true)) {
        hide();
        return;
    }
    auto items = itemsFor(context);
    if (items.empty()) {
        hide();
        return;
    }
    const bool ownerActiveNow = checkOwnerActive(context);
    if ((m_ownerActiveChecker != nullptr || (m_hasContext && m_context == context && m_observedOwnerActive)) &&
        !ownerActiveNow) {
        hide();
        return;
    }
    const bool isContextSwitchOnly =
        (anchor.x == LONG_MIN && anchor.y == LONG_MIN && avoidRects.empty() && m_hasContext && isVisible());
    if (anchor.x == LONG_MIN || anchor.y == LONG_MIN) {
        if (!GetCursorPos(&anchor)) anchor = {0, 0};
    }
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const auto monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return;
    m_workArea = monitorInfo.rcWork;

    const UINT dpiX = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
    const float scale = ShortcutHintStyle::scaleForDpi(dpiX);
    if (!isContextSwitchOnly) {
        m_avoidRects = avoidRects;
        m_hasTrackedCursor = false;
        if (!GetCursorPos(&m_initialCursorPos)) {
            m_initialCursorPos = {LONG_MIN, LONG_MIN};
        }
        m_trackedCursorPos = m_initialCursorPos;
    }
    if (m_hasContext && m_context == context && isVisible() &&
        std::abs(scale - m_scale) <= 0.01f) {
        if (ownerActiveNow) m_observedOwnerActive = true;
        if (!isContextSwitchOnly) {
            updateAvoidance(avoidRects);
        } else {
            syncPosition();
        }
        return;
    }
    if (m_hwnd && !IsWindow(m_hwnd)) {
        m_hwnd = nullptr;
    }
    if (!m_hwnd && !createWindow()) return;
    if (!m_renderTarget || std::abs(scale - m_scale) > 0.01f) {
        discardResources();
        m_scale = scale;
        if (!createResources(scale)) {
            hide();
            return;
        }
    }
    if (!layout(items, m_workArea.right - m_workArea.left, scale, m_width, m_height)) {
        hide();
        return;
    }

    POINT pos = computeOptimalPosition(m_width, m_height, m_workArea, scale, effectiveAvoidRects());
    SetWindowPos(m_hwnd, HWND_TOPMOST, pos.x, pos.y, m_width, m_height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    render();
    std::wstring accessibleName = L"快捷键提示：";
    for (const auto& item : items) {
        if (accessibleName.size() > 1024) break;
        if (accessibleName.size() > std::wstring_view(L"快捷键提示：").size()) accessibleName += L"；";
        accessibleName += item.key;
        accessibleName += L" ";
        accessibleName += item.label;
    }
    tools3000::core::accessibility::announceOverlay(m_hwnd, accessibleName);
    m_context = context;
    m_hasContext = true;
    m_observedOwnerActive = ownerActiveNow;
    SetTimer(m_hwnd, kWatchdogTimerId, 45, nullptr);
}

void ShortcutHintOverlay::render() {
    if (!m_hwnd || !m_renderTarget || m_width <= 0 || m_height <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = m_width;
    bitmapInfo.bmiHeader.biHeight = -m_height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }
    const auto previous = SelectObject(memory, bitmap);
    RECT bounds{0, 0, m_width, m_height};
    m_renderTarget->BindDC(memory, &bounds);
    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    const float cornerR = ShortcutHintStyle::BaseCornerRadius * m_scale;

    // 1. 双层物理分层高斯微阴影 (Dual-Layer Ambient & Key Shadows)
    if (!highContrastEnabled()) {
        struct ShadowStep {
            float dx;
            float dy;
            float expand;
            float alpha;
        };

        static const ShadowStep keySteps[] = {
            { 0.0f, 0.8f, 0.6f, 0.035f },
            { 0.0f, 1.4f, 1.4f, 0.028f },
            { 0.0f, 2.0f, 2.4f, 0.018f },
        };

        static const ShadowStep ambientSteps[] = {
            { 0.0f, 2.5f, 3.5f,  0.020f },
            { 0.0f, 4.0f, 6.5f,  0.016f },
            { 0.0f, 5.5f, 9.5f,  0.012f },
        };

        for (const auto& step : ambientSteps) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, step.alpha * 2.5f), b.GetAddressOf());
            if (b) {
                float exp = step.expand * m_scale;
                auto sRect = D2D1::RectF(
                    m_cardRect.left - exp + step.dx * m_scale,
                    m_cardRect.top - exp + step.dy * m_scale,
                    m_cardRect.right + exp + step.dx * m_scale,
                    m_cardRect.bottom + exp + step.dy * m_scale
                );
                auto rr = D2D1::RoundedRect(sRect, cornerR + exp, cornerR + exp);
                m_renderTarget->FillRoundedRectangle(rr, b.Get());
            }
        }
        for (const auto& step : keySteps) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, step.alpha * 2.5f), b.GetAddressOf());
            if (b) {
                float exp = step.expand * m_scale;
                auto sRect = D2D1::RectF(
                    m_cardRect.left - exp + step.dx * m_scale,
                    m_cardRect.top - exp + step.dy * m_scale,
                    m_cardRect.right + exp + step.dx * m_scale,
                    m_cardRect.bottom + exp + step.dy * m_scale
                );
                auto rr = D2D1::RoundedRect(sRect, cornerR + exp, cornerR + exp);
                m_renderTarget->FillRoundedRectangle(rr, b.Get());
            }
        }
    }

    const auto panel = D2D1::RoundedRect(m_cardRect, cornerR, cornerR);
    m_renderTarget->FillRoundedRectangle(panel, m_panelBrush.Get());
    if (highContrastEnabled() && m_borderBrush) {
        m_renderTarget->DrawRoundedRectangle(panel, m_borderBrush.Get(), 1.0f);
    }

    // 顶部 1px 微晶高光棱线
    if (m_sheenBrush) {
        const float sheenY = m_cardRect.top + 1.5f;
        m_renderTarget->DrawLine(
            D2D1::Point2F(m_cardRect.left + cornerR, sheenY),
            D2D1::Point2F(m_cardRect.right - cornerR, sheenY),
            m_sheenBrush.Get(), 1.0f);
    }

    const float keyHeight = ShortcutHintStyle::BaseKeyHeight * m_scale;
    const float labelGap = ShortcutHintStyle::BaseLabelGap * m_scale;
    for (const auto& positioned : m_items) {
        const auto keyRect = D2D1::RoundedRect(
            D2D1::RectF(positioned.x, positioned.y,
                        positioned.x + positioned.keyWidth, positioned.y + keyHeight),
            ShortcutHintStyle::BaseKeyCornerRadius * m_scale,
            ShortcutHintStyle::BaseKeyCornerRadius * m_scale);
        m_renderTarget->FillRoundedRectangle(keyRect, m_keyBrush.Get());
        if (m_keyBorderBrush) {
            m_renderTarget->DrawRoundedRectangle(keyRect, m_keyBorderBrush.Get(), 1.0f);
        }
        m_renderTarget->DrawTextW(
            positioned.item.key.c_str(), static_cast<UINT32>(positioned.item.key.size()),
            m_keyFormat.Get(), keyRect.rect, m_keyTextBrush.Get());
        const float labelStartX = positioned.labelX > 0.0f ? positioned.labelX : (positioned.x + positioned.keyWidth + labelGap);
        const auto labelRect = D2D1::RectF(
            labelStartX, positioned.y,
            labelStartX + positioned.labelWidth + 4.0f * m_scale,
            positioned.y + keyHeight);
        m_renderTarget->DrawTextW(
            positioned.item.label.c_str(), static_cast<UINT32>(positioned.item.label.size()),
            m_labelFormat.Get(), labelRect, m_labelBrush.Get());
    }
    const HRESULT drawResult = m_renderTarget->EndDraw();
    if (drawResult == D2DERR_RECREATE_TARGET) discardResources();

    POINT source{0, 0};
    SIZE size{m_width, m_height};
    POINT destination = computeOptimalPosition(m_width, m_height, m_workArea, m_scale, effectiveAvoidRects());
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(m_hwnd, screen, &destination, &size, memory,
                        &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

void ShortcutHintOverlay::discardResources() {
    m_sheenBrush.Reset();
    m_keyBorderBrush.Reset();
    m_labelBrush.Reset();
    m_keyTextBrush.Reset();
    m_keyBrush.Reset();
    m_borderBrush.Reset();
    m_panelBrush.Reset();
    m_labelFormat.Reset();
    m_keyFormat.Reset();
    m_renderTarget.Reset();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
}

void ShortcutHintOverlay::hide() {
    m_hasContext = false;
    m_observedOwnerActive = false;
    m_hasTrackedCursor = false;
    m_avoidRects.clear();
    if (m_hwnd) {
        if (!IsWindow(m_hwnd)) {
            m_hwnd = nullptr;
        } else if (GetWindowThreadProcessId(m_hwnd, nullptr) != GetCurrentThreadId()) {
            // 跨线程隐藏防护：Win32 DestroyWindow 仅允许创建窗口的线程调用，
            // 工作线程调用时立即通过 ShowWindowAsync 隐藏视觉残留并投递异步销毁消息。
            ShowWindowAsync(m_hwnd, SW_HIDE);
            PostMessageW(m_hwnd, WM_APP + 1, 0, 0);
            if (tools3000::core::MainThreadDispatcher::instance().isInitialized()) {
                tools3000::core::MainThreadDispatcher::instance().post([]() {
                    if (!ShortcutHintOverlay::instance().isVisible()) {
                        ShortcutHintOverlay::instance().hide();
                    }
                });
            }
            return;
        }
    }
    m_items.clear();
    discardResources();
    if (m_hwnd) {
        HWND hwndToDestroy = m_hwnd;
        m_hwnd = nullptr;
        KillTimer(hwndToDestroy, kWatchdogTimerId);
        tools3000::core::accessibility::hideOverlay(hwndToDestroy);
        ShowWindow(hwndToDestroy, SW_HIDE);
        DestroyWindow(hwndToDestroy);
    }
    if (m_module) {
        UnregisterClassW(WindowClass, m_module);
        m_module = nullptr;
    }
}

std::vector<tools3000::core::accessibility::OverlayUiaAction>
ShortcutHintOverlay::accessibilityActions() const {
    std::vector<tools3000::core::accessibility::OverlayUiaAction> actions;
    if (!m_hwnd || m_items.empty()) return actions;
    RECT window{};
    if (!GetWindowRect(m_hwnd, &window)) return actions;
    const float labelGap = ShortcutHintStyle::BaseLabelGap * m_scale;
    const float itemHeight = ShortcutHintStyle::BaseKeyHeight * m_scale;
    actions.reserve(m_items.size());
    for (std::size_t index = 0; index < m_items.size(); ++index) {
        const auto& item = m_items[index];
        const LONG left = window.left + static_cast<LONG>(std::lround(item.x));
        const LONG top = window.top + static_cast<LONG>(std::lround(item.y));
        const LONG right = left + static_cast<LONG>(std::lround(item.keyWidth + labelGap + item.labelWidth));
        const LONG bottom = top + static_cast<LONG>(std::lround(itemHeight));
        actions.push_back({
            L"Tools3000.ShortcutHint." + std::to_wstring(index),
            item.item.key + L"：" + item.item.label,
            L"Contextual keyboard shortcut", item.item.key,
            {left, top, right, bottom}, true, false,
            tools3000::core::accessibility::OverlayUiaActionRole::Text, 0, 0,
        });
    }
    return actions;
}

LRESULT CALLBACK ShortcutHintOverlay::wndProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* overlay = reinterpret_cast<ShortcutHintOverlay*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        overlay = static_cast<ShortcutHintOverlay*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
    }
    if (message == WM_APP + 1) {
        if (overlay && !overlay->m_hasContext) {
            overlay->hide();
        }
        return 0;
    }
    if (message == WM_TIMER && wParam == kWatchdogTimerId) {
        if (!overlay || !overlay->m_hasContext) {
            KillTimer(hwnd, kWatchdogTimerId);
            if (overlay) overlay->hide();
            return 0;
        }
        const bool ownerActiveNow = overlay->checkOwnerActive(overlay->m_context);
        if (!overlay->m_observedOwnerActive && ownerActiveNow) {
            overlay->m_observedOwnerActive = true;
        } else if ((overlay->m_ownerActiveChecker != nullptr || overlay->m_observedOwnerActive) && !ownerActiveNow) {
            overlay->hide();
            return 0;
        }
        POINT curPt{};
        if (GetCursorPos(&curPt)) {
            const bool movedSinceInit = (overlay->m_initialCursorPos.x == LONG_MIN) ||
                (std::abs(curPt.x - overlay->m_initialCursorPos.x) > 4) ||
                (std::abs(curPt.y - overlay->m_initialCursorPos.y) > 4);
            if (movedSinceInit || overlay->m_hasTrackedCursor) {
                if (!overlay->m_hasTrackedCursor ||
                    std::abs(curPt.x - overlay->m_trackedCursorPos.x) > 2 ||
                    std::abs(curPt.y - overlay->m_trackedCursorPos.y) > 2) {
                    overlay->m_hasTrackedCursor = true;
                    overlay->m_trackedCursorPos = curPt;
                    overlay->syncPosition();
                }
            }
        }
        return 0;
    }
    if (message == WM_CLOSE) {
        if (overlay && overlay->m_hwnd == hwnd) {
            overlay->hide();
        } else {
            KillTimer(hwnd, kWatchdogTimerId);
            tools3000::core::accessibility::hideOverlay(hwnd);
            ShowWindow(hwnd, SW_HIDE);
            DestroyWindow(hwnd);
        }
        return 0;
    }
    if (message == WM_GETOBJECT) {
        return tools3000::core::accessibility::respondToOverlayUiaGetObject(
            hwnd, wParam, lParam,
            {L"Tools3000.ShortcutHints", L"Contextual Tools3000 keyboard shortcut guide",
             tools3000::core::accessibility::OverlayUiaRole::Text, true},
            overlay ? overlay->accessibilityActions()
                    : std::vector<tools3000::core::accessibility::OverlayUiaAction>{});
    }
    if (message == WM_NCDESTROY) {
        if (overlay && overlay->m_hwnd == hwnd) {
            overlay->m_hwnd = nullptr;
            overlay->m_hasContext = false;
            overlay->m_observedOwnerActive = false;
        }
        tools3000::core::accessibility::disconnectOverlayUiaProvider(hwnd);
    }
    if (overlay && (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED ||
                    message == WM_SYSCOLORCHANGE)) {
        overlay->discardResources();
        if (overlay->createResources(overlay->m_scale)) overlay->render();
        return 0;
    }
    if (overlay && overlay->m_hasContext &&
        (message == WM_DPICHANGED || message == WM_DISPLAYCHANGE)) {
        if ((overlay->m_ownerActiveChecker != nullptr || overlay->m_observedOwnerActive) &&
            !overlay->checkOwnerActive(overlay->m_context)) {
            overlay->hide();
            return 0;
        }
        POINT anchor{
            (overlay->m_workArea.left + overlay->m_workArea.right) / 2,
            (overlay->m_workArea.top + overlay->m_workArea.bottom) / 2};
        if (message == WM_DPICHANGED && lParam) {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            anchor = {(suggested->left + suggested->right) / 2,
                      (suggested->top + suggested->bottom) / 2};
        }
        const auto context = overlay->m_context;
        overlay->m_hasContext = false;
        overlay->show(context, anchor);
        return 0;
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace tools3000::capture
