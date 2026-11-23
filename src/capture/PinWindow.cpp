// ─────────────────────────────────────────────────────────────────────────────
// PinWindow.cpp — 贴图窗口实现
//
// 功能:
//   - 截图后按 "钉住" 将截图贴在屏幕上
//   - 左键拖拽移动, 滚轮缩放, 双击关闭
//   - 支持透明度调节 (右键菜单)
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinWindow.h"
#include "capture/PinZoomStage.h"
#include "core/i18n/UiI18n.h"
#include "capture/PinWindowInternal.h"
#include "capture/WatermarkRenderer.h"
#include "capture/ScreenCapture.h"
#include "capture/ClipboardUtils.h"
#include "capture/CaptureOverlay.h"
#include "capture/CaptureVectorIcons.h"
#include "capture/FloatingGlassBar.h"
#include "capture/TextAnnotationHelper.h"
#include "capture/TextReplaceEngine.h"
#include "capture/CaptureHistory.h"
#include <imm.h>
#include "capture/PinSpawnCalculator.h"
#include "capture/PinPasteCoordinator.h"
#include "capture/PinInputController.h"
#include "capture/PinGroupManager.h"
#include "capture/PinGroupBarOverlay.h"
#include "capture/PinContextMenu.h"
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
#include "ocr/OcrTableReconstructor.h"
#include "ocr/OcrResultWindow.h"

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
using namespace pin_detail;

static constexpr const wchar_t* PIN_CLASS = pin_detail::kPinWindowClass;

// 静态成员
std::vector<std::shared_ptr<PinWindow>> PinWindow::s_instances;
bool PinWindow::s_classRegistered = false;
bool PinWindow::s_allHidden = false;
int PinWindow::s_activeGroupFilter = PinGroupConstants::kFilterAllGroups;
std::array<bool, PinGroupConstants::kMaxGroupId + 1> PinWindow::s_createdGroups{};

namespace pin_detail {
std::atomic<bool> s_ocrShuttingDown{false};
std::atomic<uint32_t> s_globalActiveOcrWorkers{0};
} // namespace pin_detail

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
                                             std::vector<MarkupElement> markupElements,
                                             bool autoStartLiveOcr) {
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
        // 写时复制 (COW)：未产生标注/原地滤镜修改前与 m_sourceImage 共享同一 cv::Mat 像素缓冲区，
        // 单张 4K 贴图立省 ~33 MB CPU 内存，任何原地修改前均会分离拷贝。
        pin->m_cleanBaseImage = pin->m_sourceImage;
        pin->m_markupElements.clear();
    }
    pin->m_cardCornerRadius = (std::max)(0.0f, cardCornerRadius);
    pin->m_pinnedSourceText = std::move(sourceText);
    pin->m_contentFingerprint = PinPasteCoordinator::computeContentFingerprint(pin->m_sourceImage, pin->m_pinnedSourceText);
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
        const std::string cardUtf8 = tools3000::core::WinUtils::wstringToUtf8(pin->m_pinnedSourceText);
        const cv::Rect cardBox(4, 4, (std::max)(1, image.cols - 8), (std::max)(1, image.rows - 8));
        pin->m_ocrBlocks.push_back(PinOcrBlock{
            cardUtf8,
            cardBox,
            tools3000::ocr::OcrEngine::buildProportionalCharBoxes(cardUtf8, cardBox)
        });
        pin->m_ocrReady = true;
    }

    ShowWindow(pin->m_hwnd, SW_SHOW);
    pin->focusWindow();
    pin->render();

    s_instances.push_back(pin);
    s_allHidden = false;
    refreshGroupBar();
    if (autoStartLiveOcr && pin->m_pinnedSourceText.empty() && pin->isLiveTextEnabled()) {
        wchar_t testEnvBuf[8]{};
        const DWORD envLen = GetEnvironmentVariableW(
            L"TOOLS3000_TEST_MODE", testEnvBuf, static_cast<DWORD>(std::size(testEnvBuf)));
        const bool inTestMode = (envLen > 0 && testEnvBuf[0] == L'1');
        if (!inTestMode) {
            // 真实用户贴图上屏后立即在后台静默预热极速 OCR，鼠标移上贴图时 0ms 瞬时响应选字
            pin->ensureOcrExecutedAsync(true);
        }
    }
    LOG_INFO("贴图窗口已创建: {}x{} @ ({},{}), 总数={}", image.cols, image.rows, x, y, s_instances.size());

    return pin;
}

PinWindow::PinWindow() = default;

void PinWindow::releaseGraphicsResources() noexcept {
    releaseZoomStage();
    if (m_zoomTimerPeriod) {
        m_zoomTimerPeriod->release();
    }
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
    stopZoomAnimation(false);
    if (m_editingTextReplaceIdx >= 0) {
        if (m_hwnd && IsWindow(m_hwnd)) {
            KillTimer(m_hwnd, 3);
        }
        m_editingTextReplaceIdx = -1;
        m_editingTextReplaceBackup.clear();
        m_editingTextReplaceWasNew = false;
        clearTextReplaceToolbarRects();
    }
    PinContextMenu::instance().dismiss();
    clearTranslationState();
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
    } else {
        if (closedGroup >= PinGroupConstants::kMinGroupId &&
            s_activeGroupFilter == closedGroup &&
            countInGroup(closedGroup) == 0) {
            setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
        } else {
            refreshGroupBar();
        }
    }
    // 单张贴图关闭属于明确冷路径（4K 贴图含 CPU+GPU 可达 60~100 MB），及时归还系统工作集
    tools3000::core::WinUtils::trimWorkingSet();
}

