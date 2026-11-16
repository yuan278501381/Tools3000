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
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <utility>

namespace tools3000::ocr {

namespace {

struct ModelDownloadSpec {
    const wchar_t* localFileName;
    const char* displayName;
    size_t minValidBytes;
    bool isOnnx;
    std::array<const wchar_t*, 3> mirrorUrls;
};

constexpr std::array<ModelDownloadSpec, 4> kModelSpecs = {{
    {
        DeepOcrConstants::kDetModelFileName,
        "ch_PP-OCRv4_det.onnx",
        DeepOcrConstants::kMinDetModelBytes,
        true,
        {
            L"https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_det_infer.onnx",
            L"https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_det_infer.onnx",
            nullptr
        }
    },
    {
        DeepOcrConstants::kClsModelFileName,
        "ch_ppocr_mobile_v2.0_cls.onnx",
        DeepOcrConstants::kMinClsModelBytes,
        true,
        {
            L"https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_ppocr_mobile_v2.0_cls_infer.onnx",
            L"https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_ppocr_mobile_v2.0_cls_infer.onnx",
            nullptr
        }
    },
    {
        DeepOcrConstants::kRecModelFileName,
        "ch_PP-OCRv4_rec.onnx",
        DeepOcrConstants::kMinRecModelBytes,
        true,
        {
            L"https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_rec_infer.onnx",
            L"https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_rec_infer.onnx",
            nullptr
        }
    },
    {
        DeepOcrConstants::kKeysDictFileName,
        "ppocr_keys_v1.txt",
        4096,
        false,
        {
            L"https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/ppocr/utils/ppocr_keys_v1.txt",
            L"https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ppocr_keys_v1.txt",
            L"https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv4/ppocr_keys_v1.txt"
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
           checkFileMinSize(DeepOcrConstants::kKeysDictFileName, 64);
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

bool DeepOcrEngine::ensureModelsLoadedLocked(const std::filesystem::path& modelDir) {
    if (m_modelsLoaded && m_loadedModelDir == modelDir && !m_detNet.empty() && !m_recNet.empty() && !m_dictKeys.empty()) {
        return true;
    }

    m_modelsLoaded = false;
    m_detNet = cv::dnn::Net{};
    m_clsNet = cv::dnn::Net{};
    m_recNet = cv::dnn::Net{};
    m_dictKeys.clear();

    try {
        std::vector<uint8_t> detBytes, clsBytes, recBytes;
        if (!readFileBytes(modelDir / DeepOcrConstants::kDetModelFileName, detBytes) ||
            !isValidOnnxBuffer(detBytes.data(), detBytes.size(), 256)) {
            LOG_WARN("DeepOcrEngine: invalid or missing det ONNX model");
            return false;
        }
        if (!readFileBytes(modelDir / DeepOcrConstants::kClsModelFileName, clsBytes) ||
            !isValidOnnxBuffer(clsBytes.data(), clsBytes.size(), 256)) {
            LOG_WARN("DeepOcrEngine: invalid or missing cls ONNX model");
            return false;
        }
        if (!readFileBytes(modelDir / DeepOcrConstants::kRecModelFileName, recBytes) ||
            !isValidOnnxBuffer(recBytes.data(), recBytes.size(), 256)) {
            LOG_WARN("DeepOcrEngine: invalid or missing rec ONNX model");
            return false;
        }
        if (!loadDictionaryLines(modelDir / DeepOcrConstants::kKeysDictFileName, m_dictKeys)) {
            LOG_WARN("DeepOcrEngine: invalid or missing dictionary keys file");
            return false;
        }

        m_detNet = cv::dnn::readNetFromONNX(reinterpret_cast<const char*>(detBytes.data()), detBytes.size());
        m_clsNet = cv::dnn::readNetFromONNX(reinterpret_cast<const char*>(clsBytes.data()), clsBytes.size());
        m_recNet = cv::dnn::readNetFromONNX(reinterpret_cast<const char*>(recBytes.data()), recBytes.size());

        if (m_detNet.empty() || m_clsNet.empty() || m_recNet.empty()) {
            LOG_WARN("DeepOcrEngine: OpenCV DNN failed to parse ONNX graphs");
            return false;
        }

        for (cv::dnn::Net* net : {&m_detNet, &m_clsNet, &m_recNet}) {
            net->setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            net->setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        }

        m_loadedModelDir = modelDir;
        m_modelsLoaded = true;
        LOG_INFO("DeepOcrEngine: PP-OCRv4 ONNX models loaded (dict={} keys)", m_dictKeys.size());
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("DeepOcrEngine: exception loading PP-OCRv4 models: {}", e.what());
        m_modelsLoaded = false;
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

    const cv::Mat M = cv::getPerspectiveTransform(srcPts, dstPts);
    if (outInverseTransform) {
        *outInverseTransform = cv::getPerspectiveTransform(dstPts, srcPts);
    }

    cv::Mat warped;
    cv::warpPerspective(image, warped, M, cv::Size(cropW, cropH), cv::INTER_CUBIC, cv::BORDER_REPLICATE);

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

    std::vector<RawSpan> spans;
    spans.reserve(static_cast<size_t>((std::min)(T, 256)));

    int prevIdx = 0;
    for (int t = 0; t < T; ++t) {
        const float* row = mat32f.ptr<float>(t);
        int bestIdx = 0;
        float bestVal = row[0];
        float minVal = row[0];
        for (int c = 1; c < C; ++c) {
            const float v = row[c];
            if (v > bestVal) {
                bestVal = v;
                bestIdx = c;
            }
            if (v < minVal) {
                minVal = v;
            }
        }

        if (bestIdx > 0) {
            float prob = bestVal;
            if (bestVal > 1.001f || minVal < -0.001f) {
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

    const float safeValidRatio = std::clamp(validWidthRatio, 0.05f, 1.0f);
    const float validSteps = (std::max)(1.0f, static_cast<float>(T) * safeValidRatio);

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

    if (decodedChars.size() != expectedCodepoints || inversePerspective.empty() ||
        inversePerspective.rows != 3 || inversePerspective.cols != 3 ||
        cropWidth <= 0 || cropHeight <= 0) {
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

    std::vector<OcrResult> rawBlocks;
    try {
        // 1. DBNet 文本区域检测
        const int origW = bgr.cols;
        const int origH = bgr.rows;
        const int maxSide = (std::max)(origW, origH);
        float limitRatio = 1.0f;
        const int targetLimit = (maxSide > 1600) ? 1280 : DeepOcrConstants::kDetMaxSideLen;
        if (maxSide > targetLimit) {
            limitRatio = static_cast<float>(targetLimit) / static_cast<float>(maxSide);
        } else if (maxSide < 160) {
            limitRatio = 2.0f;
        }

        const int alignW = (std::max)(DeepOcrConstants::kDetStrideAlign,
            static_cast<int>(std::round(static_cast<float>(origW) * limitRatio / 32.0f)) * 32);
        const int alignH = (std::max)(DeepOcrConstants::kDetStrideAlign,
            static_cast<int>(std::round(static_cast<float>(origH) * limitRatio / 32.0f)) * 32);

        const float ratioW = static_cast<float>(alignW) / static_cast<float>(origW);
        const float ratioH = static_cast<float>(alignH) / static_cast<float>(origH);

        cv::Mat detResized;
        cv::resize(bgr, detResized, cv::Size(alignW, alignH), 0.0, 0.0,
                   limitRatio < 1.0f ? cv::INTER_AREA : cv::INTER_LINEAR);

        cv::Mat detBlob = makeDbNetBlob(detResized);
        m_detNet.setInput(detBlob);
        cv::Mat probOut = m_detNet.forward();

        auto quads = postprocessDbMap(probOut, origW, origH, ratioW, ratioH);
        if (quads.empty()) {
            return {};
        }

        rawBlocks.reserve(quads.size());

        // 2. 透视矫正 + 180° 方向分类 + SVTR CTC 识别
        for (const auto& q : quads) {
            cv::Mat invPersp;
            bool rotatedVertical = false;
            cv::Mat crop = warpPerspectiveQuad(bgr, q.corners, &invPersp, &rotatedVertical);
            if (crop.empty() || crop.cols < 2 || crop.rows < 2) continue;

            // 180° 方向分类
            bool rotated180 = false;
            {
                const float clsAspect = static_cast<float>(crop.cols) / static_cast<float>(crop.rows);
                const int clsResizedW = std::clamp(
                    static_cast<int>(std::ceil(static_cast<float>(DeepOcrConstants::kClsImgHeight) * clsAspect)),
                    8, DeepOcrConstants::kClsImgWidth);
                cv::Mat clsBlob = makeRecOrClsBlob(
                    crop, DeepOcrConstants::kClsImgHeight, clsResizedW, DeepOcrConstants::kClsImgWidth);
                m_clsNet.setInput(clsBlob);
                cv::Mat clsOut = m_clsNet.forward();
                if (clsOut.total() >= 2) {
                    const float* p = clsOut.ptr<float>();
                    float p0 = p[0];
                    float p1 = p[1];
                    if (p0 < 0.0f || p1 < 0.0f || (p0 + p1) > 1.02f || (p0 + p1) < 0.98f) {
                        const float m = (std::max)(p0, p1);
                        const float e0 = std::exp(p0 - m);
                        const float e1 = std::exp(p1 - m);
                        p1 = e1 / (e0 + e1);
                    }
                    if (p1 >= DeepOcrConstants::kClsRotate180Thresh) {
                        cv::rotate(crop, crop, cv::ROTATE_180);
                        rotated180 = true;
                    }
                }
            }

            // SVTR / CRNN 识别
            const float recAspect = static_cast<float>(crop.cols) / static_cast<float>(crop.rows);
            const int recResizedW = std::clamp(
                static_cast<int>(std::ceil(static_cast<float>(DeepOcrConstants::kRecImgHeight) * recAspect)),
                16, DeepOcrConstants::kRecMaxWidth);
            const int recPaddedW = (std::max)(
                DeepOcrConstants::kRecMinWidth,
                ((recResizedW + 7) / 8) * 8);

            cv::Mat recBlob = makeRecOrClsBlob(crop, DeepOcrConstants::kRecImgHeight, recResizedW, recPaddedW);
            m_recNet.setInput(recBlob);
            cv::Mat recOut = m_recNet.forward();
            if (recOut.empty()) continue;

            int seqT = 0;
            int numClasses = 0;
            if (recOut.dims == 3) {
                seqT = recOut.size[1];
                numClasses = recOut.size[2];
            } else if (recOut.dims == 2) {
                seqT = recOut.rows;
                numClasses = recOut.cols;
            }
            if (seqT <= 0 || numClasses <= 1) continue;

            cv::Mat logits2d(seqT, numClasses, CV_32F, recOut.ptr<float>());
            const float validRatio = static_cast<float>(recResizedW) / static_cast<float>(recPaddedW);
            CtcDecodeOutput decoded = decodeCtcGreedy(logits2d, m_dictKeys, validRatio);
            if (decoded.text.empty() || decoded.confidence < DeepOcrConstants::kRecMinLineConfidence) {
                continue;
            }

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

            OcrResult item;
            item.text = std::move(decoded.text);
            item.confidence = decoded.confidence;
            item.box = lineBox;
            item.charBoxes = projectCharSpansToImage(
                item.text, decoded.chars, crop.cols, crop.rows,
                rotated180, rotatedVertical, invPersp, lineBox, origW, origH);
            rawBlocks.push_back(std::move(item));
        }
    } catch (const std::exception& e) {
        LOG_ERROR("DeepOcrEngine: inference exception: {}", e.what());
        return {};
    }

    if (rawBlocks.size() <= 1) {
        return rawBlocks;
    }

    // 3. 将同一栏同一视觉行上被拆成多个检测框的列/短语合并，大间距注入 \t，并无缝合并精确 charBoxes
    std::vector<OcrResult> mergedLines;
    mergedLines.reserve(rawBlocks.size());

    for (auto& blk : rawBlocks) {
        if (!mergedLines.empty()) {
            auto& prev = mergedLines.back();
            const float cyPrev = static_cast<float>(prev.box.y) + static_cast<float>(prev.box.height) * 0.5f;
            const float cyCur = static_cast<float>(blk.box.y) + static_cast<float>(blk.box.height) * 0.5f;
            const float refH = (std::max)(8.0f, static_cast<float>((std::min)(prev.box.height, blk.box.height)));
            const int prevRight = prev.box.x + prev.box.width;
            if (std::abs(cyCur - cyPrev) <= refH * 0.52f && blk.box.x >= prevRight - static_cast<int>(refH * 0.4f)) {
                const float gap = static_cast<float>(blk.box.x - prevRight);
                const char sep = (gap > refH * 1.35f) ? '\t' : ' ';
                const cv::Rect sepBox(
                    prevRight,
                    (std::min)(prev.box.y, blk.box.y),
                    (std::max)(1, blk.box.x - prevRight),
                    (std::max)(prev.box.height, blk.box.height));

                prev.text.push_back(sep);
                prev.charBoxes.push_back(sepBox);
                prev.text += blk.text;
                prev.charBoxes.insert(prev.charBoxes.end(), blk.charBoxes.begin(), blk.charBoxes.end());
                prev.confidence = (prev.confidence + blk.confidence) * 0.5f;

                const int nx1 = (std::min)(prev.box.x, blk.box.x);
                const int ny1 = (std::min)(prev.box.y, blk.box.y);
                const int nx2 = (std::max)(prev.box.x + prev.box.width, blk.box.x + blk.box.width);
                const int ny2 = (std::max)(prev.box.y + prev.box.height, blk.box.y + blk.box.height);
                prev.box = cv::Rect(nx1, ny1, nx2 - nx1, ny2 - ny1);
                continue;
            }
        }
        mergedLines.push_back(std::move(blk));
    }

    return mergedLines;
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

        bool fileDone = false;
        for (const wchar_t* mirrorUrl : spec.mirrorUrls) {
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
            bool validArtifact = false;
            if (spec.isOnnx) {
                std::vector<uint8_t> verifyBytes;
                if (readFileBytes(partPath, verifyBytes) &&
                    isValidOnnxBuffer(verifyBytes.data(), verifyBytes.size(), spec.minValidBytes)) {
                    try {
                        cv::dnn::Net probeNet = cv::dnn::readNetFromONNX(
                            reinterpret_cast<const char*>(verifyBytes.data()), verifyBytes.size());
                        validArtifact = !probeNet.empty();
                    } catch (...) {
                        validArtifact = false;
                    }
                }
            } else {
                std::vector<std::string> probeKeys;
                validArtifact = loadDictionaryLines(partPath, probeKeys) &&
                                probeKeys.size() >= DeepOcrConstants::kMinDictKeyLines;
            }

            if (!validArtifact) {
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
}

void DeepOcrEngine::releaseLoadedModels() {
    {
        std::lock_guard<std::mutex> lock(m_inferMutex);
        m_modelsLoaded = false;
        m_loadedModelDir.clear();
        m_detNet = cv::dnn::Net{};
        m_clsNet = cv::dnn::Net{};
        m_recNet = cv::dnn::Net{};
        m_dictKeys.clear();
    }
    cleanupDeepOcrWorkingSet();
}

void DeepOcrEngine::shutdown() {
    m_cancelRequested.store(true, std::memory_order_release);
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
