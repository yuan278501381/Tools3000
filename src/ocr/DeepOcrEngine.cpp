// ─────────────────────────────────────────────────────────────────────────────
// DeepOcrEngine.cpp — PP-OCRv4 离线推理运行态核心（WinML DirectML / OpenCV DNN 管线、预热与主推理）
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/DeepOcrEngineInternal.h"
#include "ocr/OcrWorkerClient.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <opencv2/imgproc.hpp>

#include <windows.h>
#include <unknwn.h>
#include <objidl.h>
#include <MemoryBuffer.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.AI.MachineLearning.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

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

    // 3 档零重编译检测画布桶（按需懒加载）: [0]=960x640 (中小图), [1]=1280x1024 (横屏大图), [2]=960x1280 (竖屏长图)
    static constexpr std::array<StaticBucketSpec, 3> kDetSpecs = {{
        {1, 640, 960},
        {1, 1024, 1280},
        {1, 1280, 960},
    }};

    // 4 档严格遵循 DirectML SVTR Attention 显存甜点律 (B * W^2 < 1.9M) 的极速微批次桶：
    //   实机基准验证：一旦超过 B*W^2 >= 3.2M 悬崖 (如 8x640 / 4x960 / 16x640)，4GB 显存 GPU 会触发 DirectML 工作区换页抖动；
    //   保持 4 档紧凑甜点桶，既避免 SVTR Attention 大量右侧零填充引起的字符精度漂移，又借助 15 分钟热保活实现后续 0ms 复用：
    //   [0]=16x160 (B*W^2=0.41M, 超短词/密集表格单元)
    //   [1]=16x320 (B*W^2=1.64M, 短词主力批桶，探测阶段即编译并直接复用)
    //   [2]=4x640  (B*W^2=1.64M, 中长句高吞吐零悬崖桶)
    //   [3]=2x960  (B*W^2=1.84M, 超长句零悬崖按需桶)
    static constexpr std::array<StaticBucketSpec, 4> kRecSpecs = {{
        {16, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecTinyBucketWidth},
        {16, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecShortBucketWidth},
        {4,  DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecMediumBucketWidth},
        {2,  DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecLongBucketWidth},
    }};

    // 独占高性能 D3D11 设备句柄：
    // 实机实证 WinML 默认 LearningModelDeviceKind::DirectXHighPerformance 复用进程级全局单例 D3D12/DirectML 设备，
    // 即使销毁全部 Session 仍永久扣留数百 MB ~ 数 GB Private Commit；通过 CreateFromDirect3D11Device 绑定独占设备并按逆序
    // 显式 Close() + IDXGIDevice3::Trim()，闲置卸载时可将 90%+ DirectML 专用私有提交内存真正归还给操作系统。
    Microsoft::WRL::ComPtr<ID3D11Device> ownedD3d11Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ownedD3d11Context;
    Microsoft::WRL::ComPtr<IDXGIDevice3> ownedDxgiDevice;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice ownedWinrtD3dDevice{nullptr};

    waml::LearningModel detModel{nullptr};
    waml::LearningModel clsModel{nullptr};
    waml::LearningModel recModel{nullptr};
    waml::LearningModelDevice device{nullptr};

    // clsSession 仅在真正遇到 90°/270° 竖排框时按需在 CPU 上懒加载（0.58MB 极轻量模型，0 占用 GPU DirectML 显存与编译耗时！）
    waml::LearningModelSession clsSession{nullptr};
    std::array<waml::LearningModelSession, 3> detBucketSessions{nullptr, nullptr, nullptr};
    std::array<waml::LearningModelSession, 4> recBucketSessions{
        nullptr, nullptr, nullptr, nullptr};
    waml::LearningModelSession cpuFallbackRecSession{nullptr};

    // 二阶高精度 Server 复核按需单一甜点会话（仅 Win11+ 且 >=2GB 独立显存走 GPU 2x640 桶；Win10 及核显/低显存自动走 CPU 防 TDR 卡死）
    std::filesystem::path serverRecPath;
    bool serverCascadeEnabled = false;
    bool allowServerGpu = false;
    bool serverSessionOnGpu = false;
    waml::LearningModel serverRecModel{nullptr};
    waml::LearningModelDevice cpuDevice{nullptr};
    waml::LearningModelSession serverRecSession{nullptr};
    winrt::hstring serverRecInName;
    winrt::hstring serverRecOutName;

    // 多语种扩展识别单一互斥轻量槽位（CPU SIMD 动态 32 对齐微批，冷启 ~30ms，0 DirectML 显存峰增量，切语种即释放旧槽）
    struct ExtRecSlot {
        std::string scriptId;
        waml::LearningModelSession session{nullptr};
        winrt::hstring inName;
        winrt::hstring outName;
        std::vector<std::string> dictKeys;
    };
    ExtRecSlot extRec;

    bool usingDirectMLGpu = false;
    bool supportsBatchEval = false;
    winrt::hstring detInName;
    winrt::hstring detOutName;
    winrt::hstring clsInName;
    winrt::hstring clsOutName;
    winrt::hstring recInName;
    winrt::hstring recOutName;

    ~WinMlContext() noexcept {
        auto closeSession = [](waml::LearningModelSession& s) noexcept {
            if (s) {
                try { s.Close(); } catch (...) {}
                s = nullptr;
            }
        };
        auto closeModel = [](waml::LearningModel& m) noexcept {
            if (m) {
                try { m.Close(); } catch (...) {}
                m = nullptr;
            }
        };
        closeSession(extRec.session);
        extRec.scriptId.clear();
        extRec.dictKeys.clear();
        closeSession(serverRecSession);
        closeSession(cpuFallbackRecSession);
        for (auto& s : recBucketSessions) closeSession(s);
        for (auto& s : detBucketSessions) closeSession(s);
        closeSession(clsSession);

        closeModel(serverRecModel);
        closeModel(recModel);
        closeModel(clsModel);
        closeModel(detModel);

        device = nullptr;
        cpuDevice = nullptr;
        if (ownedWinrtD3dDevice) {
            try { ownedWinrtD3dDevice.Close(); } catch (...) {}
            ownedWinrtD3dDevice = nullptr;
        }
        if (ownedDxgiDevice) {
            ownedDxgiDevice->Trim();
            ownedDxgiDevice.Reset();
        }
        ownedD3d11Context.Reset();
        ownedD3d11Device.Reset();
    }

    size_t countActiveSessions() const noexcept {
        size_t total = 0;
        if (clsSession) ++total;
        for (const auto& s : detBucketSessions) {
            if (s) ++total;
        }
        for (const auto& s : recBucketSessions) {
            if (s) ++total;
        }
        if (cpuFallbackRecSession) ++total;
        if (serverRecSession) ++total;
        if (extRec.session) ++total;
        return total;
    }

    ExtRecSlot* ensureExtRecSlot(const std::filesystem::path& modelDir, const std::string& scriptId) {
        if (scriptId.empty() || scriptId == "cjk" || scriptId == "auto") return nullptr;
        if (extRec.session && extRec.scriptId == scriptId && !extRec.dictKeys.empty()) {
            return &extRec;
        }
        const std::filesystem::path packPath = deep_ocr_detail::resolveScriptPackModelPath(modelDir, scriptId);
        if (packPath.empty() || !deep_ocr_detail::isScriptPackModelPresent(modelDir, scriptId)) {
            return nullptr;
        }

        // 互斥单活：切换扩展语种前先释放旧槽位，严防多语种模型内存堆积
        if (extRec.session) {
            try { extRec.session.Close(); } catch (...) {}
            extRec.session = nullptr;
        }
        extRec.scriptId.clear();
        extRec.dictKeys.clear();

        std::vector<uint8_t> rawBytes;
        if (!deep_ocr_detail::readFileBytes(packPath, rawBytes) ||
            !DeepOcrEngine::isValidOnnxBuffer(rawBytes.data(), rawBytes.size(), DeepOcrConstants::kMinRecModelBytes)) {
            return nullptr;
        }
        std::vector<std::string> keys;
        if (!DeepOcrEngine::extractEmbeddedCharacterDictFromOnnx(rawBytes.data(), rawBytes.size(), keys) ||
            keys.size() < 64) {
            return nullptr;
        }

        // 兼容手动放入的 PP-OCRv5 (ir_version=10)：若超过 IR 8 则原位或临时路径无损钳位至 IR 8 再交由 WinML 加载
        std::filesystem::path loadPath = packPath;
        if (DeepOcrEngine::clampOnnxIrVersionForWinMl(rawBytes)) {
            bool updatedInPlace = false;
            try {
                std::ofstream ofs(packPath, std::ios::binary | std::ios::trunc);
                if (ofs) {
                    ofs.write(reinterpret_cast<const char*>(rawBytes.data()),
                              static_cast<std::streamsize>(rawBytes.size()));
                    ofs.flush();
                    updatedInPlace = static_cast<bool>(ofs);
                }
            } catch (...) {
                updatedInPlace = false;
            }
            if (!updatedInPlace) {
                std::error_code ec;
                const auto tmpDir = std::filesystem::temp_directory_path(ec);
                if (!ec && !tmpDir.empty()) {
                    const auto compatPath = tmpDir / (L"t3k_ocr_" + packPath.filename().wstring());
                    std::ofstream ofs(compatPath, std::ios::binary | std::ios::trunc);
                    if (ofs) {
                        ofs.write(reinterpret_cast<const char*>(rawBytes.data()),
                                  static_cast<std::streamsize>(rawBytes.size()));
                        ofs.flush();
                        if (ofs) loadPath = compatPath;
                    }
                }
            }
        }

        try {
            if (!cpuDevice) {
                cpuDevice = waml::LearningModelDevice(waml::LearningModelDeviceKind::Cpu);
            }
            auto model = waml::LearningModel::LoadFromFilePath(loadPath.wstring());
            if (!model || model.InputFeatures().Size() == 0 || model.OutputFeatures().Size() == 0) {
                if (model) { try { model.Close(); } catch (...) {} }
                return nullptr;
            }
            const winrt::hstring inN = model.InputFeatures().GetAt(0).Name();
            const winrt::hstring outN = model.OutputFeatures().GetAt(0).Name();
            waml::LearningModelSessionOptions opts;
            opts.CloseModelOnSessionCreation(true);
            auto sess = waml::LearningModelSession(model, cpuDevice, opts);
            try { model.Close(); } catch (...) {}

            extRec.scriptId = scriptId;
            extRec.session = std::move(sess);
            extRec.inName = inN;
            extRec.outName = outN;
            extRec.dictKeys = std::move(keys);
            LOG_INFO("DeepOcrEngine: loaded script extension rec slot ({}, dict={} keys)",
                     extRec.scriptId, extRec.dictKeys.size());
            return &extRec;
        } catch (...) {
            LOG_WARN("DeepOcrEngine: failed to initialize script extension rec slot ({})", scriptId);
            return nullptr;
        }
    }

    waml::LearningModelSession* ensureClsSession() {
        if (clsSession) return &clsSession;
        if (!clsModel) return nullptr;
        try {
            if (!cpuDevice) {
                cpuDevice = waml::LearningModelDevice(waml::LearningModelDeviceKind::Cpu);
            }
            waml::LearningModelSessionOptions opts;
            opts.CloseModelOnSessionCreation(true);
            clsSession = waml::LearningModelSession(clsModel, cpuDevice, opts);
            clsModel = nullptr;
            return &clsSession;
        } catch (...) {
            return nullptr;
        }
    }

    waml::LearningModelSession* ensureDetSession(size_t idx) {
        if (idx >= kDetSpecs.size() || !detModel || !device) return nullptr;
        if (!detBucketSessions[idx]) {
            try {
                // DirectML 大尺寸检测桶（[1]=1280x1024 横屏大图 与 [2]=960x1280 竖屏长图）实施互斥单活：
                // 桌面连拍 99% 同方向 0 重编开销，跨横/竖切换时释放旧大桶并触发 DXGI Trim，削减 ~400-500MB 峰值提交内存
                if (usingDirectMLGpu && (idx == 1 || idx == 2)) {
                    const size_t otherLargeIdx = (idx == 1) ? 2 : 1;
                    if (detBucketSessions[otherLargeIdx]) {
                        try { detBucketSessions[otherLargeIdx].Close(); } catch (...) {}
                        detBucketSessions[otherLargeIdx] = nullptr;
                        if (ownedDxgiDevice) {
                            ownedDxgiDevice->Trim();
                        }
                    }
                }
                waml::LearningModelSessionOptions opts;
                opts.CloseModelOnSessionCreation(false);
                detBucketSessions[idx] = waml::LearningModelSession(detModel, device, opts);
            } catch (const winrt::hresult_error& e) {
                LOG_WARN("DeepOcrEngine: ensureDetSession({}) WinRT error hr=0x{:08X}",
                         idx, static_cast<uint32_t>(e.code().value));
                detBucketSessions[idx] = nullptr;
                return nullptr;
            } catch (...) {
                LOG_WARN("DeepOcrEngine: ensureDetSession({}) failed with unknown exception", idx);
                detBucketSessions[idx] = nullptr;
                return nullptr;
            }
        }
        return &detBucketSessions[idx];
    }

    waml::LearningModelSession* ensureRecSession(size_t idx) {
        if (!recModel) return nullptr;
        if (!usingDirectMLGpu || !supportsBatchEval) {
            if (!cpuFallbackRecSession) {
                try {
                    if (!cpuDevice) {
                        cpuDevice = waml::LearningModelDevice(waml::LearningModelDeviceKind::Cpu);
                    }
                    waml::LearningModelSessionOptions opts;
                    opts.CloseModelOnSessionCreation(false);
                    cpuFallbackRecSession = waml::LearningModelSession(
                        recModel, (usingDirectMLGpu && device) ? device : cpuDevice, opts);
                } catch (const winrt::hresult_error& e) {
                    LOG_WARN("DeepOcrEngine: ensureRecSession(fallback) WinRT error hr=0x{:08X}",
                             static_cast<uint32_t>(e.code().value));
                    cpuFallbackRecSession = nullptr;
                    return nullptr;
                } catch (...) {
                    cpuFallbackRecSession = nullptr;
                    return nullptr;
                }
            }
            return &cpuFallbackRecSession;
        }

        if (idx >= kRecSpecs.size() || !device) return nullptr;
        if (!recBucketSessions[idx]) {
            try {
                waml::LearningModelSessionOptions opts;
                opts.CloseModelOnSessionCreation(false);
                recBucketSessions[idx] = waml::LearningModelSession(recModel, device, opts);
            } catch (const winrt::hresult_error& e) {
                LOG_WARN("DeepOcrEngine: ensureRecSession({}) WinRT error hr=0x{:08X}",
                         idx, static_cast<uint32_t>(e.code().value));
                recBucketSessions[idx] = nullptr;
                return nullptr;
            } catch (...) {
                recBucketSessions[idx] = nullptr;
                return nullptr;
            }
        }
        return &recBucketSessions[idx];
    }

    waml::LearningModelSession* ensureServerRecSession() {
        if (!serverCascadeEnabled || serverRecPath.empty()) return nullptr;
        if (serverRecSession) return &serverRecSession;
        try {
            const auto compatServerPath = ensureOnnxWinMlIr8CompatiblePath(serverRecPath);
            if (!serverRecModel) {
                serverRecModel = waml::LearningModel::LoadFromFilePath(compatServerPath.wstring());
            }
            if (!serverRecModel ||
                serverRecModel.InputFeatures().Size() == 0 ||
                serverRecModel.OutputFeatures().Size() == 0) {
                serverCascadeEnabled = false;
                return nullptr;
            }
            serverRecInName = serverRecModel.InputFeatures().GetAt(0).Name();
            serverRecOutName = serverRecModel.OutputFeatures().GetAt(0).Name();

            waml::LearningModelSessionOptions opts;
            opts.CloseModelOnSessionCreation(true);
            if (usingDirectMLGpu && supportsBatchEval && allowServerGpu && device) {
                try {
                    serverRecSession = waml::LearningModelSession(serverRecModel, device, opts);
                    serverSessionOnGpu = true;
                } catch (...) {
                    allowServerGpu = false;
                    serverSessionOnGpu = false;
                    serverRecSession = nullptr;
                    // GPU 创建失败时关闭可能损坏的模型引用并重新在 CPU 上安全加载
                    if (!serverRecModel) {
                        serverRecModel = waml::LearningModel::LoadFromFilePath(compatServerPath.wstring());
                    }
                    if (!cpuDevice) {
                        cpuDevice = waml::LearningModelDevice(waml::LearningModelDeviceKind::Cpu);
                    }
                    serverRecSession = waml::LearningModelSession(serverRecModel, cpuDevice, opts);
                }
            } else {
                if (!cpuDevice) {
                    cpuDevice = waml::LearningModelDevice(waml::LearningModelDeviceKind::Cpu);
                }
                serverRecSession = waml::LearningModelSession(serverRecModel, cpuDevice, opts);
                serverSessionOnGpu = false;
            }
            serverRecModel = nullptr;
            return &serverRecSession;
        } catch (const winrt::hresult_error& e) {
            LOG_WARN("DeepOcrEngine: server rec session failed (hr=0x{:08X}), disabling server cascade for session",
                     static_cast<uint32_t>(e.code().value));
            serverCascadeEnabled = false;
            serverSessionOnGpu = false;
            serverRecModel = nullptr;
            serverRecSession = nullptr;
            return nullptr;
        } catch (...) {
            serverCascadeEnabled = false;
            serverSessionOnGpu = false;
            serverRecModel = nullptr;
            serverRecSession = nullptr;
            return nullptr;
        }
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
    try {
        winrt::clear_factory_cache();
    } catch (...) {
    }
    ::CoFreeUnusedLibrariesEx(0, 0);
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
    const bool useV6 = isV6BaseBundlePresent(modelDir);
    const std::string desiredGen = useV6 ? "v6" : "v4";
    const std::string desiredTier =
        (configuredModelTier() == "server" && isServerRecModelPresent(modelDir)) ? "server" : "mobile";

    if (m_modelsLoaded && m_loadedModelDir == modelDir && m_loadedModelTier == desiredTier &&
        m_loadedEngineGen == desiredGen &&
        !m_dictKeys.empty() &&
        ((m_useWinMl && m_winMl != nullptr) ||
         (!m_detNet.empty() && !m_recNetShort.empty() && !m_recNetLong.empty()))) {
        return true;
    }

    m_modelsLoaded = false;
    m_modelsLoadedAtomic.store(false, std::memory_order_release);
    m_bucketsPrecompiled = false;
    m_useWinMl = false;
    m_loadedModelTier.clear();
    m_loadedEngineGen.clear();
    m_winMl.reset();
    m_detNet = cv::dnn::Net{};
    m_clsNet = cv::dnn::Net{};
    m_recNetShort = cv::dnn::Net{};
    m_recNetLong = cv::dnn::Net{};
    m_dictKeys.clear();

    try {
        const auto detPath = modelDir / (useV6 ? DeepOcrConstants::kV6DetModelFileName : DeepOcrConstants::kDetModelFileName);
        const auto clsPath = modelDir / DeepOcrConstants::kClsModelFileName;
        const auto mobileRecPath = modelDir / (useV6 ? DeepOcrConstants::kV6RecModelFileName : DeepOcrConstants::kRecModelFileName);
        const auto serverRecPath = modelDir / (useV6 ? DeepOcrConstants::kV6RecMediumModelFileName : DeepOcrConstants::kRecServerModelFileName);
        const auto dictPath = modelDir / DeepOcrConstants::kKeysDictFileName;

        // 轻量化 4KB 文件头校验（std::filesystem::file_size + ONNX Protobuf 魔数），
        // 彻底消灭冷启将多个重型模型整份读盘后立即丢弃的 I/O 浪费
        if (!isValidOnnxFileHeader(detPath, 256)) {
            LOG_WARN("DeepOcrEngine: invalid or missing det ONNX model");
            return false;
        }
        if (!isValidOnnxFileHeader(clsPath, 256)) {
            LOG_WARN("DeepOcrEngine: invalid or missing cls ONNX model");
            return false;
        }

        std::string actualTier = desiredTier;
        // 无论 mobile 还是 server 档位，主力均校验并加载极速轻量的主力识别模型（v6 Small 20.3MB / v4 Mobile 10.4MB）；
        // 若用户开启高精度增强档，二阶仅对低置信度疑难行按需懒加载单一甜点高精度会话复核（v6 Medium Rec 73.1MB / v4 Server 86.3MB），
        // 彻底规避重型检测器多桶显存暴涨与纯 CPU 108s 卡顿。
        if (!isValidOnnxFileHeader(mobileRecPath, 256)) {
            LOG_WARN("DeepOcrEngine: invalid or missing rec ONNX model");
            return false;
        }
        bool serverModelVerified = false;
        if (desiredTier == "server") {
            if (isValidOnnxFileHeader(serverRecPath, 256, DeepOcrConstants::kMaxServerModelBytes)) {
                serverModelVerified = true;
            } else {
                LOG_WARN("DeepOcrEngine: server rec ONNX model invalid or unreadable, gracefully falling back to mobile tier");
                actualTier = "mobile";
            }
        }
        // PP-OCRv6 强制直接从 rec ONNX 内嵌元数据提取 18,708 字 50 语种词典（严禁误读遗留的旧 v4 ppocr_keys_v1.txt！）；
        // 旧版 PP-OCRv4 优先读 ppocr_keys_v1.txt 并在缺失时回退内嵌字典提取。
        std::vector<uint8_t> recBytesCache;
        if (useV6 || !loadDictionaryLines(dictPath, m_dictKeys)) {
            if (!readFileBytes(mobileRecPath, recBytesCache) ||
                !extractEmbeddedCharacterDictFromOnnx(recBytesCache.data(), recBytesCache.size(), m_dictKeys)) {
                LOG_WARN("DeepOcrEngine: invalid or missing dictionary keys file");
                return false;
            }
            LOG_INFO("DeepOcrEngine: loaded {} dictionary keys directly from rec ONNX metadata", m_dictKeys.size());
        }

        // 优先挂载系统内置 Windows.AI.MachineLearning + DirectML 原生 D3D12 紧凑固定桶管线；
        // 在 Windows 10 (Inbox WinML 仅支持 ONNX 1.4/opset 9) / Server / 显式测试回退模式下自动平滑回落 OpenCV DNN CPU
        if (!isForceOpenCvDnnEnabled()) {
        try {
            ensureComApartmentInitialized();
            const auto winmlDetPath = ensureOnnxWinMlIr8CompatiblePath(detPath);
            const auto winmlClsPath = ensureOnnxWinMlIr8CompatiblePath(clsPath);
            const auto winmlRecPath = ensureOnnxWinMlIr8CompatiblePath(mobileRecPath);

            auto ctx = std::make_unique<WinMlContext>();
            ctx->detModel = waml::LearningModel::LoadFromFilePath(winmlDetPath.wstring());
            ctx->clsModel = waml::LearningModel::LoadFromFilePath(winmlClsPath.wstring());
            ctx->recModel = waml::LearningModel::LoadFromFilePath(winmlRecPath.wstring());
            if (serverModelVerified) {
                ctx->serverRecPath = serverRecPath;
                ctx->serverCascadeEnabled = true;
            }

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
                SIZE_T selectedDedicatedVram = 0;
                const bool forceCpuOnly = isForceCpuOnlyMode();
                if (!forceCpuOnly) {
                try {
                    waml::LearningModelDevice gpuDev{nullptr};
                    // 优先自建独占高性能 D3D11 设备绑定至 WinML，确保闲置卸载时随设备销毁 100% 释放 DirectML 专用私有内存
                    try {
                        Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
                        if (SUCCEEDED(::CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory))) && dxgiFactory) {
                            Microsoft::WRL::ComPtr<IDXGIAdapter1> bestAdapter;
                            SIZE_T bestVram = 0;
                            for (UINT ai = 0;; ++ai) {
                                Microsoft::WRL::ComPtr<IDXGIAdapter1> ad;
                                if (FAILED(dxgiFactory->EnumAdapters1(ai, &ad)) || !ad) break;
                                DXGI_ADAPTER_DESC1 desc{};
                                if (FAILED(ad->GetDesc1(&desc))) continue;
                                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                                if (!bestAdapter || desc.DedicatedVideoMemory > bestVram) {
                                    bestVram = desc.DedicatedVideoMemory;
                                    bestAdapter = ad;
                                }
                            }
                            if (bestAdapter) {
                                selectedDedicatedVram = bestVram;
                                Microsoft::WRL::ComPtr<ID3D11Device> d3d11;
                                Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dCtx;
                                const D3D_FEATURE_LEVEL reqFl = D3D_FEATURE_LEVEL_11_0;
                                if (SUCCEEDED(::D3D11CreateDevice(
                                        bestAdapter.Get(),
                                        D3D_DRIVER_TYPE_UNKNOWN,
                                        nullptr,
                                        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        &reqFl, 1,
                                        D3D11_SDK_VERSION,
                                        &d3d11, nullptr, &d3dCtx)) && d3d11) {
                                    Microsoft::WRL::ComPtr<IDXGIDevice3> dxgiDev;
                                    winrt::com_ptr<IInspectable> inspectable;
                                    if (SUCCEEDED(d3d11.As(&dxgiDev)) && dxgiDev &&
                                        SUCCEEDED(::CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.Get(), inspectable.put())) &&
                                        inspectable) {
                                        auto winrtD3d = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
                                        gpuDev = waml::LearningModelDevice::CreateFromDirect3D11Device(winrtD3d);
                                        ctx->ownedD3d11Device = std::move(d3d11);
                                        ctx->ownedD3d11Context = std::move(d3dCtx);
                                        ctx->ownedDxgiDevice = std::move(dxgiDev);
                                        ctx->ownedWinrtD3dDevice = std::move(winrtD3d);
                                    }
                                }
                            }
                        }
                    } catch (...) {
                        gpuDev = nullptr;
                        ctx->ownedWinrtD3dDevice = nullptr;
                        ctx->ownedDxgiDevice.Reset();
                        ctx->ownedD3d11Context.Reset();
                        ctx->ownedD3d11Device.Reset();
                    }
                    if (!gpuDev) {
                        gpuDev = waml::LearningModelDevice(waml::LearningModelDeviceKind::DirectXHighPerformance);
                    }
                    waml::LearningModelSessionOptions probeOpts;
                    probeOpts.CloseModelOnSessionCreation(false);

                    // 同步验证 detModel 能在当前 GPU 设备上创建会话，并直接复用至首桶 [0]，
                    // 杜绝 Win10 老版 DirectML 仅通过 rec 探测却在推理阶段 det 会话创建时崩溃
                    waml::LearningModelSession detProbe(ctx->detModel, gpuDev, probeOpts);

                    bool batchOk = false;
                    try {
                        // 使用甜点主力 16x320 桶 ([1]) 一次性完成批处理探测并直接复用，永不触发 DirectML 大张量显存悬崖
                        const auto& sp1 = WinMlContext::kRecSpecs[1];
                        waml::LearningModelSession s16(ctx->recModel, gpuDev, probeOpts);
                        auto t16 = waml::TensorFloat::Create(winrt::single_threaded_vector<int64_t>({
                            sp1.n, 3, sp1.h, sp1.w}));
                        waml::LearningModelBinding b16(s16);
                        b16.Bind(ctx->recInName, t16);
                        auto out16 = s16.Evaluate(b16, L"").Outputs().Lookup(ctx->recOutName).as<waml::TensorFloat>();
                        if (out16 && out16.Shape().Size() >= 3 && out16.Shape().GetAt(0) == sp1.n) {
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
                            ctx->cpuFallbackRecSession = std::move(s1);
                            gpuReady = true;
                        }
                    } else {
                        gpuReady = true;
                    }

                    if (gpuReady) {
                        ctx->detBucketSessions[0] = std::move(detProbe);
                        ctx->device = gpuDev;
                        ctx->usingDirectMLGpu = true;
                        ctx->supportsBatchEval = batchOk;
                        // 90.5 MB Server 复核模型仅在 Win11+ 且 >= 2GB 独显上启用 GPU 静态桶；
                        // Win10 旧版 DirectML 及核显/低显存设备自动走 WinML CPU 会话，彻底规避 WDDM TDR 桌面卡死与显存崩溃
                        constexpr SIZE_T kMinServerGpuVramBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
                        ctx->allowServerGpu = tools3000::core::WinUtils::isWindows11OrNewer() &&
                                              selectedDedicatedVram >= kMinServerGpuVramBytes;
                    }
                } catch (...) {
                    gpuReady = false;
                }
                }

                if (!gpuReady) {
                    if (ctx->ownedWinrtD3dDevice) {
                        try { ctx->ownedWinrtD3dDevice.Close(); } catch (...) {}
                        ctx->ownedWinrtD3dDevice = nullptr;
                    }
                    if (ctx->ownedDxgiDevice) {
                        ctx->ownedDxgiDevice->Trim();
                        ctx->ownedDxgiDevice.Reset();
                    }
                    ctx->ownedD3d11Context.Reset();
                    ctx->ownedD3d11Device.Reset();
                    ctx->device = waml::LearningModelDevice(waml::LearningModelDeviceKind::Cpu);
                    ctx->cpuDevice = ctx->device;
                    ctx->usingDirectMLGpu = false;
                    ctx->supportsBatchEval = false;
                    ctx->allowServerGpu = false;
                }

                m_winMl = std::move(ctx);
                m_useWinMl = true;
                m_loadedModelDir = modelDir;
                m_loadedModelTier = actualTier;
                m_loadedEngineGen = desiredGen;
                m_modelsLoaded = true;
                m_modelsLoadedAtomic.store(true, std::memory_order_release);
                m_activeSessionCountAtomic.store(m_winMl->countActiveSessions(), std::memory_order_release);
                LOG_INFO("DeepOcrEngine: PP-OCR{} loaded with native WinML ONNX Runtime ({}{}, tier={}, serverGpu={}, dict={} keys)",
                         m_loadedEngineGen,
                         m_winMl->usingDirectMLGpu ? "DirectML GPU" : "CPU SIMD",
                         m_winMl->supportsBatchEval ? " + CompactBatchCascade" : "",
                         m_loadedModelTier,
                         m_winMl->allowServerGpu ? "true" : "false",
                         m_dictKeys.size());
                return true;
            }
        } catch (const winrt::hresult_error& e) {
            LOG_INFO("DeepOcrEngine: native WinML load unavailable (hr=0x{:08X}), falling back to OpenCV DNN CPU",
                     static_cast<uint32_t>(e.code().value));
            m_winMl.reset();
            m_useWinMl = false;
            m_activeSessionCountAtomic.store(0, std::memory_order_release);
        } catch (const std::exception& e) {
            LOG_INFO("DeepOcrEngine: native WinML load unavailable ({}), falling back to OpenCV DNN CPU", e.what());
            m_winMl.reset();
            m_useWinMl = false;
            m_activeSessionCountAtomic.store(0, std::memory_order_release);
        } catch (...) {
            LOG_INFO("DeepOcrEngine: native WinML load unavailable, falling back to OpenCV DNN CPU");
            m_winMl.reset();
            m_useWinMl = false;
            m_activeSessionCountAtomic.store(0, std::memory_order_release);
        }
        } else {
            LOG_INFO("DeepOcrEngine: forced OpenCV DNN CPU fallback mode active");
        }

        // 无损回退路径：OpenCV DNN 固定分桶推理引擎（仅在 WinML 不可用时按需整文件读盘）
        std::vector<uint8_t> detBytes, clsBytes;
        if (!readFileBytes(detPath, detBytes) ||
            !readFileBytes(clsPath, clsBytes) ||
            (recBytesCache.empty() && !readFileBytes(mobileRecPath, recBytesCache))) {
            LOG_WARN("DeepOcrEngine: OpenCV DNN fallback failed to read ONNX model files");
            return false;
        }
        const auto& recBytes = recBytesCache;

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
        m_loadedModelTier = actualTier;
        m_loadedEngineGen = desiredGen;
        m_modelsLoaded = true;
        m_modelsLoadedAtomic.store(true, std::memory_order_release);
        LOG_INFO("DeepOcrEngine: PP-OCR{} ONNX models loaded via OpenCV DNN CPU fallback (tier={}, dict={} keys)", m_loadedEngineGen, m_loadedModelTier, m_dictKeys.size());
        return true;
    } catch (const winrt::hresult_error& e) {
        LOG_ERROR("DeepOcrEngine: WinRT exception loading PP-OCRv4 models: hr=0x{:08X}",
                  static_cast<uint32_t>(e.code().value));
        m_modelsLoaded = false;
        m_modelsLoadedAtomic.store(false, std::memory_order_release);
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("DeepOcrEngine: exception loading PP-OCRv4 models: {}", e.what());
        m_modelsLoaded = false;
        m_modelsLoadedAtomic.store(false, std::memory_order_release);
        return false;
    } catch (...) {
        LOG_ERROR("DeepOcrEngine: unknown exception loading PP-OCRv4 models");
        m_modelsLoaded = false;
        m_modelsLoadedAtomic.store(false, std::memory_order_release);
        return false;
    }
}