void PinWindow::closeAll() {
    PinContextMenu::instance().dismiss();
    std::vector<std::shared_ptr<PinWindow>> targets = std::move(s_instances);
    s_instances.clear();
    for (auto& pin : targets) {
        if (pin) {
            pin->stopZoomAnimation(false);
            pin->clearTranslationState();
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
    if (s_ocrShuttingDown.load(std::memory_order_acquire)) {
        PinZoomStage::unregisterWindowClass();
        UnregisterClassW(PIN_CLASS, GetModuleHandleW(nullptr));
        s_classRegistered = false;
    }
    broadcastGroupStateChanged();
    tools3000::core::WinUtils::trimWorkingSet();
    LOG_INFO("所有贴图窗口已关闭并释放资源");
}

void PinWindow::closeAllExcept(const PinWindow* keepPin) {
    PinContextMenu::instance().dismiss();
    std::vector<std::shared_ptr<PinWindow>> toClose;
    toClose.reserve(s_instances.size());
    for (const auto& pin : s_instances) {
        if (pin && pin.get() != keepPin && pin->isAlive()) {
            toClose.push_back(pin);
        }
    }
    if (toClose.empty()) return;

    for (auto& pin : toClose) {
        pin->stopZoomAnimation(false);
        pin->clearTranslationState();
        pin->m_sourceImage.release();
        pin->m_cleanBaseImage.release();
        pin->m_markupElements.clear();
        pin->releaseGraphicsResources();
        HWND hwndToDestroy = std::exchange(pin->m_hwnd, nullptr);
        if (hwndToDestroy && IsWindow(hwndToDestroy)) {
            DestroyWindow(hwndToDestroy);
        }
    }

    s_instances.erase(
        std::remove_if(s_instances.begin(), s_instances.end(),
                       [](const auto& p) { return !p || !p->isAlive(); }),
        s_instances.end());

    if (keepPin && keepPin->m_groupId >= PinGroupConstants::kMinGroupId &&
        s_activeGroupFilter != PinGroupConstants::kFilterAllGroups &&
        s_activeGroupFilter != keepPin->m_groupId) {
        setActiveGroupFilter(PinGroupConstants::kFilterAllGroups);
    } else {
        refreshGroupBar();
    }
    tools3000::core::WinUtils::trimWorkingSet();
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
        if (pin->m_zoomAnimating || pin->m_zoomStageActive) {
            pin->stopZoomAnimation(true);
        }
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
        if (pin->m_zoomAnimating || pin->m_zoomStageActive) {
            pin->stopZoomAnimation(true);
        }
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
        if (m_zoomAnimating || m_zoomStageActive) {
            stopZoomAnimation(true);
        }
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
        msg = tools3000::core::trWCstrFor(zh, L"已移出分组", L"Removed from group");
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
        if (pin->m_zoomAnimating || pin->m_zoomStageActive) {
            pin->stopZoomAnimation(true);
        }
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
        if (pin->m_zoomAnimating || pin->m_zoomStageActive) {
            pin->stopZoomAnimation(true);
        }
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
        pin->stopZoomAnimation(false);
        pin->clearTranslationState();
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

PinWindow::~PinWindow() {
    stopZoomAnimation(false);
    releaseGraphicsResources();
    m_zoomTimerPeriod.reset();
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
    if (m_zoomStageActive) {
        SetLayeredWindowAttributes(m_hwnd, 0, 0, LWA_ALPHA);
        return;
    }
    BYTE alpha = static_cast<BYTE>(std::clamp(m_opacity * 255.0f, 10.0f, 255.0f));
    SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);
}

void PinWindow::applySharpWindowCorners() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    SetWindowRgn(m_hwnd, nullptr, TRUE);
    // DWMWA_NCRENDERING_POLICY = 2, DWMNCRP_ENABLED = 2 (enables soft outer DWM drop shadow against same-color backgrounds)
    constexpr DWORD kDwmNcRenderingPolicy = 2;
    const DWORD ncrpEnabled = 2;
    DwmSetWindowAttribute(m_hwnd, kDwmNcRenderingPolicy, &ncrpEnabled, sizeof(ncrpEnabled));
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

void PinWindow::showTransientHudBadge(std::wstring text, uint32_t durationMs, bool triggerInvalidate) {
    m_hudBadgeText = std::move(text);
    m_hudBadgeUntilTick = GetTickCount64() + static_cast<uint64_t>((std::max)(100u, durationMs));
    if (m_hwnd && IsWindow(m_hwnd)) {
        SetTimer(m_hwnd, 2, (std::max)(100u, durationMs) + 25u, nullptr);
        if (triggerInvalidate) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}

std::wstring PinWindow::activeHudBadgeText() const {
    if (m_hudBadgeText.empty() || GetTickCount64() >= m_hudBadgeUntilTick) {
        return {};
    }
    return m_hudBadgeText;
}

void PinWindow::setOpacity(float opacity) {
    const float clamped = std::clamp(opacity, 0.1f, 1.0f);
    const bool changed = std::abs(clamped - m_opacity) > 0.001f;
    m_opacity = clamped;
    applyLayeredOpacity();
    if (changed && m_hwnd && IsWindowVisible(m_hwnd) && !m_folded) {
        showTransientHudBadge(
            std::format(L"{}%", static_cast<int>(std::lround(m_opacity * 100.0f))),
            800,
            /*triggerInvalidate=*/false);
        if (m_zoomStageActive && m_zoomStage && m_zoomStage->isActive()) {
            renderCurrentZoomStageFrame();
        } else {
            render();
        }
    }
}

void PinWindow::setClickThrough(bool enable) {
    if (!m_hwnd) return;
    if (m_zoomAnimating || m_zoomStageActive) {
        stopZoomAnimation(true);
    }
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
        tools3000::core::ShowToastEvent{
            enable
                ? std::wstring(tools3000::core::trWCstrFor(isZh, L"贴图已开启鼠标穿透 (按 F4 或悬浮按 Ctrl+Alt+Shift+X 恢复)", L"Click-through enabled (F4 or Ctrl+Alt+Shift+X to restore)"))
                : std::wstring(tools3000::core::trWCstrFor(isZh, L"贴图已退出鼠标穿透模式", L"Click-through disabled"))});
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

// 将 cv::Mat 以标准底向上 CF_DIB 与 PNG 写入剪贴板（贴图外发复制出口自动按配置压印企业/品牌水印，不影响内存原图 OCR）
namespace pin_detail {
bool copyImageToClipboard(const cv::Mat& image, HWND ownerHwnd) {
    if (image.empty()) return false;
    const cv::Mat exported = WatermarkRenderer::applyGlobalCaptureWatermarkIfEnabled(image);
    return ClipboardUtils::copyImageToClipboard(exported, L"", ownerHwnd);
}

bool savePinnedImage(HWND owner, const cv::Mat& image) {
    if (image.empty()) return false;
    const bool zh = tools3000::core::isUiLanguageChinese();

    ComPtr<IFileSaveDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ERROR("贴图保存对话框创建失败: hr=0x{:08X}", static_cast<unsigned>(hr));
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{tools3000::core::trWCstrFor(zh, L"无法打开保存对话框", L"Unable to open save dialog")});
        return false;
    }

    const COMDLG_FILTERSPEC filters[] = {
        {tools3000::core::trWCstrFor(zh, L"PNG 图片 (*.png)", L"PNG Image (*.png)"), L"*.png"},
        {tools3000::core::trWCstrFor(zh, L"JPEG 图片 (*.jpg;*.jpeg)", L"JPEG Image (*.jpg;*.jpeg)"), L"*.jpg;*.jpeg"},
        {tools3000::core::trWCstrFor(zh, L"WebP 图片 (*.webp)", L"WebP Image (*.webp)"), L"*.webp"},
        {tools3000::core::trWCstrFor(zh, L"BMP 图片 (*.bmp)", L"BMP Image (*.bmp)"), L"*.bmp"},
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
            tools3000::core::ShowToastEvent{tools3000::core::trWCstrFor(zh, L"不支持该图片格式", L"Unsupported image format")});
        return false;
    }

    try {
        const cv::Mat exported = WatermarkRenderer::applyGlobalCaptureWatermarkIfEnabled(image);
        std::vector<uchar> encoded;
        std::vector<int> params;
        if (encodeExt == ".jpg") params = {cv::IMWRITE_JPEG_QUALITY, 95};
        else if (encodeExt == ".webp") params = {cv::IMWRITE_WEBP_QUALITY, 95};
        if (!cv::imencode(encodeExt, exported, encoded, params)) return false;

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size()));
        if (!file) throw std::runtime_error("write failed");
        LOG_INFO("贴图已保存: {}", tools3000::core::WinUtils::wstringToUtf8(path.wstring()));
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{tools3000::core::trWCstrFor(zh, L"贴图已保存", L"Pinned image saved")});
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("贴图保存失败: {}", e.what());
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{tools3000::core::trWCstrFor(zh, L"贴图保存失败", L"Failed to save pinned image")});
        return false;
    }
}

} // namespace pin_detail

