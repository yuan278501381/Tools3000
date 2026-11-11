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
#include "capture/ClipboardUtils.h"
#include "capture/ScreenRecorder.h"
#include "capture/RecordingIndicator.h"
#include "capture/PinWindow.h"
#include "capture/PinGroupBarOverlay.h"
#include "capture/CaptureOverlay.h"
#include "capture/CaptureHistory.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/ScrollCapture.h"
#include "capture/ScrollCaptureOverlay.h"
#include "common/AtomicFile.h"
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
                                    if (!path.empty()) {
                                        ind.showCompleted(path, dur);
                                        const bool isWeChatLimit = stopReason == "gif_wechat_limit";
                                        const bool diskFailure = stopReason == "low_disk_space" ||
                                            stopReason == "disk_space_query_failed";
                                        std::wstring toastMsg = isWeChatLimit
                                            ? (tools3000::core::WinUtils::isSystemLanguageChinese() ? L"GIF 已达 20MB 上限并自动保存" : L"GIF reached 20 MB limit and saved")
                                            : (diskFailure
                                                ? (tools3000::core::WinUtils::isSystemLanguageChinese() ? L"磁盘空间不足，已保存当前录屏" : L"Low disk space, recording saved")
                                                : (tools3000::core::WinUtils::isSystemLanguageChinese() ? L"录屏已停止并保存" : L"Recording stopped and saved"));
                                        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{toastMsg});
                                    } else {
                                        ind.hide();
                                        tools3000::core::EventBus::instance().publish(
                                            tools3000::core::ShowToastEvent{L"录屏已停止，未生成有效内容"});
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
                ShortcutHintOverlay::instance().show(
                    state == RecordState::Paused
                        ? ShortcutHintContext::RecordingPaused
                        : ShortcutHintContext::Recording,
                    {LONG_MIN, LONG_MIN},
                    indicator.avoidanceRects());
            }
        });
    });
}

