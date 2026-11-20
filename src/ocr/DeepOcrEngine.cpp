// ─────────────────────────────────────────────────────────────────────────────
// DeepOcrEngine.cpp — PP-OCRv4 离线推理运行态核心（WinML DirectML / OpenCV DNN 管线、预热与主推理）
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/DeepOcrEngineInternal.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <opencv2/imgproc.hpp>

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.AI.MachineLearning.h>

#include <windows.h>
#include <objidl.h>
#include <MemoryBuffer.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <numeric>
#include <thread>
#include <utility>

namespace tools3000::ocr {

using namespace deep_ocr_detail;
namespace waml = winrt::Windows::AI::MachineLearning;

struct DeepOcrEngine::WinMlContext {
    struct StaticBucketSpec {
        int n = 1;
        int h = 48;
        int w = 960;
    };

    // 3 档零重编译检测画布桶: [0]=960x640 (中小截图), [1]=1280x1024 (横屏/方屏高分屏大图), [2]=960x1280 (竖屏长图)
    static constexpr std::array<StaticBucketSpec, 3> kDetSpecs = {{
        {1, 640, 960},
        {1, 1024, 1280},
        {1, 1280, 960},
    }};

    // 7 档单行隔离零重编译识别微批次桶（彻底消除多行横向拼接的 SVTR 全局注意力串扰与边界拖影）:
    //   Tier S (W=320): [0]=1x320, [1]=16x320, [2]=32x320
    //   Tier M (W=640): [3]=1x640, [4]=16x640
    //   Tier L (W=960): [5]=1x960, [6]=8x960
    static constexpr std::array<StaticBucketSpec, 7> kRecSpecs = {{
        {1,  DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecShortBucketWidth},
        {16, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecShortBucketWidth},
        {32, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecShortBucketWidth},
        {1,  DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecMediumBucketWidth},
        {16, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecMediumBucketWidth},
        {1,  DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecLongBucketWidth},
        {8,  DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecLongBucketWidth},
    }};

    waml::LearningModel detModel{nullptr};
    waml::LearningModel clsModel{nullptr};
    waml::LearningModel recModel{nullptr};
    waml::LearningModelDevice device{nullptr};

    waml::LearningModelSession clsSession{nullptr};
    std::array<waml::LearningModelSession, 3> detBucketSessions{nullptr, nullptr, nullptr};
    std::array<waml::LearningModelSession, 7> recBucketSessions{
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    waml::LearningModelSession cpuFallbackRecSession{nullptr};

    bool usingDirectMLGpu = false;
    bool supportsBatchEval = false;
    winrt::hstring detInName;
    winrt::hstring detOutName;
    winrt::hstring clsInName;
    winrt::hstring clsOutName;
    winrt::hstring recInName;
    winrt::hstring recOutName;

    waml::LearningModelSession* ensureDetSession(size_t idx) {
        if (idx >= kDetSpecs.size()) return nullptr;
        if (!detBucketSessions[idx]) {
            waml::LearningModelSessionOptions opts;
            opts.CloseModelOnSessionCreation(false);
            detBucketSessions[idx] = waml::LearningModelSession(detModel, device, opts);
        }
        return &detBucketSessions[idx];
    }

    waml::LearningModelSession* ensureRecSession(size_t idx) {
        if (!usingDirectMLGpu) {
            if (!cpuFallbackRecSession) {
                waml::LearningModelSessionOptions opts;
                opts.CloseModelOnSessionCreation(false);
                cpuFallbackRecSession = waml::LearningModelSession(recModel, device, opts);
            }
            return &cpuFallbackRecSession;
        }

        if (idx >= kRecSpecs.size()) return nullptr;
        if (!supportsBatchEval) {
            // DirectML 非批处理模式仅使用各宽度档的 N=1 静态会话 ([0]=1x320, [3]=1x640, [5]=1x960)
            if (idx == 1 || idx == 2) idx = 0;
            else if (idx == 4) idx = 3;
            else if (idx == 6) idx = 5;
        }

        if (!recBucketSessions[idx]) {
            waml::LearningModelSessionOptions opts;
            opts.CloseModelOnSessionCreation(false);
            recBucketSessions[idx] = waml::LearningModelSession(recModel, device, opts);
        }
        return &recBucketSessions[idx];
    }
};

namespace {

void ensureComApartmentInitialized() noexcept {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error const& e) {
        if (e.code() != RPC_E_CHANGED_MODE) {
            // Ignore if already initialized in STA/MTA
        }
    } catch (...) {
    }
}

struct WinMlTensorBufferLock {
    winrt::Windows::Foundation::IMemoryBufferReference ref{nullptr};
    float* data = nullptr;
    size_t floatCount = 0;

    explicit WinMlTensorBufferLock(const waml::TensorFloat& tensor) noexcept {
        if (!tensor) return;
        try {
            ref = tensor.CreateReference();
            auto byteAccess = ref.as<::Windows::Foundation::IMemoryBufferByteAccess>();
            uint8_t* rawBytes = nullptr;
            uint32_t capBytes = 0;
            if (byteAccess && SUCCEEDED(byteAccess->GetBuffer(&rawBytes, &capBytes)) &&
                rawBytes != nullptr && capBytes >= sizeof(float)) {
                data = reinterpret_cast<float*>(rawBytes);
                floatCount = static_cast<size_t>(capBytes) / sizeof(float);
            }
        } catch (...) {
            data = nullptr;
            floatCount = 0;
        }
    }

    void release() noexcept {
        data = nullptr;
        floatCount = 0;
        if (ref) {
            try {
                ref.Close();
            } catch (...) {
            }
            ref = nullptr;
        }
    }

