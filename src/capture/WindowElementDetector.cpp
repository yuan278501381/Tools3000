// ─────────────────────────────────────────────────────────────────────────────
// WindowElementDetector.cpp — 世界级截图自动捕捉窗口与控件引擎实现 (MECE 四维架构)
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/WindowElementDetector.h"

#include "core/i18n/UiI18n.h"
#include "core/utils/WinUtils.h"

#include <UIAutomation.h>
#include <dwmapi.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

namespace tools3000::capture {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kMinCandidateEdgePx = 10;
constexpr int kMaxUiaDepth = 16;
constexpr int kMaxUiaChildrenPerNode = 160;
constexpr int kUiaTimeBudgetMs = 45;
constexpr int kVisionTriggerMinW = 280;
constexpr int kVisionTriggerMinH = 200;

inline bool isUiLangZh() {
    return tools3000::core::UiI18n::getEffectiveLanguage() == tools3000::core::UiLanguage::ZhCN;
}

inline int rectWidth(const RECT& r) noexcept {
    return (r.right > r.left) ? (r.right - r.left) : 0;
}

inline int rectHeight(const RECT& r) noexcept {
    return (r.bottom > r.top) ? (r.bottom - r.top) : 0;
}

inline int64_t rectArea(const RECT& r) noexcept {
    return static_cast<int64_t>(rectWidth(r)) * static_cast<int64_t>(rectHeight(r));
}

inline RECT clampRectToBounds(const RECT& rc, int maxW, int maxH) noexcept {
    RECT out{};
    out.left = std::clamp<LONG>(rc.left, 0, maxW);
    out.top = std::clamp<LONG>(rc.top, 0, maxH);
    out.right = std::clamp<LONG>(rc.right, 0, maxW);
    out.bottom = std::clamp<LONG>(rc.bottom, 0, maxH);
    return out;
}

inline bool isNearlySameRect(const RECT& a, const RECT& b, int tolPx = 3) noexcept {
    return std::abs(a.left - b.left) <= tolPx &&
           std::abs(a.top - b.top) <= tolPx &&
           std::abs(a.right - b.right) <= tolPx &&
           std::abs(a.bottom - b.bottom) <= tolPx;
}

bool isWindows11OrNewer() noexcept {
    static const bool cached = []() {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
            if (fn) {
                RTL_OSVERSIONINFOW info{};
                info.dwOSVersionInfoSize = sizeof(info);
                if (fn(&info) == 0) {
                    return info.dwMajorVersion > 10 ||
                           (info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000);
                }
            }
        }
        return false;
    }();
    return cached;
}

std::wstring sanitizeLabelText(std::wstring text, std::size_t maxChars = 14) {
    std::wstring clean;
    clean.reserve(text.size());
    bool lastSpace = false;
    for (wchar_t ch : text) {
        if (ch < 0x20 || ch == 0x7F) {
            if (!lastSpace && !clean.empty()) {
                clean.push_back(L' ');
                lastSpace = true;
            }
            continue;
        }
        if (std::iswspace(ch)) {
            if (!lastSpace && !clean.empty()) {
                clean.push_back(L' ');
                lastSpace = true;
            }
        } else {
            clean.push_back(ch);
            lastSpace = false;
        }
    }
    while (!clean.empty() && clean.back() == L' ') clean.pop_back();
    if (clean.size() > maxChars) {
        clean = clean.substr(0, maxChars - 2) + L"...";
    }
    return clean;
}

bool isIgnoredTopLevelClass(const std::wstring& cls, HWND hwnd, const RECT& screenRect) {
    if (cls == L"Tools3000_ShortcutHintOverlay" ||
        cls == L"Progman" ||
        cls == L"WorkerW" ||
        cls == L"EdgeUiInputTopWndClass" ||
        cls == L"EdgeUiInputWndClass" ||
        cls == L"DummyDwmListenerWindow" ||
        cls == L"Shell_InputSwitchTopLevelWindow" ||
        cls == L"Internet Explorer_Hidden" ||
        cls == L"Shell_Dim" ||
        cls == L"Windows.UI.Core.CoreWindow") {
        // 仅放行真正可见且有标题或当前获焦的 CoreWindow（如开始菜单/搜索浮窗），跳过全屏无标题幽灵宿主
        if (cls == L"Windows.UI.Core.CoreWindow") {
            const int sw = GetSystemMetrics(SM_CXSCREEN);
            const int sh = GetSystemMetrics(SM_CYSCREEN);
            wchar_t titleBuf[32] = {};
            const bool hasTitle = GetWindowTextW(hwnd, titleBuf, 32) > 0;
            const bool fullCover = rectWidth(screenRect) >= sw && rectHeight(screenRect) >= sh;
            if (!hasTitle && fullCover && GetForegroundWindow() != hwnd) {
                return true;
            }
            return false;
        }
        return true;
    }
    return false;
}

int defaultPriorityForKind(DetectedCandidateKind kind) noexcept {
    switch (kind) {
        case DetectedCandidateKind::UiaElement:     return 60;
        case DetectedCandidateKind::Win32Child:     return 50;
        case DetectedCandidateKind::VisionRegion:   return 40;
        case DetectedCandidateKind::WindowClient:   return 30;
        case DetectedCandidateKind::WindowFrame:    return 20;
        case DetectedCandidateKind::MonitorDisplay: return 10;
        case DetectedCandidateKind::VirtualDesktop: return 5;
        default:                                    return 0;
    }
}

