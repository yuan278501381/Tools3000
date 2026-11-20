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

    inline constexpr int kDetMaxSideLen = 1280;
    inline constexpr int kDetStrideAlign = 32;
    inline constexpr int kDetBucketAlign = 64;
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
    inline constexpr int kRecMediumBucketWidth = 640;
    inline constexpr int kRecLongBucketWidth = 960;
    inline constexpr int kRecFixedStripWidth = 960;
    inline constexpr int kRecMaxBatchSize = 32;
    inline constexpr int kRecMinWidth = 32;
    inline constexpr int kRecMaxWidth = 3200;
    inline constexpr float kRecMinLineConfidence = 0.42f;

    inline constexpr float kAxisAlignedMaxSkewPx = 1.8f;
    inline constexpr size_t kWatermarkFilterMinOrthoQuads = 4;
    inline constexpr float kWatermarkMinTiltDeg = 14.0f;
    inline constexpr float kWatermarkMaxTiltDeg = 68.0f;
    inline constexpr int kWatermarkFaintMaxContrast = 48;
    inline constexpr int kWarmupInitialDelayMs = 900;

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

    /// 后台空闲低优先级异步加载模型并预编译固定形状 DirectML 算子管线（消灭首次识字冷启动延迟）
    void warmupAsync(int delayMs = DeepOcrConstants::kWarmupInitialDelayMs);

    /// 当前模型与 GPU 固定桶管线是否已完成内存加载
    bool isWarmAndLoaded() const noexcept;

    /// 停机并安全等待后台下载/预热线程退出
    void shutdown();

    // ── 纯算法核心原语（供生产推理与高覆盖率离线单元测试直接调用） ───────────

    /// 检测图像是否属于低反差/深灰底/摩尔纹屏摄或实拍照片（原生清晰桌面截图快速返回 false，零开销直通）
    static bool isLowContrastScreenPhoto(const cv::Mat& bgr);

    /// 面向 DBNet 检测分支的轻量 LAB 色度去彩纹 + 局部 CLAHE 自适应增强（原生清晰截图零改动直通）
    static cv::Mat enhanceLowContrastForDetection(const cv::Mat& bgr);

    /// 合同/发票/发货单红章压字穿透滤除：仅在浅色纸面上定位与黑色文字墨水产生真实交叉压覆的高饱和红色印章连通域，
    /// 将红章柔和中和至周边纸面底色同时 100% 保护黑字核心笔画与独立红色警示文字（零误伤）
    static cv::Mat suppressRedStampForOcr(const cv::Mat& bgr, bool* outSuppressed = nullptr);

    /// 检测存在明显手影/台灯光照不均的纸质单据/手写笔记本/白板实拍照片（原生数字截图与含大块暗前景场景直接返回 false）
    static bool isShadowedDocumentPhoto(const cv::Mat& bgr);

    /// 纸面/手写拍照光照归一化去阴影漂白：大核背景估计除法抹平手影暗角并凸显淡笔迹反差，数字截图原样透传
    static cv::Mat enhanceDocumentPhoto(const cv::Mat& bgr);

    /// 统一 OCR 检测前自适应视觉增强管线（红章穿透 + 手写纸面去阴影 / 拍屏摩尔纹抑制）
    static cv::Mat preprocessImageForDetection(const cv::Mat& bgr, bool* outApplyToRecognition = nullptr);

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

    /// 合并同一水平基线上被 DBNet 切碎的极小标点/短值碎片（如 `:`、`"-"`），同时跳过像素级下拉箭头图标
    static void coalesceSameLineTinyFragments(
        std::vector<DbDetectedQuad>& quads,
        const cv::Mat& bgr);

    /// 对轴对齐水平文本框执行防碰撞墨水感知边界校准：挽救贴边标点（如尾随 `,` 与首部 `{`）并估算局部干净背景色
    static cv::Rect refineAxisAlignedLineBox(
        const cv::Mat& bgr,
        const cv::Rect& box,
        const std::vector<cv::Rect>& allBoxes,
        size_t selfIdx,
        cv::Vec3b* outBgColor = nullptr,
        bool* outUniformBg = nullptr);

    /// 像素级验证候选区域是否为 UI 下拉框 V/^ 形对称箭头图标（零误伤真实字母 v/V 与勾选符）
    static bool isUiDropdownChevronPatch(
        const cv::Mat& bgr,
        const cv::Rect& candidateRect,
        const cv::Rect* prevTextInkRect = nullptr);

    /// 从识别结果中剥离经像素几何验证的按钮尾部/独立 UI 下拉箭头图标，并严格保持 text 与 charBoxes 1:1 同步
    static void stripUiDropdownChevrons(
        const cv::Mat& bgr,
        std::vector<OcrResult>& blocks);

    /// 像素级漏识下划线还原：对洁净背景行中 ASCII 字母/数字间的空格校验底部独立下划线笔画，精准还原 `_` 且免疫超链接通长下划线
    static void rescueMissedUnderscores(
        const cv::Mat& bgr,
        OcrResult& item);

    /// 针对明确 JSON 键值语法行将结构位上的中文全角引号/冒号/逗号及失真括号规整为标准 ASCII（保留中文值内部标点及 charBoxes 1:1 对齐）
    static void normalizeStructuredJsonPunctuation(OcrResult& item);

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
    void precompileFixedBucketsLocked();
    void downloadWorkerLoop(std::stop_token stopToken, std::filesystem::path targetDir);

    mutable std::mutex m_inferMutex;
    std::atomic<bool> m_modelsLoadedAtomic{false};
    bool m_modelsLoaded = false;
    bool m_bucketsPrecompiled = false;
    bool m_useWinMl = false;
    std::filesystem::path m_loadedModelDir;
    std::unique_ptr<WinMlContext> m_winMl;
    cv::dnn::Net m_detNet;
    cv::dnn::Net m_clsNet;
    cv::dnn::Net m_recNetShort;
    cv::dnn::Net m_recNetLong;
    std::vector<std::string> m_dictKeys;

    mutable std::mutex m_warmupMutex;
    std::jthread m_warmupThread;
    std::atomic<bool> m_shuttingDown{false};

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