namespace {

struct ScriptGlyphProfile {
    size_t hangul = 0;
    size_t kana = 0;
    size_t japaneseNo = 0;
    size_t cyrillic = 0;
    size_t arabic = 0;
    size_t thai = 0;
    size_t latinDiacritic = 0;
    size_t rareLatinExt = 0;
    size_t cjkHan = 0;
    size_t asciiAlpha = 0;
    size_t totalChars = 0;
    float visualUnits = 0.0f;
    size_t asciiWordCount = 0;
    size_t mixedCaseAnomalousWords = 0;
};

ScriptGlyphProfile analyzeScriptGlyphs(const std::string& utf8) noexcept {
    ScriptGlyphProfile p{};
    const size_t n = utf8.size();
    size_t i = 0;

    // 同步扫描纯 ASCII 伪拉丁大小写畸变词（俄文西里尔被中文字典强拼成 ABToHOMHOe / pacno3HaBaHne 的显著特征）
    auto flushAsciiWord = [&](const std::string& word) {
        if (word.size() < 5) return;
        bool hasAlpha = false;
        for (char c : word) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                hasAlpha = true;
                break;
            }
        }
        if (!hasAlpha) return;
        ++p.asciiWordCount;
        // 将字母连续同大小写段拆解为 runs，严格排除标准 CamelCase/PascalCase（单大写接 >=2 小写，如 getUserProfile / ContentType），
        // 仅将真实伪拉丁畸变（如 ABToHOMHOe / CoxpaHnTb / pacno3HaBaHne：单字母小写夹心或大写连块夹单小写）记为异常词
        struct CaseRun { int sign; int len; };
        std::vector<CaseRun> runs;
        runs.reserve(8);
        bool hasInteriorDigitInAlpha = false;
        for (size_t k = 0; k < word.size(); ++k) {
            const char c = word[k];
            const int curCase = (c >= 'A' && c <= 'Z') ? 1 : ((c >= 'a' && c <= 'z') ? -1 : 0);
            if (curCase == 0) {
                if (c >= '0' && c <= '9' && !runs.empty() && k + 1 < word.size()) {
                    const char nxt = word[k + 1];
                    if ((nxt >= 'A' && nxt <= 'Z') || (nxt >= 'a' && nxt <= 'z')) {
                        hasInteriorDigitInAlpha = true;
                    }
                }
                continue;
            }
            if (!runs.empty() && runs.back().sign == curCase) {
                ++runs.back().len;
            } else {
                runs.push_back({curCase, 1});
            }
        }
        const int flips = runs.empty() ? 0 : static_cast<int>(runs.size() - 1);
        if (flips >= 2) {
            int singleLowerRuns = 0;
            bool multiUpperToSingleLower = false;
            bool pureCamelCase = true;
            for (size_t r = 0; r < runs.size(); ++r) {
                if (runs[r].sign == -1 && runs[r].len == 1) {
                    ++singleLowerRuns;
                    if (r > 0 && runs[r - 1].sign == 1 && runs[r - 1].len >= 2) {
                        multiUpperToSingleLower = true;
                    }
                }
                // 标准驼峰特征：每次大写段若后面还有小写段，大写长度为 1 且紧跟小写长度 >= 2（或词尾全大写缩写如 BaseURL / UserID）
                if (runs[r].sign == 1 && r + 1 < runs.size()) {
                    if (runs[r].len > 1 || runs[r + 1].len < 2) {
                        pureCamelCase = false;
                    }
                }
            }
            if (!pureCamelCase &&
                (multiUpperToSingleLower ||
                 (flips >= 3 && singleLowerRuns >= 2) ||
                 (flips >= 3 && hasInteriorDigitInAlpha))) {
                ++p.mixedCaseAnomalousWords;
            }
        }
    };

    std::string curWord;
    while (i < n) {
        const uint8_t b0 = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = 0;
        size_t adv = 1;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < n) {
            cp = ((b0 & 0x1Fu) << 6) | (static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu);
            adv = 2;
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < n) {
            cp = ((b0 & 0x0Fu) << 12) | ((static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<uint8_t>(utf8[i + 2]) & 0x3Fu);
            adv = 3;
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < n) {
            cp = ((b0 & 0x07u) << 18) | ((static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<uint8_t>(utf8[i + 2]) & 0x3Fu) << 6) | (static_cast<uint8_t>(utf8[i + 3]) & 0x3Fu);
            adv = 4;
        }
        i += adv;

        if (cp < 0x80 && ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9'))) {
            curWord.push_back(static_cast<char>(cp));
        } else {
            if (!curWord.empty()) {
                flushAsciiWord(curWord);
                curWord.clear();
            }
        }

        if (cp == 0x20) {
            p.visualUnits += 0.30f; // 空格在图像行中占据真实水平像素宽度，避免英文/代码行密度被误判塌方
            continue;
        }
        if (cp < 0x20) continue;
        ++p.totalChars;
        if ((cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F)) {
            ++p.hangul;
            p.visualUnits += 1.0f;
        } else if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)) {
            ++p.kana;
            if (cp == 0x306E) ++p.japaneseNo;
            p.visualUnits += 1.0f;
        } else if (cp >= 0x0400 && cp <= 0x04FF) {
            ++p.cyrillic;
            p.visualUnits += 0.58f;
        } else if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
                   (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF)) {
            ++p.arabic;
            p.visualUnits += 0.52f;
        } else if (cp >= 0x0E00 && cp <= 0x0E7F) {
            ++p.thai;
            p.visualUnits += 0.55f;
        } else if ((cp >= 0x00C0 && cp <= 0x024F) || cp == 0x00BF || cp == 0x00A1) {
            ++p.latinDiacritic;
            if (cp >= 0x0100 && cp <= 0x024F) {
                ++p.rareLatinExt;
            }
            p.visualUnits += 0.56f;
        } else if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) {
            ++p.cjkHan;
            p.visualUnits += 1.0f;
        } else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
            ++p.asciiAlpha;
            p.visualUnits += 0.55f;
        } else {
            p.visualUnits += 0.55f;
        }
    }
    if (!curWord.empty()) flushAsciiWord(curWord);
    return p;
}

