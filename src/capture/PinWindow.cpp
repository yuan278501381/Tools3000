// ─────────────────────────────────────────────────────────────────────────────
// PinWindow.cpp — 贴图窗口实现
//
// 功能:
//   - 截图后按 "钉住" 将截图贴在屏幕上
//   - 左键拖拽移动, 滚轮缩放, 双击关闭
//   - 支持透明度调节 (右键菜单)
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinWindow.h"
#include "capture/ScreenCapture.h"
#include "capture/ClipboardUtils.h"
#include "capture/CaptureOverlay.h"
#include "capture/CaptureVectorIcons.h"
#include "capture/FloatingGlassBar.h"
#include "capture/CaptureHistory.h"
#include "capture/PinSpawnCalculator.h"
#include "capture/PinPasteCoordinator.h"
#include "capture/PinInputController.h"
#include "capture/PinGroupManager.h"
#include "capture/PinGroupBarOverlay.h"
#include "core/events/EventBus.h"
#include "core/ipc/MessageBridge.h"
#include "core/logger/Logger.h"
#include "core/config/ConfigManager.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/utils/TraceId.h"
#include "ocr/OcrEngine.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <d2d1_1.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <windowsx.h>

namespace tools3000::capture {

using namespace Microsoft::WRL;

static constexpr const wchar_t* PIN_CLASS = L"Tools3000_PinWindow";

// 静态成员
std::vector<std::shared_ptr<PinWindow>> PinWindow::s_instances;
bool PinWindow::s_classRegistered = false;
bool PinWindow::s_allHidden = false;
int PinWindow::s_activeGroupFilter = PinGroupConstants::kFilterAllGroups;
std::array<bool, PinGroupConstants::kMaxGroupId + 1> PinWindow::s_createdGroups{};

namespace {
std::atomic<bool> s_ocrShuttingDown{false};
std::atomic<uint32_t> s_globalActiveOcrWorkers{0};
} // namespace

void PinWindow::resetModuleShutdown() noexcept {
    s_ocrShuttingDown.store(false, std::memory_order_release);
}

void PinWindow::beginModuleShutdown() noexcept {
    s_ocrShuttingDown.store(true, std::memory_order_release);
}

uint32_t PinWindow::globalActiveOcrWorkerCount() noexcept {
    return s_globalActiveOcrWorkers.load(std::memory_order_acquire);
}

bool PinWindow::waitForPendingOcrWorkers(uint32_t timeoutMs) noexcept {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (s_globalActiveOcrWorkers.load(std::memory_order_acquire) > 0) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE)) {
        }
        if (GetTickCount64() >= deadline) {
            return s_globalActiveOcrWorkers.load(std::memory_order_acquire) == 0;
        }
        Sleep(2);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 创建 / 销毁
// ─────────────────────────────────────────────────────────────────────────────

std::shared_ptr<PinWindow> PinWindow::create(const cv::Mat& image, int x, int y,
                                             float cardCornerRadius,
                                             std::wstring sourceText,
                                             const cv::Mat& cleanBaseImage,
                                             std::vector<MarkupElement> markupElements) {
    if (image.empty()) return nullptr;

    auto pin = std::shared_ptr<PinWindow>(new PinWindow());
    pin->m_origWidth = image.cols;
    pin->m_origHeight = image.rows;
    pin->m_sourceImage = image.clone();  // 保存原图，供右键"复制到剪贴板"
    if (!cleanBaseImage.empty() &&
        cleanBaseImage.cols == image.cols &&
        cleanBaseImage.rows == image.rows) {
        pin->m_cleanBaseImage = cleanBaseImage.clone();
        pin->m_markupElements = std::move(markupElements);
    } else {
        pin->m_cleanBaseImage = image.clone();
        pin->m_markupElements.clear();
    }
    pin->m_cardCornerRadius = (std::max)(0.0f, cardCornerRadius);
    pin->m_pinnedSourceText = std::move(sourceText);
    if (s_activeGroupFilter >= PinGroupConstants::kMinGroupId &&
        s_activeGroupFilter <= PinGroupConstants::kMaxGroupId) {
        pin->m_groupId = s_activeGroupFilter;
    }

    if (!pin->initWindow(GetModuleHandleW(nullptr), x, y, image.cols, image.rows)) {
        LOG_ERROR("贴图窗口创建失败");
        return nullptr;
    }

    pin->updateWindowCornerRegion();

    if (!pin->createRenderResources(image)) {
        LOG_ERROR("贴图窗口渲染资源创建失败");
        return nullptr;
    }

    if (!pin->m_pinnedSourceText.empty()) {
        // 微晶字签/色卡签直接内嵌零误差原始纯文本块，免启后台 OCR 线程消耗 CPU
        std::lock_guard<std::mutex> lock(pin->m_ocrMutex);
        pin->m_ocrBlocks.clear();
        pin->m_ocrBlocks.push_back(PinOcrBlock{
            tools3000::core::WinUtils::wstringToUtf8(pin->m_pinnedSourceText),
            cv::Rect(4, 4, (std::max)(1, image.cols - 8), (std::max)(1, image.rows - 8))
        });
        pin->m_ocrReady = true;
    }

    ShowWindow(pin->m_hwnd, SW_SHOW);
    pin->focusWindow();
    pin->render();

    s_instances.push_back(pin);
    s_allHidden = false;
    refreshGroupBar();
    if (pin->m_pinnedSourceText.empty()) {
        // 常规贴图保持零后台线程与零矩阵克隆开销，仅在用户触发选字/Alt 悬停/Ctrl+A 时按需启动 OCR
    }
    LOG_INFO("贴图窗口已创建: {}x{} @ ({},{}), 总数={}", image.cols, image.rows, x, y, s_instances.size());

    return pin;
}

void PinWindow::releaseGraphicsResources() noexcept {
    m_bitmapTiles.clear();
    m_bitmap.Reset();
    m_renderTarget.Reset();
    m_uiLeadTextFormat.Reset();
    m_uiCenterTextFormat.Reset();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
}

void PinWindow::close() {
    // 保活: 下面的 erase 会丢弃 s_instances 中指向自身的最后一个 shared_ptr，
    // 若不先持有自身引用，this 会在本函数执行途中被析构 (use-after-free)。
    // close() 通常从 pinWndProc (双击/右键) 内被调用，UAF 会偶发崩溃。
    std::shared_ptr<PinWindow> keepAlive;
    try {
        keepAlive = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        // 理论上不会发生 (实例总由 create() 用 shared_ptr 持有)
    }

    const int closedGroup = m_groupId;
    releaseGraphicsResources();
    HWND hwndToDestroy = std::exchange(m_hwnd, nullptr);
    m_sourceImage.release();
    m_cleanBaseImage.release();
    m_markupElements.clear();
    if (hwndToDestroy && IsWindow(hwndToDestroy)) {
        DestroyWindow(hwndToDestroy);
    }
    // 从全局列表移除已失效的实例
    s_instances.erase(
        std::remove_if(s_instances.begin(), s_instances.end(),
                       [](const auto& p) { return !p || !p->isAlive(); }),
        s_instances.end()
    );
    if (s_instances.empty()) {
        s_activeGroupFilter = PinGroupConstants::kFilterAllGroups;
        PinGroupBarOverlay::instance().shutdown();
        broadcastGroupStateChanged();
        tools3000::core::WinUtils::trimWorkingSet();
    } else {
        if (closedGroup >= PinGroupConstants::kMinGroupId &&
            s_activeGroupFilter == closedGroup &&
            countInGroup(closedGroup) == 0) {
            setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
        } else {
            refreshGroupBar();
        }
    }
}

void PinWindow::closeAll() {
    std::vector<std::shared_ptr<PinWindow>> targets = std::move(s_instances);
    s_instances.clear();
    for (auto& pin : targets) {
        if (pin) {
            pin->m_sourceImage.release();
            pin->m_cleanBaseImage.release();
            pin->m_markupElements.clear();
            pin->releaseGraphicsResources();
            HWND hwndToDestroy = std::exchange(pin->m_hwnd, nullptr);
            if (hwndToDestroy && IsWindow(hwndToDestroy)) {
                DestroyWindow(hwndToDestroy);
            }
        }
    }
    s_instances.clear();
    s_allHidden = false;
    s_activeGroupFilter = PinGroupConstants::kFilterAllGroups;
    s_createdGroups.fill(false);
    PinGroupBarOverlay::instance().shutdown();
    broadcastGroupStateChanged();
    tools3000::core::WinUtils::trimWorkingSet();
    LOG_INFO("所有贴图窗口已关闭并释放资源");
}

void PinWindow::toggleHideAll() {
    if (s_instances.empty()) return;
    // 智能切换：若当前处于全局隐藏态则按当前分组过滤恢复可见；否则全部隐藏
    bool anyVisible = false;
    for (auto& pin : s_instances) {
        if (pin && pin->m_hwnd && IsWindowVisible(pin->m_hwnd)) { anyVisible = true; break; }
    }
    const bool show = !anyVisible;
    for (auto& pin : s_instances) {
        if (!pin || !pin->m_hwnd) continue;
        if (show) {
            const bool matchFilter = (s_activeGroupFilter == PinGroupConstants::kFilterAllGroups) ||
                                     (pin->m_groupId == s_activeGroupFilter);
            ShowWindow(pin->m_hwnd, matchFilter ? SW_SHOWNOACTIVATE : SW_HIDE);
        } else {
            ShowWindow(pin->m_hwnd, SW_HIDE);
        }
    }
    s_allHidden = !show;
    refreshGroupBar();
    LOG_INFO("所有贴图已{}", show ? "显示" : "隐藏");
}

void PinWindow::arrangeAll() {
    if (s_instances.empty()) return;
    s_allHidden = false;
    s_activeGroupFilter = PinGroupConstants::kFilterAllGroups;

    HWND anchorHwnd = nullptr;
    bool hasGroupedPins = false;
    for (const auto& pin : s_instances) {
        if (!pin || !pin->m_hwnd || !IsWindow(pin->m_hwnd)) continue;
        anchorHwnd = pin->m_hwnd;
        if (pin->m_groupId >= PinGroupConstants::kMinGroupId) {
            hasGroupedPins = true;
        }
    }

    const HMONITOR mon = anchorHwnd
        ? MonitorFromWindow(anchorHwnd, MONITOR_DEFAULTTOPRIMARY)
        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const RECT wa = tools3000::core::dpi::workArea(mon);
    const float monScale = std::clamp(tools3000::core::dpi::scaleForMonitor(mon), 1.0f, 4.0f);

    const int cascadeStep = static_cast<int>(std::lround(32.0f * monScale));
    const int noteGap = static_cast<int>(std::lround(10.0f * monScale));
    const int marginX = static_cast<int>(std::lround(36.0f * monScale));
    const int topStart = wa.top + static_cast<int>(std::lround((hasGroupedPins ? 58.0f : 36.0f) * monScale));
    int curX = wa.left + marginX;
    int curY = topStart;
    int colMaxWidth = static_cast<int>(std::lround(220.0f * monScale));
    int countInCol = 0;
    int i = 0;

    for (auto& pin : s_instances) {
        if (!pin || !pin->m_hwnd) continue;
        RECT rc{};
        GetWindowRect(pin->m_hwnd, &rc);
        const int winW = (std::max)(1, static_cast<int>(rc.right - rc.left));
        const int winH = (std::max)(1, static_cast<int>(rc.bottom - rc.top));

        const bool isCompactNote = pin->isGeneratedCard() || pin->m_folded || winH <= static_cast<int>(110.0f * monScale);
        if (countInCol > 0 && (curY + winH > wa.bottom - 24 || countInCol >= 12)) {
            curX += colMaxWidth + static_cast<int>(std::lround(18.0f * monScale));
            if (curX + winW > wa.right - 20) {
                curX = wa.left + marginX + ((i / 12) % 4) * cascadeStep;
            }
            curY = topStart;
            colMaxWidth = static_cast<int>(std::lround(220.0f * monScale));
            countInCol = 0;
        }

        const int px = isCompactNote ? curX : (curX + countInCol * static_cast<int>(std::lround(16.0f * monScale)));
        const int py = curY;

        ShowWindow(pin->m_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(pin->m_hwnd, HWND_TOPMOST, px, py, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);

        colMaxWidth = (std::max)(colMaxWidth, winW);
        curY += isCompactNote ? (winH + noteGap) : cascadeStep;
        ++countInCol;
        ++i;
    }
    refreshGroupBar();
    LOG_INFO("已整理 {} 张贴图", i);
}

size_t PinWindow::countInGroup(int groupId) noexcept {
    const int norm = PinGroupPalette::normalizeGroupId(groupId);
    size_t cnt = 0;
    for (const auto& pin : s_instances) {
        if (pin && pin->isAlive() && pin->m_groupId == norm) {
            ++cnt;
        }
    }
    return cnt;
}

void PinWindow::ensureExplicitGroupCreated(int groupId) noexcept {
    const int norm = PinGroupPalette::normalizeGroupId(groupId);
    if (norm != PinGroupConstants::kUngroupedId) {
        s_createdGroups[static_cast<size_t>(norm)] = true;
    }
}

void PinWindow::refreshAllVisualsAndGroupBar() {
    PinGroupPalette::ensureLoadedFromConfig();
    for (auto& pin : s_instances) {
        if (!pin || !pin->isAlive() || !pin->m_hwnd) continue;
        if (pin->m_folded) {
            RECT curRc{};
            GetWindowRect(pin->m_hwnd, &curRc);
            const float dpiScale = tools3000::core::dpi::scaleForWindow(pin->m_hwnd);
            pin->m_dpiScale = dpiScale;
            const SIZE foldSz = pin->computeFoldCapsuleSize(dpiScale);
            SetWindowPos(pin->m_hwnd, nullptr, curRc.left, curRc.top, foldSz.cx, foldSz.cy,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            if (pin->m_renderTarget) {
                pin->m_renderTarget->Resize(D2D1::SizeU(
                    static_cast<UINT>(foldSz.cx), static_cast<UINT>(foldSz.cy)));
            }
            tools3000::core::WinUtils::applyUniversalRoundedCorners(
                pin->m_hwnd, foldSz.cx, foldSz.cy,
                static_cast<int>(std::round(8.0f * dpiScale)));
        }
        if (IsWindowVisible(pin->m_hwnd)) {
            pin->render();
        }
    }
    refreshGroupBar();
}

bool PinWindow::isExplicitGroupCreated(int groupId) noexcept {
    const int norm = PinGroupPalette::normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) return false;
    return s_createdGroups[static_cast<size_t>(norm)];
}

int PinWindow::nextAvailableGroupId() noexcept {
    for (int g = PinGroupConstants::kMinGroupId; g <= PinGroupConstants::kMaxGroupId; ++g) {
        if (countInGroup(g) == 0 && !s_createdGroups[static_cast<size_t>(g)]) {
            return g;
        }
    }
    return PinGroupConstants::kUngroupedId;
}

int PinWindow::createAndActivateNewGroup(PinWindow* assignPin) {
    const int chosen = nextAvailableGroupId();
    const bool zh = tools3000::core::isUiLanguageChinese();
    if (chosen == PinGroupConstants::kUngroupedId) {
        const std::wstring fullMsg = zh
            ? std::format(L"贴图分组已达上限 ({} 组)", PinGroupConstants::kMaxGroupId)
            : std::format(L"Maximum pin groups reached ({})", PinGroupConstants::kMaxGroupId);
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{fullMsg});
        return PinGroupConstants::kUngroupedId;
    }

    s_createdGroups[static_cast<size_t>(chosen)] = true;
    if (assignPin != nullptr) {
        assignPin->setGroupId(chosen);
        return chosen;
    }

    if (s_instances.empty()) {
        s_activeGroupFilter = chosen;
    }
    refreshGroupBar();

    const std::wstring msg = zh
        ? std::format(L"已新建{}", PinGroupPalette::fullLabel(chosen, true))
        : std::format(L"Created {}", PinGroupPalette::fullLabel(chosen, false));
    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{msg});
    return chosen;
}

void PinWindow::broadcastGroupStateChanged() {
    std::array<size_t, PinGroupConstants::kMaxGroupId + 1> groupCounts{};
    size_t totalAlive = 0;
    for (const auto& pin : s_instances) {
        if (!pin || !pin->isAlive()) continue;
        ++totalAlive;
        const int g = PinGroupPalette::normalizeGroupId(pin->m_groupId);
        ++groupCounts[static_cast<size_t>(g)];
    }

    const bool zh = tools3000::core::isUiLanguageChinese();
    nlohmann::json groupsJson = nlohmann::json::array();
    for (int g = PinGroupConstants::kMinGroupId; g <= PinGroupConstants::kMaxGroupId; ++g) {
        const size_t cnt = groupCounts[static_cast<size_t>(g)];
        const bool isCreated = s_createdGroups[static_cast<size_t>(g)];
        const bool hasCustom = PinGroupPalette::hasCustomization(g);
        if (cnt == 0 && !isCreated && s_activeGroupFilter != g && !hasCustom) continue;
        groupsJson.push_back({
            {"id", g},
            {"count", static_cast<int>(cnt)},
            {"explicitCreated", isCreated},
            {"shortLabel", tools3000::core::WinUtils::wstringToUtf8(PinGroupPalette::shortLabel(g, zh))},
            {"fullLabel", tools3000::core::WinUtils::wstringToUtf8(PinGroupPalette::fullLabel(g, zh))},
            {"hasCustomization", hasCustom},
            {"customName", tools3000::core::WinUtils::wstringToUtf8(PinGroupPalette::customName(g))},
            {"customColorHex", PinGroupPalette::customColorHex(g)},
            {"defaultColorHex", PinGroupPalette::defaultHex(g)},
            {"colorHex", PinGroupPalette::accentHex(g)}
        });
    }

    nlohmann::json catalogJson = nlohmann::json::array();
    for (int g = PinGroupConstants::kMinGroupId; g <= PinGroupConstants::kMaxGroupId; ++g) {
        const size_t cnt = groupCounts[static_cast<size_t>(g)];
        const bool isCreated = s_createdGroups[static_cast<size_t>(g)];
        const bool hasCustom = PinGroupPalette::hasCustomization(g);
        if (g <= PinGroupConstants::kDefaultQuickGroupCount || cnt > 0 || isCreated || s_activeGroupFilter == g || hasCustom) {
            catalogJson.push_back({
                {"id", g},
                {"count", static_cast<int>(cnt)},
                {"explicitCreated", isCreated},
                {"hasCustomization", hasCustom},
                {"shortLabel", tools3000::core::WinUtils::wstringToUtf8(PinGroupPalette::shortLabel(g, zh))},
                {"fullLabel", tools3000::core::WinUtils::wstringToUtf8(PinGroupPalette::fullLabel(g, zh))},
                {"customName", tools3000::core::WinUtils::wstringToUtf8(PinGroupPalette::customName(g))},
                {"customColorHex", PinGroupPalette::customColorHex(g)},
                {"defaultColorHex", PinGroupPalette::defaultHex(g)},
                {"colorHex", PinGroupPalette::accentHex(g)}
            });
        }
    }
    nlohmann::json palettePresetsJson = nlohmann::json::array();
    const auto& presets = PinGroupPalette::presetPaletteHexes();
    for (int idx = 0; idx < static_cast<int>(presets.size()); ++idx) {
        palettePresetsJson.push_back({
            {"slot", idx + 1},
            {"hex", std::string(presets[static_cast<size_t>(idx)])},
            {"name", tools3000::core::WinUtils::wstringToUtf8(std::wstring(PinGroupPalette::presetColorName(idx + 1, zh)))}
        });
    }

    tools3000::core::MessageBridge::instance().pushEvent("pin:groupsChanged", {
        {"totalCount", static_cast<int>(totalAlive)},
        {"ungroupedCount", static_cast<int>(groupCounts[0])},
        {"activeFilter", s_activeGroupFilter},
        {"allHidden", s_allHidden},
        {"groupBarVisible", PinGroupBarOverlay::instance().isVisible()},
        {"maxGroupId", PinGroupConstants::kMaxGroupId},
        {"nextAvailableGroupId", nextAvailableGroupId()},
        {"palettePresets", std::move(palettePresetsJson)},
        {"catalog", std::move(catalogJson)},
        {"groups", std::move(groupsJson)}
    });
}

void PinWindow::refreshGroupBar() {
    std::array<size_t, PinGroupConstants::kMaxGroupId + 1> groupCounts{};
    size_t totalAlive = 0;
    for (const auto& pin : s_instances) {
        if (!pin || !pin->isAlive()) continue;
        ++totalAlive;
        const int g = PinGroupPalette::normalizeGroupId(pin->m_groupId);
        ++groupCounts[static_cast<size_t>(g)];
    }
    PinGroupBarOverlay::instance().refresh(
        totalAlive,
        groupCounts[0],
        groupCounts,
        s_createdGroups,
        s_activeGroupFilter,
        s_allHidden);
    broadcastGroupStateChanged();
}

