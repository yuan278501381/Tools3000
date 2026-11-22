#include "core/plugin/IPlugin.h"
#include "core/i18n/UiI18n.h"
#include "Tools3000Version.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/utils/WinUtils.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/hotkey/HotkeyPolicy.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/translate/TranslationService.h"
#include "core/utils/UiThreadJoin.h"
#include "capture/ScreenCapture.h"
#include "capture/ClipboardUtils.h"
#include "capture/ScreenRecorder.h"
#include "capture/RecordingIndicator.h"
#include "capture/StudioBackdropCatalog.h"
#include "capture/StudioPreviewWindow.h"
#include "capture/WatermarkRenderer.h"
#include "capture/PinWindow.h"
#include "capture/PinContextMenu.h"
#include "capture/PinGroupBarOverlay.h"
#include "capture/CaptureOverlay.h"
#include "capture/CaptureHistory.h"
#include "capture/TextReplaceEngine.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/ScrollCapture.h"
#include "capture/ScrollCaptureOverlay.h"
#include "common/AtomicFile.h"
#include <opencv2/imgcodecs.hpp>
#include "ocr/OcrEngine.h"
#include "ocr/DeepOcrEngine.h"
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
#include <future>
#include <iterator>
#include <shobjidl.h>
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

BeautyBackgroundType studioThemeFromConfig(const std::string& raw) {
    if (raw == "sunrise_coral" || raw == "paper_chalk") return BeautyBackgroundType::PaperChalk;
    if (raw == "emerald_sea" || raw == "silk_mist") return BeautyBackgroundType::SilkMist;
    if (raw == "obsidian_dark" || raw == "midnight_graphite") return BeautyBackgroundType::MidnightGraphite;
    if (raw == "minimal_light" || raw == "pure_minimal") return BeautyBackgroundType::PureMinimal;
    if (raw == "airy_glacier") return BeautyBackgroundType::AiryGlacier;
    if (raw == "dusk_mountains") return BeautyBackgroundType::DuskMountains;
    if (raw == "misty_lake_dawn") return BeautyBackgroundType::MistyLakeDawn;
    return BeautyBackgroundType::StudioSlate;
}

void startRecordingForRegion(const CaptureRegion& region, const RecordSetupOptions& setup = {});

bool isAutomatedTestSession() {
    wchar_t buf[16]{};
    const DWORD len = GetEnvironmentVariableW(L"TOOLS3000_TEST_MODE", buf, static_cast<DWORD>(std::size(buf)));
    return len > 0 && std::wstring_view(buf, len) == L"1";
}

void ensureStudioStageDefaultsV2(tools3000::core::ConfigManager& config) {
    if (config.get<int>("/recording/studioStageSchemaVersion", 0) >= 2) {
        return;
    }
    if (config.get<double>("/recording/studioInsetBorder", 0.0) <= 0.01) {
        config.set<double>("/recording/studioInsetBorder", 7.0);
    }
    if (config.get<double>("/recording/studioPaddingRatio", 0.070) <= 0.072) {
        config.set<double>("/recording/studioPaddingRatio", 0.092);
    }
    if (config.get<double>("/recording/studioShadow", 0.65) <= 0.66) {
        config.set<double>("/recording/studioShadow", 0.72);
    }
    if (std::abs(config.get<double>("/recording/studioCornerRadius", 16.0) - 16.0) < 0.01) {
        config.set<double>("/recording/studioCornerRadius", 18.0);
    }
    const std::string curTheme = config.get<std::string>("/recording/studioTheme", "");
    if (curTheme.empty() || curTheme == "aurora_purple" || curTheme == "studio_slate") {
        config.set<std::string>("/recording/studioTheme", "blossom_silk");
    }
    config.set<int>("/recording/studioStageSchemaVersion", 2);
}

RecordOptions configuredRecordOptions(const CaptureRegion& region) {
    auto& config = tools3000::core::ConfigManager::instance();
    ensureStudioStageDefaultsV2(config);
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
    options.voiceEnhance = config.get<bool>("/recording/voiceEnhance", true);
    options.hideDesktopIcons = config.get<bool>("/recording/hideDesktopIcons", false);
    options.hideTaskbar = config.get<bool>("/recording/hideTaskbar", false);
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
    options.studioMode = config.get<bool>("/recording/studioMode", true);
    const bool autoZoom = config.get<bool>("/recording/studioAutoZoom", true);
    const int zoomModeInt = std::clamp(config.get<int>("/recording/studioZoomMode", autoZoom ? 1 : 0), 0, 2);
    options.studioConfig.autoZoomEnabled = autoZoom && (zoomModeInt != 0);
    options.studioConfig.zoomMode = (zoomModeInt == 2)
        ? StudioZoomMode::Motion3D
        : (zoomModeInt == 1 ? StudioZoomMode::Zoom2D : StudioZoomMode::None);
    options.studioConfig.typingAutoZoom = config.get<bool>("/recording/typingAutoZoom", false);
    options.studioConfig.defaultZoomFactor = std::clamp(
        static_cast<float>(config.get<double>("/recording/studioZoomFactor", 1.32)), 1.00f, 2.80f);
    options.studioConfig.zoomPacing = std::clamp(
        static_cast<float>(config.get<double>("/recording/studioZoomPacing", 0.32)), 0.0f, 1.0f);
    options.studioConfig.autoZoomSensitivity = std::clamp(
        static_cast<float>(config.get<double>("/recording/studioZoomSensitivity", 0.32)), 0.0f, 1.0f);
    options.studioConfig.focusSpotlightDim = std::clamp(
        static_cast<float>(config.get<double>("/recording/studioFocusSpotlightDim", 0.18)), 0.0f, 0.65f);
    options.studioConfig.lockCameraWhileZoomed =
        config.get<bool>("/recording/studioLockCameraHold", true);
    options.studioConfig.smoothCursorEnabled =
        options.includeCursor && config.get<bool>("/recording/studioSmoothCursor", true);
    options.studioConfig.cursorScale = std::clamp(
        static_cast<float>(config.get<double>("/recording/studioCursorScale", 1.35)), 0.80f, 2.20f);
    options.studioConfig.autoFrameEnabled = config.get<bool>("/recording/studioAutoFrame", true);
    options.studioConfig.paddingRatio = std::clamp(
        static_cast<float>(config.get<double>("/recording/studioPaddingRatio", 0.092)), 0.0f, 0.22f);
    options.studioConfig.cornerRadius = std::clamp(
        static_cast<float>(config.get<double>("/recording/studioCornerRadius", 18.0)), 0.0f, 48.0f);
    options.studioConfig.insetBorderWidth = std::clamp(
        static_cast<float>(config.get<double>("/recording/studioInsetBorder", 7.0)), 0.0f, 16.0f);
    options.studioConfig.insetBorderColorHex =
        config.get<std::string>("/recording/studioInsetBorderColor", "#FFFFFF");
    options.studioConfig.backgroundBlur = std::clamp(
        static_cast<float>(config.get<double>("/recording/studioBgBlur", 0.0)), 0.0f, 40.0f);
    options.studioConfig.shadowIntensity = std::clamp(
        static_cast<float>(config.get<double>("/recording/studioShadow", 0.72)), 0.0f, 1.0f);
    options.studioConfig.motionBlurEnabled = config.get<bool>("/recording/studioMotionBlur", false);
    options.studioConfig.cursorHighlightHalo = config.get<bool>("/recording/studioCursorHalo", false);
    const int clickStyleInt = std::clamp(config.get<int>("/recording/studioClickStyle", 0), 0, 3);
    options.studioConfig.clickEffectStyle = static_cast<StudioClickEffectStyle>(clickStyleInt);
    options.studioConfig.showClickRipple =
        options.showClickEffects && (options.studioConfig.clickEffectStyle != StudioClickEffectStyle::TactileOnly);
    if (options.showClickEffects && options.studioConfig.clickEffectStyle == StudioClickEffectStyle::TactileOnly) {
        options.studioConfig.clickEffectStyle = StudioClickEffectStyle::CrystalPulse;
        options.studioConfig.showClickRipple = true;
    }
    options.studioConfig.customBackgroundPath = config.get<std::string>("/recording/studioCustomBg", "");
    const std::string rawStudioTheme = config.get<std::string>("/recording/studioTheme", "blossom_silk");
    const auto& resolvedBackdrop = StudioBackdropCatalog::resolvePreset(rawStudioTheme, studioThemeFromConfig(rawStudioTheme));
    options.studioConfig.backgroundPresetKey = resolvedBackdrop.key;
    options.studioConfig.backgroundTheme = resolvedBackdrop.legacyTheme;
    options.studioConfig.autoCaptionsEnabled = config.get<bool>("/recording/studioAutoCaptions", false);
    options.studioConfig.watermark = WatermarkRenderer::loadStudioWatermarkConfig();
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
                if (ScreenRecorder::instance().isStopping()) {
                    // 后台异步封装落盘正在进行中，保持灵动岛 SAVING 状态直至回调切回完成态
                    return;
                }
                if (!stats.stopReason.empty() &&
                    ScreenRecorder::instance().needsFinalization()) {
                    indicator.showSaving();
                    const std::string stopReason = stats.stopReason;
                    const double fallbackDur = stats.durationSec;
                    ScreenRecorder::instance().stopRecordingAsync(
                        [stopReason, fallbackDur](const std::string& path, const RecordStats& finalStats) {
                            tools3000::core::MainThreadDispatcher::instance().post(
                                [path, stopReason, dur = (finalStats.durationSec > 0.0 ? finalStats.durationSec : fallbackDur)]() {
                                    auto& ind = RecordingIndicator::instance();
                                    const bool isZh = tools3000::core::isUiLanguageChinese();
                                    if (!path.empty()) {
                                        ind.showCompleted(path, dur);
                                        const bool isWeChatLimit = stopReason == "gif_wechat_limit";
                                        const bool diskFailure = stopReason == "low_disk_space" ||
                                            stopReason == "disk_space_query_failed";
                                        std::wstring toastMsg = isWeChatLimit
                                            ? (tools3000::core::trWCstrFor(isZh, L"GIF 已达 20MB 上限并自动保存", L"GIF reached 20 MB limit and saved"))
                                            : (diskFailure
                                                ? (tools3000::core::trWCstrFor(isZh, L"磁盘空间不足，已保存当前录屏", L"Low disk space, recording saved"))
                                                : (tools3000::core::trWCstrFor(isZh, L"录屏已停止并保存", L"Recording stopped and saved")));
                                        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{toastMsg});
                                    } else {
                                        ind.hide();
                                        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                                            tools3000::core::trWCstrFor(isZh, L"录屏已停止，未生成有效内容", L"Recording stopped, no content saved")});
                                    }
                                });
                        });
                } else if (!indicator.isCompleted() && !indicator.isSaving()) {
                    indicator.hide();
                }
            }
            else {
                indicator.setPaused(state == RecordState::Paused);
                indicator.update(stats.durationSec, stats.frameCount);
                if (state == RecordState::Paused) {
                    ShortcutHintOverlay::instance().show(
                        ShortcutHintContext::RecordingPaused,
                        {LONG_MIN, LONG_MIN},
                        indicator.avoidanceRects());
                } else if (state == RecordState::Countdown) {
                    ShortcutHintOverlay::instance().show(
                        ShortcutHintContext::Recording,
                        {LONG_MIN, LONG_MIN},
                        indicator.avoidanceRects());
                    indicator.update(stats.durationSec, stats.frameCount);
                } else {
                    // 正式录制期间立即收起左下角快捷键提示浮层，严防录进全屏底片
                    ShortcutHintOverlay::instance().hide();
                }
            }
        });
    });
}

void stopRecordingWithFeedback() {
    auto& recorder = ScreenRecorder::instance();
    if (recorder.isStopping()) {
        return;
    }
    const double duration = recorder.stats().durationSec;
    const bool willOpenStudio = recorder.wasLastSessionStudioMode() && !isAutomatedTestSession();

    // 第一优先级：微秒级原子冻结桌面与音频采样并等待在途单帧退出，确保后续任何界面切换 0% 混入录屏母带
    recorder.freezeCaptureImmediately();

    auto& indicator = RecordingIndicator::instance();
    ShortcutHintOverlay::instance().hide();
    tools3000::core::MessageBridge::instance().invokeHandler("keycast.setDockingRegion", {{"active", false}});

    // 停止封装期间仅保留轻量级顶部状态胶囊（隐藏全屏等待过渡层，彻底杜绝遮挡屏幕或未就绪窗口闪烁）
    indicator.showSaving();

    recorder.stopRecordingAsync([duration, willOpenStudio](const std::string& path, const RecordStats& finalStats) {
        // 超长录屏无阻塞预处理：在后台收尾工作线程预先备齐预览帧牌组与声学波形，保障 UI 主线程 0ms 无缝切入演播室
        std::vector<cv::Mat> preparedDeck;
        StudioTimelineData preparedTimeline;
        StudioRenderConfig preparedCfg{};
        int preparedBitrate = 10;
        bool openStudioReady = false;

        if (!path.empty()) {
            auto& rec = ScreenRecorder::instance();
            preparedDeck = rec.takeLastStudioPreviewDeck();
            if (willOpenStudio && rec.wasLastSessionStudioMode() && !isAutomatedTestSession()) {
                preparedTimeline = rec.takeLastStudioTimeline();
                preparedCfg = rec.lastStudioConfig();
                preparedBitrate = rec.lastSessionBitrateMbps();
                if (preparedDeck.empty()) {
                    int ow = preparedTimeline.sourceWidth;
                    int oh = preparedTimeline.sourceHeight;
                    double dSec = preparedTimeline.durationMs / 1000.0;
                    StudioVideoCompositor::extractPreviewDeck(
                        path, kStudioProxyMaxLongSide, kStudioProxyMaxFrames, preparedDeck, ow, oh, dSec);
                    if (preparedTimeline.sourceWidth <= 0) preparedTimeline.sourceWidth = ow;
                    if (preparedTimeline.sourceHeight <= 0) preparedTimeline.sourceHeight = oh;
                    if (preparedTimeline.durationMs <= 0 && dSec > 0.0) {
                        preparedTimeline.durationMs = static_cast<int64_t>(std::llround(dSec * 1000.0));
                    }
                }
                if (preparedTimeline.audioWaveformPeaks.empty()) {
                    StudioVideoCompositor::extractAudioWaveformPeaks(path, 4096, preparedTimeline.audioWaveformPeaks);
                }
                openStudioReady = true;
            } else {
                preparedDeck.clear();
                preparedDeck.shrink_to_fit();
                rec.clearStudioTransientData();
            }
        } else {
            ScreenRecorder::instance().clearStudioTransientData();
        }

        tools3000::core::MainThreadDispatcher::instance().post(
            [path,
             dur = (finalStats.durationSec > 0.0 ? finalStats.durationSec : duration),
             openStudioReady,
             deck = std::move(preparedDeck),
             timeline = std::move(preparedTimeline),
             preparedCfg,
             preparedBitrate]() mutable {
                auto& ind = RecordingIndicator::instance();
                const bool isZh = tools3000::core::isUiLanguageChinese();
                if (!path.empty()) {
                    LOG_INFO("录屏已保存: {}", path);
                    if (openStudioReady) {
                        ind.hide();
                        StudioPreviewWindow::instance().showSession(
                            path,
                            std::move(timeline),
                            preparedCfg,
                            preparedBitrate,
                            [](const std::string& savedPath, double savedDur, bool studioExported) {
                                const bool zh = tools3000::core::isUiLanguageChinese();
                                if (savedPath.empty()) {
                                    RecordingIndicator::instance().hide();
                                    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                                        tools3000::core::trWCstrFor(zh, L"已不保存退出，未保留本次录屏", L"Closed without saving recording")});
                                    return;
                                }
                                RecordingIndicator::instance().showCompleted(savedPath, savedDur);
                                tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                                    studioExported
                                        ? (tools3000::core::trWCstrFor(zh, L"演播室成片已导出并保存", L"Studio export completed and saved"))
                                        : (tools3000::core::trWCstrFor(zh, L"录屏已保存", L"Recording saved"))});
                            },
                            std::move(deck));
                        return;
                    }
                    StudioPreviewWindow::instance().dismissPreparingOverlay();
                    ind.showCompleted(path, dur);
                    tools3000::core::EventBus::instance().publish(
                        tools3000::core::ShowToastEvent{tools3000::core::trWCstrFor(isZh, L"录屏已保存", L"Recording saved")});
                } else {
                    StudioPreviewWindow::instance().dismissPreparingOverlay();
                    ind.hide();
                    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                        tools3000::core::trWCstrFor(isZh, L"录屏已停止，未生成有效内容", L"Recording stopped, no content saved")});
                }
            });
    });
}