void PinWindow::releaseZoomStage() noexcept {
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, kTimerIdZoomStageIdleRelease);
    }
    const bool wasStageActive = m_zoomStageActive;
    m_zoomStageActive = false;
    if (m_zoomStage) {
        m_zoomStage->destroy();
        m_zoomStage.reset();
    }
    if (wasStageActive && m_hwnd && IsWindow(m_hwnd)) {
        applyLayeredOpacity();
    }
}

void PinWindow::invalidateZoomStageTexture() noexcept {
    ++m_displayImageGeneration;
    if (m_zoomStageActive || m_zoomAnimating) {
        stopZoomAnimation(false);
    }
    if (m_zoomStage) {
        m_zoomStage->invalidateUploadedImage();
    }
}

void PinWindow::renderCurrentZoomStageFrame() {
    if (!m_zoomStageActive || !m_zoomStage || !m_zoomStage->isActive() || !m_hwnd) {
        return;
    }
    constexpr double kMaxSafeWindowDim = 32000.0;
    const double curW = std::clamp(static_cast<double>(m_origWidth) * m_zoomCurScale, 1.0, kMaxSafeWindowDim);
    const double curH = std::clamp(static_cast<double>(m_origHeight) * m_zoomCurScale, 1.0, kMaxSafeWindowDim);

    PinZoomStageFrameParams frame{};
    frame.drawLeft = m_zoomCurLeft;
    frame.drawTop = m_zoomCurTop;
    frame.drawWidth = curW;
    frame.drawHeight = curH;
    frame.scale = static_cast<float>(m_zoomCurScale);
    frame.opacity = m_opacity;
    frame.cardCornerRadius = m_cardCornerRadius;
    frame.dpiScale = tools3000::core::dpi::scaleForWindow(m_hwnd);
    m_dpiScale = frame.dpiScale;
    frame.focused = m_focused;
    frame.clickThrough = m_clickThrough;
    frame.groupId = m_groupId;
    frame.badgeText = activeHudBadgeText();
    if (frame.badgeText.empty()) {
        frame.badgeText = std::format(L"{}%", static_cast<int>(std::lround(m_zoomCurScale * 100.0)));
    }

    if (!m_zoomStage->renderFrame(frame)) {
        // GPU 提交异常或设备丢失：无缝降级至优化传统路径，绝不黑屏或卡死
        m_zoomStageActive = false;
        releaseZoomStage();
        applyLayeredOpacity();
        if (m_zoomAnimating && m_hwnd && IsWindow(m_hwnd)) {
            SetTimer(m_hwnd, kTimerIdZoomAnim, 4, nullptr);
            applyScaleAtAnchor(
                m_scale,
                POINT{static_cast<LONG>(std::llround(m_zoomAnchorX)),
                      static_cast<LONG>(std::llround(m_zoomAnchorY))},
                static_cast<float>(m_zoomAnchorRelX),
                static_cast<float>(m_zoomAnchorRelY),
                /*finalFrame=*/false);
        }
    }
}