    ~WinMlTensorBufferLock() {
        release();
    }
};

cv::Mat ensureBgr3(const cv::Mat& src) {
    if (src.empty()) return {};
    cv::Mat bgr;
    if (src.channels() == 3) {
        bgr = src;
    } else if (src.channels() == 4) {
        cv::cvtColor(src, bgr, cv::COLOR_BGRA2BGR);
    } else if (src.channels() == 1) {
        cv::cvtColor(src, bgr, cv::COLOR_GRAY2BGR);
    }
    return bgr;
}

cv::Mat makeDbNetBlob(const cv::Mat& bgrResized) {
    // PaddleOCR DBNet NormalizeImage: BGR float32 in [0, 1], mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]
    const int h = bgrResized.rows;
    const int w = bgrResized.cols;
    const int dims[4] = {1, 3, h, w};
    cv::Mat blob(4, dims, CV_32F);

    constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
    constexpr float kInvStd[3] = {1.0f / 0.229f, 1.0f / 0.224f, 1.0f / 0.225f};
    constexpr float kInv255 = 1.0f / 255.0f;

    float* c0 = blob.ptr<float>(0, 0);
    float* c1 = blob.ptr<float>(0, 1);
    float* c2 = blob.ptr<float>(0, 2);

    for (int y = 0; y < h; ++y) {
        const auto* row = bgrResized.ptr<cv::Vec3b>(y);
        const size_t base = static_cast<size_t>(y) * static_cast<size_t>(w);
        for (int x = 0; x < w; ++x) {
            c0[base + x] = (static_cast<float>(row[x][0]) * kInv255 - kMean[0]) * kInvStd[0];
            c1[base + x] = (static_cast<float>(row[x][1]) * kInv255 - kMean[1]) * kInvStd[1];
            c2[base + x] = (static_cast<float>(row[x][2]) * kInv255 - kMean[2]) * kInvStd[2];
        }
    }
    return blob;
}

cv::Mat makeRecOrClsBlob(const cv::Mat& bgrCrop, int targetH, int resizedW, int paddedW) {
    cv::Mat resized;
    cv::resize(bgrCrop, resized, cv::Size(resizedW, targetH), 0.0, 0.0, cv::INTER_LINEAR);

    const int dims[4] = {1, 3, targetH, paddedW};
    cv::Mat blob(4, dims, CV_32F, cv::Scalar(0.0f));

    float* c0 = blob.ptr<float>(0, 0);
    float* c1 = blob.ptr<float>(0, 1);
    float* c2 = blob.ptr<float>(0, 2);
    constexpr float kScale = 2.0f / 255.0f;

    for (int y = 0; y < targetH; ++y) {
        const auto* row = resized.ptr<cv::Vec3b>(y);
        const size_t base = static_cast<size_t>(y) * static_cast<size_t>(paddedW);
        for (int x = 0; x < resizedW; ++x) {
            c0[base + x] = static_cast<float>(row[x][0]) * kScale - 1.0f;
            c1[base + x] = static_cast<float>(row[x][1]) * kScale - 1.0f;
            c2[base + x] = static_cast<float>(row[x][2]) * kScale - 1.0f;
        }
    }
    return blob;
}

} // namespace

namespace deep_ocr_detail {

void cleanupDeepOcrWorkingSet() {
    tools3000::core::WinUtils::trimWorkingSet();
}

} // namespace deep_ocr_detail

DeepOcrEngine::DeepOcrEngine() = default;

DeepOcrEngine::~DeepOcrEngine() {
    shutdown();
}

DeepOcrEngine& DeepOcrEngine::instance() {
    static DeepOcrEngine inst;
    return inst;
}

bool DeepOcrEngine::ensureModelsLoadedLocked(const std::filesystem::path& modelDir) {
    if (m_modelsLoaded && m_loadedModelDir == modelDir && !m_dictKeys.empty() &&
        ((m_useWinMl && m_winMl != nullptr) ||
         (!m_detNet.empty() && !m_recNetShort.empty() && !m_recNetLong.empty()))) {
        return true;
    }

    m_modelsLoaded = false;
    m_modelsLoadedAtomic.store(false, std::memory_order_release);
    m_bucketsPrecompiled = false;
    m_useWinMl = false;
    m_winMl.reset();
    m_detNet = cv::dnn::Net{};
    m_clsNet = cv::dnn::Net{};
    m_recNetShort = cv::dnn::Net{};
    m_recNetLong = cv::dnn::Net{};
    m_dictKeys.clear();

    try {
        const auto detPath = modelDir / DeepOcrConstants::kDetModelFileName;
        const auto clsPath = modelDir / DeepOcrConstants::kClsModelFileName;
        const auto recPath = modelDir / DeepOcrConstants::kRecModelFileName;
        const auto dictPath = modelDir / DeepOcrConstants::kKeysDictFileName;

        std::vector<uint8_t> detBytes, clsBytes, recBytes;
        if (!readFileBytes(detPath, detBytes) ||
            !isValidOnnxBuffer(detBytes.data(), detBytes.size(), 256)) {
            LOG_WARN("DeepOcrEngine: invalid or missing det ONNX model");
            return false;
        }
        if (!readFileBytes(clsPath, clsBytes) ||
            !isValidOnnxBuffer(clsBytes.data(), clsBytes.size(), 256)) {
            LOG_WARN("DeepOcrEngine: invalid or missing cls ONNX model");
            return false;
        }
        if (!readFileBytes(recPath, recBytes) ||
            !isValidOnnxBuffer(recBytes.data(), recBytes.size(), 256)) {
            LOG_WARN("DeepOcrEngine: invalid or missing rec ONNX model");
            return false;
        }
        if (!loadDictionaryLines(dictPath, m_dictKeys)) {
            if (!extractEmbeddedCharacterDictFromOnnx(recBytes.data(), recBytes.size(), m_dictKeys)) {
                LOG_WARN("DeepOcrEngine: invalid or missing dictionary keys file");
                return false;
            }
            LOG_INFO("DeepOcrEngine: loaded {} dictionary keys directly from rec ONNX metadata", m_dictKeys.size());
        }

        // 优先挂载系统内置 Windows.AI.MachineLearning + DirectML 原生 D3D12 固定形状多桶管线
        try {
            ensureComApartmentInitialized();
            auto ctx = std::make_unique<WinMlContext>();
            ctx->detModel = waml::LearningModel::LoadFromFilePath(detPath.wstring());
            ctx->clsModel = waml::LearningModel::LoadFromFilePath(clsPath.wstring());
            ctx->recModel = waml::LearningModel::LoadFromFilePath(recPath.wstring());

            if (ctx->detModel.InputFeatures().Size() > 0 && ctx->detModel.OutputFeatures().Size() > 0 &&
                ctx->clsModel.InputFeatures().Size() > 0 && ctx->clsModel.OutputFeatures().Size() > 0 &&
                ctx->recModel.InputFeatures().Size() > 0 && ctx->recModel.OutputFeatures().Size() > 0) {
                ctx->detInName = ctx->detModel.InputFeatures().GetAt(0).Name();
                ctx->detOutName = ctx->detModel.OutputFeatures().GetAt(0).Name();
                ctx->clsInName = ctx->clsModel.InputFeatures().GetAt(0).Name();
                ctx->clsOutName = ctx->clsModel.OutputFeatures().GetAt(0).Name();
                ctx->recInName = ctx->recModel.InputFeatures().GetAt(0).Name();
                ctx->recOutName = ctx->recModel.OutputFeatures().GetAt(0).Name();

                bool gpuReady = false;
                try {
                    waml::LearningModelDevice gpuDev(waml::LearningModelDeviceKind::DirectXHighPerformance);
                    waml::LearningModelSessionOptions probeOpts;
                    probeOpts.CloseModelOnSessionCreation(false);
                    waml::LearningModelSession gpuCls(ctx->clsModel, gpuDev, probeOpts);

                    bool batchOk = false;
                    try {
                        waml::LearningModelSession s16(ctx->recModel, gpuDev, probeOpts);
                        auto t16 = waml::TensorFloat::Create(winrt::single_threaded_vector<int64_t>({
                            16, 3, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecShortBucketWidth}));
                        waml::LearningModelBinding b16(s16);
                        b16.Bind(ctx->recInName, t16);
                        auto out16 = s16.Evaluate(b16, L"").Outputs().Lookup(ctx->recOutName).as<waml::TensorFloat>();
                        if (out16 && out16.Shape().Size() >= 3 && out16.Shape().GetAt(0) == 16) {
                            ctx->recBucketSessions[1] = std::move(s16);
                            batchOk = true;
                        }
                    } catch (...) {
                        batchOk = false;
                    }

                    if (!batchOk) {
                        waml::LearningModelSession s1(ctx->recModel, gpuDev, probeOpts);
                        auto t1 = waml::TensorFloat::Create(winrt::single_threaded_vector<int64_t>({
                            1, 3, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecShortBucketWidth}));
                        waml::LearningModelBinding b1(s1);
                        b1.Bind(ctx->recInName, t1);
                        auto out1 = s1.Evaluate(b1, L"").Outputs().Lookup(ctx->recOutName).as<waml::TensorFloat>();
                        if (out1 && out1.Shape().Size() >= 2) {
                            ctx->recBucketSessions[0] = std::move(s1);
                            gpuReady = true;
                        }
                    } else {
                        gpuReady = true;
                    }

                    if (gpuReady) {
                        ctx->device = gpuDev;
                        ctx->clsSession = std::move(gpuCls);
                        ctx->usingDirectMLGpu = true;
                        ctx->supportsBatchEval = batchOk;
                    }
                } catch (...) {
                    gpuReady = false;
                }

                if (!gpuReady) {
                    waml::LearningModelSessionOptions cpuOpts;
                    cpuOpts.CloseModelOnSessionCreation(false);
                    ctx->device = waml::LearningModelDevice(waml::LearningModelDeviceKind::Cpu);
                    ctx->clsSession = waml::LearningModelSession(ctx->clsModel, ctx->device, cpuOpts);
                    ctx->usingDirectMLGpu = false;
                    ctx->supportsBatchEval = false;
                }

                m_winMl = std::move(ctx);
                m_useWinMl = true;
                m_loadedModelDir = modelDir;
                m_modelsLoaded = true;
                m_modelsLoadedAtomic.store(true, std::memory_order_release);
                LOG_INFO("DeepOcrEngine: PP-OCRv4 loaded with native WinML ONNX Runtime ({}{}, dict={} keys)",
                         m_winMl->usingDirectMLGpu ? "DirectML GPU" : "CPU SIMD",
                         m_winMl->supportsBatchEval ? " + StaticBatchFusion" : "",
                         m_dictKeys.size());
                return true;
            }
        } catch (...) {
            m_winMl.reset();
            m_useWinMl = false;
        }

        // 无损回退路径：OpenCV DNN 固定分桶推理引擎
        std::vector<uint8_t> clsFixed, recShortFixed, recLongFixed;
        const bool clsPatched = patchOnnxGraphInputShape(
            clsBytes.data(), clsBytes.size(),
            {1, 3, DeepOcrConstants::kClsImgHeight, DeepOcrConstants::kClsImgWidth},
            clsFixed);
        const bool recShortPatched = patchOnnxGraphInputShape(
            recBytes.data(), recBytes.size(),
            {1, 3, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecShortBucketWidth},
            recShortFixed);
        const bool recLongPatched = patchOnnxGraphInputShape(
            recBytes.data(), recBytes.size(),
            {1, 3, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecLongBucketWidth},
            recLongFixed);

        const auto& clsSrc = clsPatched ? clsFixed : clsBytes;
        const auto& recShortSrc = recShortPatched ? recShortFixed : recBytes;
        const auto& recLongSrc = recLongPatched ? recLongFixed : recBytes;

        m_detNet = cv::dnn::readNetFromONNX(reinterpret_cast<const char*>(detBytes.data()), detBytes.size());
        m_clsNet = cv::dnn::readNetFromONNX(reinterpret_cast<const char*>(clsSrc.data()), clsSrc.size());
        m_recNetShort = cv::dnn::readNetFromONNX(reinterpret_cast<const char*>(recShortSrc.data()), recShortSrc.size());
        m_recNetLong = cv::dnn::readNetFromONNX(reinterpret_cast<const char*>(recLongSrc.data()), recLongSrc.size());

        if (m_detNet.empty() || m_clsNet.empty() || m_recNetShort.empty() || m_recNetLong.empty()) {
            LOG_WARN("DeepOcrEngine: OpenCV DNN failed to parse ONNX graphs");
            return false;
        }

        for (cv::dnn::Net* net : {&m_detNet, &m_clsNet, &m_recNetShort, &m_recNetLong}) {
            net->setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            net->setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        }

        m_loadedModelDir = modelDir;
        m_modelsLoaded = true;
        m_modelsLoadedAtomic.store(true, std::memory_order_release);
        LOG_INFO("DeepOcrEngine: PP-OCRv4 ONNX models loaded (dict={} keys)", m_dictKeys.size());
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("DeepOcrEngine: exception loading PP-OCRv4 models: {}", e.what());
        m_modelsLoaded = false;
        m_modelsLoadedAtomic.store(false, std::memory_order_release);
        return false;
    }
}

std::vector<OcrResult> DeepOcrEngine::extractText(const cv::Mat& image) {
    tools3000::core::TraceId::Scope traceScope;
    if (image.empty()) return {};

    bool installed = false;
    const std::filesystem::path modelDir = resolveModelDirectory(&installed);
    if (!installed) return {};

    cv::Mat bgr = ensureBgr3(image);
    if (bgr.empty()) return {};

    std::lock_guard<std::mutex> lock(m_inferMutex);
    if (!ensureModelsLoadedLocked(modelDir)) {
        return {};
    }
    if (m_useWinMl) {
        ensureComApartmentInitialized();
    }

    std::vector<OcrResult> rawBlocks;
    const auto tTotalStart = std::chrono::steady_clock::now();
    long long detMs = 0;
    long long packMs = 0;
    long long recMs = 0;
    size_t detectedQuadCount = 0;
    size_t packedStripCount = 0;
    bool pageRotated90Or270 = false;
    bool pageRotated270Ccw = false;
    try {
        // 0. 自适应视觉增强前处理（红章压字穿透 + 阴影纸面/手写光照归一化 / 拍屏摩尔纹抑制；清晰数字截图 0 拷贝直通）
        bool applyPreprocToRec = false;
        cv::Mat detSourceBgr = preprocessImageForDetection(bgr, &applyPreprocToRec);
        if (applyPreprocToRec) {
            bgr = detSourceBgr;
        }

        // 1. DBNet 文本区域检测（3 档静态画布桶零重编译 + 多核并行张量填充 + 零拷贝直通）
        const int origW = bgr.cols;
        const int origH = bgr.rows;
        const int maxSide = (std::max)(origW, origH);
        float desiredRatio = 1.0f;
        if (maxSide > DeepOcrConstants::kDetMaxSideLen) {
            desiredRatio = static_cast<float>(DeepOcrConstants::kDetMaxSideLen) / static_cast<float>(maxSide);
        } else if (maxSide < 240) {
            desiredRatio = 2.0f;
        } else if (maxSide < 1180) {
            desiredRatio = (std::min)(1.55f, static_cast<float>(DeepOcrConstants::kDetMaxSideLen) / static_cast<float>((std::max)(1, maxSide)));
        }

        cv::Mat probOut;
        float ratioW = 1.0f;
        float ratioH = 1.0f;

        if (m_useWinMl && m_winMl) {
            const int tentativeW = static_cast<int>(std::round(static_cast<float>(origW) * desiredRatio));
            const int tentativeH = static_cast<int>(std::round(static_cast<float>(origH) * desiredRatio));
            size_t detBucketIdx = 1;
            if (tentativeW <= 960 && tentativeH <= 640) {
                detBucketIdx = 0;
            } else if (origH > origW) {
                detBucketIdx = 2;
            } else {
                detBucketIdx = 1;
            }

            const auto& bucketSpec = WinMlContext::kDetSpecs[detBucketIdx];
            const int canvasW = bucketSpec.w;
            const int canvasH = bucketSpec.h;
            const float fitScale = (std::min)(
                desiredRatio,
                (std::min)(static_cast<float>(canvasW) / static_cast<float>(origW),
                           static_cast<float>(canvasH) / static_cast<float>(origH)));
            const int validW = std::clamp(static_cast<int>(std::round(static_cast<float>(origW) * fitScale)), 16, canvasW);
            const int validH = std::clamp(static_cast<int>(std::round(static_cast<float>(origH) * fitScale)), 16, canvasH);
            ratioW = static_cast<float>(validW) / static_cast<float>(origW);
            ratioH = static_cast<float>(validH) / static_cast<float>(origH);

            cv::Mat detValid;
            cv::resize(detSourceBgr, detValid, cv::Size(validW, validH), 0.0, 0.0,
                       fitScale < 1.0f ? cv::INTER_AREA : cv::INTER_LINEAR);

            cv::Mat detCanvas;
            if (validW == canvasW && validH == canvasH) {
                detCanvas = detValid;
            } else {
                cv::copyMakeBorder(
                    detValid, detCanvas,
                    0, canvasH - validH, 0, canvasW - validW,
                    cv::BORDER_REPLICATE);
            }

            if (waml::LearningModelSession* detSess = m_winMl->ensureDetSession(detBucketIdx)) {
                auto detTensor = waml::TensorFloat::Create(
                    winrt::single_threaded_vector<int64_t>({1, 3, canvasH, canvasW}));
                {
                    WinMlTensorBufferLock inLock(detTensor);
                    const size_t plane = static_cast<size_t>(canvasH) * static_cast<size_t>(canvasW);
                    if (inLock.data && inLock.floatCount >= plane * 3) {
                        float* c0 = inLock.data;
                        float* c1 = inLock.data + plane;
                        float* c2 = inLock.data + plane * 2;
                        cv::parallel_for_(cv::Range(0, canvasH), [&](const cv::Range& range) {
                            constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
                            constexpr float kInvStd[3] = {1.0f / 0.229f, 1.0f / 0.224f, 1.0f / 0.225f};
                            constexpr float kInv255 = 1.0f / 255.0f;
                            for (int y = range.start; y < range.end; ++y) {
                                const auto* row = detCanvas.ptr<cv::Vec3b>(y);
                                const size_t base = static_cast<size_t>(y) * static_cast<size_t>(canvasW);
                                for (int x = 0; x < canvasW; ++x) {
                                    c0[base + x] = (static_cast<float>(row[x][0]) * kInv255 - kMean[0]) * kInvStd[0];
                                    c1[base + x] = (static_cast<float>(row[x][1]) * kInv255 - kMean[1]) * kInvStd[1];
                                    c2[base + x] = (static_cast<float>(row[x][2]) * kInv255 - kMean[2]) * kInvStd[2];
                                }
                            }
                        });
                    }
                }
                waml::LearningModelBinding binding(*detSess);
                binding.Bind(m_winMl->detInName, detTensor);
                auto evalRes = detSess->Evaluate(binding, L"");
                auto outTensor = evalRes.Outputs().Lookup(m_winMl->detOutName).as<waml::TensorFloat>();
                const auto outShape = outTensor.Shape();
                if (outShape.Size() >= 4) {
                    const int outH = static_cast<int>(outShape.GetAt(2));
                    const int outW = static_cast<int>(outShape.GetAt(3));
                    const int cropOutW = (std::min)(validW, outW);
                    const int cropOutH = (std::min)(validH, outH);
                    const size_t needFloats = static_cast<size_t>(outH) * static_cast<size_t>(outW);
                    if (cropOutH > 0 && cropOutW > 0) {
                        probOut.create(cropOutH, cropOutW, CV_32FC1);
                        WinMlTensorBufferLock outLock(outTensor);
                        if (outLock.data && outLock.floatCount >= needFloats) {
                            for (int y = 0; y < cropOutH; ++y) {
                                std::memcpy(
                                    probOut.ptr<float>(y),
                                    outLock.data + static_cast<size_t>(y) * static_cast<size_t>(outW),
                                    static_cast<size_t>(cropOutW) * sizeof(float));
                            }
                        } else {
                            outLock.release();
                            std::vector<float> tmpOut(needFloats);
                            auto vecView = outTensor.GetAsVectorView();
                            vecView.GetMany(0, winrt::array_view<float>(tmpOut.data(), tmpOut.data() + needFloats));
                            for (int y = 0; y < cropOutH; ++y) {
                                std::memcpy(
                                    probOut.ptr<float>(y),
                                    tmpOut.data() + static_cast<size_t>(y) * static_cast<size_t>(outW),
                                    static_cast<size_t>(cropOutW) * sizeof(float));
                            }
                        }
                    }
                }
            }
        } else {
            const int alignW = (std::max)(DeepOcrConstants::kDetStrideAlign,
                static_cast<int>(std::round(static_cast<float>(origW) * desiredRatio / 32.0f)) * 32);
            const int alignH = (std::max)(DeepOcrConstants::kDetStrideAlign,
                static_cast<int>(std::round(static_cast<float>(origH) * desiredRatio / 32.0f)) * 32);
            ratioW = static_cast<float>(alignW) / static_cast<float>(origW);
            ratioH = static_cast<float>(alignH) / static_cast<float>(origH);
            cv::Mat detResized;
            cv::resize(detSourceBgr, detResized, cv::Size(alignW, alignH), 0.0, 0.0,
                       desiredRatio < 1.0f ? cv::INTER_AREA : cv::INTER_LINEAR);
            cv::Mat detBlob = makeDbNetBlob(detResized);
            m_detNet.setInput(detBlob);
            probOut = m_detNet.forward();
        }

        auto quads = postprocessDbMap(probOut, origW, origH, ratioW, ratioH);
        detMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tTotalStart).count();
        detectedQuadCount = quads.size();
        if (quads.empty()) {
            LOG_INFO("DeepOcrEngine: recognized 0 lines on {}x{} in {} ms (det={} ms)",
                     origW, origH, detMs, detMs);
            return {};
        }

        // 自适应斜向淡色半透明水印剔除：当页面主体为水平/正交排版时，精准剔除低对比度斜铺溯源水印候选框
        if (quads.size() >= DeepOcrConstants::kWatermarkFilterMinOrthoQuads + 2) {
            auto computeMinOrthoDeltaDeg = [](const std::array<cv::Point2f, 4>& c) noexcept -> float {
                const float dxTop = c[1].x - c[0].x;
                const float dyTop = c[1].y - c[0].y;
                const float dxLeft = c[3].x - c[0].x;
                const float dyLeft = c[3].y - c[0].y;
                const float lenTop = std::hypot(dxTop, dyTop);
                const float lenLeft = std::hypot(dxLeft, dyLeft);
                const float majorDx = (lenTop >= lenLeft) ? dxTop : dxLeft;
                const float majorDy = (lenTop >= lenLeft) ? dyTop : dyLeft;
                const float absDeg = std::abs(std::atan2(majorDy, majorDx) * (180.0f / 3.14159265f));
                const float mod90 = std::fmod(absDeg, 90.0f);
                return (std::min)(mod90, 90.0f - mod90);
            };

            size_t orthoCount = 0;
            for (const auto& q : quads) {
                if (computeMinOrthoDeltaDeg(q.corners) <= 8.0f) {
                    ++orthoCount;
                }
            }

            if (orthoCount >= DeepOcrConstants::kWatermarkFilterMinOrthoQuads) {
                std::vector<DbDetectedQuad> filtered;
                filtered.reserve(quads.size());
                for (const auto& q : quads) {
                    const float orthoDelta = computeMinOrthoDeltaDeg(q.corners);
                    if (orthoDelta >= DeepOcrConstants::kWatermarkMinTiltDeg &&
                        orthoDelta <= DeepOcrConstants::kWatermarkMaxTiltDeg) {
                        // 沿倾斜四边形内部双线性采样灰度分布，避免外接矩形混入相邻黑字行导致误判
                        std::array<uint16_t, 256> hist{};
                        uint32_t count = 0;
                        constexpr int kGridU = 24;
                        constexpr int kGridV = 8;
                        for (int iv = 1; iv < kGridV; ++iv) {
                            const float v = static_cast<float>(iv) / static_cast<float>(kGridV);
                            for (int iu = 1; iu < kGridU; ++iu) {
                                const float u = static_cast<float>(iu) / static_cast<float>(kGridU);
                                const float px = (1.0f - u) * (1.0f - v) * q.corners[0].x +
                                                 u * (1.0f - v) * q.corners[1].x +
                                                 u * v * q.corners[2].x +
                                                 (1.0f - u) * v * q.corners[3].x;
                                const float py = (1.0f - u) * (1.0f - v) * q.corners[0].y +
                                                 u * (1.0f - v) * q.corners[1].y +
                                                 u * v * q.corners[2].y +
                                                 (1.0f - u) * v * q.corners[3].y;
                                const int ix = std::clamp(static_cast<int>(px), 0, origW - 1);
                                const int iy = std::clamp(static_cast<int>(py), 0, origH - 1);
                                const auto& pix = bgr.at<cv::Vec3b>(iy, ix);
                                const int lum = (pix[2] * 77 + pix[1] * 150 + pix[0] * 29) >> 8;
                                ++hist[static_cast<size_t>(lum & 0xFF)];
                                ++count;
                            }
                        }
                        if (count >= 16) {
                            auto pctAt = [&](uint32_t target) -> int {
                                uint32_t acc = 0;
                                for (int i = 0; i < 256; ++i) {
                                    acc += hist[static_cast<size_t>(i)];
                                    if (acc >= target) return i;
                                }
                                return 255;
                            };
                            const int p12 = pctAt((count * 12u) / 100u);
                            const int p88 = pctAt((count * 88u) / 100u);
                            if ((p88 - p12) <= DeepOcrConstants::kWatermarkFaintMaxContrast) {
                                continue;
                            }
                        }
                    }
                    filtered.push_back(q);
                }
                quads = std::move(filtered);
                if (quads.empty()) {
                    return {};
                }
            }
        }

        coalesceSameLineTinyFragments(quads, bgr);
        detectedQuadCount = quads.size();
        if (quads.empty()) {
            return {};
        }

        rawBlocks.reserve(quads.size());

        auto checkRotate180IfNeeded = [&](cv::Mat& crop) -> bool {
            const float clsAspect = static_cast<float>(crop.cols) / static_cast<float>((std::max)(1, crop.rows));
            const int clsResizedW = std::clamp(
                static_cast<int>(std::ceil(static_cast<float>(DeepOcrConstants::kClsImgHeight) * clsAspect)),
                8, DeepOcrConstants::kClsImgWidth);
            cv::Mat clsBlob = makeRecOrClsBlob(
                crop, DeepOcrConstants::kClsImgHeight, clsResizedW, DeepOcrConstants::kClsImgWidth);
            float p0 = 1.0f, p1 = 0.0f;
            if (m_useWinMl && m_winMl && m_winMl->clsSession) {
                auto clsTensor = waml::TensorFloat::Create(winrt::single_threaded_vector<int64_t>({
                    1, 3, DeepOcrConstants::kClsImgHeight, DeepOcrConstants::kClsImgWidth}));
                const size_t clsFloats = static_cast<size_t>(3) *
                                         static_cast<size_t>(DeepOcrConstants::kClsImgHeight) *
                                         static_cast<size_t>(DeepOcrConstants::kClsImgWidth);
                {
                    WinMlTensorBufferLock inLock(clsTensor);
                    if (inLock.data && inLock.floatCount >= clsFloats) {
                        std::memcpy(inLock.data, clsBlob.ptr<float>(), clsFloats * sizeof(float));
                    }
                }
                waml::LearningModelBinding binding(m_winMl->clsSession);
                binding.Bind(m_winMl->clsInName, clsTensor);
                auto evalRes = m_winMl->clsSession.Evaluate(binding, L"");
                auto outTensor = evalRes.Outputs().Lookup(m_winMl->clsOutName).as<waml::TensorFloat>();
                WinMlTensorBufferLock outLock(outTensor);
                if (outLock.data && outLock.floatCount >= 2) {
                    p0 = outLock.data[0];
                    p1 = outLock.data[1];
                }
            } else {
                m_clsNet.setInput(clsBlob);
                cv::Mat clsOut = m_clsNet.forward();
                if (clsOut.total() >= 2) {
                    p0 = clsOut.ptr<float>()[0];
                    p1 = clsOut.ptr<float>()[1];
                }
            }
            if (p0 < 0.0f || p1 < 0.0f || (p0 + p1) > 1.02f || (p0 + p1) < 0.98f) {
                const float m = (std::max)(p0, p1);
                const float e0 = std::exp(p0 - m);
                const float e1 = std::exp(p1 - m);
                p1 = e1 / (e0 + e1);
            }
            if (p1 >= DeepOcrConstants::kClsRotate180Thresh) {
                cv::rotate(crop, crop, cv::ROTATE_180);
                return true;
            }
            return false;
        };

        // 2. 墨水感知防碰撞边界精修 + 干净背景边距 + 重叠洁净列谷底滑窗切片 + 单行隔离宽度分桶推理
        constexpr int kStripLeadPad = 6;
        constexpr int kMaxPieceW = DeepOcrConstants::kRecLongBucketWidth - kStripLeadPad * 2; // 948

        struct QuadMeta {
            cv::Rect lineBox;
            cv::Mat invPersp;
            int origCropW = 0;
            int origCropH = 0;
            bool rotated180 = false;
            bool rotatedVertical = false;
            bool valid = false;
            std::string text;
            float confWeightedSum = 0.0f;
            size_t totalChars = 0;
            std::vector<CtcDecodedChar> chars;
            std::vector<std::pair<int, std::vector<CtcDecodedChar>>> segOutputs;
        };

        struct RecPiece {
            size_t quadIdx = 0;
            int subOrder = 0;
            int winStartPadded = 0;
            int winWidthPadded = 0;
            float keepLeftPadded = -1e9f;
            float keepRightPadded = 1e9f;
            int padLeft = 0;
            int origCropW = 1;
            cv::Mat stripBgr; // 48 x contentW（左右使用洁净估计背景色留白，绝不复制涂抹边缘墨水列）
            int resizedW = 0;
            int contentW = 0;
        };

        // 弱对比度屏摄/反光灰字条带自适应提纯：
        // 仅在送入 SVTR 前的 48px 高度条带灰度动态范围 (P95 - P05) 落在 [16, 132) 时生效，
        // 通过查表平滑拉伸动态范围（增益上限 2.35x），正常高对比度截图 (>=132) 100% 零改动直通。
        auto enhanceLowContrastStripCrop = [](cv::Mat& bgr48) {
            if (bgr48.empty() || bgr48.type() != CV_8UC3) return;
            std::array<uint32_t, 256> hist{};
            const int rows = bgr48.rows;
            const int cols = bgr48.cols;
            const uint32_t total = static_cast<uint32_t>(rows) * static_cast<uint32_t>(cols);
            if (total < 64) return;
            for (int r = 0; r < rows; ++r) {
                const auto* row = bgr48.ptr<cv::Vec3b>(r);
                for (int c = 0; c < cols; ++c) {
                    const uint32_t lum = (static_cast<uint32_t>(row[c][0]) * 29u +
                                          static_cast<uint32_t>(row[c][1]) * 150u +
                                          static_cast<uint32_t>(row[c][2]) * 77u) >> 8u;
                    ++hist[lum & 255u];
                }
            }
            auto pctBin = [&](uint32_t targetCount) -> int {
                uint32_t acc = 0;
                for (int i = 0; i < 256; ++i) {
                    acc += hist[static_cast<size_t>(i)];
                    if (acc >= targetCount) return i;
                }
                return 255;
            };
            const int p02 = pctBin((total * 2u) / 100u);
            const int p05 = pctBin((total * 5u) / 100u);
            const int p95 = pctBin((total * 95u) / 100u);
            const int p98 = pctBin((total * 98u) / 100u);
            const int dynRange = p95 - p05;
            if (dynRange < 16 || dynRange >= 132) return;

            const float lo = static_cast<float>(p02);
            const float hi = static_cast<float>((std::max)(p98, p02 + 16));
            const float mid = 0.5f * (lo + hi);
            const float gain = std::clamp(216.0f / (hi - lo), 1.15f, 2.35f);

            std::array<uint8_t, 256> lut{};
            for (int i = 0; i < 256; ++i) {
                const float mapped = 128.0f + (static_cast<float>(i) - mid) * gain;
                lut[static_cast<size_t>(i)] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(mapped)), 0, 255));
            }
            for (int r = 0; r < rows; ++r) {
                auto* row = bgr48.ptr<cv::Vec3b>(r);
                for (int c = 0; c < cols; ++c) {
                    row[c][0] = lut[row[c][0]];
                    row[c][1] = lut[row[c][1]];
                    row[c][2] = lut[row[c][2]];
                }
            }
        };

        std::vector<cv::Rect> rawQuadBoxes(quads.size());
        std::vector<bool> quadAxisFlags(quads.size(), false);
        for (size_t qi = 0; qi < quads.size(); ++qi) {
            cv::Rect r;
            const bool axis = isAxisAlignedHorizontalQuad(quads[qi], &r);
            const int bx1 = std::clamp(r.x, 0, origW - 1);
            const int by1 = std::clamp(r.y, 0, origH - 1);
            const int bx2 = std::clamp(r.x + r.width, bx1 + 1, origW);
            const int by2 = std::clamp(r.y + r.height, by1 + 1, origH);
            rawQuadBoxes[qi] = cv::Rect(bx1, by1, bx2 - bx1, by2 - by1);
            quadAxisFlags[qi] = axis;
        }

        std::vector<QuadMeta> quadMetas(quads.size());
        std::vector<RecPiece> pieces;
        pieces.reserve(quads.size() + 16);

        auto buildCleanStrip = [&](const cv::Mat& subCrop, const cv::Vec3b& bgColor, int& outRw) -> cv::Mat {
            const float subScale = static_cast<float>(DeepOcrConstants::kRecImgHeight) /
                                   static_cast<float>((std::max)(1, subCrop.rows));
            outRw = std::clamp(
                static_cast<int>(std::ceil(static_cast<float>(subCrop.cols) * subScale)),
                8, kMaxPieceW);
            cv::Mat rc;
            cv::resize(subCrop, rc, cv::Size(outRw, DeepOcrConstants::kRecImgHeight), 0.0, 0.0,
                       subCrop.rows < DeepOcrConstants::kRecImgHeight ? cv::INTER_CUBIC : cv::INTER_AREA);
            enhanceLowContrastStripCrop(rc);
            const int contentW = outRw + kStripLeadPad * 2;
            cv::Mat stripBgr(DeepOcrConstants::kRecImgHeight, contentW, CV_8UC3,
                             cv::Scalar(bgColor[0], bgColor[1], bgColor[2]));
            rc.copyTo(stripBgr(cv::Rect(kStripLeadPad, 0, outRw, DeepOcrConstants::kRecImgHeight)));
            return stripBgr;
        };

        for (size_t qi = 0; qi < quads.size(); ++qi) {
            const auto& q = quads[qi];
            cv::Rect lineBox = rawQuadBoxes[qi];
            cv::Mat invPersp;
            bool rotatedVertical = false;
            cv::Mat crop;
            cv::Vec3b bgColor(255, 255, 255);
            bool uniformBg = false;

            if (quadAxisFlags[qi]) {
                lineBox = refineAxisAlignedLineBox(bgr, lineBox, rawQuadBoxes, qi, &bgColor, &uniformBg);
                crop = bgr(lineBox);
            } else {
                crop = warpPerspectiveQuad(bgr, q.corners, &invPersp, &rotatedVertical);
                if (!crop.empty()) {
                    bgColor = estimateBorderBackgroundColor(crop, &uniformBg);
                }
            }
            if (crop.empty() || crop.cols < 2 || crop.rows < 2) continue;

            const bool rotated180 = rotatedVertical ? checkRotate180IfNeeded(crop) : false;

            auto& qm = quadMetas[qi];
            qm.lineBox = lineBox;
            qm.invPersp = std::move(invPersp);
            qm.origCropW = crop.cols;
            qm.origCropH = crop.rows;
            qm.rotated180 = rotated180;
            qm.rotatedVertical = rotatedVertical;
            qm.valid = true;

            // 为小字号/贴顶底边界的洁净背景裁剪块补充 1~2px 呼吸边距，防止 4x 放大后贴边笔画与逗号被 SVTR 截断
            int padTop = 0, padBot = 0, padLr = 0;
            if (uniformBg && !rotatedVertical) {
                auto rowTouchesInk = [&](int ry) noexcept -> bool {
                    const auto* row = crop.ptr<cv::Vec3b>(ry);
                    for (int x = 0; x < crop.cols; ++x) {
                        const int d = (std::max)({
                            std::abs(static_cast<int>(row[x][0]) - static_cast<int>(bgColor[0])),
                            std::abs(static_cast<int>(row[x][1]) - static_cast<int>(bgColor[1])),
                            std::abs(static_cast<int>(row[x][2]) - static_cast<int>(bgColor[2])),
                        });
                        if (d > 36) return true;
                    }
                    return false;
                };
                padTop = rowTouchesInk(0) ? 2 : (crop.rows <= 14 ? 1 : 0);
                padBot = rowTouchesInk(crop.rows - 1) ? 2 : (crop.rows <= 14 ? 1 : 0);
                padLr = 2;
            }

            cv::Mat paddedCrop;
            if (padTop > 0 || padBot > 0 || padLr > 0) {
                cv::copyMakeBorder(crop, paddedCrop, padTop, padBot, padLr, padLr,
                                   cv::BORDER_CONSTANT, cv::Scalar(bgColor[0], bgColor[1], bgColor[2]));
            } else {
                paddedCrop = crop;
            }

            // 严格保持纵横比缩放至识别网络标准高度 (48px)：
            // 彻底根治高像素手机原图（如 4284x5712、单行高 160~220px）因下限钳制导致的横向畸形拉扁失真与错误切碎。
            const float cropRowsF = static_cast<float>((std::max)(1, paddedCrop.rows));
            const float horizScale = static_cast<float>(DeepOcrConstants::kRecImgHeight) / cropRowsF;
            const int fullResizedW = static_cast<int>(std::ceil(static_cast<float>(paddedCrop.cols) * horizScale));

            if (fullResizedW <= kMaxPieceW) {
                int rw = 0;
                cv::Mat stripBgr = buildCleanStrip(paddedCrop, bgColor, rw);
                pieces.push_back(RecPiece{
                    qi, 0,
                    0, paddedCrop.cols,
                    -1e9f, 1e9f,
                    padLr, crop.cols,
                    std::move(stripBgr), rw, rw + kStripLeadPad * 2
                });
            } else {
                // 超宽单行（如 940px 紧凑 JSON）采用重叠滑窗 + 洁净单列墨水谷底拼接，彻底消灭切断下划线 `_` 与引号
                const int maxSliceCropW = (std::max)(
                    48,
                    static_cast<int>(std::floor(static_cast<float>(kMaxPieceW - 16) / horizScale)));
                const int overlapPx = std::clamp(
                    static_cast<int>(std::lround(static_cast<float>(paddedCrop.rows) * 2.5f)),
                    28, 52);
                const int stridePx = (std::max)(24, maxSliceCropW - overlapPx);

                std::vector<int> colMaxDiff(static_cast<size_t>(paddedCrop.cols), 0);
                for (int r = 0; r < paddedCrop.rows; ++r) {
                    const auto* row = paddedCrop.ptr<cv::Vec3b>(r);
                    for (int c = 0; c < paddedCrop.cols; ++c) {
                        const int d = (std::max)({
                            std::abs(static_cast<int>(row[c][0]) - static_cast<int>(bgColor[0])),
                            std::abs(static_cast<int>(row[c][1]) - static_cast<int>(bgColor[1])),
                            std::abs(static_cast<int>(row[c][2]) - static_cast<int>(bgColor[2])),
                        });
                        if (d > colMaxDiff[static_cast<size_t>(c)]) {
                            colMaxDiff[static_cast<size_t>(c)] = d;
                        }
                    }
                }

                std::vector<std::pair<int, int>> windows;
                for (int ws = 0; ws < paddedCrop.cols; ) {
                    const int we = (std::min)(paddedCrop.cols, ws + maxSliceCropW);
                    windows.emplace_back(ws, we);
                    if (we >= paddedCrop.cols) break;
                    ws += stridePx;
                }

                std::vector<float> splices;
                splices.reserve(windows.size() > 1 ? windows.size() - 1 : 0);
                for (size_t wi = 0; wi + 1 < windows.size(); ++wi) {
                    const int ovL = windows[wi + 1].first + 6;
                    const int ovR = windows[wi].second - 6;
                    if (ovR <= ovL) {
                        splices.push_back(0.5f * static_cast<float>(windows[wi + 1].first + windows[wi].second));
                    } else {
                        const float mid = 0.5f * static_cast<float>(ovL + ovR);
                        int bestX = static_cast<int>(std::lround(mid));
                        float bestCost = 1e9f;
                        for (int cx = ovL; cx <= ovR; ++cx) {
                            const float d = static_cast<float>(colMaxDiff[static_cast<size_t>(cx)]);
                            const float cost = ((d < 26.0f) ? 0.0f : d * 10.0f) + std::abs(static_cast<float>(cx) - mid) * 0.2f;
                            if (cost < bestCost) {
                                bestCost = cost;
                                bestX = cx;
                            }
                        }
                        splices.push_back(static_cast<float>(bestX) + 0.5f);
                    }
                }

                for (size_t wi = 0; wi < windows.size(); ++wi) {
                    const int ws = windows[wi].first;
                    const int we = windows[wi].second;
                    cv::Mat subCrop = paddedCrop(cv::Rect(ws, 0, we - ws, paddedCrop.rows));
                    int rw = 0;
                    cv::Mat stripBgr = buildCleanStrip(subCrop, bgColor, rw);
                    const float keepL = (wi == 0) ? -1e9f : splices[wi - 1];
                    const float keepR = (wi + 1 == windows.size()) ? 1e9f : splices[wi];
                    pieces.push_back(RecPiece{
                        qi, static_cast<int>(wi),
                        ws, we - ws,
                        keepL, keepR,
                        padLr, crop.cols,
                        std::move(stripBgr), rw, rw + kStripLeadPad * 2
                    });
                }
            }
        }

        if (pieces.empty()) {
            return {};
        }

        const auto tRecStart = std::chrono::steady_clock::now();
        packMs = std::chrono::duration_cast<std::chrono::milliseconds>(tRecStart - tTotalStart).count() - detMs;
        packedStripCount = pieces.size();

        auto mapIsolatedDecodedToQuad = [&](const RecPiece& pc, const CtcDecodeOutput& dec) {
            if (dec.chars.empty()) return;
            auto& qm = quadMetas[pc.quadIdx];
            const float rwF = (std::max)(1.0f, static_cast<float>(pc.resizedW));
            const float winWF = (std::max)(1.0f, static_cast<float>(pc.winWidthPadded));
            const float origWF = (std::max)(1.0f, static_cast<float>(pc.origCropW));
            const float contentWF = static_cast<float>(pc.contentW);
            const float leadF = static_cast<float>(kStripLeadPad);

            std::vector<CtcDecodedChar> chars;
            chars.reserve(dec.chars.size());
            for (const auto& dc : dec.chars) {
                const float pxLStrip = dc.normLeft * contentWF;
                const float pxRStrip = dc.normRight * contentWF;
                const float pxLPadded = static_cast<float>(pc.winStartPadded) +
                                        ((pxLStrip - leadF) / rwF) * winWF;
                const float pxRPadded = static_cast<float>(pc.winStartPadded) +
                                        ((pxRStrip - leadF) / rwF) * winWF;
                const float midPadded = 0.5f * (pxLPadded + pxRPadded);
                if (midPadded < pc.keepLeftPadded || midPadded >= pc.keepRightPadded) {
                    continue;
                }
                CtcDecodedChar mapped = dc;
                mapped.normLeft = std::clamp((pxLPadded - static_cast<float>(pc.padLeft)) / origWF, 0.0f, 1.0f);
                mapped.normRight = std::clamp(
                    (pxRPadded - static_cast<float>(pc.padLeft)) / origWF,
                    mapped.normLeft + 0.002f, 1.0f);
                chars.push_back(std::move(mapped));
            }
            if (!chars.empty()) {
                qm.segOutputs.emplace_back(pc.subOrder, std::move(chars));
            }
        };

        const int H = DeepOcrConstants::kRecImgHeight;
        if (m_useWinMl && m_winMl) {
            // 按宽度分入 3 档独立桶（W=320 / W=640 / W=960），每行独占一个 Batch 通道，彻底杜绝多行横向注意力串扰
            std::array<std::vector<size_t>, 3> tierPieces;
            for (size_t pi = 0; pi < pieces.size(); ++pi) {
                const int cw = pieces[pi].contentW;
                if (cw <= DeepOcrConstants::kRecShortBucketWidth) {
                    tierPieces[0].push_back(pi);
                } else if (cw <= DeepOcrConstants::kRecMediumBucketWidth) {
                    tierPieces[1].push_back(pi);
                } else {
                    tierPieces[2].push_back(pi);
                }
            }

            std::vector<float> hostInBuf;
            std::vector<float> hostOutBuf;

            for (size_t tier = 0; tier < 3; ++tier) {
                const auto& indices = tierPieces[tier];
                if (indices.empty()) continue;

                const int netW = (tier == 0)
                    ? DeepOcrConstants::kRecShortBucketWidth
                    : ((tier == 1) ? DeepOcrConstants::kRecMediumBucketWidth : DeepOcrConstants::kRecLongBucketWidth);

                size_t pos = 0;
                while (pos < indices.size()) {
                    const size_t rem = indices.size() - pos;
                    size_t bucketIdx = 0;
                    if (m_winMl->supportsBatchEval) {
                        if (tier == 0) {
                            bucketIdx = (rem == 1) ? 0 : ((rem >= 20) ? 2 : 1); // 1x320 / 16x320 / 32x320
                        } else if (tier == 1) {
                            bucketIdx = (rem == 1) ? 3 : 4;                     // 1x640 / 16x640
                        } else {
                            bucketIdx = (rem == 1) ? 5 : 6;                     // 1x960 / 8x960
                        }
                    } else {
                        bucketIdx = (tier == 0) ? 0 : ((tier == 1) ? 3 : 5);
                    }

                    waml::LearningModelSession* recSess = m_winMl->ensureRecSession(bucketIdx);
                    if (!recSess) break;

                    const int batchCap = m_winMl->supportsBatchEval
                        ? WinMlContext::kRecSpecs[bucketIdx].n
                        : 1;
                    const int actualB = static_cast<int>((std::min)(rem, static_cast<size_t>(batchCap)));
                    const size_t planeFloats = static_cast<size_t>(H) * static_cast<size_t>(netW);
                    const size_t sampleFloats = static_cast<size_t>(3) * planeFloats;
                    const size_t batchFloats = static_cast<size_t>(batchCap) * sampleFloats;

                    try {
                        hostInBuf.assign(batchFloats, 0.0f);
                        float* basePtr = hostInBuf.data();
                        cv::parallel_for_(cv::Range(0, actualB), [&](const cv::Range& br) {
                            constexpr float kScale = 2.0f / 255.0f;
                            for (int b = br.start; b < br.end; ++b) {
                                const auto& pc = pieces[indices[pos + static_cast<size_t>(b)]];
                                float* sampleBase = basePtr + static_cast<size_t>(b) * sampleFloats;
                                const int copyW = (std::min)(pc.contentW, netW);
                                float* c0 = sampleBase;
                                float* c1 = sampleBase + planeFloats;
                                float* c2 = sampleBase + planeFloats * 2;
                                for (int y = 0; y < H; ++y) {
                                    const auto* row = pc.stripBgr.ptr<cv::Vec3b>(y);
                                    const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(netW);
                                    for (int x = 0; x < copyW; ++x) {
                                        c0[rowBase + x] = static_cast<float>(row[x][0]) * kScale - 1.0f;
                                        c1[rowBase + x] = static_cast<float>(row[x][1]) * kScale - 1.0f;
                                        c2[rowBase + x] = static_cast<float>(row[x][2]) * kScale - 1.0f;
                                    }
                                }
                            }
                        });

                        auto recTensor = waml::TensorFloat::CreateFromArray(
                            winrt::single_threaded_vector<int64_t>({batchCap, 3, H, netW}),
                            winrt::array_view<const float>(hostInBuf.data(), hostInBuf.data() + batchFloats));
                        waml::LearningModelBinding binding(*recSess);
                        binding.Bind(m_winMl->recInName, recTensor);
                        auto evalRes = recSess->Evaluate(binding, L"");
                        auto outTensor = evalRes.Outputs().Lookup(m_winMl->recOutName).as<waml::TensorFloat>();
                        const auto outShape = outTensor.Shape();
                        int seqT = 0;
                        int numClasses = 0;
                        if (outShape.Size() >= 3) {
                            seqT = static_cast<int>(outShape.GetAt(1));
                            numClasses = static_cast<int>(outShape.GetAt(2));
                        } else if (outShape.Size() == 2) {
                            seqT = static_cast<int>(outShape.GetAt(0));
                            numClasses = static_cast<int>(outShape.GetAt(1));
                        }
                        if (seqT > 0 && numClasses > 1) {
                            const size_t oneOutFloats = static_cast<size_t>(seqT) * static_cast<size_t>(numClasses);
                            const size_t needOutFloats = static_cast<size_t>(actualB) * oneOutFloats;
                            hostOutBuf.resize(needOutFloats);
                            {
                                WinMlTensorBufferLock outLock(outTensor);
                                if (outLock.data && outLock.floatCount >= needOutFloats) {
                                    std::memcpy(hostOutBuf.data(), outLock.data, needOutFloats * sizeof(float));
                                } else {
                                    outLock.release();
                                    auto vecView = outTensor.GetAsVectorView();
                                    vecView.GetMany(0, winrt::array_view<float>(hostOutBuf.data(), hostOutBuf.data() + needOutFloats));
                                }
                            }

                            std::vector<CtcDecodeOutput> batchDecoded(static_cast<size_t>(actualB));
                            float* cachedHostPtr = hostOutBuf.data();
                            cv::parallel_for_(cv::Range(0, actualB), [&](const cv::Range& br) {
                                for (int b = br.start; b < br.end; ++b) {
                                    const auto& pc = pieces[indices[pos + static_cast<size_t>(b)]];
                                    const float validRatio = static_cast<float>(pc.contentW) / static_cast<float>(netW);
                                    cv::Mat logits2d(seqT, numClasses, CV_32FC1, cachedHostPtr + static_cast<size_t>(b) * oneOutFloats);
                                    batchDecoded[static_cast<size_t>(b)] = decodeCtcGreedy(logits2d, m_dictKeys, validRatio);
                                }
                            });

                            for (int b = 0; b < actualB; ++b) {
                                mapIsolatedDecodedToQuad(pieces[indices[pos + static_cast<size_t>(b)]], batchDecoded[static_cast<size_t>(b)]);
                            }
                        }
                    } catch (...) {
                    }

                    pos += static_cast<size_t>(actualB);
                }
            }
        } else {
            for (const auto& pc : pieces) {
                const bool useShort = (pc.contentW <= DeepOcrConstants::kRecShortBucketWidth);
                const int netW = useShort
                    ? DeepOcrConstants::kRecShortBucketWidth
                    : DeepOcrConstants::kRecLongBucketWidth;
                cv::dnn::Net& activeNet = useShort ? m_recNetShort : m_recNetLong;
                cv::Mat recBlob = makeRecOrClsBlob(pc.stripBgr, H, pc.contentW, netW);
                activeNet.setInput(recBlob);
                cv::Mat recOut = activeNet.forward();
                if (recOut.empty()) continue;
                int seqT = (recOut.dims >= 3) ? recOut.size[1] : recOut.rows;
                int numClasses = (recOut.dims >= 3) ? recOut.size[2] : recOut.cols;
                if (seqT <= 0 || numClasses <= 1) continue;
                cv::Mat logits2d(seqT, numClasses, CV_32F, recOut.ptr<float>());
                const float validRatio = static_cast<float>(pc.contentW) / static_cast<float>(netW);
                const CtcDecodeOutput dec = decodeCtcGreedy(logits2d, m_dictKeys, validRatio);
                mapIsolatedDecodedToQuad(pc, dec);
            }
        }

        recMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tRecStart).count();
        for (size_t qi = 0; qi < quadMetas.size(); ++qi) {
            auto& qm = quadMetas[qi];
            if (!qm.valid || qm.segOutputs.empty()) continue;
            if (qm.segOutputs.size() > 1) {
                std::sort(qm.segOutputs.begin(), qm.segOutputs.end(), [](const auto& a, const auto& b) {
                    return a.first < b.first;
                });
            }
            const float origWF = (std::max)(1.0f, static_cast<float>(qm.origCropW));
            for (size_t si = 0; si < qm.segOutputs.size(); ++si) {
                auto& seg = qm.segOutputs[si];
                for (size_t ci = 0; ci < seg.second.size(); ++ci) {
                    auto& ch = seg.second[ci];
                    // 跨滑窗接缝防重复守卫：若下一窗口首字符与上一窗口尾字符完全相同且原图中心间距 <= 4.5px 则去重
                    if (si > 0 && ci == 0 && !qm.chars.empty() && qm.chars.back().utf8Char == ch.utf8Char) {
                        const float prevMid = 0.5f * (qm.chars.back().normLeft + qm.chars.back().normRight) * origWF;
                        const float curMid = 0.5f * (ch.normLeft + ch.normRight) * origWF;
                        if (std::abs(curMid - prevMid) <= 4.5f) {
                            continue;
                        }
                    }
                    qm.chars.push_back(std::move(ch));
                }
            }
            while (!qm.chars.empty() && qm.chars.front().utf8Char == " ") qm.chars.erase(qm.chars.begin());
            while (!qm.chars.empty() && qm.chars.back().utf8Char == " ") qm.chars.pop_back();
            for (const auto& ch : qm.chars) {
                qm.text += ch.utf8Char;
                qm.confWeightedSum += ch.confidence;
                ++qm.totalChars;
            }
            if (qm.text.empty() || qm.totalChars == 0) continue;
            const float avgConf = qm.confWeightedSum / static_cast<float>(qm.totalChars);
            if (avgConf < DeepOcrConstants::kRecMinLineConfidence) continue;

            OcrResult item;
            item.text = std::move(qm.text);
            item.confidence = avgConf;
            item.box = qm.lineBox;
            item.charBoxes = projectCharSpansToImage(
                item.text, qm.chars, qm.origCropW, qm.origCropH,
                qm.rotated180, qm.rotatedVertical, qm.invPersp, qm.lineBox, origW, origH);
            rawBlocks.push_back(std::move(item));
        }

        // 2.4 整图 90°/270° 拍歪自动探测（0 额外推理耗时）：
        // 若 >= 3 个识别块且 >= 80% 均触发了竖长框 90° 旋转，基于 180° 方向分类器投票确定正向阅读序。
        {
            size_t validCount = 0;
            size_t vertCount = 0;
            size_t vertRot180Count = 0;
            for (const auto& qm : quadMetas) {
                if (!qm.valid || qm.totalChars == 0) continue;
                ++validCount;
                if (qm.rotatedVertical && qm.lineBox.height >= static_cast<int>(static_cast<float>((std::max)(1, qm.lineBox.width)) * 1.65f)) {
                    ++vertCount;
                    if (qm.rotated180) ++vertRot180Count;
                }
            }
            if (validCount >= 3 && vertCount * 5 >= validCount * 4) {
                pageRotated90Or270 = true;
                pageRotated270Ccw = (vertRot180Count * 2 > vertCount);
            }
        }

        // 2.5 像素级 UI 下拉箭头剥离 + 像素级漏识下划线还原 + 结构化 JSON 标点规整（均严格保持 text 与 charBoxes 1:1 对齐）
        stripUiDropdownChevrons(bgr, rawBlocks);
        for (auto& blk : rawBlocks) {
            rescueMissedUnderscores(bgr, blk);
            normalizeStructuredJsonPunctuation(blk);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("DeepOcrEngine: inference exception: {}", e.what());
        return {};
    }

    if (rawBlocks.empty()) {
        return {};
    }

    // 3. 整图 90°/270° 拍歪阅读序校正；常规正向图委托 OcrEngine 版面感知内核（竖向分割线 + 递归 XY-Cut + 同行基线对齐）
    std::vector<OcrResult> assembled;
    if (pageRotated90Or270) {
        std::sort(rawBlocks.begin(), rawBlocks.end(), [pageRotated270Ccw](const OcrResult& a, const OcrResult& b) {
            const float cxA = static_cast<float>(a.box.x) + 0.5f * static_cast<float>(a.box.width);
            const float cxB = static_cast<float>(b.box.x) + 0.5f * static_cast<float>(b.box.width);
            const float cyA = static_cast<float>(a.box.y) + 0.5f * static_cast<float>(a.box.height);
            const float cyB = static_cast<float>(b.box.y) + 0.5f * static_cast<float>(b.box.height);
            const float minW = static_cast<float>((std::max)(6, (std::min)(a.box.width, b.box.width)));
            const bool sameRotatedLine = std::abs(cxA - cxB) <= minW * 0.65f;
            if (pageRotated270Ccw) {
                // 原图逆时针偏转 90°：最左侧为第 1 行，同行内自下而上阅读
                if (sameRotatedLine) return cyA > cyB;
                return cxA < cxB;
            }
            // 原图顺时针偏转 90°：最右侧为第 1 行，同行内自上而下阅读
            if (sameRotatedLine) return cyA < cyB;
            return cxA > cxB;
        });
        for (size_t i = 0; i < rawBlocks.size(); ++i) {
            rawBlocks[i].layoutRegionId = 0;
            rawBlocks[i].alignedRowId = static_cast<int>(i);
        }
        assembled = std::move(rawBlocks);
    } else {
        assembled = OcrEngine::assembleRecognizedBlocksWithLayout(std::move(rawBlocks), image);
    }
    const long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tTotalStart).count();
    LOG_INFO("DeepOcrEngine: recognized {} lines ({} quads -> {} strips) on {}x{} in {} ms (det={} ms, pack={} ms, rec={} ms)",
             assembled.size(), detectedQuadCount, packedStripCount,
             bgr.cols, bgr.rows, totalMs, detMs, packMs, recMs);
    return assembled;
}

