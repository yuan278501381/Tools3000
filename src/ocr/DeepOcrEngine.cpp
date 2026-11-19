// ─────────────────────────────────────────────────────────────────────────────
// DeepOcrEngine.cpp — 基于 OpenCV 4.12 cv::dnn::Net 的 PP-OCRv4 离线推理引擎实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/DeepOcrEngine.h"
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
#include <winhttp.h>

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

    // 4 档零重编译识别微批次桶: [0]=1x640 (短词), [1]=4x960 (中小段落), [2]=16x960 (中大页), [3]=32x960 (满屏高密度长文主力桶)
    static constexpr std::array<StaticBucketSpec, 4> kRecSpecs = {{
        {1, DeepOcrConstants::kRecImgHeight, 640},
        {4, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecFixedStripWidth},
        {16, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecFixedStripWidth},
        {32, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecFixedStripWidth},
    }};

    waml::LearningModel detModel{nullptr};
    waml::LearningModel clsModel{nullptr};
    waml::LearningModel recModel{nullptr};
    waml::LearningModelDevice device{nullptr};

    waml::LearningModelSession clsSession{nullptr};
    std::array<waml::LearningModelSession, 3> detBucketSessions{nullptr, nullptr, nullptr};
    std::array<waml::LearningModelSession, 4> recBucketSessions{nullptr, nullptr, nullptr, nullptr};
    waml::LearningModelSession cpuFallbackRecSession960{nullptr};

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
        if (!supportsBatchEval) {
            if (idx == 0) {
                if (!recBucketSessions[0]) {
                    waml::LearningModelSessionOptions opts;
                    opts.CloseModelOnSessionCreation(false);
                    recBucketSessions[0] = waml::LearningModelSession(recModel, device, opts);
                }
                return &recBucketSessions[0];
            }
            if (!cpuFallbackRecSession960) {
                waml::LearningModelSessionOptions opts;
                opts.CloseModelOnSessionCreation(false);
                cpuFallbackRecSession960 = waml::LearningModelSession(recModel, device, opts);
            }
            return &cpuFallbackRecSession960;
        }

        if (idx >= kRecSpecs.size()) return nullptr;
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

struct ModelDownloadSpec {
    const wchar_t* localFileName;
    const char* displayName;
    size_t minValidBytes;
    bool isOnnx;
    std::array<const wchar_t*, 4> mirrorUrls;
};

constexpr std::array<ModelDownloadSpec, 4> kModelSpecs = {{
    {
        DeepOcrConstants::kDetModelFileName,
        "ch_PP-OCRv4_det.onnx",
        DeepOcrConstants::kMinDetModelBytes,
        true,
        {
            L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv4/det/ch_PP-OCRv4_det_mobile.onnx",
            L"https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_det_infer.onnx",
            L"https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_det_infer.onnx",
            nullptr
        }
    },
    {
        DeepOcrConstants::kClsModelFileName,
        "ch_ppocr_mobile_v2.0_cls.onnx",
        DeepOcrConstants::kMinClsModelBytes,
        true,
        {
            L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx",
            L"https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv1/ch_ppocr_mobile_v2.0_cls_infer.onnx",
            L"https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv1/ch_ppocr_mobile_v2.0_cls_infer.onnx",
            nullptr
        }
    },
    {
        DeepOcrConstants::kRecModelFileName,
        "ch_PP-OCRv4_rec.onnx",
        DeepOcrConstants::kMinRecModelBytes,
        true,
        {
            L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv4/rec/ch_PP-OCRv4_rec_mobile.onnx",
            L"https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_rec_infer.onnx",
            L"https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_rec_infer.onnx",
            nullptr
        }
    },
    {
        DeepOcrConstants::kKeysDictFileName,
        "ppocr_keys_v1.txt",
        4096,
        false,
        {
            L"https://ghfast.top/https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/ppocr/utils/ppocr_keys_v1.txt",
            L"https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/ppocr/utils/ppocr_keys_v1.txt",
            nullptr,
            nullptr
        }
    }
}};

bool readFileBytes(const std::filesystem::path& filePath, std::vector<uint8_t>& outBytes, size_t maxBytes = 128ULL * 1024ULL * 1024ULL) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(filePath, ec) || ec) return false;
    const auto sz = std::filesystem::file_size(filePath, ec);
    if (ec || sz == 0 || sz > maxBytes) return false;
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs) return false;
    outBytes.resize(static_cast<size_t>(sz));
    ifs.read(reinterpret_cast<char*>(outBytes.data()), static_cast<std::streamsize>(outBytes.size()));
    return static_cast<size_t>(ifs.gcount()) == outBytes.size();
}

bool loadDictionaryLines(const std::filesystem::path& dictPath, std::vector<std::string>& outKeys) {
    std::ifstream ifs(dictPath, std::ios::binary);
    if (!ifs) return false;
    outKeys.clear();
    outKeys.reserve(6700);
    std::string line;
    bool firstLine = true;
    while (std::getline(ifs, line)) {
        if (firstLine) {
            firstLine = false;
            if (line.size() >= 3 &&
                static_cast<uint8_t>(line[0]) == 0xEF &&
                static_cast<uint8_t>(line[1]) == 0xBB &&
                static_cast<uint8_t>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty()) {
            outKeys.push_back(std::move(line));
        }
    }
    return outKeys.size() >= 16;
}

bool isDirectoryCompleteModelBundle(const std::filesystem::path& dir) {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec) || ec) {
        return false;
    }
    const auto checkFileMinSize = [&](const wchar_t* name, size_t minSize) {
        const auto p = dir / name;
        if (!std::filesystem::is_regular_file(p, ec) || ec) return false;
        const auto fsz = std::filesystem::file_size(p, ec);
        return !ec && fsz >= minSize;
    };
    return checkFileMinSize(DeepOcrConstants::kDetModelFileName, 256) &&
           checkFileMinSize(DeepOcrConstants::kClsModelFileName, 256) &&
           checkFileMinSize(DeepOcrConstants::kRecModelFileName, 256) &&
           (checkFileMinSize(DeepOcrConstants::kKeysDictFileName, 64) ||
            checkFileMinSize(DeepOcrConstants::kRecModelFileName, DeepOcrConstants::kMinRecModelBytes));
}

std::array<cv::Point2f, 4> orderQuadPointsClockwise(const cv::Point2f rawPts[4]) noexcept {
    std::array<cv::Point2f, 4> sorted{rawPts[0], rawPts[1], rawPts[2], rawPts[3]};
    std::sort(sorted.begin(), sorted.end(), [](const cv::Point2f& a, const cv::Point2f& b) {
        return (a.x != b.x) ? (a.x < b.x) : (a.y < b.y);
    });

    cv::Point2f tl = sorted[0];
    cv::Point2f bl = sorted[1];
    if (tl.y > bl.y) std::swap(tl, bl);

    cv::Point2f tr = sorted[2];
    cv::Point2f br = sorted[3];
    if (tr.y > br.y) std::swap(tr, br);

    return {tl, tr, br, bl};
}

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

bool downloadHttpsUrlToFile(
    const wchar_t* url,
    const std::filesystem::path& tempFilePath,
    std::stop_token stopToken,
    std::atomic<bool>& cancelFlag,
    const std::function<void(uint64_t deltaBytes, uint64_t contentLength)>& onChunk) {
    if (!url || wcslen(url) == 0) return false;

    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    urlComp.lpszHostName = hostBuf;
    urlComp.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    urlComp.lpszUrlPath = pathBuf;
    urlComp.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf));

    if (!WinHttpCrackUrl(url, 0, 0, &urlComp) || urlComp.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }

    HINTERNET hSession = WinHttpOpen(
        L"Tools3000/1.0 (PP-OCRv4 Offline Model Installer)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return false;

    struct WinHttpHandleGuard {
        HINTERNET s = nullptr;
        HINTERNET c = nullptr;
        HINTERNET r = nullptr;
        ~WinHttpHandleGuard() {
            if (r) WinHttpCloseHandle(r);
            if (c) WinHttpCloseHandle(c);
            if (s) WinHttpCloseHandle(s);
        }
    } guard{hSession, nullptr, nullptr};

    WinHttpSetTimeouts(hSession, 10000, 12000, 15000, 25000);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    guard.c = WinHttpConnect(hSession, hostBuf, urlComp.nPort, 0);
    if (!guard.c) return false;

    guard.r = WinHttpOpenRequest(
        guard.c,
        L"GET",
        pathBuf,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!guard.r) return false;

    if (!WinHttpSendRequest(guard.r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return false;
    }
    if (!WinHttpReceiveResponse(guard.r, nullptr)) {
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(guard.r,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        statusCode < 200 || statusCode >= 300) {
        return false;
    }

    uint64_t contentLen = 0;
    wchar_t lenHeader[64]{};
    DWORD lenHeaderBytes = sizeof(lenHeader);
    if (WinHttpQueryHeaders(guard.r, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, lenHeader, &lenHeaderBytes, WINHTTP_NO_HEADER_INDEX)) {
        contentLen = std::wcstoull(lenHeader, nullptr, 10);
    }

    std::ofstream ofs(tempFilePath, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;

    constexpr DWORD kChunkSize = 64 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);
    uint64_t writtenTotal = 0;
    constexpr uint64_t kMaxSingleModelBytes = 96ULL * 1024ULL * 1024ULL;

    while (!stopToken.stop_requested() && !cancelFlag.load(std::memory_order_acquire)) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(guard.r, &avail)) return false;
        if (avail == 0) break;

        const DWORD toRead = (std::min)(avail, kChunkSize);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(guard.r, buffer.data(), toRead, &bytesRead)) return false;
        if (bytesRead == 0) break;

        writtenTotal += bytesRead;
        if (writtenTotal > kMaxSingleModelBytes) return false;

        ofs.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(bytesRead));
        if (!ofs) return false;

        if (onChunk) {
            onChunk(bytesRead, contentLen);
        }
    }

    if (stopToken.stop_requested() || cancelFlag.load(std::memory_order_acquire)) {
        return false;
    }

    ofs.flush();
    return static_cast<bool>(ofs) && writtenTotal > 0;
}