std::wstring uiaControlTypeLabel(CONTROLTYPEID ctrlType, bool isZh) {
    switch (ctrlType) {
        case UIA_ButtonControlTypeId:
        case UIA_SplitButtonControlTypeId:
        case UIA_HyperlinkControlTypeId:
        case UIA_CheckBoxControlTypeId:
        case UIA_RadioButtonControlTypeId:
            return tools3000::core::trWCstrFor(isZh, L"按钮", L"Button");
        case UIA_EditControlTypeId:
        case UIA_DocumentControlTypeId:
            return tools3000::core::trWCstrFor(isZh, L"输入框", L"Input");
        case UIA_TextControlTypeId:
            return tools3000::core::trWCstrFor(isZh, L"文本", L"Label");
        case UIA_ComboBoxControlTypeId:
            return tools3000::core::trWCstrFor(isZh, L"下拉框", L"ComboBox");
        case UIA_ListControlTypeId:
        case UIA_ListItemControlTypeId:
        case UIA_DataGridControlTypeId:
        case UIA_DataItemControlTypeId:
        case UIA_TableControlTypeId:
            return tools3000::core::trWCstrFor(isZh, L"列表", L"List");
        case UIA_TreeControlTypeId:
        case UIA_TreeItemControlTypeId:
            return tools3000::core::trWCstrFor(isZh, L"树形控件", L"TreeView");
        case UIA_TabControlTypeId:
        case UIA_TabItemControlTypeId:
            return tools3000::core::trWCstrFor(isZh, L"选项卡", L"Tab");
        case UIA_ToolBarControlTypeId:
        case UIA_MenuBarControlTypeId:
        case UIA_StatusBarControlTypeId:
        case UIA_TitleBarControlTypeId:
        case UIA_HeaderControlTypeId:
            return tools3000::core::trWCstrFor(isZh, L"工具栏", L"Toolbar");
        case UIA_WindowControlTypeId:
            return tools3000::core::trWCstrFor(isZh, L"窗口", L"Window");
        case UIA_ImageControlTypeId:
        case UIA_PaneControlTypeId:
        case UIA_GroupControlTypeId:
        case UIA_MenuControlTypeId:
        case UIA_MenuItemControlTypeId:
        case UIA_CustomControlTypeId:
        default:
            return tools3000::core::trWCstrFor(isZh, L"控件", L"Control");
    }
}

float cornerRadiusForUiaControl(const RECT& rc, CONTROLTYPEID ctrlType) noexcept {
    if (ctrlType == UIA_WindowControlTypeId) return 8.0f;
    const int minEdge = std::min(rectWidth(rc), rectHeight(rc));
    if (minEdge <= 48) return 4.0f;
    if (minEdge <= 160) return 5.0f;
    return 6.0f;
}

