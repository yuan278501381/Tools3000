#include "core/plugin/IPlugin.h"
#include "Tools3000Version.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/utils/WinUtils.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/hotkey/HotkeyPolicy.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/utils/UiThreadJoin.h"
#include "capture/ScreenCapture.h"
#include "capture/ScreenRecorder.h"
#include "capture/RecordingIndicator.h"
#include "capture/PinWindow.h"
#include "capture/CaptureOverlay.h"
#include "capture/CaptureHistory.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/ScrollCapture.h"
#include <opencv2/imgcodecs.hpp>
#include "ocr/OcrEngine.h"
#include "ocr/OcrResultWindow.h"
#include <thread>
#include <filesystem>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <fstream>
#include <iterator>
#include <windows.h>

namespace tools3000::capture {

namespace {

tools3000::core::HotkeyDef configuredHotkey(const std::string& name,
                                       const tools3000::core::HotkeyDef& fallback) {
    const auto text = tools3000::core::ConfigManager::instance().get<std::string>(
        "/hotkeys/" + name, fallback.toString());
    if (text.empty()) return {};
    return tools3000::core::HotkeyDef::fromString(text).value_or(fallback);
}

std::string timestampedPath(const std::string& directory, const char* prefix,
                            const char* extension) {
    if (directory.empty()) return {};
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream name;
    name << prefix << std::put_time(&local, "%Y%m%d_%H%M%S") << '_'
         << std::setfill('0') << std::setw(3) << ms.count() << extension;
    const auto dir = tools3000::core::WinUtils::utf8ToWstring(directory);
    return tools3000::core::WinUtils::wstringToUtf8((std::filesystem::path(dir) / name.str()).wstring());
}

ImageFormat imageFormatFromConfig(const std::string& value) {
    if (value == "jpg" || value == "jpeg") return ImageFormat::JPEG;
    if (value == "webp") return ImageFormat::WebP;
    if (value == "bmp") return ImageFormat::BMP;
    return ImageFormat::PNG;
}

const char* imageExtension(ImageFormat format) {
    switch (format) {
        case ImageFormat::JPEG: return ".jpg";
        case ImageFormat::WebP: return ".webp";
        case ImageFormat::BMP: return ".bmp";
        default: return ".png";
    }
}

void startRecordingForRegion(const CaptureRegion& region, const RecordSetupOptions& setup);

CaptureOptions configuredCaptureOptions() {
    auto& config = tools3000::core::ConfigManager::instance();
    CaptureOptions options;
    options.format = imageFormatFromConfig(config.get<std::string>("/capture/format", "png"));
    options.quality = std::clamp(config.get<int>("/capture/quality", 90), 1, 100);
    options.saveToFile = config.get<bool>("/capture/saveToFile", true);
    options.copyToClipboard = config.get<bool>("/capture/copyToClipboard", true);
    options.includeCursor = config.get<bool>("/capture/includeCursor", false);
    options.showCrosshair = config.get<bool>("/capture/showCrosshair", false);
    options.autoDetectWindow = config.get<bool>("/capture/autoDetectWindow", true);
    options.showShortcutHints = config.get<bool>("/capture/showShortcutHints", true);
    options.autoBypassFullscreen = config.get<bool>("/capture/autoBypassFullscreen", true);
    const auto directory = config.get<std::string>(
        "/capture/saveDirectory", config.get<std::string>("/capture/savePath", ""));
    options.savePath = timestampedPath(directory, "Tools3000_", imageExtension(options.format));
    CaptureOverlay::instance().setRecordCallback(startRecordingForRegion);
    return options;
}

RecordFormat recordFormatFromConfig(const std::string& value) {
    if (value == "mp4_h265") return RecordFormat::MP4_H265;
    if (value == "webm_vp9") return RecordFormat::WebM_VP9;
    if (value == "gif") return RecordFormat::GIF;
    return RecordFormat::MP4_H264;
}

const char* recordExtension(RecordFormat format) {
    switch (format) {
        case RecordFormat::WebM_VP9: return ".webm";
        case RecordFormat::GIF: return ".gif";
        default: return ".mp4";
    }
}

void startRecordingForRegion(const CaptureRegion& region, const RecordSetupOptions& setup = {});

RecordOptions configuredRecordOptions(const CaptureRegion& region) {
    auto& config = tools3000::core::ConfigManager::instance();
    RecordOptions options;
    options.regionX = region.x;
    options.regionY = region.y;
    options.width = region.width;
    options.height = region.height;
    options.cornerRadius = region.cornerRadius;
    options.fullScreen = false;
    options.format = recordFormatFromConfig(config.get<std::string>("/recording/format", "mp4_h264"));
    options.fps = std::clamp(config.get<int>("/recording/fps", 30), 1, 120);
    options.bitrateMbps = std::clamp(config.get<int>("/recording/bitrate", 8), 1, 100);
    options.includeCursor = config.get<bool>("/recording/includeCursor", true);
    options.showClickEffects = config.get<bool>("/recording/showClickEffects", false);
    options.showClickZoom = config.get<bool>("/recording/showClickZoom", true);
    options.includeKeycast = config.get<bool>("/recording/includeKeycast", true);
    options.captureSystemAudio = config.get<bool>("/recording/captureSystemAudio", false);
    options.captureMicrophone = config.get<bool>("/recording/captureMicrophone", false);
    options.systemAudioDeviceId = config.get<std::string>("/recording/systemAudioDeviceId", "");
    options.microphoneDeviceId = config.get<std::string>("/recording/microphoneDeviceId", "");
    options.systemAudioVolume = std::clamp(
        config.get<int>("/recording/systemAudioVolume", 100), 0, 200) / 100.0f;
    options.microphoneVolume = std::clamp(
        config.get<int>("/recording/microphoneVolume", 100), 0, 200) / 100.0f;
    options.countdownSeconds = std::clamp(
        config.get<int>("/recording/countdownSeconds", 3), 0, 10);
    options.experimentalGpuEncoding = config.get<bool>(
        "/recording/experimentalGpuEncoding", false);
    const auto directory = config.get<std::string>(
        "/recording/saveDirectory", config.get<std::string>("/recording/savePath", ""));
    options.outputPath = timestampedPath(directory, "record_", recordExtension(options.format));
    return options;
}

void armRecordPauseHotkey(RecordState state) {
    const bool recording = (state != RecordState::Idle);
    tools3000::core::HotkeyManager::instance().setHotkeyArmed(
        "Record Pause",
        tools3000::core::hotkeyShouldBeArmed(
            tools3000::core::HotkeyArmScope::WhileRecording,
            recording));
    tools3000::core::HotkeyManager::instance().setHotkeyArmed(
        "Record Snapshot",
        tools3000::core::hotkeyShouldBeArmed(
            tools3000::core::HotkeyArmScope::WhileRecording,
            recording));
}

void configureRecorderStateCallback() {
    ScreenRecorder::instance().setStateCallback([](RecordState state, const RecordStats& stats) {
        tools3000::core::MainThreadDispatcher::instance().post([state, stats]() {
            armRecordPauseHotkey(state);
            auto& indicator = RecordingIndicator::instance();
            if (state == RecordState::Idle) {
                ShortcutHintOverlay::instance().hide();
                tools3000::core::MessageBridge::instance().invokeHandler("keycast.setDockingRegion", {{"active", false}});
                if (!stats.stopReason.empty() &&
                    ScreenRecorder::instance().needsFinalization()) {
                    const auto path = ScreenRecorder::instance().stopRecording();
                    if (!path.empty()) {
                        indicator.showCompleted(path, stats.durationSec);
                        const bool isWeChatLimit = stats.stopReason == "gif_wechat_limit";
                        const bool diskFailure = stats.stopReason == "low_disk_space" ||
                            stats.stopReason == "disk_space_query_failed";
                        std::wstring toastMsg = isWeChatLimit
                            ? (tools3000::core::WinUtils::isSystemLanguageChinese() ? L"GIF 已达微信 20MB 上限，已自动为您安全保存" : L"GIF reached WeChat 20MB limit and saved")
                            : (diskFailure
                                ? (tools3000::core::WinUtils::isSystemLanguageChinese() ? L"磁盘空间不足，录屏已安全保存" : L"Low disk space, recording saved")
                                : (tools3000::core::WinUtils::isSystemLanguageChinese() ? L"录屏已停止，已保存可用内容" : L"Recording stopped, content saved"));
                        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{toastMsg});
                    } else {
                        indicator.hide();
                        tools3000::core::EventBus::instance().publish(
                            tools3000::core::ShowToastEvent{L"录屏已停止，未生成有效内容"});
                    }
                } else if (!indicator.isCompleted()) {
                    indicator.hide();
                }
            }
            else {
                indicator.setPaused(state == RecordState::Paused);
                indicator.update(stats.durationSec, stats.frameCount);
                ShortcutHintOverlay::instance().show(
                    state == RecordState::Paused
                        ? ShortcutHintContext::RecordingPaused
                        : ShortcutHintContext::Recording);
            }
        });
    });
}

void stopRecordingWithFeedback() {
    auto& indicator = RecordingIndicator::instance();
    ShortcutHintOverlay::instance().hide();
    tools3000::core::MessageBridge::instance().invokeHandler("keycast.setDockingRegion", {{"active", false}});
    const double duration = ScreenRecorder::instance().stats().durationSec;
    const auto path = ScreenRecorder::instance().stopRecording();
    if (!path.empty()) {
        LOG_INFO("录屏已保存: {}", path);
        indicator.showCompleted(path, duration);
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"录屏已保存"});
    } else {
        indicator.hide();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"录屏已停止，未生成有效内容"});
    }
}

