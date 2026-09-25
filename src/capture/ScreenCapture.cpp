// ─────────────────────────────────────────────────────────────────────────────
// ScreenCapture.cpp — 屏幕截图核心引擎实现
//
// 截图方案:
//   - 主方案: BitBlt GDI 截图（兼容性最好）
//   - 使用 OpenCV 编码/解码图像
//   - 剪贴板: CF_DIB + CF_BITMAP 双格式写入
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/ScreenCapture.h"
#include "capture/ClipboardUtils.h"
#include "capture/CaptureBackend.h"
#include "capture/CursorOverlay.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "capture/CaptureOverlay.h"
#include "core/logger/Logger.h"
#include "core/stats/PerformanceMonitor.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "core/config/ConfigManager.h"
#include "ocr/OcrEngine.h"
#include "ocr/OcrResultWindow.h"
#include "capture/ScrollCapture.h"
#include "capture/ScrollCaptureOverlay.h"
#include "capture/CaptureHistory.h"
#include "capture/ShortcutHintOverlay.h"
#include "common/AtomicFile.h"

#include <opencv2/opencv.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <thread>

namespace tools3000::capture {

ScreenCapture& ScreenCapture::instance() {
    static ScreenCapture inst;
    return inst;
}

bool ScreenCapture::initialize(HINSTANCE hInstance) {
    m_hInstance = hInstance;

    // 初始化截图覆盖层
    auto& overlay = CaptureOverlay::instance();
    overlay.initialize(hInstance);
    overlay.setClosedCallback([this]() { m_capturing.store(false); });
    overlay.setCallback([this](const CaptureRegion& region, const cv::Mat& markedImage,
                               const CaptureCompletion& completion) {
        tools3000::core::TraceId::Scope scope;
        CaptureResult result;
        result.region = region;
        result.imageWidth = markedImage.cols;
        result.imageHeight = markedImage.rows;

        const bool explicitSave = completion.action == CaptureCompletionAction::SaveAs &&
                                  !completion.filePath.empty();
        if ((explicitSave || m_activeOptions.saveToFile) && !markedImage.empty()) {
            const auto format = explicitSave ? completion.format : m_activeOptions.format;
            auto encoded = encodeImage(markedImage, format, m_activeOptions.quality);
            if (!encoded.empty()) {
                const std::string path = explicitSave
                    ? completion.filePath
                    : m_activeOptions.savePath.empty()
                        ? generateSavePath(format)
                        : m_activeOptions.savePath;
                result.filePath = saveToFile(encoded, path, format);
                if (explicitSave) {
                    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                        result.filePath.empty() ? L"截图保存失败" : L"截图已保存"});
                }
            }
        }

        // 复制到剪贴板 (传入已持久化或临时文件路径，赋予桌面与资源管理器直接 Ctrl+V 原生粘贴真实图片的能力)
        if ((m_activeOptions.copyToClipboard ||
             completion.action == CaptureCompletionAction::Copy ||
             completion.action == CaptureCompletionAction::Default) &&
            !markedImage.empty()) {
            const std::wstring widePath = tools3000::core::WinUtils::utf8ToWstring(result.filePath);
            int padX = (markedImage.cols > region.width && region.width > 0) ? (markedImage.cols - region.width) / 2 : 0;
            int padY = (markedImage.rows > region.height && region.height > 0) ? (markedImage.rows - region.height) / 2 : 0;
            copyToClipboard(markedImage, widePath, &region, padX, padY);
            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{L"截图已复制到剪贴板"});
        }

        result.success = true;
        LOG_INFO("截图完成: {}x{}", result.imageWidth, result.imageHeight);

        // 将截图保存到历史
        CaptureHistory::instance().push(markedImage, region, result.filePath);

