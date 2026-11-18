#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// DeepOcrEngine — 基于 OpenCV 4.12 cv::dnn::Net 的 PP-OCRv4 标准 ONNX 离线推理引擎
//
// 核心能力:
//   1. 零外部推理框架 DLL 膨胀，100% 复用项目已链接的 opencv_dnn 模块
//   2. 自动探测 <DataDir>/models/ocr/ 与 <ExeDir>/models/ocr/ 下的 PP-OCRv4 模型包
//   3. 完整实现 DBNet 检测后处理 (阈值分割、多边形打分、Unclip 法向外扩、透视变换倾斜矫正)
//   4. 实现 180° 方向分类器与 SVTR/CRNN CTC Greedy 解码 + 单字符级时间步精确 X 坐标切分
//   5. 内置安全 WinHTTP HTTPS 多镜像流式下载器 (实时进度、ONNX 头部/体积校验、原子替换、随时取消)
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_OCR_DEEPOCRENGINE_H
#define TOOLS3000_OCR_DEEPOCRENGINE_H

#include "ocr/OcrEngine.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>
#include <string>
#include <thread>
#include <vector>

namespace tools3000::ocr {

namespace DeepOcrConstants {
    inline constexpr const wchar_t* kDetModelFileName = L"ch_PP-OCRv4_det.onnx";
    inline constexpr const wchar_t* kClsModelFileName = L"ch_ppocr_mobile_v2.0_cls.onnx";
    inline constexpr const wchar_t* kRecModelFileName = L"ch_PP-OCRv4_rec.onnx";
    inline constexpr const wchar_t* kKeysDictFileName = L"ppocr_keys_v1.txt";

    inline constexpr int kDetMaxSideLen = 1600;
    inline constexpr int kDetStrideAlign = 32;
    inline constexpr float kDetBinaryThresh = 0.28f;
    inline constexpr float kDetBoxScoreThresh = 0.50f;
    inline constexpr float kDetUnclipRatio = 1.65f;
    inline constexpr float kDetMinBoxSidePx = 3.0f;
    inline constexpr int kDetMaxCandidates = 1000;

    inline constexpr int kClsImgHeight = 48;
    inline constexpr int kClsImgWidth = 192;
    inline constexpr float kClsRotate180Thresh = 0.90f;

    inline constexpr int kRecImgHeight = 48;
    inline constexpr int kRecShortBucketWidth = 320;
    inline constexpr int kRecLongBucketWidth = 960;
    inline constexpr int kRecMinWidth = 32;
    inline constexpr int kRecMaxWidth = 3200;
    inline constexpr float kRecMinLineConfidence = 0.42f;

    inline constexpr size_t kMinDetModelBytes = 512 * 1024;      // >= 512 KiB
    inline constexpr size_t kMinClsModelBytes = 64 * 1024;       // >= 64 KiB
    inline constexpr size_t kMinRecModelBytes = 1024 * 1024;     // >= 1 MiB
    inline constexpr size_t kMinDictKeyLines  = 1000;
    inline constexpr uint64_t kEstimatedBundleBytes = 16600000ULL; // ~16.6 MB
} // namespace DeepOcrConstants

struct DeepOcrModelStatus {
    bool installed = false;
    bool downloading = false;
    int progressPercent = 0;
    uint64_t downloadedBytes = 0;
    uint64_t totalBytes = DeepOcrConstants::kEstimatedBundleBytes;
    std::string state; // "installed" | "downloading" | "not_installed" | "error"
    std::string currentFile;
    std::string modelDir;
    std::string error;
};

struct DbDetectedQuad {
    std::array<cv::Point2f, 4> corners{}; // [TL, TR, BR, BL]
    float score = 0.0f;
};

struct CtcDecodedChar {
    std::string utf8Char;
    float confidence = 0.0f;
    int startStep = 0;
    int peakStep = 0;
    int endStep = 0;
    float normLeft = 0.0f;
    float normRight = 1.0f;
};

struct CtcDecodeOutput {
    std::string text;
    float confidence = 0.0f;
    std::vector<CtcDecodedChar> chars;
};

class DeepOcrEngine {
public:
    static DeepOcrEngine& instance();

    /// 检查本地是否已完整安装 PP-OCRv4 模型包 (4 个核心文件齐全且合法)
    bool isModelInstalled() const;

    /// 返回当前生效的模型目录（若未安装则返回默认目标安装目录 <DataDir>/models/ocr）
    std::filesystem::path resolveModelDirectory(bool* outInstalled = nullptr) const;

    /// 获取默认下载安装目标目录 (<DataDir>/models/ocr)
    std::filesystem::path defaultInstallDirectory() const;

    /// 供离线单元测试注入临时模型目录（传空恢复默认探测）
    void setCustomModelDirectoryForTest(const std::filesystem::path& customDir);

    /// 使用 PP-OCRv4 执行端到端离线高精度文字检测 + 方向矫正 + CTC 字符级识别
    std::vector<OcrResult> extractText(const cv::Mat& image);