void PinWindow::setGroupId(int groupId) {
    const int target = PinGroupPalette::normalizeGroupId(groupId);
    if (m_groupId == target) return;
    const int prevGroup = m_groupId;
    m_groupId = target;

    if (m_hwnd && IsWindow(m_hwnd) && !s_allHidden) {
        const bool shouldShow = (s_activeGroupFilter == PinGroupConstants::kFilterAllGroups) ||
                                (m_groupId == s_activeGroupFilter);
        ShowWindow(m_hwnd, shouldShow ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (shouldShow) {
            render();
        }
    }

    if (prevGroup >= PinGroupConstants::kMinGroupId &&
        s_activeGroupFilter == prevGroup &&
        countInGroup(prevGroup) == 0) {
        setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
    } else {
        refreshGroupBar();
    }

    const bool zh = tools3000::core::isUiLanguageChinese();
    std::wstring msg;
    if (m_groupId == PinGroupConstants::kUngroupedId) {
        msg = zh ? L"已移出分组" : L"Removed from group";
    } else {
        msg = zh
            ? std::format(L"已归入{}", PinGroupPalette::fullLabel(m_groupId, true))
            : std::format(L"Assigned to {}", PinGroupPalette::fullLabel(m_groupId, false));
    }
    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{msg});
}

void PinWindow::toggleAssignGroup(int groupId) {
    const int target = PinGroupPalette::normalizeGroupId(groupId);
    if (target == PinGroupConstants::kUngroupedId || m_groupId == target) {
        setGroupId(PinGroupConstants::kUngroupedId);
    } else {
        setGroupId(target);
    }
}

void PinWindow::setActiveGroupFilter(int groupFilter) {
    const int norm = PinGroupPalette::normalizeFilter(groupFilter);
    s_activeGroupFilter = norm;
    s_allHidden = false;

    std::shared_ptr<PinWindow> firstVisible;
    for (auto& pin : s_instances) {
        if (!pin || !pin->isAlive() || !pin->m_hwnd) continue;
        const bool visible = (norm == PinGroupConstants::kFilterAllGroups) || (pin->m_groupId == norm);
        ShowWindow(pin->m_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (visible && !firstVisible) {
            firstVisible = pin;
        }
    }
    if (firstVisible && !firstVisible->m_clickThrough) {
        firstVisible->focusWindow();
    }
    refreshGroupBar();
}

int PinWindow::groupAllUngrouped() {
    std::vector<std::shared_ptr<PinWindow>> ungrouped;
    std::array<size_t, PinGroupConstants::kMaxGroupId + 1> counts{};
    for (auto& pin : s_instances) {
        if (!pin || !pin->isAlive()) continue;
        const int g = PinGroupPalette::normalizeGroupId(pin->m_groupId);
        ++counts[static_cast<size_t>(g)];
        if (g == PinGroupConstants::kUngroupedId) {
            ungrouped.push_back(pin);
        }
    }
    if (ungrouped.empty()) {
        return PinGroupConstants::kUngroupedId;
    }

    int chosenGroup = 1;
    bool foundEmpty = false;
    for (int g = PinGroupConstants::kMinGroupId; g <= PinGroupConstants::kMaxGroupId; ++g) {
        if (counts[static_cast<size_t>(g)] == 0 && !s_createdGroups[static_cast<size_t>(g)]) {
            chosenGroup = g;
            foundEmpty = true;
            break;
        }
    }
    if (!foundEmpty) {
        for (int g = PinGroupConstants::kMinGroupId; g <= PinGroupConstants::kMaxGroupId; ++g) {
            if (counts[static_cast<size_t>(g)] == 0) {
                chosenGroup = g;
                foundEmpty = true;
                break;
            }
        }
    }
    if (!foundEmpty) {
        size_t minCnt = counts[1];
        for (int g = 2; g <= PinGroupConstants::kMaxGroupId; ++g) {
            if (counts[static_cast<size_t>(g)] < minCnt) {
                minCnt = counts[static_cast<size_t>(g)];
                chosenGroup = g;
            }
        }
    }

    for (auto& pin : ungrouped) {
        pin->m_groupId = chosenGroup;
        if (pin->m_hwnd && IsWindow(pin->m_hwnd)) {
            const bool show = (s_activeGroupFilter == PinGroupConstants::kFilterAllGroups) ||
                              (s_activeGroupFilter == chosenGroup);
            ShowWindow(pin->m_hwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
            if (show) pin->render();
        }
    }
    if (s_activeGroupFilter == PinGroupConstants::kUngroupedId) {
        s_activeGroupFilter = chosenGroup;
    }
    refreshGroupBar();

    const bool zh = tools3000::core::isUiLanguageChinese();
    const std::wstring msg = zh
        ? std::format(L"已将 {} 张贴图编入{}", ungrouped.size(), PinGroupPalette::shortLabel(chosenGroup, true))
        : std::format(L"Grouped {} pins into {}", ungrouped.size(), PinGroupPalette::shortLabel(chosenGroup, false));
    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{msg});
    return chosenGroup;
}

void PinWindow::arrangeGroup(int groupId) {
    const int norm = PinGroupPalette::normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) {
        arrangeAll();
        return;
    }

    std::vector<std::shared_ptr<PinWindow>> members;
    for (auto& pin : s_instances) {
        if (pin && pin->isAlive() && pin->m_hwnd && pin->m_groupId == norm) {
            members.push_back(pin);
        }
    }
    if (members.empty()) return;

    s_allHidden = false;
    const HMONITOR mon = MonitorFromWindow(members.back()->m_hwnd, MONITOR_DEFAULTTOPRIMARY);
    const RECT wa = tools3000::core::dpi::workArea(mon);
    const float monScale = std::clamp(tools3000::core::dpi::scaleForMonitor(mon), 1.0f, 4.0f);

    const int gap = static_cast<int>(std::lround(16.0f * monScale));
    const int marginX = static_cast<int>(std::lround(36.0f * monScale));
    const int topStart = wa.top + static_cast<int>(std::lround(58.0f * monScale)); // 留出顶部 PinGroupBarOverlay 空间
    int curX = wa.left + marginX;
    int curY = topStart;
    int rowMaxH = 0;

    for (auto& pin : members) {
        RECT rc{};
        GetWindowRect(pin->m_hwnd, &rc);
        const int w = (std::max)(1, static_cast<int>(rc.right - rc.left));
        const int h = (std::max)(1, static_cast<int>(rc.bottom - rc.top));

        if (curX > wa.left + marginX && curX + w > wa.right - static_cast<int>(std::lround(24.0f * monScale))) {
            curX = wa.left + marginX;
            curY += rowMaxH + gap;
            rowMaxH = 0;
        }
        if (curY + h > wa.bottom - static_cast<int>(std::lround(20.0f * monScale)) && curY > topStart) {
            curY = topStart + static_cast<int>(std::lround(24.0f * monScale));
        }

        ShowWindow(pin->m_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(pin->m_hwnd, HWND_TOPMOST, curX, curY, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        curX += w + gap;
        rowMaxH = (std::max)(rowMaxH, h);
    }
    refreshGroupBar();
}

size_t PinWindow::dissolveGroup(int groupId) {
    const int norm = PinGroupPalette::normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) return 0;

    const bool wasExplicit = s_createdGroups[static_cast<size_t>(norm)];
    s_createdGroups[static_cast<size_t>(norm)] = false;

    size_t countRemoved = 0;
    for (auto& pin : s_instances) {
        if (pin && pin->isAlive() && pin->m_groupId == norm) {
            pin->m_groupId = PinGroupConstants::kUngroupedId;
            ++countRemoved;
        }
    }
    if (countRemoved == 0) {
        if (wasExplicit || s_activeGroupFilter == norm) {
            if (s_activeGroupFilter == norm) {
                s_activeGroupFilter = PinGroupConstants::kFilterAllGroups;
            }
            for (auto& pin : s_instances) {
                if (!pin || !pin->isAlive() || !pin->m_hwnd) continue;
                if (!s_allHidden) {
                    const bool visible = (s_activeGroupFilter == PinGroupConstants::kFilterAllGroups) ||
                                         (pin->m_groupId == s_activeGroupFilter);
                    ShowWindow(pin->m_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
                    if (visible) pin->render();
                }
            }
            refreshGroupBar();
        }
        return 0;
    }

    if (s_activeGroupFilter == norm) {
        s_activeGroupFilter = PinGroupConstants::kFilterAllGroups;
    }

    for (auto& pin : s_instances) {
        if (!pin || !pin->isAlive() || !pin->m_hwnd) continue;
        if (!s_allHidden) {
            const bool visible = (s_activeGroupFilter == PinGroupConstants::kFilterAllGroups) ||
                                 (pin->m_groupId == s_activeGroupFilter);
            ShowWindow(pin->m_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            if (visible) pin->render();
        }
    }
    refreshGroupBar();

    const bool zh = tools3000::core::isUiLanguageChinese();
    const std::wstring msg = zh
        ? std::format(L"已移除{}（共 {} 张贴图保留）", PinGroupPalette::shortLabel(norm, true), countRemoved)
        : std::format(L"Ungrouped {} ({} pins kept)", PinGroupPalette::shortLabel(norm, false), countRemoved);
    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{msg});
    return countRemoved;
}

size_t PinWindow::closeGroup(int groupId) {
    const int norm = PinGroupPalette::normalizeGroupId(groupId);
    if (norm == PinGroupConstants::kUngroupedId) return 0;
    s_createdGroups[static_cast<size_t>(norm)] = false;

    // 先拷贝匹配目标分组的窗口局部快照，再逐个销毁窗口，
    // 防止 DestroyWindow 同步触发 WM_NCDESTROY 修改 s_instances 引发迭代器失效与 UAF
    std::vector<std::shared_ptr<PinWindow>> targets;
    targets.reserve(s_instances.size());
    for (const auto& pin : s_instances) {
        if (pin && pin->isAlive() && pin->m_groupId == norm) {
            targets.push_back(pin);
        }
    }

    size_t closedCount = 0;
    for (auto& pin : targets) {
        if (!pin) continue;
        pin->m_sourceImage.release();
        pin->m_cleanBaseImage.release();
        pin->m_markupElements.clear();
        pin->releaseGraphicsResources();
        HWND hwndToDestroy = std::exchange(pin->m_hwnd, nullptr);
        if (hwndToDestroy && IsWindow(hwndToDestroy)) {
            DestroyWindow(hwndToDestroy);
        }
        ++closedCount;
    }

    s_instances.erase(
        std::remove_if(s_instances.begin(), s_instances.end(),
                       [](const auto& p) { return !p || !p->isAlive(); }),
        s_instances.end());

    if (s_instances.empty()) {
        s_activeGroupFilter = PinGroupConstants::kFilterAllGroups;
        PinGroupBarOverlay::instance().shutdown();
        broadcastGroupStateChanged();
        tools3000::core::WinUtils::trimWorkingSet();
    } else if (s_activeGroupFilter == norm) {
        setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
    } else {
        refreshGroupBar();
    }
    return closedCount;
}

namespace PinGroupMenuCmd {
    inline constexpr int kRemoveFromGroup = 1000;
    inline constexpr int kAssignBase = 1100;       // 1101..1132
    inline constexpr int kCreateNewAndAssign = 1190;
    inline constexpr int kAutoGroupUngrouped = 1195;
    inline constexpr int kViewAll = 1200;
    inline constexpr int kViewBase = 1200;         // 1201..1232
    inline constexpr int kArrangeCurrentGroup = 1300;
    inline constexpr int kDissolveCurrentGroup = 1301;
    inline constexpr int kCloseCurrentGroup = 1302;
    inline constexpr int kRenameCurrentGroup = 1305;
    inline constexpr int kResetGroupMeta = 1400;
    inline constexpr int kSetColorPresetBase = 1400; // 1401..1412
} // namespace PinGroupMenuCmd

static void populatePinGroupMenuItems(HMENU groupMenu, const PinWindow* self, bool isZh) {
    if (!groupMenu || !self) return;
    for (int g = PinGroupConstants::kMinGroupId; g <= PinGroupConstants::kMaxGroupId; ++g) {
        const size_t cnt = PinWindow::countInGroup(g);
        const bool showInAssignList = (g <= PinGroupConstants::kDefaultQuickGroupCount) ||
                                      (cnt > 0) ||
                                      PinWindow::isExplicitGroupCreated(g) ||
                                      (self->groupId() == g) ||
                                      (PinWindow::activeGroupFilter() == g) ||
                                      PinGroupPalette::hasCustomization(g);
        if (!showInAssignList) continue;

        std::wstring itemLabel;
        if (g <= PinGroupConstants::kMaxShortcutGroupId) {
            itemLabel = isZh
                ? std::format(L"归入{}  [{}]  (Ctrl+{})", PinGroupPalette::fullLabel(g, true), cnt, g)
                : std::format(L"Assign to {}  [{}]  (Ctrl+{})", PinGroupPalette::fullLabel(g, false), cnt, g);
        } else {
            itemLabel = isZh
                ? std::format(L"归入{}  [{}]", PinGroupPalette::fullLabel(g, true), cnt)
                : std::format(L"Assign to {}  [{}]", PinGroupPalette::fullLabel(g, false), cnt);
        }
        const UINT flags = ((self->groupId() == g) ? MF_CHECKED : MF_UNCHECKED) | MF_STRING;
        AppendMenuW(groupMenu, flags, static_cast<UINT_PTR>(PinGroupMenuCmd::kAssignBase + g), itemLabel.c_str());
    }

    const int nextFree = PinWindow::nextAvailableGroupId();
    if (nextFree >= PinGroupConstants::kMinGroupId) {
        const std::wstring newGroupLabel = isZh
            ? std::format(L"+ 新建贴图组并归入：{}", PinGroupPalette::fullLabel(nextFree, true))
            : std::format(L"+ New Group & Assign: {}", PinGroupPalette::fullLabel(nextFree, false));
        AppendMenuW(groupMenu, MF_STRING, PinGroupMenuCmd::kCreateNewAndAssign, newGroupLabel.c_str());
    }

    if (self->groupId() >= PinGroupConstants::kMinGroupId) {
        AppendMenuW(groupMenu, MF_STRING, PinGroupMenuCmd::kRemoveFromGroup,
                    isZh ? L"移出当前分组  (Ctrl+0)" : L"Remove from Group  (Ctrl+0)");
    }

    const size_t ungroupedCnt = PinWindow::countInGroup(PinGroupConstants::kUngroupedId);
    if (ungroupedCnt >= 1) {
        const std::wstring autoLabel = isZh
            ? std::format(L"一键编组未分组贴图 ({} 张)  (Ctrl+Shift+G)", ungroupedCnt)
            : std::format(L"Auto-Group Ungrouped ({})  (Ctrl+Shift+G)", ungroupedCnt);
        AppendMenuW(groupMenu, MF_STRING, PinGroupMenuCmd::kAutoGroupUngrouped, autoLabel.c_str());
    }

    AppendMenuW(groupMenu, MF_SEPARATOR, 0, nullptr);
    const UINT allFlags = ((PinWindow::activeGroupFilter() == PinGroupConstants::kFilterAllGroups)
                           ? MF_CHECKED : MF_UNCHECKED) | MF_STRING;
    AppendMenuW(groupMenu, allFlags, PinGroupMenuCmd::kViewAll,
                isZh ? L"按组查看：全部贴图  (Alt+0)" : L"View: All Pins  (Alt+0)");

    for (int g = PinGroupConstants::kMinGroupId; g <= PinGroupConstants::kMaxGroupId; ++g) {
        const size_t cnt = PinWindow::countInGroup(g);
        if (cnt == 0 && !PinWindow::isExplicitGroupCreated(g) && PinWindow::activeGroupFilter() != g) continue;
        std::wstring viewLabel;
        if (g <= PinGroupConstants::kMaxShortcutGroupId) {
            viewLabel = isZh
                ? std::format(L"仅查看{} ({} 张)  (Alt+{})", PinGroupPalette::shortLabel(g, true), cnt, g)
                : std::format(L"View Only {} ({})  (Alt+{})", PinGroupPalette::shortLabel(g, false), cnt, g);
        } else {
            viewLabel = isZh
                ? std::format(L"仅查看{} ({} 张)", PinGroupPalette::shortLabel(g, true), cnt)
                : std::format(L"View Only {} ({})", PinGroupPalette::shortLabel(g, false), cnt);
        }
        const UINT fFlags = ((PinWindow::activeGroupFilter() == g) ? MF_CHECKED : MF_UNCHECKED) | MF_STRING;
        AppendMenuW(groupMenu, fFlags, static_cast<UINT_PTR>(PinGroupMenuCmd::kViewBase + g), viewLabel.c_str());
    }

    if (self->groupId() >= PinGroupConstants::kMinGroupId) {
        AppendMenuW(groupMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(groupMenu, MF_STRING, PinGroupMenuCmd::kArrangeCurrentGroup,
                    isZh ? L"网格整理本组贴图" : L"Arrange Current Group in Grid");
        AppendMenuW(groupMenu, MF_STRING, PinGroupMenuCmd::kDissolveCurrentGroup,
                    isZh ? L"移除本组全部标记（保留贴图）" : L"Ungroup Current Group (Keep Pins)");
        AppendMenuW(groupMenu, MF_STRING, PinGroupMenuCmd::kCloseCurrentGroup,
                    isZh ? L"关闭本组全部贴图" : L"Close All Pins in Current Group");
        const int gid = self->groupId();
        HMENU colorMenu = CreatePopupMenu();
        if (colorMenu) {
            const std::string curHex = PinGroupPalette::accentHex(gid);
            const auto& presets = PinGroupPalette::presetPaletteHexes();
            for (int idx = 0; idx < static_cast<int>(presets.size()); ++idx) {
                const bool checked = (curHex == presets[static_cast<size_t>(idx)]);
                const std::wstring cLabel = std::format(
                    L"{}  ({})",
                    PinGroupPalette::presetColorName(idx + 1, isZh),
                    tools3000::core::WinUtils::utf8ToWstring(std::string(presets[static_cast<size_t>(idx)])));
                AppendMenuW(colorMenu, (checked ? MF_CHECKED : MF_UNCHECKED) | MF_STRING,
                            static_cast<UINT_PTR>(PinGroupMenuCmd::kSetColorPresetBase + 1 + idx), cLabel.c_str());
            }
            AppendMenuW(groupMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(groupMenu, MF_STRING, PinGroupMenuCmd::kRenameCurrentGroup,
                        isZh ? L"重命名本组..." : L"Rename Current Group...");
            AppendMenuW(groupMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(colorMenu),
                        isZh ? L"切换本组颜色" : L"Set Group Color");
        }
        if (PinGroupPalette::hasCustomization(gid)) {
            AppendMenuW(groupMenu, MF_STRING, PinGroupMenuCmd::kResetGroupMeta,
                        isZh ? L"恢复默认组名与颜色" : L"Reset Default Group Name & Color");
        }
    }
}

static bool dispatchPinGroupMenuCommand(PinWindow* self, int cmd) {
    if (!self) return false;
    if (cmd == PinGroupMenuCmd::kRemoveFromGroup) {
        self->setGroupId(PinGroupConstants::kUngroupedId);
        return true;
    }
    if (cmd > PinGroupMenuCmd::kAssignBase &&
        cmd <= PinGroupMenuCmd::kAssignBase + PinGroupConstants::kMaxGroupId) {
        self->toggleAssignGroup(cmd - PinGroupMenuCmd::kAssignBase);
        return true;
    }
    if (cmd == PinGroupMenuCmd::kCreateNewAndAssign) {
        PinWindow::createAndActivateNewGroup(self);
        return true;
    }
    if (cmd == PinGroupMenuCmd::kAutoGroupUngrouped) {
        PinWindow::groupAllUngrouped();
        return true;
    }
    if (cmd == PinGroupMenuCmd::kViewAll) {
        PinWindow::setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
        return true;
    }
    if (cmd > PinGroupMenuCmd::kViewBase &&
        cmd <= PinGroupMenuCmd::kViewBase + PinGroupConstants::kMaxGroupId) {
        const int targetG = cmd - PinGroupMenuCmd::kViewBase;
        if (PinWindow::activeGroupFilter() == targetG) {
            PinWindow::setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
        } else {
            PinWindow::setActiveGroupFilter(targetG);
        }
        return true;
    }
    if (cmd == PinGroupMenuCmd::kArrangeCurrentGroup && self->groupId() >= PinGroupConstants::kMinGroupId) {
        PinWindow::arrangeGroup(self->groupId());
        return true;
    }
    if (cmd == PinGroupMenuCmd::kDissolveCurrentGroup && self->groupId() >= PinGroupConstants::kMinGroupId) {
        PinWindow::dissolveGroup(self->groupId());
        return true;
    }
    if (cmd == PinGroupMenuCmd::kResetGroupMeta && self->groupId() >= PinGroupConstants::kMinGroupId) {
        PinGroupPalette::resetGroupCustomization(self->groupId());
        PinWindow::refreshAllVisualsAndGroupBar();
        return true;
    }
    if (cmd == PinGroupMenuCmd::kRenameCurrentGroup && self->groupId() >= PinGroupConstants::kMinGroupId) {
        PinGroupBarOverlay::instance().beginInlineRename(self->groupId());
        return true;
    }
    const int presetCount = static_cast<int>(PinGroupPalette::presetPaletteHexes().size());
    if (cmd > PinGroupMenuCmd::kSetColorPresetBase &&
        cmd <= PinGroupMenuCmd::kSetColorPresetBase + presetCount &&
        self->groupId() >= PinGroupConstants::kMinGroupId) {
        const int slot = cmd - PinGroupMenuCmd::kSetColorPresetBase - 1;
        if (PinGroupPalette::setGroupColor(self->groupId(),
                                           PinGroupPalette::presetPaletteHexes()[static_cast<size_t>(slot)])) {
            PinWindow::refreshAllVisualsAndGroupBar();
        }
        return true;
    }
    if (cmd == PinGroupMenuCmd::kCloseCurrentGroup && self->groupId() >= PinGroupConstants::kMinGroupId) {
        PinWindow::closeGroup(self->groupId());
        return true;
    }
    return false;
}

void PinWindow::showGroupPopupMenu(POINT screenPt) {
    if (!m_hwnd) return;
    HMENU groupMenu = CreatePopupMenu();
    if (!groupMenu) return;
    const bool isZh = tools3000::core::isUiLanguageChinese();
    populatePinGroupMenuItems(groupMenu, this, isZh);
    const int cmd = TrackPopupMenu(groupMenu, TPM_RETURNCMD, screenPt.x, screenPt.y, 0, m_hwnd, nullptr);
    DestroyMenu(groupMenu);
    dispatchPinGroupMenuCommand(this, cmd);
}

PinWindow::~PinWindow() {
    releaseGraphicsResources();
    HWND hwndToDestroy = std::exchange(m_hwnd, nullptr);
    if (hwndToDestroy && IsWindow(hwndToDestroy)) {
        DestroyWindow(hwndToDestroy);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 属性
// ─────────────────────────────────────────────────────────────────────────────

void PinWindow::focusWindow() {
    if (!m_hwnd || !IsWindow(m_hwnd) || m_clickThrough) return;
    HWND fg = GetForegroundWindow();
    if (fg != m_hwnd) {
        DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD selfTid = GetCurrentThreadId();
        bool attached = false;
        if (fgTid != 0 && fgTid != selfTid) {
            attached = AttachThreadInput(fgTid, selfTid, TRUE) != FALSE;
        }
        SetForegroundWindow(m_hwnd);
        BringWindowToTop(m_hwnd);
        if (attached) {
            AttachThreadInput(fgTid, selfTid, FALSE);
        }
    }
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    if (!m_focused) {
        m_focused = true;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void PinWindow::applyLayeredOpacity() {
    if (!m_hwnd) return;
    BYTE alpha = static_cast<BYTE>(std::clamp(m_opacity * 255.0f, 10.0f, 255.0f));
    SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);
}

void PinWindow::applySharpWindowCorners() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    SetWindowRgn(m_hwnd, nullptr, TRUE);
    // DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_DONOTROUND = 1
    constexpr DWORD kDwmWindowCornerPreference = 33;
    const DWORD pref = 1;
    DwmSetWindowAttribute(m_hwnd, kDwmWindowCornerPreference, &pref, sizeof(pref));
}

void PinWindow::updateWindowCornerRegion() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int curW = (std::max)(1, static_cast<int>(rc.right - rc.left));
    const int curH = (std::max)(1, static_cast<int>(rc.bottom - rc.top));

    if (m_folded) {
        const float dpiScale = tools3000::core::dpi::scaleForWindow(m_hwnd);
        tools3000::core::WinUtils::applyUniversalRoundedCorners(
            m_hwnd, curW, curH, static_cast<int>(std::lround(8.0f * dpiScale)));
        return;
    }

    if (m_cardCornerRadius > 0.5f) {
        const int scaledR = std::clamp(
            static_cast<int>(std::lround(m_cardCornerRadius * m_scale)),
            3, (std::max)(3, (std::min)(curW, curH) / 2));
        // Disable Win11 fixed 8-DIP DWMWCP_ROUND so zoomed cards scale corner radius smoothly
        constexpr DWORD kDwmWindowCornerPreference = 33;
        const DWORD pref = 1; // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(m_hwnd, kDwmWindowCornerPreference, &pref, sizeof(pref));
        HRGN hRgn = CreateRoundRectRgn(0, 0, curW + 1, curH + 1, scaledR * 2, scaledR * 2);
        if (hRgn) {
            if (!SetWindowRgn(m_hwnd, hRgn, TRUE)) {
                DeleteObject(hRgn);
            }
        }
        return;
    }

    applySharpWindowCorners();
}

bool PinWindow::ensureTextResources(float dpiScale) {
    const float safeScale = (dpiScale > 0.0f) ? dpiScale : 1.0f;
    const bool isZh = tools3000::core::isUiLanguageChinese();

    if (!m_dwriteFactory) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
            return false;
        }
    }

    if (m_uiLeadTextFormat && m_uiCenterTextFormat &&
        std::abs(m_cachedTextScale - safeScale) <= 0.005f &&
        m_cachedTextChinese == isZh) {
        return true;
    }

    const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
    const wchar_t* localeName = isZh ? L"zh-CN" : L"en-US";
    const float fontPx = (std::max)(12.0f, 12.0f * safeScale);

    ComPtr<IDWriteTextFormat> leadFmt;
    ComPtr<IDWriteTextFormat> centerFmt;
    if (FAILED(m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontPx, localeName, leadFmt.GetAddressOf())) ||
        FAILED(m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontPx, localeName, centerFmt.GetAddressOf()))) {
        return false;
    }

    leadFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    leadFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    leadFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    centerFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    centerFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    centerFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    ComPtr<IDWriteInlineObject> leadEllipsis;
    if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(leadFmt.Get(), &leadEllipsis))) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        leadFmt->SetTrimming(&trimming, leadEllipsis.Get());
    }
    ComPtr<IDWriteInlineObject> centerEllipsis;
    if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(centerFmt.Get(), &centerEllipsis))) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        centerFmt->SetTrimming(&trimming, centerEllipsis.Get());
    }

    m_uiLeadTextFormat = std::move(leadFmt);
    m_uiCenterTextFormat = std::move(centerFmt);
    m_cachedTextScale = safeScale;
    m_cachedTextChinese = isZh;
    return true;
}