bool PinWindow::startOrUpdateZoomStage(const RECT& currentPinRect, double initialDt) {
    if (!m_hwnd || !IsWindow(m_hwnd) || m_folded) return false;
    const cv::Mat& displayImg = activeDisplayImage();
    if (displayImg.empty()) return false;

    constexpr double kZoomDamping = 16.5;
    constexpr double kLogConvergeEps = 0.0018;
    constexpr double kMinScale = 0.1;
    constexpr double kMaxScale = 5.0;
    constexpr double kMaxSafeWindowDim = 32000.0;

    // 预估目标尺寸包围盒，用于多屏跨屏缩放时分配安全舞台区域
    const double targetW = std::clamp(static_cast<double>(m_origWidth) * m_zoomTargetScale, 1.0, kMaxSafeWindowDim);
    const double targetH = std::clamp(static_cast<double>(m_origHeight) * m_zoomTargetScale, 1.0, kMaxSafeWindowDim);
    const int projLeft = static_cast<int>(std::llround(m_zoomAnchorX - targetW * m_zoomAnchorRelX));
    const int projTop = static_cast<int>(std::llround(m_zoomAnchorY - targetH * m_zoomAnchorRelY));
    const RECT projectedTargetRect{
        projLeft, projTop,
        projLeft + static_cast<int>(std::llround(targetW)),
        projTop + static_cast<int>(std::llround(targetH))
    };

    if (!m_zoomStage) {
        m_zoomStage = std::make_unique<PinZoomStage>();
        std::weak_ptr<PinWindow> weakSelf;
        try {
            weakSelf = shared_from_this();
        } catch (const std::bad_weak_ptr&) {
            // 理论上总由 shared_ptr 持有
        }
        PinZoomStageCallbacks cbs{};
        cbs.onVSyncTick = [weakSelf]() {
            if (auto locked = weakSelf.lock(); locked && locked->isAlive()) {
                locked->stepZoomAnimation();
            }
        };
        cbs.onMouseWheel = [weakSelf](WPARAM wParam, LPARAM lParam) {
            if (auto locked = weakSelf.lock(); locked && locked->isAlive()) {
                locked->handleMouseWheel(wParam, lParam);
            }
        };
        cbs.onInterruptMouse = [weakSelf](UINT msg, WPARAM wParam, POINT screenPt) {
            if (auto locked = weakSelf.lock(); locked && locked->isAlive()) {
                locked->handleZoomStageMouseInterrupt(msg, wParam, screenPt);
            }
        };
        cbs.onDisplayChanged = [weakSelf]() {
            if (auto locked = weakSelf.lock(); locked && locked->isAlive()) {
                locked->stopZoomAnimation(true);
                locked->releaseZoomStage();
            }
        };
        m_zoomStage->setCallbacks(std::move(cbs));
    }

    HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    if (!m_zoomStage->prepare(displayImg, m_displayImageGeneration, currentPinRect, projectedTargetRect, mon)) {
        releaseZoomStage();
        return false;
    }

    // 零起手延迟：直接以首帧周期推进第一步对数阻尼并作为舞台首帧提交
    const double refreshHz = static_cast<double>((std::max)(60u, m_zoomStage->refreshHz()));
    const double firstDt = std::clamp(initialDt > 0.0 ? initialDt : (1.0 / refreshHz), 0.004, 0.016);
    const double logCur = std::log((std::max)(0.05, m_zoomCurScale));
    const double logDst = std::log((std::max)(0.05, m_zoomTargetScale));
    const double logDiff = logDst - logCur;

    if (std::abs(logDiff) <= kLogConvergeEps) {
        finishZoomAnimationAtScale(m_zoomTargetScale, /*updateHudBadge=*/true);
        return true;
    }

    const double alpha = 1.0 - std::exp(-firstDt * kZoomDamping);
    const double nextScale = std::clamp(std::exp(logCur + logDiff * alpha), kMinScale, kMaxScale);
    const double nextW = std::clamp(static_cast<double>(m_origWidth) * nextScale, 1.0, kMaxSafeWindowDim);
    const double nextH = std::clamp(static_cast<double>(m_origHeight) * nextScale, 1.0, kMaxSafeWindowDim);
    const double nextLeft = m_zoomAnchorX - nextW * m_zoomAnchorRelX;
    const double nextTop = m_zoomAnchorY - nextH * m_zoomAnchorRelY;

    m_zoomCurScale = nextScale;
    m_zoomCurLeft = nextLeft;
    m_zoomCurTop = nextTop;
    m_scale = static_cast<float>(nextScale);

    const std::wstring pctText = std::format(L"{}%", static_cast<int>(std::lround(m_scale * 100.0f)));
    m_hudBadgeText = pctText;
    m_hudBadgeUntilTick = GetTickCount64() + 800ULL;

    PinZoomStageFrameParams firstFrame{};
    firstFrame.drawLeft = nextLeft;
    firstFrame.drawTop = nextTop;
    firstFrame.drawWidth = nextW;
    firstFrame.drawHeight = nextH;
    firstFrame.scale = m_scale;
    firstFrame.opacity = m_opacity;
    firstFrame.cardCornerRadius = m_cardCornerRadius;
    firstFrame.dpiScale = tools3000::core::dpi::scaleForWindow(m_hwnd);
    m_dpiScale = firstFrame.dpiScale;
    firstFrame.focused = m_focused;
    firstFrame.clickThrough = m_clickThrough;
    firstFrame.groupId = m_groupId;
    firstFrame.badgeText = pctText;

    if (!m_zoomStage->beginSession(firstFrame)) {
        releaseZoomStage();
        return false;
    }

    // 舞台首帧已提交并置顶显现：立即将原 PinWindow 视觉隐身但完整保留焦点与可见标志
    m_zoomStageActive = true;
    SetLayeredWindowAttributes(m_hwnd, 0, 0, LWA_ALPHA);
    return true;
}

void PinWindow::handleMouseWheel(WPARAM wParam, LPARAM lParam) {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    focusWindow();
    const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    const bool ctrl = (LOWORD(wParam) & MK_CONTROL) != 0 ||
                      ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
    const bool shift = (LOWORD(wParam) & MK_SHIFT) != 0 ||
                       ((GetKeyState(VK_SHIFT) & 0x8000) != 0);

    if (ctrl || shift) {
        // Ctrl/Shift + 滚轮：平滑微调透明度 (10% ~ 100%, 步进 5%)
        const float newOpacity = m_opacity + (delta > 0 ? 0.05f : -0.05f);
        setOpacity(newOpacity);
        return;
    }

    POINT candidatePt{};
    bool hasCandidate = false;
    if (lParam != 0) {
        candidatePt.x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        candidatePt.y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        hasCandidate = true;
    } else if (GetCursorPos(&candidatePt)) {
        hasCandidate = true;
    }
    handleZoomWheel(delta, candidatePt, hasCandidate);
}

