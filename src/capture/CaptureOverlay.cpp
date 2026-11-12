#include "capture/CaptureOverlay.h"
#include "capture/CaptureBackend.h"
#include "capture/CursorOverlay.h"
#include "capture/MarkupBaseHelper.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "core/events/EventBus.h"
#include "capture/CaptureHistory.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/ScrollCapture.h"
#include "capture/ScrollCaptureEditorOverlay.h"
#include "capture/ScreenRecorder.h"
#include "capture/CaptureToolbarAccessibility.h"
#include "core/stats/PerformanceMonitor.h"
#include "core/utils/DpiUtils.h"
#include "core/accessibility/OverlayAnnouncement.h"
#include "core/accessibility/OverlayUiaProvider.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/utils/ThemeUtils.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>

namespace {

float dpiScaleAt(POINT screenPoint) {
    return tools3000::core::dpi::scaleAtPoint(screenPoint);
}

double elapsedMilliseconds(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
}

constexpr UINT_PTR kTimerLongCanvasAutoScrollId = 0x5CE1;

}  // namespace

namespace tools3000::capture {

namespace {
constexpr UINT WM_ACCESSIBILITY_INVOKE_TOOLBAR = WM_APP + 74;

std::vector<tools3000::core::accessibility::OverlayUiaAction>
toolbarUiaActions(HWND hwnd, const CaptureState& state) {
    RECT window{};
    GetWindowRect(hwnd, &window);
    const bool zh = tools3000::core::isUiLanguageChinese();
    std::vector<tools3000::core::accessibility::OverlayUiaAction> actions;
    actions.reserve(state.toolbarButtons.size());
    for (std::size_t index = 0; index < state.toolbarButtons.size(); ++index) {
        const auto& button = state.toolbarButtons[index];
        const auto name = toolbarButtonAccessibleName(button, zh);
        const auto shortcut = toolbarButtonKeyboardShortcut(button);
        actions.push_back({
            L"Tools3000.CaptureToolbar." + std::to_wstring(index), name,
            (zh ? L"截图工具栏：" : L"Capture Toolbar: ") + name, shortcut, {
                window.left + static_cast<LONG>(button.rect.left),
                window.top + static_cast<LONG>(button.rect.top),
                window.left + static_cast<LONG>(button.rect.right),
                window.top + static_cast<LONG>(button.rect.bottom)},
            isToolbarButtonEnabled(button, state), isToolbarButtonSelected(button, state),
            tools3000::core::accessibility::OverlayUiaActionRole::Button,
            WM_ACCESSIBILITY_INVOKE_TOOLBAR, static_cast<WPARAM>(index)});
    }
    return actions;
}
}  // namespace

CaptureOverlay& CaptureOverlay::instance() {
    static CaptureOverlay inst;
    return inst;
}

bool CaptureOverlay::initialize(HINSTANCE hInstance) {
    m_hInstance = hInstance;
    ShortcutHintOverlay::instance().setOwnerActiveChecker([](ShortcutHintContext context) -> bool {
        switch (context) {
            case ShortcutHintContext::CaptureSelecting:
            case ShortcutHintContext::CaptureDragging:
            case ShortcutHintContext::CaptureSelected:
            case ShortcutHintContext::CaptureMarking:
            case ShortcutHintContext::CaptureMarkingNumber:
            case ShortcutHintContext::CaptureEditingText:
            case ShortcutHintContext::RecordSelecting:
                return CaptureOverlay::instance().state() != OverlayState::Idle &&
                       CaptureOverlay::instance().isOverlayVisible();
            case ShortcutHintContext::ScrollCapture:
                return ScrollCapture::instance().isRunning();
            case ShortcutHintContext::Recording:
            case ShortcutHintContext::RecordingPaused:
                return ScreenRecorder::instance().state() != RecordState::Idle;
            default:
                return false;
        }
    });
    ScrollCaptureEditorOverlay::instance().setBackendCallbacks(
        [](const cv::Mat& img, int frames, ScrollCaptureEditorOverlay::FinishCallback cb) {
            return CaptureOverlay::instance().startEditLongImage(img, frames, std::move(cb));
        },
        []() {
            auto& ov = CaptureOverlay::instance();
            if (ov.isLongImageEditing()) ov.cancel();
        },
        []() {
            const auto& ov = CaptureOverlay::instance();
            return ov.isLongImageEditing() && ov.isOverlayVisible();
        }
    );
    // 覆盖层是整个虚拟桌面大小。启动时创建一个隐藏的 layered D2D 窗口，
    // 在部分显卡/驱动上即使不可见也会持续触发表面合成，占满一个 CPU 核。
    // 这里只保存模块句柄，真正截图时再创建，用完立即释放。
    return m_hInstance != nullptr;
}

void CaptureOverlay::shutdown() {
    ScrollCaptureEditorOverlay::instance().setBackendCallbacks(nullptr, nullptr, nullptr);
    ShortcutHintOverlay::instance().setOwnerActiveChecker(nullptr);
    ShortcutHintOverlay::instance().hide();
    const bool wasActive = m_state.state.load() != OverlayState::Idle;
    m_state.state = OverlayState::Idle;
    m_state.longCanvas.reset();
    m_editPinnedCallback = nullptr;
    m_editPinnedMarkupCallback = nullptr;
    m_editPinnedOriginalCleanBase.release();
    m_editPinnedInitialRect = {};
    m_editLongImageCallback = nullptr;
    if (wasActive || m_hwnd) {
        tools3000::core::KeyboardHook::instance().setCaptureOverlayVisible(false);
    }
    ReleaseCapture();
    if (m_hwnd) {
        m_renderer.releaseWindowResources();
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    releaseFrozenSurface();
    m_renderer.shutdown();
    if (wasActive && m_closedCallback) m_closedCallback();
}

bool CaptureOverlay::createOverlayWindow(HINSTANCE hInstance, const RECT* customBounds) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = staticWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"Tools3000_CaptureOverlay";
    RegisterClassExW(&wc);

    const int initX = customBounds ? customBounds->left : 0;
    const int initY = customBounds ? customBounds->top : 0;
    const int initW = customBounds
        ? std::max<int>(320, customBounds->right - customBounds->left)
        : GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int initH = customBounds
        ? std::max<int>(240, customBounds->bottom - customBounds->top)
        : GetSystemMetrics(SM_CYVIRTUALSCREEN);

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        wc.lpszClassName, nullptr,
        WS_POPUP,
        initX, initY, initW, initH,
        nullptr, nullptr, hInstance, this
    );

    if (!m_hwnd) {
        LOG_ERROR("截图覆盖层窗口创建失败");
        return false;
    }