float PinWindow::measureUiTextWidth(const std::wstring& text, float dpiScale) {
    if (text.empty()) return 0.0f;
    const float safeScale = (dpiScale > 0.0f) ? dpiScale : 1.0f;
    if (ensureTextResources(safeScale) && m_dwriteFactory && m_uiLeadTextFormat) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()),
                m_uiLeadTextFormat.Get(),
                4096.0f * (std::max)(1.0f, safeScale),
                128.0f * (std::max)(1.0f, safeScale),
                layout.GetAddressOf())) && layout) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                return std::ceil(metrics.widthIncludingTrailingWhitespace);
            }
        }
    }
    return std::ceil(static_cast<float>(text.size()) * 12.0f * safeScale);
}

SIZE PinWindow::computeFoldCapsuleSize(float dpiScale) {
    const float safeScale = (dpiScale > 0.0f) ? dpiScale : 1.0f;
    const bool isZh = tools3000::core::isUiLanguageChinese();
    const wchar_t* txt = isZh ? L"贴图 (双击展开)" : L"Pinned (Folded)";
    const float textW = measureUiTextWidth(txt, safeScale);
    const int foldW = static_cast<int>(std::ceil(
        (std::max)(148.0f * safeScale, 34.0f * safeScale + textW + 14.0f * safeScale)));
    const int foldH = static_cast<int>(std::lround(38.0f * safeScale));
    return SIZE{foldW, foldH};
}

void PinWindow::updateHoverAnimation() {
    bool needsRender = false;
    float dt = (GetTickCount64() - m_hoverTime) / 150.0f;
    if (dt > 1.0f) dt = 1.0f;
    
    float targetAlpha = m_isHovering ? 1.0f : 0.0f;
    float startAlpha = m_isHovering ? 0.0f : 1.0f;
    
    float newAlpha = m_isHovering ? 
        startAlpha + (targetAlpha - startAlpha) * (1.0f - pow(1.0f - dt, 3.0f)) : 
        startAlpha + (targetAlpha - startAlpha) * dt; // linear fade out
        
    if (abs(m_hoverAlpha - newAlpha) > 0.01f) {
        m_hoverAlpha = newAlpha;
        needsRender = true;
    }
    
    if (dt >= 1.0f) {
        m_hoverAlpha = targetAlpha;
        KillTimer(m_hwnd, 1);
        needsRender = true;
    }
    
    if (needsRender) {
        render();
    }
}

void PinWindow::drawHoverToolbar() {
    if (!m_renderTarget) return;
    auto size = m_renderTarget->GetSize();

    const float dpiScale = m_hwnd ? tools3000::core::dpi::scaleForWindow(m_hwnd) : m_dpiScale;
    m_dpiScale = dpiScale;

    // 悬浮工具栏：折叠、旋转、反色、编辑、文字识别/复制、复制图片、保存、关闭 8 个高精度矢量按钮
    // 自适应多行网格与紧凑缩放，保障窄贴图（如 400x20 旋转为 20x400 后）全部按钮仍在可视区内
    const auto layout = PinInputController::computeHoverToolbarLayout(size.width, size.height, dpiScale);
    m_toolbarRect = layout.toolbarRect;
    m_btnFoldRect   = layout.buttons[0];
    m_btnRotateRect = layout.buttons[1];
    m_btnInvertRect = layout.buttons[2];
    m_btnEditRect   = layout.buttons[3];
    m_btnTextRect   = layout.buttons[4];
    m_btnCopyRect   = layout.buttons[5];
    m_btnSaveRect   = layout.buttons[6];
    m_btnCloseRect  = layout.buttons[7];
    
    // 绘制磨砂玻璃背景（跟随动态明暗主题与自定义强调色）
    const auto pal = FloatingGlassBar::resolveThemePalette();
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), m_toolbarRect, 6.0f * dpiScale, true);

    ComPtr<ID2D1SolidColorBrush> textBrush;
    D2D1_COLOR_F iconColor = pal.textPrimary;
    iconColor.a *= m_hoverAlpha;
    m_renderTarget->CreateSolidColorBrush(iconColor, textBrush.GetAddressOf());
    
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnFoldRect, 4.0f * dpiScale, m_hoverFold, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnRotateRect, 4.0f * dpiScale, m_hoverRotate, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnInvertRect, 4.0f * dpiScale, m_hoverInvert, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnEditRect, 4.0f * dpiScale, m_hoverEdit, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnTextRect, 4.0f * dpiScale, m_hoverText, m_textSelectionMode, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnCopyRect, 4.0f * dpiScale, m_hoverCopy, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnSaveRect, 4.0f * dpiScale, m_hoverSave, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnCloseRect, 4.0f * dpiScale, m_hoverClose, false, true, false, m_hoverAlpha, true);
    
    // 1. Fold 图标 (Reicon: ActionFold)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionFold, m_btnFoldRect, textBrush.Get(), dpiScale);

    // 2. Rotate 图标 (Lucide: RotateCw)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionRotate, m_btnRotateRect, textBrush.Get(), dpiScale);

    // 3. Invert 图标 (Reicon: ActionInvert)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionInvert, m_btnInvertRect, textBrush.Get(), dpiScale);

    // 4. Edit 图标 (Lucide: Pen)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ToolPen, m_btnEditRect, textBrush.Get(), dpiScale);

    // 5. Text/OCR 图标：普通图片贴图采用与主工具栏同源的 OCR 取景扫描图标，纯文本字签采用文本图标
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        m_pinnedSourceText.empty() ? CaptureIconId::ActionExtractText : CaptureIconId::ToolText,
        m_btnTextRect, textBrush.Get(), dpiScale);

    // 6. Copy 图标 (Lucide: Copy)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionCopy, m_btnCopyRect, textBrush.Get(), dpiScale);
    
    // 7. Save 图标 (Lucide: Download)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionSave, m_btnSaveRect, textBrush.Get(), dpiScale);
    
    // 8. Close 图标 (Lucide: X)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionCancel, m_btnCloseRect, textBrush.Get(), dpiScale);

    // 悬停微晶快捷键提示胶囊 (Tooltip Pill)
    const bool isZh = tools3000::core::isUiLanguageChinese();
    const wchar_t* tipText = nullptr;
    float anchorX = 0.0f;
    if (m_hoverFold) {
        tipText = isZh ? L"折叠胶囊 (M)" : L"Fold Capsule (M)";
        anchorX = (m_btnFoldRect.left + m_btnFoldRect.right) * 0.5f;
    } else if (m_hoverRotate) {
        tipText = isZh ? L"旋转 90° (R / 1·2)" : L"Rotate 90° (R / 1·2)";
        anchorX = (m_btnRotateRect.left + m_btnRotateRect.right) * 0.5f;
    } else if (m_hoverInvert) {
        tipText = isZh ? L"色彩反转 (I)" : L"Invert Colors (I)";
        anchorX = (m_btnInvertRect.left + m_btnInvertRect.right) * 0.5f;
    } else if (m_hoverEdit) {
        tipText = isZh ? L"二次标注 (E)" : L"Annotate (E)";
        anchorX = (m_btnEditRect.left + m_btnEditRect.right) * 0.5f;
    } else if (m_hoverText) {
        if (!m_pinnedSourceText.empty()) {
            tipText = isZh ? L"复制原文 (Ctrl+C)" : L"Copy Text (Ctrl+C)";
        } else {
            tipText = isZh ? L"识别文字 / 划词 (Alt)" : L"Recognize Text (Alt)";
        }
        anchorX = (m_btnTextRect.left + m_btnTextRect.right) * 0.5f;
    } else if (m_hoverCopy) {
        tipText = isZh ? L"复制贴图 (Ctrl+C)" : L"Copy Pin (Ctrl+C)";
        anchorX = (m_btnCopyRect.left + m_btnCopyRect.right) * 0.5f;
    } else if (m_hoverSave) {
        tipText = isZh ? L"保存图片 (Ctrl+S)" : L"Save Image (Ctrl+S)";
        anchorX = (m_btnSaveRect.left + m_btnSaveRect.right) * 0.5f;
    } else if (m_hoverClose) {
        tipText = isZh ? L"关闭贴图 (Esc)" : L"Close Pin (Esc)";
        anchorX = (m_btnCloseRect.left + m_btnCloseRect.right) * 0.5f;
    }

    if (tipText) {
        const D2D1_RECT_F baseTipRect = PinInputController::computeTooltipRect(
            m_toolbarRect, anchorX, size.width, size.height, dpiScale);
        if (baseTipRect.right > baseTipRect.left && baseTipRect.bottom > baseTipRect.top &&
            ensureTextResources(dpiScale) && m_uiCenterTextFormat && textBrush) {
            const float margin = 4.0f * dpiScale;
            const float measuredW = measureUiTextWidth(tipText, dpiScale);
            const float maxAvailW = (std::max)(40.0f * dpiScale, size.width - 2.0f * margin);
            const float tipW = std::clamp(std::ceil(measuredW + 20.0f * dpiScale), 44.0f * dpiScale, maxAvailW);
            const float maxTipX = (std::max)(margin, size.width - tipW - margin);
            const float tipX = std::clamp(anchorX - tipW * 0.5f, margin, maxTipX);
            const float desiredTipH = (std::max)(baseTipRect.bottom - baseTipRect.top, 24.0f * dpiScale);
            const float tipH = (baseTipRect.top + desiredTipH + margin <= size.height)
                ? desiredTipH
                : (baseTipRect.bottom - baseTipRect.top);
            const D2D1_RECT_F tipRect = D2D1::RectF(tipX, baseTipRect.top, tipX + tipW, baseTipRect.top + tipH);

            FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), tipRect, 4.0f * dpiScale, true);
            const D2D1_RECT_F tipTextRect = D2D1::RectF(
                tipRect.left + 6.0f * dpiScale,
                tipRect.top,
                (std::max)(tipRect.left + 6.0f * dpiScale, tipRect.right - 6.0f * dpiScale),
                tipRect.bottom);
            m_renderTarget->DrawTextW(
                tipText, static_cast<UINT32>(wcslen(tipText)),
                m_uiCenterTextFormat.Get(), tipTextRect, textBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

namespace {

enum PinInteractiveButton : int {
    kPinBtnNone = -1,
    kPinBtnFold = 0,
    kPinBtnRotate = 1,
    kPinBtnInvert = 2,
    kPinBtnEdit = 3,
    kPinBtnText = 4,
    kPinBtnCopy = 5,
    kPinBtnSave = 6,
    kPinBtnClose = 7,
    kPinBtnGroupBadge = 8,
    kPinBtnCopyFloating = 9,
};

bool isPointInRectF(const D2D1_RECT_F& rc, int x, int y) noexcept {
    return (rc.right > rc.left && rc.bottom > rc.top &&
            static_cast<float>(x) >= rc.left && static_cast<float>(x) <= rc.right &&
            static_cast<float>(y) >= rc.top && static_cast<float>(y) <= rc.bottom);
}

} // namespace

void PinWindow::forceHoverStateForTest(bool hovering) {
    m_isHovering = hovering;
    m_hoverAlpha = hovering ? 1.0f : 0.0f;
    if (!hovering) {
        clearHoverToolbarRects();
        m_hoverGroupBadge = false;
        m_hoverCopyFloating = false;
    }
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, 1);
    }
    render();
}

void PinWindow::clearHoverToolbarRects() noexcept {
    m_toolbarRect = {};
    m_btnFoldRect = {};
    m_btnRotateRect = {};
    m_btnInvertRect = {};
    m_btnEditRect = {};
    m_btnTextRect = {};
    m_btnCopyRect = {};
    m_btnSaveRect = {};
    m_btnCloseRect = {};
    m_hoverFold = false;
    m_hoverRotate = false;
    m_hoverInvert = false;
    m_hoverEdit = false;
    m_hoverText = false;
    m_hoverCopy = false;
    m_hoverSave = false;
    m_hoverClose = false;
}

int PinWindow::hitTestInteractiveButton(int x, int y) const noexcept {
    if (m_folded) {
        return kPinBtnNone;
    }
    if (isPointInRectF(m_btnGroupBadgeRect, x, y)) {
        return kPinBtnGroupBadge;
    }
    if (!m_selectedBlockIndices.empty() && isPointInRectF(m_btnCopyFloatingRect, x, y)) {
        return kPinBtnCopyFloating;
    }
    if (m_hoverAlpha > 0.0f && m_toolbarRect.right > m_toolbarRect.left) {
        if (isPointInRectF(m_btnFoldRect, x, y))   return kPinBtnFold;
        if (isPointInRectF(m_btnRotateRect, x, y)) return kPinBtnRotate;
        if (isPointInRectF(m_btnInvertRect, x, y)) return kPinBtnInvert;
        if (isPointInRectF(m_btnEditRect, x, y))   return kPinBtnEdit;
        if (isPointInRectF(m_btnTextRect, x, y))   return kPinBtnText;
        if (isPointInRectF(m_btnCopyRect, x, y))   return kPinBtnCopy;
        if (isPointInRectF(m_btnSaveRect, x, y))   return kPinBtnSave;
        if (isPointInRectF(m_btnCloseRect, x, y))  return kPinBtnClose;
    }
    return kPinBtnNone;
}

static bool copyImageToClipboard(const cv::Mat& image);
static bool savePinnedImage(HWND owner, const cv::Mat& image);

void PinWindow::executeInteractiveButton(int btnId, int clientX, int clientY, bool shiftHeld) {
    if (m_folded) return;
    switch (btnId) {
        case kPinBtnGroupBadge: {
            if (m_hwnd && IsWindow(m_hwnd)) {
                POINT screenPt{clientX, clientY};
                ClientToScreen(m_hwnd, &screenPt);
                showGroupPopupMenu(screenPt);
            }
            break;
        }
        case kPinBtnCopyFloating:
            copySelectedText();
            break;
        case kPinBtnFold:
            toggleFold();
            break;
        case kPinBtnRotate:
            rotate(shiftHeld ? 270 : 90);
            break;
        case kPinBtnInvert:
            invertColors();
            break;
        case kPinBtnEdit:
            editMarkup();
            break;
        case kPinBtnText:
            if (!m_selectedBlockIndices.empty()) {
                copySelectedText();
            } else if (!m_pinnedSourceText.empty()) {
                copyAllText();
            } else {
                toggleTextSelectionMode();
            }
            break;
        case kPinBtnCopy:
            copyImageToClipboard(m_sourceImage);
            tools3000::core::EventBus::instance().publish(
                tools3000::core::ShowToastEvent{
                    tools3000::core::isUiLanguageChinese() ? L"已复制贴图到剪贴板" : L"Copied pin to clipboard"});
            break;
        case kPinBtnSave:
            if (m_hwnd && IsWindow(m_hwnd)) {
                savePinnedImage(m_hwnd, m_sourceImage);
            }
            break;
        case kPinBtnClose:
            close();
            break;
        default:
            break;
    }
}

void PinWindow::setOpacity(float opacity) {
    m_opacity = std::clamp(opacity, 0.1f, 1.0f);
    applyLayeredOpacity();
}

void PinWindow::setClickThrough(bool enable) {
    if (!m_hwnd) return;
    LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (enable) ex |= WS_EX_TRANSPARENT;
    else        ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex);
    m_clickThrough = enable;
    if (!enable) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    applyLayeredOpacity();
    render();
    LOG_INFO("贴图鼠标穿透: {}", enable ? "开启" : "关闭");
    const bool isZh = tools3000::core::isUiLanguageChinese();
    tools3000::core::EventBus::instance().publish(
        tools3000::core::ShowToastEvent{isZh ? (enable ? L"贴图已开启鼠标穿透 (按 F4 或悬浮按 Ctrl+Alt+Shift+X 恢复)"
                                                  : L"贴图已退出鼠标穿透模式")
                                        : (enable ? L"Click-through enabled (F4 or Ctrl+Alt+Shift+X to restore)"
                                                  : L"Click-through disabled")});
}