void PinWindow::handleZoomWheel(int delta, POINT cursorScreenPt, bool hasCursorPt) {
    if (!m_hwnd || !IsWindow(m_hwnd) || m_folded || delta == 0 || m_origWidth <= 0 || m_origHeight <= 0) {
        return;
    }

    constexpr double kMinScale = 0.1;
    constexpr double kMaxScale = 5.0;
    constexpr double kWheelStepBase = 1.12;
    constexpr double kMaxSafeWindowDim = 32000.0;

    // 保证全桌面同一时刻至多 1 张贴图占用硬件缩放舞台，避免多舞台互挡或显存倍增
    for (const auto& other : s_instances) {
        if (other && other.get() != this && (other->m_zoomAnimating || other->m_zoomStageActive)) {
            other->stopZoomAnimation(true);
        }
    }

    const double notches = static_cast<double>(delta) / static_cast<double>(WHEEL_DELTA);
    const double zoomFactor = std::pow(kWheelStepBase, notches);
    const double baseScale = m_zoomAnimating ? m_zoomTargetScale : static_cast<double>(m_scale);
    const double newTarget = std::clamp(baseScale * zoomFactor, kMinScale, kMaxScale);

    const double activeScale = m_zoomAnimating ? m_zoomCurScale : static_cast<double>(m_scale);
    if (std::abs(newTarget - activeScale) <= 0.0004 && std::abs(newTarget - baseScale) <= 0.0004) {
        return;
    }

    RECT curPinWinRc{};
    GetWindowRect(m_hwnd, &curPinWinRc);
    double curLeft = 0.0;
    double curTop = 0.0;
    double curW = 1.0;
    double curH = 1.0;

    if (!m_zoomAnimating) {
        m_zoomCurScale = static_cast<double>(m_scale);
        curLeft = static_cast<double>(curPinWinRc.left);
        curTop = static_cast<double>(curPinWinRc.top);
        curW = (std::max)(1.0, static_cast<double>(curPinWinRc.right - curPinWinRc.left));
        curH = (std::max)(1.0, static_cast<double>(curPinWinRc.bottom - curPinWinRc.top));
        m_zoomCurLeft = curLeft;
        m_zoomCurTop = curTop;
    } else {
        // 动画进行中严格使用高精度浮点矩形，杜绝整数四舍五入引发连滚漂移
        curLeft = m_zoomCurLeft;
        curTop = m_zoomCurTop;
        curW = std::clamp(static_cast<double>(m_origWidth) * m_zoomCurScale, 1.0, kMaxSafeWindowDim);
        curH = std::clamp(static_cast<double>(m_origHeight) * m_zoomCurScale, 1.0, kMaxSafeWindowDim);
    }

    // 锚点零漂移与零跳变规则：
    // - 原地快速连滚（光标移动 <= 1px）：保持原有浮点 (anchorX, anchorY, relX, relY) 0.000px 漂移；
    // - 滚轮中途光标移至新点：由当前浮点矩形反算新 (relX, relY)，确保画面 0.000px 零跳变。
    const bool keepExistingAnchor =
        m_zoomAnimating && hasCursorPt &&
        std::abs(static_cast<double>(cursorScreenPt.x) - m_zoomAnchorX) <= 1.0 &&
        std::abs(static_cast<double>(cursorScreenPt.y) - m_zoomAnchorY) <= 1.0;

    if (!keepExistingAnchor) {
        const double cx = static_cast<double>(cursorScreenPt.x);
        const double cy = static_cast<double>(cursorScreenPt.y);
        if (hasCursorPt &&
            cx >= curLeft && cx <= curLeft + curW &&
            cy >= curTop && cy <= curTop + curH) {
            m_zoomAnchorX = cx;
            m_zoomAnchorY = cy;
            m_zoomAnchorRelX = (cx - curLeft) / curW;
            m_zoomAnchorRelY = (cy - curTop) / curH;
        } else {
            m_zoomAnchorX = curLeft + curW * 0.5;
            m_zoomAnchorY = curTop + curH * 0.5;
            m_zoomAnchorRelX = 0.5;
            m_zoomAnchorRelY = 0.5;
        }
    }

    m_zoomTargetScale = newTarget;
    KillTimer(m_hwnd, kTimerIdZoomStageIdleRelease);

    if (!m_zoomTimerPeriod) {
        m_zoomTimerPeriod = std::make_unique<ScopedHighResTimerPeriod>();
    }
    m_zoomTimerPeriod->acquire();

    const bool startingAnim = !m_zoomAnimating;
    m_zoomAnimating = true;

    if (startingAnim) {
        if (startOrUpdateZoomStage(curPinWinRc, 0.008)) {
            // 在舞台设备初始化与首帧提交完成后再记录 QPC，防止冷启动建设备耗时混入第 2 帧 dt
            LARGE_INTEGER postInitQpc{};
            QueryPerformanceCounter(&postInitQpc);
            m_zoomLastQpc = postInitQpc.QuadPart;
            return;
        }

        // 健壮降级兜底路径（无 DComp/D3D11 时）
        m_zoomLastQpc = 0;
        SetTimer(m_hwnd, kTimerIdZoomAnim, 4, nullptr);
        stepZoomAnimation();
    } else if (m_zoomStageActive && m_zoomStage && m_zoomStage->isActive()) {
        // 连续滚轮中：距上一帧已超半帧周期时立即补推一帧，手感跟手度拉满
        LARGE_INTEGER nowQpc{}, freqQpc{};
        QueryPerformanceCounter(&nowQpc);
        QueryPerformanceFrequency(&freqQpc);
        if (m_zoomLastQpc > 0 && freqQpc.QuadPart > 0) {
            const double sinceLast = static_cast<double>(nowQpc.QuadPart - m_zoomLastQpc) /
                                     static_cast<double>(freqQpc.QuadPart);
            const double halfFrame = 0.5 / static_cast<double>((std::max)(60u, m_zoomStage->refreshHz()));
            if (sinceLast >= halfFrame) {
                stepZoomAnimation();
            }
        }
    }
}

void PinWindow::handleZoomStageMouseInterrupt(UINT msg, WPARAM wParam, POINT screenPt) {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    if (m_zoomAnimating || m_zoomStageActive) {
        stopZoomAnimation(/*renderFinalSharpness=*/true, /*snapToTarget=*/false);
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_MBUTTONUP) {
        POINT clientPt = screenPt;
        ScreenToClient(m_hwnd, &clientPt);
        SendMessageW(m_hwnd, msg, wParam, MAKELPARAM(clientPt.x, clientPt.y));
    }
}