bool DeepOcrEngine::isWarmAndLoaded() const noexcept {
    return m_modelsLoadedAtomic.load(std::memory_order_acquire);
}

void DeepOcrEngine::precompileFixedBucketsLocked() {
    if (m_bucketsPrecompiled || !m_useWinMl || !m_winMl) return;
    try {
        for (size_t di : {size_t{0}, size_t{1}}) {
            if (m_shuttingDown.load(std::memory_order_acquire)) return;
            if (waml::LearningModelSession* dSess = m_winMl->ensureDetSession(di)) {
                const auto& sp = WinMlContext::kDetSpecs[di];
                auto dTen = waml::TensorFloat::Create(winrt::single_threaded_vector<int64_t>({sp.n, 3, sp.h, sp.w}));
                waml::LearningModelBinding db(*dSess);
                db.Bind(m_winMl->detInName, dTen);
                (void)dSess->Evaluate(db, L"");
            }
        }
        const std::array<size_t, 3> warmRecIndices = m_winMl->supportsBatchEval
            ? std::array<size_t, 3>{1, 4, 6}   // 16x320, 16x640, 8x960
            : std::array<size_t, 3>{0, 3, 5};  // 1x320,  1x640,  1x960
        for (size_t ri : warmRecIndices) {
            if (m_shuttingDown.load(std::memory_order_acquire)) return;
            if (waml::LearningModelSession* rSess = m_winMl->ensureRecSession(ri)) {
                const auto& sp = WinMlContext::kRecSpecs[ri];
                const int bCap = m_winMl->supportsBatchEval ? sp.n : 1;
                auto rTen = waml::TensorFloat::Create(
                    winrt::single_threaded_vector<int64_t>({bCap, 3, sp.h, sp.w}));
                waml::LearningModelBinding rb(*rSess);
                rb.Bind(m_winMl->recInName, rTen);
                (void)rSess->Evaluate(rb, L"");
            }
        }
        m_bucketsPrecompiled = true;
    } catch (...) {
    }
}

