// ─────────────────────────────────────────────────────────────────────────────
// DeepOcrEngineModel.cpp — PP-OCRv4 模型包寻址、ONNX Protobuf 校验/字典提取/静态维度修补与 WinHTTP 下载器
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/DeepOcrEngineInternal.h"
#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace tools3000::ocr {

using namespace deep_ocr_detail;

namespace deep_ocr_detail {

bool readFileBytes(const std::filesystem::path& filePath, std::vector<uint8_t>& outBytes, size_t maxBytes) {
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

bool isValidOnnxFileHeader(const std::filesystem::path& filePath, size_t minBytes, size_t maxBytes) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(filePath, ec) || ec) return false;
    const auto sz = std::filesystem::file_size(filePath, ec);
    if (ec || sz < minBytes || sz < 16 || sz > maxBytes) return false;
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs) return false;
    constexpr size_t kHeaderProbeBytes = 4096;
    const size_t toRead = static_cast<size_t>((std::min<std::uintmax_t>)(sz, static_cast<std::uintmax_t>(kHeaderProbeBytes)));
    std::array<uint8_t, kHeaderProbeBytes> header{};
    ifs.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(toRead));
    const size_t got = static_cast<size_t>(ifs.gcount());
    if (got < 16) return false;
    return DeepOcrEngine::isValidOnnxBuffer(header.data(), got, (std::min)(minBytes, got));
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

bool isV6BaseBundlePresent(const std::filesystem::path& dir) {
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
    return checkFileMinSize(DeepOcrConstants::kV6DetModelFileName, 256) &&
           checkFileMinSize(DeepOcrConstants::kClsModelFileName, 256) &&
           checkFileMinSize(DeepOcrConstants::kV6RecModelFileName, 256);
}

bool isV4BaseBundlePresent(const std::filesystem::path& dir) {
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

bool isDirectoryCompleteModelBundle(const std::filesystem::path& dir) {
    return isV6BaseBundlePresent(dir) || isV4BaseBundlePresent(dir);
}

bool isServerRecModelPresent(const std::filesystem::path& dir) {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec) || ec) {
        return false;
    }
    const wchar_t* targetName = isV6BaseBundlePresent(dir)
        ? DeepOcrConstants::kV6RecMediumModelFileName
        : DeepOcrConstants::kRecServerModelFileName;
    const auto p = dir / targetName;
    if (!std::filesystem::is_regular_file(p, ec) || ec) {
        return false;
    }
    const auto fsz = std::filesystem::file_size(p, ec);
    return !ec && fsz >= 256;
}

const std::array<const char*, 4>& allScriptPackIds() noexcept {
    static constexpr std::array<const char*, 4> kIds = {"korean", "cyrillic", "arabic", "thai"};
    return kIds;
}

std::string normalizeScriptMode(std::string_view rawMode) {
    std::string lower(rawMode);
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (lower == "cjk" || lower == "zh" || lower == "ch" || lower == "en") return "cjk";
    if (lower == "japan" || lower == "ja" || lower == "jp") return "japan";
    if (lower == "korean" || lower == "ko" || lower == "kr") return "korean";
    if (lower == "cyrillic" || lower == "ru" || lower == "uk" || lower == "bg" || lower == "eslav") return "cyrillic";
    if (lower == "latin" || lower == "es" || lower == "pt" || lower == "pt-br" ||
        lower == "fr" || lower == "de" || lower == "it") return "latin";
    if (lower == "arabic" || lower == "ar" || lower == "fa" || lower == "ur") return "arabic";
    if (lower == "thai" || lower == "th") return "thai";
    return "auto";
}

std::filesystem::path resolveScriptPackModelPath(const std::filesystem::path& dir, std::string_view packId) {
    if (dir.empty()) return {};
    if (packId == "japan")    return dir / DeepOcrConstants::kScriptJapanRecFileName;
    if (packId == "korean")   return dir / DeepOcrConstants::kScriptKoreanRecFileName;
    if (packId == "cyrillic") return dir / DeepOcrConstants::kScriptCyrillicRecFileName;
    if (packId == "latin")    return dir / DeepOcrConstants::kScriptLatinRecFileName;
    if (packId == "arabic")   return dir / DeepOcrConstants::kScriptArabicRecFileName;
    if (packId == "thai")     return dir / DeepOcrConstants::kScriptThaiRecFileName;
    return {};
}