void PinWindow::finishZoomAnimationAtScale(double finalScale, bool updateHudBadge) {
    const bool wasStageActive = m_zoomStageActive;
    m_zoomAnimating = false;
    m_zoomStageActive = false;
    m_zoomLastQpc = 0;

    if (m_zoomTimerPeriod) {
        m_zoomTimerPeriod->release();
    }
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, kTimerIdZoomAnim);
    }

    if (!m_hwnd || !IsWindow(m_hwnd) || m_folded) {
        if (m_zoomStage) {
            m_zoomStage->hideKeepWarm();
        }
        return;
    }

    constexpr int64_t kMaxSafeWindowDim = 32000;
    const double clamped = std::clamp(finalScale, 0.1, 5.0);
    const int64_t scaledW64 = std::clamp(
        static_cast<int64_t>(std::llround(static_cast<double>(m_origWidth) * clamped)),
        int64_t{1}, kMaxSafeWindowDim);
    const int64_t scaledH64 = std::clamp(
        static_cast<int64_t>(std::llround(static_cast<double>(m_origHeight) * clamped)),
        int64_t{1}, kMaxSafeWindowDim);
    const int finalW = static_cast<int>(scaledW64);
    const int finalH = static_cast<int>(scaledH64);
    const int finalLeft = static_cast<int>(std::llround(m_zoomAnchorX - static_cast<double>(finalW) * m_zoomAnchorRelX));
    const int finalTop = static_cast<int>(std::llround(m_zoomAnchorY - static_cast<double>(finalH) * m_zoomAnchorRelY));

    m_zoomCurScale = clamped;
    m_zoomTargetScale = clamped;
    m_zoomCurLeft = static_cast<double>(finalLeft);
    m_zoomCurTop = static_cast<double>(finalTop);
    m_scale = static_cast<float>(clamped);

    // 1. 原 PinWindow 此时仍为视觉隐身（若由舞台接管）：一次性落位（不带 SWP_NOREDRAW 以同步刷新 CS_DROPSHADOW）+ 圆角 + Resize + HIGH_QUALITY_CUBIC 终帧绘制
    SetWindowPos(
        m_hwnd, nullptr, finalLeft, finalTop, finalW, finalH,
        SWP_NOZORDER | SWP_NOACTIVATE);
    updateWindowCornerRegion();
    if (m_renderTarget) {
        m_renderTarget->SetDpi(96.0f, 96.0f);
        const UINT32 maxRt = (std::max)(256u, m_renderTarget->GetMaximumBitmapSize());
        const UINT32 rtW = (std::min)(static_cast<UINT32>((std::max)(1, finalW)), maxRt);
        const UINT32 rtH = (std::min)(static_cast<UINT32>((std::max)(1, finalH)), maxRt);
        const D2D1_SIZE_U curPx = m_renderTarget->GetPixelSize();
        if (curPx.width != rtW || curPx.height != rtH) {
            m_renderTarget->Resize(D2D1::SizeU(rtW, rtH));
        }
    }
    if (updateHudBadge) {
        showTransientHudBadge(
            std::format(L"{}%", static_cast<int>(std::lround(m_scale * 100.0f))),
            800,
            /*triggerInvalidate=*/false);
    }
    render();

    // 2. 零闪烁原子交接：原 PinWindow 瞬时恢复真实不透明度，紧接着隐藏舞台并启动 2.5s 闲置自动回收
    if (wasStageActive) {
        applyLayeredOpacity();
        if (m_zoomStage) {
            m_zoomStage->hideKeepWarm();
        }
        SetTimer(m_hwnd, kTimerIdZoomStageIdleRelease, 2500, nullptr);
    }
}

void PinWindow::stopZoomAnimation(bool renderFinalSharpness, bool snapToTarget) {
    const bool wasAnimating = m_zoomAnimating;
    const bool wasStageActive = m_zoomStageActive;
    if (!wasAnimating && !wasStageActive) {
        if (m_zoomTimerPeriod) {
            m_zoomTimerPeriod->release();
        }
        return;
    }

    if (renderFinalSharpness && m_hwnd && IsWindow(m_hwnd) && !m_folded) {
        finishZoomAnimationAtScale(
            snapToTarget ? m_zoomTargetScale : m_zoomCurScale,
            /*updateHudBadge=*/true);
        return;
    }

    m_zoomAnimating = false;
    m_zoomStageActive = false;
    m_zoomLastQpc = 0;

    if (m_zoomTimerPeriod) {
        m_zoomTimerPeriod->release();
    }
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, kTimerIdZoomAnim);
    }

    if (wasStageActive) {
        if (m_hwnd && IsWindow(m_hwnd) && !m_folded && m_origWidth > 0 && m_origHeight > 0) {
            // 先把原窗口同步至舞台当前实时位置与尺寸，确保紧随其后的 rotate/toggleFold 等能读到精准坐标
            constexpr int64_t kMaxSafeWindowDim = 32000;
            const double clamped = std::clamp(m_zoomCurScale, 0.1, 5.0);
            const int curW = static_cast<int>(std::clamp(
                static_cast<int64_t>(std::llround(static_cast<double>(m_origWidth) * clamped)),
                int64_t{1}, kMaxSafeWindowDim));
            const int curH = static_cast<int>(std::clamp(
                static_cast<int64_t>(std::llround(static_cast<double>(m_origHeight) * clamped)),
                int64_t{1}, kMaxSafeWindowDim));
            const int curLeft = static_cast<int>(std::llround(m_zoomCurLeft));
            const int curTop = static_cast<int>(std::llround(m_zoomCurTop));
            m_scale = static_cast<float>(clamped);
            SetWindowPos(
                m_hwnd, nullptr, curLeft, curTop, curW, curH,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
            applyLayeredOpacity();
        }
        if (m_zoomStage) {
            m_zoomStage->hideKeepWarm();
        }
        if (m_hwnd && IsWindow(m_hwnd)) {
            SetTimer(m_hwnd, kTimerIdZoomStageIdleRelease, 2500, nullptr);
        }
    }
}