bool toggleRecordingPauseWithFeedback() {
    auto& recorder = ScreenRecorder::instance();
    const bool isZh = tools3000::core::isUiLanguageChinese();
    if (recorder.state() == RecordState::Recording) {
        recorder.pauseRecording();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{tools3000::core::trWCstrFor(isZh, L"录屏已暂停", L"Recording paused")});
        return true;
    }
    if (recorder.state() == RecordState::Paused) {
        recorder.resumeRecording();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{tools3000::core::trWCstrFor(isZh, L"录屏已继续", L"Recording resumed")});
        return true;
    }
    return false;
}

void toggleSystemAudioMuteWithFeedback() {
    auto& recorder = ScreenRecorder::instance();
    if (!recorder.toggleSystemAudioMuted()) return;
    const auto muted = recorder.stats().systemAudioMuted;
    const bool isZh = tools3000::core::isUiLanguageChinese();
    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
        muted
            ? (tools3000::core::trWCstrFor(isZh, L"系统声音已静音", L"System audio muted"))
            : (tools3000::core::trWCstrFor(isZh, L"系统声音已恢复", L"System audio unmuted"))});
}

void toggleMicrophoneMuteWithFeedback() {
    auto& recorder = ScreenRecorder::instance();
    if (!recorder.toggleMicrophoneMuted()) return;
    const auto muted = recorder.stats().microphoneMuted;
    const bool isZh = tools3000::core::isUiLanguageChinese();
    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
        muted
            ? (tools3000::core::trWCstrFor(isZh, L"麦克风已静音", L"Microphone muted"))
            : (tools3000::core::trWCstrFor(isZh, L"麦克风已恢复", L"Microphone unmuted"))});
}

void startRecordingForRegion(const CaptureRegion& rawRegion, const RecordSetupOptions& setup) {
    // 若后台已处于倒计时或录制状态，严禁覆盖当前录制的 Keycast 停靠区，立即刷新置顶录制控制条并给出明确操作指引
    if (ScreenRecorder::instance().state() != RecordState::Idle ||
        ScreenRecorder::instance().isStopping()) {
        RecordingIndicator::instance().refreshCaptureAffinity();
        if (HWND indHwnd = FindWindowW(L"Tools3000_RecordingIndicator", nullptr)) {
            SetWindowPos(indHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
        const bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::trWCstrFor(isZh, L"正在录屏中，请点击顶部红色停止按钮或按 Ctrl+Shift+R 结束保存", L"Recording in progress — click the red Stop button or press Ctrl+Shift+R")});
        return;
    }

    // 录屏开始前统一将选区宽高对齐偶像素 (>= 2)，消除编码器偶像素裁切导致的与外部呼吸红框/Keycast 停靠区 1px 偏差
    CaptureRegion region = rawRegion;
    if (region.width > 1) region.width &= ~1;
    if (region.height > 1) region.height &= ~1;

    configureRecorderStateCallback();
    auto options = configuredRecordOptions(region);
    options.format = setup.format;
    options.fps = setup.fps;
    if (setup.bitrateMbps > 0) {
        options.bitrateMbps = std::clamp(setup.bitrateMbps, 1, 100);
    }
    options.showClickEffects = setup.showClickEffects;
    options.showClickZoom = setup.showClickZoom;
    options.studioConfig.showClickRipple = setup.showClickEffects;
    if (setup.showClickEffects && options.studioConfig.clickEffectStyle == StudioClickEffectStyle::TactileOnly) {
        options.studioConfig.clickEffectStyle = StudioClickEffectStyle::CrystalPulse;
    }
    options.includeKeycast = setup.includeKeycast;
    options.captureSystemAudio = setup.captureSystemAudio && setup.format != RecordFormat::GIF;
    options.captureMicrophone = setup.captureMicrophone && setup.format != RecordFormat::GIF;
    options.microphoneDeviceId = setup.microphoneDeviceId;
    options.voiceEnhance = setup.voiceEnhance;
    options.countdownSeconds = std::clamp(setup.countdownSeconds, 0, 10);
    options.hideDesktopIcons = setup.hideDesktopIcons;
    options.hideTaskbar = setup.hideTaskbar;
    options.studioMode = setup.studioMode;
    options.studioConfig.typingAutoZoom = setup.studioMode && setup.typingAutoZoom;
    options.studioConfig.autoZoomEnabled = setup.studioMode && (setup.zoomEffectMode != 0);
    options.studioConfig.zoomMode = !setup.studioMode ? StudioZoomMode::None
        : ((setup.zoomEffectMode == 2)
            ? StudioZoomMode::Motion3D
            : (setup.zoomEffectMode == 1 ? StudioZoomMode::Zoom2D : StudioZoomMode::None));
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
        if (options.countdownSeconds > 0) {
            ShortcutHintOverlay::instance().show(
                ShortcutHintContext::Recording,
                {LONG_MIN, LONG_MIN},
                RecordingIndicator::instance().avoidanceRects());
        } else {
            ShortcutHintOverlay::instance().hide();
        }
    } else {
        tools3000::core::MessageBridge::instance().invokeHandler("keycast.setDockingRegion", {{"active", false}});
        const auto reason = ScreenRecorder::instance().stats().stopReason;
        const bool isZh = tools3000::core::isUiLanguageChinese();
        const wchar_t* message = (reason == "output_directory_unavailable" ||
                                  reason == "output_directory_not_writable")
            ? (tools3000::core::trWCstrFor(isZh, L"无法写入录屏保存目录", L"Cannot write to recording output directory"))
            : reason == "insufficient_disk_space"
                ? (tools3000::core::trWCstrFor(isZh, L"剩余空间不足，无法开始录屏", L"Insufficient disk space to start recording"))
                : reason == "disk_space_query_failed"
                    ? (tools3000::core::trWCstrFor(isZh, L"无法检查保存目录的剩余空间", L"Failed to check available disk space"))
                    : (tools3000::core::trWCstrFor(isZh, L"无法开始录屏，请查看日志", L"Failed to start recording, check logs"));
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{message});
    }
}

void toggleRecording() {
    auto& recorder = ScreenRecorder::instance();
    if (recorder.isStopping()) {
        return;
    }
    if (recorder.state() == RecordState::Idle) {
        const auto opts = configuredCaptureOptions();
        if (opts.autoBypassFullscreen) {
            HWND fg = GetForegroundWindow();
            if (fg && tools3000::core::WinUtils::shouldBypassCaptureForWindow(fg)) {
                return;
            }
        }
        auto& overlay = CaptureOverlay::instance();
        overlay.setRecordCallback(startRecordingForRegion);
        overlay.startSelection(opts, OverlayMode::RecordRegion);
    } else {
        // Global hotkey and tray action are start/stop. Pause/resume remains an
        // explicit control on the always-visible recording indicator.
        stopRecordingWithFeedback();
    }
}

bool loadHistoryImageAndMeta(uint64_t id, int index, const std::string& fallbackPath,
                             cv::Mat& outImage, std::string& outFilePath,
                             CaptureRegion& outRegion) {
    if (id > 0) {
        if (const auto entry = CaptureHistory::instance().getById(id)) {
            if (!entry->image.empty()) {
                outImage = entry->image;
                outFilePath = !entry->filePath.empty() ? entry->filePath : fallbackPath;
                outRegion = entry->region;
                return true;
            }
        }
    }
    if (index >= 0) {
        if (const auto entry = CaptureHistory::instance().get(index)) {
            if (!entry->image.empty()) {
                outImage = entry->image;
                outFilePath = !entry->filePath.empty() ? entry->filePath : fallbackPath;
                outRegion = entry->region;
                return true;
            }
        }
    }
    if (!fallbackPath.empty()) {
        try {
            const auto filePath = std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(fallbackPath));
            std::ifstream stream(filePath, std::ios::binary);
            if (stream) {
                std::vector<unsigned char> bytes(
                    (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                outImage = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
                if (!outImage.empty()) {
                    outFilePath = fallbackPath;
                    outRegion.width = outImage.cols;
                    outRegion.height = outImage.rows;
                    return true;
                }
            }
        } catch (...) {
        }
    }
    return false;
}

bool encodeAndWriteImageFile(const cv::Mat& image, const std::filesystem::path& targetPath, int quality) {
    if (image.empty() || targetPath.empty()) return false;
    std::wstring ext = tools3000::core::WinUtils::toLower(targetPath.extension().wstring());
    std::vector<uint8_t> encoded;
    if (ext == L".jpg" || ext == L".jpeg") {
        cv::Mat bgr = image;
        if (image.channels() == 4) cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
        cv::imencode(".jpg", bgr, encoded, {cv::IMWRITE_JPEG_QUALITY, std::clamp(quality, 1, 100)});
    } else if (ext == L".webp") {
        cv::imencode(".webp", image, encoded, {cv::IMWRITE_WEBP_QUALITY, std::clamp(quality, 1, 100)});
    } else if (ext == L".bmp") {
        cv::imencode(".bmp", image, encoded);
    } else {
        cv::imencode(".png", image, encoded, {cv::IMWRITE_PNG_COMPRESSION, 6});
    }
    if (encoded.empty()) return false;
    if (const auto parent = targetPath.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    return tools3000::common::atomicWriteBinaryFileWithFlush(
        targetPath.wstring(), encoded.data(), encoded.size());
}

}  // namespace

static std::atomic<bool> g_ocrPending{false};
static std::atomic<bool> g_ocrRunning{false};
static std::atomic<uint64_t> g_ocrTaskGeneration{0};
static std::jthread g_ocrWorker;

static void releaseOcrTransientMemory() {
    tools3000::core::WinUtils::trimWorkingSet();
}

static void startMemoryOcrTask(const cv::Mat& capturedImage, const CaptureRegion* capturedRegion = nullptr) {
    if (capturedImage.empty()) return;
    const bool isZh = tools3000::core::isUiLanguageChinese();
    const uint64_t myGen = g_ocrTaskGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_ocrRunning.store(true, std::memory_order_release);

    const bool hasAnchor = (capturedRegion != nullptr && capturedRegion->width > 0 && capturedRegion->height > 0);
    const RECT anchorRc = hasAnchor
        ? RECT{capturedRegion->x, capturedRegion->y, capturedRegion->x + capturedRegion->width, capturedRegion->y + capturedRegion->height}
        : RECT{};

    const bool showResultWindow = tools3000::core::ConfigManager::instance().get<bool>(
        "/ocr/showResultWindow", true);
    if (showResultWindow) {
        tools3000::core::MainThreadDispatcher::instance().post([isZh, hasAnchor, anchorRc]() {
            tools3000::ocr::OcrResultWindow::instance().showResult(
                tools3000::core::trUtf8CstrFor(isZh, "正在识别...", "Recognizing..."),
                hasAnchor ? &anchorRc : nullptr);
        });
    } else {
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::trWCstrFor(isZh, L"正在识别...", L"Recognizing...")});
    }

    cv::Mat img = capturedImage.clone();
    std::jthread prevWorker = std::move(g_ocrWorker);
    g_ocrWorker = std::jthread([prev = std::move(prevWorker), img = std::move(img), myGen, hasAnchor, anchorRc]() mutable {
        if (prev.joinable()) {
            prev.join();
        }
        if (g_ocrTaskGeneration.load(std::memory_order_acquire) != myGen) {
            return;
        }
        const bool zh = tools3000::core::isUiLanguageChinese();
        try {
            auto results = tools3000::ocr::OcrEngine::instance().extractText(img);
            if (g_ocrTaskGeneration.load(std::memory_order_acquire) != myGen) {
                return;
            }
            std::string fullText;
            for (const auto& r : results) {
                fullText += r.text + "\r\n";
            }
            if (fullText.empty()) {
                if (tools3000::core::ConfigManager::instance().get<bool>("/ocr/showResultWindow", true)) {
                    tools3000::core::MainThreadDispatcher::instance().post([zh, hasAnchor, anchorRc]() {
                        tools3000::ocr::OcrResultWindow::instance().showResult(
                            tools3000::core::trUtf8CstrFor(zh, "未识别到文字", "No text recognized"),
                            hasAnchor ? &anchorRc : nullptr);
                    });
                } else {
                    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                        tools3000::core::trWCstrFor(zh, L"未识别到文字", L"No text recognized")});
                }
            } else {
                const bool copy = tools3000::core::ConfigManager::instance().get<bool>("/ocr/copyResult", true);
                const bool showResult = tools3000::core::ConfigManager::instance().get<bool>("/ocr/showResultWindow", true);
                if (copy) {
                    tools3000::core::WinUtils::copyToClipboard(fullText);
                }
                if (showResult) {
                    tools3000::core::MainThreadDispatcher::instance().post([text = std::move(fullText), hasAnchor, anchorRc]() {
                        tools3000::ocr::OcrResultWindow::instance().showResult(
                            text, hasAnchor ? &anchorRc : nullptr);
                    });
                } else {
                    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                        copy
                            ? (tools3000::core::trWCstrFor(zh, L"文字已复制到剪贴板", L"Text copied to clipboard"))
                            : (tools3000::core::trWCstrFor(zh, L"文字识别完成", L"Text recognition completed"))});
                    releaseOcrTransientMemory();
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("OCR 后台线程异常: {}", e.what());
            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                tools3000::core::trWCstrFor(zh, L"OCR 识别失败", L"OCR recognition failed")});
        } catch (...) {
            LOG_ERROR("OCR 后台线程未知异常");
            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                tools3000::core::trWCstrFor(zh, L"OCR 识别失败", L"OCR recognition failed")});
        }
        if (g_ocrTaskGeneration.load(std::memory_order_acquire) == myGen) {
            g_ocrRunning.store(false, std::memory_order_release);
        }
    });
}