        if (m_callback) m_callback(result);
    });

    overlay.setOcrCallback([this]([[maybe_unused]] const CaptureRegion& region, const cv::Mat& cropped) {
        if (cropped.empty()) return;
        if (m_ocrRunning.exchange(true)) {
            tools3000::core::EventBus::instance().publish(
                tools3000::core::ShowToastEvent{L"OCR 正在识别，请稍候"});
            return;
        }
        
        const bool showResultWindow = tools3000::core::ConfigManager::instance().get<bool>(
            "/ocr/showResultWindow", true);
        if (showResultWindow) {
            tools3000::ocr::OcrResultWindow::instance().showResult("识别中... (Recognizing...)");
        }

        // OCR(WinRT .get()) 放后台线程: 避免在主 STA 线程阻塞/冻结 UI; 全程 try/catch 兜底。
        std::string traceId = tools3000::core::TraceId::current();
        cv::Mat img = cropped.clone();
        if (m_ocrWorker.joinable()) m_ocrWorker.join();
        m_ocrWorker = std::jthread([this, img = std::move(img), traceId]() {
            tools3000::core::TraceId::setCurrent(traceId);
            std::string displayText;
            try {
                auto results = tools3000::ocr::OcrEngine::instance().extractText(img);
                std::string fullText;
                for (const auto& r : results) fullText += r.text + "\r\n";
                if (!fullText.empty()) {
                    if (tools3000::core::ConfigManager::instance().get<bool>("/ocr/copyResult", true)) {
                        tools3000::core::WinUtils::copyToClipboard(fullText);
                    }
                    LOG_INFO("OCR 提取完成, 行数={}", results.size());
                    displayText = std::move(fullText);
                } else {
                    LOG_WARN("OCR 未提取到文字");
                    displayText = "未识别到文字 (No text recognized)";
                }
            } catch (const std::exception& e) {
                LOG_ERROR("OCR(覆盖层) 异常: {}", e.what());
                displayText = "识别失败 (Recognition failed)";
            } catch (...) {
                LOG_ERROR("OCR(覆盖层) 未知异常");
                displayText = "识别失败 (Unknown error)";
            }
            m_ocrRunning.store(false);
            if (tools3000::core::ConfigManager::instance().get<bool>("/ocr/showResultWindow", true)) {
                tools3000::core::MainThreadDispatcher::instance().post([text = std::move(displayText)]() {
                    tools3000::ocr::OcrResultWindow::instance().showResult(text);
                });
            } else {
                tools3000::core::EventBus::instance().publish(
                    tools3000::core::ShowToastEvent{L"OCR 识别完成"});
                tools3000::core::WinUtils::trimWorkingSet();
            }
        });
    });

    ScrollCapture::instance().setProgressCallback([](const cv::Mat& preview, int frameCount) {
        ScrollCaptureOverlay::instance().updatePreview(preview, frameCount);
    });
    ScrollCapture::instance().setCompletionCallback([this](const ScrollCaptureResult& result) {
        tools3000::core::TraceId::Scope scope;
        ShortcutHintOverlay::instance().hide();
        tools3000::core::MainThreadDispatcher::instance().post([] {
            ScrollCaptureOverlay::instance().hide();
        });
        if (!result.success || result.stitchedImage.empty()) {
            LOG_ERROR("长截图失败: {}", result.errorMessage);
            tools3000::core::EventBus::instance().publish(
                tools3000::core::ShowToastEvent{L"长截图失败，请重试"});
            return;
        }

        bool copied = false;
        if (m_activeOptions.copyToClipboard) {
            copied = copyToClipboard(result.stitchedImage);
        }

        std::string savedPath;
        if (m_activeOptions.saveToFile) {
            auto encoded = encodeImage(result.stitchedImage, m_activeOptions.format, m_activeOptions.quality);
            if (!encoded.empty()) {
                std::string path = m_activeOptions.savePath.empty()
                    ? generateSavePath(m_activeOptions.format)
                    : m_activeOptions.savePath;
                savedPath = saveToFile(encoded, path, m_activeOptions.format);
            }
        }
        CaptureRegion historyRegion{};
        historyRegion.width = result.stitchedImage.cols;
        historyRegion.height = result.stitchedImage.rows;
        CaptureHistory::instance().push(result.stitchedImage, historyRegion, savedPath);
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            savedPath.empty()
                ? (copied ? L"长截图已复制到剪贴板" : L"长截图已完成")
                : L"长截图已保存"});
        LOG_INFO("长截图已保存，共 {} 帧", result.frameCount);
    });

    LOG_INFO("截图引擎已初始化");
    return true;
}