    /// 查询模型安装与后台下载实时状态
    DeepOcrModelStatus getModelStatus() const;

    /// 启动后台安全 HTTPS 模型包流式下载
    bool startModelDownload();

    /// 取消正在进行的后台模型下载
    void cancelModelDownload();

    /// 释放内存中的 cv::dnn::Net 权重缓存（冷路径回收物理内存）
    void releaseLoadedModels();

    /// 停机并安全等待后台下载线程退出
    void shutdown();

    // ── 纯算法核心原语（供生产推理与高覆盖率离线单元测试直接调用） ───────────

    /// DBNet 概率图后处理：二值化、轮廓过滤、多边形均分校验、Unclip 外扩与四点正交排序
    static std::vector<DbDetectedQuad> postprocessDbMap(
        const cv::Mat& probMap,
        int origWidth,
        int origHeight,
        float ratioW,
        float ratioH,
        float binaryThresh = DeepOcrConstants::kDetBinaryThresh,
        float boxScoreThresh = DeepOcrConstants::kDetBoxScoreThresh,
        float unclipRatio = DeepOcrConstants::kDetUnclipRatio);

    /// 对四点倾斜文本框执行透视变换矫正拉平，并输出逆变换矩阵用于反算单字符原图坐标
    static cv::Mat warpPerspectiveQuad(
        const cv::Mat& image,
        const std::array<cv::Point2f, 4>& quad,
        cv::Mat* outInverseTransform = nullptr,
        bool* outRotatedVertical = nullptr);

    /// SVTR / CRNN CTC Greedy 解码 + 单字符级时间步精确水平区间切分
    static CtcDecodeOutput decodeCtcGreedy(
        const cv::Mat& timeByClassMat,
        const std::vector<std::string>& dictKeys,
        float validWidthRatio = 1.0f);

    /// 将裁剪图上的归一化字符区间经逆透视矩阵映射回原图物理像素包围盒
    static std::vector<cv::Rect> projectCharSpansToImage(
        const std::string& lineText,
        const std::vector<CtcDecodedChar>& decodedChars,
        int cropWidth,
        int cropHeight,
        bool rotated180,
        bool rotatedVertical,
        const cv::Mat& inversePerspective,
        const cv::Rect& fallbackLineBox,
        int maxImageWidth,
        int maxImageHeight);

    /// 校验 ONNX 模型二进制头部合法性 (Protobuf ModelProto 魔数与体积阈值)
    static bool isValidOnnxBuffer(const uint8_t* data, size_t byteSize, size_t minBytes = 256) noexcept;

    /// 从 PP-OCRv4 识别模型 (ch_PP-OCRv4_rec.onnx) 内嵌的 ModelProto.metadata_props ("character") 提取完整字典行
    static bool extractEmbeddedCharacterDictFromOnnx(
        const uint8_t* data,
        size_t byteSize,
        std::vector<std::string>& outKeys);

    /// 在内存中将 ONNX ModelProto 首个 Graph Input 张量形状固化为 [N, C, H, W]，消除 OpenCV DNN 对符号动态维度的 Reshape 折叠失配
    static bool patchOnnxGraphInputShape(
        const uint8_t* data,
        size_t byteSize,
        const std::array<int64_t, 4>& fixedDims,
        std::vector<uint8_t>& outPatchedBytes);

private:
    struct WinMlContext;

    DeepOcrEngine();
    ~DeepOcrEngine();
    DeepOcrEngine(const DeepOcrEngine&) = delete;
    DeepOcrEngine& operator=(const DeepOcrEngine&) = delete;

    bool ensureModelsLoadedLocked(const std::filesystem::path& modelDir);
    void downloadWorkerLoop(std::stop_token stopToken, std::filesystem::path targetDir);

    mutable std::mutex m_inferMutex;
    bool m_modelsLoaded = false;
    bool m_useWinMl = false;
    std::filesystem::path m_loadedModelDir;
    std::unique_ptr<WinMlContext> m_winMl;
    cv::dnn::Net m_detNet;
    cv::dnn::Net m_clsNet;
    cv::dnn::Net m_recNetShort;
    cv::dnn::Net m_recNetLong;
    std::vector<std::string> m_dictKeys;

    mutable std::mutex m_pathMutex;
    std::filesystem::path m_customTestModelDir;

    mutable std::mutex m_downloadMutex;
    std::jthread m_downloadThread;
    std::atomic<bool> m_downloading{false};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<int> m_progressPercent{0};
    std::atomic<uint64_t> m_downloadedBytes{0};
    std::atomic<uint64_t> m_totalBytes{DeepOcrConstants::kEstimatedBundleBytes};
    std::string m_currentDownloadFile;
    std::string m_lastDownloadError;
};

} // namespace tools3000::ocr

#endif // TOOLS3000_OCR_DEEPOCRENGINE_H