bool hasIberianOrExtendedLatinHint(const std::string& utf8) noexcept {
    std::string lower;
    lower.reserve(utf8.size());
    for (unsigned char c : utf8) {
        lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c));
    }
    // 严格剔除 3 字母域名/代码碰撞词（如 ".com" / "del" / "con" / "die"），仅匹配高区分度欧陆词尾与 >=5 字母词汇
    int score = 0;
    size_t i = 0;
    const size_t n = lower.size();
    static constexpr const char* kStrongWords[] = {
        "espanol", "portugues", "francais", "deutsch", "italiano",
        "configuracion", "configuracao", "traduccion", "traducao",
        "aplicacion", "aplicacao", "informacion", "informacao",
        "automatica", "automatico", "einstellungen", "ubersicht",
        "preferencias", "definicoes", "atualizacao", "deteccao", "protecao", "cancelacao",
        "rapido", "rapida", "tambien", "tambem", "entao", "guardar", "cambios",
        "desea", "actuales", "confirmar", "cancelar", "idioma", "idiomas",
        "configurar", "reconocimiento", "instantanea", "senalizacion", "diseno"
    };
    static constexpr const char* kMediumWords[] = {
        "sobre", "desde", "cuando", "donde", "todos", "todas", "entre",
        "estos", "estas", "voce", "mais", "pelo", "pela", "nicht", "avec", "pour", "dans"
    };

    while (i < n) {
        while (i < n && !(lower[i] >= 'a' && lower[i] <= 'z')) ++i;
        const size_t start = i;
        while (i < n && (lower[i] >= 'a' && lower[i] <= 'z')) ++i;
        const size_t len = i - start;
        if (len < 4) continue;
        const std::string_view w(lower.data() + start, len);

        if (len >= 6 && (w.ends_with("cion") || w.ends_with("cao") || w.ends_with("sao") ||
                         w.ends_with("oes") || w.ends_with("mente"))) {
            score += 2;
        }
        for (const char* sw : kStrongWords) {
            if (w == sw) {
                score += 2;
                break;
            }
        }
        for (const char* mw : kMediumWords) {
            if (w == mw) {
                score += 1;
                break;
            }
        }
        if (score >= 2) return true;
    }
    return false;
}

/// 西里尔-拉丁视觉同形字上下文净化（仅在 detectedScript == "cyrillic" 时调用）：
/// 当拉丁英文词内部混入 1~2 个视觉完全相同的西里尔字母（如 "DireсtML" 中的 U+0441 'с'）时，
/// 同步将 dec.chars 与 dec.text 还原为对应 ASCII 拉丁字母，确保复制与检索 100% 纯净。
void normalizeCyrillicLatinHomoglyphs(CtcDecodeOutput& dec) {
    if (dec.chars.size() < 3) return;

    auto cyrHomoglyphToAscii = [](const std::string& ch) -> char {
        if (ch == "\xD0\xB0") return 'a'; // а
        if (ch == "\xD0\xB5") return 'e'; // е
        if (ch == "\xD0\xBE") return 'o'; // о
        if (ch == "\xD1\x80") return 'p'; // р
        if (ch == "\xD1\x81") return 'c'; // с
        if (ch == "\xD1\x83") return 'y'; // у
        if (ch == "\xD1\x85") return 'x'; // х
        if (ch == "\xD0\x90") return 'A'; // А
        if (ch == "\xD0\x92") return 'B'; // В
        if (ch == "\xD0\x95") return 'E'; // Е
        if (ch == "\xD0\x9A") return 'K'; // К
        if (ch == "\xD0\x9C") return 'M'; // М
        if (ch == "\xD0\x9D") return 'H'; // Н
        if (ch == "\xD0\x9E") return 'O'; // О
        if (ch == "\xD0\xA0") return 'P'; // Р
        if (ch == "\xD0\xA1") return 'C'; // С
        if (ch == "\xD0\xA2") return 'T'; // Т
        if (ch == "\xD0\xA5") return 'X'; // Х
        return '\0';
    };
    auto isAsciiAlphaNum = [](const std::string& ch) -> bool {
        if (ch.size() != 1) return false;
        const unsigned char c = static_cast<unsigned char>(ch[0]);
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    auto isCyrillicGlyph = [](const std::string& ch) -> bool {
        if (ch.size() != 2) return false;
        const uint8_t b0 = static_cast<uint8_t>(ch[0]);
        const uint8_t b1 = static_cast<uint8_t>(ch[1]);
        if ((b0 & 0xE0) != 0xC0) return false;
        const uint32_t cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
        return cp >= 0x0400 && cp <= 0x04FF;
    };

    bool changed = false;
    const size_t n = dec.chars.size();
    size_t i = 0;
    while (i < n) {
        while (i < n && !isAsciiAlphaNum(dec.chars[i].utf8Char) && !isCyrillicGlyph(dec.chars[i].utf8Char)) {
            ++i;
        }
        const size_t start = i;
        size_t latinCnt = 0;
        size_t cyrCnt = 0;
        size_t cyrHomoglyphCnt = 0;
        while (i < n && (isAsciiAlphaNum(dec.chars[i].utf8Char) || isCyrillicGlyph(dec.chars[i].utf8Char))) {
            if (isAsciiAlphaNum(dec.chars[i].utf8Char)) {
                ++latinCnt;
            } else {
                ++cyrCnt;
                if (cyrHomoglyphToAscii(dec.chars[i].utf8Char) != '\0') {
                    ++cyrHomoglyphCnt;
                }
            }
            ++i;
        }
        if (latinCnt >= 2 && cyrCnt >= 1 && cyrCnt == cyrHomoglyphCnt && latinCnt >= cyrCnt) {
            for (size_t k = start; k < i; ++k) {
                const char mapped = cyrHomoglyphToAscii(dec.chars[k].utf8Char);
                if (mapped != '\0') {
                    dec.chars[k].utf8Char.assign(1, mapped);
                    changed = true;
                }
            }
        }
    }

    if (changed) {
        std::string rebuilt;
        rebuilt.reserve(dec.text.size());
        for (const auto& ch : dec.chars) {
            rebuilt += ch.utf8Char;
        }
        dec.text = std::move(rebuilt);
    }
}

/// 阿拉伯语 PP-OCRv5 视觉左->右 CTC 输出还原为标准逻辑阅读序（RTL）：
/// 1) 仅当该行包含真实阿拉伯语码点时执行整体字符序反转（纯英文/数字行零改动直通）；
/// 2) 保留嵌入的连续 ASCII 字母/数字段（如 2026、Tools3000、95.2%）内部正序；
/// 3) 同步搬移 CtcDecodedChar 自身携带的 normLeft/normRight，保证原图字符级选框 1:1 精准对应。
void normalizeArabicRtlLine(CtcDecodeOutput& dec) {
    if (dec.chars.size() < 2) return;
    auto firstUtf8Codepoint = [](const std::string& s) noexcept -> uint32_t {
        if (s.empty()) return 0u;
        const uint8_t b0 = static_cast<uint8_t>(s[0]);
        if (b0 < 0x80u) return b0;
        if ((b0 & 0xE0u) == 0xC0u && s.size() >= 2) {
            return ((b0 & 0x1Fu) << 6) | (static_cast<uint8_t>(s[1]) & 0x3Fu);
        }
        if ((b0 & 0xF0u) == 0xE0u && s.size() >= 3) {
            return ((b0 & 0x0Fu) << 12) |
                   ((static_cast<uint8_t>(s[1]) & 0x3Fu) << 6) |
                   (static_cast<uint8_t>(s[2]) & 0x3Fu);
        }
        if ((b0 & 0xF8u) == 0xF0u && s.size() >= 4) {
            return ((b0 & 0x07u) << 18) |
                   ((static_cast<uint8_t>(s[1]) & 0x3Fu) << 12) |
                   ((static_cast<uint8_t>(s[2]) & 0x3Fu) << 6) |
                   (static_cast<uint8_t>(s[3]) & 0x3Fu);
        }
        return b0;
    };
    auto isArabicCp = [](uint32_t cp) noexcept {
        return (cp >= 0x0600u && cp <= 0x06FFu) ||
               (cp >= 0x0750u && cp <= 0x077Fu) ||
               (cp >= 0x08A0u && cp <= 0x08FFu) ||
               (cp >= 0xFB50u && cp <= 0xFDFFu) ||
               (cp >= 0xFE70u && cp <= 0xFEFFu);
    };
    auto isLtrRunChar = [](uint32_t cp) noexcept {
        return (cp >= '0' && cp <= '9') ||
               (cp >= 'A' && cp <= 'Z') ||
               (cp >= 'a' && cp <= 'z') ||
               cp == '_' || cp == '-';
    };
    auto isLtrBridgeChar = [](uint32_t cp) noexcept {
        return cp == '.' || cp == ',' || cp == ':' || cp == '/' || cp == '%';
    };

    bool hasArabic = false;
    for (const auto& ch : dec.chars) {
        if (isArabicCp(firstUtf8Codepoint(ch.utf8Char))) {
            hasArabic = true;
            break;
        }
    }
    if (!hasArabic) return;

    std::reverse(dec.chars.begin(), dec.chars.end());

    // 将嵌入的连续 LTR 拉丁/数字子段局部翻回左->右正序
    const size_t n = dec.chars.size();
    size_t i = 0;
    while (i < n) {
        if (!isLtrRunChar(firstUtf8Codepoint(dec.chars[i].utf8Char))) {
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < n) {
            const uint32_t cpJ = firstUtf8Codepoint(dec.chars[j].utf8Char);
            if (isLtrRunChar(cpJ)) {
                ++j;
                continue;
            }
            if (isLtrBridgeChar(cpJ) && j + 1 < n &&
                isLtrRunChar(firstUtf8Codepoint(dec.chars[j + 1].utf8Char))) {
                j += 2;
                continue;
            }
            break;
        }
        if (j - i >= 2) {
            std::reverse(dec.chars.begin() + static_cast<ptrdiff_t>(i),
                         dec.chars.begin() + static_cast<ptrdiff_t>(j));
        }
        i = j;
    }

    std::string rebuilt;
    rebuilt.reserve(dec.text.size());
    for (const auto& ch : dec.chars) {
        rebuilt += ch.utf8Char;
    }
    dec.text = std::move(rebuilt);
}

} // namespace