bool toggleRecordingPauseWithFeedback() {
    auto& recorder = ScreenRecorder::instance();
    if (recorder.state() == RecordState::Recording) {
        recorder.pauseRecording();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"录屏已暂停"});
        return true;
    }
    if (recorder.state() == RecordState::Paused) {
        recorder.resumeRecording();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"录屏已继续"});
        return true;
    }
    return false;
}

void toggleSystemAudioMuteWithFeedback() {
    auto& recorder = ScreenRecorder::instance();
    if (!recorder.toggleSystemAudioMuted()) return;
    const auto muted = recorder.stats().systemAudioMuted;
    tools3000::core::EventBus::instance().publish(
        tools3000::core::ShowToastEvent{muted ? L"系统声音已静音" : L"系统声音已恢复"});
}

void toggleMicrophoneMuteWithFeedback() {
    auto& recorder = ScreenRecorder::instance();
    if (!recorder.toggleMicrophoneMuted()) return;
    const auto muted = recorder.stats().microphoneMuted;
    tools3000::core::EventBus::instance().publish(
        tools3000::core::ShowToastEvent{muted ? L"麦克风已静音" : L"麦克风已恢复"});
}

void startRecordingForRegion(const CaptureRegion& region, const RecordSetupOptions& setup) {
    configureRecorderStateCallback();
    auto options = configuredRecordOptions(region);
    options.format = setup.format;
    options.fps = setup.fps;
    if (setup.qualityLevel == 0) options.bitrateMbps = 5;
    else if (setup.qualityLevel == 1) options.bitrateMbps = 10;
    else if (setup.qualityLevel == 2) options.bitrateMbps = 20;
    options.showClickEffects = setup.showClickEffects;
    options.showClickZoom = setup.showClickZoom;
    options.includeKeycast = setup.includeKeycast;
    options.captureSystemAudio = setup.captureSystemAudio && setup.format != RecordFormat::GIF;
    options.captureMicrophone = setup.captureMicrophone && setup.format != RecordFormat::GIF;
    options.outputPath = timestampedPath(
        tools3000::core::ConfigManager::instance().get<std::string>(
            "/recording/saveDirectory", tools3000::core::ConfigManager::instance().get<std::string>("/recording/savePath", "")),
        "record_", recordExtension(options.format));
    if (options.includeKeycast) {
        tools3000::core::MessageBridge::instance().invokeHandler("keycast.setDockingRegion", {
            {"active", true},
            {"x", region.x},
            {"y", region.y},
            {"width", region.width},
            {"height", region.height}
        });
    }
    if (ScreenRecorder::instance().startRecording(options)) {
        RecordingIndicator::instance().setRecordingRegion(region.x, region.y, region.width, region.height, region.cornerRadius);
        RecordingIndicator::instance().show();
        ShortcutHintOverlay::instance().show(
            ShortcutHintContext::Recording,
            {region.x + region.width / 2, region.y + region.height / 2});
    } else {
        tools3000::core::MessageBridge::instance().invokeHandler("keycast.setDockingRegion", {{"active", false}});
        const auto reason = ScreenRecorder::instance().stats().stopReason;
        const auto message = reason == "output_directory_unavailable" ||
                reason == "output_directory_not_writable"
            ? L"无法写入录屏保存目录"
            : reason == "insufficient_disk_space"
                ? L"剩余空间不足，无法开始录屏"
                : reason == "disk_space_query_failed"
                    ? L"无法检查保存目录的剩余空间"
                    : L"无法开始录屏，请查看日志";
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{message});
    }
}

