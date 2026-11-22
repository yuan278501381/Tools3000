#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// TextReplaceEngine.h — 截图原位无痕改字核心引擎 (方案 B: 6 维视觉 DNA 克隆
//                       + 背景渐变平面拟合 + 可选 MI-GAN 512 AI 纹理修复 + DirectWrite 渲染)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_TEXTREPLACEENGINE_H
#define TOOLS3000_CAPTURE_TEXTREPLACEENGINE_H

#include <opencv2/core.hpp>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tools3000::capture {

/// 原位改字字体族类别
enum class TextReplaceFontClass : int {
    UiSans    = 0, // 现代桌面无衬线 (Microsoft YaHei UI / Segoe UI / Noto Sans SC)
    Monospace = 1, // 等宽代码/终端字体 (Cascadia Mono / Consolas)
    Serif     = 2  // 经典衬线/公文宋体 (SimSun / Georgia)
};

/// 原位改字背景擦除策略
enum class TextReplaceBgMode : int {
    Auto       = 0, // 自动判断：纯色/渐变走 0.3ms 平面拟合；复杂纹理/照片优先 MI-GAN AI 修复，未装模型自动回退 Telea
    CleanPlane = 1, // 强制纯色/二维线性渐变平面还原（桌面 UI / 网页 / 表格最稳健）
    AiInpaint  = 2  // 强制局部纹理修复（优先 MI-GAN 512 GPU，未装模型自动回退 Telea）
};

/// 二维线性渐变背景平面 c(dx, dy) = c0 + cx * dx + cy * dy（坐标相对 ROI 中心）
struct LinearBgPlane {
    std::array<float, 3> c0{255.0f, 255.0f, 255.0f}; // B, G, R 基准值
    std::array<float, 3> cx{0.0f, 0.0f, 0.0f};       // 水平梯度
    std::array<float, 3> cy{0.0f, 0.0f, 0.0f};       // 垂直梯度
    float originX = 0.0f;
    float originY = 0.0f;
    float complexity = 0.0f; // 外围环带残差标准差（<= 11.0 判定为纯色/平滑渐变，> 11.0 为复杂纹理/照片）

    cv::Vec3b evalAt(float x, float y) const noexcept;
};

/// 6 维逆向克隆的视觉风格 DNA
struct ExtractedTextStyle {
    LinearBgPlane bgPlane{};
    cv::Vec3b textBgr{30, 41, 59};
    float fontSizePx = 15.0f;
    int fontWeight = 400; // 400 / 500 / 600 / 700
    TextReplaceFontClass fontClass = TextReplaceFontClass::UiSans;
    float inkLeftX = 0.0f;
    float inkRightX = 0.0f;
    float inkTopY = 0.0f;
    float inkBottomY = 0.0f;
    float baselineY = 0.0f;
    float avgCharAdvancePx = 0.0f;
    float inkCoverageSum = 0.0f;
    bool darkBackground = false;
    bool complexBackground = false;
};

/// 编辑态 Direct2D 顶层交互叠加几何（纯矢量测算，不烘焙进图片，支持 60FPS 光标呼吸与鼠标点选）
struct TextReplaceEditOverlayMetrics {
    bool valid = false;
    float boxLeft = 0.0f;
    float boxTop = 0.0f;
    float boxRight = 0.0f;
    float boxBottom = 0.0f;
    float drawTextX = 0.0f;
    float drawTextY = 0.0f;
    float caretX = 0.0f;
    float caretTop = 0.0f;
    float caretBottom = 0.0f;
    bool darkBackground = false;
    std::vector<cv::Rect2f> selectionRects;
};

/// 原位改字单次渲染参数包
struct TextReplaceRenderSpec {
    cv::Rect sourceRect;
    std::vector<cv::Rect> charBoxes;
    std::string originalTextUtf8;
    std::string newTextUtf8;
    cv::Vec3b textBgr{30, 41, 59};
    float fontSizePx = 15.0f;
    int fontWeight = 400;
    TextReplaceFontClass fontClass = TextReplaceFontClass::UiSans;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    TextReplaceBgMode bgMode = TextReplaceBgMode::Auto;
    bool styleInitialized = false;
    bool reflowRightTail = false; // 改长时自动将同行右侧原图文字无损向右顺延让位，绝不压住后面文字
    cv::Rect rightTailRect;       // 同行右侧待顺延原图文字包络盒
    bool isEditing = false;
    size_t caretUtf16 = 0;
    size_t selAnchorUtf16 = 0;
    uint32_t lastCaretTick = 0;
    bool showCaretBlink = true;
};