struct UiaCachedNode {
    ComPtr<IUIAutomationElement> element;
    RECT screenRect{};
    CONTROLTYPEID controlType = 0;
    std::wstring name;
    bool childrenLoaded = false;
    std::vector<std::unique_ptr<UiaCachedNode>> children;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 纯函数实现（可独立进行 GoogleTest 高覆盖单测）
// ─────────────────────────────────────────────────────────────────────────────

double computeRectIoU(const RECT& a, const RECT& b) noexcept {
    const int64_t areaA = rectArea(a);
    const int64_t areaB = rectArea(b);
    if (areaA <= 0 || areaB <= 0) return 0.0;

    const LONG interL = std::max(a.left, b.left);
    const LONG interT = std::max(a.top, b.top);
    const LONG interR = std::min(a.right, b.right);
    const LONG interB = std::min(a.bottom, b.bottom);
    if (interR <= interL || interB <= interT) return 0.0;

    const int64_t interArea = static_cast<int64_t>(interR - interL) * static_cast<int64_t>(interB - interT);
    const int64_t unionArea = areaA + areaB - interArea;
    if (unionArea <= 0) return 0.0;
    return static_cast<double>(interArea) / static_cast<double>(unionArea);
}

std::vector<DetectedElementCandidate> normalizeDetectedHierarchy(
    std::vector<DetectedElementCandidate> rawCandidates,
    int screenWidth,
    int screenHeight,
    std::size_t maxLayers) {
    std::vector<DetectedElementCandidate> cleaned;
    cleaned.reserve(rawCandidates.size());

    for (auto& item : rawCandidates) {
        if (screenWidth > 0 && screenHeight > 0) {
            item.rect = clampRectToBounds(item.rect, screenWidth, screenHeight);
        }
        if (rectWidth(item.rect) <= kMinCandidateEdgePx || rectHeight(item.rect) <= kMinCandidateEdgePx) {
            continue;
        }
        if (item.zPriority == 0) {
            item.zPriority = defaultPriorityForKind(item.kind);
        }
        cleaned.push_back(std::move(item));
    }

    if (cleaned.empty()) return {};

    // 按面积严格升序排列；面积相同按语义权重降序排列（确保 UIA/子控件胜出）
    std::stable_sort(cleaned.begin(), cleaned.end(), [](const DetectedElementCandidate& a, const DetectedElementCandidate& b) {
        const int64_t aa = rectArea(a.rect);
        const int64_t ab = rectArea(b.rect);
        if (aa != ab) return aa < ab;
        return a.zPriority > b.zPriority;
    });

    // 近似重合智能合并：四边差 <= 3px 或 IoU >= 0.94 视为同一几何层，保留语义权重更高者
    std::vector<DetectedElementCandidate> deduped;
    deduped.reserve(cleaned.size());
    for (auto& cand : cleaned) {
        bool merged = false;
        for (auto& kept : deduped) {
            const bool nearSame = isNearlySameRect(kept.rect, cand.rect, 3);
            const double iou = nearSame ? 1.0 : computeRectIoU(kept.rect, cand.rect);
            if (nearSame || iou >= 0.94) {
                if (cand.zPriority > kept.zPriority) {
                    kept = std::move(cand);
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            deduped.push_back(std::move(cand));
        }
    }

    // 再次确保面积单调递增（防止合并后顺序微移）
    std::stable_sort(deduped.begin(), deduped.end(), [](const DetectedElementCandidate& a, const DetectedElementCandidate& b) {
        return rectArea(a.rect) < rectArea(b.rect);
    });

    // 若层级过多（例如复杂网页嵌套 15 层 div），智能裁剪至 maxLayers：
    // 强制保留最内层(0)、窗口客户区、顶层窗口外框、显示器/全屏，按面积跨度优选最具区分度的中间容器
    if (maxLayers >= 3 && deduped.size() > maxLayers) {
        std::vector<bool> keep(deduped.size(), false);
        keep.front() = true;
        keep.back() = true;
        std::size_t keptCount = 2;

        for (std::size_t i = 1; i + 1 < deduped.size() && keptCount < maxLayers; ++i) {
            const auto k = deduped[i].kind;
            if (k == DetectedCandidateKind::WindowClient ||
                k == DetectedCandidateKind::WindowFrame ||
                k == DetectedCandidateKind::MonitorDisplay) {
                keep[i] = true;
                ++keptCount;
            }
        }

        // 其余中间容器按对数面积均匀间隔挑选
        if (keptCount < maxLayers) {
            std::vector<std::size_t> remaining;
            for (std::size_t i = 1; i + 1 < deduped.size(); ++i) {
                if (!keep[i]) remaining.push_back(i);
            }
            const std::size_t need = maxLayers - keptCount;
            if (remaining.size() <= need) {
                for (std::size_t idx : remaining) keep[idx] = true;
            } else {
                for (std::size_t s = 0; s < need; ++s) {
                    const std::size_t pick = (s * remaining.size()) / need;
                    keep[remaining[pick]] = true;
                }
            }
        }

        std::vector<DetectedElementCandidate> trimmed;
        trimmed.reserve(maxLayers);
        for (std::size_t i = 0; i < deduped.size(); ++i) {
            if (keep[i]) trimmed.push_back(std::move(deduped[i]));
        }
        return trimmed;
    }

    return deduped;
}

bool detectVisualCardRegionAtPoint(
    const cv::Mat& frozenScreen,
    POINT localCursor,
    RECT boundingLimit,
    RECT& outCardRect) {
    if (frozenScreen.empty() || frozenScreen.cols < 40 || frozenScreen.rows < 40) return false;

    const RECT limit = clampRectToBounds(boundingLimit, frozenScreen.cols, frozenScreen.rows);
    const int limitW = rectWidth(limit);
    const int limitH = rectHeight(limit);
    if (limitW < kVisionTriggerMinW || limitH < kVisionTriggerMinH) return false;
    if (!PtInRect(&limit, localCursor)) return false;

    // 围绕光标截取最大 920x720 局部 ROI（严格限制在目标边界内，控制耗时 < 4ms）
    constexpr int kHalfRoiW = 460;
    constexpr int kHalfRoiH = 360;
    const int roiX0 = std::max<int>(limit.left, localCursor.x - kHalfRoiW);
    const int roiY0 = std::max<int>(limit.top, localCursor.y - kHalfRoiH);
    const int roiX1 = std::min<int>(limit.right, localCursor.x + kHalfRoiW);
    const int roiY1 = std::min<int>(limit.bottom, localCursor.y + kHalfRoiH);
    const int roiW = roiX1 - roiX0;
    const int roiH = roiY1 - roiY0;
    if (roiW < 64 || roiH < 64) return false;

    const cv::Mat roi = frozenScreen(cv::Rect(roiX0, roiY0, roiW, roiH));
    cv::Mat gray;
    if (roi.channels() == 4) {
        cv::cvtColor(roi, gray, cv::COLOR_BGRA2GRAY);
    } else if (roi.channels() == 3) {
        cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = roi;
    }

    // 超大 ROI 降采样至 <= 480px，提速 4x 且抑制像素噪声
    double scale = 1.0;
    const int maxDim = std::max(roiW, roiH);
    cv::Mat workGray = gray;
    if (maxDim > 480) {
        scale = 480.0 / static_cast<double>(maxDim);
        cv::resize(gray, workGray, cv::Size(), scale, scale, cv::INTER_AREA);
    }

    cv::Mat blurred;
    cv::GaussianBlur(workGray, blurred, cv::Size(3, 3), 0.8);

    // 双通道边缘融合：标准低阈值 Canny + 闭合形态学连接 1~2px 圆角/抗锯齿断口
    cv::Mat edges;
    cv::Canny(blurred, edges, 28.0, 88.0, 3, true);
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(edges, edges, cv::MORPH_CLOSE, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(edges, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) return false;

    const double curX = (localCursor.x - roiX0) * scale;
    const double curY = (localCursor.y - roiY0) * scale;
    const double limitAreaScaled = static_cast<double>(limitW) * static_cast<double>(limitH) * scale * scale;
    const double minSideScaled = std::max(14.0, 24.0 * scale);

    bool found = false;
    double bestArea = 1e18;
    cv::Rect bestBox{};

    for (const auto& contour : contours) {
        if (contour.size() < 4) continue;
        const cv::Rect box = cv::boundingRect(contour);
        if (box.width < minSideScaled || box.height < minSideScaled) continue;
        if (curX < box.x || curX > box.x + box.width || curY < box.y || curY > box.y + box.height) {
            continue;
        }

        const double boxArea = static_cast<double>(box.width) * static_cast<double>(box.height);
        if (boxArea >= limitAreaScaled * 0.82) continue; // 过接近整个容器无增量价值

        // 形状矩形度校验（过滤不规则文字块、图标碎片与杂乱自然景物）
        const double contourArea = std::abs(cv::contourArea(contour));
        if (contourArea <= 0.0) continue;
        const double rectangularity = contourArea / boxArea;
        if (rectangularity < 0.84) continue;

        // 轮廓点周长紧凑度校验（确保接近真实矩形边框而非锯齿迷宫）
        const double perimeter = cv::arcLength(contour, true);
        const double expectedPerimeter = 2.0 * (box.width + box.height);
        if (perimeter <= 0.0 || perimeter > expectedPerimeter * 1.22) continue;

        if (boxArea < bestArea) {
            bestArea = boxArea;
            bestBox = box;
            found = true;
        }
    }

    if (!found) return false;

    const double invScale = 1.0 / scale;
    RECT mapped{};
    mapped.left = roiX0 + static_cast<LONG>(std::lround(bestBox.x * invScale));
    mapped.top = roiY0 + static_cast<LONG>(std::lround(bestBox.y * invScale));
    mapped.right = roiX0 + static_cast<LONG>(std::lround((bestBox.x + bestBox.width) * invScale));
    mapped.bottom = roiY0 + static_cast<LONG>(std::lround((bestBox.y + bestBox.height) * invScale));
    mapped = clampRectToBounds(mapped, frozenScreen.cols, frozenScreen.rows);

    if (rectWidth(mapped) < 24 || rectHeight(mapped) < 24) return false;
    if (!PtInRect(&mapped, localCursor)) return false;

    outCardRect = mapped;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// WindowElementDetector 生命周期与会话管理
// ─────────────────────────────────────────────────────────────────────────────

WindowElementDetector::WindowElementDetector() = default;

WindowElementDetector::~WindowElementDetector() {
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_shutdown = true;
        m_hasPendingRequest = false;
        m_clearCacheRequested = true;
    }
    m_workerCv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void WindowElementDetector::ensureWorkerStarted() {
    std::lock_guard<std::mutex> lock(m_workerMutex);
    if (m_workerRunning) return;
    m_shutdown = false;
    m_workerRunning = true;
    m_workerThread = std::thread(&WindowElementDetector::workerLoop, this);
}

void WindowElementDetector::beginSession(
    HWND excludeOverlayHwnd,
    int virtualOriginX,
    int virtualOriginY,
    int screenWidth,
    int screenHeight,
    const cv::Mat* frozenScreen) {
    m_generation.fetch_add(1, std::memory_order_acq_rel);

    m_excludeHwnd = excludeOverlayHwnd;
    m_originX = virtualOriginX;
    m_originY = virtualOriginY;
    m_screenW = screenWidth;
    m_screenH = screenHeight;
    m_windows.clear();

    {
        std::lock_guard<std::mutex> lock(m_frozenMutex);
        if (frozenScreen && !frozenScreen->empty()) {
            m_frozenCopy = *frozenScreen; // cv::Mat 引用计数浅拷贝，零像素复制开销
        } else {
            m_frozenCopy.release();
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_hasReadyResult = false;
        m_readyResult = {};
    }
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_hasPendingRequest = false;
        m_clearCacheRequested = true;
    }
    m_workerCv.notify_one();

    // 维度 1：单次预采集全桌面可见顶层窗口 Z-Order 拓扑快照（冻屏时刻窗口完全静止）
    const bool isZh = isUiLangZh();
    struct EnumCtx {
        HWND exclude;
        int originX;
        int originY;
        int screenW;
        int screenH;
        bool isZh;
        std::vector<WindowSnapshot>* out;
    } ctx{excludeOverlayHwnd, virtualOriginX, virtualOriginY, screenWidth, screenHeight, isZh, &m_windows};

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lParam);
        if (hwnd == c->exclude || IsIconic(hwnd) ||
            !tools3000::core::WinUtils::isWindowVisibleOnCurrentDesktop(hwnd)) {
            return TRUE;
        }

        // 跳过 DWM Cloaked（挂起 UWP / 非当前虚拟桌面窗）
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
            return TRUE;
        }

        // 跳过全透明鼠标穿透层及 alpha == 0 的隐形幽灵窗
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((exStyle & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) == (WS_EX_LAYERED | WS_EX_TRANSPARENT)) {
            return TRUE;
        }
        if ((exStyle & WS_EX_LAYERED) != 0) {
            BYTE alpha = 255;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) &&
                (flags & LWA_ALPHA) != 0 && alpha == 0) {
                return TRUE;
            }
        }

        RECT frameScreen{};
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameScreen, sizeof(frameScreen)))) {
            if (!GetWindowRect(hwnd, &frameScreen)) return TRUE;
        }
        if (rectWidth(frameScreen) <= kMinCandidateEdgePx || rectHeight(frameScreen) <= kMinCandidateEdgePx) {
            return TRUE;
        }

        wchar_t clsBuf[96] = {};
        GetClassNameW(hwnd, clsBuf, 96);
        std::wstring cls(clsBuf);
        if (isIgnoredTopLevelClass(cls, hwnd, frameScreen)) {
            return TRUE;
        }

        WindowSnapshot snap;
        snap.hwnd = hwnd;
        snap.screenFrameRect = frameScreen;
        snap.className = std::move(cls);
        snap.isTaskbar = (snap.className == L"Shell_TrayWnd" || snap.className == L"Shell_SecondaryTrayWnd");

        RECT localFrame{
            frameScreen.left - c->originX,
            frameScreen.top - c->originY,
            frameScreen.right - c->originX,
            frameScreen.bottom - c->originY
        };
        if (c->screenW > 0 && c->screenH > 0) {
            localFrame = clampRectToBounds(localFrame, c->screenW, c->screenH);
        }
        if (rectWidth(localFrame) <= kMinCandidateEdgePx || rectHeight(localFrame) <= kMinCandidateEdgePx) {
            return TRUE;
        }
        snap.localFrameRect = localFrame;

        wchar_t titleBuf[128] = {};
        GetWindowTextW(hwnd, titleBuf, 128);
        snap.title = sanitizeLabelText(titleBuf, 14);
        snap.frameCornerRadius = detectWindowCornerRadius(hwnd, frameScreen, snap.isTaskbar);

        const std::wstring frameType = snap.isTaskbar
            ? tools3000::core::trWCstrFor(c->isZh, L"工具栏", L"Toolbar")
            : tools3000::core::trWCstrFor(c->isZh, L"窗口", L"Window");
        snap.frameDescription = formatCandidateDesc(frameType, snap.title, snap.localFrameRect);

        // 采集精确内容客户区（剔除标题栏与外框，差值 > 6px 才作为独立层级）
        RECT clientRc{};
        if (!snap.isTaskbar && GetClientRect(hwnd, &clientRc) &&
            rectWidth(clientRc) > kMinCandidateEdgePx && rectHeight(clientRc) > kMinCandidateEdgePx) {
            POINT tl{clientRc.left, clientRc.top};
            POINT br{clientRc.right, clientRc.bottom};
            if (ClientToScreen(hwnd, &tl) && ClientToScreen(hwnd, &br)) {
                RECT localClient{
                    std::max(localFrame.left, tl.x - c->originX),
                    std::max(localFrame.top, tl.y - c->originY),
                    std::min(localFrame.right, br.x - c->originX),
                    std::min(localFrame.bottom, br.y - c->originY)
                };
                if (rectWidth(localClient) > kMinCandidateEdgePx &&
                    rectHeight(localClient) > kMinCandidateEdgePx &&
                    !isNearlySameRect(localClient, localFrame, 6)) {
                    snap.hasDistinctClient = true;
                    snap.localClientRect = localClient;
                    snap.clientDescription = formatCandidateDesc(
                        tools3000::core::trWCstrFor(c->isZh, L"控件", L"Control"),
                        snap.title,
                        localClient);
                }
            }
        }

        c->out->push_back(std::move(snap));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