void PinWindow::applyScaleAtAnchor(
    float newScale,
    POINT anchorScreenPt,
    float relX,
    float relY,
    bool finalFrame) {
    if (!m_hwnd || !IsWindow(m_hwnd) || m_folded) return;
    constexpr int64_t kMaxSafeWindowDim = 32000;
    const float clamped = std::clamp(newScale, 0.1f, 5.0f);
    const int64_t scaledW64 = std::clamp(
        static_cast<int64_t>(std::llround(static_cast<double>(m_origWidth) * clamped)),
        int64_t{1}, kMaxSafeWindowDim);
    const int64_t scaledH64 = std::clamp(
        static_cast<int64_t>(std::llround(static_cast<double>(m_origHeight) * clamped)),
        int64_t{1}, kMaxSafeWindowDim);
    const int newW = static_cast<int>(scaledW64);
    const int newH = static_cast<int>(scaledH64);

    const int newLeft = anchorScreenPt.x - static_cast<int>(std::llround(static_cast<double>(newW) * relX));
    const int newTop = anchorScreenPt.y - static_cast<int>(std::llround(static_cast<double>(newH) * relY));

    m_scale = clamped;
    SetWindowPos(
        m_hwnd, nullptr, newLeft, newTop, newW, newH,
        SWP_NOZORDER | SWP_NOACTIVATE | (finalFrame ? 0u : SWP_NOREDRAW));

    // 降级路径优化：仅在终帧调用一次 SetWindowRgn，消除动画中间帧 GDI 区域重算卡顿
    if (finalFrame) {
        updateWindowCornerRegion();
    }
    if (m_renderTarget) {
        m_renderTarget->SetDpi(96.0f, 96.0f);
        const UINT32 maxRt = (std::max)(256u, m_renderTarget->GetMaximumBitmapSize());
        const UINT32 rtW = (std::min)(static_cast<UINT32>((std::max)(1, newW)), maxRt);
        const UINT32 rtH = (std::min)(static_cast<UINT32>((std::max)(1, newH)), maxRt);
        const D2D1_SIZE_U curPx = m_renderTarget->GetPixelSize();
        if (curPx.width != rtW || curPx.height != rtH) {
            m_renderTarget->Resize(D2D1::SizeU(rtW, rtH));
        }
    }
    // 消除双重重绘：triggerInvalidate=false，仅由下方单次 render() 呈现
    showTransientHudBadge(
        std::format(L"{}%", static_cast<int>(std::lround(m_scale * 100.0f))),
        800,
        /*triggerInvalidate=*/false);
    render();
}

void PinWindow::stepZoomAnimation() {
    if (!m_zoomAnimating || !m_hwnd || !IsWindow(m_hwnd) || m_folded) {
        stopZoomAnimation(false);
        return;
    }

    constexpr double kZoomDamping = 16.5;
    constexpr double kLogConvergeEps = 0.0018;
    constexpr double kMinScale = 0.1;
    constexpr double kMaxScale = 5.0;
    constexpr double kMaxSafeWindowDim = 32000.0;

    LARGE_INTEGER nowQpc{}, freqQpc{};
    QueryPerformanceCounter(&nowQpc);
    QueryPerformanceFrequency(&freqQpc);
    double dt = 0.008;
    if (m_zoomLastQpc > 0 && freqQpc.QuadPart > 0) {
        const double rawDt = static_cast<double>(nowQpc.QuadPart - m_zoomLastQpc) /
                             static_cast<double>(freqQpc.QuadPart);
        dt = std::clamp(rawDt, 0.0005, 0.045);
    }
    m_zoomLastQpc = nowQpc.QuadPart;

    // 对数空间指数阻尼平滑：放大到 500% 与缩小到 10% 视觉变焦速率绝对恒定匀称
    const double logCur = std::log((std::max)(0.05, m_zoomCurScale));
    const double logDst = std::log((std::max)(0.05, m_zoomTargetScale));
    const double logDiff = logDst - logCur;
    if (std::abs(logDiff) <= kLogConvergeEps) {
        finishZoomAnimationAtScale(m_zoomTargetScale, /*updateHudBadge=*/true);
        return;
    }

    const double alpha = 1.0 - std::exp(-dt * kZoomDamping);
    const double nextScale = std::clamp(std::exp(logCur + logDiff * alpha), kMinScale, kMaxScale);
    const double nextW = std::clamp(static_cast<double>(m_origWidth) * nextScale, 1.0, kMaxSafeWindowDim);
    const double nextH = std::clamp(static_cast<double>(m_origHeight) * nextScale, 1.0, kMaxSafeWindowDim);
    const double nextLeft = m_zoomAnchorX - nextW * m_zoomAnchorRelX;
    const double nextTop = m_zoomAnchorY - nextH * m_zoomAnchorRelY;

    m_zoomCurScale = nextScale;
    m_zoomCurLeft = nextLeft;
    m_zoomCurTop = nextTop;
    m_scale = static_cast<float>(nextScale);

    if (m_zoomStageActive && m_zoomStage && m_zoomStage->isActive()) {
        const std::wstring pctText = std::format(L"{}%", static_cast<int>(std::lround(m_scale * 100.0f)));
        m_hudBadgeText = pctText;
        m_hudBadgeUntilTick = GetTickCount64() + 800ULL;
        renderCurrentZoomStageFrame();
        return;
    }

    applyScaleAtAnchor(
        m_scale,
        POINT{static_cast<LONG>(std::llround(m_zoomAnchorX)),
              static_cast<LONG>(std::llround(m_zoomAnchorY))},
        static_cast<float>(m_zoomAnchorRelX),
        static_cast<float>(m_zoomAnchorRelY),
        /*finalFrame=*/false);
}

void PinWindow::setScale(float scale) {
    stopZoomAnimation(false);
    const float clamped = std::clamp(scale, 0.1f, 5.0f);
    const bool changed = std::abs(clamped - m_scale) > 0.001f;
    m_scale = clamped;
    m_zoomCurScale = static_cast<double>(clamped);
    m_zoomTargetScale = static_cast<double>(clamped);
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
        if (changed && IsWindowVisible(m_hwnd)) {
            showTransientHudBadge(
                std::format(L"{}%", static_cast<int>(std::lround(m_scale * 100.0f))),
                800,
                /*triggerInvalidate=*/false);
        }
        render();
    }
}