    // 显式注入 NonRudeHWND 属性，防止全屏截图覆盖层调用 SetForegroundWindow 时被 explorer.exe (CRudeAppManager)
    // 误判为全屏独占应用而将系统任务栏 (Shell_TrayWnd) 压入 Z 序底层
    SetPropW(m_hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));

    // 默认全透明，事件穿透（直到开始截图）
    SetLayeredWindowAttributes(m_hwnd, 0, 0, LWA_ALPHA);

    if (!m_renderer.initialize(m_hwnd, m_state)) {
        LOG_ERROR("截图覆盖层 Direct2D 初始化失败");
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        return false;
    }

    m_input.initialize(m_hwnd, m_state, m_renderer,
        [this](){ this->realCancel(); },
        [this](CaptureCompletion completion){ this->confirmSelection(std::move(completion)); }
    );

    return true;
}

void CaptureOverlay::resetTransientOverlayState() {
    m_state.dragging = false;
    m_state.isMarking = false;
    m_state.markupBaseReady = false;
    m_state.markupBaseRect = {0, 0, 0, 0};
    m_state.activeElement = nullptr;
    m_state.dragHandle = HitArea::None;
    m_state.isManipulating = false;
    m_state.isAdjustingSelection = false;
    m_state.isAdjustingCornerRadius = false;
    m_state.detectedWindow = {0, 0, 0, 0};
    m_state.detectedWindowCornerRadius = 0.0f;
    m_state.detectedWindowHierarchy.clear();
    m_state.detectedWindowHierarchyDescriptions.clear();
    m_state.detectedWindowHierarchyIndex = 0;
    m_state.animDetectedWindowRect = {0, 0, 0, 0};
    m_state.targetDetectedWindowRect = {0, 0, 0, 0};
    m_state.animVelocity = {0, 0, 0, 0};
    m_state.isHierarchyAnimating = false;
    m_state.hierarchyBadgeFadeOutUntil = 0;
    m_state.hierarchyBadgeText.clear();
    m_state.toolbarButtons.clear();
    m_state.secondaryToolbarButtons.clear();
    m_state.selectionSideButtons.clear();
    m_state.submenuButtons.clear();
    m_state.primaryToolbarRect = {0, 0, 0, 0};
    m_state.secondaryToolbarRect = {0, 0, 0, 0};
    m_state.selectionSideRect = {0, 0, 0, 0};
    m_state.toolbarGripRect = {0, 0, 0, 0};
    m_state.isToolbarGripHovered = false;
    m_state.hasCustomToolbarPos = false;
    m_state.isDraggingToolbar = false;
    m_state.sizeHudRect = {0, 0, 0, 0};
    m_state.lastSizeHudEdge = -1;
    m_state.isSizeMenuOpen = false;
    m_state.openSubmenu = SubmenuType::None;
    m_state.sliderPopup.type = SliderPopupType::None;
    m_state.sliderPopup.isDragging = false;
    m_state.dropdownMenu.type = DropdownType::None;
    m_state.toolbarLayoutValid = false;
    m_state.toolbarLayoutLongImage = false;
    m_state.detectedQrText.clear();
    m_state.qrChipRect = {0, 0, 0, 0};
    m_state.textEditEnteredTick = 0;
    m_state.markup.resetSession();
    m_state.loupeToastUntil = 0;
    m_state.currentTool = MarkupTool::Rectangle;
    m_state.isMarkupToolActive = false;
    m_state.historyMode = false;
    m_state.shiftTapCandidate = false;
    m_state.isFadingOut = false;
    m_state.fadeOutStart = 0;
    m_state.longCanvas.reset();
}

void CaptureOverlay::loadUserMarkupPreferences(bool preserveSavedOutputFrame) {
    auto& config = tools3000::core::ConfigManager::instance();
    const int savedNumFmt = std::clamp(
        config.get<int>("/capture/markup/numberFormat", 0), 0, static_cast<int>(NumberFormat::COUNT) - 1);
    m_state.currentNumberFormat = static_cast<NumberFormat>(savedNumFmt);
    const int savedSpotDim = std::clamp(config.get<int>("/capture/markup/spotlightDim", 55), 15, 85);
    m_state.currentSpotlightDimAlpha = static_cast<float>(savedSpotDim) / 100.0f;
    m_state.currentSpotlightEllipse = config.get<bool>("/capture/markup/spotlightEllipse", false);
    m_state.currentSpotlightBorder = config.get<bool>("/capture/markup/spotlightBorder", false);

    const std::string shellTheme = config.get<std::string>("/capture/beautyShellTheme", "studio_slate");
    if (shellTheme == "studio_slate" || shellTheme == "aurora_purple") m_state.beautyShell.bgType = BeautyBackgroundType::StudioSlate;
    else if (shellTheme == "paper_chalk" || shellTheme == "sunrise_coral") m_state.beautyShell.bgType = BeautyBackgroundType::PaperChalk;
    else if (shellTheme == "silk_mist" || shellTheme == "emerald_sea") m_state.beautyShell.bgType = BeautyBackgroundType::SilkMist;
    else if (shellTheme == "midnight_graphite" || shellTheme == "obsidian_dark") m_state.beautyShell.bgType = BeautyBackgroundType::MidnightGraphite;
    else if (shellTheme == "pure_minimal" || shellTheme == "minimal_light") m_state.beautyShell.bgType = BeautyBackgroundType::PureMinimal;
    m_state.beautyShell.padding = config.get<int>("/capture/beautyShellPadding", 32);
    m_state.beautyShell.cornerRadius = static_cast<float>(config.get<int>("/capture/beautyShellRadius", 16));

    if (preserveSavedOutputFrame) {
        m_state.beautyShell.enabled = config.get<bool>("/capture/beautyShellEnabled", false);
        m_state.cornerRadius = static_cast<float>(config.get<double>("/screenshot/cornerRadius", 0.0));
    } else {
        m_state.beautyShell.enabled = false;
        m_state.cornerRadius = 0.0f;
    }

    m_state.magnifierCalloutMode = config.get<bool>("/capture/markup/magnifier/calloutMode", true);
    const int savedCalloutStyle = std::clamp(
        config.get<int>("/capture/markup/magnifier/calloutStyle",
                        static_cast<int>(MagnifierCalloutStyle::DashedDirect)),
        0,
        static_cast<int>(MagnifierCalloutStyle::COUNT) - 1);
    m_state.currentMagnifierCalloutStyle = static_cast<MagnifierCalloutStyle>(savedCalloutStyle);
    m_state.currentTextOutline = config.get<bool>("/capture/markup/text/outline", true);
    m_state.currentTextOutlineColor = MarkupColor::Auto();

    const std::string savedCustomColor = config.get<std::string>("/capture/customColor", "");
    if (!savedCustomColor.empty() && savedCustomColor.size() >= 7 && savedCustomColor[0] == '#') {
        unsigned int r = 0, g = 0, b = 0;
        if (sscanf_s(savedCustomColor.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            m_state.customColor = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), 255};
            m_state.hasCustomColor = true;
        }
    }

    const std::string savedSizeUnit = config.get<std::string>("/capture/sizeUnit", "px");
    m_state.sizeUnit = (savedSizeUnit == "dp")
        ? CaptureState::SizeUnit::DeviceIndependentPixel
        : CaptureState::SizeUnit::Pixel;
    m_state.showPositionInHud = config.get<bool>("/capture/showSizePosition", true);
    m_state.showUnitInHud = config.get<bool>("/capture/showSizeUnit", true);
}