void cleanupDeepOcrWorkingSet() {
    tools3000::core::WinUtils::trimWorkingSet();
}

} // namespace

DeepOcrEngine::DeepOcrEngine() = default;

DeepOcrEngine::~DeepOcrEngine() {
    shutdown();
}

DeepOcrEngine& DeepOcrEngine::instance() {
    static DeepOcrEngine inst;
    return inst;
}

std::filesystem::path DeepOcrEngine::defaultInstallDirectory() const {
    return tools3000::core::WinUtils::getAppDataDirectory() / L"models" / L"ocr";
}

void DeepOcrEngine::setCustomModelDirectoryForTest(const std::filesystem::path& customDir) {
    {
        std::lock_guard<std::mutex> lock(m_pathMutex);
        m_customTestModelDir = customDir;
    }
    releaseLoadedModels();
}

std::filesystem::path DeepOcrEngine::resolveModelDirectory(bool* outInstalled) const {
    {
        std::lock_guard<std::mutex> lock(m_pathMutex);
        if (!m_customTestModelDir.empty()) {
            const bool ok = isDirectoryCompleteModelBundle(m_customTestModelDir);
            if (outInstalled) *outInstalled = ok;
            return m_customTestModelDir;
        }
    }

    const std::filesystem::path dataModelDir = defaultInstallDirectory();
    if (isDirectoryCompleteModelBundle(dataModelDir)) {
        if (outInstalled) *outInstalled = true;
        return dataModelDir;
    }

    const std::filesystem::path exeModelDir = tools3000::core::WinUtils::getExeDirectory() / L"models" / L"ocr";
    if (isDirectoryCompleteModelBundle(exeModelDir)) {
        if (outInstalled) *outInstalled = true;
        return exeModelDir;
    }

    if (outInstalled) *outInstalled = false;
    return dataModelDir;
}

bool DeepOcrEngine::isModelInstalled() const {
    bool installed = false;
    (void)resolveModelDirectory(&installed);
    return installed;
}

bool DeepOcrEngine::isValidOnnxBuffer(const uint8_t* data, size_t byteSize, size_t minBytes) noexcept {
    if (!data || byteSize < minBytes || byteSize < 16) {
        return false;
    }
    // 拦截网关返回的 HTML / JSON / XML 文本伪文件
    size_t nonWs = 0;
    while (nonWs < byteSize && nonWs < 16 &&
           (data[nonWs] == ' ' || data[nonWs] == '\t' || data[nonWs] == '\r' || data[nonWs] == '\n')) {
        ++nonWs;
    }
    if (nonWs < byteSize && (data[nonWs] == '<' || data[nonWs] == '{' || data[nonWs] == '[')) {
        return false;
    }
    // ONNX ModelProto 合法的顶层 Protobuf 字段 tag:
    // 0x08 (ir_version, varint), 0x12 (producer_name), 0x1A (producer_version),
    // 0x22 (domain), 0x28 (model_version), 0x3A (graph), 0x42 (opset_import)
    const uint8_t tag = data[0];
    const uint8_t wireType = tag & 0x07u;
    const uint8_t fieldNum = tag >> 3u;
    if (fieldNum == 0 || fieldNum > 20) return false;
    if (wireType != 0 && wireType != 2) return false;
    return true;
}