void toggleRecording() {
    auto& recorder = ScreenRecorder::instance();
    if (recorder.state() == RecordState::Idle) {
        auto& overlay = CaptureOverlay::instance();
        overlay.setRecordCallback(startRecordingForRegion);
        overlay.startSelection(configuredCaptureOptions(), OverlayMode::RecordRegion);
    } else {
        // Global hotkey and tray action are start/stop. Pause/resume remains an
        // explicit control on the always-visible recording indicator.
        stopRecordingWithFeedback();
    }
}

}  // namespace

static std::atomic<bool> g_ocrPending{false};
static std::atomic<bool> g_ocrRunning{false};
static std::jthread g_ocrWorker;

static void onCaptureCompletedForOcr(const tools3000::capture::CaptureResult& result) {
    if (!g_ocrPending.exchange(false)) return;
    if (!result.success || result.filePath.empty()) return;

    std::string path = result.filePath;
    if (g_ocrRunning.exchange(true)) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"OCR 正在识别，请稍候"});
        return;
    }
    if (g_ocrWorker.joinable()) g_ocrWorker.join();
    g_ocrWorker = std::jthread([path]() {
        try {
            std::string text = tools3000::ocr::OcrEngine::instance().recognizeImageFile(path);
            if (text.empty()) {
                tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{L"未识别到文字"});
            } else {
                const bool copy = tools3000::core::ConfigManager::instance().get<bool>("/ocr/copyResult", true);
                const bool showResult = tools3000::core::ConfigManager::instance().get<bool>("/ocr/showResultWindow", true);
                if (copy) tools3000::core::WinUtils::copyToClipboard(text);
                if (showResult) {
                    tools3000::core::MainThreadDispatcher::instance().post([text = std::move(text)]() {
                        tools3000::ocr::OcrResultWindow::instance().showResult(text);
                    });
                } else {
                    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                        copy ? L"OCR 识别完成，已复制到剪贴板" : L"OCR 识别完成"});
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("OCR 后台线程异常: {}", e.what());
            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{L"OCR 识别失败"});
        } catch (...) {
            LOG_ERROR("OCR 后台线程未知异常");
            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{L"OCR 识别失败"});
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
        g_ocrRunning.store(false);
    });
}


static void triggerOcrCapture() {
    auto& ocr = tools3000::ocr::OcrEngine::instance();
    if (!ocr.isAvailable()) {
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{L"未找到 OCR 语言包"});
        return;
    }
    LOG_INFO("OCR Capture Triggered");
    auto opts = configuredCaptureOptions();
    opts.copyToClipboard = false;
    opts.saveToFile = true;
    auto tempPath = tools3000::core::WinUtils::getAppDataDirectory() / L"temp";
    std::filesystem::create_directories(tempPath);
    opts.savePath = tools3000::core::WinUtils::wstringToUtf8(
        (tempPath / (L"ocr_" + std::to_wstring(GetTickCount64()) + L".png")).wstring());
    opts.format = ImageFormat::PNG;
    g_ocrPending.store(true);
    tools3000::capture::ScreenCapture::instance().startCapture(opts);
}

class CapturePlugin : public tools3000::core::IPlugin {
public:
    const char* getName() const override { return "Capture"; }
    const char* getVersion() const override { return tools3000::version::String; }