void CaptureOverlay::startSelection(const CaptureOptions& options, OverlayMode mode) {
    tools3000::core::TraceId::Scope scope;
    const auto totalStarted = std::chrono::steady_clock::now();

    // 防御性安全重置：若上次截图/录屏覆盖层未正常清理，先执行全量复位
    if (m_hwnd || m_state.state.load() != OverlayState::Idle) {
        realCancel();
    }

    // The virtual-desktop overlay is physically pixel-sized. Scale its HUD from
    // the monitor under the pointer before DWrite resources are created, so the
    // very first frame is already correct at 125%/150%/200%.
    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    m_state.options = options;
    m_state.mode = mode;
    m_state.dpiScale = dpiScaleAt(cursorScreen);
    m_state.currentCursor = {
        cursorScreen.x - GetSystemMetrics(SM_XVIRTUALSCREEN),
        cursorScreen.y - GetSystemMetrics(SM_YVIRTUALSCREEN)};

    const auto windowStarted = std::chrono::steady_clock::now();
    if (!m_hwnd && (!m_hInstance || !createOverlayWindow(m_hInstance))) {
        LOG_ERROR("无法启动截图: 覆盖层窗口初始化失败");
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::isUiLanguageChinese()
                ? L"截图界面初始化失败，请重试"
                : L"Failed to initialize capture overlay, please retry"});
        if (m_closedCallback) m_closedCallback();
        return;
    }
    tools3000::core::PerformanceMonitor::instance().recordLatency(
        "screenshot.window", elapsedMilliseconds(windowStarted));

    m_editPinnedCallback = nullptr;
    m_editLongImageCallback = nullptr;
    resetTransientOverlayState();
    m_state.state = OverlayState::Selecting;
    m_state.dragStart = {0, 0};
    m_state.dragEnd = {0, 0};
    m_state.lastMousePos = m_state.currentCursor;

    loadUserMarkupPreferences(true);

    auto& config = tools3000::core::ConfigManager::instance();
    std::string recFmt = config.get<std::string>("/recording/format", "mp4_h264");
    std::string savedVideoFmt = config.get<std::string>("/recording/videoFormat", "");
    if (recFmt != "gif") {
        if (savedVideoFmt != recFmt) {
            savedVideoFmt = recFmt;
            config.set<std::string>("/recording/videoFormat", savedVideoFmt);
        }
    } else if (savedVideoFmt.empty() || savedVideoFmt == "gif") {
        savedVideoFmt = "mp4_h264";
    }
    auto parseVideoFmt = [](const std::string& f) {
        if (f == "mp4_h265") return RecordFormat::MP4_H265;
        if (f == "webm_vp9") return RecordFormat::WebM_VP9;
        return RecordFormat::MP4_H264;
    };
    m_state.recordSetup.preferredVideoFormat = parseVideoFmt(savedVideoFmt);
    m_state.recordSetup.format = (recFmt == "gif")
        ? RecordFormat::GIF
        : m_state.recordSetup.preferredVideoFormat;
    m_state.recordSetup.fps = std::clamp(config.get<int>("/recording/fps", 30), 1, 120);
    const int bitrate = std::clamp(config.get<int>("/recording/bitrate", 8), 1, 100);
    m_state.recordSetup.bitrateMbps = bitrate;
    m_state.recordSetup.qualityLevel = recordBitrateToQualityLevel(bitrate);
    bool defaultClickEff = config.get<bool>("/spotlight/clickRippleEnabled", false);
    m_state.recordSetup.showClickEffects = config.get<bool>("/recording/showClickEffects", defaultClickEff);
    m_state.recordSetup.showClickZoom = config.get<bool>("/recording/showClickZoom", true);
    m_state.recordSetup.includeKeycast = config.get<bool>("/recording/includeKeycast", true);
    m_state.recordSetup.captureSystemAudio = config.get<bool>("/recording/captureSystemAudio", false);
    m_state.recordSetup.captureMicrophone = config.get<bool>("/recording/captureMicrophone", false);

    const auto freezeStarted = std::chrono::steady_clock::now();
    if (!freezeScreen()) {
        LOG_ERROR("无法启动截图: 桌面底图捕获或上传失败");
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::isUiLanguageChinese()
                ? L"无法捕获屏幕，请重试"
                : L"Failed to capture screen, please retry"});
        realCancel();
        return;
    }
    tools3000::core::PerformanceMonitor::instance().recordLatency(
        "screenshot.freeze", elapsedMilliseconds(freezeStarted));

    const auto uploadStarted = std::chrono::steady_clock::now();
    if (!m_renderer.updateScreenBitmap(m_state.frozenScreen)) {
        LOG_ERROR("无法启动截图: 桌面底图上传失败");
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::isUiLanguageChinese()
                ? L"无法显示屏幕截图，请重试"
                : L"Failed to render screen capture, please retry"});
        realCancel();
        return;
    }
    tools3000::core::PerformanceMonitor::instance().recordLatency(
        "screenshot.upload", elapsedMilliseconds(uploadStarted));

    m_renderer.invalidate();

    SetWindowPos(m_hwnd, HWND_TOPMOST,
                 GetSystemMetrics(SM_XVIRTUALSCREEN),
                 GetSystemMetrics(SM_YVIRTUALSCREEN),
                 GetSystemMetrics(SM_CXVIRTUALSCREEN),
                 GetSystemMetrics(SM_CYVIRTUALSCREEN),
                 SWP_SHOWWINDOW);
    tools3000::core::KeyboardHook::instance().setCaptureOverlayVisible(true);

    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
    if (options.showShortcutHints) {
        const LONG pad = static_cast<LONG>(std::lround(52.0f * (m_state.dpiScale > 0.0f ? m_state.dpiScale : 1.0f)));
        std::vector<RECT> initialAvoid = {
            RECT{cursorScreen.x - pad, cursorScreen.y - pad, cursorScreen.x + pad, cursorScreen.y + pad}
        };
        ShortcutHintOverlay::instance().show(
            mode == OverlayMode::RecordRegion
                ? ShortcutHintContext::RecordSelecting
                : ShortcutHintContext::CaptureSelecting,
            cursorScreen,
            initialAvoid);
    } else {
        ShortcutHintOverlay::instance().hide();
    }
    RedrawWindow(m_hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    SetCapture(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    CaptureInput::ensureCursorVisible();
    m_input.updateHoverCursor(m_state.currentCursor);
    tools3000::core::accessibility::announceOverlay(
        m_hwnd,
        tools3000::core::isUiLanguageChinese()
            ? L"截图选区。拖动鼠标选择区域；Enter 确认；Esc 取消。"
            : L"Capture selection. Drag to select an area; Enter to confirm; Esc to cancel."
    );
    tools3000::core::PerformanceMonitor::instance().recordLatency(
        "screenshot", elapsedMilliseconds(totalStarted));
}

void CaptureOverlay::startEditPinned(const cv::Mat& image, const CaptureRegion& region,
                                     std::function<void(const cv::Mat& newImage)> onFinished) {
    if (image.empty() || region.width <= 0 || region.height <= 0) return;

    if (m_hwnd || m_state.state.load() != OverlayState::Idle) {
        realCancel();
    }

    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    m_state.options = {};
    m_state.options.showShortcutHints = tools3000::core::ConfigManager::instance().get<bool>(
        "/capture/showShortcutHints", true);
    m_state.mode = OverlayMode::Screenshot;
    m_state.dpiScale = dpiScaleAt(cursorScreen);
    m_state.currentCursor = {
        cursorScreen.x - GetSystemMetrics(SM_XVIRTUALSCREEN),
        cursorScreen.y - GetSystemMetrics(SM_YVIRTUALSCREEN)};

    if (!m_hwnd && (!m_hInstance || !createOverlayWindow(m_hInstance))) {
        LOG_ERROR("无法启动贴图编辑: 覆盖层窗口初始化失败");
        if (onFinished) onFinished(cv::Mat{});
        return;
    }

    m_editPinnedCallback = std::move(onFinished);
    m_editPinnedMarkupCallback = nullptr;
    m_editPinnedOriginalCleanBase.release();
    m_editPinnedInitialRect = {};
    m_editLongImageCallback = nullptr;
    resetTransientOverlayState();
    m_state.state = OverlayState::Selected;
    m_state.overlayShowTick = GetTickCount();
    loadUserMarkupPreferences(false);

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    m_state.dragStart = { region.x - vx, region.y - vy };
    m_state.dragEnd = { m_state.dragStart.x + region.width, m_state.dragStart.y + region.height };

    if (!freezeScreen()) {
        LOG_ERROR("无法捕获屏幕底图用于贴图编辑");
        realCancel();
        return;
    }

    // 将贴图原图覆盖至对应选区区域（兼容缩放后的贴图尺寸，并确保 3 通道与 4 通道类型严格对齐）
    cv::Rect roi(m_state.dragStart.x, m_state.dragStart.y, region.width, region.height);
    roi &= cv::Rect(0, 0, m_state.frozenScreen.cols, m_state.frozenScreen.rows);
    if (roi.width > 0 && roi.height > 0 && !image.empty() && !m_state.frozenScreen.empty()) {
        try {
            cv::Mat targetImage = image;
            if (targetImage.cols != roi.width || targetImage.rows != roi.height) {
                cv::resize(targetImage, targetImage, roi.size(), 0, 0, cv::INTER_LINEAR);
            }
            const int targetChannels = m_state.frozenScreen.channels();
            if (targetChannels == 4 && targetImage.channels() == 3) {
                cv::cvtColor(targetImage, targetImage, cv::COLOR_BGR2BGRA);
            } else if (targetChannels == 3 && targetImage.channels() == 4) {
                cv::cvtColor(targetImage, targetImage, cv::COLOR_BGRA2BGR);
            } else if (targetChannels == 1 && targetImage.channels() != 1) {
                cv::cvtColor(targetImage, targetImage, cv::COLOR_BGR2GRAY);
            }
            if (targetImage.type() == m_state.frozenScreen.type()) {
                targetImage.copyTo(m_state.frozenScreen(roi));
            } else {
                targetImage.convertTo(targetImage, m_state.frozenScreen.type());
                targetImage.copyTo(m_state.frozenScreen(roi));
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("贴图编辑底图复制异常: {}", ex.what());
        } catch (...) {
            LOG_ERROR("贴图编辑底图复制发生未知异常");
        }
    }

    if (!m_renderer.updateScreenBitmap(m_state.frozenScreen)) {
        LOG_ERROR("贴图编辑底图上传失败");
        realCancel();
        return;
    }

    m_renderer.invalidate();

    SetWindowPos(m_hwnd, HWND_TOPMOST,
                 GetSystemMetrics(SM_XVIRTUALSCREEN),
                 GetSystemMetrics(SM_YVIRTUALSCREEN),
                 GetSystemMetrics(SM_CXVIRTUALSCREEN),
                 GetSystemMetrics(SM_CYVIRTUALSCREEN),
                 SWP_SHOWWINDOW);
    tools3000::core::KeyboardHook::instance().setCaptureOverlayVisible(true);

    if (m_state.options.showShortcutHints) {
        ShortcutHintOverlay::instance().show(ShortcutHintContext::CaptureSelected, cursorScreen, {});
    } else {
        ShortcutHintOverlay::instance().hide();
    }

    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
    RedrawWindow(m_hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    SetCapture(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    CaptureInput::ensureCursorVisible();
    m_input.updateHoverCursor(m_state.currentCursor);
    tools3000::core::accessibility::announceOverlay(
        m_hwnd,
        tools3000::core::isUiLanguageChinese()
            ? L"截图标注。使用工具栏完成标注；Enter 确认；Esc 取消。"
            : L"Capture annotation. Use the toolbar to annotate; Enter to confirm; Esc to cancel."
    );
}

void CaptureOverlay::startEditPinnedMarkup(
    const cv::Mat& cleanBaseImage,
    const std::vector<MarkupElement>& existingElements,
    const CaptureRegion& pinScreenRect,
    EditPinnedMarkupCallback onFinished) {
    tools3000::core::TraceId::Scope scope;
    if (cleanBaseImage.empty() || cleanBaseImage.cols <= 0 || cleanBaseImage.rows <= 0) {
        if (onFinished) onFinished(false, {}, {}, {});
        return;
    }

    if (m_hwnd || m_state.state.load() != OverlayState::Idle) {
        realCancel();
    }

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int imgW = cleanBaseImage.cols;
    const int imgH = cleanBaseImage.rows;

    // 超出虚拟桌面尺寸的贴图（如 0.35x 缩小的超长截图）自动切入 1:1 长画布精修工作台，彻底避免下采样糊化
    if (vw > 0 && vh > 0 && (imgW > vw || imgH > vh)) {
        if (!startEditLongImage(cleanBaseImage, 1, nullptr)) {
            if (onFinished) onFinished(false, {}, {}, {});
            return;
        }
        m_editLongImageCallback = nullptr;
        m_editPinnedMarkupCallback = std::move(onFinished);
        m_editPinnedOriginalCleanBase = cleanBaseImage.clone();
        m_editPinnedInitialRect = cv::Rect(0, 0, imgW, imgH);
        if (!existingElements.empty()) {
            m_state.markup.importElements(existingElements);
        }
        m_renderer.markMarkupDirty();
        m_renderer.invalidate();
        return;
    }

    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    m_state.options = {};
    m_state.options.autoDetectWindow = false;
    m_state.options.showShortcutHints = tools3000::core::ConfigManager::instance().get<bool>(
        "/capture/showShortcutHints", true);
    m_state.mode = OverlayMode::Screenshot;
    m_state.dpiScale = dpiScaleAt(cursorScreen);
    m_state.currentCursor = {cursorScreen.x - vx, cursorScreen.y - vy};

    if (!m_hwnd && (!m_hInstance || !createOverlayWindow(m_hInstance))) {
        LOG_ERROR("Failed to initialize overlay window for pinned markup editing");
        if (onFinished) onFinished(false, {}, {}, {});
        return;
    }

    m_editPinnedCallback = nullptr;
    m_editPinnedMarkupCallback = std::move(onFinished);
    m_editPinnedOriginalCleanBase = cleanBaseImage.clone();
    m_editLongImageCallback = nullptr;
    resetTransientOverlayState();
    m_state.state = OverlayState::Selected;
    m_state.overlayShowTick = GetTickCount();
    loadUserMarkupPreferences(false);

    // 以贴图中心为锚点在 1:1 原始物理像素空间展开编辑选区，严禁按 m_scale 对底图做任何有损缩放
    const int pinCenterX = pinScreenRect.x + pinScreenRect.width / 2;
    const int pinCenterY = pinScreenRect.y + pinScreenRect.height / 2;
    POINT anchorPt{pinCenterX, pinCenterY};
    HMONITOR hMon = MonitorFromPoint(anchorPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    int screenLeft = pinCenterX - imgW / 2;
    int screenTop = pinCenterY - imgH / 2;
    if (hMon && GetMonitorInfoW(hMon, &mi)) {
        const int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        const int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
        if (imgW <= monW && imgH <= monH) {
            screenLeft = std::clamp(screenLeft, static_cast<int>(mi.rcMonitor.left), static_cast<int>(mi.rcMonitor.right - imgW));
            screenTop = std::clamp(screenTop, static_cast<int>(mi.rcMonitor.top), static_cast<int>(mi.rcMonitor.bottom - imgH));
        }
    }
    screenLeft = std::clamp(screenLeft, vx, vx + (std::max)(0, vw - imgW));
    screenTop = std::clamp(screenTop, vy, vy + (std::max)(0, vh - imgH));

    m_state.dragStart = {screenLeft - vx, screenTop - vy};
    m_state.dragEnd = {m_state.dragStart.x + imgW, m_state.dragStart.y + imgH};

    if (!freezeScreen()) {
        LOG_ERROR("Failed to freeze screen for pinned markup editing");
        realCancel();
        return;
    }

    const cv::Rect roi(m_state.dragStart.x, m_state.dragStart.y, imgW, imgH);
    const cv::Rect clippedRoi = roi & cv::Rect(0, 0, m_state.frozenScreen.cols, m_state.frozenScreen.rows);
    m_editPinnedInitialRect = clippedRoi;
    if (clippedRoi.width == imgW && clippedRoi.height == imgH && !m_state.frozenScreen.empty()) {
        try {
            cv::Mat targetImage = cleanBaseImage;
            const int targetChannels = m_state.frozenScreen.channels();
            if (targetChannels == 4 && targetImage.channels() == 3) {
                cv::cvtColor(targetImage, targetImage, cv::COLOR_BGR2BGRA);
            } else if (targetChannels == 3 && targetImage.channels() == 4) {
                cv::cvtColor(targetImage, targetImage, cv::COLOR_BGRA2BGR);
            } else if (targetChannels == 1 && targetImage.channels() != 1) {
                cv::cvtColor(targetImage, targetImage, cv::COLOR_BGR2GRAY);
            }
            if (targetImage.type() == m_state.frozenScreen.type()) {
                targetImage.copyTo(m_state.frozenScreen(clippedRoi));
            } else {
                targetImage.convertTo(targetImage, m_state.frozenScreen.type());
                targetImage.copyTo(m_state.frozenScreen(clippedRoi));
            }
        } catch (const std::exception& ex) {
            LOG_ERROR("Pinned markup clean base copy failed: {}", ex.what());
        }
    }

    syncMarkupBase(
        m_state.frozenScreen,
        clippedRoi,
        m_state.markupBaseReady,
        m_state.markupBaseRect,
        m_state.markup);
    if (!existingElements.empty()) {
        m_state.markup.importElements(existingElements);
    }
    m_renderer.markMarkupDirty();

    if (!m_renderer.updateScreenBitmap(m_state.frozenScreen)) {
        LOG_ERROR("Failed to upload frozen screen bitmap for pinned markup editing");
        realCancel();
        return;
    }

    m_renderer.invalidate();

    SetWindowPos(m_hwnd, HWND_TOPMOST, vx, vy, vw, vh, SWP_SHOWWINDOW);
    tools3000::core::KeyboardHook::instance().setCaptureOverlayVisible(true);

    if (m_state.options.showShortcutHints) {
        ShortcutHintOverlay::instance().show(ShortcutHintContext::CaptureSelected, cursorScreen, {});
    } else {
        ShortcutHintOverlay::instance().hide();
    }

    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
    RedrawWindow(m_hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    SetCapture(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    CaptureInput::ensureCursorVisible();
    m_input.updateHoverCursor(m_state.currentCursor);
    tools3000::core::accessibility::announceOverlay(
        m_hwnd,
        tools3000::core::isUiLanguageChinese()
            ? L"截图标注。使用工具栏完成标注；Enter 确认；Esc 取消。"
            : L"Capture annotation. Use the toolbar to annotate; Enter to confirm; Esc to cancel."
    );
}

bool CaptureOverlay::startEditLongImage(
    const cv::Mat& stitchedImage,
    int frameCount,
    std::function<void(const cv::Mat& finalImage, ScrollFinishAction action, int frameCount)> onFinished) {
    tools3000::core::TraceId::Scope scope;
    if (stitchedImage.empty() || stitchedImage.cols <= 0 || stitchedImage.rows <= 0) {
        return false;
    }

    if (m_hwnd || m_state.state.load() != OverlayState::Idle) {
        realCancel();
    }

    releaseFrozenSurface();
    cv::Mat bgra;
    if (stitchedImage.channels() == 4) {
        bgra = stitchedImage.clone();
    } else if (stitchedImage.channels() == 3) {
        cv::cvtColor(stitchedImage, bgra, cv::COLOR_BGR2BGRA);
    } else if (stitchedImage.channels() == 1) {
        cv::cvtColor(stitchedImage, bgra, cv::COLOR_GRAY2BGRA);
    } else {
        bgra = stitchedImage.clone();
    }
    if (!bgra.isContinuous()) {
        bgra = bgra.clone();
    }
    if (bgra.empty()) {
        return false;
    }

    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    HMONITOR hMon = MonitorFromPoint(cursorScreen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT monRect{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (hMon && GetMonitorInfoW(hMon, &mi)) {
        monRect = mi.rcMonitor;
    }
    // FocusAssistAvoidance: 物理高度主动缩减 1px，避免被 Windows Shell 误判为全屏独占触发专注助手免打扰
    RECT winBounds{
        monRect.left,
        monRect.top,
        std::max(monRect.left + 320L, monRect.right),
        std::max(monRect.top + 240L, monRect.bottom - 1L)
    };

    m_state.options = {};
    m_state.options.autoDetectWindow = false;
    m_state.options.showShortcutHints = tools3000::core::ConfigManager::instance().get<bool>(
        "/capture/showShortcutHints", true);
    m_state.mode = OverlayMode::Screenshot;
    m_state.dpiScale = dpiScaleAt(cursorScreen);
    m_state.currentCursor = {
        cursorScreen.x - winBounds.left,
        cursorScreen.y - winBounds.top
    };

    HINSTANCE hInst = m_hInstance ? m_hInstance : GetModuleHandleW(nullptr);
    if (!m_hwnd && (!hInst || !createOverlayWindow(hInst, &winBounds))) {
        LOG_ERROR("无法启动长截图精修工作台: 覆盖层窗口初始化失败");
        releaseFrozenSurface();
        return false;
    }

    m_editPinnedCallback = nullptr;
    m_editPinnedMarkupCallback = nullptr;
    m_editPinnedOriginalCleanBase.release();
    m_editPinnedInitialRect = {};
    m_editLongImageCallback = std::move(onFinished);
    resetTransientOverlayState();
    loadUserMarkupPreferences(false);

    m_state.frozenScreen = std::move(bgra);
    m_state.state = OverlayState::Selected;
    m_state.overlayShowTick = GetTickCount();
    m_state.lastMousePos = m_state.currentCursor;
    m_state.dragStart = {0, 0};
    m_state.dragEnd = {m_state.frozenScreen.cols, m_state.frozenScreen.rows};

    m_state.longCanvas.active = true;
    m_state.longCanvas.frameCount = std::max(1, frameCount);
    m_state.longCanvas.docWidth = m_state.frozenScreen.cols;
    m_state.longCanvas.docHeight = m_state.frozenScreen.rows;
    m_state.longCanvas.zoom = 0.0f; // 首帧在 updateLongCanvasLayout 中自动计算最佳阅读适宽缩放
    m_state.longCanvas.fitZoom = 1.0f;
    m_state.longCanvas.scrollX = 0.0f;
    m_state.longCanvas.scrollY = 0.0f;

    syncMarkupBase(
        m_state.frozenScreen,
        cv::Rect(0, 0, m_state.frozenScreen.cols, m_state.frozenScreen.rows),
        m_state.markupBaseReady,
        m_state.markupBaseRect,
        m_state.markup);

    if (!m_renderer.updateScreenBitmap(m_state.frozenScreen)) {
        LOG_ERROR("长截图精修工作台底图上传失败");
        m_editLongImageCallback = nullptr;
        realCancel();
        return false;
    }

    m_renderer.invalidate();

    SetWindowPos(m_hwnd, HWND_TOPMOST,
                 winBounds.left,
                 winBounds.top,
                 winBounds.right - winBounds.left,
                 winBounds.bottom - winBounds.top,
                 SWP_SHOWWINDOW);
    tools3000::core::KeyboardHook::instance().setCaptureOverlayVisible(true);

    if (m_state.options.showShortcutHints) {
        ShortcutHintOverlay::instance().show(ShortcutHintContext::CaptureSelected, cursorScreen, {});
    } else {
        ShortcutHintOverlay::instance().hide();
    }

    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
    RedrawWindow(m_hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    SetCapture(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    CaptureInput::ensureCursorVisible();
    m_input.updateHoverCursor(m_state.currentCursor);
    tools3000::core::accessibility::announceOverlay(
        m_hwnd,
        tools3000::core::isUiLanguageChinese()
            ? L"长截图精修工作台。拖动上下标尺无损裁切首尾，使用底部工具栏添加文字、序号与全部标注；Enter 完成；Esc 取消。"
            : L"Long screenshot studio. Drag top and bottom handles to trim, use the toolbar to annotate; Enter to finish; Esc to cancel."
    );
    LOG_INFO("长截图精修工作台已打开: 尺寸={}x{}, 拼接帧数={}",
             m_state.frozenScreen.cols, m_state.frozenScreen.rows, m_state.longCanvas.frameCount);
    return true;
}

bool CaptureOverlay::freezeScreen() {
    releaseFrozenSurface();
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (w <= 0 || h <= 0) {
        LOG_ERROR("无效的虚拟屏幕尺寸: {}x{}", w, h);
        return false;
    }

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) {
        LOG_ERROR("GetDC(nullptr) 失败, error={}", GetLastError());
        return false;
    }
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        LOG_ERROR("CreateCompatibleDC 失败, error={}", GetLastError());
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = w;
    bitmapInfo.bmiHeader.biHeight = -h;  // 自顶向下 DIB
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        hdcScreen, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        LOG_ERROR("CreateDIBSection 失败, error={}", GetLastError());
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }

    HGDIOBJ previous = SelectObject(hdcMem, bitmap);
    BOOL copied = BitBlt(
        hdcMem, 0, 0, w, h, hdcScreen, x, y, SRCCOPY | CAPTUREBLT);
    if (!copied && (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_INVALID_HANDLE)) {
        // 第一重降级：在受限沙箱或无分层窗口权限环境尝试不带 CAPTUREBLT
        copied = BitBlt(hdcMem, 0, 0, w, h, hdcScreen, x, y, SRCCOPY);
    }
    if (copied) {
        // cv::Mat 直接引用 DIBSection 的像素；m_frozenBitmap 持有该存储，
        // 因此在覆盖层关闭前引用始终有效。这里仍包含 BitBlt 的屏幕复制。
        m_state.frozenScreen = cv::Mat(
            h, w, CV_8UC4, pixels, static_cast<size_t>(w) * 4);
        m_frozenBitmap = bitmap;
        bitmap = nullptr;
        LOG_DEBUG("截图覆盖层桌面底图捕获成功: {}x{} (坐标=[{}, {}])", w, h, x, y);
    } else if (w > 0 && h > 0 && pixels != nullptr) {
        // 第二重兜底：在无头 CI、受限沙箱会话或虚拟会话中，屏幕 DC 复制受系统限制。
        // 使用安全底色兜底，确保截图窗口生命周期与交互管线具备极致容灾弹性。
        LOG_WARN("截图覆盖层 BitBlt 受系统环境限制 (error={})，启用安全底图兜底", GetLastError());
        std::memset(pixels, 0x1E, static_cast<size_t>(w) * 4 * h);
        m_state.frozenScreen = cv::Mat(
            h, w, CV_8UC4, pixels, static_cast<size_t>(w) * 4);
        m_frozenBitmap = bitmap;
        bitmap = nullptr;
        copied = TRUE;
    } else {
        LOG_ERROR("截图覆盖层 BitBlt 失败, error={}", GetLastError());
        m_state.frozenScreen.release();
    }

    SelectObject(hdcMem, previous);
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    if (copied && !m_state.frozenScreen.empty() && m_state.options.includeCursor) {
        CaptureFrameView frameView;
        frameView.data = m_state.frozenScreen.data;
        frameView.width = m_state.frozenScreen.cols;
        frameView.height = m_state.frozenScreen.rows;
        frameView.stride = static_cast<int>(m_state.frozenScreen.step[0]);
        frameView.format = CapturePixelFormat::Bgra32;
        CursorOverlay cursorOverlay;
        cursorOverlay.applyPermanent(frameView, {x, y, w, h}, true);
    }

    return copied && !m_state.frozenScreen.empty();
}

void CaptureOverlay::releaseFrozenSurface() {
    m_state.frozenScreen.release();
    if (m_frozenBitmap) {
        DeleteObject(m_frozenBitmap);
        m_frozenBitmap = nullptr;
    }
}

void CaptureOverlay::cancel() {
    realCancel();
}

void CaptureOverlay::setShortcutHintsEnabled(bool enabled) {
    m_state.options.showShortcutHints = enabled;
    if (!enabled) {
        ShortcutHintOverlay::instance().hide();
        return;
    }
    const auto current = m_state.state.load();
    if (current == OverlayState::Idle || !isOverlayVisible()) return;
    ShortcutHintContext context = ShortcutHintContext::CaptureSelecting;
    if (m_state.mode == OverlayMode::RecordRegion) {
        context = ShortcutHintContext::RecordSelecting;
    } else if (current == OverlayState::Selected || current == OverlayState::Marking) {
        context = ShortcutHintContext::CaptureSelected;
    }
    ShortcutHintOverlay::instance().show(context);
    m_renderer.invalidate();
}

void CaptureOverlay::cleanupAfterDelivery() {
    tools3000::core::KeyboardHook::instance().setCaptureOverlayVisible(false);
    m_state.dragging = false;
    m_state.isMarking = false;
    m_state.isManipulating = false;
    m_state.isAdjustingSelection = false;
    m_state.isAdjustingCornerRadius = false;
    m_state.isHierarchyAnimating = false;
    m_state.shiftTapCandidate = false;
    m_state.isFadingOut = false;
    m_state.animDetectedWindowRect = {};
    m_state.targetDetectedWindowRect = {};
    m_state.animVelocity = {};
    m_state.longCanvas.reset();
    m_state.toolbarLayoutLongImage = false;
    if (m_hwnd) {
        KillTimer(m_hwnd, 1);
        KillTimer(m_hwnd, kTimerLongCanvasAutoScrollId);
        m_renderer.releaseWindowResources();
        HWND hwndToDestroy = m_hwnd;
        m_hwnd = nullptr;
        DestroyWindow(hwndToDestroy);
    }
    m_state.markup.resetSession();
    m_state.detectedWindowHierarchy.clear();
    m_state.detectedWindowHierarchyDescriptions.clear();
    m_state.detectedWindow = {};
    releaseFrozenSurface();
    if (m_closedCallback) m_closedCallback();

    // 冷路径退场：主动修剪物理内存，将大图/D2D/DirectX 缓存归还系统
    tools3000::core::WinUtils::trimWorkingSet();
}

void CaptureOverlay::realCancel() {
    tools3000::core::KeyboardHook::instance().setCaptureOverlayVisible(false);
    ShortcutHintOverlay::instance().hide();
    auto editPinnedCb = std::move(m_editPinnedCallback);
    m_editPinnedCallback = nullptr;
    auto editPinnedMarkupCb = std::move(m_editPinnedMarkupCallback);
    m_editPinnedMarkupCallback = nullptr;
    m_editPinnedOriginalCleanBase.release();
    m_editPinnedInitialRect = {};
    const int savedLongFrames = m_state.longCanvas.frameCount;
    auto editLongCb = std::move(m_editLongImageCallback);
    m_editLongImageCallback = nullptr;

    m_state.state = OverlayState::Idle;
    m_state.dragging = false;
    m_state.isMarking = false;
    m_state.isManipulating = false;
    m_state.isAdjustingSelection = false;
    m_state.isAdjustingCornerRadius = false;
    m_state.isFadingOut = false;
    ReleaseCapture();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));

    if (m_hwnd) {
        tools3000::core::accessibility::hideOverlay(m_hwnd);
        ShowWindow(m_hwnd, SW_HIDE);
    }
    cleanupAfterDelivery();
    if (editPinnedCb) {
        editPinnedCb(cv::Mat{});
    }
    if (editPinnedMarkupCb) {
        editPinnedMarkupCb(false, cv::Mat{}, cv::Mat{}, {});
    }
    if (editLongCb) {
        editLongCb(cv::Mat{}, ScrollFinishAction::Cancelled, savedLongFrames);
    }
    ShortcutHintOverlay::instance().hide();
}

void CaptureOverlay::confirmSelection(CaptureCompletion completion) {
    tools3000::core::TraceId::Scope scope;
    if (m_state.state.load() == OverlayState::Idle) return;

    if (m_state.activeElement) {
        m_state.activeElement->isActive = false;
        m_state.activeElement->isEditing = false;
        m_state.activeElement = nullptr;
    }

    int x1 = std::min(m_state.dragStart.x, m_state.dragEnd.x);
    int y1 = std::min(m_state.dragStart.y, m_state.dragEnd.y);
    int x2 = std::max(m_state.dragStart.x, m_state.dragEnd.x);
    int y2 = std::max(m_state.dragStart.y, m_state.dragEnd.y);
    int w = x2 - x1;
    int h = y2 - y1;

    if (w <= 0 || h <= 0) {
        realCancel();
        return;
    }

    cv::Mat cropped;
    cv::Rect targetRect(x1, y1, w, h);
    syncMarkupBase(m_state.frozenScreen, targetRect, m_state.markupBaseReady, m_state.markupBaseRect, m_state.markup);
    if (m_state.markup.elementCount() > 0) {
        cropped = m_state.markup.getCompositeImage();
    } else {
        cropped = cropMarkupBase(m_state.frozenScreen, targetRect);
    }

    cv::Mat cleanBaseDelivered;
    std::vector<MarkupElement> updatedElements;
    if (m_editPinnedMarkupCallback) {
        if (targetRect == m_editPinnedInitialRect && !m_editPinnedOriginalCleanBase.empty()) {
            cleanBaseDelivered = m_editPinnedOriginalCleanBase.clone();
        } else {
            cleanBaseDelivered = cropMarkupBase(m_state.frozenScreen, targetRect);
        }
        updatedElements = m_state.markup.exportElements();
    }

    const bool bakedDecorations =
        (m_state.beautyShell.enabled || m_state.cornerRadius > 0.5f) &&
        !cropped.empty() && m_state.mode != OverlayMode::RecordRegion;
    if (m_state.beautyShell.enabled && !cropped.empty() && m_state.mode != OverlayMode::RecordRegion) {
        cropped = applyBeautyShell(cropped, m_state.beautyShell);
    } else if (m_state.cornerRadius > 0.5f && !cropped.empty() && m_state.mode != OverlayMode::RecordRegion) {
        cropped = applyRoundedCorners(cropped, m_state.cornerRadius);
    }
    if (bakedDecorations && m_editPinnedMarkupCallback) {
        cleanBaseDelivered = cropped.clone();
        updatedElements.clear();
    }

    int offsetX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int offsetY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    RECT winRc{};
    if (m_hwnd && IsWindow(m_hwnd) && GetWindowRect(m_hwnd, &winRc)) {
        offsetX = winRc.left;
        offsetY = winRc.top;
    }
    CaptureRegion region{x1 + offsetX, y1 + offsetY, w, h, m_state.cornerRadius};

    auto editPinnedCb = std::move(m_editPinnedCallback);
    m_editPinnedCallback = nullptr;
    auto editPinnedMarkupCb = std::move(m_editPinnedMarkupCallback);
    m_editPinnedMarkupCallback = nullptr;
    m_editPinnedOriginalCleanBase.release();
    m_editPinnedInitialRect = {};
    const int savedLongFrames = m_state.longCanvas.frameCount;
    auto editLongCb = std::move(m_editLongImageCallback);
    m_editLongImageCallback = nullptr;
    auto cb = m_state.callback;
    auto rcb = m_state.recordCallback;
    auto mode = m_state.mode;

    // 阶段 1 (视觉退场): 立即同步隐藏覆盖层与提示窗，释放光标捕获，
    // 确保录屏首帧不被遮挡，桌面视觉即刻恢复纯净交互。
    tools3000::core::KeyboardHook::instance().setCaptureOverlayVisible(false);
    ShortcutHintOverlay::instance().hide();
    m_state.state = OverlayState::Idle;
    m_state.dragging = false;
    m_state.isMarking = false;
    m_state.isManipulating = false;
    m_state.isAdjustingSelection = false;
    m_state.isAdjustingCornerRadius = false;
    m_state.isFadingOut = false;
    ReleaseCapture();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));

    if (m_hwnd) {
        tools3000::core::accessibility::hideOverlay(m_hwnd);
        ShowWindow(m_hwnd, SW_HIDE);
    }

    // 阶段 2 (业务交付): 同步执行交付回调 (写入多矩阵剪贴板与保存磁盘)
    if (mode == OverlayMode::RecordRegion) {
        if (rcb) rcb(region, m_state.recordSetup);
    } else if (!editPinnedCb && !editPinnedMarkupCb && !editLongCb) {
        if (cb) cb(region, cropped, completion);
    }

    // 阶段 3 (冷路径清理与内存修剪): 业务数据交付彻底完成后，再安全销毁覆盖层窗口、释放底图并执行 trimWorkingSet()
    cleanupAfterDelivery();
    if (editPinnedCb && !cropped.empty()) {
        editPinnedCb(cropped);
    }
    if (editPinnedMarkupCb && !cropped.empty()) {
        editPinnedMarkupCb(true, cleanBaseDelivered, cropped, std::move(updatedElements));
    }
    if (editLongCb) {
        ScrollFinishAction finishAct = ScrollFinishAction::Confirm;
        switch (completion.action) {
            case CaptureCompletionAction::Copy:   finishAct = ScrollFinishAction::Copy; break;
            case CaptureCompletionAction::Pin:    finishAct = ScrollFinishAction::Pin; break;
            case CaptureCompletionAction::SaveAs: finishAct = ScrollFinishAction::SaveAs; break;
            case CaptureCompletionAction::Default:
            default:                              finishAct = ScrollFinishAction::Confirm; break;
        }
        editLongCb(cropped, finishAct, savedLongFrames);
    }
    if (mode != OverlayMode::RecordRegion) {
        ShortcutHintOverlay::instance().hide();
    }
}

LRESULT CALLBACK CaptureOverlay::staticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CaptureOverlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto createStruct = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<CaptureOverlay*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<CaptureOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        try {
            if (msg == WM_MOUSEACTIVATE) {
                SetForegroundWindow(hwnd);
                SetFocus(hwnd);
                return MA_ACTIVATE;
            }
            if (msg == WM_CLOSE) {
                self->realCancel();
                return 0;
            }
            if (msg == WM_ACCESSIBILITY_INVOKE_TOOLBAR) {
                self->m_input.invokeToolbarButton(static_cast<std::size_t>(wParam));
                return 0;
            }
            if (msg == WM_GETOBJECT) {
                return tools3000::core::accessibility::respondToOverlayUiaGetObject(
                    hwnd, wParam, lParam,
                    {L"Tools3000.CaptureOverlay",
                     L"Screenshot selection and annotation. Enter confirms. Escape cancels.",
                     tools3000::core::accessibility::OverlayUiaRole::Pane, true},
                     toolbarUiaActions(hwnd, self->m_state));
            }
            if (msg == WM_NCDESTROY) {
                if (self->m_hwnd == hwnd) {
                    self->m_hwnd = nullptr;
                }
                tools3000::core::KeyboardHook::instance().setCaptureOverlayVisible(false);
                if (self->m_state.state.load() != OverlayState::Idle) {
                    self->m_state.state = OverlayState::Idle;
                    ShortcutHintOverlay::instance().hide();
                }
                tools3000::core::accessibility::disconnectOverlayUiaProvider(hwnd);
            }
            if (msg == WM_PAINT) {
                if (self->m_state.state.load() != OverlayState::Idle) {
                    self->m_renderer.render(self->m_state);
                }
                ValidateRect(hwnd, nullptr);
                return 0;
            }
            return self->m_input.handleMessage(hwnd, msg, wParam, lParam);
        } catch (const std::exception& ex) {
            LOG_ERROR("CaptureOverlay::staticWndProc caught unhandled exception (msg={}): {}", msg, ex.what());
            if (auto lg = tools3000::core::Logger::instance()) lg->flush();
            if (msg == WM_PAINT) {
                ValidateRect(hwnd, nullptr);
            }
            try { self->realCancel(); } catch (...) {}
            return 0;
        } catch (...) {
            LOG_ERROR("CaptureOverlay::staticWndProc caught unknown exception (msg={})", msg);
            if (auto lg = tools3000::core::Logger::instance()) lg->flush();
            if (msg == WM_PAINT) {
                ValidateRect(hwnd, nullptr);
            }
            try { self->realCancel(); } catch (...) {}
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::capture
