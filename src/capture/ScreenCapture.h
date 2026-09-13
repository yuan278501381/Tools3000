#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ScreenCapture — 屏幕截图核心引擎
//
// 职责:
//   1. 全屏截图（Windows Graphics Capture API / BitBlt 降级）
//   2. 区域选择（创建选区覆盖层）
//   3. 窗口检测与自动吸附（光标悬停窗口高亮）
//   4. 截图输出到剪贴板/文件/内存
//   5. 多显示器支持
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_SCREENCAPTURE_H
#define TOOLS3000_CAPTURE_SCREENCAPTURE_H

#include <windows.h>
#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>

// OpenCV 前向声明
namespace cv { class Mat; }

namespace tools3000::capture {

/// 截图格式
enum class ImageFormat {
    PNG,
    JPEG,
    WebP,
    BMP,
};

/// 截图区域
struct CaptureRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    float cornerRadius = 0.0f;

    bool isValid() const { return width > 0 && height > 0; }
};

/// 截图选项
struct CaptureOptions {
    ImageFormat format = ImageFormat::PNG;
    int quality = 95;                  // JPEG/WebP 质量 (1-100)
    bool copyToClipboard = true;       // 复制到剪贴板
    bool saveToFile = false;           // 保存到文件
    std::string savePath;              // 保存路径（空则自动生成）
    bool autoNumber = false;           // 标注时自动编号
    bool showCrosshair = false;        // 选区前显示全屏十字对齐线 (默认关闭)
    bool autoDetectWindow = true;      // 光标悬停时检测并吸附窗口
    bool showShortcutHints = true;     // 操作期间在当前显示器左下角显示轻量快捷键提示
    bool autoBypassFullscreen = true;  // 前台处于全屏独占应用时自动免打扰 (默认开启)
    bool includeCursor = false;        // 截图时合成/保留系统鼠标光标 (默认关闭)
};

/// 截图结果
struct CaptureResult {
    bool success = false;
    std::string filePath;              // 文件保存路径（如有）
    CaptureRegion region;              // 实际截取区域
    int imageWidth = 0;
    int imageHeight = 0;
    std::string errorMessage;
};

/// 截图完成回调
using CaptureCallback = std::function<void(const CaptureResult& result)>;

/// 将 OpenCV Mat 以标准 PNG (32位透明通道) 与标准底向上 CF_DIB 双格式安全写入 Windows 剪贴板
bool copyMatToClipboard(const cv::Mat& image);

class ScreenCapture {
public:
    bool copyToClipboard(const cv::Mat& image, const std::wstring& filePath = L"", const CaptureRegion* sourceRegion = nullptr, int padX = 0, int padY = 0);
public:
    static ScreenCapture& instance();

    /// 初始化截图系统
    bool initialize(HINSTANCE hInstance);

    /// 关闭截图系统
    void shutdown();

    /// 开始截图流程（进入区域选择模式）
    void startCapture(const CaptureOptions& options = {});

    /// 快速全屏截图（跳过区域选择）
    CaptureResult captureFullScreen(const CaptureOptions& options = {});

    /// 截取指定窗口
    CaptureResult captureWindow(HWND hwnd, const CaptureOptions& options = {});

    /// 截取指定区域
    CaptureResult captureRegion(const CaptureRegion& region, const CaptureOptions& options = {});

    /// 设置截图完成回调
    void setCallback(CaptureCallback callback) { m_callback = std::move(callback); }

    /// 是否正在截图
    bool isCapturing() const { return m_capturing.load(); }

private:
    ScreenCapture() = default;
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    /// 统一屏幕捕获（优先硬件加速后端 DXGI / WGC，自动回退到 GDI BitBlt）
    std::unique_ptr<cv::Mat> captureScreen(const CaptureRegion& region);

    /// 使用 BitBlt 截取屏幕区域（兼容性方案）
    std::unique_ptr<cv::Mat> captureScreenBitBlt(const CaptureRegion& region);

    /// 将 cv::Mat 编码为指定格式的字节流
    std::vector<uint8_t> encodeImage(const cv::Mat& image, ImageFormat format, int quality);

    /// 将字节流复制到剪贴板
    

    /// 保存到文件
    std::string saveToFile(const std::vector<uint8_t>& data, const std::string& path,
                           ImageFormat format);

    /// 生成自动保存路径
    std::string generateSavePath(ImageFormat format) const;

    /// 格式对应的文件扩展名
    static std::string formatExtension(ImageFormat format);

    CaptureCallback m_callback;
    std::atomic<bool> m_capturing{false};
    HINSTANCE m_hInstance = nullptr;
    CaptureOptions m_activeOptions;
    std::jthread m_ocrWorker;
    std::atomic<bool> m_ocrRunning{false};
};

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_SCREENCAPTURE_H
