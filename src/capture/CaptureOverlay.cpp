#include "capture/CaptureOverlay.h"
#include "capture/CaptureBackend.h"
#include "capture/CursorOverlay.h"
#include "capture/MarkupBaseHelper.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "core/events/EventBus.h"
#include "capture/CaptureHistory.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/CaptureToolbarAccessibility.h"
#include "core/stats/PerformanceMonitor.h"
#include "core/utils/DpiUtils.h"
#include "core/accessibility/OverlayAnnouncement.h"
#include "core/accessibility/OverlayUiaProvider.h"
#include "core/config/ConfigManager.h"

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

}  // namespace

namespace tools3000::capture {

namespace {
constexpr UINT WM_ACCESSIBILITY_INVOKE_TOOLBAR = WM_APP + 74;

std::vector<tools3000::core::accessibility::OverlayUiaAction>
toolbarUiaActions(HWND hwnd, const CaptureState& state) {
    RECT window{};
    GetWindowRect(hwnd, &window);
    std::vector<tools3000::core::accessibility::OverlayUiaAction> actions;
    actions.reserve(state.toolbarButtons.size());
    for (std::size_t index = 0; index < state.toolbarButtons.size(); ++index) {
        const auto& button = state.toolbarButtons[index];
        const auto name = toolbarButtonAccessibleName(button);
        const auto shortcut = toolbarButtonKeyboardShortcut(button);
        actions.push_back({
            L"Tools3000.CaptureToolbar." + std::to_wstring(index), name,
            L"截图工具栏：" + name, shortcut, {
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
    // 覆盖层是整个虚拟桌面大小。启动时创建一个隐藏的 layered D2D 窗口，
    // 在部分显卡/驱动上即使不可见也会持续触发表面合成，占满一个 CPU 核。
    // 这里只保存模块句柄，真正截图时再创建，用完立即释放。
    return m_hInstance != nullptr;
}

void CaptureOverlay::shutdown() {
    ShortcutHintOverlay::instance().hide();
    const bool wasActive = m_state.state.load() != OverlayState::Idle;
    m_state.state = OverlayState::Idle;
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

bool CaptureOverlay::createOverlayWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = staticWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"Tools3000_CaptureOverlay";
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        wc.lpszClassName, nullptr,
        WS_POPUP,
        0, 0, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
        nullptr, nullptr, hInstance, this
    );

    if (!m_hwnd) {
        LOG_ERROR("截图覆盖层窗口创建失败");
        return false;
    }

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
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"截图界面初始化失败，请重试"});
        if (m_closedCallback) m_closedCallback();
        return;
    }
    tools3000::core::PerformanceMonitor::instance().recordLatency(
        "screenshot.window", elapsedMilliseconds(windowStarted));

    m_state.state = OverlayState::Selecting;
    m_state.dragging = false;
    m_state.isMarking = false;
    m_state.markupBaseReady = false;
    m_state.markupBaseRect = {0, 0, 0, 0};
    m_state.activeElement = nullptr;
    m_state.dragHandle = HitArea::None;
    m_state.isManipulating = false;
    m_state.toolbarButtons.clear();
    m_state.toolbarLayoutValid = false;
    m_state.markup.clearAll();
    m_state.loupeToastUntil = 0;
    m_state.currentTool = MarkupTool::Rectangle;
    m_state.isMarkupToolActive = false;
    m_state.isFadingOut = false;
    m_state.fadeOutStart = 0;

    auto& config = tools3000::core::ConfigManager::instance();
    m_state.beautyShell.enabled = config.get<bool>("/capture/beautyShellEnabled", false);
    const std::string shellTheme = config.get<std::string>("/capture/beautyShellTheme", "studio_slate");
    if (shellTheme == "studio_slate" || shellTheme == "aurora_purple") m_state.beautyShell.bgType = BeautyBackgroundType::StudioSlate;
    else if (shellTheme == "paper_chalk" || shellTheme == "sunrise_coral") m_state.beautyShell.bgType = BeautyBackgroundType::PaperChalk;
    else if (shellTheme == "silk_mist" || shellTheme == "emerald_sea") m_state.beautyShell.bgType = BeautyBackgroundType::SilkMist;
    else if (shellTheme == "midnight_graphite" || shellTheme == "obsidian_dark") m_state.beautyShell.bgType = BeautyBackgroundType::MidnightGraphite;
    else if (shellTheme == "pure_minimal" || shellTheme == "minimal_light") m_state.beautyShell.bgType = BeautyBackgroundType::PureMinimal;
    m_state.beautyShell.padding = config.get<int>("/capture/beautyShellPadding", 32);
    m_state.beautyShell.cornerRadius = static_cast<float>(config.get<int>("/capture/beautyShellRadius", 16));
    m_state.cornerRadius = static_cast<float>(config.get<double>("/screenshot/cornerRadius", 0.0));
    m_state.currentTextOutline = config.get<bool>("/capture/markup/text/outline", true);
    std::string outlineCol = config.get<std::string>("/capture/markup/text/outlineColor", "auto");
    if (outlineCol == "black") {
        m_state.currentTextOutlineColor = MarkupColor::Black();
    } else if (outlineCol == "white") {
        m_state.currentTextOutlineColor = MarkupColor::White();
    } else {
        m_state.currentTextOutlineColor = MarkupColor::Auto();
    }

    std::string savedCustomColor = config.get<std::string>("/capture/customColor", "");
    if (!savedCustomColor.empty() && savedCustomColor.size() >= 7 && savedCustomColor[0] == '#') {
        unsigned int r = 0, g = 0, b = 0;
        if (sscanf_s(savedCustomColor.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            m_state.customColor = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), 255};
            m_state.hasCustomColor = true;
        }
    }