bool isScriptPackModelPresent(const std::filesystem::path& dir, std::string_view packId, uint64_t* outFileBytes) {
    if (outFileBytes) *outFileBytes = 0;
    const auto p = resolveScriptPackModelPath(dir, packId);
    if (p.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec) || ec) {
        return false;
    }
    const auto fsz = std::filesystem::file_size(p, ec);
    if (ec || fsz < DeepOcrConstants::kMinRecModelBytes) {
        return false;
    }
    if (outFileBytes) *outFileBytes = static_cast<uint64_t>(fsz);
    return true;
}

std::filesystem::path ensureOnnxWinMlIr8CompatiblePath(const std::filesystem::path& onnxPath) {
    if (onnxPath.empty()) return onnxPath;
    // 快速 64B 头部零拷贝嗅探：已为 IR<=8 时零整文件读盘开销直通
    std::ifstream probeIfs(onnxPath, std::ios::binary);
    if (!probeIfs) return onnxPath;
    std::vector<uint8_t> head(64, 0);
    probeIfs.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    const size_t got = static_cast<size_t>(probeIfs.gcount());
    probeIfs.close();
    if (got < 16) return onnxPath;
    head.resize(got);
    if (!DeepOcrEngine::clampOnnxIrVersionForWinMl(head)) {
        return onnxPath;
    }

    // 检测到手动放入的 IR>8 (如 IR=10 的 PP-OCRv5/v6) 模型：无损原位钳位至 IR 8，失败则落入临时兼容路径
    std::vector<uint8_t> fullBytes;
    if (!readFileBytes(onnxPath, fullBytes)) {
        return onnxPath;
    }
    if (!DeepOcrEngine::clampOnnxIrVersionForWinMl(fullBytes)) {
        return onnxPath;
    }
    try {
        std::ofstream ofs(onnxPath, std::ios::binary | std::ios::trunc);
        if (ofs) {
            ofs.write(reinterpret_cast<const char*>(fullBytes.data()),
                      static_cast<std::streamsize>(fullBytes.size()));
            ofs.flush();
            if (ofs) return onnxPath;
        }
    } catch (...) {
    }

    std::error_code ec;
    const auto tmpDir = std::filesystem::temp_directory_path(ec);
    if (!ec && !tmpDir.empty()) {
        const auto compatPath = tmpDir / (L"t3k_ocr_" + onnxPath.filename().wstring());
        std::ofstream ofs(compatPath, std::ios::binary | std::ios::trunc);
        if (ofs) {
            ofs.write(reinterpret_cast<const char*>(fullBytes.data()),
                      static_cast<std::streamsize>(fullBytes.size()));
            ofs.flush();
            if (ofs) return compatPath;
        }
    }
    return onnxPath;
}

} // namespace deep_ocr_detail