void WindowElementDetector::endSession() {
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    m_windows.clear();
    {
        std::lock_guard<std::mutex> lock(m_frozenMutex);
        m_frozenCopy.release();
    }
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_hasReadyResult = false;
        m_readyResult = {};
    }
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_hasPendingRequest = false;
        m_clearCacheRequested = true;
    }
    m_workerCv.notify_one();
}

// ─────────────────────────────────────────────────────────────────────────────
// 维度 1：零卡顿同步命中查询（含懒扫描深层子 HWND）
// ─────────────────────────────────────────────────────────────────────────────

void WindowElementDetector::scanChildHwndsForWindow(WindowSnapshot& win) {
    if (win.childrenScanned || !win.hwnd || !IsWindow(win.hwnd)) return;
    win.childrenScanned = true;

    const bool isZh = isUiLangZh();
    struct ChildCtx {
        int originX;
        int originY;
        RECT parentLocal;
        bool isZh;
        std::vector<CachedChildHwnd>* out;
    } ctx{m_originX, m_originY, win.localFrameRect, isZh, &win.children};

    EnumChildWindows(win.hwnd, [](HWND child, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<ChildCtx*>(lParam);
        if (c->out->size() >= 180) return FALSE; // 熔断保护，防止老式 MDI 子窗数量爆炸
        if (!IsWindowVisible(child)) return TRUE;

        const LONG_PTR exStyle = GetWindowLongPtrW(child, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TRANSPARENT) != 0) return TRUE;

        RECT rc{};
        if (!GetWindowRect(child, &rc)) return TRUE;
        RECT local{
            std::max(c->parentLocal.left, rc.left - c->originX),
            std::max(c->parentLocal.top, rc.top - c->originY),
            std::min(c->parentLocal.right, rc.right - c->originX),
            std::min(c->parentLocal.bottom, rc.bottom - c->originY)
        };
        if (rectWidth(local) <= kMinCandidateEdgePx || rectHeight(local) <= kMinCandidateEdgePx) {
            return TRUE;
        }

        wchar_t clsBuf[64] = {};
        GetClassNameW(child, clsBuf, 64);
        wchar_t titleBuf[64] = {};
        GetWindowTextW(child, titleBuf, 64);

        CachedChildHwnd entry;
        entry.hwnd = child;
        entry.localRect = local;
        const std::wstring typeLabel = classifyWin32ClassName(clsBuf, false, c->isZh);
        entry.description = formatCandidateDesc(typeLabel, sanitizeLabelText(titleBuf, 12), local);
        entry.cornerRadius = (std::min(rectWidth(local), rectHeight(local)) <= 64) ? 4.0f : 5.0f;
        c->out->push_back(std::move(entry));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

std::vector<DetectedElementCandidate> WindowElementDetector::querySync(POINT screenPt, HWND* outTopHwnd) {
    if (outTopHwnd) *outTopHwnd = nullptr;
    const POINT localPt{screenPt.x - m_originX, screenPt.y - m_originY};
    std::vector<DetectedElementCandidate> raw;
    raw.reserve(12);

    // 1. 按预采集 Z-Order 顺序 O(N) 命中光标下的最顶层真实可见窗口
    WindowSnapshot* hitWin = nullptr;
    for (auto& win : m_windows) {
        if (PtInRect(&win.localFrameRect, localPt)) {
            hitWin = &win;
            break;
        }
    }

    if (hitWin) {
        if (outTopHwnd) *outTopHwnd = hitWin->hwnd;

        // 懒扫描该窗口下的深层可见子 HWND（每窗口整个会话只扫 1 次）
        scanChildHwndsForWindow(*hitWin);
        for (const auto& child : hitWin->children) {
            if (PtInRect(&child.localRect, localPt)) {
                DetectedElementCandidate cand;
                cand.rect = child.localRect;
                cand.description = child.description;
                cand.cornerRadius = child.cornerRadius;
                cand.kind = DetectedCandidateKind::Win32Child;
                cand.zPriority = defaultPriorityForKind(cand.kind);
                raw.push_back(std::move(cand));
            }
        }

        // 窗口内容客户区
        if (hitWin->hasDistinctClient && PtInRect(&hitWin->localClientRect, localPt)) {
            DetectedElementCandidate cand;
            cand.rect = hitWin->localClientRect;
            cand.description = hitWin->clientDescription;
            cand.cornerRadius = 0.0f;
            cand.kind = DetectedCandidateKind::WindowClient;
            cand.zPriority = defaultPriorityForKind(cand.kind);
            raw.push_back(std::move(cand));
        }

        // 顶层窗口外框
        {
            DetectedElementCandidate cand;
            cand.rect = hitWin->localFrameRect;
            cand.description = hitWin->frameDescription;
            cand.cornerRadius = hitWin->frameCornerRadius;
            cand.kind = DetectedCandidateKind::WindowFrame;
            cand.zPriority = defaultPriorityForKind(cand.kind);
            raw.push_back(std::move(cand));
        }
    }

    // 2. 追加光标所在的单块显示器
    const bool isZh = isUiLangZh();
    if (HMONITOR hMon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(hMon, &mi)) {
            RECT monRc{
                mi.rcMonitor.left - m_originX,
                mi.rcMonitor.top - m_originY,
                mi.rcMonitor.right - m_originX,
                mi.rcMonitor.bottom - m_originY
            };
            DetectedElementCandidate cand;
            cand.rect = monRc;
            cand.description =
                std::wstring(tools3000::core::trWCstrFor(isZh, L"屏幕 (", L"Display (")) +
                std::to_wstring(rectWidth(monRc)) + L"x" + std::to_wstring(rectHeight(monRc)) + L")";
            cand.cornerRadius = 0.0f;
            cand.kind = DetectedCandidateKind::MonitorDisplay;
            cand.zPriority = defaultPriorityForKind(cand.kind);
            raw.push_back(std::move(cand));
        }
    }

    // 3. 兜底虚拟全屏
    if (raw.empty() && m_screenW > 0 && m_screenH > 0) {
        DetectedElementCandidate cand;
        cand.rect = RECT{0, 0, m_screenW, m_screenH};
        cand.description =
            std::wstring(tools3000::core::trWCstrFor(isZh, L"全屏 (", L"Full Screen (")) +
            std::to_wstring(m_screenW) + L"x" + std::to_wstring(m_screenH) + L")";
        cand.cornerRadius = 0.0f;
        cand.kind = DetectedCandidateKind::VirtualDesktop;
        cand.zPriority = defaultPriorityForKind(cand.kind);
        raw.push_back(std::move(cand));
    }

    return normalizeDetectedHierarchy(std::move(raw), m_screenW, m_screenH);
}

// ─────────────────────────────────────────────────────────────────────────────
// 维度 2 + 维度 3：后台异步懒缓存空间剪枝 UIA + 视觉卡片补全
// ─────────────────────────────────────────────────────────────────────────────

uint64_t WindowElementDetector::requestAsyncRefine(
    HWND notifyHwnd,
    POINT screenPt,
    HWND topHwnd,
    const std::vector<DetectedElementCandidate>& baseSyncCandidates) {
    ensureWorkerStarted();
    const uint64_t gen = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;

    RECT topLocalBounds{0, 0, m_screenW, m_screenH};
    for (const auto& win : m_windows) {
        if (win.hwnd == topHwnd) {
            topLocalBounds = win.localFrameRect;
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_pendingRequest.generation = gen;
        m_pendingRequest.notifyHwnd = notifyHwnd;
        m_pendingRequest.topHwnd = topHwnd;
        m_pendingRequest.screenPt = screenPt;
        m_pendingRequest.topLocalBounds = topLocalBounds;
        m_pendingRequest.baseSyncCandidates = baseSyncCandidates;
        m_hasPendingRequest = true;
    }
    m_workerCv.notify_one();
    return gen;
}

bool WindowElementDetector::consumeRefinedResult(AsyncRefinedHierarchyResult& outResult) {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    if (!m_hasReadyResult) return false;
    m_hasReadyResult = false;
    if (m_readyResult.generation != m_generation.load(std::memory_order_acquire)) {
        return false;
    }
    outResult = std::move(m_readyResult);
    return !outResult.candidates.empty();
}

void WindowElementDetector::workerLoop() {
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IUIAutomation> uia;
    ComPtr<IUIAutomationCondition> controlCond;
    ComPtr<IUIAutomationCacheRequest> childCacheReq;

    if (SUCCEEDED(hrCom) || hrCom == RPC_E_CHANGED_MODE) {
        HRESULT hrUia = CoCreateInstance(
            __uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(uia.GetAddressOf()));
        if (FAILED(hrUia) || !uia) {
            hrUia = CoCreateInstance(
                __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IUIAutomation), reinterpret_cast<void**>(uia.GetAddressOf()));
        }
        if (SUCCEEDED(hrUia) && uia) {
            // UIA2 防卡死超时保护：任何无响应目标窗口 60ms 内硬超时返回
            ComPtr<IUIAutomation2> uia2;
            if (SUCCEEDED(uia.As(&uia2)) && uia2) {
                uia2->put_ConnectionTimeout(40);
                uia2->put_TransactionTimeout(60);
            }
            uia->get_ControlViewCondition(controlCond.GetAddressOf());
            if (SUCCEEDED(uia->CreateCacheRequest(childCacheReq.GetAddressOf())) && childCacheReq) {
                childCacheReq->AddProperty(UIA_BoundingRectanglePropertyId);
                childCacheReq->AddProperty(UIA_ControlTypePropertyId);
                childCacheReq->AddProperty(UIA_NamePropertyId);
                childCacheReq->AddProperty(UIA_IsOffscreenPropertyId);
                childCacheReq->put_TreeScope(TreeScope_Element);
            }
        }
    }

    // 会话级懒缓存 UIA 树（完全属于本后台线程，零跨线程 COM 调用）
    std::unordered_map<HWND, std::unique_ptr<UiaCachedNode>> uiaTreeCache;

    auto loadChildrenIfNeeded = [&](UiaCachedNode& node) {
        if (node.childrenLoaded || !node.element || !uia || !controlCond || !childCacheReq) {
            node.childrenLoaded = true;
            return;
        }
        node.childrenLoaded = true;

        ComPtr<IUIAutomationElementArray> arr;
        const HRESULT hr = node.element->FindAllBuildCache(
            TreeScope_Children, controlCond.Get(), childCacheReq.Get(), arr.GetAddressOf());
        if (FAILED(hr) || !arr) return;

        int count = 0;
        if (FAILED(arr->get_Length(&count)) || count <= 0) return;
        const int capped = std::min(count, kMaxUiaChildrenPerNode);
        node.children.reserve(static_cast<std::size_t>(capped));

        for (int i = 0; i < capped; ++i) {
            ComPtr<IUIAutomationElement> childEl;
            if (FAILED(arr->GetElement(i, childEl.GetAddressOf())) || !childEl) continue;

            BOOL isOffscreen = FALSE;
            if (SUCCEEDED(childEl->get_CachedIsOffscreen(&isOffscreen)) && isOffscreen) {
                continue;
            }

            RECT brc{};
            if (FAILED(childEl->get_CachedBoundingRectangle(&brc))) continue;
            if (rectWidth(brc) <= 4 || rectHeight(brc) <= 4) continue;

            auto childNode = std::make_unique<UiaCachedNode>();
            childNode->element = std::move(childEl);
            childNode->screenRect = brc;
            childNode->element->get_CachedControlType(&childNode->controlType);

            BSTR bstrName = nullptr;
            if (SUCCEEDED(childNode->element->get_CachedName(&bstrName)) && bstrName) {
                childNode->name = sanitizeLabelText(std::wstring(bstrName, SysStringLen(bstrName)), 14);
                SysFreeString(bstrName);
            }
            node.children.push_back(std::move(childNode));
        }
    };

    while (true) {
        AsyncRequest req{};
        bool clearCache = false;
        bool hasReq = false;

        {
            std::unique_lock<std::mutex> lock(m_workerMutex);
            m_workerCv.wait(lock, [&]() {
                return m_shutdown || m_clearCacheRequested || m_hasPendingRequest;
            });
            if (m_shutdown) break;
            if (m_clearCacheRequested) {
                clearCache = true;
                m_clearCacheRequested = false;
            }
            if (m_hasPendingRequest) {
                req = std::move(m_pendingRequest);
                m_hasPendingRequest = false;
                hasReq = true;
            }
        }

        if (clearCache) {
            uiaTreeCache.clear();
        }
        if (!hasReq) continue;
        if (req.generation != m_generation.load(std::memory_order_acquire)) continue;

        std::vector<DetectedElementCandidate> merged = req.baseSyncCandidates;
        const bool isZh = isUiLangZh();
        const int originX = m_originX;
        const int originY = m_originY;
        const int screenW = m_screenW;
        const int screenH = m_screenH;

        // 维度 2：自顶向下空间剪枝 + 冻结画布懒缓存 UIA 探测（彻底免疫顶层遮罩）
        if (uia && req.topHwnd && IsWindow(req.topHwnd)) {
            const auto startClock = std::chrono::steady_clock::now();
            auto itRoot = uiaTreeCache.find(req.topHwnd);
            if (itRoot == uiaTreeCache.end()) {
                ComPtr<IUIAutomationElement> rootEl;
                if (SUCCEEDED(uia->ElementFromHandle(req.topHwnd, rootEl.GetAddressOf())) && rootEl) {
                    auto rootNode = std::make_unique<UiaCachedNode>();
                    rootNode->element = std::move(rootEl);
                    rootNode->element->get_CurrentBoundingRectangle(&rootNode->screenRect);
                    rootNode->element->get_CurrentControlType(&rootNode->controlType);
                    itRoot = uiaTreeCache.emplace(req.topHwnd, std::move(rootNode)).first;
                }
            }

            if (itRoot != uiaTreeCache.end() && itRoot->second) {
                UiaCachedNode* current = itRoot->second.get();
                std::vector<const UiaCachedNode*> hitPath;
                hitPath.reserve(kMaxUiaDepth);

                for (int depth = 0; depth < kMaxUiaDepth; ++depth) {
                    if (req.generation != m_generation.load(std::memory_order_acquire)) break;
                    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - startClock).count();
                    if (elapsedMs >= kUiaTimeBudgetMs) break;

                    loadChildrenIfNeeded(*current);
                    if (current->children.empty()) break;

                    // 寻找包含光标的最具体（面积最小；面积近似时优先 Z 序靠后的）直接子节点
                    UiaCachedNode* bestChild = nullptr;
                    int64_t bestArea = INT64_MAX;
                    for (const auto& childPtr : current->children) {
                        if (!childPtr) continue;
                        if (!PtInRect(&childPtr->screenRect, req.screenPt)) continue;
                        const int64_t area = rectArea(childPtr->screenRect);
                        if (area <= 0) continue;
                        if (!bestChild || area <= bestArea) {
                            bestArea = area;
                            bestChild = childPtr.get();
                        }
                    }

                    if (!bestChild) break;
                    hitPath.push_back(bestChild);
                    current = bestChild;
                }

                for (const UiaCachedNode* node : hitPath) {
                    RECT local{
                        std::max(req.topLocalBounds.left, node->screenRect.left - originX),
                        std::max(req.topLocalBounds.top, node->screenRect.top - originY),
                        std::min(req.topLocalBounds.right, node->screenRect.right - originX),
                        std::min(req.topLocalBounds.bottom, node->screenRect.bottom - originY)
                    };
                    if (rectWidth(local) <= kMinCandidateEdgePx || rectHeight(local) <= kMinCandidateEdgePx) {
                        continue;
                    }
                    DetectedElementCandidate cand;
                    cand.rect = local;
                    cand.kind = DetectedCandidateKind::UiaElement;
                    cand.zPriority = defaultPriorityForKind(cand.kind);
                    cand.cornerRadius = cornerRadiusForUiaControl(local, node->controlType);
                    cand.description = formatCandidateDesc(
                        uiaControlTypeLabel(node->controlType, isZh),
                        node->name,
                        local);
                    merged.push_back(std::move(cand));
                }
            }
        }

        if (req.generation != m_generation.load(std::memory_order_acquire)) continue;

        auto normalized = normalizeDetectedHierarchy(std::move(merged), screenW, screenH);

        // 维度 3：无障碍盲区视觉卡片补全（仅当最内层候选仍面积巨大且非纯小控件时运行）
        if (!normalized.empty()) {
            const auto& innermost = normalized.front();
            const bool isLargeBlankArea =
                rectWidth(innermost.rect) >= kVisionTriggerMinW &&
                rectHeight(innermost.rect) >= kVisionTriggerMinH &&
                innermost.kind != DetectedCandidateKind::MonitorDisplay &&
                innermost.kind != DetectedCandidateKind::VirtualDesktop;
            if (isLargeBlankArea) {
                cv::Mat frozenRef;
                {
                    std::lock_guard<std::mutex> lock(m_frozenMutex);
                    frozenRef = m_frozenCopy;
                }
                if (!frozenRef.empty()) {
                    const POINT localPt{req.screenPt.x - originX, req.screenPt.y - originY};
                    RECT cardRect{};
                    if (detectVisualCardRegionAtPoint(frozenRef, localPt, innermost.rect, cardRect)) {
                        DetectedElementCandidate visCand;
                        visCand.rect = cardRect;
                        visCand.kind = DetectedCandidateKind::VisionRegion;
                        visCand.zPriority = defaultPriorityForKind(visCand.kind);
                        visCand.cornerRadius = 4.0f;
                        visCand.description = formatCandidateDesc(
                            tools3000::core::trWCstrFor(isZh, L"控件", L"Control"),
                            L"",
                            cardRect);
                        normalized.push_back(std::move(visCand));
                        normalized = normalizeDetectedHierarchy(std::move(normalized), screenW, screenH);
                    }
                }
            }
        }

        if (req.generation != m_generation.load(std::memory_order_acquire)) continue;

        {
            std::lock_guard<std::mutex> lock(m_resultMutex);
            m_readyResult.generation = req.generation;
            m_readyResult.queryScreenPt = req.screenPt;
            m_readyResult.candidates = std::move(normalized);
            m_hasReadyResult = true;
        }

        if (req.notifyHwnd && IsWindow(req.notifyHwnd)) {
            PostMessageW(req.notifyHwnd, WM_CAPTURE_ELEMENT_REFINED, 0, static_cast<LPARAM>(req.generation));
        }
    }

    uiaTreeCache.clear();
    childCacheReq.Reset();
    controlCond.Reset();
    uia.Reset();
    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 辅助函数：动态圆角探测与分类描述
// ─────────────────────────────────────────────────────────────────────────────

float WindowElementDetector::detectWindowCornerRadius(HWND hwnd, const RECT& screenFrame, bool isTaskbar) {
    if (isTaskbar || !hwnd || IsZoomed(hwnd)) return 0.0f;

    // 贴满当前显示器或工作区边缘时不套假圆角
    if (HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(hMon, &mi)) {
            const bool fillsMonitor =
                screenFrame.left <= mi.rcMonitor.left + 2 &&
                screenFrame.top <= mi.rcMonitor.top + 2 &&
                screenFrame.right >= mi.rcMonitor.right - 2 &&
                screenFrame.bottom >= mi.rcMonitor.bottom - 2;
            const bool fillsWork =
                screenFrame.left <= mi.rcWork.left + 2 &&
                screenFrame.top <= mi.rcWork.top + 2 &&
                screenFrame.right >= mi.rcWork.right - 2 &&
                screenFrame.bottom >= mi.rcWork.bottom - 2;
            if (fillsMonitor || fillsWork) return 0.0f;
        }
    }

    DWORD cornerPref = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref)))) {
        if (cornerPref == 1 /* DWMWCP_DONOTROUND */) return 0.0f;
        if (cornerPref == 3 /* DWMWCP_ROUNDSMALL */) return 4.0f;
        if (cornerPref == 2 /* DWMWCP_ROUND */) return 8.0f;
    }

    if (!isWindows11OrNewer()) return 0.0f;

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) != 0 || (style & WS_THICKFRAME) != 0) {
        return 8.0f;
    }
    return 6.0f;
}