    std::string recFmt = config.get<std::string>("/recording/format", "mp4_h264");
    m_state.recordSetup.format = (recFmt == "gif") ? RecordFormat::GIF
        : (recFmt == "mp4_h265" ? RecordFormat::MP4_H265
        : (recFmt == "webm_vp9" ? RecordFormat::WebM_VP9 : RecordFormat::MP4_H264));
    m_state.recordSetup.fps = std::clamp(config.get<int>("/recording/fps", 30), 1, 120);
    int bitrate = config.get<int>("/recording/bitrate", 10);
    if (bitrate <= 5) m_state.recordSetup.qualityLevel = 0;
    else if (bitrate <= 10) m_state.recordSetup.qualityLevel = 1;
    else m_state.recordSetup.qualityLevel = 2;
    bool defaultClickEff = config.get<bool>("/spotlight/clickRippleEnabled", false);
    m_state.recordSetup.showClickEffects = config.get<bool>("/recording/showClickEffects", defaultClickEff);
    m_state.recordSetup.showClickZoom = config.get<bool>("/recording/showClickZoom", true);
    m_state.recordSetup.includeKeycast = config.get<bool>("/recording/includeKeycast", true);
    m_state.recordSetup.captureSystemAudio = config.get<bool>("/recording/captureSystemAudio", false);
    m_state.recordSetup.captureMicrophone = config.get<bool>("/recording/captureMicrophone", false);

    const auto freezeStarted = std::chrono::steady_clock::now();
    if (!freezeScreen()) {
        LOG_ERROR("无法启动截图: 桌面底图捕获或上传失败");
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"无法捕获屏幕，请重试"});
        realCancel();
        return;
    }
    tools3000::core::PerformanceMonitor::instance().recordLatency(
        "screenshot.freeze", elapsedMilliseconds(freezeStarted));

    const auto uploadStarted = std::chrono::steady_clock::now();
    if (!m_renderer.updateScreenBitmap(m_state.frozenScreen)) {
        LOG_ERROR("无法启动截图: 桌面底图上传失败");
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"无法显示屏幕截图，请重试"});
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
        m_hwnd, L"截图选区。拖动鼠标选择区域；Enter 确认；Esc 取消。"
    );
    if (options.showShortcutHints) {
        ShortcutHintOverlay::instance().show(
            mode == OverlayMode::RecordRegion
                ? ShortcutHintContext::RecordSelecting
                : ShortcutHintContext::CaptureSelecting);
    }
    tools3000::core::PerformanceMonitor::instance().recordLatency(
        "screenshot", elapsedMilliseconds(totalStarted));
}