namespace {

struct ModelDownloadSpec {
    const wchar_t* localFileName;
    const char* displayName;
    size_t minValidBytes;
    bool isOnnx;
    bool requiresEmbeddedDict;
    bool isScriptExtOnnx;
    std::array<const wchar_t*, 4> mirrorUrls;
};

struct ScriptPackDownloadMeta {
    const char* id;
    uint64_t estimatedBytes;
    ModelDownloadSpec spec;
};

// 新一代 PP-OCRv6 Small 核心底座（原生 50 语种：中简/繁/英/日/法/德/西/葡等，全内嵌字典，零外部 txt）
constexpr std::array<ModelDownloadSpec, 3> kModelSpecs = {{
    {
        DeepOcrConstants::kV6DetModelFileName,
        "PP-OCRv6_det_small.onnx",
        DeepOcrConstants::kMinDetModelBytes,
        true,
        false,
        false,
        {
            L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/det/PP-OCRv6_det_small.onnx",
            L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/det/PP-OCRv6_det_small.onnx",
            nullptr,
            nullptr
        }
    },
    {
        DeepOcrConstants::kClsModelFileName,
        "ch_ppocr_mobile_v2.0_cls.onnx",
        DeepOcrConstants::kMinClsModelBytes,
        true,
        false,
        false,
        {
            L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx",
            L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx",
            L"https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv1/ch_ppocr_mobile_v2.0_cls_infer.onnx",
            L"https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv1/ch_ppocr_mobile_v2.0_cls_infer.onnx"
        }
    },
    {
        DeepOcrConstants::kV6RecModelFileName,
        "PP-OCRv6_rec_small.onnx",
        DeepOcrConstants::kMinRecModelBytes,
        true,
        true,
        false,
        {
            L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx",
            L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx",
            nullptr,
            nullptr
        }
    }
}};

// 高精度混搭增强包：仅按需下载 PP-OCRv6 Medium Rec (~73.1 MB)，复用轻量 det_small 省 50MB 体积与 1.75GB Commit
constexpr ModelDownloadSpec kServerRecModelSpec = {
    DeepOcrConstants::kV6RecMediumModelFileName,
    "PP-OCRv6_rec_medium.onnx",
    DeepOcrConstants::kMinRecServerModelBytes,
    true,
    true,
    false,
    {
        L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/rec/PP-OCRv6_rec_medium.onnx",
        L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/rec/PP-OCRv6_rec_medium.onnx",
        nullptr,
        nullptr
    }
};

// 4 大官方 Apache-2.0 精选按需多语种扩展包（与 PP-OCRv6 50 语种互补，覆盖全球 65+ 主流语言）
constexpr std::array<ScriptPackDownloadMeta, 4> kScriptPackSpecs = {{
    {
        "korean",
        DeepOcrConstants::kEstimatedScriptKoreanBytes,
        {
            DeepOcrConstants::kScriptKoreanRecFileName,
            "korean_PP-OCRv5_rec_mobile.onnx",
            DeepOcrConstants::kMinRecModelBytes,
            true,
            true,
            true,
            {
                L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/korean_PP-OCRv5_rec_mobile.onnx",
                L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/korean_PP-OCRv5_rec_mobile.onnx",
                nullptr,
                nullptr
            }
        }
    },
    {
        "cyrillic",
        DeepOcrConstants::kEstimatedScriptCyrillicBytes,
        {
            DeepOcrConstants::kScriptCyrillicRecFileName,
            "cyrillic_PP-OCRv5_rec_mobile.onnx",
            DeepOcrConstants::kMinRecModelBytes,
            true,
            true,
            true,
            {
                L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/cyrillic_PP-OCRv5_rec_mobile.onnx",
                L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/cyrillic_PP-OCRv5_rec_mobile.onnx",
                nullptr,
                nullptr
            }
        }
    },
    {
        "arabic",
        DeepOcrConstants::kEstimatedScriptArabicBytes,
        {
            DeepOcrConstants::kScriptArabicRecFileName,
            "arabic_PP-OCRv5_rec_mobile.onnx",
            DeepOcrConstants::kMinRecModelBytes,
            true,
            true,
            true,
            {
                L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/arabic_PP-OCRv5_rec_mobile.onnx",
                L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/arabic_PP-OCRv5_rec_mobile.onnx",
                nullptr,
                nullptr
            }
        }
    },
    {
        "thai",
        DeepOcrConstants::kEstimatedScriptThaiBytes,
        {
            DeepOcrConstants::kScriptThaiRecFileName,
            "th_PP-OCRv5_rec_mobile.onnx",
            DeepOcrConstants::kMinRecModelBytes,
            true,
            true,
            true,
            {
                L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/th_PP-OCRv5_rec_mobile.onnx",
                L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/th_PP-OCRv5_rec_mobile.onnx",
                nullptr,
                nullptr
            }
        }
    }
}};

const ScriptPackDownloadMeta* findScriptPackMeta(std::string_view id) noexcept {
    for (const auto& item : kScriptPackSpecs) {
        if (id == item.id) return &item;
    }
    return nullptr;
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


} // namespace

std::filesystem::path DeepOcrEngine::defaultInstallDirectory() const {
    return tools3000::core::WinUtils::getAppDataDirectory() / L"models" / L"ocr";
}

void DeepOcrEngine::setCustomModelDirectoryForTest(const std::filesystem::path& customDir) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_pathMutex);
        if (m_customTestModelDir != customDir) {
            m_customTestModelDir = customDir;
            changed = true;
        }
    }
    // 仅在自定义模型目录实际变更时释放已加载模型，避免 Worker 热保活期间同路径请求误销毁已编译 DirectML Session
    if (changed) {
        releaseLoadedModels();
    }
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

std::string DeepOcrEngine::installedEngineGeneration() const {
    bool installed = false;
    const auto dir = resolveModelDirectory(&installed);
    if (!installed) return {};
    if (isV6BaseBundlePresent(dir)) return "v6";
    if (isV4BaseBundlePresent(dir)) return "v4";
    return {};
}

bool DeepOcrEngine::isServerModelInstalled() const {
    bool installed = false;
    const auto dir = resolveModelDirectory(&installed);
    return installed && isServerRecModelPresent(dir);
}