namespace {

bool readProtobufVarint(const uint8_t* data, size_t byteSize, size_t& cursor, uint64_t& outVal) noexcept {
    outVal = 0;
    uint32_t shift = 0;
    while (cursor < byteSize && shift < 64) {
        const uint8_t b = data[cursor++];
        outVal |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
        if ((b & 0x80u) == 0) {
            return true;
        }
        shift += 7;
    }
    return false;
}

bool splitCharacterPayloadIntoLines(std::string_view payload, std::vector<std::string>& outKeys) {
    outKeys.clear();
    outKeys.reserve(6700);
    size_t start = 0;
    bool firstLine = true;
    while (start <= payload.size()) {
        size_t nl = payload.find('\n', start);
        std::string_view line = (nl == std::string_view::npos)
            ? payload.substr(start)
            : payload.substr(start, nl - start);
        if (firstLine) {
            firstLine = false;
            if (line.size() >= 3 &&
                static_cast<uint8_t>(line[0]) == 0xEF &&
                static_cast<uint8_t>(line[1]) == 0xBB &&
                static_cast<uint8_t>(line[2]) == 0xBF) {
                line.remove_prefix(3);
            }
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            outKeys.emplace_back(line);
        }
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return outKeys.size() >= 16;
}

bool parseMetadataEntryProto(const uint8_t* entryData, size_t entryLen, std::string_view& outKey, std::string_view& outVal) noexcept {
    outKey = {};
    outVal = {};
    size_t pos = 0;
    while (pos < entryLen) {
        uint64_t tag = 0;
        if (!readProtobufVarint(entryData, entryLen, pos, tag) || tag == 0) return false;
        const uint32_t wire = static_cast<uint32_t>(tag & 0x07u);
        const uint32_t field = static_cast<uint32_t>(tag >> 3u);
        if (wire == 2) {
            uint64_t strLen = 0;
            if (!readProtobufVarint(entryData, entryLen, pos, strLen)) return false;
            if (strLen > entryLen - pos) return false;
            std::string_view sv(reinterpret_cast<const char*>(entryData + pos), static_cast<size_t>(strLen));
            if (field == 1) outKey = sv;
            else if (field == 2) outVal = sv;
            pos += static_cast<size_t>(strLen);
        } else if (wire == 0) {
            uint64_t dummy = 0;
            if (!readProtobufVarint(entryData, entryLen, pos, dummy)) return false;
        } else if (wire == 1) {
            if (entryLen - pos < 8) return false;
            pos += 8;
        } else if (wire == 5) {
            if (entryLen - pos < 4) return false;
            pos += 4;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

bool DeepOcrEngine::extractEmbeddedCharacterDictFromOnnx(
    const uint8_t* data,
    size_t byteSize,
    std::vector<std::string>& outKeys) {
    outKeys.clear();
    if (!data || byteSize < 32) {
        return false;
    }

    // 路径 1: 标准 ONNX ModelProto 顶层字段遍历 (metadata_props = field 14, wire_type 2 => tag 0x72)
    size_t cursor = 0;
    while (cursor < byteSize) {
        uint64_t tag = 0;
        if (!readProtobufVarint(data, byteSize, cursor, tag) || tag == 0) {
            break;
        }
        const uint32_t wire = static_cast<uint32_t>(tag & 0x07u);
        const uint32_t field = static_cast<uint32_t>(tag >> 3u);
        if (wire == 2) {
            uint64_t len = 0;
            if (!readProtobufVarint(data, byteSize, cursor, len) || len > byteSize - cursor) {
                break;
            }
            if (field == 14) {
                std::string_view k, v;
                if (parseMetadataEntryProto(data + cursor, static_cast<size_t>(len), k, v) &&
                    (k == "character" || k == "dict") && !v.empty()) {
                    if (splitCharacterPayloadIntoLines(v, outKeys)) {
                        return true;
                    }
                }
            }
            cursor += static_cast<size_t>(len);
        } else if (wire == 0) {
            uint64_t dummy = 0;
            if (!readProtobufVarint(data, byteSize, cursor, dummy)) break;
        } else if (wire == 1) {
            if (byteSize - cursor < 8) break;
            cursor += 8;
        } else if (wire == 5) {
            if (byteSize - cursor < 4) break;
            cursor += 4;
        } else {
            break;
        }
    }

    // 路径 2: 尾部元数据特征扫描兜底 (\x0A\x09character\x12 + varint length)
    static constexpr uint8_t kMarker[] = {0x0A, 0x09, 'c', 'h', 'a', 'r', 'a', 'c', 't', 'e', 'r', 0x12};
    const size_t scanWindow = (std::min)(byteSize, static_cast<size_t>(512 * 1024));
    const size_t scanStart = byteSize - scanWindow;
    for (size_t i = scanStart; i + sizeof(kMarker) < byteSize; ++i) {
        if (std::memcmp(data + i, kMarker, sizeof(kMarker)) == 0) {
            size_t vPos = i + sizeof(kMarker);
            uint64_t vLen = 0;
            if (readProtobufVarint(data, byteSize, vPos, vLen) && vLen > 0 && vLen <= byteSize - vPos) {
                std::string_view v(reinterpret_cast<const char*>(data + vPos), static_cast<size_t>(vLen));
                if (splitCharacterPayloadIntoLines(v, outKeys)) {
                    return true;
                }
            }
        }
    }

    outKeys.clear();
    return false;
}

namespace {

void appendProtobufVarint(std::vector<uint8_t>& out, uint64_t val) {
    while (true) {
        uint8_t byte = static_cast<uint8_t>(val & 0x7Fu);
        val >>= 7u;
        if (val != 0) {
            out.push_back(static_cast<uint8_t>(byte | 0x80u));
        } else {
            out.push_back(byte);
            break;
        }
    }
}

void appendProtobufLengthDelimited(std::vector<uint8_t>& out, uint32_t fieldNum, const uint8_t* payload, size_t len) {
    appendProtobufVarint(out, (static_cast<uint64_t>(fieldNum) << 3u) | 2ULL);
    appendProtobufVarint(out, static_cast<uint64_t>(len));
    if (len > 0 && payload != nullptr) {
        out.insert(out.end(), payload, payload + len);
    }
}

std::string extractValueInfoName(const uint8_t* buf, size_t len) {
    size_t p = 0;
    while (p < len) {
        uint64_t tag = 0;
        if (!readProtobufVarint(buf, len, p, tag) || tag == 0) break;
        const uint32_t wire = static_cast<uint32_t>(tag & 0x07u);
        const uint32_t fn = static_cast<uint32_t>(tag >> 3u);
        if (wire == 2) {
            uint64_t slen = 0;
            if (!readProtobufVarint(buf, len, p, slen) || slen > len - p) break;
            if (fn == 1) {
                return std::string(reinterpret_cast<const char*>(buf + p), static_cast<size_t>(slen));
            }
            p += static_cast<size_t>(slen);
        } else if (wire == 0) {
            uint64_t dummy = 0;
            if (!readProtobufVarint(buf, len, p, dummy)) break;
        } else if (wire == 1) {
            if (len - p < 8) break;
            p += 8;
        } else if (wire == 5) {
            if (len - p < 4) break;
            p += 4;
        } else {
            break;
        }
    }
    return "x";
}

std::vector<uint8_t> buildFixedFloatValueInfoProto(std::string_view inputName, const std::array<int64_t, 4>& dims) {
    std::vector<uint8_t> shapePayload;
    shapePayload.reserve(48);
    for (int64_t d : dims) {
        std::vector<uint8_t> dimMsg;
        appendProtobufVarint(dimMsg, (1ULL << 3u) | 0ULL); // Dimension.dim_value = field 1, varint
        appendProtobufVarint(dimMsg, static_cast<uint64_t>(d));
        appendProtobufLengthDelimited(shapePayload, 1, dimMsg.data(), dimMsg.size()); // TensorShapeProto.dim = field 1
    }

    std::vector<uint8_t> tensorPayload;
    appendProtobufVarint(tensorPayload, (1ULL << 3u) | 0ULL); // TypeProto.Tensor.elem_type = 1 (FLOAT)
    appendProtobufVarint(tensorPayload, 1ULL);
    appendProtobufLengthDelimited(tensorPayload, 2, shapePayload.data(), shapePayload.size()); // shape = field 2

    std::vector<uint8_t> typePayload;
    appendProtobufLengthDelimited(typePayload, 1, tensorPayload.data(), tensorPayload.size()); // TypeProto.tensor_type = field 1

    std::vector<uint8_t> valueInfo;
    appendProtobufLengthDelimited(
        valueInfo, 1, reinterpret_cast<const uint8_t*>(inputName.data()), inputName.size()); // ValueInfoProto.name = field 1
    appendProtobufLengthDelimited(valueInfo, 2, typePayload.data(), typePayload.size());     // ValueInfoProto.type = field 2
    return valueInfo;
}

} // namespace

bool DeepOcrEngine::patchOnnxGraphInputShape(
    const uint8_t* data,
    size_t byteSize,
    const std::array<int64_t, 4>& fixedDims,
    std::vector<uint8_t>& outPatchedBytes) {
    outPatchedBytes.clear();
    if (!data || byteSize < 16) return false;

    outPatchedBytes.reserve(byteSize + 64);
    size_t pos = 0;
    bool patchedInput = false;

    while (pos < byteSize) {
        const size_t fieldStart = pos;
        uint64_t tag = 0;
        if (!readProtobufVarint(data, byteSize, pos, tag) || tag == 0) {
            return false;
        }
        const uint32_t wire = static_cast<uint32_t>(tag & 0x07u);
        const uint32_t fnum = static_cast<uint32_t>(tag >> 3u);

        if (fnum == 7 && wire == 2) { // ModelProto.graph = field 7
            uint64_t graphLen = 0;
            if (!readProtobufVarint(data, byteSize, pos, graphLen) || graphLen > byteSize - pos) {
                return false;
            }
            const uint8_t* gData = data + pos;
            const size_t gSize = static_cast<size_t>(graphLen);
            pos += gSize;

            std::vector<uint8_t> newGraph;
            newGraph.reserve(gSize + 64);
            size_t gp = 0;
            while (gp < gSize) {
                const size_t gFieldStart = gp;
                uint64_t gtag = 0;
                if (!readProtobufVarint(gData, gSize, gp, gtag) || gtag == 0) {
                    return false;
                }
                const uint32_t gwire = static_cast<uint32_t>(gtag & 0x07u);
                const uint32_t gfnum = static_cast<uint32_t>(gtag >> 3u);

                if (gfnum == 11 && gwire == 2 && !patchedInput) { // GraphProto.input = field 11
                    uint64_t inLen = 0;
                    if (!readProtobufVarint(gData, gSize, gp, inLen) || inLen > gSize - gp) {
                        return false;
                    }
                    const std::string inName = extractValueInfoName(gData + gp, static_cast<size_t>(inLen));
                    gp += static_cast<size_t>(inLen);
                    const auto fixedVi = buildFixedFloatValueInfoProto(inName, fixedDims);
                    appendProtobufLengthDelimited(newGraph, 11, fixedVi.data(), fixedVi.size());
                    patchedInput = true;
                } else if (gwire == 0) {
                    uint64_t dummy = 0;
                    if (!readProtobufVarint(gData, gSize, gp, dummy)) return false;
                    newGraph.insert(newGraph.end(), gData + gFieldStart, gData + gp);
                } else if (gwire == 2) {
                    uint64_t subLen = 0;
                    if (!readProtobufVarint(gData, gSize, gp, subLen) || subLen > gSize - gp) return false;
                    gp += static_cast<size_t>(subLen);
                    newGraph.insert(newGraph.end(), gData + gFieldStart, gData + gp);
                } else if (gwire == 5) {
                    if (gSize - gp < 4) return false;
                    gp += 4;
                    newGraph.insert(newGraph.end(), gData + gFieldStart, gData + gp);
                } else if (gwire == 1) {
                    if (gSize - gp < 8) return false;
                    gp += 8;
                    newGraph.insert(newGraph.end(), gData + gFieldStart, gData + gp);
                } else {
                    return false;
                }
            }
            appendProtobufLengthDelimited(outPatchedBytes, 7, newGraph.data(), newGraph.size());
        } else if (wire == 0) {
            uint64_t dummy = 0;
            if (!readProtobufVarint(data, byteSize, pos, dummy)) return false;
            outPatchedBytes.insert(outPatchedBytes.end(), data + fieldStart, data + pos);
        } else if (wire == 2) {
            uint64_t subLen = 0;
            if (!readProtobufVarint(data, byteSize, pos, subLen) || subLen > byteSize - pos) return false;
            pos += static_cast<size_t>(subLen);
            outPatchedBytes.insert(outPatchedBytes.end(), data + fieldStart, data + pos);
        } else if (wire == 5) {
            if (byteSize - pos < 4) return false;
            pos += 4;
            outPatchedBytes.insert(outPatchedBytes.end(), data + fieldStart, data + pos);
        } else if (wire == 1) {
            if (byteSize - pos < 8) return false;
            pos += 8;
            outPatchedBytes.insert(outPatchedBytes.end(), data + fieldStart, data + pos);
        } else {
            return false;
        }
    }

    return patchedInput;
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
                        waml::LearningModelSession s4(ctx->recModel, gpuDev, probeOpts);
                        auto t4 = waml::TensorFloat::Create(winrt::single_threaded_vector<int64_t>({
                            4, 3, DeepOcrConstants::kRecImgHeight, DeepOcrConstants::kRecFixedStripWidth}));
                        waml::LearningModelBinding b4(s4);
                        b4.Bind(ctx->recInName, t4);
                        auto out4 = s4.Evaluate(b4, L"").Outputs().Lookup(ctx->recOutName).as<waml::TensorFloat>();
                        if (out4 && out4.Shape().Size() >= 3 && out4.Shape().GetAt(0) == 4) {
                            ctx->recBucketSessions[1] = std::move(s4);
                            batchOk = true;
                        }
                    } catch (...) {
                        batchOk = false;
                    }

                    if (!batchOk) {
                        waml::LearningModelSession s1(ctx->recModel, gpuDev, probeOpts);
                        auto t1 = waml::TensorFloat::Create(winrt::single_threaded_vector<int64_t>({
                            1, 3, DeepOcrConstants::kRecImgHeight, 640}));
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

std::vector<DbDetectedQuad> DeepOcrEngine::postprocessDbMap(
    const cv::Mat& probMapInput,
    int origWidth,
    int origHeight,
    float ratioW,
    float ratioH,
    float binaryThresh,
    float boxScoreThresh,
    float unclipRatio) {
    std::vector<DbDetectedQuad> results;
    if (probMapInput.empty() || origWidth <= 0 || origHeight <= 0) {
        return results;
    }

    cv::Mat prob2d;
    if (probMapInput.dims == 4 && probMapInput.size[2] > 0 && probMapInput.size[3] > 0) {
        prob2d = cv::Mat(probMapInput.size[2], probMapInput.size[3], CV_32F,
                         const_cast<float*>(probMapInput.ptr<float>())).clone();
    } else if (probMapInput.dims == 2) {
        if (probMapInput.type() == CV_32FC1) {
            prob2d = probMapInput;
        } else {
            probMapInput.convertTo(prob2d, CV_32FC1);
        }
    } else {
        return results;
    }

    double minV = 0.0, maxV = 1.0;
    cv::minMaxLoc(prob2d, &minV, &maxV);
    if (minV < -0.02 || maxV > 1.02) {
        cv::Mat neg;
        cv::exp(-prob2d, neg);
        prob2d = 1.0f / (1.0f + neg);
    }

    cv::Mat bitMap;
    cv::threshold(prob2d, bitMap, binaryThresh, 255.0, cv::THRESH_BINARY);
    bitMap.convertTo(bitMap, CV_8UC1);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bitMap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) return results;

    if (contours.size() > static_cast<size_t>(DeepOcrConstants::kDetMaxCandidates)) {
        contours.resize(static_cast<size_t>(DeepOcrConstants::kDetMaxCandidates));
    }

    const float safeRatioW = (ratioW > 1e-5f) ? ratioW : 1.0f;
    const float safeRatioH = (ratioH > 1e-5f) ? ratioH : 1.0f;
    results.reserve(contours.size());

    for (const auto& contour : contours) {
        if (contour.size() < 4) continue;

        cv::RotatedRect minRect = cv::minAreaRect(contour);
        const float minSide = (std::min)(minRect.size.width, minRect.size.height);
        if (minSide < DeepOcrConstants::kDetMinBoxSidePx) continue;

        // 计算局部多边形 ROI 内的概率图均分
        cv::Rect polyRoi = cv::boundingRect(contour) & cv::Rect(0, 0, prob2d.cols, prob2d.rows);
        if (polyRoi.width <= 0 || polyRoi.height <= 0) continue;

        cv::Mat maskRoi = cv::Mat::zeros(polyRoi.size(), CV_8UC1);
        std::vector<cv::Point> shiftedContour;
        shiftedContour.reserve(contour.size());
        for (const auto& pt : contour) {
            shiftedContour.emplace_back(pt.x - polyRoi.x, pt.y - polyRoi.y);
        }
        const cv::Point* ptsPtr = shiftedContour.data();
        const int npts = static_cast<int>(shiftedContour.size());
        cv::fillPoly(maskRoi, &ptsPtr, &npts, 1, cv::Scalar(255));

        const float score = static_cast<float>(cv::mean(prob2d(polyRoi), maskRoi)[0]);
        if (score < boxScoreThresh) continue;

        // DBNet Unclip 外扩: distance = Area * unclipRatio / Perimeter
        const double polyArea = (std::max)(1.0, std::abs(cv::contourArea(contour)));
        const double polyPerim = (std::max)(1.0, cv::arcLength(contour, true));
        const float expandDist = (std::max)(
            minSide * 0.30f,
            static_cast<float>((polyArea * static_cast<double>(unclipRatio)) / polyPerim));

        minRect.size.width += 2.0f * expandDist;
        minRect.size.height += 2.0f * expandDist;

        cv::Point2f boxPts[4]{};
        minRect.points(boxPts);
        auto ordered = orderQuadPointsClockwise(boxPts);

        float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
        for (auto& pt : ordered) {
            pt.x = std::clamp(pt.x / safeRatioW, 0.0f, static_cast<float>(origWidth));
            pt.y = std::clamp(pt.y / safeRatioH, 0.0f, static_cast<float>(origHeight));
            minX = (std::min)(minX, pt.x);
            minY = (std::min)(minY, pt.y);
            maxX = (std::max)(maxX, pt.x);
            maxY = (std::max)(maxY, pt.y);
        }
        if ((maxX - minX) < 3.0f || (maxY - minY) < 3.0f) continue;

        results.push_back(DbDetectedQuad{ordered, score});
    }

    // 阅读序初排：按垂直位置为主，同一水平带按左边界升序
    std::sort(results.begin(), results.end(), [](const DbDetectedQuad& a, const DbDetectedQuad& b) {
        const float cyA = (a.corners[0].y + a.corners[2].y) * 0.5f;
        const float cyB = (b.corners[0].y + b.corners[2].y) * 0.5f;
        const float hA = (std::max)(4.0f, std::abs(a.corners[3].y - a.corners[0].y));
        const float hB = (std::max)(4.0f, std::abs(b.corners[3].y - b.corners[0].y));
        if (std::abs(cyA - cyB) <= (std::min)(hA, hB) * 0.5f) {
            return a.corners[0].x < b.corners[0].x;
        }
        return cyA < cyB;
    });

    return results;
}

cv::Mat DeepOcrEngine::warpPerspectiveQuad(
    const cv::Mat& image,
    const std::array<cv::Point2f, 4>& quad,
    cv::Mat* outInverseTransform,
    bool* outRotatedVertical) {
    if (outRotatedVertical) *outRotatedVertical = false;
    if (image.empty()) return {};

    const float wTop = std::hypot(quad[1].x - quad[0].x, quad[1].y - quad[0].y);
    const float wBot = std::hypot(quad[2].x - quad[3].x, quad[2].y - quad[3].y);
    const float hLeft = std::hypot(quad[3].x - quad[0].x, quad[3].y - quad[0].y);
    const float hRight = std::hypot(quad[2].x - quad[1].x, quad[2].y - quad[1].y);

    const int cropW = (std::max)(4, static_cast<int>(std::round((std::max)(wTop, wBot))));
    const int cropH = (std::max)(4, static_cast<int>(std::round((std::max)(hLeft, hRight))));

    const cv::Point2f srcPts[4] = {quad[0], quad[1], quad[2], quad[3]};
    const cv::Point2f dstPts[4] = {
        {0.0f, 0.0f},
        {static_cast<float>(cropW), 0.0f},
        {static_cast<float>(cropW), static_cast<float>(cropH)},
        {0.0f, static_cast<float>(cropH)}
    };

    if (outInverseTransform) {
        *outInverseTransform = cv::getPerspectiveTransform(dstPts, srcPts);
    }

    // 先截取紧致局部 ROI 再做局部透视变换，消除在 2K/4K 全屏大图上的跨页缓存未命中
    float minX = quad[0].x, maxX = quad[0].x, minY = quad[0].y, maxY = quad[0].y;
    for (size_t i = 1; i < 4; ++i) {
        minX = (std::min)(minX, quad[i].x);
        maxX = (std::max)(maxX, quad[i].x);
        minY = (std::min)(minY, quad[i].y);
        maxY = (std::max)(maxY, quad[i].y);
    }
    const int rx0 = std::clamp(static_cast<int>(std::floor(minX)) - 2, 0, image.cols - 1);
    const int ry0 = std::clamp(static_cast<int>(std::floor(minY)) - 2, 0, image.rows - 1);
    const int rx1 = std::clamp(static_cast<int>(std::ceil(maxX)) + 2, rx0 + 1, image.cols);
    const int ry1 = std::clamp(static_cast<int>(std::ceil(maxY)) + 2, ry0 + 1, image.rows);

    const cv::Point2f localSrcPts[4] = {
        {quad[0].x - static_cast<float>(rx0), quad[0].y - static_cast<float>(ry0)},
        {quad[1].x - static_cast<float>(rx0), quad[1].y - static_cast<float>(ry0)},
        {quad[2].x - static_cast<float>(rx0), quad[2].y - static_cast<float>(ry0)},
        {quad[3].x - static_cast<float>(rx0), quad[3].y - static_cast<float>(ry0)}
    };
    const cv::Mat localM = cv::getPerspectiveTransform(localSrcPts, dstPts);
    cv::Mat roiView = image(cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0));

    cv::Mat warped;
    cv::warpPerspective(roiView, warped, localM, cv::Size(cropW, cropH), cv::INTER_LINEAR, cv::BORDER_REPLICATE);

    if (cropH >= static_cast<int>(static_cast<float>(cropW) * 1.5f) && cropW >= 8) {
        cv::rotate(warped, warped, cv::ROTATE_90_COUNTERCLOCKWISE);
        if (outRotatedVertical) *outRotatedVertical = true;
    }
    return warped;
}

CtcDecodeOutput DeepOcrEngine::decodeCtcGreedy(
    const cv::Mat& timeByClassMat,
    const std::vector<std::string>& dictKeys,
    float validWidthRatio) {
    CtcDecodeOutput output{};
    if (timeByClassMat.empty() || timeByClassMat.rows <= 0 || timeByClassMat.cols <= 1) {
        return output;
    }

    cv::Mat mat32f;
    if (timeByClassMat.type() == CV_32FC1) {
        mat32f = timeByClassMat;
    } else {
        timeByClassMat.convertTo(mat32f, CV_32FC1);
    }

    const int T = mat32f.rows;
    const int C = mat32f.cols;

    struct RawSpan {
        int classIdx = 0;
        int startStep = 0;
        int endStep = 0;
        int peakStep = 0;
        float peakProb = 0.0f;
    };

    const float safeValidRatio = std::clamp(validWidthRatio, 0.05f, 1.0f);
    const float validSteps = (std::max)(1.0f, static_cast<float>(T) * safeValidRatio);
    const int scanSteps = (safeValidRatio < 0.99f)
        ? (std::min)(T, static_cast<int>(std::ceil(validSteps)) + 2)
        : T;

    std::vector<RawSpan> spans;
    spans.reserve(static_cast<size_t>((std::min)(scanSteps, 256)));

    int prevIdx = 0;
    for (int t = 0; t < scanSteps; ++t) {
        const float* row = mat32f.ptr<float>(t);
        int bestIdx = 0;
        float bestVal = row[0];
        for (int c = 1; c < C; ++c) {
            const float v = row[c];
            if (v > bestVal) {
                bestVal = v;
                bestIdx = c;
            }
        }

        if (bestIdx > 0) {
            float prob = bestVal;
            if (bestVal > 1.001f || row[0] < -0.001f) {
                double sumExp = 0.0;
                for (int c = 0; c < C; ++c) {
                    sumExp += std::exp(static_cast<double>(row[c] - bestVal));
                }
                prob = sumExp > 0.0 ? static_cast<float>(1.0 / sumExp) : 0.0f;
            }
            prob = std::clamp(prob, 0.0f, 1.0f);

            if (bestIdx == prevIdx && !spans.empty()) {
                auto& back = spans.back();
                back.endStep = t;
                if (prob > back.peakProb) {
                    back.peakProb = prob;
                    back.peakStep = t;
                }
            } else {
                spans.push_back(RawSpan{bestIdx, t, t, t, prob});
            }
        }
        prevIdx = bestIdx;
    }

    if (spans.empty()) {
        return output;
    }

    std::vector<CtcDecodedChar> rawChars;
    rawChars.reserve(spans.size());
    float probSum = 0.0f;

    for (const auto& sp : spans) {
        std::string tokenStr;
        if (sp.classIdx >= 1 && static_cast<size_t>(sp.classIdx) <= dictKeys.size()) {
            tokenStr = dictKeys[static_cast<size_t>(sp.classIdx - 1)];
        } else if (static_cast<size_t>(sp.classIdx) == dictKeys.size() + 1) {
            tokenStr = " ";
        }
        if (tokenStr.empty()) continue;

        CtcDecodedChar dc;
        dc.utf8Char = std::move(tokenStr);
        dc.confidence = sp.peakProb;
        dc.startStep = sp.startStep;
        dc.peakStep = sp.peakStep;
        dc.endStep = sp.endStep;
        rawChars.push_back(std::move(dc));
        probSum += sp.peakProb;
    }

    if (rawChars.empty()) {
        return output;
    }

    // 计算每个解码字符在有效裁剪宽度上的精确归一化区间 [normLeft, normRight]
    const size_t N = rawChars.size();
    std::vector<float> centers(N, 0.0f);
    for (size_t i = 0; i < N; ++i) {
        const float midStep = 0.5f * static_cast<float>(rawChars[i].startStep + rawChars[i].endStep) + 0.5f;
        centers[i] = std::clamp(midStep / validSteps, 0.0f, 1.0f);
    }

    for (size_t i = 0; i < N; ++i) {
        const float spanHalf = static_cast<float>(rawChars[i].endStep - rawChars[i].startStep + 1) / (2.0f * validSteps);
        float neighborHalf = 0.45f / static_cast<float>(N);
        if (N > 1) {
            if (i == 0) neighborHalf = (centers[1] - centers[0]) * 0.5f;
            else if (i + 1 == N) neighborHalf = (centers[N - 1] - centers[N - 2]) * 0.5f;
            else neighborHalf = ((centers[i] - centers[i - 1]) + (centers[i + 1] - centers[i])) * 0.25f;
        }
        const float halfW = (std::max)(spanHalf, (std::max)(0.005f, neighborHalf));

        float left = (std::max)(0.0f, centers[i] - halfW);
        float right = (std::min)(1.0f, centers[i] + halfW);
        if (i > 0) {
            const float midPrev = 0.5f * (centers[i - 1] + centers[i]);
            left = (std::max)(left, midPrev);
        }
        if (i + 1 < N) {
            const float midNext = 0.5f * (centers[i] + centers[i + 1]);
            right = (std::min)(right, midNext);
        }
        if (right <= left) {
            right = (std::min)(1.0f, left + (1.0f / static_cast<float>(N + 1)));
            if (right <= left) left = (std::max)(0.0f, right - 0.01f);
        }
        rawChars[i].normLeft = left;
        rawChars[i].normRight = right;
    }

    // 若某字典项包含多个 Unicode 码点，按码点数线性细分，严格保证 1 个码点 = 1 个 CtcDecodedChar
    output.chars.reserve(rawChars.size());
    for (const auto& rc : rawChars) {
        output.text += rc.utf8Char;
        const auto cps = OcrEngine::splitUtf8Codepoints(rc.utf8Char);
        if (cps.size() <= 1) {
            output.chars.push_back(rc);
        } else {
            const float totalSpan = rc.normRight - rc.normLeft;
            for (size_t k = 0; k < cps.size(); ++k) {
                CtcDecodedChar sub = rc;
                sub.utf8Char = cps[k];
                sub.normLeft = rc.normLeft + totalSpan * (static_cast<float>(k) / static_cast<float>(cps.size()));
                sub.normRight = rc.normLeft + totalSpan * (static_cast<float>(k + 1) / static_cast<float>(cps.size()));
                output.chars.push_back(std::move(sub));
            }
        }
    }

    output.confidence = probSum / static_cast<float>(rawChars.size());
    return output;
}

std::vector<cv::Rect> DeepOcrEngine::projectCharSpansToImage(
    const std::string& lineText,
    const std::vector<CtcDecodedChar>& decodedChars,
    int cropWidth,
    int cropHeight,
    bool rotated180,
    bool rotatedVertical,
    const cv::Mat& inversePerspective,
    const cv::Rect& fallbackLineBox,
    int maxImageWidth,
    int maxImageHeight) {
    const size_t expectedCodepoints = OcrEngine::countUtf8Codepoints(lineText);
    if (expectedCodepoints == 0) return {};

    if (decodedChars.size() != expectedCodepoints || cropWidth <= 0 || cropHeight <= 0) {
        return OcrEngine::buildProportionalCharBoxes(lineText, fallbackLineBox);
    }

    // 轴对齐水平框零矩阵快速精确映射：直接按 CTC 时间步区间投影至原图物理像素包围盒
    if (inversePerspective.empty() || inversePerspective.rows != 3 || inversePerspective.cols != 3) {
        if (!rotatedVertical && fallbackLineBox.width > 0 && fallbackLineBox.height > 0) {
            std::vector<cv::Rect> fastBoxes;
            fastBoxes.reserve(decodedChars.size());
            const float baseLeft = static_cast<float>(fallbackLineBox.x);
            const float spanW = static_cast<float>(fallbackLineBox.width);
            for (const auto& dc : decodedChars) {
                float u0 = std::clamp(dc.normLeft, 0.0f, 1.0f);
                float u1 = std::clamp(dc.normRight, u0, 1.0f);
                if (rotated180) {
                    const float nu0 = 1.0f - u1;
                    const float nu1 = 1.0f - u0;
                    u0 = nu0;
                    u1 = nu1;
                }
                int rx1 = static_cast<int>(std::floor(baseLeft + u0 * spanW));
                int rx2 = static_cast<int>(std::ceil(baseLeft + u1 * spanW));
                int ry1 = fallbackLineBox.y;
                int ry2 = fallbackLineBox.y + fallbackLineBox.height;
                if (maxImageWidth > 0 && maxImageHeight > 0) {
                    rx1 = std::clamp(rx1, 0, maxImageWidth - 1);
                    ry1 = std::clamp(ry1, 0, maxImageHeight - 1);
                    rx2 = std::clamp(rx2, rx1 + 1, maxImageWidth);
                    ry2 = std::clamp(ry2, ry1 + 1, maxImageHeight);
                } else {
                    rx2 = (std::max)(rx1 + 1, rx2);
                    ry2 = (std::max)(ry1 + 1, ry2);
                }
                fastBoxes.emplace_back(rx1, ry1, rx2 - rx1, ry2 - ry1);
            }
            return fastBoxes;
        }
        return OcrEngine::buildProportionalCharBoxes(lineText, fallbackLineBox);
    }

    std::vector<cv::Rect> charBoxes;
    charBoxes.reserve(decodedChars.size());

    // 注意: 若竖排框经历过逆时针 90° 旋转，原正交拉平框尺寸为 (origCropW = cropHeight, origCropH = cropWidth)
    const float unrotW = rotatedVertical ? static_cast<float>(cropHeight) : static_cast<float>(cropWidth);
    const float unrotH = rotatedVertical ? static_cast<float>(cropWidth) : static_cast<float>(cropHeight);

    for (const auto& dc : decodedChars) {
        float u0 = std::clamp(dc.normLeft, 0.0f, 1.0f);
        float u1 = std::clamp(dc.normRight, u0, 1.0f);
        if (rotated180) {
            const float nu0 = 1.0f - u1;
            const float nu1 = 1.0f - u0;
            u0 = nu0;
            u1 = nu1;
        }

        std::vector<cv::Point2f> localQuad(4);
        if (!rotatedVertical) {
            const float x0 = u0 * unrotW;
            const float x1 = (std::max)(x0 + 1.0f, u1 * unrotW);
            localQuad[0] = {x0, 0.0f};
            localQuad[1] = {x1, 0.0f};
            localQuad[2] = {x1, unrotH};
            localQuad[3] = {x0, unrotH};
        } else {
            // 竖排经过 90° CCW，横轴 u 对应旋转前从底到顶或从顶到底的纵轴区间
            const float y0 = (1.0f - u1) * unrotH;
            const float y1 = (std::max)(y0 + 1.0f, (1.0f - u0) * unrotH);
            localQuad[0] = {0.0f, y0};
            localQuad[1] = {unrotW, y0};
            localQuad[2] = {unrotW, y1};
            localQuad[3] = {0.0f, y1};
        }

        std::vector<cv::Point2f> imgQuad(4);
        cv::perspectiveTransform(localQuad, imgQuad, inversePerspective);

        float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
        for (const auto& p : imgQuad) {
            minX = (std::min)(minX, p.x);
            minY = (std::min)(minY, p.y);
            maxX = (std::max)(maxX, p.x);
            maxY = (std::max)(maxY, p.y);
        }

        int rx1 = static_cast<int>(std::floor(minX));
        int ry1 = static_cast<int>(std::floor(minY));
        int rx2 = static_cast<int>(std::ceil(maxX));
        int ry2 = static_cast<int>(std::ceil(maxY));

        if (maxImageWidth > 0 && maxImageHeight > 0) {
            rx1 = std::clamp(rx1, 0, maxImageWidth - 1);
            ry1 = std::clamp(ry1, 0, maxImageHeight - 1);
            rx2 = std::clamp(rx2, rx1 + 1, maxImageWidth);
            ry2 = std::clamp(ry2, ry1 + 1, maxImageHeight);
        } else {
            rx2 = (std::max)(rx1 + 1, rx2);
            ry2 = (std::max)(ry1 + 1, ry2);
        }

        charBoxes.emplace_back(rx1, ry1, rx2 - rx1, ry2 - ry1);
    }

    return charBoxes;
}

std::vector<OcrResult> DeepOcrEngine::extractText(const cv::Mat& image) {
    tools3000::core::TraceId::Scope traceScope;
    if (image.empty()) return {};

    bool installed = false;
    const std::filesystem::path modelDir = resolveModelDirectory(&installed);
    if (!installed) return {};

    const cv::Mat bgr = ensureBgr3(image);
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
    try {
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
            cv::resize(bgr, detValid, cv::Size(validW, validH), 0.0, 0.0,
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
            cv::resize(bgr, detResized, cv::Size(alignW, alignH), 0.0, 0.0,
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

        // 2. 轴对齐零拷贝切片 + 光学字宽自适应归一化 + 单趟列方差切分 + FFD 降序最佳装箱
        constexpr int kBatchStripW = DeepOcrConstants::kRecFixedStripWidth;
        constexpr int kStripLeadPad = 6;
        constexpr int kStripEdgeExtend = 6;
        constexpr int kStripSepTotal = 24; // 6px 右延展 + 12px 中性零激活死区 + 6px 左延展
        constexpr int kMaxPieceW = kBatchStripW - kStripLeadPad * 2; // 948

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
            float uOffset = 0.0f;
            float uScale = 1.0f;
            cv::Mat resizedCrop; // 48 x resizedW
            int resizedW = 0;
        };

        // 弱对比度屏摄/反光灰字条带自适应提纯：
        // 仅在送入 SVTR 前的 48px 高度条带灰度动态范围 (P95 - P05) 落在 [16, 112) 时生效，
        // 通过查表平滑拉伸动态范围（增益上限 2.35x），正常高对比度截图 (>=112) 100% 零改动直通。
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
            if (dynRange < 16 || dynRange >= 112) return;

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

        std::vector<QuadMeta> quadMetas(quads.size());
        std::vector<RecPiece> pieces;
        pieces.reserve(quads.size() + 16);

        for (size_t qi = 0; qi < quads.size(); ++qi) {
            const auto& q = quads[qi];
            float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
            for (const auto& pt : q.corners) {
                minX = (std::min)(minX, pt.x);
                minY = (std::min)(minY, pt.y);
                maxX = (std::max)(maxX, pt.x);
                maxY = (std::max)(maxY, pt.y);
            }
            const int bx1 = std::clamp(static_cast<int>(std::floor(minX)), 0, origW - 1);
            const int by1 = std::clamp(static_cast<int>(std::floor(minY)), 0, origH - 1);
            const int bx2 = std::clamp(static_cast<int>(std::ceil(maxX)), bx1 + 1, origW);
            const int by2 = std::clamp(static_cast<int>(std::ceil(maxY)), by1 + 1, origH);
            const cv::Rect lineBox(bx1, by1, bx2 - bx1, by2 - by1);

            const float topSkew = std::abs(q.corners[1].y - q.corners[0].y);
            const float botSkew = std::abs(q.corners[2].y - q.corners[3].y);
            const float leftSkew = std::abs(q.corners[3].x - q.corners[0].x);
            const float rightSkew = std::abs(q.corners[2].x - q.corners[1].x);
            const bool isNearAxisAlignedHoriz =
                topSkew <= DeepOcrConstants::kAxisAlignedMaxSkewPx &&
                botSkew <= DeepOcrConstants::kAxisAlignedMaxSkewPx &&
                leftSkew <= DeepOcrConstants::kAxisAlignedMaxSkewPx &&
                rightSkew <= DeepOcrConstants::kAxisAlignedMaxSkewPx &&
                lineBox.height < static_cast<int>(static_cast<float>(lineBox.width) * 1.45f);

            cv::Mat invPersp;
            bool rotatedVertical = false;
            cv::Mat crop;
            if (isNearAxisAlignedHoriz) {
                crop = bgr(lineBox);
            } else {
                crop = warpPerspectiveQuad(bgr, q.corners, &invPersp, &rotatedVertical);
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

            // 严格保持纵横比缩放至识别网络标准高度 (48px)：
            // 彻底根治高像素手机原图（如 4284x5712、单行高 160~220px）因下限钳制导致的横向畸形拉扁失真与错误切碎。
            const float cropRowsF = static_cast<float>((std::max)(1, crop.rows));
            const float horizScale = static_cast<float>(DeepOcrConstants::kRecImgHeight) / cropRowsF;
            const int fullResizedW = static_cast<int>(std::ceil(static_cast<float>(crop.cols) * horizScale));

            if (fullResizedW <= kMaxPieceW) {
                const int rw = std::clamp(fullResizedW, 8, kMaxPieceW);
                cv::Mat rc;
                cv::resize(crop, rc, cv::Size(rw, DeepOcrConstants::kRecImgHeight), 0.0, 0.0,
                           crop.rows < DeepOcrConstants::kRecImgHeight ? cv::INTER_CUBIC : cv::INTER_AREA);
                enhanceLowContrastStripCrop(rc);
                pieces.push_back(RecPiece{qi, 0, 0.0f, 1.0f, std::move(rc), rw});
            } else {
                cv::Mat grayCrop;
                cv::cvtColor(crop, grayCrop, cv::COLOR_BGR2GRAY);
                const int maxSliceCropW = (std::max)(
                    24,
                    static_cast<int>(std::floor(static_cast<float>(kMaxPieceW - 12) / horizScale)));

                // 单趟连续指针扫描预计算每列方差，O(W*H) 零 OpenCV 虚函数分配开销
                const int gRows = grayCrop.rows;
                const int gCols = grayCrop.cols;
                std::vector<uint32_t> colSum(static_cast<size_t>(gCols), 0);
                std::vector<uint32_t> colSumSq(static_cast<size_t>(gCols), 0);
                for (int r = 0; r < gRows; ++r) {
                    const uint8_t* gPtr = grayCrop.ptr<uint8_t>(r);
                    for (int c = 0; c < gCols; ++c) {
                        const uint32_t v = gPtr[c];
                        colSum[static_cast<size_t>(c)] += v;
                        colSumSq[static_cast<size_t>(c)] += v * v;
                    }
                }
                const double invRows = 1.0 / static_cast<double>((std::max)(1, gRows));

                int segStart = 0;
                int subOrd = 0;
                while (segStart < crop.cols) {
                    int segEnd = crop.cols;
                    if (crop.cols - segStart > maxSliceCropW) {
                        const int idealCut = segStart + maxSliceCropW;
                        const int searchL = (std::max)(segStart + maxSliceCropW / 2, idealCut - crop.rows * 2);
                        const int searchR = (std::min)(crop.cols - 6, idealCut);
                        int bestCut = idealCut;
                        double minColVar = 1e18;
                        for (int x = searchL; x <= searchR; ++x) {
                            const double s = static_cast<double>(colSum[static_cast<size_t>(x)]);
                            const double sq = static_cast<double>(colSumSq[static_cast<size_t>(x)]);
                            const double var = sq * invRows - (s * invRows) * (s * invRows);
                            if (var < minColVar) {
                                minColVar = var;
                                bestCut = x;
                            }
                        }
                        segEnd = bestCut;
                    }

                    const int segW = segEnd - segStart;
                    cv::Mat subCrop = crop(cv::Rect(segStart, 0, segW, crop.rows));
                    const int rw = std::clamp(
                        static_cast<int>(std::ceil(static_cast<float>(segW) * horizScale)),
                        8, kMaxPieceW);
                    cv::Mat rc;
                    cv::resize(subCrop, rc, cv::Size(rw, DeepOcrConstants::kRecImgHeight), 0.0, 0.0,
                               crop.rows < DeepOcrConstants::kRecImgHeight ? cv::INTER_CUBIC : cv::INTER_AREA);
                    enhanceLowContrastStripCrop(rc);
                    pieces.push_back(RecPiece{
                        qi,
                        subOrd++,
                        static_cast<float>(segStart) / static_cast<float>(crop.cols),
                        static_cast<float>(segW) / static_cast<float>(crop.cols),
                        std::move(rc),
                        rw
                    });
                    segStart = segEnd;
                }
            }
        }

        if (pieces.empty()) {
            return {};
        }

        struct StripSlot {
            size_t pieceIdx = 0;
            int stripX0 = 0;
            int stripX1 = 0;
            int resizedW = 0;
        };

        struct PackedStrip {
            cv::Mat stripBgr;
            int contentW = 0;
            std::vector<StripSlot> slots;
        };

        // First-Fit Decreasing (FFD 降序最佳适应装箱)：消除长行尾块碎片化，条带装载率提升至 92%+
        std::vector<size_t> ffdOrder(pieces.size());
        std::iota(ffdOrder.begin(), ffdOrder.end(), 0);
        std::stable_sort(ffdOrder.begin(), ffdOrder.end(), [&](size_t a, size_t b) {
            return pieces[a].resizedW > pieces[b].resizedW;
        });

        struct OpenBin {
            int cursor = kStripLeadPad;
            std::vector<StripSlot> slots;
        };
        std::vector<OpenBin> openBins;
        openBins.reserve((pieces.size() / 2) + 2);
        for (size_t pi : ffdOrder) {
            const int rw = pieces[pi].resizedW;
            bool placed = false;
            for (auto& bin : openBins) {
                const int x0 = bin.slots.empty() ? bin.cursor : (bin.cursor + kStripSepTotal);
                if (x0 + rw + kStripLeadPad <= kBatchStripW) {
                    bin.slots.push_back(StripSlot{pi, x0, x0 + rw, rw});
                    bin.cursor = x0 + rw;
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                OpenBin nb;
                nb.slots.reserve(8);
                nb.slots.push_back(StripSlot{pi, kStripLeadPad, kStripLeadPad + rw, rw});
                nb.cursor = kStripLeadPad + rw;
                openBins.push_back(std::move(nb));
            }
        }

        std::vector<PackedStrip> strips;
        strips.reserve(openBins.size());
        for (auto& bin : openBins) {
            if (bin.slots.empty()) continue;
            const int contentW = (std::min)(kBatchStripW, bin.cursor + kStripLeadPad);
            cv::Mat stripBgr(DeepOcrConstants::kRecImgHeight, contentW, CV_8UC3, cv::Scalar(128, 128, 128));
            for (size_t i = 0; i < bin.slots.size(); ++i) {
                const auto& sl = bin.slots[i];
                const auto& pc = pieces[sl.pieceIdx];
                if (sl.stripX0 < 0 || sl.stripX1 > contentW) continue;
                pc.resizedCrop.copyTo(stripBgr(cv::Rect(sl.stripX0, 0, sl.resizedW, DeepOcrConstants::kRecImgHeight)));

                const int leftPad = (i == 0) ? kStripLeadPad : kStripEdgeExtend;
                if (leftPad > 0 && sl.stripX0 >= leftPad) {
                    cv::Mat leftCol = pc.resizedCrop.col(0);
                    for (int px = sl.stripX0 - leftPad; px < sl.stripX0; ++px) {
                        leftCol.copyTo(stripBgr.col(px));
                    }
                }
                const int rightPad = (i + 1 == bin.slots.size()) ? (contentW - sl.stripX1) : kStripEdgeExtend;
                if (rightPad > 0 && sl.stripX1 + rightPad <= contentW) {
                    cv::Mat rightCol = pc.resizedCrop.col(sl.resizedW - 1);
                    for (int px = sl.stripX1; px < sl.stripX1 + rightPad; ++px) {
                        rightCol.copyTo(stripBgr.col(px));
                    }
                }
            }
            strips.push_back(PackedStrip{std::move(stripBgr), contentW, std::move(bin.slots)});
        }
        const auto tRecStart = std::chrono::steady_clock::now();
        packMs = std::chrono::duration_cast<std::chrono::milliseconds>(tRecStart - tTotalStart).count() - detMs;
        packedStripCount = strips.size();

        auto scatterStripDecodedToQuads = [&](const PackedStrip& st, const CtcDecodeOutput& stripDecoded) {
            if (stripDecoded.chars.empty() || st.slots.empty()) return;

            if (st.slots.size() == 1) {
                const auto& sl = st.slots[0];
                const auto& pc = pieces[sl.pieceIdx];
                auto& qm = quadMetas[pc.quadIdx];
                const float sw = (std::max)(1.0f, static_cast<float>(sl.resizedW));
                const float sx0 = static_cast<float>(sl.stripX0);
                std::vector<CtcDecodedChar> chars;
                chars.reserve(stripDecoded.chars.size());
                for (const auto& dc : stripDecoded.chars) {
                    const float pxL = dc.normLeft * static_cast<float>(st.contentW);
                    const float pxR = dc.normRight * static_cast<float>(st.contentW);
                    CtcDecodedChar mapped = dc;
                    mapped.normLeft = std::clamp((pxL - sx0) / sw, 0.0f, 1.0f);
                    mapped.normRight = std::clamp((pxR - sx0) / sw, mapped.normLeft + 0.005f, 1.0f);
                    mapped.normLeft = pc.uOffset + mapped.normLeft * pc.uScale;
                    mapped.normRight = pc.uOffset + mapped.normRight * pc.uScale;
                    chars.push_back(std::move(mapped));
                }
                while (!chars.empty() && chars.front().utf8Char == " ") chars.erase(chars.begin());
                while (!chars.empty() && chars.back().utf8Char == " ") chars.pop_back();
                if (!chars.empty()) {
                    qm.segOutputs.emplace_back(pc.subOrder, std::move(chars));
                }
                return;
            }

            std::vector<std::vector<CtcDecodedChar>> slotChars(st.slots.size());
            for (const auto& dc : stripDecoded.chars) {
                const float pxL = dc.normLeft * static_cast<float>(st.contentW);
                const float pxR = dc.normRight * static_cast<float>(st.contentW);
                const float pxMid = 0.5f * (pxL + pxR);

                int bestSlot = -1;
                float bestDist = FLT_MAX;
                for (size_t i = 0; i < st.slots.size(); ++i) {
                    const float sx0 = static_cast<float>(st.slots[i].stripX0);
                    const float sx1 = static_cast<float>(st.slots[i].stripX1);
                    if (pxMid >= sx0 - 10.0f && pxMid <= sx1 + 10.0f) {
                        const float dist = (pxMid < sx0) ? (sx0 - pxMid) : ((pxMid > sx1) ? (pxMid - sx1) : 0.0f);
                        if (dist < bestDist) {
                            bestDist = dist;
                            bestSlot = static_cast<int>(i);
                        }
                    }
                }
                if (bestSlot < 0) continue;

                const auto& targetSlot = st.slots[static_cast<size_t>(bestSlot)];
                const float sx0 = static_cast<float>(targetSlot.stripX0);
                const float sx1 = static_cast<float>(targetSlot.stripX1);
                if (dc.utf8Char == " " && (pxMid < sx0 + 2.0f || pxMid > sx1 - 2.0f)) {
                    continue;
                }

                const float sw = (std::max)(1.0f, static_cast<float>(targetSlot.resizedW));
                CtcDecodedChar mapped = dc;
                mapped.normLeft = std::clamp((pxL - sx0) / sw, 0.0f, 1.0f);
                mapped.normRight = std::clamp((pxR - sx0) / sw, mapped.normLeft + 0.005f, 1.0f);
                slotChars[static_cast<size_t>(bestSlot)].push_back(std::move(mapped));
            }

            for (size_t i = 0; i < st.slots.size(); ++i) {
                auto& chars = slotChars[i];
                while (!chars.empty() && chars.front().utf8Char == " ") chars.erase(chars.begin());
                while (!chars.empty() && chars.back().utf8Char == " ") chars.pop_back();
                if (chars.empty()) continue;

                const auto& pc = pieces[st.slots[i].pieceIdx];
                auto& qm = quadMetas[pc.quadIdx];
                for (auto& ch : chars) {
                    ch.normLeft = pc.uOffset + ch.normLeft * pc.uScale;
                    ch.normRight = pc.uOffset + ch.normRight * pc.uScale;
                }
                qm.segOutputs.emplace_back(pc.subOrder, std::move(chars));
            }
        };

        const int H = DeepOcrConstants::kRecImgHeight;
        if (m_useWinMl && m_winMl) {
            std::vector<float> hostInBuf;
            std::vector<float> hostOutBuf;
            size_t si = 0;
            while (si < strips.size()) {
                const size_t rem = strips.size() - si;
                size_t bucketIdx = 1;
                if (m_winMl->supportsBatchEval) {
                    if (rem >= 18) {
                        bucketIdx = 3; // 32 x 960
                    } else if (rem >= 7) {
                        bucketIdx = 2; // 16 x 960
                    } else if (rem == 1 && strips[si].contentW <= 640) {
                        bucketIdx = 0; // 1 x 640
                    } else {
                        bucketIdx = 1; // 4 x 960
                    }
                } else {
                    bucketIdx = (strips[si].contentW <= 640) ? 0 : 1;
                }

                waml::LearningModelSession* recSess = m_winMl->ensureRecSession(bucketIdx);
                if (!recSess) {
                    break;
                }

                const int batchCap = m_winMl->supportsBatchEval
                    ? WinMlContext::kRecSpecs[bucketIdx].n
                    : 1;
                const int netW = (bucketIdx == 0) ? 640 : kBatchStripW;
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
                            const auto& st = strips[si + static_cast<size_t>(b)];
                            float* sampleBase = basePtr + static_cast<size_t>(b) * sampleFloats;
                            const int copyW = (std::min)(st.contentW, netW);
                            float* c0 = sampleBase;
                            float* c1 = sampleBase + planeFloats;
                            float* c2 = sampleBase + planeFloats * 2;
                            for (int y = 0; y < H; ++y) {
                                const auto* row = st.stripBgr.ptr<cv::Vec3b>(y);
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
                            // 关键硬件加速优化：DirectML 输出张量映射区为 D3D12 Readback 非缓存内存 (Uncached/WC)，
                            // 先以整块流式 std::memcpy 突发拉取至 CPU L3-Cached 连续缓冲区，杜绝 7000 万次标量随机读总线惩罚
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
                                const auto& st = strips[si + static_cast<size_t>(b)];
                                const float validRatio = static_cast<float>(st.contentW) / static_cast<float>(netW);
                                cv::Mat logits2d(seqT, numClasses, CV_32FC1, cachedHostPtr + static_cast<size_t>(b) * oneOutFloats);
                                batchDecoded[static_cast<size_t>(b)] = decodeCtcGreedy(logits2d, m_dictKeys, validRatio);
                            }
                        });

                        for (int b = 0; b < actualB; ++b) {
                            scatterStripDecodedToQuads(strips[si + static_cast<size_t>(b)], batchDecoded[static_cast<size_t>(b)]);
                        }
                    }
                } catch (...) {
                }

                si += static_cast<size_t>(actualB);
            }
        } else {
            for (const auto& st : strips) {
                const bool useShort = (st.contentW <= DeepOcrConstants::kRecShortBucketWidth);
                const int netW = useShort
                    ? DeepOcrConstants::kRecShortBucketWidth
                    : DeepOcrConstants::kRecLongBucketWidth;
                cv::dnn::Net& activeNet = useShort ? m_recNetShort : m_recNetLong;
                cv::Mat recBlob = makeRecOrClsBlob(st.stripBgr, H, st.contentW, netW);
                activeNet.setInput(recBlob);
                cv::Mat recOut = activeNet.forward();
                if (recOut.empty()) continue;
                int seqT = (recOut.dims >= 3) ? recOut.size[1] : recOut.rows;
                int numClasses = (recOut.dims >= 3) ? recOut.size[2] : recOut.cols;
                if (seqT <= 0 || numClasses <= 1) continue;
                cv::Mat logits2d(seqT, numClasses, CV_32F, recOut.ptr<float>());
                const float validRatio = static_cast<float>(st.contentW) / static_cast<float>(netW);
                const CtcDecodeOutput dec = decodeCtcGreedy(logits2d, m_dictKeys, validRatio);
                scatterStripDecodedToQuads(st, dec);
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
            for (auto& seg : qm.segOutputs) {
                for (auto& ch : seg.second) {
                    qm.text += ch.utf8Char;
                    qm.confWeightedSum += ch.confidence;
                    ++qm.totalChars;
                    qm.chars.push_back(std::move(ch));
                }
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
    } catch (const std::exception& e) {
        LOG_ERROR("DeepOcrEngine: inference exception: {}", e.what());
        return {};
    }

    if (rawBlocks.empty()) {
        return {};
    }

    // 3. 统一委托至 OcrEngine 版面感知内核（竖向分割线检测 + 递归 XY-Cut 分区 + 严苛同行基线对齐锁）
    auto assembled = OcrEngine::assembleRecognizedBlocksWithLayout(std::move(rawBlocks), image);
    const long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tTotalStart).count();
    LOG_INFO("DeepOcrEngine: recognized {} lines ({} quads -> {} strips) on {}x{} in {} ms (det={} ms, pack={} ms, rec={} ms)",
             assembled.size(), detectedQuadCount, packedStripCount,
             bgr.cols, bgr.rows, totalMs, detMs, packMs, recMs);
    return assembled;
}

DeepOcrModelStatus DeepOcrEngine::getModelStatus() const {
    DeepOcrModelStatus st;
    bool installed = false;
    const auto dir = resolveModelDirectory(&installed);
    st.installed = installed;
    st.modelDir = tools3000::core::WinUtils::wstringToUtf8(dir.wstring());
    st.downloading = m_downloading.load(std::memory_order_acquire);
    st.progressPercent = m_progressPercent.load(std::memory_order_acquire);
    st.downloadedBytes = m_downloadedBytes.load(std::memory_order_acquire);
    st.totalBytes = m_totalBytes.load(std::memory_order_acquire);

    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        st.currentFile = m_currentDownloadFile;
        st.error = m_lastDownloadError;
    }

    if (st.downloading) {
        st.state = "downloading";
    } else if (st.installed) {
        st.state = "installed";
        st.progressPercent = 100;
    } else if (!st.error.empty()) {
        st.state = "error";
    } else {
        st.state = "not_installed";
    }
    return st;
}

bool DeepOcrEngine::startModelDownload() {
    if (m_downloading.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }

    const std::filesystem::path targetDir = defaultInstallDirectory();
    m_cancelRequested.store(false, std::memory_order_release);
    m_progressPercent.store(0, std::memory_order_release);
    m_downloadedBytes.store(0, std::memory_order_release);
    m_totalBytes.store(DeepOcrConstants::kEstimatedBundleBytes, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        m_currentDownloadFile.clear();
        m_lastDownloadError.clear();
        if (m_downloadThread.joinable()) {
            m_downloadThread.join();
        }
        m_downloadThread = std::jthread([this, targetDir](std::stop_token st) {
            downloadWorkerLoop(st, targetDir);
        });
    }
    return true;
}

void DeepOcrEngine::cancelModelDownload() {
    m_cancelRequested.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_downloadMutex);
    if (m_downloadThread.joinable()) {
        m_downloadThread.request_stop();
    }
}

void DeepOcrEngine::downloadWorkerLoop(std::stop_token stopToken, std::filesystem::path targetDir) {
    tools3000::core::TraceId::Scope traceScope;
    std::error_code ec;
    std::filesystem::create_directories(targetDir, ec);
    if (ec) {
        {
            std::lock_guard<std::mutex> lock(m_downloadMutex);
            m_lastDownloadError = "Failed to create model directory";
            m_currentDownloadFile.clear();
        }
        m_downloading.store(false, std::memory_order_release);
        return;
    }

    uint64_t cumulativeBytes = 0;
    bool allSucceeded = true;
    std::string failReason;

    for (size_t idx = 0; idx < kModelSpecs.size(); ++idx) {
        if (stopToken.stop_requested() || m_cancelRequested.load(std::memory_order_acquire)) {
            allSucceeded = false;
            failReason = "cancelled";
            break;
        }

        const auto& spec = kModelSpecs[idx];
        {
            std::lock_guard<std::mutex> lock(m_downloadMutex);
            m_currentDownloadFile = spec.displayName;
        }

        const std::filesystem::path finalPath = targetDir / spec.localFileName;
        const std::filesystem::path partPath = targetDir / (std::wstring(spec.localFileName) + L".part");

        auto verifyCandidatePath = [&](const std::filesystem::path& candidatePath, uint64_t* outByteLen = nullptr) -> bool {
            if (spec.isOnnx) {
                std::vector<uint8_t> verifyBytes;
                if (!readFileBytes(candidatePath, verifyBytes) ||
                    !isValidOnnxBuffer(verifyBytes.data(), verifyBytes.size(), spec.minValidBytes)) {
                    return false;
                }
                try {
                    cv::dnn::Net probeNet = cv::dnn::readNetFromONNX(
                        reinterpret_cast<const char*>(verifyBytes.data()), verifyBytes.size());
                    if (probeNet.empty()) return false;
                    if (outByteLen) *outByteLen = static_cast<uint64_t>(verifyBytes.size());
                    return true;
                } catch (...) {
                    return false;
                }
            } else {
                std::vector<std::string> probeKeys;
                if (!loadDictionaryLines(candidatePath, probeKeys) ||
                    probeKeys.size() < DeepOcrConstants::kMinDictKeyLines) {
                    return false;
                }
                if (outByteLen) {
                    std::error_code szEc;
                    const auto sz = std::filesystem::file_size(candidatePath, szEc);
                    *outByteLen = szEc ? 0ULL : static_cast<uint64_t>(sz);
                }
                return true;
            }
        };

        // 断点续传加速：若本地目标文件已完整且通过 ONNX 图解析/字典校验，直接复用跳过重复下载
        uint64_t existingValidBytes = 0;
        if (verifyCandidatePath(finalPath, &existingValidBytes)) {
            cumulativeBytes += existingValidBytes;
            m_downloadedBytes.store(cumulativeBytes, std::memory_order_release);
            const uint64_t estTotal = (std::max)(cumulativeBytes + 65536ULL, DeepOcrConstants::kEstimatedBundleBytes);
            m_totalBytes.store(estTotal, std::memory_order_release);
            m_progressPercent.store(
                std::clamp(static_cast<int>((cumulativeBytes * 98ULL) / estTotal), 1, 98),
                std::memory_order_release);
            continue;
        }

        bool fileDone = false;

        // 字典文件零网络内嵌自提取优先：直接从已落盘的 ch_PP-OCRv4_rec.onnx 元数据导出官方字符表
        if (!spec.isOnnx) {
            std::vector<uint8_t> recModelBytes;
            std::vector<std::string> embeddedKeys;
            if (readFileBytes(targetDir / DeepOcrConstants::kRecModelFileName, recModelBytes) &&
                extractEmbeddedCharacterDictFromOnnx(recModelBytes.data(), recModelBytes.size(), embeddedKeys) &&
                embeddedKeys.size() >= DeepOcrConstants::kMinDictKeyLines) {
                std::filesystem::remove(partPath, ec);
                std::ofstream dictOut(partPath, std::ios::binary | std::ios::trunc);
                if (dictOut) {
                    for (const auto& k : embeddedKeys) {
                        dictOut.write(k.data(), static_cast<std::streamsize>(k.size()));
                        dictOut.put('\n');
                    }
                    dictOut.flush();
                    const auto writtenPos = dictOut.tellp();
                    dictOut.close();
                    if (writtenPos > 0 &&
                        MoveFileExW(partPath.c_str(), finalPath.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
                        cumulativeBytes += static_cast<uint64_t>(writtenPos);
                        m_downloadedBytes.store(cumulativeBytes, std::memory_order_release);
                        fileDone = true;
                        LOG_INFO("DeepOcrEngine: generated {} from embedded ONNX metadata ({} keys)",
                                 spec.displayName, embeddedKeys.size());
                    } else {
                        std::filesystem::remove(partPath, ec);
                    }
                }
            }
        }

        for (const wchar_t* mirrorUrl : spec.mirrorUrls) {
            if (fileDone) break;
            if (!mirrorUrl) continue;
            if (stopToken.stop_requested() || m_cancelRequested.load(std::memory_order_acquire)) {
                break;
            }

            std::filesystem::remove(partPath, ec);
            const uint64_t baseBeforeFile = cumulativeBytes;
            uint64_t fileProgressBytes = 0;

            const bool dlOk = downloadHttpsUrlToFile(
                mirrorUrl, partPath, stopToken, m_cancelRequested,
                [&](uint64_t delta, uint64_t /*contentLen*/) {
                    fileProgressBytes += delta;
                    const uint64_t curTotal = baseBeforeFile + fileProgressBytes;
                    m_downloadedBytes.store(curTotal, std::memory_order_release);
                    const uint64_t estTotal = (std::max)(curTotal + 65536ULL, DeepOcrConstants::kEstimatedBundleBytes);
                    m_totalBytes.store(estTotal, std::memory_order_release);
                    const int pct = std::clamp(
                        static_cast<int>((curTotal * 98ULL) / estTotal),
                        1, 98);
                    m_progressPercent.store(pct, std::memory_order_release);
                });

            if (!dlOk) {
                std::filesystem::remove(partPath, ec);
                continue;
            }

            // 严格验证文件完整性 (ONNX 魔数 + OpenCV DNN 试解析，或字典行数校验)
            if (!verifyCandidatePath(partPath)) {
                LOG_WARN("DeepOcrEngine: downloaded file failed integrity check: {}", spec.displayName);
                std::filesystem::remove(partPath, ec);
                continue;
            }

            if (MoveFileExW(partPath.c_str(), finalPath.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
                cumulativeBytes += fileProgressBytes;
                fileDone = true;
                break;
            }
            std::filesystem::remove(partPath, ec);
        }

        if (!fileDone) {
            allSucceeded = false;
            if (stopToken.stop_requested() || m_cancelRequested.load(std::memory_order_acquire)) {
                failReason = "cancelled";
            } else {
                failReason = std::string("Failed to download or verify ") + spec.displayName;
            }
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        m_currentDownloadFile.clear();
        if (allSucceeded) {
            m_lastDownloadError.clear();
            m_progressPercent.store(100, std::memory_order_release);
            LOG_INFO("DeepOcrEngine: PP-OCRv4 offline model bundle installed successfully");
        } else if (failReason == "cancelled") {
            m_lastDownloadError.clear();
            m_progressPercent.store(0, std::memory_order_release);
            LOG_INFO("DeepOcrEngine: model download cancelled by user");
        } else {
            m_lastDownloadError = failReason;
            LOG_WARN("DeepOcrEngine: model download failed: {}", failReason);
        }
    }

    m_downloading.store(false, std::memory_order_release);
    cleanupDeepOcrWorkingSet();
    if (allSucceeded && !m_shuttingDown.load(std::memory_order_acquire)) {
        warmupAsync(100);
    }
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
        const size_t maxRecBucket = m_winMl->supportsBatchEval ? WinMlContext::kRecSpecs.size() : 2;
        for (size_t ri = 0; ri < maxRecBucket; ++ri) {
            if (m_shuttingDown.load(std::memory_order_acquire)) return;
            if (waml::LearningModelSession* rSess = m_winMl->ensureRecSession(ri)) {
                const int bCap = m_winMl->supportsBatchEval ? WinMlContext::kRecSpecs[ri].n : 1;
                const int netW = (ri == 0) ? 640 : DeepOcrConstants::kRecFixedStripWidth;
                auto rTen = waml::TensorFloat::Create(
                    winrt::single_threaded_vector<int64_t>({bCap, 3, DeepOcrConstants::kRecImgHeight, netW}));
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
    cleanupDeepOcrWorkingSet();
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