void PinWindow::rotate(int angleDeg) {
    if (m_sourceImage.empty() || !m_hwnd) return;
    if (PinInputController::normalizeRightAngleDegrees(angleDeg) == 0) return;
    stopZoomAnimation(false);
    clearTranslationState();

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
    const bool sharedCleanBase = (!m_cleanBaseImage.empty() && m_cleanBaseImage.data == m_sourceImage.data);
    if (!PinInputController::applyRotation(m_sourceImage, m_colorBackup, m_origWidth, m_origHeight, angleDeg)) {
        return;
    }
    if (sharedCleanBase) {
        m_cleanBaseImage = m_sourceImage;
    } else if (!m_cleanBaseImage.empty()) {
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
            for (auto& rc : el.textSpans) {
                cv::Point c1(rc.x, rc.y);
                cv::Point c2(rc.x + rc.width, rc.y + rc.height);
                rotPt(c1);
                rotPt(c2);
                const int nx1 = (std::min)(c1.x, c2.x);
                const int ny1 = (std::min)(c1.y, c2.y);
                const int nx2 = (std::max)(c1.x, c2.x);
                const int ny2 = (std::max)(c1.y, c2.y);
                rc = cv::Rect(nx1, ny1, (std::max)(1, nx2 - nx1), (std::max)(1, ny2 - ny1));
            }
        }
    }
    m_pinUndoStack.clear();
    m_pinRedoStack.clear();
    m_ocrMarkupPreviewClean = false;

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
    stopZoomAnimation(false);
    clearTranslationState();
    const bool sharedCleanBase = (!m_cleanBaseImage.empty() && m_cleanBaseImage.data == m_sourceImage.data);
    if (!PinInputController::applyFlip(m_sourceImage, m_colorBackup, horizontal)) {
        return;
    }
    if (sharedCleanBase) {
        m_cleanBaseImage = m_sourceImage;
    } else if (!m_cleanBaseImage.empty()) {
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
            for (auto& rc : el.textSpans) {
                if (horizontal) {
                    rc.x = m_origWidth - (rc.x + rc.width);
                } else {
                    rc.y = m_origHeight - (rc.y + rc.height);
                }
            }
        }
    }
    m_pinUndoStack.clear();
    m_pinRedoStack.clear();
    m_ocrMarkupPreviewClean = false;
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
    stopZoomAnimation(false);
    clearTranslationState();
    // COW 写前分离：防止原地像素反色污染共享的纯净底图 m_cleanBaseImage
    if (!m_cleanBaseImage.empty() && m_cleanBaseImage.data == m_sourceImage.data) {
        m_sourceImage = m_sourceImage.clone();
    }
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
    stopZoomAnimation(false);
    clearTranslationState();
    if (!m_grayscale) {
        // COW 写前分离：防止原地灰度写回污染共享的纯净底图 m_cleanBaseImage
        if (!m_cleanBaseImage.empty() && m_cleanBaseImage.data == m_sourceImage.data) {
            m_sourceImage = m_sourceImage.clone();
        }
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
    stopZoomAnimation(false);
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
    stopZoomAnimation(true);
    releaseZoomStage();

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
    // 若用户主动按住 Shift+F3，允许显式强制复制一份新副本；常规按 F3 开启当前虚拟桌面防重复套娃去重
    const bool forceDuplicate = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    std::unordered_set<uint64_t> activeFps;
    if (!forceDuplicate) {
        for (const auto& inst : s_instances) {
            if (!inst || !inst->m_hwnd || !IsWindow(inst->m_hwnd)) continue;
            if (!tools3000::core::WinUtils::isWindowVisibleOnCurrentDesktop(inst->m_hwnd)) continue;
            if (inst->m_contentFingerprint != 0ULL) {
                activeFps.insert(inst->m_contentFingerprint);
            }
            if (!inst->m_cleanBaseImage.empty()) {
                const uint64_t baseFp = PinPasteCoordinator::computeContentFingerprint(
                    inst->m_cleanBaseImage, inst->m_pinnedSourceText);
                if (baseFp != 0ULL) {
                    activeFps.insert(baseFp);
                }
            }
        }
    }

    auto decision = PinPasteCoordinator::instance().stepNext(
        nullptr,
        forceDuplicate ? nullptr : &activeFps);
    if (!decision.has_value()) return nullptr;
    const auto& d = *decision;

    if (d.alreadyVisibleOnDesktop && !forceDuplicate) {
        std::shared_ptr<PinWindow> existingPin;
        for (auto it = s_instances.rbegin(); it != s_instances.rend(); ++it) {
            const auto& inst = *it;
            if (!inst || !inst->m_hwnd || !IsWindow(inst->m_hwnd)) continue;
            if (!tools3000::core::WinUtils::isWindowVisibleOnCurrentDesktop(inst->m_hwnd)) continue;
            const uint64_t baseFp = inst->m_cleanBaseImage.empty()
                ? 0ULL
                : PinPasteCoordinator::computeContentFingerprint(inst->m_cleanBaseImage, inst->m_pinnedSourceText);
            if (inst->m_contentFingerprint == d.candidate.contentFingerprint ||
                (baseFp != 0ULL && baseFp == d.candidate.contentFingerprint)) {
                existingPin = inst;
                break;
            }
        }
        if (existingPin) {
            if (existingPin->m_folded) {
                existingPin->toggleFold();
            }
            SetWindowPos(existingPin->m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            existingPin->focusWindow();
            existingPin->render();
        }
        const bool isZh = tools3000::core::isUiLanguageChinese();
        std::wstring toastMsg;
        if (d.totalCount <= 1) {
            toastMsg = tools3000::core::trWCstrFor(isZh, L"该贴图已在桌面显示（暂无更多历史截图）", L"Already pinned on desktop (no more history)");
        } else {
            const std::wstring cntStr = std::to_wstring(d.totalCount);
            toastMsg = tools3000::core::trFmtW(L"全部 {0} 张历史贴图均已在桌面显示", L"All {0} history pins are already on desktop", {cntStr});
        }
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{toastMsg});
        return existingPin;
    }

    auto pin = create(d.candidate.image, d.spawnPos.x, d.spawnPos.y,
                      d.candidate.cardCornerRadius, d.candidate.sourceText);

    if (d.totalCount > 1 && d.targetIndex > 0) {
        const std::wstring idxStr = std::to_wstring(d.targetIndex + 1);
        const std::wstring totStr = std::to_wstring(d.totalCount);
        std::wstring toastMsg = tools3000::core::trFmtW(L"历史贴图 {0}/{1}", L"Pin history {0}/{1}", {idxStr, totStr});
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
        if (!RegisterClassExW(&wc) && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            UnregisterClassW(PIN_CLASS, hInstance);
            RegisterClassExW(&wc);
        }
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

}  // namespace tools3000::capture