void DeepOcrEngine::setModelTierOverrideForWorker(const std::string& tier) {
    std::lock_guard<std::mutex> lock(m_pathMutex);
    std::string normalized = tier;
    for (char& ch : normalized) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    m_workerModelTierOverride = (normalized == "server" || normalized == "mobile") ? normalized : std::string{};
}

std::string DeepOcrEngine::configuredModelTier() const {
    {
        std::lock_guard<std::mutex> lock(m_pathMutex);
        if (!m_workerModelTierOverride.empty()) {
            return m_workerModelTierOverride;
        }
    }
    try {
        std::string tier = tools3000::core::ConfigManager::instance().get<std::string>("/ocr/modelTier", "mobile");
        for (char& ch : tier) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        return (tier == "server") ? "server" : "mobile";
    } catch (...) {
        return "mobile";
    }
}

std::string DeepOcrEngine::effectiveModelTier() const {
    if (configuredModelTier() == "server" && isServerModelInstalled()) {
        return "server";
    }
    return "mobile";
}

void DeepOcrEngine::setScriptModeOverrideForWorker(const std::string& scriptMode) {
    std::lock_guard<std::mutex> lock(m_pathMutex);
    m_workerScriptModeOverride = scriptMode.empty() ? std::string{} : normalizeScriptMode(scriptMode);
}

std::string DeepOcrEngine::configuredScriptMode() const {
    {
        std::lock_guard<std::mutex> lock(m_pathMutex);
        if (!m_workerScriptModeOverride.empty()) {
            return m_workerScriptModeOverride;
        }
    }
    try {
        const std::string mode = tools3000::core::ConfigManager::instance().get<std::string>("/ocr/scriptMode", "auto");
        return normalizeScriptMode(mode);
    } catch (...) {
        return "auto";
    }
}

bool DeepOcrEngine::isScriptPackInstalled(const std::string& packId) const {
    bool baseInstalled = false;
    const auto dir = resolveModelDirectory(&baseInstalled);
    return isScriptPackModelPresent(dir, packId);
}

std::string DeepOcrEngine::installedScriptPacksSignature() const {
    bool baseInstalled = false;
    const auto dir = resolveModelDirectory(&baseInstalled);
    const std::string gen = isV6BaseBundlePresent(dir) ? "v6" : (isV4BaseBundlePresent(dir) ? "v4" : "none");
    std::string sig;
    for (const char* id : allScriptPackIds()) {
        if (isScriptPackModelPresent(dir, id)) {
            if (!sig.empty()) sig.push_back(',');
            sig += id;
        }
    }
    if (gen == "v4") {
        for (const char* legacyId : {"japan", "latin"}) {
            if (isScriptPackModelPresent(dir, legacyId)) {
                if (!sig.empty()) sig.push_back(',');
                sig += legacyId;
            }
        }
    }
    return gen + ":" + (sig.empty() ? "none" : sig);
}