void ScreenCapture::shutdown() {
    ScrollCapture::instance().shutdown();
    ScrollCaptureOverlay::instance().hide();
    if (m_ocrWorker.joinable()) m_ocrWorker.join();
    m_ocrRunning.store(false);
    CaptureOverlay::instance().shutdown();
    m_capturing.store(false);
    LOG_INFO("截图引擎已关闭");
}

// ─────────────────────────────────────────────────────────────────────────────
// 截图入口
// ─────────────────────────────────────────────────────────────────────────────

void ScreenCapture::startCapture(const CaptureOptions& options) {
    if (options.autoBypassFullscreen) {
        HWND fg = GetForegroundWindow();
        if (fg && tools3000::core::WinUtils::shouldBypassFullscreenInteractions(fg)) {
            LOG_INFO("前台处于全屏独占应用，自动免打扰跳过截图: hwnd=0x{:X}", reinterpret_cast<uintptr_t>(fg));
            return;
        }
    }

    // 防御性状态自愈：如果底层覆盖层已闲置，强制重置 m_capturing 标志
    if (CaptureOverlay::instance().state() == OverlayState::Idle) {
        m_capturing.store(false);
    }

    bool expected = false;
    if (!m_capturing.compare_exchange_strong(expected, true)) {
        LOG_WARN("截图已在进行中");
        return;
    }

    tools3000::core::TraceId::Scope scope;
    m_activeOptions = options;

    // 启动区域选择覆盖层
    CaptureOverlay::instance().startSelection(options);

}

CaptureResult ScreenCapture::captureFullScreen(const CaptureOptions& options) {
    tools3000::core::TraceId::Scope scope;
    LOG_INFO("执行全屏截图");

    // 获取虚拟屏幕区域（多显示器）
    CaptureRegion fullRegion;
    fullRegion.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    fullRegion.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    fullRegion.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    fullRegion.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    return captureRegion(fullRegion, options);
}

CaptureResult ScreenCapture::captureWindow(HWND hwnd, const CaptureOptions& options) {
    tools3000::core::TraceId::Scope scope;

    if (!hwnd || !IsWindow(hwnd)) {
        return { false, "", {}, 0, 0, "无效的窗口句柄" };
    }

    RECT rc;
    GetWindowRect(hwnd, &rc);

    CaptureRegion region;
    region.x = rc.left;
    region.y = rc.top;
    region.width = rc.right - rc.left;
    region.height = rc.bottom - rc.top;

    LOG_INFO("截取窗口: hwnd={}, region=({},{})x{}x{}", 
             reinterpret_cast<uintptr_t>(hwnd), region.x, region.y, region.width, region.height);

    return captureRegion(region, options);
}