bool PinWindow::toggleClickThroughUnderCursor() {
    POINT pt;
    GetCursorPos(&pt);
    // 倒序遍历（后创建的视为更上层）：找到光标矩形命中的贴图并切换其穿透态
    for (auto it = s_instances.rbegin(); it != s_instances.rend(); ++it) {
        auto& pin = *it;
        if (!pin || !pin->m_hwnd) continue;
        RECT rc;
        if (GetWindowRect(pin->m_hwnd, &rc) && PtInRect(&rc, pt)) {
            pin->setClickThrough(!pin->m_clickThrough);
            return true;
        }
    }
    // 兜底容错：如果光标未直接命中贴图，但存在处于穿透状态的贴图，恢复最近一个穿透贴图
    for (auto it = s_instances.rbegin(); it != s_instances.rend(); ++it) {
        auto& pin = *it;
        if (pin && pin->m_hwnd && pin->m_clickThrough) {
            pin->setClickThrough(false);
            return true;
        }
    }
    return false;
}

void PinWindow::cancelAllClickThrough() {
    for (auto& pin : s_instances) {
        if (pin && pin->m_hwnd && pin->m_clickThrough) {
            pin->setClickThrough(false);
        }
    }
}

// 将 cv::Mat 以标准底向上 CF_DIB 与 PNG 写入剪贴板
static bool copyImageToClipboard(const cv::Mat& image) {
    if (image.empty()) return false;
    return copyMatToClipboard(image);
}

static bool savePinnedImage(HWND owner, const cv::Mat& image) {
    if (image.empty()) return false;
    const bool zh = tools3000::core::isUiLanguageChinese();

    ComPtr<IFileSaveDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ERROR("贴图保存对话框创建失败: hr=0x{:08X}", static_cast<unsigned>(hr));
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{zh ? L"无法打开保存对话框" : L"Unable to open save dialog"});
        return false;
    }

    const COMDLG_FILTERSPEC filters[] = {
        {zh ? L"PNG 图片 (*.png)" : L"PNG Image (*.png)", L"*.png"},
        {zh ? L"JPEG 图片 (*.jpg;*.jpeg)" : L"JPEG Image (*.jpg;*.jpeg)", L"*.jpg;*.jpeg"},
        {zh ? L"WebP 图片 (*.webp)" : L"WebP Image (*.webp)", L"*.webp"},
        {zh ? L"BMP 图片 (*.bmp)" : L"BMP Image (*.bmp)", L"*.bmp"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetDefaultExtension(L"png");

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t defaultName[80]{};
    swprintf_s(defaultName, L"Tools3000_%04u%02u%02u_%02u%02u%02u.png",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    dialog->SetFileName(defaultName);

    hr = dialog->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return false;
    if (FAILED(hr)) {
        LOG_ERROR("贴图保存对话框失败: hr=0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return false;
    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) return false;
    std::filesystem::path path(rawPath);
    CoTaskMemFree(rawPath);

    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (ext.empty()) {
        path += L".png";
        ext = L".png";
    }
    if (ext == L".jpeg") ext = L".jpg";
    const std::string encodeExt = tools3000::core::WinUtils::wstringToUtf8(ext);
    const bool supported = encodeExt == ".png" || encodeExt == ".jpg" ||
                           encodeExt == ".webp" || encodeExt == ".bmp";
    if (!supported) {
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{zh ? L"不支持该图片格式" : L"Unsupported image format"});
        return false;
    }

    try {
        std::vector<uchar> encoded;
        std::vector<int> params;
        if (encodeExt == ".jpg") params = {cv::IMWRITE_JPEG_QUALITY, 95};
        else if (encodeExt == ".webp") params = {cv::IMWRITE_WEBP_QUALITY, 95};
        if (!cv::imencode(encodeExt, image, encoded, params)) return false;

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size()));
        if (!file) throw std::runtime_error("write failed");
        LOG_INFO("贴图已保存: {}", tools3000::core::WinUtils::wstringToUtf8(path.wstring()));
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{zh ? L"贴图已保存" : L"Pinned image saved"});
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("贴图保存失败: {}", e.what());
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{zh ? L"贴图保存失败" : L"Failed to save pinned image"});
        return false;
    }
}

void PinWindow::setScale(float scale) {
    m_scale = std::clamp(scale, 0.1f, 5.0f);
    if (m_hwnd) {
        if (m_folded) {
            return;
        }
        int w = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
        int h = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
        SetWindowPos(m_hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        updateWindowCornerRegion();

        if (m_renderTarget) {
            m_renderTarget->Resize(D2D1::SizeU(w, h));
        }
        render();
    }
}

void PinWindow::rotate(int angleDeg) {
    if (m_sourceImage.empty() || !m_hwnd) return;
    if (PinInputController::normalizeRightAngleDegrees(angleDeg) == 0) return;

    RECT baseRect{};
    GetWindowRect(m_hwnd, &baseRect);
    if (m_folded) {
        const int prevW = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
        const int prevH = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
        baseRect.right = baseRect.left + prevW;
        baseRect.bottom = baseRect.top + prevH;
        m_folded = false;
    }

    const int prevOrigW = m_origWidth;
    const int prevOrigH = m_origHeight;
    if (!PinInputController::applyRotation(m_sourceImage, m_colorBackup, m_origWidth, m_origHeight, angleDeg)) {
        return;
    }
    if (!m_cleanBaseImage.empty()) {
        cv::Mat dummyBackup;
        int cleanW = m_cleanBaseImage.cols;
        int cleanH = m_cleanBaseImage.rows;
        PinInputController::applyRotation(m_cleanBaseImage, dummyBackup, cleanW, cleanH, angleDeg);
    }
    if (!m_markupElements.empty() && prevOrigW > 0 && prevOrigH > 0) {
        const int normDeg = PinInputController::normalizeRightAngleDegrees(angleDeg);
        auto rotPt = [&](cv::Point& pt) {
            const int px = pt.x;
            const int py = pt.y;
            if (normDeg == 90) {
                pt = {prevOrigH - 1 - py, px};
            } else if (normDeg == 180) {
                pt = {prevOrigW - 1 - px, prevOrigH - 1 - py};
            } else if (normDeg == 270) {
                pt = {py, prevOrigW - 1 - px};
            }
        };
        for (auto& el : m_markupElements) {
            rotPt(el.startPt);
            rotPt(el.endPt);
            for (auto& p : el.penPoints) rotPt(p);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_ocrBlocks.clear();
        m_selectedBlockIndices.clear();
        m_hoveredBlockIndex = -1;
        m_ocrReady = false;
    }
    invalidateOcrStateAndRefreshIfSelecting();

    const int w = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
    const int h = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
    HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    const RECT workArea = tools3000::core::dpi::workArea(mon);
    const RECT targetRect = PinInputController::calculateRotatedWindowRect(baseRect, w, h, workArea);
    m_unfoldedRect = targetRect;

    SetWindowPos(m_hwnd, nullptr,
                 targetRect.left, targetRect.top,
                 w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    updateWindowCornerRegion();
    createRenderResources(m_sourceImage);
    render();
}

void PinWindow::flip(bool horizontal) {
    if (m_sourceImage.empty() || !m_hwnd) return;
    if (!PinInputController::applyFlip(m_sourceImage, m_colorBackup, horizontal)) {
        return;
    }
    if (!m_cleanBaseImage.empty()) {
        cv::Mat dummyBackup;
        PinInputController::applyFlip(m_cleanBaseImage, dummyBackup, horizontal);
    }
    if (!m_markupElements.empty() && m_origWidth > 0 && m_origHeight > 0) {
        auto flipPt = [&](cv::Point& pt) {
            if (horizontal) pt.x = m_origWidth - 1 - pt.x;
            else pt.y = m_origHeight - 1 - pt.y;
        };
        for (auto& el : m_markupElements) {
            flipPt(el.startPt);
            flipPt(el.endPt);
            for (auto& p : el.penPoints) flipPt(p);
        }
    }
    if (m_folded) {
        m_folded = false;
        RECT curRc{};
        GetWindowRect(m_hwnd, &curRc);
        const int w = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
        const int h = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        const RECT workArea = tools3000::core::dpi::workArea(mon);
        int posX = curRc.left;
        int posY = curRc.top;
        if (posX + w > workArea.right) posX = (std::max)(static_cast<int>(workArea.left), static_cast<int>(workArea.right - w));
        if (posY + h > workArea.bottom) posY = (std::max)(static_cast<int>(workArea.top), static_cast<int>(workArea.bottom - h));
        m_unfoldedRect = RECT{posX, posY, posX + w, posY + h};
        SetWindowPos(m_hwnd, nullptr, posX, posY, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    updateWindowCornerRegion();
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_ocrBlocks.clear();
        m_selectedBlockIndices.clear();
        m_hoveredBlockIndex = -1;
        m_ocrReady = false;
    }
    invalidateOcrStateAndRefreshIfSelecting();
    createRenderResources(m_sourceImage);
    render();
}

void PinWindow::invertColors() {
    if (m_sourceImage.empty() || !m_hwnd) return;
    auto invertMat = [](cv::Mat& mat) {
        if (mat.channels() == 4) {
            std::vector<cv::Mat> channels;
            cv::split(mat, channels);
            channels[0] = 255 - channels[0];
            channels[1] = 255 - channels[1];
            channels[2] = 255 - channels[2];
            cv::merge(channels, mat);
        } else if (mat.channels() == 3) {
            mat = cv::Scalar(255, 255, 255) - mat;
        }
    };
    invertMat(m_sourceImage);
    if (!m_colorBackup.empty()) {
        invertMat(m_colorBackup);
    }
    m_inverted = !m_inverted;
    createRenderResources(m_sourceImage);
    render();
}

void PinWindow::toggleGrayscale() {
    if (m_sourceImage.empty() || !m_hwnd) return;
    if (!m_grayscale) {
        m_colorBackup = m_sourceImage.clone();
        if (m_sourceImage.channels() == 4) {
            cv::Mat bgr, gray, grayBgra;
            cv::cvtColor(m_sourceImage, bgr, cv::COLOR_BGRA2BGR);
            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
            cv::cvtColor(gray, grayBgra, cv::COLOR_GRAY2BGRA);
            std::vector<cv::Mat> origChannels, grayChannels;
            cv::split(m_sourceImage, origChannels);
            cv::split(grayBgra, grayChannels);
            grayChannels[3] = origChannels[3];
            cv::merge(grayChannels, m_sourceImage);
        } else if (m_sourceImage.channels() == 3) {
            cv::Mat gray;
            cv::cvtColor(m_sourceImage, gray, cv::COLOR_BGR2GRAY);
            cv::cvtColor(gray, m_sourceImage, cv::COLOR_GRAY2BGR);
        }
        m_grayscale = true;
    } else {
        if (!m_colorBackup.empty()) {
            m_sourceImage = m_colorBackup.clone();
            m_colorBackup.release();
        }
        m_grayscale = false;
    }
    createRenderResources(m_sourceImage);
    render();
}

void PinWindow::resetScale() {
    setScale(1.0f);
}

void PinWindow::toggleFold() {
    if (!m_hwnd) return;
    const float dpiScale = m_hwnd ? tools3000::core::dpi::scaleForWindow(m_hwnd) : m_dpiScale;
    m_dpiScale = dpiScale;
    if (!m_folded) {
        GetWindowRect(m_hwnd, &m_unfoldedRect);
        m_folded = true;
        const SIZE foldSz = computeFoldCapsuleSize(dpiScale);
        const int foldW = foldSz.cx;
        const int foldH = foldSz.cy;
        SetWindowPos(m_hwnd, nullptr, m_unfoldedRect.left, m_unfoldedRect.top, foldW, foldH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (m_renderTarget) {
            m_renderTarget->Resize(D2D1::SizeU(static_cast<UINT>(foldW), static_cast<UINT>(foldH)));
        }
        tools3000::core::WinUtils::applyUniversalRoundedCorners(
            m_hwnd, foldW, foldH, static_cast<int>(std::round(8.0f * dpiScale)));
        render();
    } else {
        m_folded = false;
        RECT curRc{};
        GetWindowRect(m_hwnd, &curRc);
        const int w = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
        const int h = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        RECT workArea = tools3000::core::dpi::workArea(mon);
        int posX = curRc.left;
        int posY = curRc.top;
        if (posX + w > workArea.right) posX = (std::max)(static_cast<int>(workArea.left), static_cast<int>(workArea.right - w));
        if (posY + h > workArea.bottom) posY = (std::max)(static_cast<int>(workArea.top), static_cast<int>(workArea.bottom - h));
        m_unfoldedRect = RECT{posX, posY, posX + w, posY + h};
        SetWindowPos(m_hwnd, nullptr, posX, posY, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        updateWindowCornerRegion();
        if (m_renderTarget) {
            m_renderTarget->Resize(D2D1::SizeU(w, h));
        }
        render();
    }
}

void PinWindow::editMarkup() {
    if (!m_hwnd || m_sourceImage.empty()) return;

    RECT rc{};
    GetWindowRect(m_hwnd, &rc);
    CaptureRegion reg{rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top};

    const cv::Mat baseForEdit = (!m_cleanBaseImage.empty() &&
                                 m_cleanBaseImage.cols == m_origWidth &&
                                 m_cleanBaseImage.rows == m_origHeight)
        ? m_cleanBaseImage
        : m_sourceImage;

    ShowWindow(m_hwnd, SW_HIDE);
    std::weak_ptr<PinWindow> weakSelf = shared_from_this();
    CaptureOverlay::instance().startEditPinnedMarkup(
        baseForEdit,
        m_markupElements,
        reg,
        [weakSelf](bool accepted,
                   const cv::Mat& newCleanBase,
                   const cv::Mat& compositedImage,
                   std::vector<MarkupElement> updatedElements) {
            if (auto self = weakSelf.lock()) {
                if (accepted && !newCleanBase.empty()) {
                    self->applyEditedMarkup(newCleanBase, updatedElements, compositedImage);
                }
                if (self->m_hwnd && IsWindow(self->m_hwnd)) {
                    ShowWindow(self->m_hwnd, SW_SHOW);
                    self->focusWindow();
                }
            }
        });
}

static void cleanupOcrWorkingSet() {
    tools3000::core::WinUtils::trimWorkingSet();
}

void PinWindow::invalidateOcrStateAndRefreshIfSelecting() {
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_ocrBlocks.clear();
        m_selectedBlockIndices.clear();
        m_hoveredBlockIndex = -1;
        m_pendingSelectAllOcr = false;
        ++m_ocrVersion;
        m_ocrInFlightVersion = 0;
        if (!m_pinnedSourceText.empty()) {
            m_ocrBlocks.push_back(PinOcrBlock{
                tools3000::core::WinUtils::wstringToUtf8(m_pinnedSourceText),
                cv::Rect(4, 4, (std::max)(1, m_origWidth - 8), (std::max)(1, m_origHeight - 8))
            });
            m_ocrReady = true;
        } else {
            m_ocrReady = false;
        }
    }
    if (m_textSelectionMode && m_pinnedSourceText.empty()) {
        ensureOcrExecutedAsync();
    }
}

void PinWindow::ensureOcrExecutedAsync() {
    if (s_ocrShuttingDown.load(std::memory_order_acquire) ||
        m_sourceImage.empty() || !m_pinnedSourceText.empty() || m_ocrReady.load()) {
        return;
    }
    uint64_t currentVer = 0;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (s_ocrShuttingDown.load(std::memory_order_acquire) || m_ocrReady.load() || m_sourceImage.empty()) {
            return;
        }
        currentVer = m_ocrVersion.load();
        if (currentVer == 0) currentVer = ++m_ocrVersion;
        if (m_ocrInFlightVersion.load() == currentVer) return;
        m_ocrInFlightVersion.store(currentVer);
    }
    ++m_ocrRunCount;
    cv::Mat imgCopy = m_sourceImage.clone();
    std::weak_ptr<PinWindow> weakSelf = shared_from_this();
    auto activeWorkers = m_ocrActiveWorkers;
    if (activeWorkers) {
        activeWorkers->fetch_add(1, std::memory_order_acq_rel);
    }
    s_globalActiveOcrWorkers.fetch_add(1, std::memory_order_acq_rel);

    auto applyRecognizedBlocks = [](const std::shared_ptr<PinWindow>& self,
                                    const std::vector<tools3000::ocr::OcrResult>& results,
                                    uint64_t expectedVer) {
        if (self->m_ocrVersion.load() != expectedVer) return;
        self->m_ocrBlocks.clear();
        self->m_ocrBlocks.reserve(results.size());
        for (const auto& r : results) {
            if (!r.text.empty()) {
                self->m_ocrBlocks.push_back({r.text, r.box});
            }
        }
        self->m_ocrReady = true;
        if (self->m_pendingSelectAllOcr.exchange(false)) {
            if (!self->m_ocrBlocks.empty()) {
                self->m_textSelectionMode = true;
                self->m_selectedBlockIndices.resize(self->m_ocrBlocks.size());
                for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                    self->m_selectedBlockIndices[i] = i;
                }
                const bool isZh = tools3000::core::isUiLanguageChinese();
                tools3000::core::EventBus::instance().publish(
                    tools3000::core::ShowToastEvent{isZh ? L"已全选文字，按 Ctrl+C 复制"
                                                         : L"All text selected, press Ctrl+C to copy"});
            } else {
                const bool isZh = tools3000::core::isUiLanguageChinese();
                tools3000::core::EventBus::instance().publish(
                    tools3000::core::ShowToastEvent{isZh ? L"未识别到文字"
                                                         : L"No text recognized"});
            }
        }
        if (self->m_hwnd && IsWindow(self->m_hwnd)) {
            InvalidateRect(self->m_hwnd, nullptr, FALSE);
        }
    };

    try {
        std::thread([weakSelf, imgCopy, currentVer, activeWorkers, applyRecognizedBlocks]() {
            bool inFlightHandedOff = false;
            struct WorkerScopeGuard {
                std::weak_ptr<PinWindow> weakPin;
                uint64_t ver;
                bool* handedOff;
                std::shared_ptr<std::atomic<uint32_t>> workers;
                ~WorkerScopeGuard() {
                    if (handedOff && !*handedOff) {
                        if (auto self = weakPin.lock()) {
                            uint64_t expected = ver;
                            self->m_ocrInFlightVersion.compare_exchange_strong(expected, 0);
                        }
                    }
                    cleanupOcrWorkingSet();
                    if (workers) {
                        workers->fetch_sub(1, std::memory_order_acq_rel);
                    }
                    s_globalActiveOcrWorkers.fetch_sub(1, std::memory_order_acq_rel);
                }
            } workerGuard{weakSelf, currentVer, &inFlightHandedOff, activeWorkers};

            if (s_ocrShuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            tools3000::core::TraceId::Scope traceScope;
            auto results = tools3000::ocr::OcrEngine::instance().extractText(imgCopy);
            if (s_ocrShuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            const bool posted = tools3000::core::MainThreadDispatcher::instance().post(
                [weakSelf, results, currentVer, applyRecognizedBlocks]() {
                    if (s_ocrShuttingDown.load(std::memory_order_acquire)) return;
                    if (auto self = weakSelf.lock()) {
                        uint64_t expected = currentVer;
                        self->m_ocrInFlightVersion.compare_exchange_strong(expected, 0);
                        if (self->m_ocrVersion.load() != currentVer) return;
                        std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                        applyRecognizedBlocks(self, results, currentVer);
                    }
                });
            if (posted) {
                inFlightHandedOff = true;
            } else if (!s_ocrShuttingDown.load(std::memory_order_acquire)) {
                if (auto self = weakSelf.lock()) {
                    std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                    uint64_t expected = currentVer;
                    self->m_ocrInFlightVersion.compare_exchange_strong(expected, 0);
                    inFlightHandedOff = true;
                    applyRecognizedBlocks(self, results, currentVer);
                }
            }
        }).detach();
    } catch (...) {
        uint64_t expected = currentVer;
        m_ocrInFlightVersion.compare_exchange_strong(expected, 0);
        if (activeWorkers) {
            activeWorkers->fetch_sub(1, std::memory_order_acq_rel);
        }
        s_globalActiveOcrWorkers.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void PinWindow::selectAllText() {
    if (m_folded) return;
    if (!m_ocrReady.load()) {
        m_pendingSelectAllOcr.store(true);
        ensureOcrExecutedAsync();
        return;
    }

    size_t countSelected = 0;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (!m_ocrBlocks.empty()) {
            m_textSelectionMode = true;
            m_selectedBlockIndices.resize(m_ocrBlocks.size());
            for (size_t i = 0; i < m_ocrBlocks.size(); ++i) {
                m_selectedBlockIndices[i] = i;
            }
            countSelected = m_selectedBlockIndices.size();
        }
    }

    const bool isZh = tools3000::core::isUiLanguageChinese();
    if (countSelected > 0) {
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"已全选文字，按 Ctrl+C 复制"
                                                 : L"All text selected, press Ctrl+C to copy"});
        render();
    } else {
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"未识别到文字"
                                                 : L"No text recognized"});
    }
}

size_t PinWindow::selectedTextBlockCount() const {
    std::lock_guard<std::mutex> lock(m_ocrMutex);
    return m_selectedBlockIndices.size();
}

void PinWindow::setOcrBlocksForTest(std::vector<PinOcrBlock> blocks) {
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_ocrBlocks = std::move(blocks);
        m_selectedBlockIndices.clear();
        m_hoveredBlockIndex = -1;
        m_ocrReady = true;
        m_pendingSelectAllOcr = false;
        ++m_ocrVersion;
        m_ocrInFlightVersion = 0;
    }
    render();
}

void PinWindow::toggleTextSelectionMode() {
    m_textSelectionMode = !m_textSelectionMode;
    if (m_textSelectionMode && !m_ocrReady.load()) {
        ensureOcrExecutedAsync();
    }
    if (!m_textSelectionMode) {
        clearTextSelection();
    }
    const bool isZh = tools3000::core::isUiLanguageChinese();
    tools3000::core::EventBus::instance().publish(
        tools3000::core::ShowToastEvent{m_textSelectionMode
            ? (isZh ? L"已进入文字划选模式（拖拽选择文字，按 Esc 退出）" : L"Text selection mode (Drag to select, Esc to exit)")
            : (isZh ? L"已退出文字划选模式" : L"Exited text selection mode")});
    render();
}

void PinWindow::clearTextSelection() {
    std::lock_guard<std::mutex> lock(m_ocrMutex);
    m_selectedBlockIndices.clear();
    m_hoveredBlockIndex = -1;
    m_isSelectingText = false;
}

void PinWindow::copySelectedText() {
    std::string combined;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (m_selectedBlockIndices.empty()) return;

        std::vector<tools3000::ocr::OcrResult> selectedBlocks;
        selectedBlocks.reserve(m_selectedBlockIndices.size());
        for (size_t idx : m_selectedBlockIndices) {
            if (idx < m_ocrBlocks.size() && !m_ocrBlocks[idx].text.empty()) {
                tools3000::ocr::OcrResult r;
                r.text = m_ocrBlocks[idx].text;
                r.confidence = 1.0f;
                r.box = m_ocrBlocks[idx].origBox;
                selectedBlocks.push_back(std::move(r));
            }
        }
        combined = tools3000::ocr::OcrEngine::orderTextBlocksWithXYCut(selectedBlocks, "\r\n");
        m_selectedBlockIndices.clear();
    }

    if (!combined.empty()) {
        std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(combined);
        tools3000::core::WinUtils::copyToClipboard(wtext);
        bool isZh = tools3000::core::isUiLanguageChinese();
        std::wstring preview = wtext.size() > 20 ? (wtext.substr(0, 20) + L"...") : wtext;
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? (L"已复制所选文字: " + preview) : (L"Copied selected text: " + preview)});
    }
    render();
}