bool DeepOcrEngine::clampOnnxIrVersionForWinMl(std::vector<uint8_t>& onnxBytes) noexcept {
    if (onnxBytes.size() < 16) return false;
    size_t pos = 0;
    const size_t n = onnxBytes.size();
    while (pos < n) {
        uint64_t tag = 0;
        size_t cur = pos;
        uint32_t shift = 0;
        bool ok = false;
        while (cur < n && shift < 64) {
            const uint8_t b = onnxBytes[cur++];
            tag |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
            if ((b & 0x80u) == 0) { ok = true; break; }
            shift += 7;
        }
        if (!ok || tag == 0) break;
        const uint32_t wire = static_cast<uint32_t>(tag & 0x07u);
        const uint32_t field = static_cast<uint32_t>(tag >> 3u);
        pos = cur;
        if (wire == 0) {
            const size_t valStart = pos;
            uint64_t val = 0;
            shift = 0;
            ok = false;
            while (pos < n && shift < 64) {
                const uint8_t b = onnxBytes[pos++];
                val |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
                if ((b & 0x80u) == 0) { ok = true; break; }
                shift += 7;
            }
            if (!ok) break;
            // ModelProto.ir_version (field 1): opset 14 仅需 IR 8，钳位至 8 即可在 Win10/11 原生 WinML 100% 无损加载 PP-OCRv5
            if (field == 1) {
                if (val > 8 && val <= 127 && (pos - valStart) == 1) {
                    onnxBytes[valStart] = 8u;
                    return true;
                }
                return false;
            }
        } else if (wire == 2) {
            uint64_t len = 0;
            shift = 0;
            ok = false;
            while (pos < n && shift < 64) {
                const uint8_t b = onnxBytes[pos++];
                len |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
                if ((b & 0x80u) == 0) { ok = true; break; }
                shift += 7;
            }
            if (!ok || len > n - pos) break;
            pos += static_cast<size_t>(len);
        } else if (wire == 1) {
            if (n - pos < 8) break;
            pos += 8;
        } else if (wire == 5) {
            if (n - pos < 4) break;
            pos += 4;
        } else {
            break;
        }
    }
    return false;
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

DeepOcrModelStatus DeepOcrEngine::getModelStatus() const {
    DeepOcrModelStatus st;
    bool installed = false;
    const auto dir = resolveModelDirectory(&installed);
    const bool v6Installed = installed && isV6BaseBundlePresent(dir);
    const bool v4OnlyInstalled = installed && !v6Installed && isV4BaseBundlePresent(dir);
    st.installed = installed;
    st.engineGeneration = v6Installed ? "v6" : (v4OnlyInstalled ? "v4" : "");
    st.upgradeAvailable = v4OnlyInstalled;
    st.serverInstalled = installed && isServerRecModelPresent(dir);
    st.modelTier = configuredModelTier();
    st.activeModelTier = (st.serverInstalled && st.modelTier == "server") ? "server" : "mobile";
    st.scriptMode = configuredScriptMode();
    st.modelDir = tools3000::core::WinUtils::wstringToUtf8(dir.wstring());
    st.downloading = m_downloading.load(std::memory_order_acquire);
    st.downloadingServer = st.downloading && m_downloadingServer.load(std::memory_order_acquire);
    st.progressPercent = m_progressPercent.load(std::memory_order_acquire);
    st.downloadedBytes = m_downloadedBytes.load(std::memory_order_acquire);
    st.totalBytes = m_totalBytes.load(std::memory_order_acquire);
    st.serverTotalBytes = DeepOcrConstants::kEstimatedServerAddOnBytes;

    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        st.downloadingScriptPack = st.downloading ? m_downloadingScriptPack : std::string{};
        st.currentFile = m_currentDownloadFile;
        st.error = m_lastDownloadError;
    }

    st.scriptPacks.reserve(kScriptPackSpecs.size());
    for (const auto& meta : kScriptPackSpecs) {
        DeepOcrScriptPackStatus ps{};
        ps.id = meta.id;
        ps.estimatedBytes = meta.estimatedBytes;
        ps.installed = isScriptPackModelPresent(dir, meta.id, &ps.fileBytes);
        ps.downloading = st.downloading &&
                         (st.downloadingScriptPack == meta.id || st.downloadingScriptPack == "all");
        st.scriptPacks.push_back(std::move(ps));
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

bool DeepOcrEngine::startModelDownload(const std::string& tier) {
    if (m_downloading.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }

    const bool includeServerRec = (tier == "server");
    const std::filesystem::path targetDir = defaultInstallDirectory();
    const bool baseAlreadyPresent = isV6BaseBundlePresent(targetDir);
    const uint64_t initialEstTotal = includeServerRec
        ? (baseAlreadyPresent
               ? DeepOcrConstants::kEstimatedServerAddOnBytes
               : (DeepOcrConstants::kEstimatedBundleBytes + DeepOcrConstants::kEstimatedServerAddOnBytes))
        : DeepOcrConstants::kEstimatedBundleBytes;

    m_downloadingServer.store(includeServerRec, std::memory_order_release);
    m_cancelRequested.store(false, std::memory_order_release);
    m_progressPercent.store(0, std::memory_order_release);
    m_downloadedBytes.store(0, std::memory_order_release);
    m_totalBytes.store(initialEstTotal, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        m_downloadingScriptPack.clear();
        m_currentDownloadFile.clear();
        m_lastDownloadError.clear();
        if (m_downloadThread.joinable()) {
            m_downloadThread.join();
        }
        m_downloadThread = std::jthread([this, targetDir, includeServerRec](std::stop_token st) {
            downloadWorkerLoop(st, targetDir, includeServerRec, {});
        });
    }
    return true;
}

bool DeepOcrEngine::startScriptPackDownload(const std::string& packId) {
    if (packId != "all" && !findScriptPackMeta(packId)) {
        return false;
    }
    if (m_downloading.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }

    const std::filesystem::path targetDir = defaultInstallDirectory();
    const bool baseAlreadyPresent = isDirectoryCompleteModelBundle(targetDir);
    uint64_t initialEstTotal = baseAlreadyPresent ? 0ULL : DeepOcrConstants::kEstimatedBundleBytes;
    if (packId == "all") {
        for (const auto& meta : kScriptPackSpecs) {
            if (!isScriptPackModelPresent(targetDir, meta.id)) {
                initialEstTotal += meta.estimatedBytes;
            }
        }
    } else if (const auto* meta = findScriptPackMeta(packId)) {
        initialEstTotal += meta->estimatedBytes;
    }
    if (initialEstTotal == 0ULL) {
        initialEstTotal = 1024ULL * 1024ULL;
    }

    m_downloadingServer.store(false, std::memory_order_release);
    m_cancelRequested.store(false, std::memory_order_release);
    m_progressPercent.store(0, std::memory_order_release);
    m_downloadedBytes.store(0, std::memory_order_release);
    m_totalBytes.store(initialEstTotal, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        m_downloadingScriptPack = packId;
        m_currentDownloadFile.clear();
        m_lastDownloadError.clear();
        if (m_downloadThread.joinable()) {
            m_downloadThread.join();
        }
        m_downloadThread = std::jthread([this, targetDir, packId](std::stop_token st) {
            downloadWorkerLoop(st, targetDir, false, packId);
        });
    }
    return true;
}

bool DeepOcrEngine::deleteScriptPack(const std::string& packId) {
    if (m_downloading.load(std::memory_order_acquire)) {
        return false;
    }
    if (!findScriptPackMeta(packId)) {
        return false;
    }

    std::filesystem::path targetDir;
    {
        std::lock_guard<std::mutex> lock(m_pathMutex);
        if (!m_customTestModelDir.empty()) {
            targetDir = m_customTestModelDir;
        }
    }
    if (targetDir.empty()) {
        bool installed = false;
        const auto resolved = resolveModelDirectory(&installed);
        targetDir = installed ? resolved : defaultInstallDirectory();
    }

    releaseLoadedModels();

    const std::filesystem::path packPath = resolveScriptPackModelPath(targetDir, packId);
    const std::filesystem::path partPath = packPath.wstring() + L".part";
    std::error_code ec;
    std::filesystem::remove(partPath, ec);
    ec.clear();
    if (std::filesystem::exists(packPath, ec) && !ec) {
        std::filesystem::remove(packPath, ec);
    }

    const bool removed = !std::filesystem::exists(packPath, ec) && !ec;
    if (removed && configuredScriptMode() == packId) {
        try {
            auto& cfg = tools3000::core::ConfigManager::instance();
            if (!cfg.mergePatch({{"ocr", {{"scriptMode", "auto"}}}}, "/ocr")) {
                cfg.set<std::string>("/ocr/scriptMode", "auto");
            }
        } catch (...) {
        }
    }
    deep_ocr_detail::cleanupDeepOcrWorkingSet();
    if (removed) {
        LOG_INFO("DeepOcrEngine: removed script extension pack ({})", packId);
    }
    return removed;
}

void DeepOcrEngine::cancelModelDownload() {
    m_cancelRequested.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_downloadMutex);
    if (m_downloadThread.joinable()) {
        m_downloadThread.request_stop();
    }
}

bool DeepOcrEngine::deleteServerModel() {
    if (m_downloading.load(std::memory_order_acquire)) {
        return false;
    }

    std::filesystem::path targetDir;
    {
        std::lock_guard<std::mutex> lock(m_pathMutex);
        if (!m_customTestModelDir.empty()) {
            targetDir = m_customTestModelDir;
        }
    }
    if (targetDir.empty()) {
        bool installed = false;
        const auto resolved = resolveModelDirectory(&installed);
        targetDir = installed ? resolved : defaultInstallDirectory();
    }

    // 先释放已加载的 WinML/OpenCV 会话，避免 Windows 文件映射句柄锁死导致删除失败
    releaseLoadedModels();

    std::error_code ec;
    for (const wchar_t* fileName : {DeepOcrConstants::kV6RecMediumModelFileName, DeepOcrConstants::kRecServerModelFileName}) {
        const std::filesystem::path serverPath = targetDir / fileName;
        const std::filesystem::path partPath = targetDir / (std::wstring(fileName) + L".part");
        std::filesystem::remove(partPath, ec);
        ec.clear();
        if (std::filesystem::exists(serverPath, ec) && !ec) {
            std::filesystem::remove(serverPath, ec);
        }
        ec.clear();
    }

    const bool removed = !isServerRecModelPresent(targetDir);
    if (removed && configuredModelTier() == "server") {
        try {
            auto& cfg = tools3000::core::ConfigManager::instance();
            if (!cfg.mergePatch({{"ocr", {{"modelTier", "mobile"}}}}, "/ocr")) {
                cfg.set<std::string>("/ocr/modelTier", "mobile");
            }
        } catch (...) {
        }
    }
    deep_ocr_detail::cleanupDeepOcrWorkingSet();
    if (removed) {
        LOG_INFO("DeepOcrEngine: removed server rec model from local cache");
    }
    return removed;
}

void DeepOcrEngine::downloadWorkerLoop(
    std::stop_token stopToken,
    std::filesystem::path targetDir,
    bool includeServerRec,
    std::string scriptPackTarget) {
    tools3000::core::TraceId::Scope traceScope;
    std::error_code ec;
    std::filesystem::create_directories(targetDir, ec);
    if (ec) {
        {
            std::lock_guard<std::mutex> lock(m_downloadMutex);
            m_lastDownloadError = "Failed to create model directory";
            m_currentDownloadFile.clear();
            m_downloadingScriptPack.clear();
        }
        m_downloadingServer.store(false, std::memory_order_release);
        m_downloading.store(false, std::memory_order_release);
        return;
    }

    const bool baseAlreadyPresent = isDirectoryCompleteModelBundle(targetDir);
    std::vector<ModelDownloadSpec> specs;
    specs.reserve(kModelSpecs.size() + 5u);
    if (scriptPackTarget.empty() || !baseAlreadyPresent) {
        for (const auto& baseSpec : kModelSpecs) {
            specs.push_back(baseSpec);
        }
    }
    if (includeServerRec) {
        specs.push_back(kServerRecModelSpec);
    }
    if (!scriptPackTarget.empty()) {
        if (scriptPackTarget == "all") {
            for (const auto& meta : kScriptPackSpecs) {
                specs.push_back(meta.spec);
            }
        } else if (const auto* meta = findScriptPackMeta(scriptPackTarget)) {
            specs.push_back(meta->spec);
        }
    }

    const uint64_t expectedTotalBytes = m_totalBytes.load(std::memory_order_acquire);
    uint64_t cumulativeBytes = 0;
    bool allSucceeded = true;
    std::string failReason;

    for (size_t idx = 0; idx < specs.size(); ++idx) {
        if (stopToken.stop_requested() || m_cancelRequested.load(std::memory_order_acquire)) {
            allSucceeded = false;
            failReason = "cancelled";
            break;
        }

        const auto& spec = specs[idx];
        {
            std::lock_guard<std::mutex> lock(m_downloadMutex);
            m_currentDownloadFile = spec.displayName;
        }

        const std::filesystem::path finalPath = targetDir / spec.localFileName;
        const std::filesystem::path partPath = targetDir / (std::wstring(spec.localFileName) + L".part");

        auto verifyCandidatePath = [&](const std::filesystem::path& candidatePath, uint64_t* outByteLen = nullptr, bool normalizeIrInPlace = false) -> bool {
            if (spec.isOnnx) {
                std::vector<uint8_t> verifyBytes;
                if (!readFileBytes(candidatePath, verifyBytes) ||
                    !isValidOnnxBuffer(verifyBytes.data(), verifyBytes.size(), spec.minValidBytes)) {
                    return false;
                }
                if (spec.requiresEmbeddedDict) {
                    std::vector<std::string> embeddedKeys;
                    const size_t minKeys = spec.isScriptExtOnnx ? 100u : DeepOcrConstants::kMinDictKeyLines;
                    if (!extractEmbeddedCharacterDictFromOnnx(verifyBytes.data(), verifyBytes.size(), embeddedKeys) ||
                        embeddedKeys.size() < minKeys) {
                        return false;
                    }
                }
                if (normalizeIrInPlace && clampOnnxIrVersionForWinMl(verifyBytes)) {
                    std::ofstream out(candidatePath, std::ios::binary | std::ios::trunc);
                    if (!out) return false;
                    out.write(reinterpret_cast<const char*>(verifyBytes.data()),
                              static_cast<std::streamsize>(verifyBytes.size()));
                    out.flush();
                    if (!out) return false;
                }
                if (spec.isScriptExtOnnx) {
                    if (outByteLen) *outByteLen = static_cast<uint64_t>(verifyBytes.size());
                    return true;
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
        if (verifyCandidatePath(finalPath, &existingValidBytes, /*normalizeIrInPlace=*/spec.isOnnx)) {
            const bool isIncrementalSkip =
                (includeServerRec && idx < kModelSpecs.size() && expectedTotalBytes <= DeepOcrConstants::kEstimatedServerAddOnBytes) ||
                (!scriptPackTarget.empty() && spec.isScriptExtOnnx && scriptPackTarget == "all");
            if (!isIncrementalSkip) {
                cumulativeBytes += existingValidBytes;
                m_downloadedBytes.store(cumulativeBytes, std::memory_order_release);
                const uint64_t estTotal = (std::max)(cumulativeBytes + 65536ULL, expectedTotalBytes);
                m_totalBytes.store(estTotal, std::memory_order_release);
                m_progressPercent.store(
                    std::clamp(static_cast<int>((cumulativeBytes * 98ULL) / estTotal), 1, 98),
                    std::memory_order_release);
            }
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
                    const uint64_t estTotal = (std::max)(curTotal + 65536ULL, expectedTotalBytes);
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

            // 严格验证文件完整性 (ONNX 魔数 + 全模型 IR<=8 无损钳位 + 内嵌词典校验 / OpenCV DNN 试解析)
            if (!verifyCandidatePath(partPath, nullptr, /*normalizeIrInPlace=*/spec.isOnnx)) {
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

    if (allSucceeded && includeServerRec) {
        try {
            auto& cfg = tools3000::core::ConfigManager::instance();
            if (!cfg.mergePatch({{"ocr", {{"modelTier", "server"}}}}, "/ocr")) {
                cfg.set<std::string>("/ocr/modelTier", "server");
            }
        } catch (...) {
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        m_currentDownloadFile.clear();
        m_downloadingScriptPack.clear();
        if (allSucceeded) {
            m_lastDownloadError.clear();
            m_progressPercent.store(100, std::memory_order_release);
            if (!scriptPackTarget.empty()) {
                LOG_INFO("DeepOcrEngine: multilingual script pack installed successfully (pack={})",
                         scriptPackTarget);
            } else {
                LOG_INFO("DeepOcrEngine: PP-OCRv6 offline model bundle installed successfully (tier={})",
                         includeServerRec ? "server" : "mobile");
            }
        } else if (failReason == "cancelled") {
            m_lastDownloadError.clear();
            m_progressPercent.store(0, std::memory_order_release);
            LOG_INFO("DeepOcrEngine: model download cancelled by user");
        } else {
            m_lastDownloadError = failReason;
            LOG_WARN("DeepOcrEngine: model download failed: {}", failReason);
        }
    }

    m_downloadingServer.store(false, std::memory_order_release);
    m_downloading.store(false, std::memory_order_release);
    if (allSucceeded && !m_shuttingDown.load(std::memory_order_acquire)) {
        // 清空旧档位驻留会话，下次识别自动按新档位/扩展包按需懒加载，杜绝后台空占显存
        releaseLoadedModels();

        // 平滑升级自清理：当新一代 PP-OCRv6 Small 主包已完整落盘后，自动清理已淘汰的旧 v4 冗余文件及退役扩展包，释放磁盘空间
        if (scriptPackTarget.empty() && isV6BaseBundlePresent(targetDir)) {
            static constexpr std::array<const wchar_t*, 6> kLegacyFiles = {{
                DeepOcrConstants::kDetModelFileName,
                DeepOcrConstants::kRecModelFileName,
                DeepOcrConstants::kRecServerModelFileName,
                DeepOcrConstants::kKeysDictFileName,
                DeepOcrConstants::kScriptJapanRecFileName,
                DeepOcrConstants::kScriptLatinRecFileName,
            }};
            for (const wchar_t* legacyName : kLegacyFiles) {
                std::error_code rmEc;
                const auto lp = targetDir / legacyName;
                if (std::filesystem::exists(lp, rmEc) && !rmEc) {
                    std::filesystem::remove(lp, rmEc);
                }
                rmEc.clear();
                std::filesystem::remove(targetDir / (std::wstring(legacyName) + L".part"), rmEc);
            }
        }
    } else {
        deep_ocr_detail::cleanupDeepOcrWorkingSet();
    }
}

} // namespace tools3000::ocr