CaptureResult ScreenCapture::captureRegion(const CaptureRegion& region, const CaptureOptions& options) {
    tools3000::core::TraceId::Scope scope;
    CaptureResult result;

    if (!region.isValid() || region.width > 32768 || region.height > 32768) {
        result.errorMessage = "无效的截图区域";
        LOG_ERROR("截图失败: {}", result.errorMessage);
        return result;
    }

    m_activeOptions = options;

    // 截取屏幕
    tools3000::core::PerfTimer captureTimer("screenshot");
    auto image = captureScreen(region);
    captureTimer.stop();
    if (!image || image->empty()) {
        result.errorMessage = "屏幕截图失败";
        LOG_ERROR("截图失败: {}", result.errorMessage);
        return result;
    }

    result.region = region;
    result.imageWidth = image->cols;
    result.imageHeight = image->rows;

    // 复制到剪贴板
    if (options.copyToClipboard) {
        if (copyToClipboard(*image, L"", &region)) {
            LOG_DEBUG("截图已复制到剪贴板");
        } else {
            LOG_WARN("复制到剪贴板失败");
        }
    }

    // 保存到文件
    if (options.saveToFile) {
        auto encoded = encodeImage(*image, options.format, options.quality);
        if (!encoded.empty()) {
            std::string path = options.savePath.empty() 
                ? generateSavePath(options.format) 
                : options.savePath;
            result.filePath = saveToFile(encoded, path, options.format);
            if (!result.filePath.empty()) {
                LOG_INFO("截图已保存: path={}, size={}KB", result.filePath, encoded.size() / 1024);
            }
        }
    }

    result.success = true;
    LOG_INFO("截图完成: {}x{}, format={}", result.imageWidth, result.imageHeight,
             formatExtension(options.format));

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 统一屏幕截取策略（加速后端 + 降级方案）
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<cv::Mat> ScreenCapture::captureScreen(const CaptureRegion& region) {
    auto backend = createCaptureBackend();
    std::string backendError;
    if (backend && backend->initialize(region, backendError)) {
        CaptureFrameView frame;
        if (backend->capture(frame, backendError) && frame.data && frame.width > 0 && frame.height > 0) {
            if (m_activeOptions.includeCursor) {
                CursorOverlay cursorOverlay;
                cursorOverlay.applyPermanent(frame, region, true);
            }
            auto mat = std::make_unique<cv::Mat>();
            if (frame.format == CapturePixelFormat::Bgr24) {
                *mat = cv::Mat(frame.height, frame.width, CV_8UC3, frame.data, frame.stride).clone();
            } else {
                cv::Mat bgra(frame.height, frame.width, CV_8UC4, frame.data, frame.stride);
                cv::cvtColor(bgra, *mat, cv::COLOR_BGRA2BGR);
            }
            backend->releaseFrame();
            backend->shutdown();
            return mat;
        }
        backend->releaseFrame();
        backend->shutdown();
    }
    return captureScreenBitBlt(region);
}

// ─────────────────────────────────────────────────────────────────────────────
// BitBlt 屏幕截取
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<cv::Mat> ScreenCapture::captureScreenBitBlt(const CaptureRegion& region) {
    struct GdiCaptureGuard {
        HDC screen = nullptr;
        HDC memory = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        ~GdiCaptureGuard() {
            if (memory && previous && previous != HGDI_ERROR) SelectObject(memory, previous);
            if (bitmap) DeleteObject(bitmap);
            if (memory) DeleteDC(memory);
            if (screen) ReleaseDC(nullptr, screen);
        }
    } resources;

    resources.screen = GetDC(nullptr);
    if (!resources.screen) {
        LOG_ERROR("获取屏幕 DC 失败, error={}", GetLastError());
        return nullptr;
    }
    resources.memory = CreateCompatibleDC(resources.screen);
    if (!resources.memory) {
        LOG_ERROR("创建截图内存 DC 失败, error={}", GetLastError());
        return nullptr;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = region.width;
    bitmapInfo.bmiHeader.biHeight = -region.height; // top-down，避免后续翻转
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 24;           // 直接得到 OpenCV BGR
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    resources.bitmap = CreateDIBSection(
        resources.screen, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!resources.bitmap || !pixels) {
        LOG_ERROR("创建截图 DIBSection 失败, error={}", GetLastError());
        return nullptr;
    }
    resources.previous = SelectObject(resources.memory, resources.bitmap);
    if (!resources.previous || resources.previous == HGDI_ERROR) {
        LOG_ERROR("选择截图位图失败, error={}", GetLastError());
        return nullptr;
    }

    // BitBlt 截取屏幕区域
    BOOL ok = BitBlt(resources.memory, 0, 0, region.width, region.height,
                     resources.screen, region.x, region.y, SRCCOPY | CAPTUREBLT);

    if (!ok) {
        LOG_ERROR("BitBlt 失败, error={}", GetLastError());
        return nullptr;
    }

    // DIBSection 已经是顶向 BGR。由于位图内存随 HBITMAP 销毁，返回前仅需
    // 一次紧凑 clone；相比旧路径省去 32 位临时图和 BGRA→BGR 颜色转换。
    const size_t stride = (static_cast<size_t>(region.width) * 3U + 3U) & ~size_t{3U};
    const cv::Mat dibView(region.height, region.width, CV_8UC3, pixels, stride);
    auto image = std::make_unique<cv::Mat>(dibView.clone());

    if (m_activeOptions.includeCursor && image && !image->empty()) {
        CaptureFrameView frameView;
        frameView.data = image->data;
        frameView.width = image->cols;
        frameView.height = image->rows;
        frameView.stride = static_cast<int>(image->step[0]);
        frameView.format = CapturePixelFormat::Bgr24;
        CursorOverlay cursorOverlay;
        cursorOverlay.applyPermanent(frameView, region, true);
    }

    return image;
}

// ─────────────────────────────────────────────────────────────────────────────
// 图像编码
// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> ScreenCapture::encodeImage(const cv::Mat& image, ImageFormat format, int quality) {
    std::vector<uint8_t> buffer;
    std::vector<int> params;

    switch (format) {
        case ImageFormat::PNG:
            params = { cv::IMWRITE_PNG_COMPRESSION, 6 };
            cv::imencode(".png", image, buffer, params);
            break;

        case ImageFormat::JPEG:
            params = { cv::IMWRITE_JPEG_QUALITY, quality };
            cv::imencode(".jpg", image, buffer, params);
            break;

        case ImageFormat::WebP:
            params = { cv::IMWRITE_WEBP_QUALITY, quality };
            cv::imencode(".webp", image, buffer, params);
            break;

        case ImageFormat::BMP:
            cv::imencode(".bmp", image, buffer);
            break;
    }

    return buffer;
}

// ─────────────────────────────────────────────────────────────────────────────
// 剪贴板
// ─────────────────────────────────────────────────────────────────────────────

bool ScreenCapture::copyToClipboard(const cv::Mat& image, const std::wstring& filePath, const CaptureRegion* sourceRegion, int padX, int padY) {
    return ClipboardUtils::copyImageToClipboard(image, filePath, nullptr, sourceRegion, padX, padY);
}

// ─────────────────────────────────────────────────────────────────────────────
// 文件操作
// ─────────────────────────────────────────────────────────────────────────────

std::string ScreenCapture::saveToFile(const std::vector<uint8_t>& data, 
                                       const std::string& path,
                                       [[maybe_unused]] ImageFormat format) {
    try {
        if (data.empty() || path.empty()) {
            LOG_ERROR("保存截图失败: 数据或路径为空");
            return "";
        }
        // 确保目录存在
        const auto widePath = tools3000::core::WinUtils::utf8ToWstring(path);
        auto dir = std::filesystem::path(widePath).parent_path();
        if (!dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                LOG_ERROR("无法创建截图保存目录: path={}, error={}", path, ec.message());
                return "";
            }
        }

        if (!tools3000::common::atomicWriteBinaryFileWithFlush(widePath, data.data(), data.size())) {
            LOG_ERROR("原子写入或刷新截图文件失败: {}", path);
            return "";
        }

        return path;
    } catch (const std::exception& e) {
        LOG_ERROR("保存截图失败: {}", e.what());
        return "";
    }
}

std::string ScreenCapture::generateSavePath(ImageFormat format) const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm localTime{};
    localtime_s(&localTime, &time_t);

    std::ostringstream oss;
    oss << "Tools3000_" 
        << std::put_time(&localTime, "%Y%m%d_%H%M%S")
        << "_" << std::setfill('0') << std::setw(3) << ms.count()
        << "." << formatExtension(format);

    // 保存到用户图片目录
    auto picturesDir = tools3000::core::WinUtils::getAppDataDirectory() / L"Screenshots";
    return tools3000::core::WinUtils::wstringToUtf8((picturesDir / oss.str()).wstring());
}

std::string ScreenCapture::formatExtension(ImageFormat format) {
    switch (format) {
        case ImageFormat::PNG:  return "png";
        case ImageFormat::JPEG: return "jpg";
        case ImageFormat::WebP: return "webp";
        case ImageFormat::BMP:  return "bmp";
        default: return "png";
    }
}

}  // namespace tools3000::capture