/// MI-GAN 512 AI 局部无痕修复模型状态快照（供设置页与贴图 UI 查询）
struct AiInpaintModelStatus {
    bool installed = false;
    bool downloading = false;
    int progressPercent = 0;
    uint64_t downloadedBytes = 0;
    uint64_t totalBytes = 28079181ULL;
    std::string state = "not_installed"; // "installed" | "downloading" | "not_installed" | "error"
    std::string modelDir;
    std::string modelFile;
    std::string error;
    bool usingGpu = false;
};

/// 可选按需下载的轻量级 AI 纹理无痕修复引擎 (MI-GAN 512 ONNX · 26.8 MB · WinML DirectML GPU)
class AiInpaintEngine {
public:
    static AiInpaintEngine& instance();

    AiInpaintModelStatus getModelStatus() const;
    bool isModelInstalled() const;
    bool startModelDownload();
    void cancelModelDownload();
    bool deleteModel();

    /// 在给定画布与修复掩码 (mask: 255=需擦除修复区域, 0=保留原图) 上执行 AI 纹理级无痕修复
    /// 返回 false 表示未安装模型或推理不可用，调用方可无缝回退 OpenCV Telea 修复
    bool inpaintRegion(const cv::Mat& bgrCanvas, const cv::Rect& contextRoi,
                       const cv::Mat& eraseMask255, cv::Mat& outRepairedRoiBgr);

    /// 清空推理会话与 LRU 缓存（插件卸载/释放内存时调用）
    void releaseResources();

    /// 查询当前 WinML 推理会话是否驻留于内存（供生命周期单测验证）
    bool isSessionLoaded() const;

    /// 配置闲置自动卸载超时时间（默认 90 秒，供单元测试缩短窗口验证）
    void setIdleUnloadTimeoutMs(uint64_t timeoutMs) noexcept { m_idleUnloadTimeoutMs.store(timeoutMs, std::memory_order_release); }

private:
    AiInpaintEngine() = default;
    ~AiInpaintEngine();
    AiInpaintEngine(const AiInpaintEngine&) = delete;
    AiInpaintEngine& operator=(const AiInpaintEngine&) = delete;

    std::filesystem::path resolveModelDirectory(bool* outInstalled = nullptr) const;
    bool ensureSessionLoadedLocked(const std::filesystem::path& modelPath);
    void ensureIdleReaperStarted();
    void stopIdleReaper();

    struct WinMlInpaintContext;
    std::unique_ptr<WinMlInpaintContext> m_winMl;
    mutable std::mutex m_inferMutex;
    std::atomic<bool> m_usingGpu{false};
    std::atomic<uint64_t> m_lastUsedTick{0};
    std::atomic<uint64_t> m_idleUnloadTimeoutMs{90000ULL};

    std::mutex m_idleMutex;
    std::condition_variable_any m_idleCv;
    std::jthread m_idleReaper;
    std::atomic<bool> m_idleReaperRunning{false};

    mutable std::mutex m_downloadMutex;
    std::thread m_downloadWorker;
    std::atomic<bool> m_downloading{false};
    std::atomic<bool> m_cancelDownload{false};
    std::atomic<int> m_progressPercent{0};
    std::atomic<uint64_t> m_downloadedBytes{0};
    std::atomic<uint64_t> m_totalBytes{28079181ULL};
    std::string m_lastDownloadError;
};

/// 截图原位无痕改字主引擎（无状态静态入口 + 内部擦除背景 LRU 缓存）
class TextReplaceEngine {
public:
    /// 6 维视觉 DNA 逆向克隆（背景平面/复杂度、字色、视觉字号、基线、字重、字体族）
    static ExtractedTextStyle extractStyle(const cv::Mat& canvas,
                                           const cv::Rect& sourceRect,
                                           const std::vector<cv::Rect>& charBoxes,
                                           const std::string& originalTextUtf8);

    /// 渲染原位无痕改字图元至 canvas（100% 纯净无光标污染），并返回实际覆盖外接矩形
    static cv::Rect renderReplacement(cv::Mat& canvas, const TextReplaceRenderSpec& spec);

    /// 计算编辑态 Direct2D 顶层焦框、文字选区高亮矩形与精确插入光标坐标（< 0.1ms 纯矢量计算）
    static TextReplaceEditOverlayMetrics computeEditOverlayMetrics(const cv::Mat& cleanCanvas,
                                                                   const TextReplaceRenderSpec& spec);

    /// 根据图像坐标系下的点击点 (imgX, imgY) 精准命中编辑框内的 UTF-16 光标索引
    static size_t hitTestCaretIndex(const cv::Mat& cleanCanvas,
                                    const TextReplaceRenderSpec& spec,
                                    float imgX,
                                    float imgY);

    /// 清空擦除底图 LRU 缓存（切图/释放内存时调用）
    static void clearPatchCache();
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_TEXTREPLACEENGINE_H