    bool initialize() override {
        LOG_INFO("CapturePlugin: 初始化截图/录屏引擎");

        if (!initializeMarkupTextRenderer()) return false;

        HMODULE hMod = GetModuleHandleW(nullptr);

        ScreenCapture::instance().initialize(hMod);
        ScreenCapture::instance().setCallback(onCaptureCompletedForOcr);
        ScreenRecorder::instance().initialize();
        CaptureOverlay::instance().setRecordCallback(startRecordingForRegion);

        auto& indicator = RecordingIndicator::instance();
        indicator.initialize(hMod);
        indicator.onPause([]() {
            toggleRecordingPauseWithFeedback();
        });
        indicator.onStop([]() {
            stopRecordingWithFeedback();
        });
        indicator.onSnapshot([]() {
            ScreenRecorder::instance().requestSnapshot();
        });
        indicator.onSystemAudioMute(toggleSystemAudioMuteWithFeedback);
        indicator.onMicrophoneMute(toggleMicrophoneMuteWithFeedback);

        tools3000::ocr::OcrEngine::instance().initialize();

        
        auto& mb = tools3000::core::MessageBridge::instance();
        auto& bus = tools3000::core::EventBus::instance();

        // 订阅 EventBus 上的托盘/快捷键触发事件
        m_screenshotSubscription = bus.subscribe<tools3000::core::ActionTriggerScreenshotEvent>([](const tools3000::core::ActionTriggerScreenshotEvent&) {
            g_ocrPending.store(false);
            tools3000::capture::ScreenCapture::instance().startCapture(configuredCaptureOptions());
        });

        m_recordingSubscription = bus.subscribe<tools3000::core::ActionToggleRecordingEvent>([](const tools3000::core::ActionToggleRecordingEvent&) {
            toggleRecording();
        });

        m_themeSubscription = bus.subscribe<tools3000::core::ThemeChangedEvent>([](const tools3000::core::ThemeChangedEvent&) {
            tools3000::capture::CaptureOverlay::instance().reloadThemeColors();
        });
        
        mb.registerHandler("capture.triggerScreenshot", [](const nlohmann::json&) -> nlohmann::json {
            g_ocrPending.store(false);
            tools3000::capture::ScreenCapture::instance().startCapture(configuredCaptureOptions());
            return {{"success", true}};
        });

        mb.registerHandler("capture.toggleRecording", [](const nlohmann::json&) -> nlohmann::json {
            toggleRecording();
            return {{"success", true}};
        });

        mb.registerHandler("recording.togglePause", [](const nlohmann::json&) -> nlohmann::json {
            return {{"success", toggleRecordingPauseWithFeedback()}};
        });

        mb.registerHandler("recording.snapshot", [](const nlohmann::json&) -> nlohmann::json {
            ScreenRecorder::instance().requestSnapshot();
            return {{"success", true}};
        });

        auto& hotkeys = tools3000::core::HotkeyManager::instance();
        hotkeys.registerHotkey("Screenshot", configuredHotkey("Screenshot", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Shift, 'A'}), []() {
            g_ocrPending.store(false);
            tools3000::capture::ScreenCapture::instance().startCapture(configuredCaptureOptions());
        });
        hotkeys.registerHotkey("Record", configuredHotkey("Record", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Shift, 'R'}), []() {
            toggleRecording();
        });
        hotkeys.registerHotkey("Record Pause", configuredHotkey("Record Pause", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Shift, 'P'}), []() {
            toggleRecordingPauseWithFeedback();
        });
        hotkeys.registerHotkey("Record Snapshot", configuredHotkey("Record Snapshot", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Shift, 'S'}), []() {
            ScreenRecorder::instance().requestSnapshot();
        });
        armRecordPauseHotkey(ScreenRecorder::instance().state());
        configureRecorderStateCallback();
        hotkeys.registerHotkey("OCR", configuredHotkey("OCR", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Shift, 'O'}), []() {
            triggerOcrCapture();
        });
        hotkeys.registerHotkey("Pin Toggle", configuredHotkey("Pin Toggle", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt | tools3000::core::ModKey::Shift, 'X'}), []() {
            tools3000::capture::PinWindow::toggleClickThroughUnderCursor();
        });
        hotkeys.registerHotkey("Pin Paste", configuredHotkey("Pin Paste", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt | tools3000::core::ModKey::Shift, 'V'}), []() {
            tools3000::capture::PinWindow::createFromClipboard();
        });
        hotkeys.registerHotkey("Pin Hide All", configuredHotkey("Pin Hide All", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt | tools3000::core::ModKey::Shift, 'H'}), []() {
            tools3000::capture::PinWindow::toggleHideAll();
        });
        hotkeys.registerHotkey("Pin Arrange", configuredHotkey("Pin Arrange", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt | tools3000::core::ModKey::Shift, 'G'}), []() {
            tools3000::capture::PinWindow::arrangeAll();
        });
        hotkeys.registerHotkey("Mute System Audio", configuredHotkey("Mute System Audio", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt | tools3000::core::ModKey::Shift, 'S'}), []() {
            toggleSystemAudioMuteWithFeedback();
        });
        hotkeys.registerHotkey("Mute Microphone", configuredHotkey("Mute Microphone", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt | tools3000::core::ModKey::Shift, 'M'}), []() {
            toggleMicrophoneMuteWithFeedback();
        });

        mb.registerHandler("capture.getSettings", [](const nlohmann::json&) -> nlohmann::json {
            auto& config = tools3000::core::ConfigManager::instance();
            return {
                {"format", config.get<std::string>("/capture/format", "png")},
                {"quality", config.get<int>("/capture/quality", 90)},
                {"saveToFile", config.get<bool>("/capture/saveToFile", true)},
                {"copyToClipboard", config.get<bool>("/capture/copyToClipboard", true)},
                {"includeCursor", config.get<bool>("/capture/includeCursor", false)},
                {"saveDirectory", config.get<std::string>(
                    "/capture/saveDirectory", config.get<std::string>("/capture/savePath", ""))},
                {"showCrosshair", config.get<bool>("/capture/showCrosshair", false)},
                {"autoDetectWindow", config.get<bool>("/capture/autoDetectWindow", true)},
                {"showShortcutHints", config.get<bool>("/capture/showShortcutHints", true)},
                {"autoBypassFullscreen", config.get<bool>("/capture/autoBypassFullscreen", true)},
                {"beautyShellEnabled", config.get<bool>("/capture/beautyShellEnabled", false)},
                {"beautyShellTheme", config.get<std::string>("/capture/beautyShellTheme", "aurora_purple")},
                {"beautyShellPadding", config.get<int>("/capture/beautyShellPadding", 32)},
                {"beautyShellRadius", config.get<int>("/capture/beautyShellRadius", 16)}
            };
        });

        mb.registerHandler("capture.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
            static const std::unordered_set<std::string> formats = {"png", "jpg", "jpeg", "webp", "bmp"};
            static const std::unordered_set<std::string> themes = {
                "studio_slate", "paper_chalk", "silk_mist", "midnight_graphite", "pure_minimal",
                "aurora_purple", "sunrise_coral", "emerald_sea", "obsidian_dark", "minimal_light"
            };
            static const std::unordered_set<std::string> boolKeys = {
                "saveToFile", "copyToClipboard", "includeCursor", "showCrosshair", "autoDetectWindow",
                "showShortcutHints", "autoBypassFullscreen", "beautyShellEnabled"
            };
            if (!params.is_object() || params.empty()) return {{"success", false}, {"error", "no settings supplied"}};
            for (const auto& [key, value] : params.items()) {
                if (key == "format" && (!value.is_string() || !formats.contains(value.get<std::string>())))
                    return {{"success", false}, {"error", "invalid image format"}};
                if (key == "quality" && (!value.is_number_integer() || value.get<int>() < 1 || value.get<int>() > 100))
                    return {{"success", false}, {"error", "quality must be between 1 and 100"}};
                if (key == "saveDirectory" && (!value.is_string() || value.get_ref<const std::string&>().size() > 32767))
                    return {{"success", false}, {"error", "invalid save directory"}};
                if (key == "beautyShellTheme" && (!value.is_string() || !themes.contains(value.get<std::string>())))
                    return {{"success", false}, {"error", "invalid beauty shell theme"}};
                if (key == "beautyShellPadding" && (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 128))
                    return {{"success", false}, {"error", "beauty shell padding must be between 0 and 128"}};
                if (key == "beautyShellRadius" && (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 64))
                    return {{"success", false}, {"error", "beauty shell radius must be between 0 and 64"}};
                if (boolKeys.contains(key) && !value.is_boolean())
                    return {{"success", false}, {"error", key + " must be boolean"}};
                if (key != "format" && key != "quality" && key != "saveDirectory" &&
                    key != "beautyShellTheme" && key != "beautyShellPadding" && key != "beautyShellRadius" &&
                    !boolKeys.contains(key))
                    return {{"success", false}, {"error", "unsupported setting: " + key}};
            }
            const bool saved = tools3000::core::ConfigManager::instance().mergePatch(
                {{"capture", params}}, "/capture");
            if (saved && params.contains("showShortcutHints")) {
                const bool enabled = params.at("showShortcutHints").get<bool>();
                tools3000::capture::CaptureOverlay::instance().setShortcutHintsEnabled(enabled);
                if (!enabled) {
                    ShortcutHintOverlay::instance().hide();
                } else if (ScrollCapture::instance().isRunning()) {
                    ShortcutHintOverlay::instance().show(ShortcutHintContext::ScrollCapture);
                } else {
                    const auto recordState = ScreenRecorder::instance().state();
                    if (recordState == RecordState::Recording || recordState == RecordState::Paused) {
                        ShortcutHintOverlay::instance().show(
                            recordState == RecordState::Paused
                                ? ShortcutHintContext::RecordingPaused
                                : ShortcutHintContext::Recording);
                    }
                }
            }
            return {{"success", saved}, {"error", saved ? "" : "failed to persist settings"}};
        });

        mb.registerHandler("recording.getSettings", [](const nlohmann::json&) -> nlohmann::json {
            auto& config = tools3000::core::ConfigManager::instance();
            return {
                {"format", config.get<std::string>("/recording/format", "mp4_h264")},
                {"fps", config.get<int>("/recording/fps", 30)},
                {"bitrate", config.get<int>("/recording/bitrate", 8)},
                {"includeCursor", config.get<bool>("/recording/includeCursor", true)},
                {"showClickEffects", config.get<bool>("/recording/showClickEffects", false)},
                {"showClickZoom", config.get<bool>("/recording/showClickZoom", true)},
                {"includeKeycast", config.get<bool>("/recording/includeKeycast", true)},
                {"captureSystemAudio", config.get<bool>("/recording/captureSystemAudio", false)},
                {"captureMicrophone", config.get<bool>("/recording/captureMicrophone", false)},
                {"systemAudioDeviceId", config.get<std::string>("/recording/systemAudioDeviceId", "")},
                {"microphoneDeviceId", config.get<std::string>("/recording/microphoneDeviceId", "")},
                {"systemAudioVolume", config.get<int>("/recording/systemAudioVolume", 100)},
                {"microphoneVolume", config.get<int>("/recording/microphoneVolume", 100)},
                {"countdownSeconds", config.get<int>("/recording/countdownSeconds", 3)},
                {"experimentalGpuEncoding", config.get<bool>("/recording/experimentalGpuEncoding", false)},
                {"saveDirectory", config.get<std::string>(
                    "/recording/saveDirectory", config.get<std::string>("/recording/savePath", ""))}
            };
        });

        mb.registerHandler("recording.getStatus", [](const nlohmann::json&) -> nlohmann::json {
            auto& recorder = tools3000::capture::ScreenRecorder::instance();
            const auto stats = recorder.stats();
            return {
                {"state", static_cast<int>(recorder.state())},
                {"frameCount", stats.frameCount},
                {"droppedFrameCount", stats.droppedFrameCount},
                {"durationSec", stats.durationSec},
                {"countdownRemaining", stats.countdownRemaining},
                {"diskFreeBytes", stats.diskFreeBytes},
                {"estimatedRemainingSec", stats.estimatedRemainingSec},
                {"storageWarning", stats.storageWarning},
                {"captureLatencyMs", stats.captureLatencyMs},
                {"conversionLatencyMs", stats.conversionLatencyMs},
                {"encodeLatencyMs", stats.encodeLatencyMs},
                {"pipelineLatencyMs", stats.pipelineLatencyMs},
                {"effectiveFps", stats.effectiveFps},
                {"adaptiveFrameStep", stats.adaptiveFrameStep},
                {"performanceLimited", stats.performanceLimited},
                {"gpuExperimentRequested", stats.gpuExperimentRequested},
                {"gpuExperimentAvailable", stats.gpuExperimentAvailable},
                {"gpuExperimentStatus", stats.gpuExperimentStatus},
                {"stopReason", stats.stopReason},
                {"fileSizeBytes", stats.fileSizeBytes},
                {"captureBackend", stats.captureBackend},
                {"encoderName", stats.encoderName},
                {"hardwareEncoder", stats.hardwareEncoder},
                {"audioEncoderName", stats.audioEncoderName},
                {"systemAudioActive", stats.systemAudioActive},
                {"microphoneActive", stats.microphoneActive},
                {"droppedAudioFrames", stats.droppedAudioFrames},
                {"audioDiscontinuities", stats.audioDiscontinuities},
                {"audioReconnectAttempts", stats.audioReconnectAttempts},
                {"audioReconnectSuccesses", stats.audioReconnectSuccesses},
                {"systemAudioPeak", stats.systemAudioPeak},
                {"microphonePeak", stats.microphonePeak},
                {"mixedAudioPeak", stats.mixedAudioPeak},
                {"systemAudioMuted", stats.systemAudioMuted},
                {"microphoneMuted", stats.microphoneMuted},
                {"audioError", stats.audioError}
            };
        });

        mb.registerHandler("recording.getCapabilities", [](const nlohmann::json&) -> nlohmann::json {
            const auto capabilities = tools3000::capture::ScreenRecorder::capabilities();
            nlohmann::json backends = nlohmann::json::array();
            for (const auto& backend : capabilities.captureBackends) {
                backends.push_back({
                    {"id", backend.id}, {"name", backend.name},
                    {"available", backend.available}, {"accelerated", backend.accelerated},
                    {"unavailableReason", backend.unavailableReason}
                });
            }
            nlohmann::json encoders = nlohmann::json::array();
            for (const auto& encoder : capabilities.encoders) {
                encoders.push_back({
                    {"format", encoder.format}, {"name", encoder.name},
                    {"hardware", encoder.hardware}
                });
            }
            nlohmann::json audioDevices = nlohmann::json::array();
            for (const auto& device : capabilities.audioDevices) {
                audioDevices.push_back({
                    {"id", device.id}, {"name", device.name},
                    {"systemAudio", device.systemAudio},
                    {"defaultDevice", device.defaultDevice}
                });
            }
            return {
                {"captureBackends", std::move(backends)},
                {"encoders", std::move(encoders)},
                {"audioDevices", std::move(audioDevices)}
            };
        });

        mb.registerHandler("recording.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
            static const std::unordered_set<std::string> formats = {"mp4_h264", "mp4_h265", "webm_vp9", "gif"};
            static const std::unordered_set<int> countdowns = {0, 3, 5, 10};
            if (!params.is_object() || params.empty()) return {{"success", false}, {"error", "no settings supplied"}};
            for (const auto& [key, value] : params.items()) {
                if (key == "format" && (!value.is_string() || !formats.contains(value.get<std::string>())))
                    return {{"success", false}, {"error", "invalid recording format"}};
                if (key == "fps" && (!value.is_number_integer() || value.get<int>() < 1 || value.get<int>() > 120))
                    return {{"success", false}, {"error", "fps must be between 1 and 120"}};
                if (key == "bitrate" && (!value.is_number_integer() || value.get<int>() < 1 || value.get<int>() > 100))
                    return {{"success", false}, {"error", "bitrate must be between 1 and 100"}};
                if (key == "countdownSeconds" && (!value.is_number_integer() ||
                    !countdowns.contains(value.get<int>())))
                    return {{"success", false}, {"error", "countdown must be 0, 3, 5, or 10 seconds"}};
                if (key == "saveDirectory" && (!value.is_string() || value.get_ref<const std::string&>().size() > 32767))
                    return {{"success", false}, {"error", "invalid save directory"}};
                if ((key == "systemAudioDeviceId" || key == "microphoneDeviceId") &&
                    (!value.is_string() || value.get_ref<const std::string&>().size() > 2048))
                    return {{"success", false}, {"error", "invalid audio device id"}};
                if ((key == "systemAudioVolume" || key == "microphoneVolume") &&
                    (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 200))
                    return {{"success", false}, {"error", "audio volume must be between 0 and 200"}};
                if ((key == "includeCursor" || key == "showClickEffects" ||
                     key == "showClickZoom" || key == "includeKeycast" ||
                     key == "captureSystemAudio" || key == "captureMicrophone" ||
                     key == "experimentalGpuEncoding") &&
                    !value.is_boolean())
                    return {{"success", false}, {"error", key + " must be boolean"}};
                if (key != "format" && key != "fps" && key != "bitrate" && key != "saveDirectory" &&
                    key != "countdownSeconds" &&
                    key != "includeCursor" && key != "showClickEffects" &&
                    key != "showClickZoom" && key != "includeKeycast" &&
                    key != "captureSystemAudio" && key != "captureMicrophone" &&
                    key != "experimentalGpuEncoding" &&
                    key != "systemAudioDeviceId" && key != "microphoneDeviceId" &&
                    key != "systemAudioVolume" && key != "microphoneVolume")
                    return {{"success", false}, {"error", "unsupported setting: " + key}};
            }
            const bool saved = tools3000::core::ConfigManager::instance().mergePatch(
                {{"recording", params}}, "/recording");
            if (saved && (params.contains("systemAudioVolume") ||
                          params.contains("microphoneVolume"))) {
                auto& config = tools3000::core::ConfigManager::instance();
                ScreenRecorder::instance().setAudioVolumes(
                    std::clamp(config.get<int>("/recording/systemAudioVolume", 100), 0, 200) / 100.0f,
                    std::clamp(config.get<int>("/recording/microphoneVolume", 100), 0, 200) / 100.0f);
            }
            return {{"success", saved}, {"error", saved ? "" : "failed to persist settings"}};
        });

        mb.registerHandler("ocr.getSettings", [](const nlohmann::json&) -> nlohmann::json {
            auto& config = tools3000::core::ConfigManager::instance();
            return {
                {"engine", "windows"},
                {"language", "system"},
                {"copyResult", config.get<bool>("/ocr/copyResult", true)},
                {"showResultWindow", config.get<bool>("/ocr/showResultWindow", true)}
            };
        });

        mb.registerHandler("ocr.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
            if (!params.is_object() || params.empty()) return {{"success", false}, {"error", "no settings supplied"}};
            for (const auto& [key, value] : params.items()) {
                if ((key != "copyResult" && key != "showResultWindow") || !value.is_boolean())
                    return {{"success", false}, {"error", "unsupported OCR setting: " + key}};
            }
            const bool saved = tools3000::core::ConfigManager::instance().mergePatch(
                {{"ocr", params}}, "/ocr");
            return {{"success", saved}, {"error", saved ? "" : "failed to persist settings"}};
        });

        mb.registerHandler("ocr.getStatus", [](const nlohmann::json&) -> nlohmann::json {
            return {{"available", tools3000::ocr::OcrEngine::instance().isAvailable()}};
        });

        mb.registerHandler("ocr.recognizeImageFile", [](const nlohmann::json& params) -> nlohmann::json {
            std::string path = params.value("path", "");
            if (path.empty()) return {{"success", false}, {"error", "path is required"}};
            std::string text = tools3000::ocr::OcrEngine::instance().recognizeImageFile(path);
            bool copy = params.value("copyToClipboard", tools3000::core::ConfigManager::instance().get<bool>("/ocr/copyResult", true));
            if (copy && !text.empty()) tools3000::core::WinUtils::copyToClipboard(text);
            return {{"success", true}, {"text", text}, {"copied", copy && !text.empty()}};
        });

        mb.registerHandler("capture.pinImageFile", [](const nlohmann::json& params) -> nlohmann::json {
            std::string path = params.value("path", "");
            if (path.empty()) return {{"success", false}, {"error", "path is required"}};
            try {
                const auto filePath = std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(path));
                std::ifstream stream(filePath, std::ios::binary);
                if (!stream) return {{"success", false}, {"error", "file not found"}};
                std::vector<unsigned char> bytes(
                    (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                cv::Mat image = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
                if (!image.empty()) {
                    POINT pt;
                    GetCursorPos(&pt);
                    POINT spawnPos = tools3000::capture::PinWindow::calculateSmartSpawnPosition(image.cols, image.rows, &pt);
                    tools3000::capture::PinWindow::create(image, spawnPos.x, spawnPos.y);
                    return {{"success", true}};
                }
            } catch (const std::exception& e) {
                LOG_ERROR("贴图文件加载失败: {}", e.what());
                return {{"success", false}, {"error", e.what()}};
            }
            return {{"success", false}, {"error", "unsupported or invalid image"}};
        });

        mb.registerHandler("capture.pasteAsPin", [](const nlohmann::json&) -> nlohmann::json {
            auto pin = tools3000::capture::PinWindow::createFromClipboard();
            return {{"success", static_cast<bool>(pin)}};
        });

        mb.registerHandler("capture.cancelAllPinClickThrough", [](const nlohmann::json&) -> nlohmann::json {
            tools3000::capture::PinWindow::cancelAllClickThrough();
            return {{"success", true}};
        });

        mb.registerHandler("capture.togglePinClickThrough", [](const nlohmann::json&) -> nlohmann::json {
            bool toggled = tools3000::capture::PinWindow::toggleClickThroughUnderCursor();
            return {{"success", true}, {"toggled", toggled}};
        });

        mb.registerHandler("history.getAll", [](const nlohmann::json& params) -> nlohmann::json {
            auto result = nlohmann::json::parse(
                tools3000::capture::CaptureHistory::instance().toMetadataJson());
            if (!params.value("includeThumbnails", false)) return result;

            for (auto& metadata : result) {
                const auto entry = tools3000::capture::CaptureHistory::instance().get(
                    metadata.value("index", -1));
                if (!entry || entry->thumbnail.empty()) continue;
                std::vector<uint8_t> buffer;
                if (cv::imencode(".png", entry->thumbnail, buffer)) {
                    metadata["base64"] = tools3000::core::WinUtils::base64Encode(buffer);
                }
            }
            return result;
        });

        mb.registerHandler("history.getThumbnail", [](const nlohmann::json& params) -> nlohmann::json {
            int index = params.value("index", -1);
            if (index < 0) return {{"success", false}};
            const auto entry = tools3000::capture::CaptureHistory::instance().get(index);
            if (!entry || entry->thumbnail.empty()) return {{"success", false}};
            
            std::vector<uint8_t> buffer;
            cv::imencode(".png", entry->thumbnail, buffer);
            std::string base64 = tools3000::core::WinUtils::base64Encode(buffer);
            return {{"success", true}, {"base64", base64}};
        });

        mb.registerHandler("history.clear", [](const nlohmann::json&) -> nlohmann::json {
            tools3000::capture::CaptureHistory::instance().clear();
            return {{"success", true}};
        });

        mb.registerHandler("history.open", [](const nlohmann::json& params) -> nlohmann::json {
            const int index = params.value("index", -1);
            const auto entry = tools3000::capture::CaptureHistory::instance().get(index);
            if (!entry || entry->image.empty()) {
                return {{"success", false}, {"error", "history entry not found"}};
            }
            POINT cursor{};
            GetCursorPos(&cursor);
            const auto& reg = entry->region;
            const tools3000::capture::CaptureRegion* pReg = (reg.width > 0 && reg.height > 0) ? &reg : nullptr;
            POINT spawnPos = tools3000::capture::PinWindow::calculateSmartSpawnPosition(entry->image.cols, entry->image.rows, &cursor, pReg);
            const auto pin = tools3000::capture::PinWindow::create(entry->image, spawnPos.x, spawnPos.y);
            return {{"success", static_cast<bool>(pin)}};
        });

        return true;
    }

    void shutdown() override {
        LOG_INFO("CapturePlugin: 卸载截图/录屏引擎");
        // 先关闭所有外部入口并等待在途回调归零，再停止内部线程和窗口。
        auto& bridge = tools3000::core::MessageBridge::instance();
        bridge.unregisterHandlersByPrefix("capture.");
        bridge.unregisterHandlersByPrefix("recording.");
        bridge.unregisterHandlersByPrefix("ocr.");
        bridge.unregisterHandlersByPrefix("history.");

        auto& hotkeys = tools3000::core::HotkeyManager::instance();
        for (const char* name : {"Screenshot", "Record", "Record Pause", "OCR", "Pin Toggle",
                                 "Pin Paste", "Pin Hide All", "Pin Arrange",
                                 "Mute System Audio", "Mute Microphone"}) {
            hotkeys.unregisterHotkey(name);
        }
        auto& bus = tools3000::core::EventBus::instance();
        bus.unsubscribeAndWait(m_screenshotSubscription);
        bus.unsubscribeAndWait(m_recordingSubscription);
        bus.unsubscribeAndWait(m_themeSubscription);
        m_screenshotSubscription = 0;
        m_recordingSubscription = 0;
        m_themeSubscription = 0;

        g_ocrPending.store(false);
        if (g_ocrWorker.joinable()) {
            tools3000::core::joinWorkerWhilePumpingSentMessages(g_ocrWorker, 3000);
        }
        g_ocrRunning.store(false);
        PinWindow::closeAll();
        RecordingIndicator::instance().onPause(nullptr);
        RecordingIndicator::instance().onStop(nullptr);
        RecordingIndicator::instance().onSystemAudioMute(nullptr);
        RecordingIndicator::instance().onMicrophoneMute(nullptr);
        RecordingIndicator::instance().shutdown();
        ScreenRecorder::instance().shutdown();
        ScreenRecorder::instance().setStateCallback(nullptr);
        ShortcutHintOverlay::instance().shutdown();
        ScreenCapture::instance().shutdown();
        ScreenCapture::instance().setCallback(nullptr);
        tools3000::ocr::OcrResultWindow::instance().cleanup();
        tools3000::ocr::OcrEngine::instance().shutdown();
        shutdownMarkupTextRenderer();
    }

private:
    tools3000::core::SubscriptionId m_screenshotSubscription = 0;
    tools3000::core::SubscriptionId m_recordingSubscription = 0;
    tools3000::core::SubscriptionId m_themeSubscription = 0;
};

} // namespace tools3000::capture

PLUGIN_API tools3000::core::IPlugin* CreatePlugin() {
    static tools3000::capture::CapturePlugin instance;
    return &instance;
}

PLUGIN_API std::uint32_t GetPluginAbiVersion() {
    return tools3000::core::CurrentPluginAbiVersion;
}