void DeepOcrEngine::warmupAsync(int delayMs) {
    if (m_shuttingDown.load(std::memory_order_acquire)) return;
    bool installed = false;
    const auto modelDir = resolveModelDirectory(&installed);
    if (!installed) return;

    std::lock_guard<std::mutex> lock(m_warmupMutex);
    if (m_shuttingDown.load(std::memory_order_acquire)) return;
    if (m_modelsLoadedAtomic.load(std::memory_order_acquire) && m_bucketsPrecompiled) {
        return;
    }
    if (m_warmupThread.joinable()) {
        return;
    }

    m_warmupThread = std::jthread([this, modelDir, delayMs](std::stop_token st) {
        tools3000::core::TraceId::Scope traceScope;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        int remainingMs = (std::max)(0, delayMs);
        while (remainingMs > 0) {
            if (st.stop_requested() || m_shuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            const int step = (std::min)(50, remainingMs);
            Sleep(static_cast<DWORD>(step));
            remainingMs -= step;
        }
        if (st.stop_requested() || m_shuttingDown.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<std::mutex> inferLock(m_inferMutex);
        if (st.stop_requested() || m_shuttingDown.load(std::memory_order_acquire)) {
            return;
        }
        if (ensureModelsLoadedLocked(modelDir)) {
            precompileFixedBucketsLocked();
            LOG_INFO("DeepOcrEngine: background static bucket pre-compilation completed");
        }
    });
}

void DeepOcrEngine::releaseLoadedModels() {
    {
        std::lock_guard<std::mutex> lock(m_inferMutex);
        m_modelsLoaded = false;
        m_modelsLoadedAtomic.store(false, std::memory_order_release);
        m_bucketsPrecompiled = false;
        m_useWinMl = false;
        m_loadedModelDir.clear();
        m_winMl.reset();
        m_detNet = cv::dnn::Net{};
        m_clsNet = cv::dnn::Net{};
        m_recNetShort = cv::dnn::Net{};
        m_recNetLong = cv::dnn::Net{};
        m_dictKeys.clear();
    }
    deep_ocr_detail::cleanupDeepOcrWorkingSet();
}

void DeepOcrEngine::shutdown() {
    m_shuttingDown.store(true, std::memory_order_release);
    m_cancelRequested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> wLock(m_warmupMutex);
        if (m_warmupThread.joinable()) {
            m_warmupThread.request_stop();
            m_warmupThread.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        if (m_downloadThread.joinable()) {
            m_downloadThread.request_stop();
            m_downloadThread.join();
        }
    }
    m_downloading.store(false, std::memory_order_release);
    releaseLoadedModels();
}

} // namespace tools3000::ocr