std::wstring WindowElementDetector::classifyWin32ClassName(
    const std::wstring& className,
    bool isTopLevel,
    bool isZh) {
    if (isTopLevel) {
        return tools3000::core::trWCstrFor(isZh, L"窗口", L"Window");
    }
    std::wstring cls = className;
    for (auto& ch : cls) ch = static_cast<wchar_t>(std::towlower(ch));

    if (cls.find(L"button") != std::wstring::npos) return tools3000::core::trWCstrFor(isZh, L"按钮", L"Button");
    if (cls.find(L"edit") != std::wstring::npos) return tools3000::core::trWCstrFor(isZh, L"输入框", L"Input");
    if (cls.find(L"static") != std::wstring::npos) return tools3000::core::trWCstrFor(isZh, L"文本", L"Label");
    if (cls.find(L"combo") != std::wstring::npos) return tools3000::core::trWCstrFor(isZh, L"下拉框", L"ComboBox");
    if (cls.find(L"list") != std::wstring::npos) return tools3000::core::trWCstrFor(isZh, L"列表", L"List");
    if (cls.find(L"tree") != std::wstring::npos) return tools3000::core::trWCstrFor(isZh, L"树形控件", L"TreeView");
    if (cls.find(L"tab") != std::wstring::npos) return tools3000::core::trWCstrFor(isZh, L"选项卡", L"Tab");
    if (cls.find(L"toolbar") != std::wstring::npos) return tools3000::core::trWCstrFor(isZh, L"工具栏", L"Toolbar");
    return tools3000::core::trWCstrFor(isZh, L"控件", L"Control");
}

std::wstring WindowElementDetector::formatCandidateDesc(
    const std::wstring& typeLabel,
    const std::wstring& title,
    const RECT& rc) {
    std::wstring desc = typeLabel;
    if (!title.empty()) {
        desc += L": " + title;
    }
    desc += L" (" + std::to_wstring(rectWidth(rc)) + L"x" + std::to_wstring(rectHeight(rc)) + L")";
    return desc;
}

} // namespace tools3000::capture