std::vector<OcrResult> DeepOcrEngine::extractText(const cv::Mat& image, const std::string& scriptModeOverride) {
    tools3000::core::TraceId::Scope traceScope;
    m_lastExtractFailed.store(false, std::memory_order_release);
    if (image.empty()) return {};

    bool installed = false;
    const std::filesystem::path modelDir = resolveModelDirectory(&installed);
    if (!installed) return {};

    const std::string effectiveScriptMode = scriptModeOverride.empty()
        ? configuredScriptMode()
        : normalizeScriptMode(scriptModeOverride);

    cv::Mat bgr = ensureBgr3(image);
    if (bgr.empty()) return {};

    // 生产宿主环境优先通过独立 OCR Worker 子进程 (--worker=ocr) 执行 DirectML GPU 推理：
    // 主进程 0 加载 DirectML/ONNX Runtime/显卡内核驱动，子进程采用两阶段智能热保活（60s 软修剪 + 15 分钟 OS 双归零退出）。
    // 若独立子进程在推理中崩溃或超时（WorkerFailed），绝不回退到主进程执行 DirectML/WinML 以免拖垮整个宿主程序！
    if (OcrWorkerClient::instance().isOutOfProcessAvailable()) {
        std::vector<OcrResult> workerResults;
        DeepOcrRunStats workerStats{};
        std::string workerActiveTier;
        const OcrWorkerExtractStatus status = OcrWorkerClient::instance().extractTextEx(
            bgr,
            modelDir,
            effectiveModelTier(),
            effectiveScriptMode,
            workerResults,
            workerStats,
            workerActiveTier,
            currentStageCallback());
        if (status == OcrWorkerExtractStatus::Succeeded) {
            {
                std::lock_guard<std::mutex> statsLock(m_statsMutex);
                m_lastRunStats = workerStats;
            }
            m_runSequence.fetch_add(1, std::memory_order_release);
            return workerResults;
        }
        if (status == OcrWorkerExtractStatus::WorkerFailed) {
            LOG_WARN("DeepOcrEngine: isolated OCR worker failed; aborting DeepOCR without unsafe in-process fallback");
            m_lastExtractFailed.store(true, std::memory_order_release);
            return {};
        }
    }

    const auto tTotalStart = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_inferMutex);
    struct ActivityTouchGuard {
        DeepOcrEngine* self = nullptr;
        ~ActivityTouchGuard() {
            if (self) self->touchActivityAndScheduleIdleUnload();
        }
    } activityGuard{this};

    const cv::Mat originalBgr = bgr;
    std::vector<OcrResult> rawBlocks;
    long long loadMs = 0;
    long long detMs = 0;
    long long packMs = 0;
    long long recMs = 0;
    long long refineMs = 0;
    size_t serverRefineCandidateCount = 0;
    size_t serverRefinedCount = 0;
    size_t detectedQuadCount = 0;
    size_t packedStripCount = 0;
    bool autoScriptSwitched = false;
    std::string bucketProfile;
    std::string detectedScript = "cjk";
    bool pageRotated90Or270 = false;
    bool pageRotated270Ccw = false;

    for (int inferAttempt = 0; inferAttempt < 2; ++inferAttempt) {
        bgr = originalBgr;
        rawBlocks.clear();
        detMs = 0;
        packMs = 0;
        recMs = 0;
        refineMs = 0;
        serverRefinedCount = 0;
        detectedQuadCount = 0;
        packedStripCount = 0;
        bucketProfile.clear();
        detectedScript = "cjk";
        pageRotated90Or270 = false;
        pageRotated270Ccw = false;

        const auto tLoadStart = std::chrono::steady_clock::now();
        if (!m_modelsLoaded) {
            const std::string genHint = deep_ocr_detail::isV6BaseBundlePresent(modelDir) ? "v6" : "v4";
            notifyStage(OcrRecognitionStage::DeepModelLoading, 0u, genHint);
        }
        if (!ensureModelsLoadedLocked(modelDir)) {
            m_lastExtractFailed.store(true, std::memory_order_release);
            return {};
        }
        const auto tInferStart = std::chrono::steady_clock::now();
        loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            tInferStart - tLoadStart).count();

        // 若当前落入 CPU 兼容深度模式（OpenCV DNN 或 WinML CPU），在进入检测前即刻反馈给用户，避免耗时数秒时界面无反馈
        if (!m_useWinMl) {
            notifyStage(OcrRecognitionStage::DeepCpuInference, 0u, "opencv_dnn");
        } else if (m_winMl && !m_winMl->usingDirectMLGpu) {
            notifyStage(OcrRecognitionStage::DeepCpuInference, 0u, "winml_cpu");
        } else if (m_loadedModelTier == "server") {
            notifyStage(OcrRecognitionStage::DeepMultiLineRecognizing, 0u, "server");
        }

        if (m_useWinMl) {
            ensureComApartmentInitialized();
        }

        bool gpuErrorNeedsCpuRetry = false;
        try {
        // 0. 自适应视觉增强前处理（红章压字穿透 + 阴影纸面/手写光照归一化 / 拍屏摩尔纹抑制；清晰数字截图 0 拷贝直通）
        bool applyPreprocToRec = false;
        bool isScreenPhoto = false;
        cv::Mat detSourceBgr = preprocessImageForDetection(bgr, &applyPreprocToRec, &isScreenPhoto);
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
            if (!m_winMl->usingDirectMLGpu) {
                // WinML CPU SIMD 动态精确 32 对齐画布（0 固定大桶无效像素浪费，仅需单一 detSession）
                const int alignW = (std::max)(DeepOcrConstants::kDetStrideAlign,
                    static_cast<int>(std::round(static_cast<float>(origW) * desiredRatio / 32.0f)) * 32);
                const int alignH = (std::max)(DeepOcrConstants::kDetStrideAlign,
                    static_cast<int>(std::round(static_cast<float>(origH) * desiredRatio / 32.0f)) * 32);
                ratioW = static_cast<float>(alignW) / static_cast<float>(origW);
                ratioH = static_cast<float>(alignH) / static_cast<float>(origH);

                cv::Mat detResized;
                cv::resize(detSourceBgr, detResized, cv::Size(alignW, alignH), 0.0, 0.0,
                           desiredRatio < 1.0f ? cv::INTER_AREA : cv::INTER_LINEAR);

                if (waml::LearningModelSession* detSess = m_winMl->ensureDetSession(0)) {
                    m_activeSessionCountAtomic.store(m_winMl->countActiveSessions(), std::memory_order_release);
                    auto detTensor = waml::TensorFloat::Create(
                        winrt::single_threaded_vector<int64_t>({1, 3, alignH, alignW}));
                    {
                        WinMlTensorBufferLock inLock(detTensor);
                        const size_t plane = static_cast<size_t>(alignH) * static_cast<size_t>(alignW);
                        if (inLock.data && inLock.floatCount >= plane * 3) {
                            float* c0 = inLock.data;
                            float* c1 = inLock.data + plane;
                            float* c2 = inLock.data + plane * 2;
                            cv::parallel_for_(cv::Range(0, alignH), [&](const cv::Range& range) {
                                constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
                                constexpr float kInvStd[3] = {1.0f / 0.229f, 1.0f / 0.224f, 1.0f / 0.225f};
                                constexpr float kInv255 = 1.0f / 255.0f;
                                for (int y = range.start; y < range.end; ++y) {
                                    const auto* row = detResized.ptr<cv::Vec3b>(y);
                                    const size_t base = static_cast<size_t>(y) * static_cast<size_t>(alignW);
                                    for (int x = 0; x < alignW; ++x) {
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
                        const size_t needFloats = static_cast<size_t>(outH) * static_cast<size_t>(outW);
                        if (outH > 0 && outW > 0) {
                            probOut.create(outH, outW, CV_32FC1);
                            WinMlTensorBufferLock outLock(outTensor);
                            if (outLock.data && outLock.floatCount >= needFloats) {
                                std::memcpy(probOut.ptr<float>(), outLock.data, needFloats * sizeof(float));
                            } else {
                                outLock.release();
                                auto vecView = outTensor.GetAsVectorView();
                                vecView.GetMany(0, winrt::array_view<float>(probOut.ptr<float>(), probOut.ptr<float>() + needFloats));
                            }
                        }
                    }
                }
            } else {
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
                m_activeSessionCountAtomic.store(m_winMl->countActiveSessions(), std::memory_order_release);
                // 注意：严禁跨 Evaluate 复用绑定过 DirectML GPU 的 TensorFloat，否则 WinML GPU 缓存位不感知裸指针写回导致脏帧
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
            } else {
                throw std::runtime_error("WinML GPU det session unavailable");
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
            std::chrono::steady_clock::now() - tInferStart).count();
        detectedQuadCount = quads.size();
        if (quads.empty()) {
            const long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tTotalStart).count();
            const size_t sessCnt = activeSessionCount();
            {
                std::lock_guard<std::mutex> sl(m_statsMutex);
                DeepOcrRunStats emptyStats{};
                emptyStats.totalMs = totalMs;
                emptyStats.loadMs = loadMs;
                emptyStats.detMs = detMs;
                emptyStats.activeSessions = sessCnt;
                m_lastRunStats = std::move(emptyStats);
            }
            LOG_INFO("DeepOcrEngine: recognized 0 lines on {}x{} in {} ms (load={} ms, det={} ms)",
                     origW, origH, totalMs, loadMs, detMs);
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
        if (isScreenPhoto) {
            rescueDarkBannerQuadsOnScreenPhoto(quads, bgr);
        }
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
            if (m_useWinMl && m_winMl) {
                if (waml::LearningModelSession* clsSess = m_winMl->ensureClsSession()) {
                    m_activeSessionCountAtomic.store(m_winMl->countActiveSessions(), std::memory_order_release);
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
                    waml::LearningModelBinding binding(*clsSess);
                    binding.Bind(m_winMl->clsInName, clsTensor);
                    auto evalRes = clsSess->Evaluate(binding, L"");
                    auto outTensor = evalRes.Outputs().Lookup(m_winMl->clsOutName).as<waml::TensorFloat>();
                    WinMlTensorBufferLock outLock(outTensor);
                    if (outLock.data && outLock.floatCount >= 2) {
                        p0 = outLock.data[0];
                        p1 = outLock.data[1];
                    }
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

        auto buildCleanStrip = [&](const cv::Mat& subCrop, const cv::Vec3b& bgColor, int origCropH, int& outRw) -> cv::Mat {
            const float subScale = static_cast<float>(DeepOcrConstants::kRecImgHeight) /
                                   static_cast<float>((std::max)(1, subCrop.rows));
            outRw = std::clamp(
                static_cast<int>(std::ceil(static_cast<float>(subCrop.cols) * subScale)),
                8, kMaxPieceW);
            cv::Mat rc;
            cv::resize(subCrop, rc, cv::Size(outRw, DeepOcrConstants::kRecImgHeight), 0.0, 0.0,
                       subCrop.rows < DeepOcrConstants::kRecImgHeight ? cv::INTER_CUBIC : cv::INTER_AREA);
            cv::Vec3b effectiveBg = bgColor;
            const bool invertedDarkStrip = isScreenPhoto && normalizeDarkScreenPhotoStrip(rc, origCropH, effectiveBg);
            if (!invertedDarkStrip) {
                enhanceLowContrastStripCrop(rc);
            }
            const int contentW = outRw + kStripLeadPad * 2;
            cv::Mat stripBgr(DeepOcrConstants::kRecImgHeight, contentW, CV_8UC3,
                             cv::Scalar(effectiveBg[0], effectiveBg[1], effectiveBg[2]));
            rc.copyTo(stripBgr(cv::Rect(kStripLeadPad, 0, outRw, DeepOcrConstants::kRecImgHeight)));
            return stripBgr;
        };

        struct RawQuadCrop {
            cv::Rect lineBox;
            cv::Mat invPersp;
            cv::Mat crop;
            cv::Vec3b bgColor{255, 255, 255};
            bool uniformBg = false;
            bool rotatedVertical = false;
            bool rotated180 = false;
            bool valid = false;
        };
        std::vector<RawQuadCrop> rawCrops(quads.size());
        std::vector<size_t> verticalIndices;
        verticalIndices.reserve(quads.size());

        for (size_t qi = 0; qi < quads.size(); ++qi) {
            const auto& q = quads[qi];
            auto& rc = rawCrops[qi];
            rc.lineBox = rawQuadBoxes[qi];
            if (quadAxisFlags[qi]) {
                rc.lineBox = refineAxisAlignedLineBox(bgr, rc.lineBox, rawQuadBoxes, qi, &rc.bgColor, &rc.uniformBg);
                rc.crop = bgr(rc.lineBox);
            } else {
                rc.crop = warpPerspectiveQuad(bgr, q.corners, &rc.invPersp, &rc.rotatedVertical);
                if (!rc.crop.empty()) {
                    rc.bgColor = estimateBorderBackgroundColor(rc.crop, &rc.uniformBg);
                }
            }
            if (rc.crop.empty() || rc.crop.cols < 2 || rc.crop.rows < 2) continue;
            rc.valid = true;
            if (rc.rotatedVertical) {
                verticalIndices.push_back(qi);
            }
        }

        // 90°/270° 竖排文档批量代表投票方向检测：
        // 若竖框数量 >= 4，先挑面积最大的 5 个代表框做 180° 方向投票；共识 >= 80% 时直接批量应用（将 30+ 次 GPU 往返降至 5 次）
        if (verticalIndices.size() >= 4) {
            std::vector<size_t> byArea = verticalIndices;
            std::sort(byArea.begin(), byArea.end(), [&](size_t a, size_t b) {
                const int areaA = rawCrops[a].crop.cols * rawCrops[a].crop.rows;
                const int areaB = rawCrops[b].crop.cols * rawCrops[b].crop.rows;
                return areaA > areaB;
            });
            const size_t probeCount = (std::min<size_t>)(5, byArea.size());
            size_t rot180Votes = 0;
            for (size_t k = 0; k < probeCount; ++k) {
                auto& rc = rawCrops[byArea[k]];
                rc.rotated180 = checkRotate180IfNeeded(rc.crop);
                if (rc.rotated180) ++rot180Votes;
            }
            const bool consensusRot180 = (rot180Votes * 5 >= probeCount * 4);
            const bool consensusNormal = ((probeCount - rot180Votes) * 5 >= probeCount * 4);
            for (size_t k = probeCount; k < byArea.size(); ++k) {
                auto& rc = rawCrops[byArea[k]];
                if (consensusRot180) {
                    cv::rotate(rc.crop, rc.crop, cv::ROTATE_180);
                    rc.rotated180 = true;
                } else if (consensusNormal) {
                    rc.rotated180 = false;
                } else {
                    rc.rotated180 = checkRotate180IfNeeded(rc.crop);
                }
            }
        } else {
            for (size_t qi : verticalIndices) {
                rawCrops[qi].rotated180 = checkRotate180IfNeeded(rawCrops[qi].crop);
            }
        }

        for (size_t qi = 0; qi < quads.size(); ++qi) {
            auto& rc = rawCrops[qi];
            if (!rc.valid) continue;
            const cv::Mat& crop = rc.crop;
            const cv::Vec3b bgColor = rc.bgColor;
            const bool uniformBg = rc.uniformBg;
            const bool rotatedVertical = rc.rotatedVertical;

            auto& qm = quadMetas[qi];
            qm.lineBox = rc.lineBox;
            qm.invPersp = std::move(rc.invPersp);
            qm.origCropW = crop.cols;
            qm.origCropH = crop.rows;
            qm.rotated180 = rc.rotated180;
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
                cv::Mat stripBgr = buildCleanStrip(paddedCrop, bgColor, crop.rows, rw);
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
                    cv::Mat stripBgr = buildCleanStrip(subCrop, bgColor, crop.rows, rw);
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
        packMs = std::chrono::duration_cast<std::chrono::milliseconds>(tRecStart - tInferStart).count() - detMs;
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
            std::vector<CtcDecodeOutput> pieceDecoded(pieces.size());

            // 1) 一阶极速 Mobile 识别：
            //    - WinML CPU SIMD 模式：按实际宽度升序排序 + 精确 32 对齐动态微批（仅需单一 recSession，0 多桶显存占用，0 无效 padding）
            //    - DirectML GPU 模式：2 档主力静态桶（16x320 / 2x960）零拷贝直写，收敛冷编译 Session 数至 2~3
            std::vector<float> hostOutBuf;

            // 统一零拷贝原位解码器：直接映射 TensorFloat 主机缓冲 + 多核并行数学精确提前终止 CTC Greedy 解码
            auto decodeWinMlRecEvalWithDict = [&](const waml::LearningModelEvaluationResult& evalRes,
                                                  int actualB,
                                                  const winrt::hstring& outName,
                                                  const std::vector<std::string>& dictKeys,
                                                  auto&& getValidRatio,
                                                  auto&& onDecoded) {
                auto outTensor = evalRes.Outputs().Lookup(outName).as<waml::TensorFloat>();
                if (!outTensor) return;
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
                if (seqT <= 0 || numClasses <= 1) return;

                const size_t oneOutFloats = static_cast<size_t>(seqT) * static_cast<size_t>(numClasses);
                const size_t needOutFloats = static_cast<size_t>(actualB) * oneOutFloats;
                WinMlTensorBufferLock outLock(outTensor);
                float* decodeBasePtr = nullptr;
                if (outLock.data && outLock.floatCount >= needOutFloats) {
                    decodeBasePtr = outLock.data;
                } else {
                    outLock.release();
                    hostOutBuf.resize(needOutFloats);
                    outTensor.GetAsVectorView().GetMany(
                        0, winrt::array_view<float>(hostOutBuf.data(), hostOutBuf.data() + needOutFloats));
                    decodeBasePtr = hostOutBuf.data();
                }

                if (actualB > 1) {
                    std::vector<CtcDecodeOutput> tmpDec(static_cast<size_t>(actualB));
                    cv::parallel_for_(cv::Range(0, actualB), [&](const cv::Range& br) {
                        for (int b = br.start; b < br.end; ++b) {
                            cv::Mat logits2d(seqT, numClasses, CV_32FC1, decodeBasePtr + static_cast<size_t>(b) * oneOutFloats);
                            tmpDec[static_cast<size_t>(b)] = decodeCtcGreedy(logits2d, dictKeys, getValidRatio(b));
                        }
                    });
                    for (int b = 0; b < actualB; ++b) {
                        onDecoded(b, std::move(tmpDec[static_cast<size_t>(b)]));
                    }
                } else {
                    cv::Mat logits2d(seqT, numClasses, CV_32FC1, decodeBasePtr);
                    onDecoded(0, decodeCtcGreedy(logits2d, dictKeys, getValidRatio(0)));
                }
            };

            auto decodeWinMlRecEval = [&](const waml::LearningModelEvaluationResult& evalRes,
                                          int actualB,
                                          const winrt::hstring& outName,
                                          auto&& getValidRatio,
                                          auto&& onDecoded) {
                decodeWinMlRecEvalWithDict(evalRes, actualB, outName, m_dictKeys, getValidRatio, onDecoded);
            };

            // 轻量 CPU SIMD 扩展语种批推理原语（动态 32 对齐精确宽度，0 DirectML 显存增长，低配本友好）
            auto evalExtRecOnIndices = [&](WinMlContext::ExtRecSlot& slot,
                                           const std::vector<size_t>& targetIndices,
                                           int maxProbeWidthCap,
                                           std::vector<CtcDecodeOutput>& outVec) {
                outVec.resize(targetIndices.size());
                if (targetIndices.empty() || !slot.session) return;
                std::vector<size_t> order(targetIndices.size());
                std::iota(order.begin(), order.end(), 0u);
                std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                    return pieces[targetIndices[a]].contentW < pieces[targetIndices[b]].contentW;
                });

                constexpr size_t kExtBatch = 4;
                size_t pos = 0;
                while (pos < order.size()) {
                    const size_t rem = order.size() - pos;
                    const size_t actualB = (std::min)(rem, kExtBatch);
                    const int rawMaxW = pieces[targetIndices[order[pos + actualB - 1]]].contentW;
                    const int cappedMaxW = (maxProbeWidthCap > 0) ? (std::min)(rawMaxW, maxProbeWidthCap) : rawMaxW;
                    const int evalW = std::clamp(((cappedMaxW + 31) / 32) * 32, 32, DeepOcrConstants::kRecLongBucketWidth);
                    const size_t planeFloats = static_cast<size_t>(H) * static_cast<size_t>(evalW);
                    const size_t sampleFloats = 3u * planeFloats;
                    const size_t batchFloats = actualB * sampleFloats;

                    try {
                        auto recTensor = waml::TensorFloat::Create(
                            winrt::single_threaded_vector<int64_t>({static_cast<int64_t>(actualB), 3, H, evalW}));
                        {
                            WinMlTensorBufferLock inLock(recTensor);
                            if (inLock.data && inLock.floatCount >= batchFloats) {
                                std::memset(inLock.data, 0, batchFloats * sizeof(float));
                                float* basePtr = inLock.data;
                                cv::parallel_for_(cv::Range(0, static_cast<int>(actualB)), [&](const cv::Range& br) {
                                    constexpr float kScale = 2.0f / 255.0f;
                                    for (int b = br.start; b < br.end; ++b) {
                                        const auto& pc = pieces[targetIndices[order[pos + static_cast<size_t>(b)]]];
                                        float* sampleBase = basePtr + static_cast<size_t>(b) * sampleFloats;
                                        const int copyW = (std::min)(pc.contentW, evalW);
                                        float* c0 = sampleBase;
                                        float* c1 = sampleBase + planeFloats;
                                        float* c2 = sampleBase + planeFloats * 2;
                                        for (int y = 0; y < H; ++y) {
                                            const auto* row = pc.stripBgr.ptr<cv::Vec3b>(y);
                                            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(evalW);
                                            for (int x = 0; x < copyW; ++x) {
                                                c0[rowBase + x] = static_cast<float>(row[x][0]) * kScale - 1.0f;
                                                c1[rowBase + x] = static_cast<float>(row[x][1]) * kScale - 1.0f;
                                                c2[rowBase + x] = static_cast<float>(row[x][2]) * kScale - 1.0f;
                                            }
                                        }
                                    }
                                });
                            }
                        }

                        waml::LearningModelBinding binding(slot.session);
                        binding.Bind(slot.inName, recTensor);
                        auto evalRes = slot.session.Evaluate(binding, L"");
                        decodeWinMlRecEvalWithDict(
                            evalRes, static_cast<int>(actualB),
                            slot.outName, slot.dictKeys,
                            [&](int b) -> float {
                                const auto& pc = pieces[targetIndices[order[pos + static_cast<size_t>(b)]]];
                                return static_cast<float>((std::min)(pc.contentW, evalW)) / static_cast<float>(evalW);
                            },
                            [&](int b, CtcDecodeOutput&& dec) {
                                outVec[order[pos + static_cast<size_t>(b)]] = std::move(dec);
                            });
                    } catch (...) {
                    }
                    pos += actualB;
                }
            };

            // 分支 A：用户显式指定了某个多语种扩展包（且本地已安装）时，零浪费一步直通该扩展语种！
            bool usedExplicitExtPack = false;
            if (effectiveScriptMode != "auto" && effectiveScriptMode != "cjk") {
                if (auto* extSlot = m_winMl->ensureExtRecSlot(modelDir, effectiveScriptMode)) {
                    m_activeSessionCountAtomic.store(m_winMl->countActiveSessions(), std::memory_order_release);
                    std::vector<size_t> allIndices(pieces.size());
                    std::iota(allIndices.begin(), allIndices.end(), 0u);
                    evalExtRecOnIndices(*extSlot, allIndices, /*maxProbeWidthCap=*/0, pieceDecoded);
                    detectedScript = effectiveScriptMode;
                    usedExplicitExtPack = true;
                }
            }

            if (!usedExplicitExtPack && !m_winMl->usingDirectMLGpu) {
                std::vector<size_t> sortedIdx(pieces.size());
                std::iota(sortedIdx.begin(), sortedIdx.end(), 0u);
                std::sort(sortedIdx.begin(), sortedIdx.end(), [&](size_t a, size_t b) {
                    return pieces[a].contentW < pieces[b].contentW;
                });

                if (waml::LearningModelSession* recSess = m_winMl->ensureRecSession(0)) {
                    m_activeSessionCountAtomic.store(m_winMl->countActiveSessions(), std::memory_order_release);
                    constexpr size_t kCpuMaxBatch = 8;
                    size_t pos = 0;
                    while (pos < sortedIdx.size()) {
                        const size_t rem = sortedIdx.size() - pos;
                        const size_t actualB = (std::min)(rem, kCpuMaxBatch);
                        const int maxContentW = pieces[sortedIdx[pos + actualB - 1]].contentW;
                        const int evalW = std::clamp(((maxContentW + 31) / 32) * 32, 32, DeepOcrConstants::kRecLongBucketWidth);
                        const size_t planeFloats = static_cast<size_t>(H) * static_cast<size_t>(evalW);
                        const size_t sampleFloats = 3u * planeFloats;
                        const size_t batchFloats = actualB * sampleFloats;

                        try {
                            auto recTensor = waml::TensorFloat::Create(
                                winrt::single_threaded_vector<int64_t>({static_cast<int64_t>(actualB), 3, H, evalW}));
                            {
                                WinMlTensorBufferLock inLock(recTensor);
                                if (inLock.data && inLock.floatCount >= batchFloats) {
                                    std::memset(inLock.data, 0, batchFloats * sizeof(float));
                                    float* basePtr = inLock.data;
                                    cv::parallel_for_(cv::Range(0, static_cast<int>(actualB)), [&](const cv::Range& br) {
                                        constexpr float kScale = 2.0f / 255.0f;
                                        for (int b = br.start; b < br.end; ++b) {
                                            const auto& pc = pieces[sortedIdx[pos + static_cast<size_t>(b)]];
                                            float* sampleBase = basePtr + static_cast<size_t>(b) * sampleFloats;
                                            const int copyW = (std::min)(pc.contentW, evalW);
                                            float* c0 = sampleBase;
                                            float* c1 = sampleBase + planeFloats;
                                            float* c2 = sampleBase + planeFloats * 2;
                                            for (int y = 0; y < H; ++y) {
                                                const auto* row = pc.stripBgr.ptr<cv::Vec3b>(y);
                                                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(evalW);
                                                for (int x = 0; x < copyW; ++x) {
                                                    c0[rowBase + x] = static_cast<float>(row[x][0]) * kScale - 1.0f;
                                                    c1[rowBase + x] = static_cast<float>(row[x][1]) * kScale - 1.0f;
                                                    c2[rowBase + x] = static_cast<float>(row[x][2]) * kScale - 1.0f;
                                                }
                                            }
                                        }
                                    });
                                }
                            }

                            waml::LearningModelBinding binding(*recSess);
                            binding.Bind(m_winMl->recInName, recTensor);
                            auto evalRes = recSess->Evaluate(binding, L"");
                            decodeWinMlRecEval(
                                evalRes, static_cast<int>(actualB),
                                m_winMl->recOutName,
                                [&](int b) -> float {
                                    const size_t pi = sortedIdx[pos + static_cast<size_t>(b)];
                                    return static_cast<float>(pieces[pi].contentW) / static_cast<float>(evalW);
                                },
                                [&](int b, CtcDecodeOutput&& dec) {
                                    pieceDecoded[sortedIdx[pos + static_cast<size_t>(b)]] = std::move(dec);
                                });
                        } catch (...) {
                        }
                        pos += actualB;
                    }
                }
            } else if (!usedExplicitExtPack) {
            // DirectML GPU 4 档紧致甜点桶：[0]=16x160, [1]=16x320, [2]=4x640, [3]=2x960
            std::array<std::vector<size_t>, 4> tierPieces;
            for (size_t pi = 0; pi < pieces.size(); ++pi) {
                const int cw = pieces[pi].contentW;
                if (cw <= DeepOcrConstants::kRecTinyBucketWidth) {
                    tierPieces[0].push_back(pi);
                } else if (cw <= DeepOcrConstants::kRecShortBucketWidth) {
                    tierPieces[1].push_back(pi);
                } else if (cw <= DeepOcrConstants::kRecMediumBucketWidth) {
                    tierPieces[2].push_back(pi);
                } else {
                    tierPieces[3].push_back(pi);
                }
            }
            // 超短词较少（<=8 条）时并入 16x320 主力桶，免去额外初始化 16x160 会话节省 ~200MB 显存与编译耗时
            if (!tierPieces[0].empty() && tierPieces[0].size() <= 8) {
                tierPieces[1].insert(tierPieces[1].end(), tierPieces[0].begin(), tierPieces[0].end());
                tierPieces[0].clear();
            }

            std::array<long long, 4> tierEvalMs{0, 0, 0, 0};
            std::array<long long, 4> tierDecMs{0, 0, 0, 0};
            std::array<size_t, 4> tierCalls{0, 0, 0, 0};
            for (size_t tier = 0; tier < 4; ++tier) {
                const auto& indices = tierPieces[tier];
                if (indices.empty()) continue;

                const int netW = (tier == 0)
                    ? DeepOcrConstants::kRecTinyBucketWidth
                    : ((tier == 1)
                        ? DeepOcrConstants::kRecShortBucketWidth
                        : ((tier == 2) ? DeepOcrConstants::kRecMediumBucketWidth : DeepOcrConstants::kRecLongBucketWidth));

                size_t pos = 0;
                while (pos < indices.size()) {
                    const size_t rem = indices.size() - pos;
                    const size_t bucketIdx = tier; // [0]=16x160, [1]=16x320, [2]=4x640, [3]=2x960

                    waml::LearningModelSession* recSess = m_winMl->ensureRecSession(bucketIdx);
                    if (!recSess) break;
                    m_activeSessionCountAtomic.store(m_winMl->countActiveSessions(), std::memory_order_release);

                    const bool useStaticBucket = (m_winMl->usingDirectMLGpu && m_winMl->supportsBatchEval);
                    const int batchCap = useStaticBucket ? WinMlContext::kRecSpecs[bucketIdx].n : 1;
                    const int actualB = static_cast<int>((std::min)(rem, static_cast<size_t>(batchCap)));
                    const int evalW = useStaticBucket
                        ? netW
                        : std::clamp(((pieces[indices[pos]].contentW + 31) / 32) * 32, 32, DeepOcrConstants::kRecLongBucketWidth);
                    const size_t planeFloats = static_cast<size_t>(H) * static_cast<size_t>(evalW);
                    const size_t sampleFloats = static_cast<size_t>(3) * planeFloats;
                    const size_t batchFloats = static_cast<size_t>(batchCap) * sampleFloats;

                    try {
                        // 每次推理创建全新 TensorFloat 并原位直写，避免 DirectML GPU 驻留缓存锁死旧批次像素
                        auto recTensor = waml::TensorFloat::Create(
                            winrt::single_threaded_vector<int64_t>({batchCap, 3, H, evalW}));
                        {
                            WinMlTensorBufferLock inLock(recTensor);
                            if (inLock.data && inLock.floatCount >= batchFloats) {
                                std::memset(inLock.data, 0, batchFloats * sizeof(float));
                                float* basePtr = inLock.data;
                                cv::parallel_for_(cv::Range(0, actualB), [&](const cv::Range& br) {
                                    constexpr float kScale = 2.0f / 255.0f;
                                    for (int b = br.start; b < br.end; ++b) {
                                        const auto& pc = pieces[indices[pos + static_cast<size_t>(b)]];
                                        float* sampleBase = basePtr + static_cast<size_t>(b) * sampleFloats;
                                        const int copyW = (std::min)(pc.contentW, evalW);
                                        float* c0 = sampleBase;
                                        float* c1 = sampleBase + planeFloats;
                                        float* c2 = sampleBase + planeFloats * 2;
                                        for (int y = 0; y < H; ++y) {
                                            const auto* row = pc.stripBgr.ptr<cv::Vec3b>(y);
                                            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(evalW);
                                            for (int x = 0; x < copyW; ++x) {
                                                c0[rowBase + x] = static_cast<float>(row[x][0]) * kScale - 1.0f;
                                                c1[rowBase + x] = static_cast<float>(row[x][1]) * kScale - 1.0f;
                                                c2[rowBase + x] = static_cast<float>(row[x][2]) * kScale - 1.0f;
                                            }
                                        }
                                    }
                                });
                            }
                        }

                        waml::LearningModelBinding binding(*recSess);
                        binding.Bind(m_winMl->recInName, recTensor);
                        const auto tEv0 = std::chrono::steady_clock::now();
                        auto evalRes = recSess->Evaluate(binding, L"");
                        const auto tEv1 = std::chrono::steady_clock::now();
                        tierEvalMs[tier] += std::chrono::duration_cast<std::chrono::milliseconds>(tEv1 - tEv0).count();
                        ++tierCalls[tier];
                        const auto tDc0 = std::chrono::steady_clock::now();
                        decodeWinMlRecEval(
                            evalRes, actualB,
                            m_winMl->recOutName,
                            [&](int b) -> float {
                                const size_t pi = indices[pos + static_cast<size_t>(b)];
                                return static_cast<float>(pieces[pi].contentW) / static_cast<float>(evalW);
                            },
                            [&](int b, CtcDecodeOutput&& dec) {
                                pieceDecoded[indices[pos + static_cast<size_t>(b)]] = std::move(dec);
                            });
                        tierDecMs[tier] += std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - tDc0).count();
                    } catch (...) {
                    }

                    pos += static_cast<size_t>(actualB);
                }
            }
            bucketProfile =
                "160:" + std::to_string(tierPieces[0].size()) + "/" + std::to_string(tierCalls[0]) + "c/" + std::to_string(tierEvalMs[0]) + "e+" + std::to_string(tierDecMs[0]) + "d " +
                "320:" + std::to_string(tierPieces[1].size()) + "/" + std::to_string(tierCalls[1]) + "c/" + std::to_string(tierEvalMs[1]) + "e+" + std::to_string(tierDecMs[1]) + "d " +
                "640:" + std::to_string(tierPieces[2].size()) + "/" + std::to_string(tierCalls[2]) + "c/" + std::to_string(tierEvalMs[2]) + "e+" + std::to_string(tierDecMs[2]) + "d " +
                "960:" + std::to_string(tierPieces[3].size()) + "/" + std::to_string(tierCalls[3]) + "c/" + std::to_string(tierEvalMs[3]) + "e+" + std::to_string(tierDecMs[3]) + "d";
            LOG_DEBUG("DeepOcrEngine: rec buckets {}", bucketProfile);
            }

            // 1.5) 世界级跨语种零配置自动嗅探与早停路由（Auto Script Probe & Cascade）：
            //      - 仅在 scriptMode == "auto" 且用户至少安装了 1 个扩展语种包时启用；
            //      - 正常中/英文清晰截图 0.02ms 四重门控直接短路放行（0ms 额外开销、0 扩展模型加载）；
            //      - 命中异常信号时按特征排序候选 + Top-3 限宽(<=480px) CPU 轻探针 + 命中即早停 + 650ms 低配机预算硬护栏；
            //      - 胜出语种免重跑 det，直接全页重识并切换 detectedScript，实现「中文用户截韩/日/俄/西葡图零切换自动识译」。
            if (!usedExplicitExtPack && effectiveScriptMode == "auto" && !pieces.empty()) {
                const bool isV6Main = (m_loadedEngineGen == "v6");
                std::vector<std::string> installedExtPacks;
                installedExtPacks.reserve(6);
                for (const char* pid : allScriptPackIds()) {
                    if (isScriptPackModelPresent(modelDir, pid)) {
                        installedExtPacks.emplace_back(pid);
                    }
                }
                // 旧版 PP-OCRv4 兼容态保留 japan / latin 外挂包探针；PP-OCRv6 已原生高精度覆盖 50 语种（含日/法/德/西/葡），免无用探测
                if (!isV6Main) {
                    for (const char* legacyId : {"japan", "latin"}) {
                        if (isScriptPackModelPresent(modelDir, legacyId)) {
                            installedExtPacks.emplace_back(legacyId);
                        }
                    }
                }
                auto hasPack = [&](std::string_view id) -> bool {
                    return std::find(installedExtPacks.begin(), installedExtPacks.end(), id) != installedExtPacks.end();
                };

                if (!installedExtPacks.empty()) {
                    size_t validPieces = 0;
                    float confSum = 0.0f;
                    float expectedUnitsSum = 0.0f;
                    float actualUnitsSum = 0.0f;
                    size_t totalCjkHan = 0;
                    size_t totalAsciiAlpha = 0;
                    size_t totalJapaneseNo = 0;
                    size_t totalKana = 0;
                    size_t totalLatinDiac = 0;
                    size_t totalRareLatinExt = 0;
                    size_t totalAsciiWords = 0;
                    size_t totalMixedCaseAnom = 0;
                    bool iberianHint = false;

                    for (size_t pi = 0; pi < pieces.size(); ++pi) {
                        const float expU = (std::max)(1.0f, static_cast<float>((std::max)(1, pieces[pi].resizedW)) / 42.0f);
                        expectedUnitsSum += expU;
                        const auto& dec = pieceDecoded[pi];
                        if (dec.chars.empty() || dec.confidence < DeepOcrConstants::kRecMinLineConfidence) {
                            continue;
                        }
                        ++validPieces;
                        confSum += dec.confidence;
                        const auto gp = analyzeScriptGlyphs(dec.text);
                        actualUnitsSum += gp.visualUnits;
                        totalCjkHan += gp.cjkHan;
                        totalAsciiAlpha += gp.asciiAlpha;
                        totalJapaneseNo += gp.japaneseNo;
                        totalKana += gp.kana;
                        totalLatinDiac += gp.latinDiacritic;
                        totalRareLatinExt += gp.rareLatinExt;
                        totalAsciiWords += gp.asciiWordCount;
                        totalMixedCaseAnom += gp.mixedCaseAnomalousWords;
                        if (!iberianHint && hasIberianOrExtendedLatinHint(dec.text)) {
                            iberianHint = true;
                        }
                    }

                    const float survivalRatio = static_cast<float>(validPieces) / static_cast<float>(pieces.size());
                    const float mainAvgConf = (validPieces > 0) ? (confSum / static_cast<float>(validPieces)) : 0.0f;
                    const float densityRatio = (expectedUnitsSum > 0.0f) ? (actualUnitsSum / expectedUnitsSum) : 1.0f;
                    const float mixedCaseRatio = (totalAsciiWords >= 3)
                        ? (static_cast<float>(totalMixedCaseAnom) / static_cast<float>(totalAsciiWords))
                        : 0.0f;

                    // 四重正交异常信号门控（高汉字/假名占比且稳定的中英日文档直接短路放行，0 额外开销）
                    const bool sigJapaneseCue      = (!isV6Main) && ((totalKana >= 2) ||
                                                     (totalJapaneseNo >= 1 && (densityRatio < 0.65f || totalCjkHan < 20)));
                    const bool clearlyChineseDoc   = ((totalCjkHan + (isV6Main ? totalKana : 0u)) >= 15 &&
                                                      survivalRatio >= 0.72f &&
                                                      mainAvgConf >= 0.74f && densityRatio >= 0.45f &&
                                                      !sigJapaneseCue);
                    const bool sigSurvivalCollapse = (survivalRatio < (isV6Main ? 0.76f : 0.72f));
                    const bool sigDensityCollapse  = (expectedUnitsSum >= 6.0f && densityRatio < (isV6Main ? 0.62f : 0.58f));
                    const bool sigCyrillicGlitch   = ((mixedCaseRatio >= (isV6Main ? 0.16f : 0.22f) ||
                                                       (isV6Main && totalRareLatinExt >= 4 && mainAvgConf < 0.90f)) &&
                                                      totalCjkHan <= 4);
                    const bool sigLowConf          = (!clearlyChineseDoc && (validPieces == 0 || mainAvgConf < (isV6Main ? 0.87f : 0.85f)));
                    const bool sigLatinCue         = (!isV6Main && totalCjkHan <= 2 && totalAsciiAlpha >= 16 &&
                                                      (iberianHint || totalLatinDiac >= 1));

                    const bool shouldProbe = !clearlyChineseDoc && (
                        (sigSurvivalCollapse && (hasPack("korean") || hasPack("japan") || hasPack("cyrillic") || hasPack("arabic") || hasPack("thai"))) ||
                        ((sigDensityCollapse || sigJapaneseCue) && (hasPack("japan") || hasPack("korean") || hasPack("cyrillic") || hasPack("arabic") || hasPack("thai"))) ||
                        (sigCyrillicGlitch && hasPack("cyrillic")) ||
                        (sigLatinCue && hasPack("latin")) ||
                        (sigLowConf && !installedExtPacks.empty()));

                    if (shouldProbe) {
                        // 信号导向候选优先级排序（先试最可能 1 个 + 热槽复用，命中即早停，对低配本极其友好）
                        std::vector<std::string> probeOrder;
                        auto pushIfInstalled = [&](std::string_view id) {
                            if (hasPack(id) && std::find(probeOrder.begin(), probeOrder.end(), id) == probeOrder.end()) {
                                probeOrder.emplace_back(id);
                            }
                        };
                        if (sigCyrillicGlitch) pushIfInstalled("cyrillic");
                        if (sigJapaneseCue || (!isV6Main && sigDensityCollapse && totalCjkHan >= 2)) pushIfInstalled("japan");
                        if (sigSurvivalCollapse) {
                            pushIfInstalled("korean");
                            pushIfInstalled("cyrillic");
                            pushIfInstalled("arabic");
                            pushIfInstalled("thai");
                        }
                        if (sigLatinCue) pushIfInstalled("latin");
                        // 若有已热驻留的扩展槽，紧随强信号后优先复用以省 ~25ms 加载
                        if (m_winMl->extRec.session && !m_winMl->extRec.scriptId.empty()) {
                            pushIfInstalled(m_winMl->extRec.scriptId);
                        }
                        for (const auto& pid : installedExtPacks) {
                            pushIfInstalled(pid);
                        }
                        // 纯高置信拉丁西/葡线索只探 latin，绝不白探韩/日/俄
                        if (sigLatinCue && !sigSurvivalCollapse && !sigDensityCollapse && !sigCyrillicGlitch && !sigLowConf && !sigJapaneseCue) {
                            probeOrder.clear();
                            pushIfInstalled("latin");
                        }

                        // 挑选 Top-3 最具代表性的清晰宽行作为轻探针（低配机算力硬约束）
                        std::vector<size_t> probeIndices(pieces.size());
                        std::iota(probeIndices.begin(), probeIndices.end(), 0u);
                        std::sort(probeIndices.begin(), probeIndices.end(), [&](size_t a, size_t b) {
                            const int scoreA = (pieces[a].contentW <= 560) ? pieces[a].contentW : (560 - (pieces[a].contentW - 560) / 8);
                            const int scoreB = (pieces[b].contentW <= 560) ? pieces[b].contentW : (560 - (pieces[b].contentW - 560) / 8);
                            return scoreA > scoreB;
                        });
                        if (probeIndices.size() > 3u) probeIndices.resize(3u);

                        auto scoreSubset = [&](const std::vector<CtcDecodeOutput>& decs,
                                               std::string_view scriptId,
                                               float& outAvgConf,
                                               bool& outHasTargetScript,
                                               size_t& outLatinDiac) -> float {
                            size_t okLines = 0;
                            float cSum = 0.0f;
                            float expU = 0.0f;
                            float actU = 0.0f;
                            size_t targetHits = 0;
                            size_t words = 0;
                            size_t anom = 0;
                            outLatinDiac = 0;
                            for (size_t k = 0; k < probeIndices.size() && k < decs.size(); ++k) {
                                const auto& pc = pieces[probeIndices[k]];
                                const int effW = (std::min)(pc.resizedW, 480);
                                expU += (std::max)(1.0f, static_cast<float>((std::max)(1, effW)) / 42.0f);
                                const auto& d = decs[k];
                                if (d.chars.empty() || d.confidence < DeepOcrConstants::kRecMinLineConfidence) continue;
                                ++okLines;
                                cSum += d.confidence;
                                const auto gp = analyzeScriptGlyphs(d.text);
                                actU += gp.visualUnits;
                                words += gp.asciiWordCount;
                                anom += gp.mixedCaseAnomalousWords;
                                outLatinDiac += gp.latinDiacritic;
                                if (scriptId == "korean") targetHits += gp.hangul;
                                else if (scriptId == "japan") targetHits += gp.kana;
                                else if (scriptId == "cyrillic") targetHits += gp.cyrillic;
                                else if (scriptId == "arabic") targetHits += gp.arabic;
                                else if (scriptId == "thai") targetHits += gp.thai;
                                else if (scriptId == "latin") targetHits += gp.latinDiacritic;
                            }
                            outAvgConf = (okLines > 0) ? (cSum / static_cast<float>(okLines)) : 0.0f;
                            const float surv = probeIndices.empty() ? 0.0f : (static_cast<float>(okLines) / static_cast<float>(probeIndices.size()));
                            const float dens = (expU > 0.0f) ? (std::min)(1.0f, actU / expU) : 0.0f;
                            const float glitch = (words >= 2) ? (static_cast<float>(anom) / static_cast<float>(words)) : 0.0f;
                            if (scriptId == "korean") outHasTargetScript = (targetHits >= 2);
                            else if (scriptId == "japan") outHasTargetScript = (targetHits >= 2);
                            else if (scriptId == "cyrillic") outHasTargetScript = (targetHits >= 4);
                            else if (scriptId == "arabic") outHasTargetScript = (targetHits >= 3);
                            else if (scriptId == "thai") outHasTargetScript = (targetHits >= 3);
                            else if (scriptId == "latin") outHasTargetScript = (targetHits >= 2);
                            else outHasTargetScript = true;
                            return outAvgConf * (0.35f + 0.35f * surv + 0.30f * dens) - 0.25f * glitch;
                        };

                        std::vector<CtcDecodeOutput> mainProbeDecs;
                        mainProbeDecs.reserve(probeIndices.size());
                        for (size_t idx : probeIndices) mainProbeDecs.push_back(pieceDecoded[idx]);
                        float mainSubConf = 0.0f;
                        bool mainHasTarget = true;
                        size_t mainSubDiac = 0;
                        const float mainScore = scoreSubset(mainProbeDecs, "cjk", mainSubConf, mainHasTarget, mainSubDiac);

                        constexpr long long kAutoProbeWithWinnerBudgetMs = 650;
                        constexpr long long kAutoProbeNoWinnerBudgetMs = 2200;
                        const auto tProbeStart = std::chrono::steady_clock::now();
                        std::string winnerScript;
                        float bestScore = mainScore;

                        for (const auto& candId : probeOrder) {
                            const long long elapsedProbeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - tProbeStart).count();
                            if (!winnerScript.empty() && elapsedProbeMs >= kAutoProbeWithWinnerBudgetMs) {
                                break;
                            }
                            if (elapsedProbeMs >= kAutoProbeNoWinnerBudgetMs) {
                                break;
                            }
                            auto* extSlot = m_winMl->ensureExtRecSlot(modelDir, candId);
                            if (!extSlot) continue;
                            m_activeSessionCountAtomic.store(m_winMl->countActiveSessions(), std::memory_order_release);

                            std::vector<CtcDecodeOutput> candDecs;
                            const int probeWidthCap = (candId == "latin") ? 640 : 480;
                            evalExtRecOnIndices(*extSlot, probeIndices, /*maxProbeWidthCap=*/probeWidthCap, candDecs);

                            float candConf = 0.0f;
                            bool candHasTarget = false;
                            size_t candDiac = 0;
                            const float rawCandScore = scoreSubset(candDecs, candId, candConf, candHasTarget, candDiac);
                            if (!candHasTarget || candConf < 0.78f) continue;

                            // 拉丁扩展包专项加权：基础 ch_PP-OCRv4 词表不含西/葡/法/德变音符（ñ/ç/ã/¿ 等），
                            // 即使主包输出高置信去音 ASCII，只要 latin 扩展包以高置信真实还原 >=2 个变音符即可胜出。
                            float effectiveCandScore = rawCandScore;
                            bool beatMain = false;
                            if (candId == "latin") {
                                const size_t diacGain = (candDiac > mainSubDiac) ? (candDiac - mainSubDiac) : 0;
                                if (diacGain >= 2 && candConf >= 0.90f &&
                                    candConf >= mainSubConf - 0.035f &&
                                    rawCandScore >= mainScore - 0.035f) {
                                    effectiveCandScore = (std::max)(rawCandScore, mainScore) +
                                                         0.025f * static_cast<float>((std::min<size_t>)(diacGain, 6u));
                                    beatMain = true;
                                }
                            } else {
                                beatMain = (rawCandScore >= mainScore + 0.06f);
                            }

                            if (beatMain && effectiveCandScore > bestScore) {
                                bestScore = effectiveCandScore;
                                winnerScript = candId;
                                // 强早停门禁：单次命中压倒性高置信即刻锁定胜者，绝不浪费低配机 CPU 试跑其余包
                                if (candConf >= 0.90f && (effectiveCandScore >= mainScore + 0.12f || candId == "latin")) {
                                    break;
                                }
                            }
                        }

                        const long long probeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - tProbeStart).count();
                        if (!winnerScript.empty()) {
                            if (auto* winSlot = m_winMl->ensureExtRecSlot(modelDir, winnerScript)) {
                                notifyStage(OcrRecognitionStage::AutoScriptSwitching,
                                            static_cast<uint32_t>(pieces.size()),
                                            winnerScript);
                                m_activeSessionCountAtomic.store(m_winMl->countActiveSessions(), std::memory_order_release);
                                std::vector<size_t> allIndices(pieces.size());
                                std::iota(allIndices.begin(), allIndices.end(), 0u);
                                evalExtRecOnIndices(*winSlot, allIndices, /*maxProbeWidthCap=*/0, pieceDecoded);
                                detectedScript = winnerScript;
                                autoScriptSwitched = true;
                                LOG_INFO("DeepOcrEngine: auto script router switched to '{}' (mainScore={:.3f} -> extScore={:.3f}, probeMs={} ms, lines={})",
                                         winnerScript, mainScore, bestScore, probeMs, pieces.size());
                            }
                        } else {
                            LOG_DEBUG("DeepOcrEngine: auto script probe kept primary model (mainScore={:.3f}, survival={:.2f}, density={:.2f}, avgConf={:.3f}, tried={}, probeMs={} ms)",
                                      mainScore, survivalRatio, densityRatio, mainAvgConf, probeOrder.size(), probeMs);
                        }
                    }
                }
            }

            if (detectedScript == "cyrillic") {
                for (auto& dec : pieceDecoded) {
                    normalizeCyrillicLatinHomoglyphs(dec);
                }
            } else if (detectedScript == "arabic") {
                for (auto& dec : pieceDecoded) {
                    normalizeArabicRtlLine(dec);
                }
            }

            // 2) 二阶高精度 Server 精准复核（仅当用户开启 server 增强档位、处于中英主语种且存在低置信度疑难行时触发）：
            //    GPU 优先使用唯一 1 个轻量 2x640 甜点固定桶；无 GPU 优雅回退单 CPU 会话；
            //    严格执行单向择优门禁，且同样受 120s 闲置自动卸载守护线程 100% 回收归零
            if (m_winMl->serverCascadeEnabled && detectedScript == "cjk") {
                const auto tRefineStart = std::chrono::steady_clock::now();
                struct HardPieceCandidate {
                    size_t pieceIdx = 0;
                    float priorityScore = 1.0f; // 越低越疑难，优先复核
                    float mobileMinCharConf = 1.0f;
                };
                std::vector<HardPieceCandidate> hardCandidates;
                hardCandidates.reserve(pieces.size());
                size_t trigEmpty = 0, trigLowAvg = 0, trigLowChar = 0, trigShort = 0;
                for (size_t pi = 0; pi < pieces.size(); ++pi) {
                    const auto& dec = pieceDecoded[pi];
                    if (dec.chars.empty()) {
                        if (pieces[pi].contentW >= 18) {
                            hardCandidates.push_back({pi, 0.0f, 0.0f});
                            ++trigEmpty;
                        }
                        continue;
                    }
                    float minCharConf = 1.0f;
                    for (const auto& ch : dec.chars) {
                        minCharConf = (std::min)(minCharConf, ch.confidence);
                    }
                    const bool lowAvg = (dec.confidence < DeepOcrConstants::kServerRefineAvgConfThresh);
                    const bool lowChar = (minCharConf < DeepOcrConstants::kServerRefineMinCharConfThresh) &&
                                         (dec.confidence < DeepOcrConstants::kServerRefineHighAvgSkipLowCharThresh ||
                                          minCharConf < DeepOcrConstants::kServerRefineExtremeMinCharConfThresh);
                    const bool shortUncertain = (dec.chars.size() <= 2 && dec.confidence < DeepOcrConstants::kServerRefineShortLineConfThresh);
                    // 通用几何无损保真门控：GPU 精修桶固定宽度为 640px，避免对已高置信（非 lowAvg）的
                    // 超宽条带执行下采样压扁后反向覆盖主识别的高分辨率无损结果。
                    const bool gpuWideDownscaleRisk =
                        m_winMl->usingDirectMLGpu &&
                        m_winMl->allowServerGpu &&
                        (pieces[pi].contentW > DeepOcrConstants::kServerRefineGpuBucketWidth) &&
                        !lowAvg;
                    if (!gpuWideDownscaleRisk && (lowAvg || lowChar || shortUncertain)) {
                        hardCandidates.push_back({pi, (std::min)(dec.confidence, minCharConf), minCharConf});
                        if (lowAvg) ++trigLowAvg;
                        if (lowChar) ++trigLowChar;
                        if (shortUncertain) ++trigShort;
                    }
                }

                if (!hardCandidates.empty()) {
                    const size_t totalHardFound = hardCandidates.size();
                    const size_t maxRefinePieces = (m_winMl->usingDirectMLGpu && m_winMl->supportsBatchEval)
                        ? DeepOcrConstants::kServerRefineMaxPiecesPerImage
                        : DeepOcrConstants::kServerRefineMaxPiecesCpuFallback;
                    if (hardCandidates.size() > maxRefinePieces) {
                        std::partial_sort(
                            hardCandidates.begin(),
                            hardCandidates.begin() + static_cast<ptrdiff_t>(maxRefinePieces),
                            hardCandidates.end(),
                            [](const HardPieceCandidate& a, const HardPieceCandidate& b) {
                                return a.priorityScore < b.priorityScore;
                            });
                        hardCandidates.resize(maxRefinePieces);
                    }
                    serverRefineCandidateCount = hardCandidates.size();
                    LOG_INFO("DeepOcrEngine: low-confidence refinement triggered ({}/{} candidates selected from {} strips: lowAvg={}, lowChar={}, short={}, empty={})",
                             hardCandidates.size(), totalHardFound, pieces.size(),
                             trigLowAvg, trigLowChar, trigShort, trigEmpty);
                    notifyStage(OcrRecognitionStage::LowConfidenceRefining,
                                static_cast<uint32_t>(hardCandidates.size()),
                                m_loadedEngineGen == "v6" ? "v6_medium" : "v4_server");

                    float acceptedConfBeforeSum = 0.0f;
                    float acceptedConfAfterSum = 0.0f;
                    auto maybeAcceptServerDecode = [&](const HardPieceCandidate& cand, CtcDecodeOutput&& srvDec) {
                        if (srvDec.chars.empty()) return;
                        float srvMinChar = 1.0f;
                        for (const auto& ch : srvDec.chars) {
                            srvMinChar = (std::min)(srvMinChar, ch.confidence);
                        }
                        const auto& mobDec = pieceDecoded[cand.pieceIdx];
                        // 通用字符完整性护栏：主识别已达高置信且精修丢失字符时，拒绝仅因平均分微增覆盖原行
                        const bool highConfTruncationLoss =
                            (mobDec.confidence >= 0.90f) &&
                            (srvDec.chars.size() < mobDec.chars.size()) &&
                            (srvDec.confidence < mobDec.confidence + 0.04f);
                        const bool acceptServer = !highConfTruncationLoss && (
                            (mobDec.chars.empty() && srvDec.confidence >= DeepOcrConstants::kRecMinLineConfidence) ||
                            (srvDec.confidence > mobDec.confidence + 0.015f) ||
                            (srvMinChar > cand.mobileMinCharConf + 0.08f && srvDec.confidence >= mobDec.confidence - 0.01f));
                        if (acceptServer) {
                            acceptedConfBeforeSum += mobDec.confidence;
                            acceptedConfAfterSum += srvDec.confidence;
                            pieceDecoded[cand.pieceIdx] = std::move(srvDec);
                            ++serverRefinedCount;
                        }
                    };

                    if (waml::LearningModelSession* srvSess = m_winMl->ensureServerRecSession()) {
                        m_activeSessionCountAtomic.store(m_winMl->countActiveSessions(), std::memory_order_release);
                        if (m_winMl->serverSessionOnGpu) {
                            constexpr int kSrvBatch = DeepOcrConstants::kServerRefineGpuBatchSize;
                            constexpr int kSrvW = DeepOcrConstants::kServerRefineGpuBucketWidth;
                            const size_t planeFloats = static_cast<size_t>(H) * static_cast<size_t>(kSrvW);
                            const size_t sampleFloats = 3u * planeFloats;
                            const size_t batchFloats = static_cast<size_t>(kSrvBatch) * sampleFloats;

                            size_t cpos = 0;
                            while (cpos < hardCandidates.size()) {
                                const size_t rem = hardCandidates.size() - cpos;
                                const int actualB = static_cast<int>((std::min<size_t>)(rem, static_cast<size_t>(kSrvBatch)));
                                std::array<float, kSrvBatch> batchValidRatios{};
                                try {
                                    auto srvGpuTensor = waml::TensorFloat::Create(
                                        winrt::single_threaded_vector<int64_t>({kSrvBatch, 3, H, kSrvW}));
                                    {
                                        WinMlTensorBufferLock inLock(srvGpuTensor);
                                        if (inLock.data && inLock.floatCount >= batchFloats) {
                                            std::memset(inLock.data, 0, batchFloats * sizeof(float));
                                            constexpr float kScale = 2.0f / 255.0f;
                                            for (int b = 0; b < actualB; ++b) {
                                                const auto& pc = pieces[hardCandidates[cpos + static_cast<size_t>(b)].pieceIdx];
                                                cv::Mat srcStrip;
                                                int copyW = pc.contentW;
                                                if (pc.contentW <= kSrvW) {
                                                    srcStrip = pc.stripBgr;
                                                    batchValidRatios[static_cast<size_t>(b)] =
                                                        static_cast<float>(pc.contentW) / static_cast<float>(kSrvW);
                                                } else {
                                                    cv::resize(pc.stripBgr, srcStrip, cv::Size(kSrvW, H), 0.0, 0.0, cv::INTER_AREA);
                                                    copyW = kSrvW;
                                                    batchValidRatios[static_cast<size_t>(b)] = 1.0f;
                                                }
                                                float* sampleBase = inLock.data + static_cast<size_t>(b) * sampleFloats;
                                                float* c0 = sampleBase;
                                                float* c1 = sampleBase + planeFloats;
                                                float* c2 = sampleBase + planeFloats * 2;
                                                for (int y = 0; y < H; ++y) {
                                                    const auto* row = srcStrip.ptr<cv::Vec3b>(y);
                                                    const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kSrvW);
                                                    for (int x = 0; x < copyW; ++x) {
                                                        c0[rowBase + x] = static_cast<float>(row[x][0]) * kScale - 1.0f;
                                                        c1[rowBase + x] = static_cast<float>(row[x][1]) * kScale - 1.0f;
                                                        c2[rowBase + x] = static_cast<float>(row[x][2]) * kScale - 1.0f;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    waml::LearningModelBinding srvBind(*srvSess);
                                    srvBind.Bind(m_winMl->serverRecInName, srvGpuTensor);
                                    auto srvEval = srvSess->Evaluate(srvBind, L"");
                                    decodeWinMlRecEval(
                                        srvEval, actualB,
                                        m_winMl->serverRecOutName,
                                        [&](int b) -> float { return batchValidRatios[static_cast<size_t>(b)]; },
                                        [&](int b, CtcDecodeOutput&& srvDec) {
                                            maybeAcceptServerDecode(hardCandidates[cpos + static_cast<size_t>(b)], std::move(srvDec));
                                        });
                                } catch (const winrt::hresult_error& e) {
                                    LOG_WARN("DeepOcrEngine: server GPU refine WinRT error hr=0x{:08X}, disabling GPU server bucket",
                                             static_cast<uint32_t>(e.code().value));
                                    m_winMl->allowServerGpu = false;
                                    m_winMl->serverSessionOnGpu = false;
                                    if (m_winMl->serverRecSession) {
                                        try { m_winMl->serverRecSession.Close(); } catch (...) {}
                                        m_winMl->serverRecSession = nullptr;
                                    }
                                    break;
                                } catch (...) {
                                    m_winMl->allowServerGpu = false;
                                    m_winMl->serverSessionOnGpu = false;
                                    if (m_winMl->serverRecSession) {
                                        try { m_winMl->serverRecSession.Close(); } catch (...) {}
                                        m_winMl->serverRecSession = nullptr;
                                    }
                                    break;
                                }
                                cpos += static_cast<size_t>(actualB);
                            }
                        } else {
                            for (const auto& cand : hardCandidates) {
                                const auto& pc = pieces[cand.pieceIdx];
                                const int dynW = std::clamp(
                                    ((pc.contentW + 31) / 32) * 32,
                                    32,
                                    DeepOcrConstants::kRecLongBucketWidth);
                                const size_t planeFloats = static_cast<size_t>(H) * static_cast<size_t>(dynW);
                                const size_t totalFloats = 3u * planeFloats;
                                try {
                                    auto srvTensor = waml::TensorFloat::Create(
                                        winrt::single_threaded_vector<int64_t>({1, 3, H, dynW}));
                                    {
                                        WinMlTensorBufferLock inLock(srvTensor);
                                        if (inLock.data && inLock.floatCount >= totalFloats) {
                                            std::memset(inLock.data, 0, totalFloats * sizeof(float));
                                            constexpr float kScale = 2.0f / 255.0f;
                                            const int copyW = (std::min)(pc.contentW, dynW);
                                            float* c0 = inLock.data;
                                            float* c1 = inLock.data + planeFloats;
                                            float* c2 = inLock.data + planeFloats * 2;
                                            for (int y = 0; y < H; ++y) {
                                                const auto* row = pc.stripBgr.ptr<cv::Vec3b>(y);
                                                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(dynW);
                                                for (int x = 0; x < copyW; ++x) {
                                                    c0[rowBase + x] = static_cast<float>(row[x][0]) * kScale - 1.0f;
                                                    c1[rowBase + x] = static_cast<float>(row[x][1]) * kScale - 1.0f;
                                                    c2[rowBase + x] = static_cast<float>(row[x][2]) * kScale - 1.0f;
                                                }
                                            }
                                        }
                                    }
                                    waml::LearningModelBinding srvBind(*srvSess);
                                    srvBind.Bind(m_winMl->serverRecInName, srvTensor);
                                    auto srvEval = srvSess->Evaluate(srvBind, L"");
                                    decodeWinMlRecEval(
                                        srvEval, 1,
                                        m_winMl->serverRecOutName,
                                        [&](int) -> float { return static_cast<float>(pc.contentW) / static_cast<float>(dynW); },
                                        [&](int, CtcDecodeOutput&& srvDec) {
                                            maybeAcceptServerDecode(cand, std::move(srvDec));
                                        });
                                } catch (...) {
                                }
                            }
                        }
                    }
                    const long long curRefineMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - tRefineStart).count();
                    const float avgBefore = (serverRefinedCount > 0)
                        ? (acceptedConfBeforeSum / static_cast<float>(serverRefinedCount)) : 0.0f;
                    const float avgAfter = (serverRefinedCount > 0)
                        ? (acceptedConfAfterSum / static_cast<float>(serverRefinedCount)) : 0.0f;
                    LOG_INFO("DeepOcrEngine: low-confidence refinement finished (checked={}, accepted={}, avgConf={:.3f}->{:.3f}, backend={}, elapsed={} ms)",
                             hardCandidates.size(), serverRefinedCount, avgBefore, avgAfter,
                             m_winMl->serverSessionOnGpu ? "DirectML-GPU" : "CPU", curRefineMs);
                }
                refineMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - tRefineStart).count();
            }

            for (size_t pi = 0; pi < pieces.size(); ++pi) {
                mapIsolatedDecodedToQuad(pieces[pi], pieceDecoded[pi]);
            }
        } else {
            // OpenCV DNN CPU 回退分支（Win10 / Server 2016-2022 完整支持主包 + 显式扩展语种包）
            if (!pieces.empty()) {
                notifyStage(OcrRecognitionStage::DeepCpuInference,
                            static_cast<uint32_t>(pieces.size()),
                            "opencv_dnn");
            }
            cv::dnn::Net extNetShort;
            cv::dnn::Net extNetLong;
            std::vector<std::string> extDictKeys;
            bool useCvExtPack = false;
            if (effectiveScriptMode != "auto" && effectiveScriptMode != "cjk") {
                const auto packPath = deep_ocr_detail::resolveScriptPackModelPath(modelDir, effectiveScriptMode);
                if (!packPath.empty() && deep_ocr_detail::isScriptPackModelPresent(modelDir, effectiveScriptMode)) {
                    std::vector<uint8_t> extBytes;
                    if (readFileBytes(packPath, extBytes) &&
                        extractEmbeddedCharacterDictFromOnnx(extBytes.data(), extBytes.size(), extDictKeys) &&
                        extDictKeys.size() >= 64) {
                        std::vector<uint8_t> extShortFixed, extLongFixed;
                        const bool spOk = patchOnnxGraphInputShape(
                            extBytes.data(), extBytes.size(),
                            {1, 3, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecShortBucketWidth},
                            extShortFixed);
                        const bool lpOk = patchOnnxGraphInputShape(
                            extBytes.data(), extBytes.size(),
                            {1, 3, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecLongBucketWidth},
                            extLongFixed);
                        try {
                            const auto& sSrc = spOk ? extShortFixed : extBytes;
                            const auto& lSrc = lpOk ? extLongFixed : extBytes;
                            extNetShort = cv::dnn::readNetFromONNX(reinterpret_cast<const char*>(sSrc.data()), sSrc.size());
                            extNetLong = cv::dnn::readNetFromONNX(reinterpret_cast<const char*>(lSrc.data()), lSrc.size());
                            if (!extNetShort.empty() && !extNetLong.empty()) {
                                extNetShort.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                                extNetShort.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                                extNetLong.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
                                extNetLong.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
                                useCvExtPack = true;
                                detectedScript = effectiveScriptMode;
                            }
                        } catch (...) {
                            useCvExtPack = false;
                        }
                    }
                }
            }
            const auto& activeDict = useCvExtPack ? extDictKeys : m_dictKeys;
            for (const auto& pc : pieces) {
                const bool useShort = (pc.contentW <= DeepOcrConstants::kRecShortBucketWidth);
                const int netW = useShort
                    ? DeepOcrConstants::kRecShortBucketWidth
                    : DeepOcrConstants::kRecLongBucketWidth;
                cv::dnn::Net& activeNet = useCvExtPack
                    ? (useShort ? extNetShort : extNetLong)
                    : (useShort ? m_recNetShort : m_recNetLong);
                const int clampedContentW = (std::min)(pc.contentW, netW);
                cv::Mat recBlob = makeRecOrClsBlob(pc.stripBgr, H, clampedContentW, netW);
                activeNet.setInput(recBlob);
                cv::Mat recOut = activeNet.forward();
                if (recOut.empty()) continue;
                int seqT = (recOut.dims >= 3) ? recOut.size[1] : recOut.rows;
                int numClasses = (recOut.dims >= 3) ? recOut.size[2] : recOut.cols;
                if (seqT <= 0 || numClasses <= 1) continue;
                cv::Mat logits2d(seqT, numClasses, CV_32F, recOut.ptr<float>());
                const float validRatio = static_cast<float>(clampedContentW) / static_cast<float>(netW);
                CtcDecodeOutput dec = decodeCtcGreedy(logits2d, activeDict, validRatio);
                if (detectedScript == "cyrillic") {
                    normalizeCyrillicLatinHomoglyphs(dec);
                } else if (detectedScript == "arabic") {
                    normalizeArabicRtlLine(dec);
                }
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
        } catch (const winrt::hresult_error& e) {
            const uint32_t hr = static_cast<uint32_t>(e.code().value);
            if (m_useWinMl && m_winMl && m_winMl->usingDirectMLGpu && inferAttempt == 0) {
                LOG_WARN("DeepOcrEngine: WinML GPU inference threw hr=0x{:08X}, latching CPU-only fallback and retrying once", hr);
                gpuErrorNeedsCpuRetry = true;
            } else {
                LOG_ERROR("DeepOcrEngine: WinRT inference exception: hr=0x{:08X}", hr);
                m_lastExtractFailed.store(true, std::memory_order_release);
                return {};
            }
        } catch (const std::exception& e) {
            if (m_useWinMl && m_winMl && m_winMl->usingDirectMLGpu && inferAttempt == 0) {
                LOG_WARN("DeepOcrEngine: WinML GPU inference failed ({}), latching CPU-only fallback and retrying once", e.what());
                gpuErrorNeedsCpuRetry = true;
            } else {
                LOG_ERROR("DeepOcrEngine: inference exception: {}", e.what());
                m_lastExtractFailed.store(true, std::memory_order_release);
                return {};
            }
        } catch (...) {
            if (m_useWinMl && m_winMl && m_winMl->usingDirectMLGpu && inferAttempt == 0) {
                LOG_WARN("DeepOcrEngine: WinML GPU inference unknown exception, latching CPU-only fallback and retrying once");
                gpuErrorNeedsCpuRetry = true;
            } else {
                LOG_ERROR("DeepOcrEngine: inference unknown exception");
                m_lastExtractFailed.store(true, std::memory_order_release);
                return {};
            }
        }

        if (gpuErrorNeedsCpuRetry) {
            m_gpuRuntimeBlacklisted.store(true, std::memory_order_release);
            m_modelsLoaded = false;
            m_modelsLoadedAtomic.store(false, std::memory_order_release);
            m_bucketsPrecompiled = false;
            m_useWinMl = false;
            m_winMl.reset();
            m_activeSessionCountAtomic.store(0, std::memory_order_release);
            continue;
        }
        break;
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
    const size_t sessCnt = activeSessionCount();
    {
        std::lock_guard<std::mutex> sl(m_statsMutex);
        DeepOcrRunStats runStats{};
        runStats.totalMs = totalMs;
        runStats.loadMs = loadMs;
        runStats.detMs = detMs;
        runStats.packMs = packMs;
        runStats.recMs = recMs - refineMs;
        runStats.refineMs = refineMs;
        runStats.detectedQuads = detectedQuadCount;
        runStats.stripCount = packedStripCount;
        runStats.serverRefineCandidateCount = serverRefineCandidateCount;
        runStats.serverRefinedCount = serverRefinedCount;
        runStats.activeSessions = sessCnt;
        runStats.autoScriptSwitched = autoScriptSwitched;
        runStats.bucketProfile = bucketProfile;
        runStats.detectedScript = detectedScript;
        m_lastRunStats = std::move(runStats);
    }
    m_runSequence.fetch_add(1, std::memory_order_release);
    LOG_INFO("DeepOcrEngine: recognized {} lines ({} quads -> {} strips, script={}, autoSwitched={}, refineCandidates={}, serverRefined={}) on {}x{} in {} ms (load={} ms, det={} ms, pack={} ms, rec={} ms, refine={} ms, sessions={})",
             assembled.size(), detectedQuadCount, packedStripCount, detectedScript,
             autoScriptSwitched ? "true" : "false", serverRefineCandidateCount, serverRefinedCount,
             bgr.cols, bgr.rows, totalMs, loadMs, detMs, packMs, recMs - refineMs, refineMs, sessCnt);
    return assembled;
}

void DeepOcrEngine::setStageCallback(OcrStageCallback cb) {
    std::lock_guard<std::mutex> lock(m_stageCbMutex);
    m_stageCallback = std::move(cb);
}

OcrStageCallback DeepOcrEngine::currentStageCallback() const {
    std::lock_guard<std::mutex> lock(m_stageCbMutex);
    return m_stageCallback;
}

void DeepOcrEngine::notifyStage(OcrRecognitionStage stage, uint32_t count, const std::string& detail) const {
    OcrStageCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_stageCbMutex);
        cb = m_stageCallback;
    }
    if (cb) {
        try {
            cb(stage, count, detail);
        } catch (...) {
        }
    }
}

void formatOcrStageFeedback(
    OcrRecognitionStage stage,
    uint32_t count,
    const std::string& detail,
    bool isZh,
    std::wstring& outMessage,
    std::wstring& outBadge) {
    outMessage.clear();
    outBadge.clear();

    auto scriptLabel = [&](const std::string& script) -> std::wstring {
        if (script == "korean")   return isZh ? L"韩语" : L"Korean";
        if (script == "cyrillic") return isZh ? L"俄语/西里尔文" : L"Cyrillic";
        if (script == "arabic")   return isZh ? L"阿拉伯语" : L"Arabic";
        if (script == "thai")     return isZh ? L"泰语" : L"Thai";
        if (script == "japan")    return isZh ? L"日语" : L"Japanese";
        if (script == "latin")    return isZh ? L"拉丁扩展" : L"Latin";
        return isZh ? L"扩展语种" : L"Extended Script";
    };

    switch (stage) {
        case OcrRecognitionStage::DeepModelLoading:
            outMessage = isZh
                ? L"正在启动离线深度识别引擎，请稍候..."
                : L"Loading offline deep OCR engine...";
            outBadge = isZh ? L"离线引擎启动中" : L"Loading Engine";
            break;
        case OcrRecognitionStage::DeepCpuInference:
            if (count > 0) {
                outMessage = isZh
                    ? (L"深度离线识别中（CPU 兼容模式 · " + std::to_wstring(count) + L" 行文字）...")
                    : (L"Deep offline OCR in progress (CPU mode · " + std::to_wstring(count) + L" lines)...");
            } else {
                outMessage = isZh
                    ? L"已进入深度离线识别（CPU 兼容模式），正在分析画面..."
                    : L"Running deep offline OCR (CPU compatible mode)...";
            }
            outBadge = isZh ? L"CPU 深度模式" : L"CPU Deep Mode";
            break;
        case OcrRecognitionStage::DeepMultiLineRecognizing:
            if (count > 0) {
                outMessage = isZh
                    ? (L"高精度深度模型正在识别 " + std::to_wstring(count) + L" 行文字...")
                    : (L"High-precision deep model recognizing " + std::to_wstring(count) + L" lines...");
            } else {
                outMessage = isZh
                    ? L"已进入高精度深度识别模型，正在精细解析文字..."
                    : L"Running high-precision deep OCR model...";
            }
            outBadge = (detail == "server")
                ? (isZh ? L"高精度深度模式" : L"High-Precision Mode")
                : (isZh ? L"深度识别中" : L"Deep OCR");
            break;
        case OcrRecognitionStage::LowConfidenceRefining:
            outMessage = isZh
                ? (L"检测到 " + std::to_wstring(count) + L" 处疑难文字，正在高精度精修...")
                : (L"Refining " + std::to_wstring(count) + L" challenging text lines...");
            outBadge = isZh ? L"高精度精修中" : L"Refining";
            break;
        case OcrRecognitionStage::AutoScriptSwitching: {
            const std::wstring langW = scriptLabel(detail);
            outMessage = isZh
                ? (L"检测到 " + langW + L"，正在使用扩展语种模型复核...")
                : (L"Detected " + langW + L", re-recognizing with script pack...");
            outBadge = langW;
            break;
        }
        default:
            break;
    }
}

bool DeepOcrEngine::isWarmAndLoaded() const noexcept {
    return m_modelsLoadedAtomic.load(std::memory_order_acquire);
}

size_t DeepOcrEngine::activeSessionCount() const noexcept {
    return m_activeSessionCountAtomic.load(std::memory_order_acquire);
}

DeepOcrRunStats DeepOcrEngine::lastRunStats() const noexcept {
    std::lock_guard<std::mutex> sl(m_statsMutex);
    return m_lastRunStats;
}

void DeepOcrEngine::setForceCpuOnlyModeForWorker(bool forceCpu) noexcept {
    m_forceCpuOnlyForWorker.store(forceCpu, std::memory_order_release);
}

bool DeepOcrEngine::isForceCpuOnlyMode() const noexcept {
    if (m_forceCpuOnlyForWorker.load(std::memory_order_acquire) ||
        m_gpuRuntimeBlacklisted.load(std::memory_order_acquire)) {
        return true;
    }
    return worker::envFlagEnabled(L"TOOLS3000_OCR_FORCE_CPU");
}

bool DeepOcrEngine::isForceOpenCvDnnEnabled() const noexcept {
    if (m_forceOpenCvDnnForTest.load(std::memory_order_acquire)) {
        return true;
    }
    return worker::envFlagEnabled(L"TOOLS3000_OCR_FORCE_OPENCV_DNN");
}

bool DeepOcrEngine::isGpuRuntimeBlacklisted() const noexcept {
    return m_gpuRuntimeBlacklisted.load(std::memory_order_acquire);
}

void DeepOcrEngine::resetCpuSafeModeForTest() noexcept {
    m_forceCpuOnlyForWorker.store(false, std::memory_order_release);
    m_gpuRuntimeBlacklisted.store(false, std::memory_order_release);
}

void DeepOcrEngine::setIdleUnloadTimeoutForTest(int seconds) noexcept {
    m_idleUnloadTimeoutSec.store(seconds, std::memory_order_release);
    m_idleCv.notify_all();
}

void DeepOcrEngine::touchActivityAndScheduleIdleUnload() {
    if (m_shuttingDown.load(std::memory_order_acquire)) return;
    if (m_idleUnloadTimeoutSec.load(std::memory_order_acquire) <= 0) return;
    std::lock_guard<std::mutex> lock(m_idleMutex);
    m_lastActivityTime = std::chrono::steady_clock::now();
    if (!m_idleUnloadThread.joinable()) {
        m_idleUnloadThread = std::jthread([this](std::stop_token st) {
            idleUnloadWorkerLoop(st);
        });
    }
    m_idleCv.notify_all();
}

void DeepOcrEngine::idleUnloadWorkerLoop(std::stop_token stopToken) {
    tools3000::core::TraceId::Scope traceScope;
    while (!stopToken.stop_requested() && !m_shuttingDown.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(m_idleMutex);
        const int timeoutSec = m_idleUnloadTimeoutSec.load(std::memory_order_acquire);
        if (timeoutSec <= 0 || !m_modelsLoadedAtomic.load(std::memory_order_acquire)) {
            m_idleCv.wait(lock, [&]() {
                return stopToken.stop_requested() ||
                       m_shuttingDown.load(std::memory_order_acquire) ||
                       (m_idleUnloadTimeoutSec.load(std::memory_order_acquire) > 0 &&
                        m_modelsLoadedAtomic.load(std::memory_order_acquire));
            });
            continue;
        }

        const auto deadline = m_lastActivityTime + std::chrono::seconds(timeoutSec);
        if (std::chrono::steady_clock::now() < deadline) {
            m_idleCv.wait_until(lock, deadline, [&]() {
                return stopToken.stop_requested() ||
                       m_shuttingDown.load(std::memory_order_acquire) ||
                       !m_modelsLoadedAtomic.load(std::memory_order_acquire);
            });
            continue;
        }
        lock.unlock();

        // 非阻塞尝试加锁释放，绝不与正在进行的 OCR 推理争锁
        bool released = false;
        {
            std::unique_lock<std::mutex> inferLock(m_inferMutex, std::try_to_lock);
            if (!inferLock.owns_lock()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            std::lock_guard<std::mutex> checkLock(m_idleMutex);
            const int curTimeout = m_idleUnloadTimeoutSec.load(std::memory_order_acquire);
            if (curTimeout > 0 &&
                m_modelsLoaded &&
                (std::chrono::steady_clock::now() >= m_lastActivityTime + std::chrono::seconds(curTimeout))) {
                m_modelsLoaded = false;
                m_bucketsPrecompiled = false;
                m_useWinMl = false;
                m_loadedModelDir.clear();
                m_loadedModelTier.clear();
                m_winMl.reset();
                m_detNet = cv::dnn::Net{};
                m_clsNet = cv::dnn::Net{};
                m_recNetShort = cv::dnn::Net{};
                m_recNetLong = cv::dnn::Net{};
                m_dictKeys.clear();
                released = true;
            }
        }
        if (released) {
            deep_ocr_detail::cleanupDeepOcrWorkingSet();
            // 严格在 WinML/D3D11 析构与工作集修剪全部完成后再发布卸载完成状态
            m_activeSessionCountAtomic.store(0, std::memory_order_release);
            m_modelsLoadedAtomic.store(false, std::memory_order_release);
            LOG_INFO("DeepOcrEngine: idle timeout reached, released WinML/DirectML sessions and reclaimed memory");
        }
    }
}

void DeepOcrEngine::precompileFixedBucketsLocked() {
    if (m_bucketsPrecompiled || !m_useWinMl || !m_winMl) return;
    try {
        // 仅预编译 1 个最常用轻量检测桶 [0](960x640) 与 1 个主力识别桶 [0](16x320)，避免多桶预占显存
        if (!m_shuttingDown.load(std::memory_order_acquire)) {
            if (waml::LearningModelSession* dSess = m_winMl->ensureDetSession(0)) {
                const auto& sp = WinMlContext::kDetSpecs[0];
                auto dTen = waml::TensorFloat::Create(winrt::single_threaded_vector<int64_t>({sp.n, 3, sp.h, sp.w}));
                waml::LearningModelBinding db(*dSess);
                db.Bind(m_winMl->detInName, dTen);
                (void)dSess->Evaluate(db, L"");
            }
        }
        // 注意：若 16x320 主力桶 ([1]) 已在加载探测阶段完成编译与 Evaluate，则直接复用无需重复热身
        if (!m_shuttingDown.load(std::memory_order_acquire) &&
            m_winMl->supportsBatchEval &&
            !m_winMl->recBucketSessions[1]) {
            if (waml::LearningModelSession* rSess = m_winMl->ensureRecSession(1)) {
                const auto& sp = WinMlContext::kRecSpecs[1];
                auto rTen = waml::TensorFloat::Create(
                    winrt::single_threaded_vector<int64_t>({sp.n, 3, sp.h, sp.w}));
                waml::LearningModelBinding rb(*rSess);
                rb.Bind(m_winMl->recInName, rTen);
                (void)rSess->Evaluate(rb, L"");
            }
        }
        m_activeSessionCountAtomic.store(m_winMl->countActiveSessions(), std::memory_order_release);
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
            touchActivityAndScheduleIdleUnload();
            LOG_INFO("DeepOcrEngine: background static bucket pre-compilation completed");
        }
    });
}

void DeepOcrEngine::trimIdleMemory() {
    bool trimmedGpu = false;
    {
        // 非阻塞尝试加锁，绝不与正在进行的 OCR 推理争锁
        std::unique_lock<std::mutex> lock(m_inferMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }
        if (m_useWinMl && m_winMl) {
            if (m_winMl->ownedD3d11Context) {
                m_winMl->ownedD3d11Context->Flush();
            }
            if (m_winMl->ownedDxgiDevice) {
                m_winMl->ownedDxgiDevice->Trim();
                trimmedGpu = true;
            }
        }
    }
    // 阶段 1 软修剪：仅归还暂存显存并收缩当前进程物理工作集，保留全部已编译 DirectML Session 以保障下次 OCR 0ms 热启
    ::SetProcessWorkingSetSizeEx(
        ::GetCurrentProcess(),
        static_cast<SIZE_T>(-1),
        static_cast<SIZE_T>(-1),
        0);
    LOG_INFO("DeepOcrEngine: stage-1 idle soft-trim completed (dxgiTrim={}, sessions={})",
             trimmedGpu ? "true" : "false", activeSessionCount());
}

void DeepOcrEngine::releaseLoadedModels() {
    OcrWorkerClient::instance().shutdownWorker();
    {
        std::lock_guard<std::mutex> lock(m_inferMutex);
        m_modelsLoaded = false;
        m_bucketsPrecompiled = false;
        m_useWinMl = false;
        m_loadedModelDir.clear();
        m_loadedModelTier.clear();
        m_loadedEngineGen.clear();
        m_winMl.reset();
        m_detNet = cv::dnn::Net{};
        m_clsNet = cv::dnn::Net{};
        m_recNetShort = cv::dnn::Net{};
        m_recNetLong = cv::dnn::Net{};
        m_dictKeys.clear();
    }
    m_idleCv.notify_all();
    deep_ocr_detail::cleanupDeepOcrWorkingSet();
    // 严格在 WinML/D3D11 析构与 COM/工作集修剪全部完成后再发布卸载完成状态，消除并发观测竞态
    m_activeSessionCountAtomic.store(0, std::memory_order_release);
    m_modelsLoadedAtomic.store(false, std::memory_order_release);
}

void DeepOcrEngine::shutdown() {
    m_shuttingDown.store(true, std::memory_order_release);
    m_cancelRequested.store(true, std::memory_order_release);
    m_idleCv.notify_all();
    {
        std::lock_guard<std::mutex> iLock(m_idleMutex);
        if (m_idleUnloadThread.joinable()) {
            m_idleUnloadThread.request_stop();
        }
    }
    m_idleCv.notify_all();
    if (m_idleUnloadThread.joinable()) {
        m_idleUnloadThread.join();
    }
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
    m_downloadingServer.store(false, std::memory_order_release);
    m_downloading.store(false, std::memory_order_release);
    releaseLoadedModels();
}

} // namespace tools3000::ocr