void PinWindow::copyAllText() {
    if (!m_pinnedSourceText.empty()) {
        tools3000::core::WinUtils::copyToClipboard(m_pinnedSourceText);
        const bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"已复制贴图文字" : L"Copied note text"});
        return;
    }

    std::string combined;
    bool hasReadyBlocks = false;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (m_ocrReady && !m_ocrBlocks.empty()) {
            hasReadyBlocks = true;
            std::vector<tools3000::ocr::OcrResult> allBlocks;
            allBlocks.reserve(m_ocrBlocks.size());
            for (const auto& blk : m_ocrBlocks) {
                if (!blk.text.empty()) {
                    tools3000::ocr::OcrResult r;
                    r.text = blk.text;
                    r.confidence = 1.0f;
                    r.box = blk.origBox;
                    allBlocks.push_back(std::move(r));
                }
            }
            combined = tools3000::ocr::OcrEngine::orderTextBlocksWithXYCut(allBlocks, "\r\n");
        }
    }

    if (hasReadyBlocks && !combined.empty()) {
        std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(combined);
        tools3000::core::WinUtils::copyToClipboard(wtext);
        bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"已复制贴图全部文字" : L"Copied all text from pin"});
    } else {
        extractText();
    }
}

void PinWindow::extractText() {
    if (!m_pinnedSourceText.empty()) {
        tools3000::core::WinUtils::copyToClipboard(m_pinnedSourceText);
        const bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"已复制贴图文字" : L"Copied note text"});
        return;
    }
    if (s_ocrShuttingDown.load(std::memory_order_acquire) || !isAlive() || m_sourceImage.empty()) return;

    // 单窗口 OCR 进行中防重入守卫，杜绝连按重复拉起多线程识别
    bool expectedInFlight = false;
    if (!m_extractTextInFlight.compare_exchange_strong(expectedInFlight, true, std::memory_order_acq_rel)) {
        return;
    }

    std::weak_ptr<PinWindow> weakSelf;
    try {
        weakSelf = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        m_extractTextInFlight.store(false, std::memory_order_release);
        return;
    }

    const uint64_t expectedVersion = m_ocrVersion.load(std::memory_order_acquire);
    cv::Mat imgCopy = m_sourceImage.clone();
    auto activeWorkers = m_ocrActiveWorkers;
    if (activeWorkers) {
        activeWorkers->fetch_add(1, std::memory_order_acq_rel);
    }
    s_globalActiveOcrWorkers.fetch_add(1, std::memory_order_acq_rel);

    try {
        std::thread([imgCopy, activeWorkers, weakSelf, expectedVersion]() {
            struct ExtractWorkerGuard {
                std::shared_ptr<std::atomic<uint32_t>> workers;
                std::weak_ptr<PinWindow> owner;
                ~ExtractWorkerGuard() {
                    if (auto pin = owner.lock()) {
                        pin->m_extractTextInFlight.store(false, std::memory_order_release);
                    }
                    cleanupOcrWorkingSet();
                    if (workers) {
                        workers->fetch_sub(1, std::memory_order_acq_rel);
                    }
                    s_globalActiveOcrWorkers.fetch_sub(1, std::memory_order_acq_rel);
                }
            } workerGuard{activeWorkers, weakSelf};

            if (s_ocrShuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            if (auto pin = weakSelf.lock(); !pin || !pin->isAlive() ||
                pin->m_ocrVersion.load(std::memory_order_acquire) != expectedVersion) {
                return;
            }
            tools3000::core::TraceId::Scope traceScope;
            std::string text = tools3000::ocr::OcrEngine::instance().recognizeToText(imgCopy);
            if (s_ocrShuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            tools3000::core::MainThreadDispatcher::instance().post([text, weakSelf, expectedVersion]() {
                if (s_ocrShuttingDown.load(std::memory_order_acquire)) return;
                auto pin = weakSelf.lock();
                if (!pin || !pin->isAlive() ||
                    pin->m_ocrVersion.load(std::memory_order_acquire) != expectedVersion) {
                    return;
                }
                bool isZh = tools3000::core::isUiLanguageChinese();
                if (!text.empty()) {
                    std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(text);
                    tools3000::core::WinUtils::copyToClipboard(wtext);
                    tools3000::core::EventBus::instance().publish(
                        tools3000::core::ShowToastEvent{isZh ? L"已提取文本并复制到剪贴板" : L"Text extracted and copied to clipboard"});
                } else {
                    tools3000::core::EventBus::instance().publish(
                        tools3000::core::ShowToastEvent{isZh ? L"未识别到文字" : L"No text recognized"});
                }
            });
        }).detach();
    } catch (...) {
        m_extractTextInFlight.store(false, std::memory_order_release);
        if (activeWorkers) {
            activeWorkers->fetch_sub(1, std::memory_order_acq_rel);
        }
        s_globalActiveOcrWorkers.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void PinWindow::updateImage(const cv::Mat& newImage, bool clearCardMetadata) {
    if (newImage.empty() || !m_hwnd) return;
    m_colorBackup.release();
    m_grayscale = false;
    m_inverted = false;
    if (clearCardMetadata) {
        m_cardCornerRadius = 0.0f;
        m_pinnedSourceText.clear();
    }
    m_sourceImage = newImage.clone();
    m_cleanBaseImage = newImage.clone();
    m_markupElements.clear();
    m_origWidth = newImage.cols;
    m_origHeight = newImage.rows;

    if (m_pinnedSourceText.empty()) {
        {
            std::lock_guard<std::mutex> lock(m_ocrMutex);
            m_ocrBlocks.clear();
            m_selectedBlockIndices.clear();
            m_hoveredBlockIndex = -1;
            m_ocrReady = false;
        }
        invalidateOcrStateAndRefreshIfSelecting();
    }

    const int newW = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
    const int newH = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
    if (!m_folded) {
        SetWindowPos(m_hwnd, nullptr, 0, 0, newW, newH, SWP_NOMOVE | SWP_NOZORDER);
        updateWindowCornerRegion();
    }

    createRenderResources(m_sourceImage);
    render();
}

void PinWindow::applyEditedMarkup(
    const cv::Mat& cleanBaseImage,
    const std::vector<MarkupElement>& elements,
    const cv::Mat& precomposited) {
    if (cleanBaseImage.empty() || !m_hwnd) return;

    m_colorBackup.release();
    m_grayscale = false;
    m_inverted = false;
    m_cleanBaseImage = cleanBaseImage.clone();
    m_markupElements = elements;

    cv::Mat composited;
    if (!precomposited.empty() &&
        precomposited.cols == m_cleanBaseImage.cols &&
        precomposited.rows == m_cleanBaseImage.rows) {
        composited = precomposited.clone();
    } else if (m_markupElements.empty()) {
        composited = m_cleanBaseImage.clone();
    } else {
        MarkupEngine engine;
        cv::Mat baseBgr = m_cleanBaseImage;
        if (baseBgr.channels() == 4) {
            cv::cvtColor(baseBgr, baseBgr, cv::COLOR_BGRA2BGR);
        } else if (baseBgr.channels() == 1) {
            cv::cvtColor(baseBgr, baseBgr, cv::COLOR_GRAY2BGR);
        }
        engine.setBaseImage(baseBgr);
        engine.importElements(m_markupElements);
        composited = engine.getCompositeImage();
    }
    if (composited.empty()) {
        composited = m_cleanBaseImage.clone();
    }

    m_sourceImage = std::move(composited);
    m_origWidth = m_sourceImage.cols;
    m_origHeight = m_sourceImage.rows;

    if (m_pinnedSourceText.empty()) {
        {
            std::lock_guard<std::mutex> lock(m_ocrMutex);
            m_ocrBlocks.clear();
            m_selectedBlockIndices.clear();
            m_hoveredBlockIndex = -1;
            m_ocrReady = false;
        }
        invalidateOcrStateAndRefreshIfSelecting();
    }

    // 保持原始 1:1 m_origWidth/Height 与用户当前 m_scale 倍率恒定不变，彻底根除缩放平方膨胀与长图糊化
    const int keepW = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
    const int keepH = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
    if (!m_folded) {
        SetWindowPos(m_hwnd, nullptr, 0, 0, keepW, keepH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        updateWindowCornerRegion();
    }

    createRenderResources(m_sourceImage);
    render();
}

POINT PinWindow::calculateMagneticSnap(HWND currentHwnd, POINT targetPos, int curW, int curH) {
    POINT res = targetPos;
    const HMONITOR monitor = MonitorFromPoint(targetPos, MONITOR_DEFAULTTONEAREST);
    const RECT work = tools3000::core::dpi::workArea(monitor);
    const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
    const int snapDist = tools3000::core::dpi::scaleMetric(12, scale);

    // 1. 吸附到当前显示器工作区四边
    if (std::abs(res.x - work.left) <= snapDist) res.x = work.left;
    else if (std::abs(res.x + curW - work.right) <= snapDist) res.x = work.right - curW;

    if (std::abs(res.y - work.top) <= snapDist) res.y = work.top;
    else if (std::abs(res.y + curH - work.bottom) <= snapDist) res.y = work.bottom - curH;

    // 2. 吸附到其他贴图窗口边缘
    for (const auto& pin : s_instances) {
        if (!pin || !pin->isAlive() || pin->m_hwnd == currentHwnd) continue;
        if (!IsWindowVisible(pin->m_hwnd)) continue;

        RECT otherRc{};
        GetWindowRect(pin->m_hwnd, &otherRc);

        // X 轴吸附：左对齐、右对齐、左贴右、右贴左
        if (std::abs(res.x - otherRc.left) <= snapDist) {
            res.x = otherRc.left;
        } else if (std::abs(res.x + curW - otherRc.right) <= snapDist) {
            res.x = otherRc.right - curW;
        } else if (std::abs(res.x - otherRc.right) <= snapDist) {
            res.x = otherRc.right;
        } else if (std::abs(res.x + curW - otherRc.left) <= snapDist) {
            res.x = otherRc.left - curW;
        }

        // Y 轴吸附：顶对齐、底对齐、顶贴底、底贴顶
        if (std::abs(res.y - otherRc.top) <= snapDist) {
            res.y = otherRc.top;
        } else if (std::abs(res.y + curH - otherRc.bottom) <= snapDist) {
            res.y = otherRc.bottom - curH;
        } else if (std::abs(res.y - otherRc.bottom) <= snapDist) {
            res.y = otherRc.bottom;
        } else if (std::abs(res.y + curH - otherRc.top) <= snapDist) {
            res.y = otherRc.top - curH;
        }
    }

    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// 剪贴板贴图（图像 / 文本 / 颜色 → 浮空贴图）
// ─────────────────────────────────────────────────────────────────────────────

// 解析 #RGB / #RRGGBB 文本为 BGR 颜色（委托至 PinPasteCoordinator 单一事实源）
bool PinWindow::parseHexColor(const std::wstring& in, cv::Scalar& bgr, std::wstring& label) {
    return PinPasteCoordinator::parseHexColor(in, bgr, label);
}

// 颜色 → 色卡贴图（委托至 PinPasteCoordinator 单一事实源）
cv::Mat PinWindow::renderColorSwatch(const cv::Scalar& bgr, const std::wstring& label, float dpiScaleOverride) {
    return PinPasteCoordinator::renderColorSwatch(bgr, label, dpiScaleOverride);
}

// 文本/代码 → Mac 极客风卡片贴图（委托至 PinPasteCoordinator 单一事实源）
cv::Mat PinWindow::renderTextToImage(const std::wstring& text) {
    return PinPasteCoordinator::renderTextToImage(text);
}

POINT PinWindow::calculateSmartSpawnPosition(int imageWidth, int imageHeight, const POINT* fallbackCursor, const CaptureRegion* preferredRegion, int padX, int padY) {
    POINT cursor{};
    if (fallbackCursor) {
        cursor = *fallbackCursor;
    } else {
        GetCursorPos(&cursor);
    }

    POINT refPoint = cursor;
    if (preferredRegion && preferredRegion->width > 0 && preferredRegion->height > 0) {
        refPoint = POINT{preferredRegion->x + preferredRegion->width / 2,
                         preferredRegion->y + preferredRegion->height / 2};
    }

    HMONITOR mon = MonitorFromPoint(refPoint, MONITOR_DEFAULTTONEAREST);
    RECT work = tools3000::core::dpi::workArea(mon);

    int minVx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int minVy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int maxVw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int maxVh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT vScreen{minVx, minVy, minVx + maxVw, minVy + maxVh};

    PinSpawnInput input;
    input.imageWidth = imageWidth;
    input.imageHeight = imageHeight;
    input.padX = padX;
    input.padY = padY;
    input.cursor = cursor;
    input.workArea = work;
    input.virtualScreen = vScreen;

    if (preferredRegion && preferredRegion->width > 0 && preferredRegion->height > 0) {
        input.preferredRegion = PinSpawnRegion{
            preferredRegion->x, preferredRegion->y,
            preferredRegion->width, preferredRegion->height
        };
    }

    auto lastEntry = CaptureHistory::instance().get(0);
    if (lastEntry.has_value() && !lastEntry->image.empty()) {
        PinHistorySnapshot hist;
        hist.imageWidth = lastEntry->image.cols;
        hist.imageHeight = lastEntry->image.rows;
        hist.region = PinSpawnRegion{
            lastEntry->region.x, lastEntry->region.y,
            lastEntry->region.width, lastEntry->region.height
        };
        input.lastHistory = hist;
    }

    for (const auto& pin : s_instances) {
        if (!pin || !pin->isAlive() || !pin->m_hwnd || !IsWindowVisible(pin->m_hwnd)) continue;
        RECT rc{};
        GetWindowRect(pin->m_hwnd, &rc);
        input.existingPins.push_back(rc);
    }

    POINT spawnPos = PinSpawnCalculator::calculate(input);
    LOG_INFO("PinWindow: 智能计算最佳生成坐标 @ ({}, {})", spawnPos.x, spawnPos.y);
    return spawnPos;
}

std::shared_ptr<PinWindow> PinWindow::createFromClipboard() {
    POINT pt;
    GetCursorPos(&pt);

    std::wstring text;
    bool preferTextOverImage = false;
    {
        bool opened = false;
        for (int retry = 0; retry < 10; ++retry) {
            if (OpenClipboard(nullptr)) {
                opened = true;
                break;
            }
            Sleep(5);
        }
        if (opened) {
            const UINT formatPinMeta = RegisterClipboardFormatW(L"Tools3000_PinMetadata");
            const UINT formatPng = RegisterClipboardFormatW(L"PNG");
            const bool hasPinMeta = (formatPinMeta != 0 && IsClipboardFormatAvailable(formatPinMeta));
            if (!hasPinMeta) {
                for (UINT fmt = EnumClipboardFormats(0); fmt != 0; fmt = EnumClipboardFormats(fmt)) {
                    if (fmt == CF_UNICODETEXT || fmt == CF_TEXT || fmt == CF_OEMTEXT) {
                        preferTextOverImage = true;
                        break;
                    }
                    if ((formatPng != 0 && fmt == formatPng) ||
                        fmt == CF_DIBV5 || fmt == CF_DIB || fmt == CF_BITMAP) {
                        preferTextOverImage = false;
                        break;
                    }
                }
            }
            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                        text = p;
                        GlobalUnlock(h);
                    }
                }
            }
            CloseClipboard();
        }
    }

    Tools3000PinMetadata meta{};
    meta.magic = 0;
    cv::Mat img;
    bool generatedFromCard = false;
    float cardCornerRadius = 0.0f;
    std::wstring cardSourceText;

    HMONITOR curMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    const float monScale = std::clamp(tools3000::core::dpi::scaleForMonitor(curMon), 1.0f, 3.5f);

    auto renderTextOrColor = [&](const std::wstring& s) -> cv::Mat {
        if (s.empty()) return {};
        cv::Scalar c;
        std::wstring hexLabel;
        if (parseHexColor(s, c, hexLabel)) {
            cv::Mat swatch = renderColorSwatch(c, hexLabel, monScale);
            if (!swatch.empty()) {
                generatedFromCard = true;
                cardCornerRadius = 10.0f * monScale;
                cardSourceText = hexLabel;
            }
            return swatch;
        }
        cv::Mat note = PinPasteCoordinator::renderTextToImage(s, monScale);
        if (!note.empty()) {
            generatedFromCard = true;
            cardCornerRadius = 10.0f * monScale;
            const size_t firstNonNl = s.find_first_not_of(L"\r\n");
            const size_t lastNonWs = s.find_last_not_of(L" \t\r\n");
            if (firstNonNl != std::wstring::npos && lastNonWs != std::wstring::npos && lastNonWs >= firstNonNl) {
                cardSourceText = s.substr(firstNonNl, lastNonWs - firstNonNl + 1);
            } else {
                cardSourceText = s;
            }
        }
        return note;
    };

    if (preferTextOverImage && !text.empty()) {
        img = renderTextOrColor(text);
    }

    if (img.empty()) {
        img = ClipboardUtils::readImageFromClipboard(&meta);
    }

    if (img.empty() && !text.empty()) {
        img = renderTextOrColor(text);
    }

    if (img.empty()) {
        LOG_INFO("剪贴板贴图：剪贴板无可贴内容");
        return nullptr;
    }

    POINT spawnPos{};
    if (generatedFromCard) {
        PinSpawnInput spawnIn;
        spawnIn.imageWidth = img.cols;
        spawnIn.imageHeight = img.rows;
        spawnIn.cursor = pt;
        spawnIn.workArea = tools3000::core::dpi::workArea(curMon);
        spawnIn.virtualScreen = RECT{
            GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)
        };
        spawnIn.stackVerticallyOnIntersect = true;
        spawnIn.stackGap = tools3000::core::dpi::scaleMetric(8, monScale);
        for (const auto& pin : s_instances) {
            if (!pin || !pin->isAlive() || !pin->m_hwnd || !IsWindowVisible(pin->m_hwnd)) continue;
            RECT rc{};
            GetWindowRect(pin->m_hwnd, &rc);
            spawnIn.existingPins.push_back(rc);
        }
        spawnPos = PinSpawnCalculator::calculate(spawnIn);
    } else {
        CaptureRegion reg{};
        const CaptureRegion* pReg = nullptr;
        if (meta.magic == 0x54333030 && meta.regionW > 0 && meta.regionH > 0) {
            reg.x = meta.regionX;
            reg.y = meta.regionY;
            reg.width = meta.regionW;
            reg.height = meta.regionH;
            reg.cornerRadius = meta.cornerRadius;
            pReg = &reg;
        }
        spawnPos = calculateSmartSpawnPosition(img.cols, img.rows, &pt, pReg, meta.padX, meta.padY);
    }

    LOG_INFO("剪贴板贴图：{}x{} @ ({},{})", img.cols, img.rows, spawnPos.x, spawnPos.y);
    return create(img, spawnPos.x, spawnPos.y, cardCornerRadius, std::move(cardSourceText));
}

std::shared_ptr<PinWindow> PinWindow::pasteNextHistoryOrClipboard() {
    auto decision = PinPasteCoordinator::instance().stepNext();
    if (!decision.has_value()) return nullptr;
    const auto& d = *decision;
    auto pin = create(d.candidate.image, d.spawnPos.x, d.spawnPos.y,
                      d.candidate.cardCornerRadius, d.candidate.sourceText);

    if (d.totalCount > 1 && d.reverseIndex > 0) {
        bool isZh = tools3000::core::isUiLanguageChinese();
        std::wstring toastMsg;
        if (d.isLooped) {
            toastMsg = isZh ? std::format(L"历史贴图 1/{}（最新）", d.totalCount)
                            : std::format(L"Pin history 1/{} (Latest)", d.totalCount);
        } else {
            toastMsg = isZh ? std::format(L"历史贴图 {}/{}", d.targetIndex + 1, d.totalCount)
                            : std::format(L"Pin history {}/{}", d.targetIndex + 1, d.totalCount);
        }
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{toastMsg});
    }

    return pin;
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口初始化
// ─────────────────────────────────────────────────────────────────────────────

bool PinWindow::initWindow(HINSTANCE hInstance, int x, int y, int w, int h) {
    if (!s_classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS | 0x00020000 /*CS_DROPSHADOW*/;
        wc.lpfnWndProc = pinWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
        wc.lpszClassName = PIN_CLASS;
        RegisterClassExW(&wc);
        s_classRegistered = true;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        PIN_CLASS, L"",
        WS_POPUP,
        x, y, w, h,
        nullptr, nullptr, hInstance, this
    );

    if (!m_hwnd) return false;

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetPropW(m_hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
    m_dpiScale = tools3000::core::dpi::scaleForWindow(m_hwnd);
    applySharpWindowCorners();

    return true;
}

bool PinWindow::createRenderResources(const cv::Mat& image) {
    HRESULT hr;

    if (!m_d2dFactory) {
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf());
        if (FAILED(hr)) return false;
    }

    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const UINT targetW = static_cast<UINT>((std::max)(1L, rc.right - rc.left));
    const UINT targetH = static_cast<UINT>((std::max)(1L, rc.bottom - rc.top));

    if (!m_renderTarget) {
        auto rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        // DXGI 硬件交换链单边上限为 16384；若超长贴图窗口超过硬件交换链上限则钳制后备缓冲或回退软件渲染目标
        constexpr UINT kSafeInitMaxRtDim = 16384u;
        auto hwndProps = D2D1::HwndRenderTargetProperties(
            m_hwnd,
            D2D1::SizeU((std::min)(targetW, kSafeInitMaxRtDim), (std::min)(targetH, kSafeInitMaxRtDim)),
            D2D1_PRESENT_OPTIONS_IMMEDIATELY
        );

        hr = m_d2dFactory->CreateHwndRenderTarget(rtProps, hwndProps, m_renderTarget.GetAddressOf());
        if (FAILED(hr)) {
            auto swProps = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );
            hr = m_d2dFactory->CreateHwndRenderTarget(swProps, hwndProps, m_renderTarget.GetAddressOf());
            if (FAILED(hr)) return false;
        }

        // 禁用 D2D 的自动 DPI 缩放
        m_renderTarget->SetDpi(96.0f, 96.0f);
    } else {
        const UINT32 maxRt = (std::max)(256u, m_renderTarget->GetMaximumBitmapSize());
        m_renderTarget->Resize(D2D1::SizeU((std::min)(targetW, maxRt), (std::min)(targetH, maxRt)));
    }

    // 转为 BGRA 并构建单块或二维分块 D2D Bitmap 列表（支持 >16384px 超长截图贴图）
    cv::Mat bgra;
    if (image.channels() == 3) {
        cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
    } else if (image.channels() == 1) {
        cv::cvtColor(image, bgra, cv::COLOR_GRAY2BGRA);
    } else if (image.channels() == 4) {
        bgra = image.clone();
        for (int r = 0; r < bgra.rows; ++r) {
            auto* p = reinterpret_cast<cv::Vec4b*>(bgra.data + static_cast<size_t>(r) * bgra.step[0]);
            for (int c = 0; c < bgra.cols; ++c) {
                const uchar a = p[c][3];
                if (a == 0) {
                    p[c][0] = 0; p[c][1] = 0; p[c][2] = 0;
                } else if (a < 255) {
                    p[c][0] = static_cast<uchar>((p[c][0] * a) / 255);
                    p[c][1] = static_cast<uchar>((p[c][1] * a) / 255);
                    p[c][2] = static_cast<uchar>((p[c][2] * a) / 255);
                }
            }
        }
    } else {
        return false;
    }

    if (!bgra.isContinuous()) {
        bgra = bgra.clone();
    }

    D2D1_BITMAP_PROPERTIES bitmapProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    m_bitmap.Reset();
    m_bitmapTiles.clear();

    const UINT32 maxBmp = (std::max)(256u, m_renderTarget->GetMaximumBitmapSize());
    if (static_cast<UINT32>(bgra.cols) <= maxBmp && static_cast<UINT32>(bgra.rows) <= maxBmp) {
        hr = m_renderTarget->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(bgra.cols), static_cast<UINT32>(bgra.rows)),
            bgra.data,
            static_cast<UINT32>(bgra.step[0]),
            bitmapProps,
            m_bitmap.GetAddressOf()
        );
        if (SUCCEEDED(hr) && m_bitmap) {
            return true;
        }
        m_bitmap.Reset();
    }

    // 超出硬件最大单张纹理尺寸（如 >16384px 长截图或 8192px 显卡），自动切分为二维 ID2D1Bitmap 瓦片网格
    const int tileStep = static_cast<int>((std::min)(maxBmp, 4096u));
    for (int ty = 0; ty < bgra.rows; ty += tileStep) {
        const int th = (std::min)(tileStep, bgra.rows - ty);
        for (int tx = 0; tx < bgra.cols; tx += tileStep) {
            const int tw = (std::min)(tileStep, bgra.cols - tx);
            cv::Mat sub = bgra(cv::Rect(tx, ty, tw, th));
            if (!sub.isContinuous()) {
                sub = sub.clone();
            }
            PinBitmapTile tile;
            tile.x = tx;
            tile.y = ty;
            tile.width = tw;
            tile.height = th;
            hr = m_renderTarget->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(tw), static_cast<UINT32>(th)),
                sub.data,
                static_cast<UINT32>(sub.step[0]),
                bitmapProps,
                tile.bitmap.GetAddressOf()
            );
            if (FAILED(hr) || !tile.bitmap) {
                m_bitmapTiles.clear();
                return false;
            }
            m_bitmapTiles.push_back(std::move(tile));
        }
    }

    return !m_bitmapTiles.empty();
}