void CaptureOverlay::startEditPinned(const cv::Mat& image, const CaptureRegion& region,
                                     std::function<void(const cv::Mat& newImage)> onFinished) {
    if (image.empty() || region.width <= 0 || region.height <= 0) return;

    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    m_state.options = {};
    m_state.mode = OverlayMode::Screenshot;
    m_state.dpiScale = dpiScaleAt(cursorScreen);
    m_state.currentCursor = {
        cursorScreen.x - GetSystemMetrics(SM_XVIRTUALSCREEN),
        cursorScreen.y - GetSystemMetrics(SM_YVIRTUALSCREEN)};

    if (!m_hwnd && (!m_hInstance || !createOverlayWindow(m_hInstance))) {
        LOG_ERROR("无法启动贴图编辑: 覆盖层窗口初始化失败");
        return;
    }

    m_state.state = OverlayState::Selected;
    m_state.dragging = false;
    m_state.isMarking = false;
    m_state.markupBaseReady = false;
    m_state.markupBaseRect = {0, 0, 0, 0};
    m_state.activeElement = nullptr;
    m_state.dragHandle = HitArea::None;
    m_state.isManipulating = false;
    m_state.toolbarButtons.clear();
    m_state.toolbarLayoutValid = false;
    m_state.markup.clearAll();
    m_state.loupeToastUntil = 0;
    m_state.showTimestamp = GetTickCount();
    m_state.isFadingOut = false;
    m_state.fadeOutStart = 0;

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    m_state.dragStart = { region.x - vx, region.y - vy };
    m_state.dragEnd = { m_state.dragStart.x + region.width, m_state.dragStart.y + region.height };

    if (!freezeScreen()) {
        LOG_ERROR("无法捕获屏幕底图用于贴图编辑");
        realCancel();
        return;
    }

    // 将贴图原图覆盖至对应选区区域（确保 3 通道与 4 通道类型严格对齐）
    cv::Rect roi(m_state.dragStart.x, m_state.dragStart.y, region.width, region.height);
    roi &= cv::Rect(0, 0, m_state.frozenScreen.cols, m_state.frozenScreen.rows);
    if (roi.width == image.cols && roi.height == image.rows && !m_state.frozenScreen.empty()) {
        try {
            cv::Mat targetImage = image;
            const int targetChannels = m_state.frozenScreen.channels();
            if (targetChannels == 4 && image.channels() == 3) {
                cv::cvtColor(image, targetImage, cv::COLOR_BGR2BGRA);
            } else if (targetChannels == 3 && image.channels() == 4) {
                cv::cvtColor(image, targetImage, cv::COLOR_BGRA2BGR);
            } else if (targetChannels == 1 && image.channels() != 1) {
                cv::cvtColor(image, targetImage, cv::COLOR_BGR2GRAY);
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

    // 设置回调
    m_state.callback = [onFinished](const CaptureRegion&, const cv::Mat& markedImage, const CaptureCompletion&) {
        if (onFinished && !markedImage.empty()) {
            onFinished(markedImage);
        }
    };

    m_renderer.invalidate();

    SetWindowPos(m_hwnd, HWND_TOPMOST,
                 GetSystemMetrics(SM_XVIRTUALSCREEN),
                 GetSystemMetrics(SM_YVIRTUALSCREEN),
                 GetSystemMetrics(SM_CXVIRTUALSCREEN),
                 GetSystemMetrics(SM_CYVIRTUALSCREEN),
                 SWP_SHOWWINDOW);

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
        m_hwnd, L"截图标注。使用工具栏完成标注；Enter 确认；Esc 取消。"
    );

    ShortcutHintOverlay::instance().show(ShortcutHintContext::CaptureSelected);
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
    if (current == OverlayState::Idle) return;
    ShortcutHintContext context = ShortcutHintContext::CaptureSelecting;
    if (m_state.mode == OverlayMode::RecordRegion) {
        context = ShortcutHintContext::RecordSelecting;
    } else if (current == OverlayState::Selected || current == OverlayState::Marking) {
        context = ShortcutHintContext::CaptureSelected;
    }
    ShortcutHintOverlay::instance().show(context);
}

void CaptureOverlay::cleanupAfterDelivery() {
    if (m_hwnd) {
        m_renderer.releaseWindowResources();
        HWND hwndToDestroy = m_hwnd;
        m_hwnd = nullptr;
        DestroyWindow(hwndToDestroy);
    }
    m_state.markup.clearAll();
    m_state.detectedWindowHierarchy.clear();
    m_state.detectedWindow = {};
    releaseFrozenSurface();
    if (m_closedCallback) m_closedCallback();

    // 冷路径退场：主动修剪物理内存，将大图/D2D/DirectX 缓存归还系统
    tools3000::core::WinUtils::trimWorkingSet();
}

void CaptureOverlay::realCancel() {
    ShortcutHintOverlay::instance().hide();
    m_state.state = OverlayState::Idle;
    ReleaseCapture();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));

    if (m_hwnd) {
        tools3000::core::accessibility::hideOverlay(m_hwnd);
        ShowWindow(m_hwnd, SW_HIDE);
    }
    cleanupAfterDelivery();
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
    if (m_state.markup.elementCount() > 0) {
        syncMarkupBase(m_state.frozenScreen, targetRect, m_state.markupBaseReady, m_state.markupBaseRect, m_state.markup);
        cropped = m_state.markup.getCompositeImage();
    } else {
        cropped = cropMarkupBase(m_state.frozenScreen, targetRect);
    }

    if (m_state.beautyShell.enabled && !cropped.empty() && m_state.mode != OverlayMode::RecordRegion) {
        cropped = applyBeautyShell(cropped, m_state.beautyShell);
    } else if (m_state.cornerRadius > 0.5f && !cropped.empty() && m_state.mode != OverlayMode::RecordRegion) {
        cropped = applyRoundedCorners(cropped, m_state.cornerRadius);
    }

    int offsetX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int offsetY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    CaptureRegion region{x1 + offsetX, y1 + offsetY, w, h, m_state.cornerRadius};

    auto cb = m_state.callback;
    auto rcb = m_state.recordCallback;
    auto mode = m_state.mode;

    // 阶段 1 (视觉退场): 立即同步隐藏覆盖层与提示窗，释放光标捕获，
    // 确保录屏首帧不被遮挡，桌面视觉即刻恢复纯净交互。
    ShortcutHintOverlay::instance().hide();
    m_state.state = OverlayState::Idle;
    ReleaseCapture();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));

    if (m_hwnd) {
        tools3000::core::accessibility::hideOverlay(m_hwnd);
        ShowWindow(m_hwnd, SW_HIDE);
    }

    // 阶段 2 (业务交付): 同步执行交付回调 (写入多矩阵剪贴板与保存磁盘)
    if (mode == OverlayMode::RecordRegion) {
        if (rcb) rcb(region, m_state.recordSetup);
    } else {
        if (cb) cb(region, cropped, completion);
    }

    // 阶段 3 (冷路径清理与内存修剪): 业务数据交付彻底完成后，再安全销毁窗口、释放底图并执行 trimWorkingSet()
    cleanupAfterDelivery();
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
        if (msg == WM_MOUSEACTIVATE) {
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
            return MA_ACTIVATE;
        }
        if (msg == WM_CLOSE) {
            DestroyWindow(hwnd);
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
            tools3000::core::accessibility::disconnectOverlayUiaProvider(hwnd);
        }
        if (msg == WM_PAINT) {
            self->m_renderer.render(self->m_state);
            ValidateRect(hwnd, nullptr);
            return 0;
        }
        return self->m_input.handleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::capture