static void onCaptureCompletedForOcr(const tools3000::capture::CaptureResult& result) {
    if (!g_ocrPending.exchange(false)) return;
    if (!result.success || result.filePath.empty()) return;

    const bool isZh = tools3000::core::isUiLanguageChinese();
    std::string path = result.filePath;
    if (g_ocrRunning.exchange(true)) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::trWCstrFor(isZh, L"OCR 正在识别，请稍候", L"OCR recognition in progress, please wait")});
        return;
    }
    if (!tools3000::core::ConfigManager::instance().get<bool>("/ocr/showResultWindow", true)) {
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::trWCstrFor(isZh, L"正在识别...", L"Recognizing...")});
    }
    if (g_ocrWorker.joinable()) g_ocrWorker.join();
    g_ocrWorker = std::jthread([path]() {
        const bool zh = tools3000::core::isUiLanguageChinese();
        try {
            std::string text = tools3000::ocr::OcrEngine::instance().recognizeImageFile(path);
            if (text.empty()) {
                tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                    tools3000::core::trWCstrFor(zh, L"未识别到文字", L"No text recognized")});
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
                        copy
                            ? (tools3000::core::trWCstrFor(zh, L"文字已复制到剪贴板", L"Text copied to clipboard"))
                            : (tools3000::core::trWCstrFor(zh, L"文字识别完成", L"Text recognition completed"))});
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("OCR 后台线程异常: {}", e.what());
            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                tools3000::core::trWCstrFor(zh, L"OCR 识别失败", L"OCR recognition failed")});
        } catch (...) {
            LOG_ERROR("OCR 后台线程未知异常");
            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                tools3000::core::trWCstrFor(zh, L"OCR 识别失败", L"OCR recognition failed")});
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
        g_ocrRunning.store(false);
    });
}


static void triggerOcrCapture() {
    auto opts = configuredCaptureOptions();
    if (opts.autoBypassFullscreen) {
        HWND fg = GetForegroundWindow();
        if (fg && tools3000::core::WinUtils::shouldBypassCaptureForWindow(fg)) {
            return;
        }
    }
    auto& ocr = tools3000::ocr::OcrEngine::instance();
    if (!ocr.isAvailable()) {
        const bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::trWCstrFor(isZh, L"未找到 OCR 语言包", L"No OCR language pack installed")});
        return;
    }
    LOG_INFO("OCR Capture Triggered");
    // 纯内存直达识别：零截图剪贴板污染、零临时文件落盘、零幽灵历史记录
    opts.copyToClipboard = false;
    opts.saveToFile = false;
    opts.savePath.clear();
    opts.recordHistory = false;
    opts.onImageCaptured = [](const cv::Mat& image, const CaptureRegion& reg) {
        startMemoryOcrTask(image, &reg);
    };
    g_ocrPending.store(false);
    tools3000::capture::ScreenCapture::instance().startCapture(opts);
}

class CapturePlugin : public tools3000::core::IPlugin {
public:
    const char* getName() const override { return "Capture"; }
    const char* getVersion() const override { return tools3000::version::String; }

    bool initialize() override {
        LOG_INFO("CapturePlugin: 初始化截图/录屏引擎");
        PinWindow::resetModuleShutdown();

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
        if (tools3000::core::ConfigManager::instance().get<bool>("/ocr/autoDownloadPackOnStartup", false)) {
            tools3000::core::ConfigManager::instance().set<bool>("/ocr/autoDownloadPackOnStartup", false);
            if (!tools3000::ocr::DeepOcrEngine::instance().isModelInstalled()) {
                LOG_INFO("CapturePlugin: starting background PP-OCRv4 offline pack download requested by installer");
                tools3000::ocr::DeepOcrEngine::instance().startModelDownload();
            }
        }
        // OCR 模型改为首次识字按需懒加载 + 60s 闲置自动卸载，彻底避免开机空跑预占 DirectML 显存与私有提交内存

        
        auto& mb = tools3000::core::MessageBridge::instance();
        auto& bus = tools3000::core::EventBus::instance();

        auto runOnUiThread = [](tools3000::core::MainThreadDispatcher::Task task) {
            auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
            if (dispatcher.isInitialized()) {
                dispatcher.post(std::move(task));
            } else if (task) {
                task();
            }
        };

        auto callOnUiThread = [](std::function<nlohmann::json()> task) -> nlohmann::json {
            if (!task) return {{"success", false}, {"error", "empty UI task"}};
            auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
            if (!dispatcher.isInitialized() || dispatcher.isOwnerThread()) {
                return task();
            }
            auto promise = std::make_shared<std::promise<nlohmann::json>>();
            auto future = promise->get_future();
            if (!dispatcher.post([promise, fn = std::move(task)]() mutable {
                    try {
                        promise->set_value(fn());
                    } catch (const std::exception& e) {
                        promise->set_value(nlohmann::json{{"success", false}, {"error", e.what()}});
                    } catch (...) {
                        promise->set_value(nlohmann::json{{"success", false}, {"error", "UI thread task failed"}});
                    }
                })) {
                return {{"success", false}, {"error", "UI dispatcher unavailable"}};
            }
            if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
                return {{"success", false}, {"error", "UI thread task timed out"}};
            }
            return future.get();
        };

        // 订阅 EventBus 上的托盘/快捷键触发事件
        m_screenshotSubscription = bus.subscribe<tools3000::core::ActionTriggerScreenshotEvent>([runOnUiThread](const tools3000::core::ActionTriggerScreenshotEvent&) {
            runOnUiThread([]() {
                g_ocrPending.store(false);
                tools3000::capture::ScreenCapture::instance().startCapture(configuredCaptureOptions());
            });
        });

        m_recordingSubscription = bus.subscribe<tools3000::core::ActionToggleRecordingEvent>([runOnUiThread](const tools3000::core::ActionToggleRecordingEvent&) {
            runOnUiThread([]() {
                toggleRecording();
            });
        });

        auto refreshCaptureAndOcrThemesOnUi = []() {
            tools3000::capture::CaptureOverlay::instance().reloadThemeColors();
            tools3000::capture::PinWindow::refreshAllVisualsAndGroupBar();
            tools3000::capture::RecordingIndicator::instance().reloadTheme();
            tools3000::capture::StudioPreviewWindow::instance().reloadTheme();
            tools3000::capture::ScrollCaptureOverlay::instance().reloadTheme();
            tools3000::capture::ShortcutHintOverlay::instance().reloadTheme();
            tools3000::ocr::OcrResultWindow::instance().reloadTheme();
        };

        m_themeSubscription = bus.subscribe<tools3000::core::ThemeChangedEvent>([runOnUiThread, refreshCaptureAndOcrThemesOnUi](const tools3000::core::ThemeChangedEvent&) {
            runOnUiThread(refreshCaptureAndOcrThemesOnUi);
        });
        PinGroupPalette::ensureLoadedFromConfig();
        m_configChangeId = tools3000::core::ConfigManager::instance().onChange([runOnUiThread, refreshCaptureAndOcrThemesOnUi](const std::string& key) {
            if (key == "*" || key.starts_with("/capture/pinGroups")) {
                PinGroupPalette::reloadFromConfig();
                runOnUiThread([]() {
                    PinWindow::refreshAllVisualsAndGroupBar();
                });
            }
            if (key == "*" || key == "/general/theme" || key == "/general/accentColor") {
                runOnUiThread(refreshCaptureAndOcrThemesOnUi);
            }
        });
        
        mb.registerHandler("capture.triggerScreenshot", [runOnUiThread](const nlohmann::json&) -> nlohmann::json {
            runOnUiThread([]() {
                g_ocrPending.store(false);
                tools3000::capture::ScreenCapture::instance().startCapture(configuredCaptureOptions());
            });
            return {{"success", true}};
        });

        mb.registerHandler("capture.toggleRecording", [runOnUiThread](const nlohmann::json&) -> nlohmann::json {
            runOnUiThread([]() {
                toggleRecording();
            });
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
        hotkeys.registerHotkey("Screenshot", configuredHotkey("Screenshot", {tools3000::core::ModKey::None, VK_F1}), []() {
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
        hotkeys.registerHotkey("Pin Paste", configuredHotkey("Pin Paste", {tools3000::core::ModKey::None, VK_F3}), []() {
            tools3000::capture::PinWindow::pasteNextHistoryOrClipboard();
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
            const auto wmCfg = tools3000::capture::WatermarkRenderer::loadGlobalCaptureConfig();
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
                {"beautyShellRadius", config.get<int>("/capture/beautyShellRadius", 16)},
                {"watermarkEnabled", wmCfg.enabled},
                {"watermarkMode", static_cast<int>(wmCfg.mode)},
                {"watermarkImagePath", wmCfg.imagePathUtf8},
                {"watermarkText", wmCfg.textTemplate},
                {"watermarkAnchor", static_cast<int>(wmCfg.anchor)},
                {"watermarkScale", static_cast<double>(wmCfg.scale)},
                {"watermarkOpacity", static_cast<double>(wmCfg.opacity)},
                {"watermarkPolicyForced", wmCfg.policyForced},
                {"sizeUnit", config.get<std::string>("/capture/sizeUnit", "px")},
                {"showSizePosition", config.get<bool>("/capture/showSizePosition", true)},
                {"showSizeUnit", config.get<bool>("/capture/showSizeUnit", true)}
            };
        });

        mb.registerHandler("capture.pickWatermarkImage", [](const nlohmann::json&) -> nlohmann::json {
            struct ComStaGuard {
                HRESULT hr;
                ComStaGuard() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
                ~ComStaGuard() { if (hr == S_OK || hr == S_FALSE) CoUninitialize(); }
            } comGuard;

            IFileOpenDialog* dlg = nullptr;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
                return {{"success", false}, {"error", "failed to open file dialog"}};
            }
            const bool isZh = tools3000::core::isUiLanguageChinese();
            const COMDLG_FILTERSPEC spec[] = {
                {tools3000::core::trWCstrFor(isZh, L"图片文件 (*.png;*.webp;*.jpg;*.jpeg;*.bmp)", L"Image Files (*.png;*.webp;*.jpg;*.jpeg;*.bmp)"),
                 L"*.png;*.webp;*.jpg;*.jpeg;*.bmp"},
                {tools3000::core::trWCstrFor(isZh, L"所有文件 (*.*)", L"All Files (*.*)"), L"*.*"}
            };
            dlg->SetFileTypes(static_cast<UINT>(std::size(spec)), spec);
            dlg->SetTitle(tools3000::core::trWCstrFor(isZh, L"选择水印图片 / 企业 Logo", L"Select Watermark Logo Image"));
            DWORD fos = 0;
            if (SUCCEEDED(dlg->GetOptions(&fos))) {
                dlg->SetOptions(fos | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
            }
            std::string chosenUtf8;
            const HRESULT showHr = dlg->Show(GetForegroundWindow());
            if (SUCCEEDED(showHr)) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                    PWSTR rawPath = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath) {
                        chosenUtf8 = tools3000::core::WinUtils::wstringToUtf8(rawPath);
                        CoTaskMemFree(rawPath);
                    }
                    item->Release();
                }
            }
            dlg->Release();
            if (chosenUtf8.empty()) {
                return {{"success", false}, {"cancelled", true}};
            }
            tools3000::capture::WatermarkRenderer::pushStudioRecentLogoPath(chosenUtf8);
            return {{"success", true}, {"path", chosenUtf8}};
        });