void stopRecordingWithFeedback() {
    auto& recorder = ScreenRecorder::instance();
    if (recorder.isStopping()) {
        return;
    }
    auto& indicator = RecordingIndicator::instance();
    ShortcutHintOverlay::instance().hide();
    tools3000::core::MessageBridge::instance().invokeHandler("keycast.setDockingRegion", {{"active", false}});
    const double duration = recorder.stats().durationSec;
    indicator.showSaving();
    recorder.stopRecordingAsync([duration](const std::string& path, const RecordStats& finalStats) {
        tools3000::core::MainThreadDispatcher::instance().post(
            [path, dur = (finalStats.durationSec > 0.0 ? finalStats.durationSec : duration)]() {
                auto& ind = RecordingIndicator::instance();
                if (!path.empty()) {
                    LOG_INFO("录屏已保存: {}", path);
                    ind.showCompleted(path, dur);
                    tools3000::core::EventBus::instance().publish(
                        tools3000::core::ShowToastEvent{L"录屏已保存"});
                } else {
                    ind.hide();
                    tools3000::core::EventBus::instance().publish(
                        tools3000::core::ShowToastEvent{L"录屏已停止，未生成有效内容"});
                }
            });
    });
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

void startRecordingForRegion(const CaptureRegion& rawRegion, const RecordSetupOptions& setup) {
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
            {region.x + region.width / 2, region.y + region.height / 2},
            RecordingIndicator::instance().avoidanceRects());
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
static std::jthread g_ocrWorker;

static void releaseOcrTransientMemory() {
    tools3000::core::WinUtils::trimWorkingSet();
}

static void startMemoryOcrTask(const cv::Mat& capturedImage) {
    if (capturedImage.empty()) return;
    if (g_ocrRunning.exchange(true)) {
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"OCR 正在识别，请稍候"});
        return;
    }

    const bool showResultWindow = tools3000::core::ConfigManager::instance().get<bool>(
        "/ocr/showResultWindow", true);
    if (showResultWindow) {
        tools3000::core::MainThreadDispatcher::instance().post([]() {
            tools3000::ocr::OcrResultWindow::instance().showResult("识别中... (Recognizing...)");
        });
    }

    cv::Mat img = capturedImage.clone();
    if (g_ocrWorker.joinable()) g_ocrWorker.join();
    g_ocrWorker = std::jthread([img = std::move(img)]() {
        try {
            auto results = tools3000::ocr::OcrEngine::instance().extractText(img);
            std::string fullText;
            for (const auto& r : results) {
                fullText += r.text + "\r\n";
            }
            if (fullText.empty()) {
                if (tools3000::core::ConfigManager::instance().get<bool>("/ocr/showResultWindow", true)) {
                    tools3000::core::MainThreadDispatcher::instance().post([]() {
                        tools3000::ocr::OcrResultWindow::instance().showResult(
                            "未识别到文字 (No text recognized)");
                    });
                } else {
                    tools3000::core::EventBus::instance().publish(
                        tools3000::core::ShowToastEvent{L"未识别到文字"});
                }
            } else {
                const bool copy = tools3000::core::ConfigManager::instance().get<bool>("/ocr/copyResult", true);
                const bool showResult = tools3000::core::ConfigManager::instance().get<bool>("/ocr/showResultWindow", true);
                if (copy) {
                    tools3000::core::WinUtils::copyToClipboard(fullText);
                }
                if (showResult) {
                    tools3000::core::MainThreadDispatcher::instance().post([text = std::move(fullText)]() {
                        tools3000::ocr::OcrResultWindow::instance().showResult(text);
                    });
                } else {
                    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                        copy ? L"文字已复制到剪贴板" : L"文字识别完成"});
                    releaseOcrTransientMemory();
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("OCR 后台线程异常: {}", e.what());
            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{L"OCR 识别失败"});
        } catch (...) {
            LOG_ERROR("OCR 后台线程未知异常");
            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{L"OCR 识别失败"});
        }
        g_ocrRunning.store(false);
    });
}

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
                        copy ? L"文字已复制到剪贴板" : L"文字识别完成"});
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
    auto opts = configuredCaptureOptions();
    if (opts.autoBypassFullscreen) {
        HWND fg = GetForegroundWindow();
        if (fg && tools3000::core::WinUtils::shouldBypassCaptureForWindow(fg)) {
            return;
        }
    }
    auto& ocr = tools3000::ocr::OcrEngine::instance();
    if (!ocr.isAvailable()) {
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{L"未找到 OCR 语言包"});
        return;
    }
    LOG_INFO("OCR Capture Triggered");
    // 纯内存直达识别：零截图剪贴板污染、零临时文件落盘、零幽灵历史记录
    opts.copyToClipboard = false;
    opts.saveToFile = false;
    opts.savePath.clear();
    opts.recordHistory = false;
    opts.onImageCaptured = [](const cv::Mat& image, const CaptureRegion&) {
        startMemoryOcrTask(image);
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
                {"sizeUnit", config.get<std::string>("/capture/sizeUnit", "px")},
                {"showSizePosition", config.get<bool>("/capture/showSizePosition", true)},
                {"showSizeUnit", config.get<bool>("/capture/showSizeUnit", true)}
            };
        });

        mb.registerHandler("capture.updateSettings", [runOnUiThread](const nlohmann::json& params) -> nlohmann::json {
            static const std::unordered_set<std::string> formats = {"png", "jpg", "jpeg", "webp", "bmp"};
            static const std::unordered_set<std::string> themes = {
                "studio_slate", "paper_chalk", "silk_mist", "midnight_graphite", "pure_minimal",
                "aurora_purple", "sunrise_coral", "emerald_sea", "obsidian_dark", "minimal_light"
            };
            static const std::unordered_set<std::string> sizeUnits = {"px", "dp"};
            static const std::unordered_set<std::string> boolKeys = {
                "saveToFile", "copyToClipboard", "includeCursor", "showCrosshair", "autoDetectWindow",
                "showShortcutHints", "autoBypassFullscreen", "beautyShellEnabled",
                "showSizePosition", "showSizeUnit"
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
                if (key == "sizeUnit" && (!value.is_string() || !sizeUnits.contains(value.get<std::string>())))
                    return {{"success", false}, {"error", "invalid size unit"}};
                if (boolKeys.contains(key) && !value.is_boolean())
                    return {{"success", false}, {"error", key + " must be boolean"}};
                if (key != "format" && key != "quality" && key != "saveDirectory" &&
                    key != "beautyShellTheme" && key != "beautyShellPadding" && key != "beautyShellRadius" &&
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

        mb.registerHandler("ocr.openResultWindow", [callOnUiThread](const nlohmann::json& params) -> nlohmann::json {
            const std::string text = params.value("text", "");
            return callOnUiThread([text]() -> nlohmann::json {
                auto& win = tools3000::ocr::OcrResultWindow::instance();
                win.showResult(text);
                RECT rc{};
                if (win.hwnd()) GetClientRect(win.hwnd(), &rc);
                const float cw = static_cast<float>((std::max)(1L, rc.right - rc.left));
                return {
                    {"success", win.hwnd() != nullptr && IsWindow(win.hwnd())},
                    {"lineCount", static_cast<int>(win.lineCount())},
                    {"maxScroll", win.maxScroll()},
                    {"singleLineFooter", win.isFooterHintSingleLine(cw)}
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
            if (path.empty()) return {{"success", false}, {"error", "path is required"}};
            try {
                const auto filePath = std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(path));
                std::ifstream stream(filePath, std::ios::binary);
                if (!stream) return {{"success", false}, {"error", "file not found"}};
                std::vector<unsigned char> bytes(
                    (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                cv::Mat image = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
                if (!image.empty()) {
                    return callOnUiThread([img = std::move(image)]() -> nlohmann::json {
                        POINT pt;
                        GetCursorPos(&pt);
                        POINT spawnPos = tools3000::capture::PinWindow::calculateSmartSpawnPosition(img.cols, img.rows, &pt);
                        const auto pin = tools3000::capture::PinWindow::create(img, spawnPos.x, spawnPos.y);
                        return {{"success", static_cast<bool>(pin)}};
                    });
                }
            } catch (const std::exception& e) {
                LOG_ERROR("贴图文件加载失败: {}", e.what());
                return {{"success", false}, {"error", e.what()}};
            }
            return {{"success", false}, {"error", "unsupported or invalid image"}};
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
                        blocks.push_back(std::move(b));
                    }
                    pin->setOcrBlocksForTest(std::move(blocks));
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
                if (params.value("clearSelection", false)) {
                    pin->clearTextSelection();
                }
                if (params.value("toggleSelectionMode", false)) {
                    pin->toggleTextSelectionMode();
                }
                if (params.value("triggerEnsureOcr", false)) {
                    pin->ensureOcrExecutedAsync();
                }
                if (params.contains("forceHover") && params["forceHover"].is_boolean()) {
                    pin->forceHoverStateForTest(params["forceHover"].get<bool>());
                }
                if (params.contains("setScale") && params["setScale"].is_number()) {
                    pin->setScale(params["setScale"].get<float>());
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
                return {
                    {"success", true},
                    {"textSelectionMode", pin->isTextSelectionMode()},
                    {"selectedBlockCount", static_cast<int>(pin->selectedTextBlockCount())},
                    {"ocrReady", pin->isOcrReady()},
                    {"ocrInFlight", pin->isOcrInFlight()},
                    {"ocrRunCount", pin->ocrRunCount()},
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
                    {"pressedToolbarBtn", pin->pressedToolbarBtn()},
                    {"isDragging", pin->isDragging()},
                    {"isInverted", pin->isInverted()},
                    {"isFolded", pin->isFolded()}
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
                const auto entry = (id > 0)
                    ? hist.getById(id)
                    : hist.get(metadata.value("index", -1));
                if (!entry || entry->thumbnail.empty()) continue;
                std::vector<uint8_t> buffer;
                if (cv::imencode(".png", entry->thumbnail, buffer)) {
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
            const auto entry = (id > 0) ? hist.getById(id) : hist.get(index);
            if (!entry || entry->thumbnail.empty()) return {{"success", false}};
            
            std::vector<uint8_t> buffer;
            cv::imencode(".png", entry->thumbnail, buffer);
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
        m_screenshotSubscription = 0;
        m_recordingSubscription = 0;
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
        PinGroupBarOverlay::instance().shutdown();
        RecordingIndicator::instance().onPause(nullptr);
        RecordingIndicator::instance().onStop(nullptr);
        RecordingIndicator::instance().onSystemAudioMute(nullptr);
        RecordingIndicator::instance().onMicrophoneMute(nullptr);
        RecordingIndicator::instance().shutdown();
        ShortcutHintOverlay::instance().shutdown();
        tools3000::ocr::OcrResultWindow::instance().cleanup();
        tools3000::ocr::OcrEngine::instance().shutdown();
        shutdownMarkupTextRenderer();
    }

private:
    tools3000::core::SubscriptionId m_screenshotSubscription = 0;
    tools3000::core::SubscriptionId m_recordingSubscription = 0;
    tools3000::core::SubscriptionId m_themeSubscription = 0;
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