void PinWindow::render() {
    if ((!m_renderTarget || (!m_bitmap && m_bitmapTiles.empty())) &&
        !m_sourceImage.empty() && m_hwnd && IsWindow(m_hwnd)) {
        createRenderResources(m_sourceImage);
    }
    if (!m_renderTarget || (!m_bitmap && m_bitmapTiles.empty())) return;

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    const auto pal = FloatingGlassBar::resolveThemePalette();
    auto size = m_renderTarget->GetSize();
    if (m_folded) {
        const float dpiScale = m_hwnd ? tools3000::core::dpi::scaleForWindow(m_hwnd) : m_dpiScale;
        m_dpiScale = dpiScale;
        const tools3000::core::AccentColorRGB themeRgb = FloatingGlassBar::effectiveAccentRGB();

        ComPtr<ID2D1SolidColorBrush> bgBrush, textBrush, accentBrush;
        m_renderTarget->CreateSolidColorBrush(pal.panelBg, bgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.textPrimary, textBrush.GetAddressOf());
        const D2D1_COLOR_F foldAccent = (m_groupId >= PinGroupConstants::kMinGroupId)
            ? PinGroupPalette::accentColor(m_groupId, 0.96f)
            : D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.95f);
        m_renderTarget->CreateSolidColorBrush(foldAccent, accentBrush.GetAddressOf());

        const float foldStroke = 1.5f * dpiScale;
        const float foldInset = 0.75f * dpiScale;
        const float capsuleR = (std::max)(1.0f, 8.0f * dpiScale - foldInset);
        D2D1_ROUNDED_RECT capsule = D2D1::RoundedRect(
            D2D1::RectF(foldInset, foldInset,
                        (std::max)(foldInset, size.width - foldInset),
                        (std::max)(foldInset, size.height - foldInset)),
            capsuleR, capsuleR);
        if (bgBrush) m_renderTarget->FillRoundedRectangle(&capsule, bgBrush.Get());
        if (accentBrush) m_renderTarget->DrawRoundedRectangle(&capsule, accentBrush.Get(), foldStroke);

        // Fold 图标居左
        auto iconRect = D2D1::RectF(6.0f * dpiScale, 6.0f * dpiScale, 30.0f * dpiScale, size.height - 6.0f * dpiScale);
        CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionFold, iconRect, textBrush.Get(), dpiScale);

        // 文字居右（复用缓存 DWrite 12px 格式，强制 NO_WRAP）
        if (ensureTextResources(dpiScale) && m_uiLeadTextFormat && textBrush) {
            const bool isZh = tools3000::core::isUiLanguageChinese();
            const wchar_t* txt = isZh ? L"贴图 (双击展开)" : L"Pinned (Folded)";
            const D2D1_RECT_F textRect = D2D1::RectF(
                34.0f * dpiScale, 0.0f,
                (std::max)(34.0f * dpiScale, size.width - 6.0f * dpiScale),
                size.height);
            m_renderTarget->DrawTextW(
                txt, static_cast<UINT32>(wcslen(txt)),
                m_uiLeadTextFormat.Get(), textRect, textBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    } else {
        // 利用 Direct2D SetTransform 硬件矩阵直接缩放绘制源纹理（或二维瓦片），
        // 避免高倍放大 (5.0x) 时分配超限重采样位图，并按视口可见范围裁剪绘制超长截图瓦片
        const float sx = (m_origWidth > 0) ? (size.width / static_cast<float>(m_origWidth)) : 1.0f;
        const float sy = (m_origHeight > 0) ? (size.height / static_cast<float>(m_origHeight)) : 1.0f;
        D2D1_MATRIX_3X2_F prevTransform = D2D1::Matrix3x2F::Identity();
        m_renderTarget->GetTransform(&prevTransform);
        m_renderTarget->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * prevTransform);

        ComPtr<ID2D1DeviceContext> dc;
        m_renderTarget.As(&dc);
        const bool highQualityZoom = (std::abs(sx - 1.0f) > 0.01f || std::abs(sy - 1.0f) > 0.01f);
        const D2D1_INTERPOLATION_MODE dcInterp = highQualityZoom
            ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
            : D2D1_INTERPOLATION_MODE_LINEAR;
        const D2D1_BITMAP_INTERPOLATION_MODE rtInterp = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;

        if (m_bitmap) {
            const D2D1_RECT_F srcSpaceRect = D2D1::RectF(
                0.0f, 0.0f,
                static_cast<float>((std::max)(1, m_origWidth)),
                static_cast<float>((std::max)(1, m_origHeight)));
            if (dc) {
                dc->DrawBitmap(m_bitmap.Get(), &srcSpaceRect, 1.0f, dcInterp, nullptr, nullptr);
            } else {
                m_renderTarget->DrawBitmap(m_bitmap.Get(), srcSpaceRect, 1.0f, rtInterp);
            }
        } else {
            // 计算当前显示器屏幕可见区域映射回源图像空间的裁剪边界，仅提交可见瓦片
            float visLeft = 0.0f;
            float visTop = 0.0f;
            float visRight = static_cast<float>((std::max)(1, m_origWidth));
            float visBottom = static_cast<float>((std::max)(1, m_origHeight));
            if (m_hwnd && sx > 0.0001f && sy > 0.0001f) {
                RECT winRc{};
                if (GetWindowRect(m_hwnd, &winRc)) {
                    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                    if (vw > 0 && vh > 0) {
                        visLeft = (std::max)(0.0f, static_cast<float>(vx - winRc.left) / sx - 64.0f);
                        visTop = (std::max)(0.0f, static_cast<float>(vy - winRc.top) / sy - 64.0f);
                        visRight = (std::min)(visRight, static_cast<float>(vx + vw - winRc.left) / sx + 64.0f);
                        visBottom = (std::min)(visBottom, static_cast<float>(vy + vh - winRc.top) / sy + 64.0f);
                    }
                }
            }

            for (const auto& tile : m_bitmapTiles) {
                if (!tile.bitmap || tile.width <= 0 || tile.height <= 0) continue;
                const float tx0 = static_cast<float>(tile.x);
                const float ty0 = static_cast<float>(tile.y);
                const float tx1 = static_cast<float>(tile.x + tile.width);
                const float ty1 = static_cast<float>(tile.y + tile.height);
                if (tx1 <= visLeft || ty1 <= visTop || tx0 >= visRight || ty0 >= visBottom) {
                    continue;
                }
                const D2D1_RECT_F tileDst = D2D1::RectF(tx0, ty0, tx1, ty1);
                if (dc) {
                    dc->DrawBitmap(tile.bitmap.Get(), &tileDst, 1.0f, dcInterp, nullptr, nullptr);
                } else {
                    m_renderTarget->DrawBitmap(tile.bitmap.Get(), tileDst, 1.0f, rtInterp);
                }
            }
        }
        m_renderTarget->SetTransform(prevTransform);

        // 渲染划选与悬停的文字高亮块 (Live Text Selection Overlay)
        if (m_ocrReady) {
            std::lock_guard<std::mutex> lock(m_ocrMutex);
            const float dpiScale = m_hwnd ? tools3000::core::dpi::scaleForWindow(m_hwnd) : m_dpiScale;
            m_dpiScale = dpiScale;
            const tools3000::core::AccentColorRGB themeRgb = FloatingGlassBar::effectiveAccentRGB();

            ComPtr<ID2D1SolidColorBrush> hoverFillBrush, hoverStrokeBrush;
            ComPtr<ID2D1SolidColorBrush> selFillBrush, selStrokeBrush;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.16f), hoverFillBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.50f), hoverStrokeBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.32f), selFillBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.90f), selStrokeBrush.GetAddressOf());

            // 0. 文字划选模式下：为全部未被选中的文字块绘制半透明引导虚光胶囊
            if (m_textSelectionMode) {
                ComPtr<ID2D1SolidColorBrush> guideFillBrush, guideStrokeBrush;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.04f), guideFillBrush.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.22f), guideStrokeBrush.GetAddressOf());
                for (size_t i = 0; i < m_ocrBlocks.size(); ++i) {
                    bool alreadySelected = std::find(m_selectedBlockIndices.begin(), m_selectedBlockIndices.end(), i) != m_selectedBlockIndices.end();
                    if (!alreadySelected && static_cast<int>(i) != m_hoveredBlockIndex) {
                        const auto& b = m_ocrBlocks[i].origBox;
                        D2D1_RECT_F r = D2D1::RectF(b.x * m_scale, b.y * m_scale, (b.x + b.width) * m_scale, (b.y + b.height) * m_scale);
                        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 3.0f * dpiScale, 3.0f * dpiScale);
                        if (guideFillBrush) m_renderTarget->FillRoundedRectangle(&rr, guideFillBrush.Get());
                        if (guideStrokeBrush) m_renderTarget->DrawRoundedRectangle(&rr, guideStrokeBrush.Get(), 0.8f * dpiScale);
                    }
                }
            }

            // 1. 悬停块高亮
            if (m_hoveredBlockIndex >= 0 && static_cast<size_t>(m_hoveredBlockIndex) < m_ocrBlocks.size()) {
                bool alreadySelected = std::find(m_selectedBlockIndices.begin(), m_selectedBlockIndices.end(),
                                                 static_cast<size_t>(m_hoveredBlockIndex)) != m_selectedBlockIndices.end();
                if (!alreadySelected) {
                    const auto& b = m_ocrBlocks[m_hoveredBlockIndex].origBox;
                    D2D1_RECT_F r = D2D1::RectF(b.x * m_scale, b.y * m_scale, (b.x + b.width) * m_scale, (b.y + b.height) * m_scale);
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 3.0f * dpiScale, 3.0f * dpiScale);
                    if (hoverFillBrush) m_renderTarget->FillRoundedRectangle(&rr, hoverFillBrush.Get());
                    if (hoverStrokeBrush) m_renderTarget->DrawRoundedRectangle(&rr, hoverStrokeBrush.Get(), 1.0f * dpiScale);
                }
            }

            // 2. 选中块高亮
            float minSelY = size.height;
            float maxSelY = 0.0f;
            float centerSelX = size.width / 2.0f;
            for (size_t idx : m_selectedBlockIndices) {
                if (idx < m_ocrBlocks.size()) {
                    const auto& b = m_ocrBlocks[idx].origBox;
                    D2D1_RECT_F r = D2D1::RectF(b.x * m_scale, b.y * m_scale, (b.x + b.width) * m_scale, (b.y + b.height) * m_scale);
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 3.0f * dpiScale, 3.0f * dpiScale);
                    if (selFillBrush) m_renderTarget->FillRoundedRectangle(&rr, selFillBrush.Get());
                    if (selStrokeBrush) m_renderTarget->DrawRoundedRectangle(&rr, selStrokeBrush.Get(), 1.5f * dpiScale);
                    if (r.top < minSelY) {
                        minSelY = r.top;
                        centerSelX = (r.left + r.right) / 2.0f;
                    }
                    if (r.bottom > maxSelY) {
                        maxSelY = r.bottom;
                    }
                }
            }

            // 3. 正在拉框划选时的半透明选区
            if (m_isSelectingText) {
                float sx1 = static_cast<float>((std::min)(m_selectStart.x, m_selectEnd.x));
                float sy1 = static_cast<float>((std::min)(m_selectStart.y, m_selectEnd.y));
                float sx2 = static_cast<float>((std::max)(m_selectStart.x, m_selectEnd.x));
                float sy2 = static_cast<float>((std::max)(m_selectStart.y, m_selectEnd.y));
                D2D1_RECT_F selBox = D2D1::RectF(sx1, sy1, sx2, sy2);
                if (hoverFillBrush) m_renderTarget->FillRectangle(&selBox, hoverFillBrush.Get());
                if (selStrokeBrush) m_renderTarget->DrawRectangle(&selBox, selStrokeBrush.Get(), 1.0f * dpiScale);
            }

            // 4. 浮动“复制文字”微晶胶囊（动态测宽 + 极窄贴图 maxPillX 安全保护 + 顶部悬停工具栏重叠避让）
            if (!m_selectedBlockIndices.empty() && !m_isSelectingText) {
                const bool isZh = tools3000::core::isUiLanguageChinese();
                const wchar_t* btnLabel = isZh ? L"复制文字" : L"Copy";
                const float labelW = measureUiTextWidth(btnLabel, dpiScale);
                const float desiredPillW = std::ceil((std::max)(74.0f * dpiScale, 26.0f * dpiScale + labelW + 12.0f * dpiScale));
                const float pillW = (size.width >= 36.0f * dpiScale)
                    ? (std::min)(desiredPillW, (std::max)(36.0f * dpiScale, size.width - 12.0f * dpiScale))
                    : desiredPillW;
                const float pillH = 24.0f * dpiScale;
                const float maxPillX = (std::max)(6.0f * dpiScale, size.width - pillW - 6.0f * dpiScale);
                const float pillX = std::clamp(centerSelX - pillW * 0.5f, 6.0f * dpiScale, maxPillX);
                float pillY = minSelY - pillH - 6.0f * dpiScale;
                if (pillY < 6.0f * dpiScale) {
                    pillY = (std::max)(minSelY + 28.0f * dpiScale, maxSelY + 6.0f * dpiScale);
                }
                if (m_hoverAlpha > 0.01f &&
                    PinInputController::shouldShowHoverToolbar(
                        size.width, size.height, dpiScale, !m_pinnedSourceText.empty())) {
                    const auto tbLayout = PinInputController::computeHoverToolbarLayout(
                        size.width, size.height, dpiScale);
                    const D2D1_RECT_F tb = tbLayout.toolbarRect;
                    const bool overlapsTb =
                        (pillX < tb.right && pillX + pillW > tb.left &&
                         pillY < tb.bottom && pillY + pillH > tb.top);
                    if (overlapsTb) {
                        pillY = (std::max)(maxSelY + 6.0f * dpiScale, tb.bottom + 6.0f * dpiScale);
                    }
                }
                const float maxPillY = (std::max)(6.0f * dpiScale, size.height - pillH - 6.0f * dpiScale);
                pillY = std::clamp(pillY, 6.0f * dpiScale, maxPillY);
                m_btnCopyFloatingRect = D2D1::RectF(pillX, pillY, pillX + pillW, pillY + pillH);

                FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnCopyFloatingRect, 4.0f * dpiScale,
                                                m_hoverCopyFloating, false, false, false, 0.95f, true);

                ComPtr<ID2D1SolidColorBrush> pillTextBrush;
                m_renderTarget->CreateSolidColorBrush(pal.textPrimary, pillTextBrush.GetAddressOf());

                // 复制图标居左
                auto iconRect = D2D1::RectF(pillX + 4.0f * dpiScale, pillY + 3.0f * dpiScale,
                                            pillX + 22.0f * dpiScale, pillY + pillH - 3.0f * dpiScale);
                CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                                              CaptureIconId::ActionCopy, iconRect, pillTextBrush.Get(), dpiScale);

                // “复制文字”居右（复用缓存 DWrite 12px 格式，强制 NO_WRAP）
                if (ensureTextResources(dpiScale) && m_uiLeadTextFormat && pillTextBrush &&
                    pillW > 34.0f * dpiScale) {
                    const D2D1_RECT_F lblRect = D2D1::RectF(
                        pillX + 24.0f * dpiScale, pillY,
                        (std::max)(pillX + 24.0f * dpiScale, pillX + pillW - 4.0f * dpiScale),
                        pillY + pillH);
                    m_renderTarget->DrawTextW(
                        btnLabel, static_cast<UINT32>(wcslen(btnLabel)),
                        m_uiLeadTextFormat.Get(), lblRect, pillTextBrush.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            } else {
                m_btnCopyFloatingRect = D2D1::RectF(0, 0, 0, 0);
            }
        }

        const float curDpiScale = m_hwnd ? tools3000::core::dpi::scaleForWindow(m_hwnd) : m_dpiScale;
        const bool showHoverToolbar = PinInputController::shouldShowHoverToolbar(
            size.width, size.height, curDpiScale, !m_pinnedSourceText.empty());
        if (m_hoverAlpha > 0.01f && !m_isSelectingText && showHoverToolbar) {
            drawHoverToolbar();
        } else {
            clearHoverToolbarRects();
        }

        // 左上角微晶分组胶囊：已分组贴图常驻显示宝石色组标；未分组贴图仅在大尺寸贴图悬停时浮现分组快捷入口（小贴图保持 100% 零遮挡）
        m_btnGroupBadgeRect = {};
        if (!m_isSelectingText) {
            const float dpiScale = curDpiScale;
            const bool hasGroup = (m_groupId >= PinGroupConstants::kMinGroupId);
            const bool showUngroupedBtn = (!hasGroup && m_hoverAlpha > 0.05f && showHoverToolbar);
            if ((hasGroup || showUngroupedBtn) && size.width >= 68.0f * dpiScale && size.height >= 36.0f * dpiScale) {
                const bool isZh = tools3000::core::isUiLanguageChinese();
                const float pad = 8.0f * dpiScale;
                const float pillH = 24.0f * dpiScale;
                std::wstring badgeLabel;
                float pillW = 28.0f * dpiScale;
                if (hasGroup) {
                    badgeLabel = (m_hoverAlpha > 0.25f && size.width >= 180.0f * dpiScale)
                        ? PinGroupPalette::shortLabel(m_groupId, isZh)
                        : std::format(L"{}", m_groupId);
                    const std::wstring customLabel = PinGroupPalette::customName(m_groupId);
                    if (!customLabel.empty() && size.width >= 108.0f * dpiScale) {
                        if (m_hoverAlpha > 0.25f && size.width >= 156.0f * dpiScale) {
                            badgeLabel = PinGroupPalette::shortLabel(m_groupId, isZh);
                        } else if (customLabel.size() > 8) {
                            badgeLabel = customLabel.substr(0, 8) + L"…";
                        } else {
                            badgeLabel = customLabel;
                        }
                    }
                    const float tw = measureUiTextWidth(badgeLabel, dpiScale);
                    pillW = std::ceil(18.0f * dpiScale + tw + 8.0f * dpiScale);
                }

                const D2D1_RECT_F candidate = D2D1::RectF(pad, pad, pad + pillW, pad + pillH);
                const bool overlapsToolbar =
                    (m_hoverAlpha > 0.01f && m_toolbarRect.right > m_toolbarRect.left &&
                     candidate.right + 4.0f * dpiScale > m_toolbarRect.left &&
                     candidate.top < m_toolbarRect.bottom && candidate.bottom > m_toolbarRect.top);
                if (!overlapsToolbar) {
                    m_btnGroupBadgeRect = candidate;
                    const float badgeAlpha = hasGroup ? 0.96f : (0.92f * m_hoverAlpha);
                    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), candidate, 6.0f * dpiScale, true);
                    if (m_hoverGroupBadge) {
                        FloatingGlassBar::drawButtonPill(
                            m_renderTarget.Get(), candidate, 5.0f * dpiScale, true, false, false, false, badgeAlpha, true);
                    }
                    if (hasGroup) {
                        ComPtr<ID2D1SolidColorBrush> dotBr, txtBr;
                        m_renderTarget->CreateSolidColorBrush(
                            PinGroupPalette::accentColor(m_groupId, badgeAlpha), dotBr.GetAddressOf());
                        D2D1_COLOR_F badgeTxt = pal.textPrimary;
                        badgeTxt.a *= badgeAlpha;
                        m_renderTarget->CreateSolidColorBrush(badgeTxt, txtBr.GetAddressOf());
                        const float dotR = 3.8f * dpiScale;
                        const float dotCx = candidate.left + 8.0f * dpiScale + dotR;
                        const float dotCy = (candidate.top + candidate.bottom) * 0.5f;
                        if (dotBr) {
                            m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotCx, dotCy), dotR, dotR), dotBr.Get());
                        }
                        if (txtBr && ensureTextResources(dpiScale) && m_uiCenterTextFormat) {
                            const D2D1_RECT_F tRect = D2D1::RectF(
                                dotCx + dotR + 2.5f * dpiScale, candidate.top,
                                candidate.right - 5.0f * dpiScale, candidate.bottom);
                            m_renderTarget->DrawTextW(
                                badgeLabel.c_str(), static_cast<UINT32>(badgeLabel.size()),
                                m_uiCenterTextFormat.Get(), tRect, txtBr.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                        }
                    } else {
                        ComPtr<ID2D1SolidColorBrush> iconBr;
                        D2D1_COLOR_F badgeIcon = pal.textPrimary;
                        badgeIcon.a *= badgeAlpha;
                        m_renderTarget->CreateSolidColorBrush(badgeIcon, iconBr.GetAddressOf());
                        if (iconBr) {
                            CaptureVectorIcons::renderIcon(
                                m_renderTarget.Get(), m_d2dFactory.Get(),
                                CaptureIconId::ActionPinGroup, candidate, iconBr.Get(), dpiScale);
                        }
                    }
                }
            }
        }

        // 边框：全部按半线宽内缩，防止 HwndRenderTarget 边界裁掉外半边线宽；微晶字签/色卡签同步采用同心真圆角描边
        const float scaledCardR = (m_cardCornerRadius > 0.5f)
            ? std::clamp(m_cardCornerRadius * m_scale, 3.0f, (std::max)(3.0f, (std::min)(size.width, size.height) * 0.5f))
            : 0.0f;
        ComPtr<ID2D1SolidColorBrush> borderBrush;
        if (m_focused) {
            const tools3000::core::AccentColorRGB themeRgb = FloatingGlassBar::effectiveAccentRGB();
            const D2D1_COLOR_F focusCol = (m_groupId >= PinGroupConstants::kMinGroupId)
                ? PinGroupPalette::accentColor(m_groupId, 0.96f)
                : D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.95f);
            m_renderTarget->CreateSolidColorBrush(focusCol, borderBrush.GetAddressOf());
            const D2D1_RECT_F rc = D2D1::RectF(1.0f, 1.0f, (std::max)(1.0f, size.width - 1.0f), (std::max)(1.0f, size.height - 1.0f));
            if (scaledCardR > 0.5f) {
                const float r = (std::max)(2.0f, scaledCardR - 1.0f);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, r, r), borderBrush.Get(), 2.0f);
            } else {
                m_renderTarget->DrawRectangle(rc, borderBrush.Get(), 2.0f);
            }
        } else if (m_clickThrough) {
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.85f, 1.0f, 0.65f), borderBrush.GetAddressOf());
            const D2D1_RECT_F rc = D2D1::RectF(0.75f, 0.75f, (std::max)(0.75f, size.width - 0.75f), (std::max)(0.75f, size.height - 0.75f));
            if (scaledCardR > 0.5f) {
                const float r = (std::max)(2.0f, scaledCardR - 0.75f);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, r, r), borderBrush.Get(), 1.5f);
            } else {
                m_renderTarget->DrawRectangle(rc, borderBrush.Get(), 1.5f);
            }
        } else if (scaledCardR > 0.5f) {
            // 纯白微晶字签/色卡签在未获焦态补充 1.0f 浅灰外边框保护，确保贴在白色网页上也边界分明
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.75f, 0.80f, 0.86f, 0.78f), borderBrush.GetAddressOf());
            const D2D1_RECT_F rc = D2D1::RectF(0.5f, 0.5f, (std::max)(0.5f, size.width - 0.5f), (std::max)(0.5f, size.height - 0.5f));
            const float r = (std::max)(2.0f, scaledCardR - 0.5f);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, r, r), borderBrush.Get(), 1.0f);
        } else {
            // 普通直角截图绘制 1px 中性外框
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.5f), borderBrush.GetAddressOf());
            m_renderTarget->DrawRectangle(
                D2D1::RectF(0.5f, 0.5f, (std::max)(0.5f, size.width - 0.5f), (std::max)(0.5f, size.height - 0.5f)),
                borderBrush.Get(), 1.0f);
        }
    }

    const HRESULT hrEnd = m_renderTarget->EndDraw();
    if (hrEnd == D2DERR_RECREATE_TARGET ||
        hrEnd == DXGI_ERROR_DEVICE_REMOVED ||
        hrEnd == DXGI_ERROR_DEVICE_RESET) {
        m_renderTarget.Reset();
        m_bitmap.Reset();
        m_bitmapTiles.clear();
        if (m_hwnd && IsWindow(m_hwnd)) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口过程
// ─────────────────────────────────────────────────────────────────────────────

LRESULT CALLBACK PinWindow::pinWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_MOUSEACTIVATE: {
            if (self) {
                self->focusWindow();
            }
            return MA_ACTIVATE;
        }

        case WM_SYSCOMMAND: {
            if ((wParam & 0xFFF0) == SC_KEYMENU || (wParam & 0xFFF0) == SC_CONTEXTHELP) {
                return 0;
            }
            break;
        }

        case WM_SETCURSOR: {
            if (self && !self->m_folded) {
                if (self->m_isDragging) {
                    SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                    return TRUE;
                }
                // 1. 悬停在分组胶囊、浮动复制微晶胶囊或悬浮工具栏按钮上：手型光标 IDC_HAND
                if (self->m_hoverGroupBadge || self->m_hoverCopyFloating || self->m_hoverFold || self->m_hoverRotate ||
                    self->m_hoverInvert || self->m_hoverEdit || self->m_hoverText ||
                    self->m_hoverCopy || self->m_hoverSave || self->m_hoverClose) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                    return TRUE;
                }

                // 2. Alt 键按下、正在划选或悬停在 OCR 文字块上：文本光标 IDC_IBEAM
                const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                if (alt || self->m_isSelectingText || self->m_hoveredBlockIndex >= 0 ||
                    (self->m_textSelectionMode && !self->m_ocrReady.load())) {
                    SetCursor(LoadCursor(nullptr, IDC_IBEAM));
                    return TRUE;
                }

                // 3. 否则默认平移拖拽光标 IDC_SIZEALL（含文字划选模式下指向空白区域时）
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            if (!self) break;
            self->focusWindow();
            const int clickX = GET_X_LPARAM(lParam);
            const int clickY = GET_Y_LPARAM(lParam);

            self->m_pressedToolbarBtn = kPinBtnNone;
            self->m_pressStartClientPt = {clickX, clickY};
            self->m_dragOffset = {clickX, clickY};
            self->m_dragThresholdExceeded = false;
            self->m_isDragging = false;

            // Drag-First 状态机：按下工具栏按钮/分组胶囊/复制胶囊时不立即触发动作或阻断拖拽，
            // 先记录候选按钮并捕获鼠标；若后续移动 >= 4px*dpiScale 则直接拖动贴图，原地抬起才执行按钮动作！
            const int hitBtn = self->hitTestInteractiveButton(clickX, clickY);
            if (hitBtn != kPinBtnNone) {
                self->m_pressedToolbarBtn = hitBtn;
                SetCapture(hwnd);
                return 0;
            }

            // 文字划选不阻断空白区拖拽：
            // 仅当用户按住 Alt 键，或开启了 m_textSelectionMode 且按下位置命中 OCR 文字块包围盒时，才进入拉框选字；
            // 否则一律走整窗拖拽移动，绝不吞没贴图拖拽！
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool canSelectText = alt || self->m_textSelectionMode;
            if (!self->m_folded && canSelectText) {
                if (!self->m_ocrReady.load()) self->ensureOcrExecutedAsync();

                int hitOcrBlock = -1;
                {
                    std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                    const float mx = static_cast<float>(clickX) / (std::max)(0.05f, self->m_scale);
                    const float my = static_cast<float>(clickY) / (std::max)(0.05f, self->m_scale);
                    const cv::Point pt(static_cast<int>(mx), static_cast<int>(my));
                    for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                        if (self->m_ocrBlocks[i].origBox.contains(pt)) {
                            hitOcrBlock = static_cast<int>(i);
                            break;
                        }
                    }
                }

                if (alt || hitOcrBlock >= 0) {
                    self->m_isSelectingText = true;
                    self->m_selectStart = {clickX, clickY};
                    self->m_selectEnd = {clickX, clickY};
                    self->m_selectedBlockIndices.clear();
                    if (hitOcrBlock >= 0) {
                        self->m_selectedBlockIndices.push_back(static_cast<size_t>(hitOcrBlock));
                    }
                    SetCapture(hwnd);
                    self->render();
                    return 0;
                }
            }

            // 普通点击：如果有选中文字但点击在外部空白区，先清除文字选中并允许直接拖拽窗口
            if (!self->m_selectedBlockIndices.empty()) {
                self->m_selectedBlockIndices.clear();
                self->render();
            }

            self->m_isDragging = true;
            SetCapture(hwnd);
            return 0;
        }

        case WM_SETFOCUS: {
            if (self) { self->m_focused = true; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }

        case WM_KILLFOCUS: {
            if (self) { self->m_focused = false; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            if (!self) break;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (msg == WM_SYSKEYDOWN) || ((GetKeyState(VK_MENU) & 0x8000) != 0);
            bool hasSelectedBlocks = false;
            {
                std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                hasSelectedBlocks = !self->m_selectedBlockIndices.empty();
            }
            const bool hasTextContext = self->m_textSelectionMode || self->m_isSelectingText || hasSelectedBlocks;

            const auto decision = PinInputController::resolveKeyDown(wParam, ctrl, shift, alt, hasTextContext);
            switch (decision.action) {
                case PinShortcutAction::RotateClockwise90:
                case PinShortcutAction::RotateCounterClockwise90:
                    self->rotate(decision.rotateDegrees);
                    return 0;
                case PinShortcutAction::FlipHorizontal:
                    self->flip(true);
                    return 0;
                case PinShortcutAction::FlipVertical:
                    self->flip(false);
                    return 0;
                case PinShortcutAction::SetOpacity:
                    self->setOpacity(decision.opacityValue);
                    return 0;
                case PinShortcutAction::AdjustOpacity:
                    self->setOpacity(std::clamp(self->m_opacity + decision.opacityDelta,
                                                PinInteractionConstants::kMinPinOpacity,
                                                PinInteractionConstants::kMaxPinOpacity));
                    return 0;
                case PinShortcutAction::ResetScaleAndOpacity:
                    self->resetScale();
                    self->setOpacity(PinInteractionConstants::kMaxPinOpacity);
                    return 0;
                case PinShortcutAction::ZoomIn:
                case PinShortcutAction::ZoomOut:
                    self->setScale(self->m_scale * decision.scaleFactor);
                    return 0;
                case PinShortcutAction::Nudge: {
                    RECT rc{};
                    if (GetWindowRect(hwnd, &rc)) {
                        SetWindowPos(hwnd, nullptr,
                                     rc.left + decision.nudgeDx,
                                     rc.top + decision.nudgeDy,
                                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                    return 0;
                }
                case PinShortcutAction::EditMarkup:
                    self->editMarkup();
                    return 0;
                case PinShortcutAction::InvertColors:
                    self->invertColors();
                    return 0;
                case PinShortcutAction::ToggleGrayscale:
                    self->toggleGrayscale();
                    return 0;
                case PinShortcutAction::ToggleFold:
                    self->toggleFold();
                    return 0;
                case PinShortcutAction::ToggleClickThrough:
                    self->setClickThrough(!self->m_clickThrough);
                    return 0;
                case PinShortcutAction::PasteHistoryOrClipboard:
                    PinWindow::pasteNextHistoryOrClipboard();
                    return 0;
                case PinShortcutAction::SelectAllText:
                    self->selectAllText();
                    return 0;
                case PinShortcutAction::CopyAllText:
                    self->copyAllText();
                    return 0;
                case PinShortcutAction::CopySelectedOrImage:
                    if (!self->m_selectedBlockIndices.empty()) {
                        self->copySelectedText();
                        return 0;
                    }
                    if (!self->m_pinnedSourceText.empty()) {
                        self->copyAllText();
                        return 0;
                    }
                    copyImageToClipboard(self->m_sourceImage);
                    tools3000::core::EventBus::instance().publish(
                        tools3000::core::ShowToastEvent{
                            tools3000::core::isUiLanguageChinese() ? L"已复制贴图到剪贴板" : L"Copied pin to clipboard"});
                    return 0;
                case PinShortcutAction::ExtractText:
                    self->extractText();
                    return 0;
                case PinShortcutAction::SaveImage:
                    savePinnedImage(hwnd, self->m_sourceImage);
                    return 0;
                case PinShortcutAction::HistoryPrev: {
                    const int totalHist = CaptureHistory::instance().count();
                    if (totalHist > 1) {
                        self->m_historyBrowseIndex = std::min(self->m_historyBrowseIndex + 1, totalHist - 1);
                        auto entry = CaptureHistory::instance().get(self->m_historyBrowseIndex);
                        if (entry.has_value() && !entry->image.empty()) {
                            self->updateImage(entry->image);
                            bool isZh = tools3000::core::isUiLanguageChinese();
                            std::wstring toastText = isZh
                                ? std::format(L"历史贴图 {}/{}", self->m_historyBrowseIndex + 1, totalHist)
                                : std::format(L"Pin history {}/{}", self->m_historyBrowseIndex + 1, totalHist);
                            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{toastText});
                        }
                    }
                    return 0;
                }
                case PinShortcutAction::HistoryNext: {
                    const int totalHist = CaptureHistory::instance().count();
                    if (totalHist > 1 && self->m_historyBrowseIndex > 0) {
                        self->m_historyBrowseIndex = std::max(0, self->m_historyBrowseIndex - 1);
                        auto entry = CaptureHistory::instance().get(self->m_historyBrowseIndex);
                        if (entry.has_value() && !entry->image.empty()) {
                            self->updateImage(entry->image);
                            bool isZh = tools3000::core::isUiLanguageChinese();
                            std::wstring toastText = isZh
                                ? std::format(L"历史贴图 {}/{}", self->m_historyBrowseIndex + 1, totalHist)
                                : std::format(L"Pin history {}/{}", self->m_historyBrowseIndex + 1, totalHist);
                            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{toastText});
                        }
                    }
                    return 0;
                }
                case PinShortcutAction::AssignOrToggleGroup:
                    self->toggleAssignGroup(decision.groupIndex);
                    return 0;
                case PinShortcutAction::RemoveFromGroup:
                    self->setGroupId(PinGroupConstants::kUngroupedId);
                    return 0;
                case PinShortcutAction::AutoGroupUngrouped:
                    if (PinWindow::countInGroup(PinGroupConstants::kUngroupedId) == 0 &&
                        self->m_groupId == PinGroupConstants::kUngroupedId) {
                        self->setGroupId(1);
                    } else {
                        PinWindow::groupAllUngrouped();
                    }
                    return 0;
                case PinShortcutAction::FilterByGroup:
                    if (PinWindow::activeGroupFilter() == decision.groupIndex) {
                        PinWindow::setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
                    } else {
                        PinWindow::setActiveGroupFilter(decision.groupIndex);
                    }
                    return 0;
                case PinShortcutAction::ShowAllGroups:
                    PinWindow::setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
                    return 0;
                case PinShortcutAction::EscapeOrClose:
                    if (!self->m_selectedBlockIndices.empty()) {
                        self->m_selectedBlockIndices.clear();
                        self->render();
                        return 0;
                    }
                    if (self->m_textSelectionMode) {
                        self->m_textSelectionMode = false;
                        self->render();
                        return 0;
                    }
                    self->close();
                    return 0;
                case PinShortcutAction::ClosePin:
                    self->close();
                    return 0;
                case PinShortcutAction::None:
                default:
                    if (alt) return 0;
                    break;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (!self) break;
            if (!self->m_isHovering) {
                self->m_isHovering = true;
                self->m_hoverTime = GetTickCount64();
                SetTimer(hwnd, 1, 16, nullptr);
                
                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, HOVER_DEFAULT };
                TrackMouseEvent(&tme);
            }

            // 悬停即获焦（当贴图尚未获焦、不处于鼠标穿透且截图遮罩未打开时），
            // 保障无论是首次移入还是遮罩关闭后鼠标仍在贴图内移动，按键旋转/缩放/关闭均即时生效
            const bool isActuallyFocused = self->m_focused && (GetFocus() == hwnd);
            if (PinInputController::shouldAutoFocusOnHover(
                    isActuallyFocused, self->m_clickThrough, CaptureOverlay::instance().isOverlayVisible())) {
                self->focusWindow();
            }

            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const bool leftHeld = (wParam & MK_LBUTTON) != 0;

            if (!leftHeld && (self->m_pressedToolbarBtn != kPinBtnNone || self->m_isDragging)) {
                self->m_pressedToolbarBtn = kPinBtnNone;
                self->m_isDragging = false;
                self->m_dragThresholdExceeded = false;
            }

            // Drag-First 阈值跃迁：若在工具栏按钮/胶囊上按下左键后移动距离 >= 4.0 * dpiScale，
            // 立即取消按钮触发候选，平滑切换为整窗拖拽移动！
            if (leftHeld && self->m_pressedToolbarBtn != kPinBtnNone && !self->m_isDragging) {
                const float moveDist = std::hypot(
                    static_cast<float>(x - self->m_pressStartClientPt.x),
                    static_cast<float>(y - self->m_pressStartClientPt.y));
                const float safeScale = (self->m_dpiScale > 0.0f) ? self->m_dpiScale : 1.0f;
                if (moveDist >= PinInteractionConstants::kDragStartThresholdDip * safeScale) {
                    self->m_pressedToolbarBtn = kPinBtnNone;
                    self->m_dragThresholdExceeded = true;
                    self->m_isDragging = true;
                }
            }

            if (self->m_isDragging) {
                if (self->m_hoverGroupBadge || self->m_hoverFold || self->m_hoverRotate || self->m_hoverInvert ||
                    self->m_hoverEdit || self->m_hoverText || self->m_hoverCopy || self->m_hoverSave ||
                    self->m_hoverClose || self->m_hoverCopyFloating) {
                    self->m_hoverGroupBadge = false;
                    self->m_hoverFold = false;
                    self->m_hoverRotate = false;
                    self->m_hoverInvert = false;
                    self->m_hoverEdit = false;
                    self->m_hoverText = false;
                    self->m_hoverCopy = false;
                    self->m_hoverSave = false;
                    self->m_hoverClose = false;
                    self->m_hoverCopyFloating = false;
                    self->render();
                }
                RECT winRc{};
                GetWindowRect(hwnd, &winRc);
                int curW = (std::max)(1, static_cast<int>(winRc.right - winRc.left));
                int curH = (std::max)(1, static_cast<int>(winRc.bottom - winRc.top));
                POINT targetPos = {
                    winRc.left + x - self->m_dragOffset.x,
                    winRc.top + y - self->m_dragOffset.y
                };
                targetPos = calculateMagneticSnap(hwnd, targetPos, curW, curH);
                SetWindowPos(hwnd, nullptr,
                             targetPos.x,
                             targetPos.y,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER);
                return 0;
            }
            
            if (self->m_folded) {
                if (self->m_hoverGroupBadge || self->m_hoverFold || self->m_hoverRotate || self->m_hoverInvert || self->m_hoverEdit ||
                    self->m_hoverText || self->m_hoverCopy || self->m_hoverSave || self->m_hoverClose) {
                    self->m_hoverGroupBadge = false;
                    self->m_hoverFold = false;
                    self->m_hoverRotate = false;
                    self->m_hoverInvert = false;
                    self->m_hoverEdit = false;
                    self->m_hoverText = false;
                    self->m_hoverCopy = false;
                    self->m_hoverSave = false;
                    self->m_hoverClose = false;
                    self->render();
                }
            } else {
                const bool hoverFold = isPointInRectF(self->m_btnFoldRect, x, y);
                const bool hoverRotate = isPointInRectF(self->m_btnRotateRect, x, y);
                const bool hoverInvert = isPointInRectF(self->m_btnInvertRect, x, y);
                const bool hoverEdit = isPointInRectF(self->m_btnEditRect, x, y);
                const bool hoverText = isPointInRectF(self->m_btnTextRect, x, y);
                const bool hoverCopy = isPointInRectF(self->m_btnCopyRect, x, y);
                const bool hoverSave = isPointInRectF(self->m_btnSaveRect, x, y);
                const bool hoverClose = isPointInRectF(self->m_btnCloseRect, x, y);
                const bool hoverGroupBadge = isPointInRectF(self->m_btnGroupBadgeRect, x, y);
                const bool hoverCopyFloating = (!self->m_selectedBlockIndices.empty() &&
                                                isPointInRectF(self->m_btnCopyFloatingRect, x, y));
                
                if (hoverGroupBadge != self->m_hoverGroupBadge ||
                    hoverFold != self->m_hoverFold || hoverRotate != self->m_hoverRotate ||
                    hoverInvert != self->m_hoverInvert || hoverEdit != self->m_hoverEdit ||
                    hoverText != self->m_hoverText || hoverCopy != self->m_hoverCopy ||
                    hoverSave != self->m_hoverSave || hoverClose != self->m_hoverClose ||
                    hoverCopyFloating != self->m_hoverCopyFloating) {
                    self->m_hoverGroupBadge = hoverGroupBadge;
                    self->m_hoverFold = hoverFold;
                    self->m_hoverRotate = hoverRotate;
                    self->m_hoverInvert = hoverInvert;
                    self->m_hoverEdit = hoverEdit;
                    self->m_hoverText = hoverText;
                    self->m_hoverCopy = hoverCopy;
                    self->m_hoverSave = hoverSave;
                    self->m_hoverClose = hoverClose;
                    self->m_hoverCopyFloating = hoverCopyFloating;
                    self->render();
                }

                // 正在拉框划选文字
                if (self->m_isSelectingText) {
                    self->m_selectEnd = {x, y};
                    int sx1 = (std::min)(self->m_selectStart.x, self->m_selectEnd.x);
                    int sy1 = (std::min)(self->m_selectStart.y, self->m_selectEnd.y);
                    int sx2 = (std::max)(self->m_selectStart.x, self->m_selectEnd.x);
                    int sy2 = (std::max)(self->m_selectStart.y, self->m_selectEnd.y);

                    std::vector<size_t> newIndices;
                    {
                        std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                        for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                            const auto& b = self->m_ocrBlocks[i].origBox;
                            int bx1 = static_cast<int>(b.x * self->m_scale);
                            int by1 = static_cast<int>(b.y * self->m_scale);
                            int bx2 = static_cast<int>((b.x + b.width) * self->m_scale);
                            int by2 = static_cast<int>((b.y + b.height) * self->m_scale);

                            bool overlap = !(bx2 < sx1 || bx1 > sx2 || by2 < sy1 || by1 > sy2);
                            if (overlap) {
                                newIndices.push_back(i);
                            }
                        }
                    }
                    if (newIndices != self->m_selectedBlockIndices) {
                        self->m_selectedBlockIndices = newIndices;
                    }
                    self->render();
                    return 0;
                }

                // Alt 或处于文字划选模式下：悬停文字块检测
                const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                const bool canInspect = alt || self->m_textSelectionMode;
                if (canInspect && !self->m_ocrReady.load()) {
                    self->ensureOcrExecutedAsync();
                }
                if (canInspect && self->m_ocrReady) {
                    int hovered = -1;
                    {
                        std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                        float mx = static_cast<float>(x) / self->m_scale;
                        float my = static_cast<float>(y) / self->m_scale;
                        for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                            if (self->m_ocrBlocks[i].origBox.contains(cv::Point(static_cast<int>(mx), static_cast<int>(my)))) {
                                hovered = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                    if (hovered != self->m_hoveredBlockIndex) {
                        self->m_hoveredBlockIndex = hovered;
                        self->render();
                    }
                } else if (self->m_hoveredBlockIndex >= 0) {
                    self->m_hoveredBlockIndex = -1;
                    self->render();
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (!self) break;
            self->m_isHovering = false;
            self->m_hoverTime = GetTickCount64();
            self->m_hoverGroupBadge = false;
            self->m_hoverFold = false;
            self->m_hoverRotate = false;
            self->m_hoverInvert = false;
            self->m_hoverEdit = false;
            self->m_hoverText = false;
            self->m_hoverCopy = false;
            self->m_hoverSave = false;
            self->m_hoverClose = false;
            self->m_hoverCopyFloating = false;
            self->m_hoveredBlockIndex = -1;
            SetTimer(hwnd, 1, 16, nullptr);
            return 0;
        }

        case WM_TIMER: {
            if (self && wParam == 1) {
                self->updateHoverAnimation();
            }
            return 0;
        }

        case WM_CANCELMODE:
        case WM_CAPTURECHANGED: {
            if (self) {
                self->m_pressedToolbarBtn = kPinBtnNone;
                self->m_isDragging = false;
                self->m_dragThresholdExceeded = false;
                self->m_isSelectingText = false;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (self) {
                const int upX = GET_X_LPARAM(lParam);
                const int upY = GET_Y_LPARAM(lParam);
                if (self->m_isSelectingText) {
                    self->m_isSelectingText = false;
                    ReleaseCapture();
                    self->render();
                    return 0;
                }
                const int pressedBtn = self->m_pressedToolbarBtn;
                const bool wasDragging = self->m_isDragging || self->m_dragThresholdExceeded;
                self->m_pressedToolbarBtn = kPinBtnNone;
                self->m_isDragging = false;
                self->m_dragThresholdExceeded = false;
                ReleaseCapture();

                if (!wasDragging && pressedBtn != kPinBtnNone &&
                    self->hitTestInteractiveButton(upX, upY) == pressedBtn) {
                    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    self->executeInteractiveButton(pressedBtn, upX, upY, shift);
                }
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            // 双击：若折叠则展开微晶胶囊；若点在工具栏按钮上则按连击按钮处理；否则进入标注编辑模式
            if (self) {
                if (self->m_folded) {
                    self->toggleFold();
                } else {
                    const int dblX = GET_X_LPARAM(lParam);
                    const int dblY = GET_Y_LPARAM(lParam);
                    const int hitBtn = self->hitTestInteractiveButton(dblX, dblY);
                    if (hitBtn != kPinBtnNone) {
                        self->m_pressedToolbarBtn = hitBtn;
                        self->m_pressStartClientPt = {dblX, dblY};
                        self->m_dragOffset = {dblX, dblY};
                        self->m_dragThresholdExceeded = false;
                        self->m_isDragging = false;
                        SetCapture(hwnd);
                        return 0;
                    }
                    self->editMarkup();
                }
            }
            return 0;
        }

        case WM_MBUTTONUP: {
            // 鼠标中键单击：快速恢复 1:1 原始物理像素尺寸
            if (self) {
                self->focusWindow();
                self->resetScale();
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!self) break;
            self->focusWindow();
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const bool ctrl = (LOWORD(wParam) & MK_CONTROL) != 0 ||
                              ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
            const bool shift = (LOWORD(wParam) & MK_SHIFT) != 0 ||
                               ((GetKeyState(VK_SHIFT) & 0x8000) != 0);

            if (ctrl || shift) {
                // Ctrl/Shift + 滚轮：平滑微调透明度 (10% ~ 100%, 步进 5%)
                float newOpacity = self->m_opacity + (delta > 0 ? 0.05f : -0.05f);
                self->setOpacity(newOpacity);
            } else {
                // 滚轮：以当前鼠标光标为锚点平滑缩放 (0.1x ~ 5.0x)
                POINT cursor;
                GetCursorPos(&cursor);
                RECT rc;
                GetWindowRect(hwnd, &rc);

                float oldScale = self->m_scale;
                float zoomFactor = (delta > 0 ? 1.1f : 0.9f);
                float newScale = std::clamp(oldScale * zoomFactor, 0.1f, 5.0f);

                if (std::abs(newScale - oldScale) > 0.001f) {
                    if (self->m_folded) {
                        return 0;
                    }
                    float relX = (cursor.x - rc.left) / static_cast<float>((std::max)(1L, rc.right - rc.left));
                    float relY = (cursor.y - rc.top) / static_cast<float>((std::max)(1L, rc.bottom - rc.top));

                    constexpr int64_t kMaxSafeWindowDim = 32000;
                    const int64_t scaledW64 = std::clamp(
                        static_cast<int64_t>(std::llround(static_cast<double>(self->m_origWidth) * newScale)),
                        int64_t{1}, kMaxSafeWindowDim);
                    const int64_t scaledH64 = std::clamp(
                        static_cast<int64_t>(std::llround(static_cast<double>(self->m_origHeight) * newScale)),
                        int64_t{1}, kMaxSafeWindowDim);
                    int newW = static_cast<int>(scaledW64);
                    int newH = static_cast<int>(scaledH64);

                    int newLeft = cursor.x - static_cast<int>(std::llround(static_cast<double>(newW) * relX));
                    int newTop = cursor.y - static_cast<int>(std::llround(static_cast<double>(newH) * relY));

                    self->m_scale = newScale;
                    SetWindowPos(hwnd, nullptr, newLeft, newTop, newW, newH, SWP_NOZORDER | SWP_NOACTIVATE);
                    self->updateWindowCornerRegion();
                    if (self->m_renderTarget) {
                        RECT clientRc{};
                        GetClientRect(hwnd, &clientRc);
                        const UINT32 maxRt = (std::max)(256u, self->m_renderTarget->GetMaximumBitmapSize());
                        const UINT32 rtW = (std::min)(
                            static_cast<UINT32>((std::max)(1L, clientRc.right - clientRc.left)), maxRt);
                        const UINT32 rtH = (std::min)(
                            static_cast<UINT32>((std::max)(1L, clientRc.bottom - clientRc.top)), maxRt);
                        self->m_renderTarget->Resize(D2D1::SizeU(rtW, rtH));
                    }
                    self->render();
                }
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            // 右键菜单
            if (!self) break;
            self->focusWindow();
            HMENU menu = CreatePopupMenu();
            bool isZh = tools3000::core::isUiLanguageChinese();
            if (!self->m_pinnedSourceText.empty()) {
                AppendMenuW(menu, MF_STRING, 25, isZh ? L"复制贴图文字  (Ctrl+C)" : L"Copy Note Text  (Ctrl+C)");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            }
            AppendMenuW(menu, MF_STRING, 16, isZh ? L"编辑 / 二次标注  (E / 双击)" : L"Edit / Annotate  (E / Double Click)");
            AppendMenuW(menu, (self->m_folded ? MF_CHECKED : MF_UNCHECKED) | MF_STRING, 18, isZh ? L"折叠为迷你条  (M)" : L"Collapse  (M)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, (self->m_inverted ? MF_CHECKED : MF_UNCHECKED) | MF_STRING, 19, isZh ? L"色彩反转  (I)" : L"Invert Colors  (I)");
            AppendMenuW(menu, (self->m_grayscale ? MF_CHECKED : MF_UNCHECKED) | MF_STRING, 20, isZh ? L"灰度化  (G)" : L"Grayscale  (G)");
            AppendMenuW(menu, (self->m_textSelectionMode ? MF_CHECKED : MF_UNCHECKED) | MF_STRING, 23, isZh ? L"文字划选模式  (Alt 划选)" : L"Text Selection Mode  (Alt to select)");
            AppendMenuW(menu, MF_STRING, 24, isZh ? L"全选贴图文字  (Ctrl+A)" : L"Select All Text  (Ctrl+A)");
            if (!self->m_selectedBlockIndices.empty()) {
                AppendMenuW(menu, MF_STRING, 21, isZh ? L"复制所选文字  (Ctrl+C)" : L"Copy Selected Text  (Ctrl+C)");
            }
            AppendMenuW(menu, MF_STRING, 22, isZh ? L"复制全部文字  (Ctrl+Shift+C)" : L"Copy All Text  (Ctrl+Shift+C)");
            AppendMenuW(menu, MF_STRING, 1, isZh ? L"复制图片到剪贴板" : L"Copy Image to Clipboard");
            AppendMenuW(menu, MF_STRING, 17, isZh ? L"提取文本 (OCR)  (Ctrl+T)" : L"Extract Text (OCR)  (Ctrl+T)");
            AppendMenuW(menu, MF_STRING, 10, isZh ? L"保存图片...  (Ctrl+S)" : L"Save Image...  (Ctrl+S)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 15, isZh ? L"1:1 原始尺寸  (0 或鼠标中键)" : L"1:1 Original Scale  (0 or MButton)");
            AppendMenuW(menu, MF_STRING, 11, isZh ? L"顺时针旋转 90°  (R / 数字 2)" : L"Rotate Right 90°  (R / Key 2)");
            AppendMenuW(menu, MF_STRING, 12, isZh ? L"逆时针旋转 90°  (Shift+R / L / 数字 1)" : L"Rotate Left 90°  (Shift+R / L / Key 1)");
            AppendMenuW(menu, MF_STRING, 13, isZh ? L"水平翻转  (H / 数字 3)" : L"Flip Horizontal  (H / Key 3)");
            AppendMenuW(menu, MF_STRING, 14, isZh ? L"垂直翻转  (V / 数字 4)" : L"Flip Vertical  (V / Key 4)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, isZh ? L"透明度 100%  (数字 0 · [ / ] 微调)" : L"Opacity 100%  (Key 0 · [ / ] Step)");
            AppendMenuW(menu, MF_STRING, 3, isZh ? L"透明度 75%  (数字 7)" : L"Opacity 75%  (Key 7)");
            AppendMenuW(menu, MF_STRING, 4, isZh ? L"透明度 50%  (数字 5)" : L"Opacity 50%  (Key 5)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, (self->m_clickThrough ? MF_CHECKED : MF_UNCHECKED) | MF_STRING, 7, isZh ? L"鼠标穿透 / 幽灵模式  (T / F4)"
                                                                                                       : L"Click-through / Ghost  (T / F4)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            HMENU groupSubMenu = CreatePopupMenu();
            if (groupSubMenu) {
                populatePinGroupMenuItems(groupSubMenu, self, isZh);
                const std::wstring groupMenuTitle = (self->m_groupId >= PinGroupConstants::kMinGroupId)
                    ? (isZh ? std::format(L"贴图分组 [{}]  (Ctrl+1~9)", PinGroupPalette::shortLabel(self->m_groupId, true))
                            : std::format(L"Pin Group [{}]  (Ctrl+1~9)", PinGroupPalette::shortLabel(self->m_groupId, false)))
                    : (isZh ? L"贴图分组与按组查看  (Ctrl+1~9 / Alt+1~9)"
                            : L"Pin Groups & Filter  (Ctrl+1~9 / Alt+1~9)");
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(groupSubMenu), groupMenuTitle.c_str());
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            }
            AppendMenuW(menu, MF_STRING, 9, isZh ? L"整理全部贴图" : L"Arrange All Pins");
            AppendMenuW(menu, MF_STRING, 8, isZh ? L"隐藏全部贴图  (Ctrl+Alt+Shift+H 显示)"
                                               : L"Hide All Pins  (Ctrl+Alt+Shift+H shows)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 5, isZh ? L"关闭  (Esc / Delete / Ctrl+W)" : L"Close  (Esc / Delete / Ctrl+W)");
            AppendMenuW(menu, MF_STRING, 6, isZh ? L"关闭全部贴图" : L"Close All Pins");

            POINT pt;
            GetCursorPos(&pt);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);

            if (dispatchPinGroupMenuCommand(self, cmd)) {
                return 0;
            }

            switch (cmd) {
                case 25: self->copyAllText(); break;
                case 16: self->editMarkup(); break;
                case 18: self->toggleFold(); break;
                case 19: self->invertColors(); break;
                case 20: self->toggleGrayscale(); break;
                case 23: self->toggleTextSelectionMode(); break;
                case 24: self->selectAllText(); break;
                case 21: self->copySelectedText(); break;
                case 22: self->copyAllText(); break;
                case 1: copyImageToClipboard(self->m_sourceImage); break;
                case 17: self->extractText(); break;
                case 10: savePinnedImage(hwnd, self->m_sourceImage); break;
                case 15: self->resetScale(); break;
                case 11: self->rotate(90); break;
                case 12: self->rotate(270); break;
                case 13: self->flip(true); break;
                case 14: self->flip(false); break;
                case 2: self->setOpacity(1.0f); break;
                case 3: self->setOpacity(0.75f); break;
                case 4: self->setOpacity(0.5f); break;
                case 5: self->close(); break;
                case 6: PinWindow::closeAll(); break;
                case 7: self->setClickThrough(!self->m_clickThrough); break;
                case 8: PinWindow::toggleHideAll(); break;
                case 9: PinWindow::arrangeAll(); break;
            }
            return 0;
        }

        case WM_DPICHANGED: {
            if (!self) break;
            const UINT newDpi = HIWORD(wParam);
            self->m_dpiScale = tools3000::core::dpi::scaleForDpi(newDpi);
            self->m_cachedTextScale = 0.0f;
            self->m_uiLeadTextFormat.Reset();
            self->m_uiCenterTextFormat.Reset();
            if (self->m_folded) {
                RECT curRc{};
                GetWindowRect(hwnd, &curRc);
                if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
                    curRc.left = suggested->left;
                    curRc.top = suggested->top;
                }
                const SIZE foldSz = self->computeFoldCapsuleSize(self->m_dpiScale);
                SetWindowPos(hwnd, nullptr, curRc.left, curRc.top, foldSz.cx, foldSz.cy,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                if (self->m_renderTarget) {
                    self->m_renderTarget->Resize(D2D1::SizeU(
                        static_cast<UINT>(foldSz.cx), static_cast<UINT>(foldSz.cy)));
                }
                tools3000::core::WinUtils::applyUniversalRoundedCorners(
                    hwnd, foldSz.cx, foldSz.cy,
                    static_cast<int>(std::round(8.0f * self->m_dpiScale)));
            } else if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                self->updateWindowCornerRegion();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_PAINT: {
            if (self) self->render();
            ValidateRect(hwnd, nullptr);
            return 0;
        }

        case WM_NCDESTROY: {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (self) {
                if (self->m_hwnd == hwnd) {
                    self->m_hwnd = nullptr;
                }
                self->releaseGraphicsResources();
                self->m_sourceImage.release();
                s_instances.erase(
                    std::remove_if(s_instances.begin(), s_instances.end(),
                                   [self](const auto& p) { return !p || p.get() == self || !p->isAlive(); }),
                    s_instances.end());
            }
            break;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace tools3000::capture