        mb.registerHandler("capture.updateSettings", [runOnUiThread](const nlohmann::json& params) -> nlohmann::json {
            static const std::unordered_set<std::string> formats = {"png", "jpg", "jpeg", "webp", "bmp"};
            static const std::unordered_set<std::string> themes = {
                "studio_slate", "paper_chalk", "silk_mist", "midnight_graphite", "pure_minimal",
                "aurora_purple", "sunrise_coral", "emerald_sea", "obsidian_dark", "minimal_light",
                "airy_glacier", "dusk_mountains", "misty_lake_dawn"
            };
            static const std::unordered_set<std::string> sizeUnits = {"px", "dp"};
            static const std::unordered_set<std::string> boolKeys = {
                "saveToFile", "copyToClipboard", "includeCursor", "showCrosshair", "autoDetectWindow",
                "showShortcutHints", "autoBypassFullscreen", "beautyShellEnabled",
                "watermarkEnabled", "showSizePosition", "showSizeUnit"
            };
            if (!params.is_object() || params.empty()) return {{"success", false}, {"error", "no settings supplied"}};
            const auto currentWm = tools3000::capture::WatermarkRenderer::loadGlobalCaptureConfig();
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
                if (key == "watermarkEnabled" && currentWm.policyForced && value.is_boolean() && !value.get<bool>())
                    return {{"success", false}, {"error", "watermark is enforced by enterprise policy"}};
                if (key == "watermarkMode" && (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 2))
                    return {{"success", false}, {"error", "invalid watermark mode"}};
                if (key == "watermarkAnchor" && (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 5))
                    return {{"success", false}, {"error", "invalid watermark anchor"}};
                if (key == "watermarkImagePath" && (!value.is_string() || value.get_ref<const std::string&>().size() > 32767))
                    return {{"success", false}, {"error", "invalid watermark image path"}};
                if (key == "watermarkText" && (!value.is_string() || value.get_ref<const std::string&>().size() > 512))
                    return {{"success", false}, {"error", "invalid watermark text"}};
                if (key == "watermarkScale" && (!value.is_number() || value.get<double>() < 0.30 || value.get<double>() > 3.0))
                    return {{"success", false}, {"error", "invalid watermark scale"}};
                if (key == "watermarkOpacity" && (!value.is_number() || value.get<double>() < 0.03 || value.get<double>() > 1.0))
                    return {{"success", false}, {"error", "invalid watermark opacity"}};
                if (key == "sizeUnit" && (!value.is_string() || !sizeUnits.contains(value.get<std::string>())))
                    return {{"success", false}, {"error", "invalid size unit"}};
                if (boolKeys.contains(key) && !value.is_boolean())
                    return {{"success", false}, {"error", key + " must be boolean"}};
                if (key != "format" && key != "quality" && key != "saveDirectory" &&
                    key != "beautyShellTheme" && key != "beautyShellPadding" && key != "beautyShellRadius" &&
                    key != "watermarkMode" && key != "watermarkAnchor" && key != "watermarkImagePath" &&
                    key != "watermarkText" && key != "watermarkScale" && key != "watermarkOpacity" &&
                    key != "sizeUnit" && !boolKeys.contains(key))
                    return {{"success", false}, {"error", "unsupported setting: " + key}};
            }
            const bool saved = tools3000::core::ConfigManager::instance().mergePatch(
                {{"capture", params}}, "/capture");
            if (saved && params.contains("showShortcutHints")) {
                const bool enabled = params.at("showShortcutHints").get<bool>();
                runOnUiThread([enabled]() {
                    tools3000::capture::CaptureOverlay::instance().setShortcutHintsEnabled(enabled);
                    if (!enabled) {
                        ShortcutHintOverlay::instance().hide();
                    } else if (ScrollCapture::instance().isRunning()) {
                        ShortcutHintOverlay::instance().hide();
                    } else {
                        const auto recordState = ScreenRecorder::instance().state();
                        if (recordState == RecordState::Recording || recordState == RecordState::Paused) {
                            ShortcutHintOverlay::instance().show(
                                recordState == RecordState::Paused
                                    ? ShortcutHintContext::RecordingPaused
                                    : ShortcutHintContext::Recording,
                                {LONG_MIN, LONG_MIN},
                                RecordingIndicator::instance().avoidanceRects());
                        }
                    }
                });
            }
            return {{"success", saved}, {"error", saved ? "" : "failed to persist settings"}};
        });

        mb.registerHandler("recording.getSettings", [](const nlohmann::json&) -> nlohmann::json {
            auto& config = tools3000::core::ConfigManager::instance();
            ensureStudioStageDefaultsV2(config);
            const auto studioWm = tools3000::capture::WatermarkRenderer::loadStudioWatermarkConfig();
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
                {"voiceEnhance", config.get<bool>("/recording/voiceEnhance", true)},
                {"hideDesktopIcons", config.get<bool>("/recording/hideDesktopIcons", false)},
                {"hideTaskbar", config.get<bool>("/recording/hideTaskbar", false)},
                {"typingAutoZoom", config.get<bool>("/recording/typingAutoZoom", false)},
                {"hideIndicatorInCapture", config.get<bool>("/recording/hideIndicatorInCapture", true)},
                {"studioMode", config.get<bool>("/recording/studioMode", true)},
                {"studioAutoZoom", config.get<bool>("/recording/studioAutoZoom", true)},
                {"studioZoomMode", config.get<int>("/recording/studioZoomMode", 1)},
                {"studioZoomFactor", config.get<double>("/recording/studioZoomFactor", 1.32)},
                {"studioZoomPacing", config.get<double>("/recording/studioZoomPacing", 0.32)},
                {"studioZoomSensitivity", config.get<double>("/recording/studioZoomSensitivity", 0.32)},
                {"studioFocusSpotlightDim", config.get<double>("/recording/studioFocusSpotlightDim", 0.18)},
                {"studioLockCameraHold", config.get<bool>("/recording/studioLockCameraHold", true)},
                {"studioSmoothCursor", config.get<bool>("/recording/studioSmoothCursor", true)},
                {"studioCursorScale", config.get<double>("/recording/studioCursorScale", 1.35)},
                {"studioAutoFrame", config.get<bool>("/recording/studioAutoFrame", true)},
                {"studioPaddingRatio", config.get<double>("/recording/studioPaddingRatio", 0.092)},
                {"studioInsetBorder", config.get<double>("/recording/studioInsetBorder", 7.0)},
                {"studioTheme", config.get<std::string>("/recording/studioTheme", "blossom_silk")},
                {"studioAutoCaptions", config.get<bool>("/recording/studioAutoCaptions", false)},
                {"studioBgBlur", config.get<double>("/recording/studioBgBlur", 0.0)},
                {"studioShadow", config.get<double>("/recording/studioShadow", 0.72)},
                {"studioCornerRadius", config.get<double>("/recording/studioCornerRadius", 18.0)},
                {"studioMotionBlur", config.get<bool>("/recording/studioMotionBlur", false)},
                {"studioCursorHalo", config.get<bool>("/recording/studioCursorHalo", false)},
                {"studioClickStyle", config.get<int>("/recording/studioClickStyle", 0)},
                {"studioCustomBg", config.get<std::string>("/recording/studioCustomBg", "")},
                {"studioExportFormat", std::clamp(config.get<int>("/recording/studioExport/format", 0), 0, 5)},
                {"studioExportResolution", std::clamp(config.get<int>("/recording/studioExport/resolution", 3), 0, 6)},
                {"studioExportAutoClose", config.get<bool>("/recording/studioExport/autoClose", false)},
                {"studioWatermarkEnabled", studioWm.enabled},
                {"studioWatermarkMode", static_cast<int>(studioWm.mode)},
                {"studioWatermarkImagePath", studioWm.imagePathUtf8},
                {"studioWatermarkText", studioWm.textTemplate},
                {"studioWatermarkAnchor", static_cast<int>(studioWm.anchor)},
                {"studioWatermarkScale", static_cast<double>(studioWm.scale)},
                {"studioWatermarkOpacity", static_cast<double>(studioWm.opacity)},
                {"studioWatermarkPolicyForced", studioWm.policyForced},
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
            auto curStudioWm = tools3000::capture::WatermarkRenderer::loadStudioWatermarkConfig();
            bool studioWmChanged = false;
            nlohmann::json directParams = nlohmann::json::object();

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
                if (key == "studioTheme" && (!value.is_string() ||
                    tools3000::capture::StudioBackdropCatalog::findByKey(value.get<std::string>()) == nullptr))
                    return {{"success", false}, {"error", "invalid studioTheme"}};
                if (key == "studioPaddingRatio" && (!value.is_number() || value.get<double>() < 0.0 || value.get<double>() > 0.22))
                    return {{"success", false}, {"error", "studioPaddingRatio must be between 0 and 0.22"}};
                if (key == "studioInsetBorder" && (!value.is_number() || value.get<double>() < 0.0 || value.get<double>() > 16.0))
                    return {{"success", false}, {"error", "studioInsetBorder must be between 0 and 16"}};
                if (key == "studioZoomFactor" && (!value.is_number() || value.get<double>() < 1.0 || value.get<double>() > 2.8))
                    return {{"success", false}, {"error", "studioZoomFactor must be between 1.0 and 2.8"}};
                if (key == "studioZoomPacing" && (!value.is_number() || value.get<double>() < 0.0 || value.get<double>() > 1.0))
                    return {{"success", false}, {"error", "studioZoomPacing must be between 0 and 1"}};
                if (key == "studioZoomSensitivity" && (!value.is_number() || value.get<double>() < 0.0 || value.get<double>() > 1.0))
                    return {{"success", false}, {"error", "studioZoomSensitivity must be between 0 and 1"}};
                if (key == "studioFocusSpotlightDim" && (!value.is_number() || value.get<double>() < 0.0 || value.get<double>() > 0.65))
                    return {{"success", false}, {"error", "studioFocusSpotlightDim must be between 0 and 0.65"}};
                if (key == "studioLockCameraHold" && !value.is_boolean())
                    return {{"success", false}, {"error", "studioLockCameraHold must be a boolean"}};
                if (key == "studioCursorScale" && (!value.is_number() || value.get<double>() < 0.8 || value.get<double>() > 2.2))
                    return {{"success", false}, {"error", "studioCursorScale must be between 0.8 and 2.2"}};
                if (key == "studioZoomMode" && (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 2))
                    return {{"success", false}, {"error", "studioZoomMode must be 0, 1, or 2"}};
                if (key == "studioClickStyle" && (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 3))
                    return {{"success", false}, {"error", "studioClickStyle must be between 0 and 3"}};
                if (key == "studioBgBlur" && (!value.is_number() || value.get<double>() < 0.0 || value.get<double>() > 40.0))
                    return {{"success", false}, {"error", "studioBgBlur must be between 0 and 40"}};
                if (key == "studioShadow" && (!value.is_number() || value.get<double>() < 0.0 || value.get<double>() > 1.0))
                    return {{"success", false}, {"error", "studioShadow must be between 0 and 1"}};
                if (key == "studioCornerRadius" && (!value.is_number() || value.get<double>() < 0.0 || value.get<double>() > 48.0))
                    return {{"success", false}, {"error", "studioCornerRadius must be between 0 and 48"}};
                if (key == "studioCustomBg" && (!value.is_string() || value.get_ref<const std::string&>().size() > 32767))
                    return {{"success", false}, {"error", "invalid studioCustomBg"}};
                if (key == "studioExportFormat") {
                    if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 5)
                        return {{"success", false}, {"error", "invalid studioExportFormat"}};
                    continue;
                }
                if (key == "studioExportResolution") {
                    if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 6)
                        return {{"success", false}, {"error", "invalid studioExportResolution"}};
                    continue;
                }
                if (key == "studioExportAutoClose") {
                    if (!value.is_boolean())
                        return {{"success", false}, {"error", "studioExportAutoClose must be boolean"}};
                    continue;
                }
                if (key == "studioWatermarkEnabled") {
                    if (!value.is_boolean())
                        return {{"success", false}, {"error", "studioWatermarkEnabled must be boolean"}};
                    if (curStudioWm.policyForced && !value.get<bool>())
                        return {{"success", false}, {"error", "watermark is enforced by enterprise policy"}};
                    curStudioWm.enabled = value.get<bool>();
                    studioWmChanged = true;
                    continue;
                }
                if (key == "studioWatermarkMode") {
                    if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 2)
                        return {{"success", false}, {"error", "invalid studioWatermarkMode"}};
                    curStudioWm.mode = static_cast<tools3000::capture::WatermarkMode>(value.get<int>());
                    studioWmChanged = true;
                    continue;
                }
                if (key == "studioWatermarkAnchor") {
                    if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 5)
                        return {{"success", false}, {"error", "invalid studioWatermarkAnchor"}};
                    curStudioWm.anchor = static_cast<tools3000::capture::WatermarkAnchor>(value.get<int>());
                    studioWmChanged = true;
                    continue;
                }
                if (key == "studioWatermarkImagePath") {
                    if (!value.is_string() || value.get_ref<const std::string&>().size() > 32767)
                        return {{"success", false}, {"error", "invalid studioWatermarkImagePath"}};
                    curStudioWm.imagePathUtf8 = value.get<std::string>();
                    studioWmChanged = true;
                    continue;
                }
                if (key == "studioWatermarkText") {
                    if (!value.is_string() || value.get_ref<const std::string&>().size() > 512)
                        return {{"success", false}, {"error", "invalid studioWatermarkText"}};
                    curStudioWm.textTemplate = value.get<std::string>();
                    studioWmChanged = true;
                    continue;
                }
                if (key == "studioWatermarkScale") {
                    if (!value.is_number() || value.get<double>() < 0.30 || value.get<double>() > 3.0)
                        return {{"success", false}, {"error", "invalid studioWatermarkScale"}};
                    curStudioWm.scale = std::clamp(static_cast<float>(value.get<double>()), 0.35f, 2.80f);
                    studioWmChanged = true;
                    continue;
                }
                if (key == "studioWatermarkOpacity") {
                    if (!value.is_number() || value.get<double>() < 0.03 || value.get<double>() > 1.0)
                        return {{"success", false}, {"error", "invalid studioWatermarkOpacity"}};
                    curStudioWm.opacity = std::clamp(static_cast<float>(value.get<double>()), 0.04f, 1.00f);
                    studioWmChanged = true;
                    continue;
                }
                if ((key == "includeCursor" || key == "showClickEffects" ||
                     key == "showClickZoom" || key == "includeKeycast" ||
                     key == "captureSystemAudio" || key == "captureMicrophone" ||
                     key == "experimentalGpuEncoding" ||
                     key == "voiceEnhance" || key == "hideDesktopIcons" ||
                     key == "hideTaskbar" || key == "typingAutoZoom" ||
                     key == "hideIndicatorInCapture" ||
                     key == "studioMode" || key == "studioAutoZoom" ||
                     key == "studioSmoothCursor" || key == "studioAutoFrame" ||
                     key == "studioAutoCaptions" || key == "studioMotionBlur" ||
                     key == "studioCursorHalo") &&
                    !value.is_boolean())
                    return {{"success", false}, {"error", key + " must be boolean"}};
                if (key != "format" && key != "fps" && key != "bitrate" && key != "saveDirectory" &&
                    key != "countdownSeconds" &&
                    key != "includeCursor" && key != "showClickEffects" &&
                    key != "showClickZoom" && key != "includeKeycast" &&
                    key != "captureSystemAudio" && key != "captureMicrophone" &&
                    key != "experimentalGpuEncoding" &&
                    key != "voiceEnhance" && key != "hideDesktopIcons" &&
                    key != "hideTaskbar" && key != "typingAutoZoom" &&
                    key != "hideIndicatorInCapture" &&
                    key != "studioMode" && key != "studioAutoZoom" && key != "studioZoomMode" &&
                    key != "studioZoomFactor" && key != "studioZoomPacing" &&
                    key != "studioZoomSensitivity" && key != "studioFocusSpotlightDim" &&
                    key != "studioLockCameraHold" && key != "studioSmoothCursor" &&
                    key != "studioCursorScale" && key != "studioAutoFrame" &&
                    key != "studioPaddingRatio" && key != "studioInsetBorder" &&
                    key != "studioTheme" && key != "studioAutoCaptions" &&
                    key != "studioBgBlur" && key != "studioShadow" &&
                    key != "studioCornerRadius" && key != "studioMotionBlur" &&
                    key != "studioCursorHalo" && key != "studioClickStyle" && key != "studioCustomBg" &&
                    key != "systemAudioDeviceId" && key != "microphoneDeviceId" &&
                    key != "systemAudioVolume" && key != "microphoneVolume")
                    return {{"success", false}, {"error", "unsupported setting: " + key}};
                directParams[key] = value;
            }

            bool saved = true;
            if (!directParams.empty()) {
                saved = tools3000::core::ConfigManager::instance().mergePatch(
                    {{"recording", directParams}}, "/recording");
            }
            if (saved) {
                auto& cm = tools3000::core::ConfigManager::instance();
                if (params.contains("studioExportFormat")) {
                    cm.set<int>("/recording/studioExport/format", params.at("studioExportFormat").get<int>());
                    cm.set<bool>("/recording/studioExport/savePrefs", true);
                }
                if (params.contains("studioExportResolution")) {
                    cm.set<int>("/recording/studioExport/resolution", params.at("studioExportResolution").get<int>());
                    cm.set<bool>("/recording/studioExport/savePrefs", true);
                }
                if (params.contains("studioExportAutoClose")) {
                    cm.set<bool>("/recording/studioExport/autoClose", params.at("studioExportAutoClose").get<bool>());
                }
                if (studioWmChanged) {
                    tools3000::capture::WatermarkRenderer::saveStudioWatermarkConfig(curStudioWm);
                    if (!curStudioWm.imagePathUtf8.empty()) {
                        tools3000::capture::WatermarkRenderer::pushStudioRecentLogoPath(curStudioWm.imagePathUtf8);
                    }
                }
            }
            if (saved && params.contains("format")) {
                const auto fmt = params.at("format").get<std::string>();
                if (fmt != "gif") {
                    // 记忆最近选择的非 GIF 视频编码器，确保录屏工具栏切到 GIF 后再切回视频时 100% 恢复原编码器
                    tools3000::core::ConfigManager::instance().set<std::string>("/recording/videoFormat", fmt);
                }
            }
            if (saved && (params.contains("systemAudioVolume") ||
                          params.contains("microphoneVolume"))) {
                auto& config = tools3000::core::ConfigManager::instance();
                ScreenRecorder::instance().setAudioVolumes(
                    std::clamp(config.get<int>("/recording/systemAudioVolume", 100), 0, 200) / 100.0f,
                    std::clamp(config.get<int>("/recording/microphoneVolume", 100), 0, 200) / 100.0f);
            }
            if (saved && params.contains("hideIndicatorInCapture")) {
                tools3000::core::MainThreadDispatcher::instance().post([]() {
                    RecordingIndicator::instance().refreshCaptureAffinity();
                });
            }
            return {{"success", saved}, {"error", saved ? "" : "failed to persist settings"}};
        });

        mb.registerHandler("ocr.getSettings", [](const nlohmann::json&) -> nlohmann::json {
            auto& config = tools3000::core::ConfigManager::instance();
            return {
                {"engine", config.get<std::string>("/ocr/engine", "auto")},
                {"modelTier", config.get<std::string>("/ocr/modelTier", "mobile")},
                {"language", config.get<std::string>("/ocr/language", "auto")},
                {"copyResult", config.get<bool>("/ocr/copyResult", true)},
                {"showResultWindow", config.get<bool>("/ocr/showResultWindow", true)},
                {"liveTextOnPin", config.get<bool>("/ocr/liveTextOnPin", true)},
                {"pinLiveTextOnCapture", config.get<bool>("/ocr/pinLiveTextOnCapture", true)}
            };
        });

        mb.registerHandler("ocr.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
            if (!params.is_object() || params.empty()) return {{"success", false}, {"error", "no settings supplied"}};
            for (const auto& [key, value] : params.items()) {
                if (key == "copyResult" || key == "showResultWindow" ||
                    key == "liveTextOnPin" || key == "pinLiveTextOnCapture") {
                    if (!value.is_boolean())
                        return {{"success", false}, {"error", "invalid boolean OCR setting: " + key}};
                } else if (key == "language") {
                    if (!value.is_string() || value.get<std::string>().empty())
                        return {{"success", false}, {"error", "invalid OCR language"}};
                } else if (key == "engine") {
                    if (!value.is_string())
                        return {{"success", false}, {"error", "invalid OCR engine"}};
                    const std::string eng = value.get<std::string>();
                    if (eng != "auto" && eng != "ppocr_v4" && eng != "windows")
                        return {{"success", false}, {"error", "unsupported OCR engine: " + eng}};
                } else if (key == "modelTier") {
                    if (!value.is_string())
                        return {{"success", false}, {"error", "invalid OCR modelTier"}};
                    const std::string tier = value.get<std::string>();
                    if (tier != "mobile" && tier != "server")
                        return {{"success", false}, {"error", "unsupported OCR modelTier: " + tier}};
                } else {
                    return {{"success", false}, {"error", "unsupported OCR setting: " + key}};
                }
            }
            const bool saved = tools3000::core::ConfigManager::instance().mergePatch(
                {{"ocr", params}}, "/ocr");
            if (saved && params.contains("modelTier")) {
                // 切换识别档位时先清理已驻留会话，下次识别立即按新档位按需懒加载，避免空占显存
                tools3000::ocr::DeepOcrEngine::instance().releaseLoadedModels();
            }
            return {{"success", saved}, {"error", saved ? "" : "failed to persist settings"}};
        });

        auto providerToUi = [](const std::string& p) -> std::string {
            using namespace tools3000::core::translate;
            if (p == TranslationConstants::kProviderOpenAiCompatible) return "openai";
            if (p == TranslationConstants::kProviderDeepL) return "deepl";
            return "free_auto";
        };
        auto langToUi = [](const std::string& rawLang, const std::string& fallbackUiLang) -> std::string {
            std::string canonical = tools3000::core::translate::TranslationService::canonicalizeLanguageCode(rawLang);
            if (canonical.empty() || canonical == "auto") {
                canonical = tools3000::core::translate::TranslationService::canonicalizeLanguageCode(fallbackUiLang);
            }
            if (canonical == "zh-CN" || canonical == "auto" || canonical.empty()) return "zh-Hans";
            if (canonical == "zh-TW") return "zh-Hant";
            if (canonical == "pt-BR") return "pt";
            return canonical;
        };

        auto serializeTranslateSettings = [providerToUi, langToUi](const tools3000::core::translate::TranslationSettings& s) -> nlohmann::json {
            using namespace tools3000::core::translate;
            const std::string uiLang = tools3000::core::UiI18n::getEffectiveLanguageTag();
            const std::string targetUi = langToUi(s.targetLang, uiLang);
            const std::string secondaryUi = langToUi(s.secondaryTargetLang, targetUi == "en" ? "zh-CN" : "en");
            const int timeoutSec = std::clamp(
                (s.timeoutMs + 500) / 1000,
                TranslationConstants::kMinTimeoutMs / 1000,
                TranslationConstants::kMaxTimeoutMs / 1000);
            return {
                {"provider", providerToUi(s.provider)},
                {"targetLang", targetUi},
                {"secondaryTargetLang", secondaryUi},
                {"freePrimary", s.freePrimary == TranslationConstants::kFreePrimaryGoogle ? "google" : "edge"},
                {"openaiBaseUrl", s.openaiBaseUrl},
                {"openaiModel", s.openaiModel},
                {"hasOpenaiApiKey", !s.openaiApiKey.empty()},
                {"openaiApiKeyMasked", TranslationService::maskApiKeyForUi(s.openaiApiKey)},
                {"deeplEndpoint", s.deeplBaseUrl},
                {"hasDeeplApiKey", !s.deeplApiKey.empty()},
                {"deeplApiKeyMasked", TranslationService::maskApiKeyForUi(s.deeplApiKey)},
                {"timeoutSec", timeoutSec}
            };
        };

        auto applyTranslateParams = [](tools3000::core::translate::TranslationSettings& dst,
                                       const nlohmann::json& params,
                                       std::string& outError) -> bool {
            using namespace tools3000::core::translate;
            if (!params.is_object()) {
                outError = "invalid translate settings payload";
                return false;
            }
            for (const auto& [key, value] : params.items()) {
                if (key == "provider") {
                    if (!value.is_string()) { outError = "invalid provider"; return false; }
                    const std::string v = value.get<std::string>();
                    if (v == "free_auto" || v == TranslationConstants::kProviderAutoFree) {
                        dst.provider = TranslationConstants::kProviderAutoFree;
                    } else if (v == "openai" || v == TranslationConstants::kProviderOpenAiCompatible) {
                        dst.provider = TranslationConstants::kProviderOpenAiCompatible;
                    } else if (v == "deepl") {
                        dst.provider = TranslationConstants::kProviderDeepL;
                    } else {
                        outError = "unsupported translation provider: " + v;
                        return false;
                    }
                } else if (key == "targetLang") {
                    if (!value.is_string() || value.get<std::string>().empty()) {
                        outError = "invalid targetLang";
                        return false;
                    }
                    dst.targetLang = TranslationService::canonicalizeLanguageCode(value.get<std::string>());
                } else if (key == "secondaryTargetLang") {
                    if (!value.is_string() || value.get<std::string>().empty()) {
                        outError = "invalid secondaryTargetLang";
                        return false;
                    }
                    dst.secondaryTargetLang = TranslationService::canonicalizeLanguageCode(value.get<std::string>());
                } else if (key == "freePrimary") {
                    if (!value.is_string()) { outError = "invalid freePrimary"; return false; }
                    const std::string v = value.get<std::string>();
                    if (v != TranslationConstants::kFreePrimaryEdge && v != TranslationConstants::kFreePrimaryGoogle) {
                        outError = "unsupported freePrimary channel: " + v;
                        return false;
                    }
                    dst.freePrimary = v;
                } else if (key == "openaiBaseUrl") {
                    if (!value.is_string()) { outError = "invalid openaiBaseUrl"; return false; }
                    dst.openaiBaseUrl = value.get<std::string>();
                } else if (key == "openaiModel") {
                    if (!value.is_string()) { outError = "invalid openaiModel"; return false; }
                    dst.openaiModel = value.get<std::string>();
                } else if (key == "openaiApiKey") {
                    if (!value.is_string()) { outError = "invalid openaiApiKey"; return false; }
                    const std::string k = value.get<std::string>();
                    if (!k.empty()) dst.openaiApiKey = k;
                } else if (key == "clearOpenaiApiKey") {
                    if (!value.is_boolean()) { outError = "invalid clearOpenaiApiKey"; return false; }
                    if (value.get<bool>()) dst.openaiApiKey.clear();
                } else if (key == "deeplEndpoint") {
                    if (!value.is_string()) { outError = "invalid deeplEndpoint"; return false; }
                    dst.deeplBaseUrl = value.get<std::string>();
                } else if (key == "deeplApiKey") {
                    if (!value.is_string()) { outError = "invalid deeplApiKey"; return false; }
                    const std::string k = value.get<std::string>();
                    if (!k.empty()) dst.deeplApiKey = k;
                } else if (key == "clearDeeplApiKey") {
                    if (!value.is_boolean()) { outError = "invalid clearDeeplApiKey"; return false; }
                    if (value.get<bool>()) dst.deeplApiKey.clear();
                } else if (key == "timeoutSec") {
                    if (!value.is_number_integer()) { outError = "invalid timeoutSec"; return false; }
                    dst.timeoutMs = std::clamp(value.get<int>(), 3, 60) * 1000;
                } else {
                    outError = "unsupported translate setting: " + key;
                    return false;
                }
            }
            return true;
        };

        mb.registerHandler("translate.getSettings", [serializeTranslateSettings](const nlohmann::json&) -> nlohmann::json {
            return serializeTranslateSettings(tools3000::core::translate::TranslationService::instance().loadSettings());
        });

        mb.registerHandler("translate.updateSettings", [serializeTranslateSettings, applyTranslateParams](const nlohmann::json& params) -> nlohmann::json {
            if (!params.is_object() || params.empty()) {
                return {{"success", false}, {"error", "no settings supplied"}};
            }
            auto next = tools3000::core::translate::TranslationService::instance().loadSettings();
            std::string err;
            if (!applyTranslateParams(next, params, err)) {
                return {{"success", false}, {"error", err}};
            }
            const bool saved = tools3000::core::translate::TranslationService::instance().saveSettings(next);
            return {
                {"success", saved},
                {"error", saved ? "" : "failed to persist translation settings"},
                {"settings", serializeTranslateSettings(tools3000::core::translate::TranslationService::instance().loadSettings())}
            };
        });

        mb.registerHandler("translate.testConnection", [applyTranslateParams](const nlohmann::json& params) -> nlohmann::json {
            using namespace tools3000::core::translate;
            auto candidate = TranslationService::instance().loadSettings();
            if (params.is_object() && !params.empty()) {
                std::string err;
                if (!applyTranslateParams(candidate, params, err)) {
                    return {{"success", false}, {"errorCode", "invalid_params"}, {"errorMessage", err}};
                }
            }
            TranslationBatchRequest req;
            req.texts = {"Hello, Tools3000!"};
            req.uiLanguageCode = tools3000::core::UiI18n::getEffectiveLanguageTag();
            const auto res = TranslationService::instance().translateBatchWithSettings(req, candidate);
            const std::string sampleOutput = res.translations.empty() ? "" : res.translations[0];
            return {
                {"success", res.success},
                {"providerUsed", res.providerUsed},
                {"resolvedTargetLang", res.effectiveTargetLang},
                {"elapsedMs", res.elapsedMs},
                {"sampleOutput", sampleOutput},
                {"errorCode", res.errorCode},
                {"errorMessage", res.errorDetail}
            };
        });

        auto serializeModelStatus = []() -> nlohmann::json {
            const auto st = tools3000::ocr::DeepOcrEngine::instance().getModelStatus();
            return {
                {"installed", st.installed},
                {"serverInstalled", st.serverInstalled},
                {"downloading", st.downloading},
                {"downloadingServer", st.downloadingServer},
                {"progressPercent", st.progressPercent},
                {"downloadedBytes", st.downloadedBytes},
                {"totalBytes", st.totalBytes},
                {"serverTotalBytes", st.serverTotalBytes},
                {"modelTier", st.modelTier},
                {"activeModelTier", st.activeModelTier},
                {"state", st.state},
                {"currentFile", st.currentFile},
                {"modelDir", st.modelDir},
                {"error", st.error}
            };
        };

        mb.registerHandler("ocr.getStatus", [serializeModelStatus](const nlohmann::json&) -> nlohmann::json {
            const auto& ocr = tools3000::ocr::OcrEngine::instance();
            const bool deepInstalled = tools3000::ocr::DeepOcrEngine::instance().isModelInstalled();
            const std::string engPref = tools3000::core::ConfigManager::instance().get<std::string>("/ocr/engine", "auto");
            const std::string effectiveEngine = ((engPref == "auto" || engPref == "ppocr_v4") && deepInstalled)
                ? "ppocr_v4"
                : "windows";
            return {
                {"available", ocr.isAvailable()},
                {"windowsOcrAvailable", ocr.isWindowsOcrAvailable()},
                {"deepOcrInstalled", deepInstalled},
                {"effectiveEngine", effectiveEngine},
                {"modelStatus", serializeModelStatus()}
            };
        });

        mb.registerHandler("ocr.getModelStatus", [serializeModelStatus](const nlohmann::json&) -> nlohmann::json {
            return serializeModelStatus();
        });

        mb.registerHandler("ocr.downloadModel", [serializeModelStatus](const nlohmann::json& params) -> nlohmann::json {
            std::string tier = "mobile";
            if (params.is_object() && params.contains("tier") && params["tier"].is_string()) {
                const std::string reqTier = params["tier"].get<std::string>();
                if (reqTier == "server") {
                    tier = "server";
                }
            }
            const bool started = tools3000::ocr::DeepOcrEngine::instance().startModelDownload(tier);
            return {
                {"success", started},
                {"modelStatus", serializeModelStatus()}
            };
        });

        mb.registerHandler("ocr.cancelModelDownload", [serializeModelStatus](const nlohmann::json&) -> nlohmann::json {
            tools3000::ocr::DeepOcrEngine::instance().cancelModelDownload();
            return {
                {"success", true},
                {"modelStatus", serializeModelStatus()}
            };
        });

        mb.registerHandler("ocr.deleteServerModel", [serializeModelStatus](const nlohmann::json&) -> nlohmann::json {
            const bool deleted = tools3000::ocr::DeepOcrEngine::instance().deleteServerModel();
            return {
                {"success", deleted},
                {"modelStatus", serializeModelStatus()}
            };
        });

        auto serializeInpaintStatus = []() -> nlohmann::json {
            const auto st = tools3000::capture::AiInpaintEngine::instance().getModelStatus();
            return {
                {"installed", st.installed},
                {"downloading", st.downloading},
                {"progressPercent", st.progressPercent},
                {"downloadedBytes", st.downloadedBytes},
                {"totalBytes", st.totalBytes},
                {"state", st.state},
                {"modelDir", st.modelDir},
                {"modelFile", st.modelFile},
                {"error", st.error},
                {"usingGpu", st.usingGpu}
            };
        };

        mb.registerHandler("capture.getInpaintModelStatus", [serializeInpaintStatus](const nlohmann::json&) -> nlohmann::json {
            return serializeInpaintStatus();
        });

        mb.registerHandler("capture.downloadInpaintModel", [serializeInpaintStatus](const nlohmann::json&) -> nlohmann::json {
            const bool started = tools3000::capture::AiInpaintEngine::instance().startModelDownload();
            return {
                {"success", started},
                {"modelStatus", serializeInpaintStatus()}
            };
        });

        mb.registerHandler("capture.cancelInpaintModelDownload", [serializeInpaintStatus](const nlohmann::json&) -> nlohmann::json {
            tools3000::capture::AiInpaintEngine::instance().cancelModelDownload();
            return {
                {"success", true},
                {"modelStatus", serializeInpaintStatus()}
            };
        });

        mb.registerHandler("capture.deleteInpaintModel", [serializeInpaintStatus](const nlohmann::json&) -> nlohmann::json {
            const bool deleted = tools3000::capture::AiInpaintEngine::instance().deleteModel();
            return {
                {"success", deleted},
                {"modelStatus", serializeInpaintStatus()}
            };
        });

        mb.registerHandler("ocr.recognizeImageFile", [](const nlohmann::json& params) -> nlohmann::json {
            std::string path = params.value("path", "");
            if (path.empty()) return {{"success", false}, {"error", "path is required"}};
            std::string text = tools3000::ocr::OcrEngine::instance().recognizeImageFile(path);
            bool copy = params.value("copyToClipboard", tools3000::core::ConfigManager::instance().get<bool>("/ocr/copyResult", true));
            if (copy && !text.empty()) tools3000::core::WinUtils::copyToClipboard(text);
            return {{"success", true}, {"text", text}, {"copied", copy && !text.empty()}};
        });

        mb.registerHandler("ocr.openResultWindow", [callOnUiThread](const nlohmann::json& params) -> nlohmann::json {
            const std::string text = params.value("text", "");
            bool hasAnchor = false;
            RECT anchorRc{};
            if (params.contains("anchorRect") && params["anchorRect"].is_object()) {
                const auto& ar = params["anchorRect"];
                anchorRc.left = ar.value("left", 0);
                anchorRc.top = ar.value("top", 0);
                anchorRc.right = ar.value("right", 0);
                anchorRc.bottom = ar.value("bottom", 0);
                hasAnchor = (anchorRc.right > anchorRc.left && anchorRc.bottom > anchorRc.top);
            }
            return callOnUiThread([text, hasAnchor, anchorRc]() -> nlohmann::json {
                auto& win = tools3000::ocr::OcrResultWindow::instance();
                win.showResult(text, hasAnchor ? &anchorRc : nullptr);
                RECT rc{};
                RECT winRc{};
                if (win.hwnd()) {
                    GetClientRect(win.hwnd(), &rc);
                    GetWindowRect(win.hwnd(), &winRc);
                }
                const float cw = static_cast<float>((std::max)(1L, rc.right - rc.left));
                return {
                    {"success", win.hwnd() != nullptr && IsWindow(win.hwnd())},
                    {"lineCount", static_cast<int>(win.lineCount())},
                    {"maxScroll", win.maxScroll()},
                    {"singleLineFooter", win.isFooterHintSingleLine(cw)},
                    {"windowLeft", static_cast<int>(winRc.left)},
                    {"windowTop", static_cast<int>(winRc.top)},
                    {"windowRight", static_cast<int>(winRc.right)},
                    {"windowBottom", static_cast<int>(winRc.bottom)}
                };
            });
        });

        mb.registerHandler("ocr.selectResultRange", [callOnUiThread](const nlohmann::json& params) -> nlohmann::json {
            return callOnUiThread([params]() -> nlohmann::json {
                auto& win = tools3000::ocr::OcrResultWindow::instance();
                if (params.contains("setMergeParagraphs") && params["setMergeParagraphs"].is_boolean()) {
                    win.setMergeParagraphs(params["setMergeParagraphs"].get<bool>());
                }
                if (params.value("clear", false)) {
                    win.clearSelection();
                } else if (params.value("selectAll", false)) {
                    win.selectAll();
                } else if (params.contains("anchorLine") && params.contains("activeLine")) {
                    tools3000::ocr::OcrResultWindow::TextPosition anchor{
                        params.value("anchorLine", 0),
                        static_cast<UINT32>((std::max)(0, params.value("anchorOffset", 0)))
                    };
                    tools3000::ocr::OcrResultWindow::TextPosition caret{
                        params.value("activeLine", 0),
                        static_cast<UINT32>((std::max)(0, params.value("activeOffset", 0)))
                    };
                    win.setSelectionRange(anchor, caret);
                }
                std::string copiedWord;
                if (params.contains("selectWordLine") && params.contains("selectWordOffset")) {
                    copiedWord = win.selectAndCopyWordAt({
                        params.value("selectWordLine", 0),
                        static_cast<UINT32>((std::max)(0, params.value("selectWordOffset", 0)))
                    });
                }
                if (params.contains("insertText") && params["insertText"].is_string()) {
                    win.insertTextAtCaret(
                        tools3000::core::WinUtils::utf8ToWstring(params["insertText"].get<std::string>()));
                }
                if (params.value("deleteBackward", false)) {
                    win.deleteBackwardAtCaret();
                }
                if (params.value("copy", false)) {
                    win.copySelectionOrAll();
                }
                RECT rc{};
                if (win.hwnd()) GetClientRect(win.hwnd(), &rc);
                const float cw = static_cast<float>((std::max)(1L, rc.right - rc.left));
                return {
                    {"success", true},
                    {"lineCount", static_cast<int>(win.lineCount())},
                    {"mergeParagraphs", win.isMergeParagraphsEnabled()},
                    {"currentText", win.currentTextUtf8()},
                    {"copiedWord", copiedWord},
                    {"hasSelection", win.hasSelection()},
                    {"selectedText", win.selectedTextUtf8()},
                    {"scrollY", win.scrollY()},
                    {"maxScroll", win.maxScroll()},
                    {"singleLineFooter", win.isFooterHintSingleLine(cw)}
                };
            });
        });

        mb.registerHandler("ocr.closeResultWindow", [callOnUiThread](const nlohmann::json&) -> nlohmann::json {
            return callOnUiThread([]() -> nlohmann::json {
                tools3000::ocr::OcrResultWindow::instance().cleanup();
                return {{"success", true}};
            });
        });

        mb.registerHandler("capture.pinImageFile", [callOnUiThread](const nlohmann::json& params) -> nlohmann::json {
            std::string path = params.value("path", "");
            const bool autoOcr = params.value("autoOcr", false);
            const bool autoTranslate = params.value("autoTranslate", false);
            if (path.empty()) return {{"success", false}, {"error", "path is required"}};
            try {
                const auto filePath = std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(path));
                std::ifstream stream(filePath, std::ios::binary);
                if (!stream) return {{"success", false}, {"error", "file not found"}};
                std::vector<unsigned char> bytes(
                    (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                cv::Mat image = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
                if (!image.empty()) {
                    return callOnUiThread([img = std::move(image), autoOcr, autoTranslate]() -> nlohmann::json {
                        POINT pt;
                        GetCursorPos(&pt);
                        POINT spawnPos = tools3000::capture::PinWindow::calculateSmartSpawnPosition(img.cols, img.rows, &pt);
                        const auto pin = tools3000::capture::PinWindow::create(img, spawnPos.x, spawnPos.y);
                        if (pin) {
                            if (autoTranslate) {
                                pin->translateInPlace(false, false);
                            } else if (autoOcr) {
                                pin->extractText();
                            }
                        }
                        return {{"success", static_cast<bool>(pin)}};
                    });
                }
            } catch (const std::exception& e) {
                LOG_ERROR("贴图文件加载失败: {}", e.what());
                return {{"success", false}, {"error", e.what()}};
            }
            return {{"success", false}, {"error", "unsupported or invalid image"}};
        });

        m_pinImageFileSubId = tools3000::core::EventBus::instance().subscribe<tools3000::core::ActionPinImageFileEvent>(
            [](const tools3000::core::ActionPinImageFileEvent& ev) {
                if (ev.path.empty()) return;
                std::thread([path = ev.path, autoOcr = ev.autoOcr, autoTranslate = ev.autoTranslate]() {
                    try {
                        const auto filePath = std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(path));
                        std::ifstream stream(filePath, std::ios::binary);
                        if (!stream) return;
                        std::vector<unsigned char> bytes(
                            (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                        cv::Mat image = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
                        if (image.empty()) return;
                        tools3000::core::MainThreadDispatcher::instance().post(
                            [img = std::move(image), autoOcr, autoTranslate]() {
                                POINT pt{};
                                GetCursorPos(&pt);
                                POINT spawnPos = tools3000::capture::PinWindow::calculateSmartSpawnPosition(
                                    img.cols, img.rows, &pt);
                                const auto pin = tools3000::capture::PinWindow::create(img, spawnPos.x, spawnPos.y);
                                if (pin) {
                                    if (autoTranslate) {
                                        pin->translateInPlace(false, false);
                                    } else if (autoOcr) {
                                        pin->extractText();
                                    }
                                }
                            });
                    } catch (...) {
                    }
                }).detach();
            });

        mb.registerHandler("capture.copyHistoryItem", [](const nlohmann::json& params) -> nlohmann::json {
            const uint64_t id = params.value("id", static_cast<uint64_t>(0));
            const int index = params.value("index", -1);
            const std::string fallbackPath = params.value("path", "");
            cv::Mat image;
            std::string resolvedPath;
            CaptureRegion region{};
            if (!loadHistoryImageAndMeta(id, index, fallbackPath, image, resolvedPath, region)) {
                return {{"success", false}, {"error", "history image not available"}};
            }
            const std::wstring widePath = tools3000::core::WinUtils::utf8ToWstring(resolvedPath);
            const CaptureRegion* pReg = (region.width > 0 && region.height > 0) ? &region : nullptr;
            const bool copied = ClipboardUtils::copyImageToClipboard(image, widePath, nullptr, pReg);
            return {{"success", copied}, {"error", copied ? "" : "failed to copy image to clipboard"}};
        });

        mb.registerHandler("capture.saveHistoryItemAs", [](const nlohmann::json& params) -> nlohmann::json {
            const uint64_t id = params.value("id", static_cast<uint64_t>(0));
            const int index = params.value("index", -1);
            const std::string fallbackPath = params.value("path", "");
            const std::string directTarget = params.value("targetPath", "");
            cv::Mat image;
            std::string resolvedPath;
            CaptureRegion region{};
            if (!loadHistoryImageAndMeta(id, index, fallbackPath, image, resolvedPath, region)) {
                return {{"success", false}, {"error", "history image not available"}};
            }
            const int quality = std::clamp(
                tools3000::core::ConfigManager::instance().get<int>("/capture/quality", 90), 1, 100);

            if (!directTarget.empty()) {
                std::filesystem::path target(tools3000::core::WinUtils::utf8ToWstring(directTarget));
                if (!target.has_extension()) target += L".png";
                const bool ok = encodeAndWriteImageFile(image, target, quality);
                return {
                    {"success", ok},
                    {"saved", ok},
                    {"path", ok ? tools3000::core::WinUtils::wstringToUtf8(target.wstring()) : ""},
                    {"error", ok ? "" : "failed to write image file"}
                };
            }

            struct ComStaGuard {
                HRESULT hr;
                ComStaGuard() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
                ~ComStaGuard() { if (hr == S_OK || hr == S_FALSE) CoUninitialize(); }
            } comGuard;

            IFileSaveDialog* dialog = nullptr;
            HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&dialog));
            if (FAILED(hr) || !dialog) {
                return {{"success", false}, {"error", "failed to open file save dialog"}};
            }

            static const COMDLG_FILTERSPEC filters[] = {
                {L"PNG Image (*.png)", L"*.png"},
                {L"JPEG Image (*.jpg;*.jpeg)", L"*.jpg;*.jpeg"},
                {L"WebP Image (*.webp)", L"*.webp"},
                {L"Bitmap (*.bmp)", L"*.bmp"}
            };
            dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
            dialog->SetFileTypeIndex(1);
            dialog->SetDefaultExtension(L"png");

            std::wstring defaultFileName = L"Tools3000_Export.png";
            if (!resolvedPath.empty()) {
                const std::filesystem::path src(tools3000::core::WinUtils::utf8ToWstring(resolvedPath));
                if (!src.filename().empty()) {
                    defaultFileName = src.filename().wstring();
                }
            }
            dialog->SetFileName(defaultFileName.c_str());

            std::filesystem::path chosenPath;
            UINT chosenTypeIndex = 1;
            const HRESULT showHr = dialog->Show(GetForegroundWindow());
            if (SUCCEEDED(showHr)) {
                dialog->GetFileTypeIndex(&chosenTypeIndex);
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                    PWSTR rawPath = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath) {
                        chosenPath = std::filesystem::path(rawPath);
                        CoTaskMemFree(rawPath);
                    }
                    item->Release();
                }
            }
            dialog->Release();

            if (chosenPath.empty()) {
                return {{"success", true}, {"saved", false}, {"cancelled", true}};
            }
            if (!chosenPath.has_extension()) {
                chosenPath += (chosenTypeIndex == 2) ? L".jpg"
                            : (chosenTypeIndex == 3) ? L".webp"
                            : (chosenTypeIndex == 4) ? L".bmp" : L".png";
            }
            const bool ok = encodeAndWriteImageFile(image, chosenPath, quality);
            const std::string savedUtf8 = ok ? tools3000::core::WinUtils::wstringToUtf8(chosenPath.wstring()) : "";
            return {
                {"success", ok},
                {"saved", ok},
                {"path", savedUtf8},
                {"error", ok ? "" : "failed to save image"}
            };
        });

        mb.registerHandler("capture.pasteAsPin", [callOnUiThread](const nlohmann::json&) -> nlohmann::json {
            return callOnUiThread([]() -> nlohmann::json {
                auto pin = tools3000::capture::PinWindow::pasteNextHistoryOrClipboard();
                return {{"success", static_cast<bool>(pin)}};
            });
        });

        mb.registerHandler("capture.cancelAllPinClickThrough", [callOnUiThread](const nlohmann::json&) -> nlohmann::json {
            return callOnUiThread([]() -> nlohmann::json {
                tools3000::capture::PinWindow::cancelAllClickThrough();
                return {{"success", true}};
            });
        });

        mb.registerHandler("capture.togglePinClickThrough", [callOnUiThread](const nlohmann::json&) -> nlohmann::json {
            return callOnUiThread([]() -> nlohmann::json {
                bool toggled = tools3000::capture::PinWindow::toggleClickThroughUnderCursor();
                return {{"success", true}, {"toggled", toggled}};
            });
        });

        mb.registerHandler("capture.inspectScrollControlBar", [callOnUiThread](const nlohmann::json& params) -> nlohmann::json {
            return callOnUiThread([params]() -> nlohmann::json {
                const float scale = params.value("scale", 1.0f);
                const bool chinese = params.value("chinese", true);
                const bool paused = params.value("paused", false);
                const auto snap = ScrollCaptureOverlay::instance().computeControlBarLayoutForTest(scale, chinese, paused);
                nlohmann::json buttons = nlohmann::json::array();
                for (size_t i = 0; i < snap.buttonRects.size(); ++i) {
                    buttons.push_back({
                        {"left", snap.buttonRects[i].left},
                        {"top", snap.buttonRects[i].top},
                        {"right", snap.buttonRects[i].right},
                        {"bottom", snap.buttonRects[i].bottom},
                        {"keycap", tools3000::core::WinUtils::wstringToUtf8(snap.keycaps[i])}
                    });
                }
                return {
                    {"success", true},
                    {"barWidth", snap.barWidth},
                    {"barHeight", snap.barHeight},
                    {"dividerX", snap.dividerX},
                    {"buttons", buttons}
                };
            });
        });

        mb.registerHandler("capture.inspectPinOcrState", [callOnUiThread](const nlohmann::json& params) -> nlohmann::json {
            return callOnUiThread([params]() -> nlohmann::json {
                auto pin = tools3000::capture::PinWindow::latestInstance();
                if (!pin || !pin->isAlive()) {
                    return {{"success", false}, {"error", "no active pin window"}};
                }
                if (params.contains("mockBlocks") && params["mockBlocks"].is_array()) {
                    std::vector<tools3000::capture::PinOcrBlock> blocks;
                    for (const auto& item : params["mockBlocks"]) {
                        tools3000::capture::PinOcrBlock b;
                        b.text = item.value("text", "");
                        b.origBox = cv::Rect(
                            item.value("x", 0),
                            item.value("y", 0),
                            item.value("w", 40),
                            item.value("h", 18));
                        b.layoutRegionId = item.value("layoutRegionId", 0);
                        b.alignedRowId = item.value("alignedRowId", -1);
                        blocks.push_back(std::move(b));
                    }
                    pin->setOcrBlocksForTest(std::move(blocks));
                }
                if (params.contains("mockQrCodes") && params["mockQrCodes"].is_array()) {
                    std::vector<tools3000::ocr::OcrQrResult> qrs;
                    for (const auto& item : params["mockQrCodes"]) {
                        tools3000::ocr::OcrQrResult q;
                        q.text = item.value("text", "");
                        q.boundingBox = cv::Rect(
                            item.value("x", 0),
                            item.value("y", 0),
                            item.value("w", 48),
                            item.value("h", 48));
                        qrs.push_back(std::move(q));
                    }
                    pin->setQrCodesForTest(std::move(qrs));
                }
                if (params.value("copyTable", false)) {
                    pin->copySelectedOrAllAsTable();
                }
                if (params.value("selectAll", false)) {
                    pin->selectAllText();
                }
                if (params.value("copySelected", false)) {
                    pin->copySelectedText();
                }
                if (params.value("copyAll", false)) {
                    pin->copyAllText();
                }
                if (params.value("extractText", false)) {
                    pin->extractText();
                }
                if (params.value("openComparisonPanel", false)) {
                    pin->openOcrComparisonPanel();
                }
                if (params.value("clearSelection", false)) {
                    pin->clearTextSelection();
                }
                if (params.value("toggleSelectionMode", false)) {
                    pin->toggleTextSelectionMode();
                }
                if (params.value("triggerEnsureOcr", false)) {
                    pin->ensureOcrExecutedAsync();
                }
                if (params.contains("setTheme") && params["setTheme"].is_string()) {
                    tools3000::core::ConfigManager::instance().set<std::string>(
                        "/general/theme", params["setTheme"].get<std::string>());
                }
                if (params.contains("forceHover") && params["forceHover"].is_boolean()) {
                    pin->forceHoverStateForTest(params["forceHover"].get<bool>());
                }
                if (params.contains("setScale") && params["setScale"].is_number()) {
                    pin->setScale(params["setScale"].get<float>());
                }
                if (params.contains("setOpacity") && params["setOpacity"].is_number()) {
                    pin->setOpacity(params["setOpacity"].get<float>());
                }
                if (params.contains("setOcrInFlight") && params["setOcrInFlight"].is_boolean()) {
                    pin->setOcrInFlightForTest(params["setOcrInFlight"].get<bool>());
                }
                if (params.contains("setMarkupColorIndex") && params["setMarkupColorIndex"].is_number_integer()) {
                    pin->setTextMarkupColorIndex(params["setMarkupColorIndex"].get<int>());
                }
                if (params.contains("annotateMode") && params["annotateMode"].is_string()) {
                    const std::string modeStr = params["annotateMode"].get<std::string>();
                    if (modeStr == "highlight") {
                        pin->annotateSelectedText(tools3000::capture::TextMarkupMode::Highlight);
                    } else if (modeStr == "wavy") {
                        pin->annotateSelectedText(tools3000::capture::TextMarkupMode::WavyUnderline);
                    } else if (modeStr == "underline") {
                        pin->annotateSelectedText(tools3000::capture::TextMarkupMode::StraightUnderline);
                    }
                }
                if (params.value("undoMarkup", false)) {
                    pin->undoPinMarkup();
                }
                if (params.value("redoMarkup", false)) {
                    pin->redoPinMarkup();
                }
                if (params.value("startTextReplace", false)) {
                    pin->startInPlaceTextReplace();
                }
                if (params.contains("replaceTypeText") && params["replaceTypeText"].is_string()) {
                    const std::wstring typed = tools3000::core::WinUtils::utf8ToWstring(
                        params["replaceTypeText"].get<std::string>());
                    for (wchar_t ch : typed) {
                        pin->handleTextReplaceChar(ch);
                    }
                }
                if (params.contains("commitTextReplace") && params["commitTextReplace"].is_boolean()) {
                    pin->commitInPlaceTextReplace(params["commitTextReplace"].get<bool>());
                }
                if (params.value("forceRender", false)) {
                    pin->renderForTest();
                }
                if (params.contains("applyEditedRects") && params["applyEditedRects"].is_array()) {
                    const cv::Mat cleanBase = pin->cleanBaseImage().empty()
                        ? pin->sourceImage().clone()
                        : pin->cleanBaseImage().clone();
                    tools3000::capture::MarkupEngine engine;
                    engine.setBaseImage(cleanBase);
                    if (params.value("appendExistingMarkup", true)) {
                        engine.importElements(pin->markupElements());
                    }
                    for (const auto& item : params["applyEditedRects"]) {
                        const int x1 = item.value("x1", 0);
                        const int y1 = item.value("y1", 0);
                        const int x2 = item.value("x2", 20);
                        const int y2 = item.value("y2", 20);
                        tools3000::capture::MarkupColor col{
                            static_cast<uint8_t>(std::clamp(item.value("r", 239), 0, 255)),
                            static_cast<uint8_t>(std::clamp(item.value("g", 68), 0, 255)),
                            static_cast<uint8_t>(std::clamp(item.value("b", 68), 0, 255)),
                            255
                        };
                        auto* rectElem = engine.drawRectangle(cv::Point(x1, y1), cv::Point(x2, y2), col, 2.0f);
                        if (rectElem && item.value("fill", false)) {
                            rectElem->fill = true;
                        }
                    }
                    const cv::Mat precomp = engine.getCompositeImage(false);
                    pin->applyEditedMarkup(cleanBase, engine.exportElements(), precomp);
                }
                RECT winRc{};
                if (pin->hwnd()) GetWindowRect(pin->hwnd(), &winRc);
                int sampleB = -1, sampleG = -1, sampleR = -1;
                if (params.contains("sampleX") && params.contains("sampleY") && !pin->sourceImage().empty()) {
                    const int sx = std::clamp(params.value("sampleX", 0), 0, pin->sourceImage().cols - 1);
                    const int sy = std::clamp(params.value("sampleY", 0), 0, pin->sourceImage().rows - 1);
                    const cv::Mat& srcImg = pin->sourceImage();
                    if (srcImg.channels() >= 3) {
                        const uint8_t* rowPtr = srcImg.ptr<uint8_t>(sy) + sx * srcImg.channels();
                        sampleB = rowPtr[0];
                        sampleG = rowPtr[1];
                        sampleR = rowPtr[2];
                    }
                }
                const auto tbRect = pin->hoverToolbarRect();
                const auto txtRect = pin->textButtonRect();
                const auto invRect = pin->invertButtonRect();
                const auto copyFloatingRect = pin->copyFloatingButtonRect();
                const auto copyTblRect = pin->copyTableFloatingButtonRect();
                const auto openPnlRect = pin->openPanelFloatingButtonRect();
                const auto hlRect = pin->highlightFloatingButtonRect();
                const auto wavyRect = pin->wavyFloatingButtonRect();
                const auto ulRect = pin->underlineFloatingButtonRect();
                const auto trRect = pin->textReplaceFloatingButtonRect();
                const auto trDragRect = pin->textReplaceDragHandleRect();
                const auto undoFloatingRect = pin->undoFloatingButtonRect();
                std::string latestTextReplaceText;
                double latestTextReplaceOffsetX = 0.0;
                double latestTextReplaceOffsetY = 0.0;
                for (auto it = pin->markupElements().rbegin(); it != pin->markupElements().rend(); ++it) {
                    if (it->tool == tools3000::capture::MarkupTool::TextReplace) {
                        latestTextReplaceText = it->text;
                        latestTextReplaceOffsetX = it->replaceOffsetX;
                        latestTextReplaceOffsetY = it->replaceOffsetY;
                        break;
                    }
                }
                return {
                    {"success", true},
                    {"pinHwnd", static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pin->hwnd()))},
                    {"textSelectionMode", pin->isTextSelectionMode()},
                    {"selectedBlockCount", static_cast<int>(pin->selectedTextBlockCount())},
                    {"selectedText", pin->selectedTextUtf8()},
                    {"hasDetectedTable", pin->hasDetectedTable()},
                    {"qrCodeCount", static_cast<int>(pin->qrCodeCount())},
                    {"hoveredBlockIndex", pin->hoveredBlockIndex()},
                    {"copyTableBtnVisible", copyTblRect.right > copyTblRect.left},
                    {"openPanelBtnVisible", openPnlRect.right > openPnlRect.left},
                    {"highlightBtnVisible", hlRect.right > hlRect.left},
                    {"wavyBtnVisible", wavyRect.right > wavyRect.left},
                    {"underlineBtnVisible", ulRect.right > ulRect.left},
                    {"textReplaceBtnVisible", trRect.right > trRect.left},
                    {"editingTextReplace", pin->isEditingTextReplace()},
                    {"latestTextReplaceText", latestTextReplaceText},
                    {"latestTextReplaceOffsetX", latestTextReplaceOffsetX},
                    {"latestTextReplaceOffsetY", latestTextReplaceOffsetY},
                    {"trDragHandleVisible", trDragRect.right > trDragRect.left},
                    {"trDragHandleCenterX", static_cast<int>(std::lround((trDragRect.left + trDragRect.right) * 0.5f))},
                    {"trDragHandleCenterY", static_cast<int>(std::lround((trDragRect.top + trDragRect.bottom) * 0.5f))},
                    {"undoBtnVisible", undoFloatingRect.right > undoFloatingRect.left},
                    {"canUndoPinMarkup", pin->canUndoPinMarkup()},
                    {"canRedoPinMarkup", pin->canRedoPinMarkup()},
                    {"textMarkupColorIndex", pin->textMarkupColorIndex()},
                    {"ocrReady", pin->isOcrReady()},
                    {"ocrInFlight", pin->isOcrInFlight()},
                    {"ocrRunCount", pin->ocrRunCount()},
                    {"ocrPanelText", tools3000::ocr::OcrResultWindow::instance().currentTextUtf8()},
                    {"globalOcrWorkers", tools3000::capture::PinWindow::globalActiveOcrWorkerCount()},
                    {"origWidth", pin->origWidth()},
                    {"origHeight", pin->origHeight()},
                    {"scale", pin->scale()},
                    {"cleanWidth", pin->cleanBaseImage().cols},
                    {"cleanHeight", pin->cleanBaseImage().rows},
                    {"sourceWidth", pin->sourceImage().cols},
                    {"sourceHeight", pin->sourceImage().rows},
                    {"markupCount", static_cast<int>(pin->markupElements().size())},
                    {"windowWidth", static_cast<int>(winRc.right - winRc.left)},
                    {"windowHeight", static_cast<int>(winRc.bottom - winRc.top)},
                    {"sampleB", sampleB},
                    {"sampleG", sampleG},
                    {"sampleR", sampleR},
                    {"isGeneratedCard", pin->isGeneratedCard()},
                    {"groupId", pin->groupId()},
                    {"pinnedSourceText", tools3000::core::WinUtils::wstringToUtf8(pin->pinnedSourceText())},
                    {"hoverToolbarVisible", pin->isHoverToolbarVisible()},
                    {"toolbarWidth", static_cast<double>(tbRect.right - tbRect.left)},
                    {"toolbarHeight", static_cast<double>(tbRect.bottom - tbRect.top)},
                    {"textBtnCenterX", static_cast<int>(std::lround((txtRect.left + txtRect.right) * 0.5f))},
                    {"textBtnCenterY", static_cast<int>(std::lround((txtRect.top + txtRect.bottom) * 0.5f))},
                    {"invertBtnCenterX", static_cast<int>(std::lround((invRect.left + invRect.right) * 0.5f))},
                    {"invertBtnCenterY", static_cast<int>(std::lround((invRect.top + invRect.bottom) * 0.5f))},
                    {"copyFloatingCenterX", static_cast<int>(std::lround((copyFloatingRect.left + copyFloatingRect.right) * 0.5f))},
                    {"copyFloatingCenterY", static_cast<int>(std::lround((copyFloatingRect.top + copyFloatingRect.bottom) * 0.5f))},
                    {"pressedToolbarBtn", pin->pressedToolbarBtn()},
                    {"isDragging", pin->isDragging()},
                    {"isInverted", pin->isInverted()},
                    {"isFolded", pin->isFolded()},
                    {"opacity", pin->opacity()},
                    {"lastChromeIsDark", pin->lastChromeIsDark()},
                    {"lastToolbarIsDark", pin->lastToolbarIsDark()},
                    {"lastRenderedPixelGrid", pin->lastRenderedPixelGrid()},
                    {"lastRenderedOcrBadge", pin->lastRenderedOcrBadge()},
                    {"activeHudBadgeText", tools3000::core::WinUtils::wstringToUtf8(pin->activeHudBadgeText())}
                };
            });
        });

        mb.registerHandler("capture.pinGroupControl", [callOnUiThread](const nlohmann::json& params) -> nlohmann::json {
            return callOnUiThread([params]() -> nlohmann::json {
                tools3000::capture::PinGroupPalette::ensureLoadedFromConfig();
                int autoAssignedGroup = 0;
                bool metaChanged = false;

                if (params.contains("updateGroupMeta") && params["updateGroupMeta"].is_object()) {
                    const auto& meta = params["updateGroupMeta"];
                    const int gid = meta.value("id", 0);
                    if (gid >= tools3000::capture::PinGroupConstants::kMinGroupId &&
                        gid <= tools3000::capture::PinGroupConstants::kMaxGroupId) {
                        std::optional<std::wstring> newName;
                        std::optional<std::string> newColor;
                        if (meta.contains("name") && meta["name"].is_string()) {
                            newName = tools3000::core::WinUtils::utf8ToWstring(meta["name"].get<std::string>());
                        }
                        if (meta.contains("colorHex") && meta["colorHex"].is_string()) {
                            newColor = meta["colorHex"].get<std::string>();
                        }
                        std::optional<std::wstring_view> nameView = newName.has_value()
                            ? std::optional<std::wstring_view>(*newName) : std::nullopt;
                        std::optional<std::string_view> colorView = newColor.has_value()
                            ? std::optional<std::string_view>(*newColor) : std::nullopt;
                        if (tools3000::capture::PinGroupPalette::setGroupCustomization(gid, nameView, colorView)) {
                            if (meta.value("ensureCreated", false)) {
                                tools3000::capture::PinWindow::ensureExplicitGroupCreated(gid);
                            }
                            metaChanged = true;
                        }
                    }
                }
                if (params.contains("renameGroup") && params["renameGroup"].is_object()) {
                    const auto& rn = params["renameGroup"];
                    const int gid = rn.value("id", 0);
                    if (rn.contains("name") && rn["name"].is_string()) {
                        const std::wstring wname = tools3000::core::WinUtils::utf8ToWstring(rn["name"].get<std::string>());
                        if (tools3000::capture::PinGroupPalette::setGroupName(gid, wname)) {
                            metaChanged = true;
                        }
                    }
                }
                if (params.contains("recolorGroup") && params["recolorGroup"].is_object()) {
                    const auto& rc = params["recolorGroup"];
                    const int gid = rc.value("id", 0);
                    if (rc.contains("colorHex") && rc["colorHex"].is_string()) {
                        if (tools3000::capture::PinGroupPalette::setGroupColor(gid, rc["colorHex"].get<std::string>())) {
                            metaChanged = true;
                        }
                    }
                }
                if (params.contains("resetGroupMeta") && params["resetGroupMeta"].is_number_integer()) {
                    tools3000::capture::PinGroupPalette::resetGroupCustomization(params["resetGroupMeta"].get<int>());
                    metaChanged = true;
                }
                if (params.value("resetAllGroupMeta", false)) {
                    tools3000::capture::PinGroupPalette::resetAllCustomizations();
                    metaChanged = true;
                }
                int createdGroup = 0;
                size_t affectedCount = 0;
                if (params.contains("assignLatestGroup") && params["assignLatestGroup"].is_number_integer()) {
                    if (auto pin = tools3000::capture::PinWindow::latestInstance()) {
                        pin->setGroupId(params["assignLatestGroup"].get<int>());
                    }
                }
                if (params.value("createNewGroup", false)) {
                    const bool assignLatest = params.value("assignLatestToNewGroup", false);
                    auto targetPin = assignLatest ? tools3000::capture::PinWindow::latestInstance() : nullptr;
                    createdGroup = tools3000::capture::PinWindow::createAndActivateNewGroup(targetPin.get());
                }
                if (params.value("autoGroupUngrouped", false)) {
                    autoAssignedGroup = tools3000::capture::PinWindow::groupAllUngrouped();
                }
                if (params.contains("filterGroup") && params["filterGroup"].is_number_integer()) {
                    tools3000::capture::PinWindow::setActiveGroupFilter(params["filterGroup"].get<int>());
                }
                if (params.contains("arrangeGroup") && params["arrangeGroup"].is_number_integer()) {
                    tools3000::capture::PinWindow::arrangeGroup(params["arrangeGroup"].get<int>());
                }
                if (params.contains("dissolveGroup") && params["dissolveGroup"].is_number_integer()) {
                    affectedCount = tools3000::capture::PinWindow::dissolveGroup(params["dissolveGroup"].get<int>());
                }
                if (params.contains("closeGroup") && params["closeGroup"].is_number_integer()) {
                    affectedCount = tools3000::capture::PinWindow::closeGroup(params["closeGroup"].get<int>());
                }
                if (params.value("toggleHideAll", false)) {
                    tools3000::capture::PinWindow::toggleHideAll();
                }
                if (params.value("arrangeAll", false)) {
                    tools3000::capture::PinWindow::arrangeAll();
                }
                if (params.value("closeAll", false)) {
                    tools3000::capture::PinWindow::closeAll();
                }

                const bool zh = tools3000::core::isUiLanguageChinese();
                nlohmann::json groupCounts = nlohmann::json::array();
                nlohmann::json groupsJson = nlohmann::json::array();
                if (metaChanged) {
                    tools3000::capture::PinWindow::refreshAllVisualsAndGroupBar();
                }
                const int activeFilter = tools3000::capture::PinWindow::activeGroupFilter();
                nlohmann::json catalogJson = nlohmann::json::array();
                nlohmann::json palettePresetsJson = nlohmann::json::array();
                const auto& presets = tools3000::capture::PinGroupPalette::presetPaletteHexes();
                for (int idx = 0; idx < static_cast<int>(presets.size()); ++idx) {
                    palettePresetsJson.push_back({
                        {"slot", idx + 1},
                        {"hex", std::string(presets[static_cast<size_t>(idx)])},
                        {"name", tools3000::core::WinUtils::wstringToUtf8(
                            std::wstring(tools3000::capture::PinGroupPalette::presetColorName(idx + 1, zh)))}
                    });
                }
                for (int g = 0; g <= tools3000::capture::PinGroupConstants::kMaxGroupId; ++g) {
                    const int cnt = static_cast<int>(tools3000::capture::PinWindow::countInGroup(g));
                    groupCounts.push_back(cnt);
                    if (g >= tools3000::capture::PinGroupConstants::kMinGroupId) {
                        const bool isCreated = tools3000::capture::PinWindow::isExplicitGroupCreated(g);
                        const bool hasCustom = tools3000::capture::PinGroupPalette::hasCustomization(g);
                        auto buildEntry = [&]() -> nlohmann::json {
                            return {
                                {"id", g},
                                {"count", cnt},
                                {"explicitCreated", isCreated},
                                {"hasCustomization", hasCustom},
                                {"shortLabel", tools3000::core::WinUtils::wstringToUtf8(
                                    tools3000::capture::PinGroupPalette::shortLabel(g, zh))},
                                {"fullLabel", tools3000::core::WinUtils::wstringToUtf8(
                                    tools3000::capture::PinGroupPalette::fullLabel(g, zh))},
                                {"customName", tools3000::core::WinUtils::wstringToUtf8(
                                    tools3000::capture::PinGroupPalette::customName(g))},
                                {"customColorHex", tools3000::capture::PinGroupPalette::customColorHex(g)},
                                {"defaultColorHex", tools3000::capture::PinGroupPalette::defaultHex(g)},
                                {"colorHex", tools3000::capture::PinGroupPalette::accentHex(g)}
                            };
                        };
                        if (g <= tools3000::capture::PinGroupConstants::kDefaultQuickGroupCount ||
                            cnt > 0 || isCreated || activeFilter == g || hasCustom) {
                            catalogJson.push_back(buildEntry());
                        }
                        if (cnt > 0 || isCreated || activeFilter == g || hasCustom) {
                            groupsJson.push_back({
                                {"id", g},
                                {"count", cnt},
                                {"explicitCreated", isCreated},
                                {"shortLabel", tools3000::core::WinUtils::wstringToUtf8(
                                    tools3000::capture::PinGroupPalette::shortLabel(g, zh))},
                                {"fullLabel", tools3000::core::WinUtils::wstringToUtf8(
                                    tools3000::capture::PinGroupPalette::fullLabel(g, zh))},
                                {"customName", tools3000::core::WinUtils::wstringToUtf8(
                                    tools3000::capture::PinGroupPalette::customName(g))},
                                {"customColorHex", tools3000::capture::PinGroupPalette::customColorHex(g)},
                                {"defaultColorHex", tools3000::capture::PinGroupPalette::defaultHex(g)},
                                {"hasCustomization", hasCustom},
                                {"colorHex", tools3000::capture::PinGroupPalette::accentHex(g)}
                            });
                        }
                    }
                }
                const int ungroupedCount = groupCounts.empty() ? 0 : groupCounts[0].get<int>();
                auto latest = tools3000::capture::PinWindow::latestInstance();
                return {
                    {"success", true},
                    {"totalCount", static_cast<int>(tools3000::capture::PinWindow::count())},
                    {"ungroupedCount", ungroupedCount},
                    {"activeFilter", activeFilter},
                    {"allHidden", tools3000::capture::PinWindow::isAllHidden()},
                    {"groupBarVisible", tools3000::capture::PinGroupBarOverlay::instance().isVisible()},
                    {"latestGroupId", latest ? latest->groupId() : 0},
                    {"latestHwnd", latest ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(latest->hwnd())) : 0ULL},
                    {"autoAssignedGroup", autoAssignedGroup},
                    {"createdGroup", createdGroup},
                    {"affectedCount", static_cast<int>(affectedCount)},
                    {"maxGroupId", tools3000::capture::PinGroupConstants::kMaxGroupId},
                    {"nextAvailableGroupId", tools3000::capture::PinWindow::nextAvailableGroupId()},
                    {"palettePresets", std::move(palettePresetsJson)},
                    {"catalog", std::move(catalogJson)},
                    {"groupCounts", std::move(groupCounts)},
                    {"groups", std::move(groupsJson)}
                };
            });
        });

        mb.registerHandler("history.getAll", [](const nlohmann::json& params) -> nlohmann::json {
            auto result = nlohmann::json::parse(
                tools3000::capture::CaptureHistory::instance().toMetadataJson());
            if (!params.value("includeThumbnails", false)) return result;

            auto& hist = tools3000::capture::CaptureHistory::instance();
            for (auto& metadata : result) {
                const uint64_t id = metadata.value("id", static_cast<uint64_t>(0));
                const auto thumb = (id > 0)
                    ? hist.getThumbnailById(id)
                    : hist.getThumbnail(metadata.value("index", -1));
                if (!thumb || thumb->empty()) continue;
                std::vector<uint8_t> buffer;
                if (cv::imencode(".png", *thumb, buffer)) {
                    metadata["base64"] = tools3000::core::WinUtils::base64Encode(buffer);
                }
            }
            return result;
        });

        mb.registerHandler("history.getThumbnail", [](const nlohmann::json& params) -> nlohmann::json {
            const uint64_t id = params.value("id", static_cast<uint64_t>(0));
            const int index = params.value("index", -1);
            if (id == 0 && index < 0) return {{"success", false}};
            auto& hist = tools3000::capture::CaptureHistory::instance();
            const auto thumb = (id > 0) ? hist.getThumbnailById(id) : hist.getThumbnail(index);
            if (!thumb || thumb->empty()) return {{"success", false}};
            
            std::vector<uint8_t> buffer;
            cv::imencode(".png", *thumb, buffer);
            std::string base64 = tools3000::core::WinUtils::base64Encode(buffer);
            return {{"success", true}, {"base64", base64}};
        });

        mb.registerHandler("history.remove", [](const nlohmann::json& params) -> nlohmann::json {
            const uint64_t id = params.value("id", static_cast<uint64_t>(0));
            const int index = params.value("index", -1);
            auto& hist = tools3000::capture::CaptureHistory::instance();
            const bool removed = (id > 0) ? hist.removeById(id) : hist.remove(index);
            if (!removed) {
                return {{"success", false}, {"error", "history entry not found"}};
            }
            return {
                {"success", true},
                {"count", hist.count()}
            };
        });

        mb.registerHandler("history.clear", [](const nlohmann::json&) -> nlohmann::json {
            tools3000::capture::CaptureHistory::instance().clear();
            return {{"success", true}};
        });

        mb.registerHandler("history.pushMemoryEntry", [](const nlohmann::json& params) -> nlohmann::json {
            const int width = std::clamp(params.value("width", 64), 1, 4096);
            const int height = std::clamp(params.value("height", 48), 1, 4096);
            const int b = std::clamp(params.value("b", 180), 0, 255);
            const int g = std::clamp(params.value("g", 110), 0, 255);
            const int r = std::clamp(params.value("r", 45), 0, 255);
            cv::Mat synthetic(height, width, CV_8UC3, cv::Scalar(b, g, r));
            CaptureRegion reg{10, 20, width, height};
            tools3000::capture::CaptureHistory::instance().push(synthetic, reg, "");
            return {
                {"success", true},
                {"count", tools3000::capture::CaptureHistory::instance().count()}
            };
        });

        mb.registerHandler("history.open", [callOnUiThread](const nlohmann::json& params) -> nlohmann::json {
            const uint64_t id = params.value("id", static_cast<uint64_t>(0));
            const int index = params.value("index", -1);
            auto& hist = tools3000::capture::CaptureHistory::instance();
            const auto entry = (id > 0) ? hist.getById(id) : hist.get(index);
            if (!entry || entry->image.empty()) {
                return {{"success", false}, {"error", "history entry not found"}};
            }
            return callOnUiThread([entry]() -> nlohmann::json {
                POINT cursor{};
                GetCursorPos(&cursor);
                const auto& reg = entry->region;
                const tools3000::capture::CaptureRegion* pReg = (reg.width > 0 && reg.height > 0) ? &reg : nullptr;
                POINT spawnPos = tools3000::capture::PinWindow::calculateSmartSpawnPosition(entry->image.cols, entry->image.rows, &cursor, pReg);
                const auto pin = tools3000::capture::PinWindow::create(entry->image, spawnPos.x, spawnPos.y);
                return {{"success", static_cast<bool>(pin)}};
            });
        });

        for (const char* asyncMethod : {
                 "recording.getCapabilities",
                 "ocr.recognizeImageFile",
                 "capture.pinImageFile",
                 "capture.copyHistoryItem",
                 "capture.saveHistoryItemAs",
                 "history.getAll",
                 "history.getThumbnail",
                 "translate.testConnection",
             }) {
            mb.markMethodAsync(asyncMethod);
        }

        return true;
    }

    void shutdown() override {
        LOG_INFO("CapturePlugin: 卸载截图/录屏引擎");
        // 先关闭所有外部入口并等待在途回调归零，再停止内部线程和窗口。
        PinWindow::beginModuleShutdown();
        auto& bridge = tools3000::core::MessageBridge::instance();
        bridge.unregisterHandlersByPrefix("capture.");
        bridge.unregisterHandlersByPrefix("recording.");
        bridge.unregisterHandlersByPrefix("ocr.");
        bridge.unregisterHandlersByPrefix("translate.");
        bridge.unregisterHandlersByPrefix("history.");

        auto& hotkeys = tools3000::core::HotkeyManager::instance();
        for (const char* name : {"Screenshot", "Record", "Record Pause", "Record Snapshot", "OCR", "Pin Toggle",
                                 "Pin Paste", "Pin Hide All", "Pin Arrange",
                                 "Mute System Audio", "Mute Microphone"}) {
            hotkeys.unregisterHotkey(name);
        }
        auto& bus = tools3000::core::EventBus::instance();
        bus.unsubscribeAndWait(m_screenshotSubscription);
        bus.unsubscribeAndWait(m_recordingSubscription);
        bus.unsubscribeAndWait(m_themeSubscription);
        bus.unsubscribeAndWait(m_pinImageFileSubId);
        m_screenshotSubscription = 0;
        m_recordingSubscription = 0;
        m_pinImageFileSubId = 0;
        if (m_configChangeId != 0) {
            tools3000::core::ConfigManager::instance().removeOnChange(m_configChangeId);
            m_configChangeId = 0;
        }
        m_themeSubscription = 0;

        // 阶段 1: 先取消并等待(join)所有后台异步 OCR、长截图、录屏与捕获工作线程彻底合并，
        // 确保不会再有残留后台线程向 MainThreadDispatcher 投递 UI 回调
        g_ocrPending.store(false);
        if (g_ocrWorker.joinable()) {
            tools3000::core::joinWorkerWhilePumpingSentMessages(g_ocrWorker, 3000);
        }
        g_ocrRunning.store(false);
        ScrollCapture::instance().shutdown();
        ScreenRecorder::instance().waitForAsyncStop();
        ScreenRecorder::instance().shutdown();
        ScreenRecorder::instance().setStateCallback(nullptr);
        ScreenCapture::instance().shutdown();
        ScreenCapture::instance().setCallback(nullptr);
        PinWindow::closeAll();
        PinWindow::waitForPendingOcrWorkers(5000);

        // 阶段 2: 所有后台任务线程已 100% 停止，最后排空(drain) UI 主线程回调队列并销毁全部 UI 宿主单例
        tools3000::core::MainThreadDispatcher::instance().drain();
        ScrollCaptureOverlay::instance().hide();
        PinContextMenu::instance().shutdown();
        PinGroupBarOverlay::instance().shutdown();
        RecordingIndicator::instance().onPause(nullptr);
        RecordingIndicator::instance().onStop(nullptr);
        RecordingIndicator::instance().onSystemAudioMute(nullptr);
        RecordingIndicator::instance().onMicrophoneMute(nullptr);
        RecordingIndicator::instance().shutdown();
        StudioPreviewWindow::instance().shutdown();
        ShortcutHintOverlay::instance().shutdown();
        tools3000::ocr::OcrResultWindow::instance().cleanup();
        tools3000::ocr::OcrEngine::instance().shutdown();
        AiInpaintEngine::instance().releaseResources();
        TextReplaceEngine::clearPatchCache();
        shutdownMarkupTextRenderer();
    }

private:
    tools3000::core::SubscriptionId m_screenshotSubscription = 0;
    tools3000::core::SubscriptionId m_recordingSubscription = 0;
    tools3000::core::SubscriptionId m_themeSubscription = 0;
    tools3000::core::SubscriptionId m_pinImageFileSubId = 0;
    size_t m_configChangeId = 0;
};

} // namespace tools3000::capture

PLUGIN_API tools3000::core::IPlugin* CreatePlugin() {
    static tools3000::capture::CapturePlugin instance;
    return &instance;
}

PLUGIN_API std::uint32_t GetPluginAbiVersion() {
    return tools3000::core::CurrentPluginAbiVersion;
}
