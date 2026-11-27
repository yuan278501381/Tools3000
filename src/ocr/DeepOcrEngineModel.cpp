// ─────────────────────────────────────────────────────────────────────────────
// DeepOcrEngineModel.cpp — PP-OCRv4 模型包寻址、ONNX Protobuf 校验/字典提取/静态维度修补与 WinHTTP 下载器
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/DeepOcrEngineInternal.h"
#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/network/ResilientModelDownloader.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <windows.h>

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
#include <unordered_map>
#include <unordered_set>
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
    // 快速 64B 头部零拷贝嗅探：ir_version <= 7 时已兼容 Win11 21H2+ 全系列原生 WinML，零整文件读盘直通
    std::ifstream probeIfs(onnxPath, std::ios::binary);
    if (!probeIfs) return onnxPath;
    std::array<uint8_t, 64> head{};
    probeIfs.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    const size_t got = static_cast<size_t>(probeIfs.gcount());
    probeIfs.close();
    if (got < 16) return onnxPath;
    {
        size_t pos = 0;
        uint64_t headIr = 0;
        while (pos < got) {
            uint64_t tag = 0;
            uint32_t shift = 0;
            bool ok = false;
            while (pos < got && shift < 64) {
                const uint8_t b = head[pos++];
                tag |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
                if ((b & 0x80u) == 0) { ok = true; break; }
                shift += 7;
            }
            if (!ok || tag == 0) break;
            const uint32_t wire = static_cast<uint32_t>(tag & 0x07u);
            const uint32_t field = static_cast<uint32_t>(tag >> 3u);
            if (wire == 0) {
                uint64_t val = 0;
                shift = 0;
                ok = false;
                while (pos < got && shift < 64) {
                    const uint8_t b = head[pos++];
                    val |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
                    if ((b & 0x80u) == 0) { ok = true; break; }
                    shift += 7;
                }
                if (!ok) break;
                if (field == 1) {
                    headIr = val;
                    break;
                }
            } else {
                break;
            }
        }
        if (headIr > 0 && headIr <= 7) {
            return onnxPath;
        }
    }

    // 检测到 IR>=8 模型：按 opset_import 精确钳位（opset<=12 钳位至 IR 7 兼容 Win11 21H2，更高 opset 钳位至 IR 8）
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

struct MirrorSpecEntry {
    const wchar_t* url;
    tools3000::core::net::MirrorRegion region;
};

struct ModelDownloadSpec {
    const wchar_t* localFileName;
    const char* displayName;
    size_t minValidBytes;
    uint64_t estimatedBytes;
    bool isOnnx;
    bool requiresEmbeddedDict;
    bool isScriptExtOnnx;
    std::array<MirrorSpecEntry, 6> mirrors;
};

struct ScriptPackDownloadMeta {
    const char* id;
    uint64_t estimatedBytes;
    ModelDownloadSpec spec;
};

// 新一代 PP-OCRv6 Small 核心底座（原生 50 语种，China + Global 双栈并发竞速 + 锁定 Tag v3.10.0 与 master 互备）
constexpr std::array<ModelDownloadSpec, 3> kModelSpecs = {{
    {
        DeepOcrConstants::kV6DetModelFileName,
        "PP-OCRv6_det_small.onnx",
        DeepOcrConstants::kMinDetModelBytes,
        5000000ULL,
        true,
        false,
        false,
        {{
            {L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/det/PP-OCRv6_det_small.onnx", tools3000::core::net::MirrorRegion::China},
            {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/det/PP-OCRv6_det_small.onnx", tools3000::core::net::MirrorRegion::Global},
            {L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/det/PP-OCRv6_det_small.onnx", tools3000::core::net::MirrorRegion::China},
            {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/det/PP-OCRv6_det_small.onnx", tools3000::core::net::MirrorRegion::Global},
            {nullptr, tools3000::core::net::MirrorRegion::Auto},
            {nullptr, tools3000::core::net::MirrorRegion::Auto}
        }}
    },
    {
        DeepOcrConstants::kClsModelFileName,
        "ch_ppocr_mobile_v2.0_cls.onnx",
        DeepOcrConstants::kMinClsModelBytes,
        600000ULL,
        true,
        false,
        false,
        {{
            {L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx", tools3000::core::net::MirrorRegion::China},
            {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx", tools3000::core::net::MirrorRegion::Global},
            {L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx", tools3000::core::net::MirrorRegion::China},
            {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx", tools3000::core::net::MirrorRegion::Global},
            {L"https://hf-mirror.com/SWHL/RapidOCR/resolve/main/PP-OCRv1/ch_ppocr_mobile_v2.0_cls_infer.onnx", tools3000::core::net::MirrorRegion::China},
            {L"https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv1/ch_ppocr_mobile_v2.0_cls_infer.onnx", tools3000::core::net::MirrorRegion::Global}
        }}
    },
    {
        DeepOcrConstants::kV6RecModelFileName,
        "PP-OCRv6_rec_small.onnx",
        DeepOcrConstants::kMinRecModelBytes,
        26150000ULL,
        true,
        true,
        false,
        {{
            {L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx", tools3000::core::net::MirrorRegion::China},
            {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx", tools3000::core::net::MirrorRegion::Global},
            {L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx", tools3000::core::net::MirrorRegion::China},
            {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx", tools3000::core::net::MirrorRegion::Global},
            {nullptr, tools3000::core::net::MirrorRegion::Auto},
            {nullptr, tools3000::core::net::MirrorRegion::Auto}
        }}
    }
}};

// 高精度混搭增强包：仅按需下载 PP-OCRv6 Medium Rec (~73.1 MB)，复用轻量 det_small 省 50MB 体积与 1.75GB Commit
constexpr ModelDownloadSpec kServerRecModelSpec = {
    DeepOcrConstants::kV6RecMediumModelFileName,
    "PP-OCRv6_rec_medium.onnx",
    DeepOcrConstants::kMinRecServerModelBytes,
    DeepOcrConstants::kEstimatedServerAddOnBytes,
    true,
    true,
    false,
    {{
        {L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/rec/PP-OCRv6_rec_medium.onnx", tools3000::core::net::MirrorRegion::China},
        {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/rec/PP-OCRv6_rec_medium.onnx", tools3000::core::net::MirrorRegion::Global},
        {L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/rec/PP-OCRv6_rec_medium.onnx", tools3000::core::net::MirrorRegion::China},
        {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv6/rec/PP-OCRv6_rec_medium.onnx", tools3000::core::net::MirrorRegion::Global},
        {nullptr, tools3000::core::net::MirrorRegion::Auto},
        {nullptr, tools3000::core::net::MirrorRegion::Auto}
    }}
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
            DeepOcrConstants::kEstimatedScriptKoreanBytes,
            true,
            true,
            true,
            {{
                {L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/rec/korean_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::China},
                {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/rec/korean_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::Global},
                {L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/korean_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::China},
                {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/korean_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::Global},
                {nullptr, tools3000::core::net::MirrorRegion::Auto},
                {nullptr, tools3000::core::net::MirrorRegion::Auto}
            }}
        }
    },
    {
        "cyrillic",
        DeepOcrConstants::kEstimatedScriptCyrillicBytes,
        {
            DeepOcrConstants::kScriptCyrillicRecFileName,
            "cyrillic_PP-OCRv5_rec_mobile.onnx",
            DeepOcrConstants::kMinRecModelBytes,
            DeepOcrConstants::kEstimatedScriptCyrillicBytes,
            true,
            true,
            true,
            {{
                {L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/rec/cyrillic_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::China},
                {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/rec/cyrillic_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::Global},
                {L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/cyrillic_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::China},
                {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/cyrillic_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::Global},
                {nullptr, tools3000::core::net::MirrorRegion::Auto},
                {nullptr, tools3000::core::net::MirrorRegion::Auto}
            }}
        }
    },
    {
        "arabic",
        DeepOcrConstants::kEstimatedScriptArabicBytes,
        {
            DeepOcrConstants::kScriptArabicRecFileName,
            "arabic_PP-OCRv5_rec_mobile.onnx",
            DeepOcrConstants::kMinRecModelBytes,
            DeepOcrConstants::kEstimatedScriptArabicBytes,
            true,
            true,
            true,
            {{
                {L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/rec/arabic_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::China},
                {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/rec/arabic_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::Global},
                {L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/arabic_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::China},
                {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/arabic_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::Global},
                {nullptr, tools3000::core::net::MirrorRegion::Auto},
                {nullptr, tools3000::core::net::MirrorRegion::Auto}
            }}
        }
    },
    {
        "thai",
        DeepOcrConstants::kEstimatedScriptThaiBytes,
        {
            DeepOcrConstants::kScriptThaiRecFileName,
            "th_PP-OCRv5_rec_mobile.onnx",
            DeepOcrConstants::kMinRecModelBytes,
            DeepOcrConstants::kEstimatedScriptThaiBytes,
            true,
            true,
            true,
            {{
                {L"https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/rec/th_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::China},
                {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv5/rec/th_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::Global},
                {L"https://modelscope.cn/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/th_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::China},
                {L"https://www.modelscope.ai/models/RapidAI/RapidOCR/resolve/master/onnx/PP-OCRv5/rec/th_PP-OCRv5_rec_mobile.onnx", tools3000::core::net::MirrorRegion::Global},
                {nullptr, tools3000::core::net::MirrorRegion::Auto},
                {nullptr, tools3000::core::net::MirrorRegion::Auto}
            }}
        }
    }
}};

const ScriptPackDownloadMeta* findScriptPackMeta(std::string_view id) noexcept {
    for (const auto& item : kScriptPackSpecs) {
        if (id == item.id) return &item;
    }
    return nullptr;
}

std::vector<tools3000::core::net::MirrorEndpoint> buildMirrorList(const ModelDownloadSpec& spec) {
    std::vector<tools3000::core::net::MirrorEndpoint> out;
    out.reserve(spec.mirrors.size());
    for (const auto& entry : spec.mirrors) {
        if (entry.url && wcslen(entry.url) > 0) {
            out.push_back(tools3000::core::net::MirrorEndpoint{entry.url, entry.region, {}});
        }
    }
    return out;
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
    bool hasIr = false;
    size_t irValStart = 0;
    size_t irValBytes = 0;
    uint64_t irVal = 0;
    uint64_t maxDefaultOpset = 0;

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
            if (field == 1) {
                hasIr = true;
                irValStart = valStart;
                irValBytes = pos - valStart;
                irVal = val;
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
            // ModelProto.opset_import (field 8): OperatorSetIdProto { domain=1, version=2 }
            if (field == 8 && len > 0) {
                size_t sp = pos;
                const size_t sEnd = pos + static_cast<size_t>(len);
                std::string_view domain;
                uint64_t opVer = 0;
                while (sp < sEnd) {
                    uint64_t st = 0;
                    shift = 0;
                    ok = false;
                    while (sp < sEnd && shift < 64) {
                        const uint8_t b = onnxBytes[sp++];
                        st |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
                        if ((b & 0x80u) == 0) { ok = true; break; }
                        shift += 7;
                    }
                    if (!ok || st == 0) break;
                    const uint32_t sw = static_cast<uint32_t>(st & 0x07u);
                    const uint32_t sf = static_cast<uint32_t>(st >> 3u);
                    if (sw == 0) {
                        uint64_t sv = 0;
                        shift = 0;
                        ok = false;
                        while (sp < sEnd && shift < 64) {
                            const uint8_t b = onnxBytes[sp++];
                            sv |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
                            if ((b & 0x80u) == 0) { ok = true; break; }
                            shift += 7;
                        }
                        if (!ok) break;
                        if (sf == 2) opVer = sv;
                    } else if (sw == 2) {
                        uint64_t sl = 0;
                        shift = 0;
                        ok = false;
                        while (sp < sEnd && shift < 64) {
                            const uint8_t b = onnxBytes[sp++];
                            sl |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
                            if ((b & 0x80u) == 0) { ok = true; break; }
                            shift += 7;
                        }
                        if (!ok || sl > sEnd - sp) break;
                        if (sf == 1) {
                            domain = std::string_view(reinterpret_cast<const char*>(onnxBytes.data() + sp),
                                                      static_cast<size_t>(sl));
                        }
                        sp += static_cast<size_t>(sl);
                    } else if (sw == 1) {
                        if (sEnd - sp < 8) break;
                        sp += 8;
                    } else if (sw == 5) {
                        if (sEnd - sp < 4) break;
                        sp += 4;
                    } else {
                        break;
                    }
                }
                if ((domain.empty() || domain == "ai.onnx") && opVer > maxDefaultOpset) {
                    maxDefaultOpset = opVer;
                }
            }
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

    if (!hasIr || irValBytes != 1 || irVal > 127) {
        return false;
    }
    // opset <= 12（如 PP-OCRv6 det/rec 的 opset=11）仅需 IR 7，钳位至 7 可同时兼容 Win11 21H2 (Build 22000) 与 22H2+；
    // 更高 opset（如 PP-OCRv5 扩展包的 opset=14）或未携带 opset_import 的测试包钳位至 IR 8。
    const uint64_t targetIr = (maxDefaultOpset > 0 && maxDefaultOpset <= 12) ? 7u : 8u;
    if (irVal > targetIr) {
        onnxBytes[irValStart] = static_cast<uint8_t>(targetIr);
        return true;
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

bool DeepOcrEngine::isCompleteOnnxModelProto(const uint8_t* data, size_t byteSize, size_t minBytes) noexcept {
    if (!isValidOnnxBuffer(data, byteSize, minBytes)) {
        return false;
    }

    size_t pos = 0;
    bool hasIrVersion = false;
    bool hasGraph = false;
    while (pos < byteSize) {
        uint64_t tag = 0;
        uint32_t shift = 0;
        bool ok = false;
        while (pos < byteSize && shift < 64) {
            const uint8_t b = data[pos++];
            tag |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
            if ((b & 0x80u) == 0) {
                ok = true;
                break;
            }
            shift += 7;
        }
        if (!ok || tag == 0) {
            return false;
        }
        const uint32_t wire = static_cast<uint32_t>(tag & 0x07u);
        const uint32_t field = static_cast<uint32_t>(tag >> 3u);
        if (field == 0 || field > 64) {
            return false;
        }

        if (wire == 0) {
            uint64_t val = 0;
            shift = 0;
            ok = false;
            while (pos < byteSize && shift < 64) {
                const uint8_t b = data[pos++];
                val |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
                if ((b & 0x80u) == 0) {
                    ok = true;
                    break;
                }
                shift += 7;
            }
            if (!ok) return false;
            if (field == 1 && val >= 1 && val <= 64) {
                hasIrVersion = true;
            }
        } else if (wire == 2) {
            uint64_t len = 0;
            shift = 0;
            ok = false;
            while (pos < byteSize && shift < 64) {
                const uint8_t b = data[pos++];
                len |= (static_cast<uint64_t>(b & 0x7Fu) << shift);
                if ((b & 0x80u) == 0) {
                    ok = true;
                    break;
                }
                shift += 7;
            }
            if (!ok || len > byteSize - pos) {
                return false;
            }
            if (field == 7 && len > 0) {
                hasGraph = true;
            }
            pos += static_cast<size_t>(len);
        } else if (wire == 1) {
            if (byteSize - pos < 8) return false;
            pos += 8;
        } else if (wire == 5) {
            if (byteSize - pos < 4) return false;
            pos += 4;
        } else {
            return false;
        }
    }

    return pos == byteSize && hasIrVersion && hasGraph;
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

            // 预扫描 1：识别 Paddle2ONNX 导出的 0 元素标量形状常量 (dims == [0]) 及恒等冗余链路
            //   Reshape(x, empty_0d_shape) -> y  紧接  Unsqueeze(y) -> z
            //   OpenCV 4.12 ONNXImporter 在解析 0 元素 Reshape shape 张量时会把空 Mat 视为 CV_8UC1 触发
            //   CV_Assert(blob.type() == CV_32SC1) 崩溃（Win10/Server 退回 OpenCV DNN 时 100% 导致 PP-OCRv6 加载失败）。
            //   无损折叠该等价恒等对 (z := x) 后，PP-OCRv6_rec_small/medium 与全套 PP-OCRv5 语种包 100% 畅通运行。
            std::unordered_set<std::string> emptyZeroDimInits;
            struct NodeBrief {
                std::string opType;
                std::vector<std::string> inputs;
                std::vector<std::string> outputs;
            };
            std::vector<NodeBrief> graphNodes;
            {
                size_t sp = 0;
                while (sp < gSize) {
                    uint64_t gtag = 0;
                    if (!readProtobufVarint(gData, gSize, sp, gtag) || gtag == 0) break;
                    const uint32_t gwire = static_cast<uint32_t>(gtag & 0x07u);
                    const uint32_t gfnum = static_cast<uint32_t>(gtag >> 3u);
                    if (gwire == 2) {
                        uint64_t subLen = 0;
                        if (!readProtobufVarint(gData, gSize, sp, subLen) || subLen > gSize - sp) break;
                        const uint8_t* sub = gData + sp;
                        const size_t sl = static_cast<size_t>(subLen);
                        sp += sl;
                        if (gfnum == 5) { // GraphProto.initializer (TensorProto)
                            size_t tp = 0;
                            std::string tname;
                            std::vector<uint64_t> tdims;
                            while (tp < sl) {
                                uint64_t tt = 0;
                                if (!readProtobufVarint(sub, sl, tp, tt) || tt == 0) break;
                                const uint32_t tw = static_cast<uint32_t>(tt & 0x07u);
                                const uint32_t tf = static_cast<uint32_t>(tt >> 3u);
                                if (tw == 0) {
                                    uint64_t dv = 0;
                                    if (!readProtobufVarint(sub, sl, tp, dv)) break;
                                    if (tf == 1) tdims.push_back(dv);
                                } else if (tw == 2) {
                                    uint64_t fl = 0;
                                    if (!readProtobufVarint(sub, sl, tp, fl) || fl > sl - tp) break;
                                    if (tf == 8) {
                                        tname.assign(reinterpret_cast<const char*>(sub + tp), static_cast<size_t>(fl));
                                    } else if (tf == 1) {
                                        size_t pk = tp;
                                        const size_t pkEnd = tp + static_cast<size_t>(fl);
                                        while (pk < pkEnd) {
                                            uint64_t dv = 0;
                                            if (!readProtobufVarint(sub, pkEnd, pk, dv)) break;
                                            tdims.push_back(dv);
                                        }
                                    }
                                    tp += static_cast<size_t>(fl);
                                } else if (tw == 5) {
                                    if (sl - tp < 4) break;
                                    tp += 4;
                                } else if (tw == 1) {
                                    if (sl - tp < 8) break;
                                    tp += 8;
                                } else {
                                    break;
                                }
                            }
                            if (!tname.empty() && tdims.size() == 1 && tdims[0] == 0) {
                                emptyZeroDimInits.insert(std::move(tname));
                            }
                        } else if (gfnum == 1) { // GraphProto.node (NodeProto)
                            NodeBrief nb{};
                            size_t np = 0;
                            while (np < sl) {
                                uint64_t nt = 0;
                                if (!readProtobufVarint(sub, sl, np, nt) || nt == 0) break;
                                const uint32_t nw = static_cast<uint32_t>(nt & 0x07u);
                                const uint32_t nf = static_cast<uint32_t>(nt >> 3u);
                                if (nw == 2) {
                                    uint64_t fl = 0;
                                    if (!readProtobufVarint(sub, sl, np, fl) || fl > sl - np) break;
                                    if (nf == 1) {
                                        nb.inputs.emplace_back(reinterpret_cast<const char*>(sub + np), static_cast<size_t>(fl));
                                    } else if (nf == 2) {
                                        nb.outputs.emplace_back(reinterpret_cast<const char*>(sub + np), static_cast<size_t>(fl));
                                    } else if (nf == 4) {
                                        nb.opType.assign(reinterpret_cast<const char*>(sub + np), static_cast<size_t>(fl));
                                    }
                                    np += static_cast<size_t>(fl);
                                } else if (nw == 0) {
                                    uint64_t dummy = 0;
                                    if (!readProtobufVarint(sub, sl, np, dummy)) break;
                                } else if (nw == 5) {
                                    if (sl - np < 4) break;
                                    np += 4;
                                } else if (nw == 1) {
                                    if (sl - np < 8) break;
                                    np += 8;
                                } else {
                                    break;
                                }
                            }
                            graphNodes.push_back(std::move(nb));
                        }
                    } else if (gwire == 0) {
                        uint64_t dummy = 0;
                        if (!readProtobufVarint(gData, gSize, sp, dummy)) break;
                    } else if (gwire == 5) {
                        if (gSize - sp < 4) break;
                        sp += 4;
                    } else if (gwire == 1) {
                        if (gSize - sp < 8) break;
                        sp += 8;
                    } else {
                        break;
                    }
                }
            }

            std::unordered_map<std::string, std::string> inputAlias;
            std::unordered_set<std::string> dropNodeOutputs;
            if (!emptyZeroDimInits.empty()) {
                std::unordered_map<std::string, size_t> consumerCount;
                for (const auto& nb : graphNodes) {
                    for (const auto& inName : nb.inputs) {
                        ++consumerCount[inName];
                    }
                }
                std::unordered_map<std::string, std::string> scalarReshaped;
                for (const auto& nb : graphNodes) {
                    if (nb.opType == "Reshape" && nb.inputs.size() == 2 && nb.outputs.size() == 1 &&
                        emptyZeroDimInits.count(nb.inputs[1]) > 0 && consumerCount[nb.outputs[0]] == 1) {
                        scalarReshaped[nb.outputs[0]] = nb.inputs[0];
                    }
                }
                for (const auto& nb : graphNodes) {
                    if (nb.opType == "Unsqueeze" && !nb.inputs.empty() && nb.outputs.size() == 1) {
                        const auto it = scalarReshaped.find(nb.inputs[0]);
                        if (it != scalarReshaped.end()) {
                            inputAlias[nb.outputs[0]] = it->second;
                            dropNodeOutputs.insert(it->first);
                            dropNodeOutputs.insert(nb.outputs[0]);
                        }
                    }
                }
            }

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
                } else if (gfnum == 1 && gwire == 2 && !dropNodeOutputs.empty()) { // GraphProto.node = field 1
                    uint64_t nodeLen = 0;
                    if (!readProtobufVarint(gData, gSize, gp, nodeLen) || nodeLen > gSize - gp) {
                        return false;
                    }
                    const uint8_t* nData = gData + gp;
                    const size_t nSize = static_cast<size_t>(nodeLen);
                    gp += nSize;

                    bool dropThisNode = false;
                    bool needsInputRewrite = false;
                    {
                        size_t np = 0;
                        size_t outCount = 0;
                        std::string firstOut;
                        while (np < nSize) {
                            uint64_t nt = 0;
                            if (!readProtobufVarint(nData, nSize, np, nt) || nt == 0) break;
                            const uint32_t nw = static_cast<uint32_t>(nt & 0x07u);
                            const uint32_t nf = static_cast<uint32_t>(nt >> 3u);
                            if (nw == 2) {
                                uint64_t fl = 0;
                                if (!readProtobufVarint(nData, nSize, np, fl) || fl > nSize - np) break;
                                if (nf == 2) {
                                    ++outCount;
                                    if (outCount == 1) {
                                        firstOut.assign(reinterpret_cast<const char*>(nData + np), static_cast<size_t>(fl));
                                    }
                                } else if (nf == 1) {
                                    std::string inStr(reinterpret_cast<const char*>(nData + np), static_cast<size_t>(fl));
                                    if (inputAlias.count(inStr) > 0) {
                                        needsInputRewrite = true;
                                    }
                                }
                                np += static_cast<size_t>(fl);
                            } else if (nw == 0) {
                                uint64_t dummy = 0;
                                if (!readProtobufVarint(nData, nSize, np, dummy)) break;
                            } else if (nw == 5) {
                                if (nSize - np < 4) break;
                                np += 4;
                            } else if (nw == 1) {
                                if (nSize - np < 8) break;
                                np += 8;
                            } else {
                                break;
                            }
                        }
                        if (outCount == 1 && dropNodeOutputs.count(firstOut) > 0) {
                            dropThisNode = true;
                        }
                    }

                    if (dropThisNode) {
                        continue;
                    }
                    if (!needsInputRewrite) {
                        newGraph.insert(newGraph.end(), gData + gFieldStart, gData + gp);
                    } else {
                        std::vector<uint8_t> rewrittenNode;
                        rewrittenNode.reserve(nSize + 16);
                        size_t np = 0;
                        while (np < nSize) {
                            const size_t nFieldStart = np;
                            uint64_t nt = 0;
                            if (!readProtobufVarint(nData, nSize, np, nt) || nt == 0) return false;
                            const uint32_t nw = static_cast<uint32_t>(nt & 0x07u);
                            const uint32_t nf = static_cast<uint32_t>(nt >> 3u);
                            if (nw == 2) {
                                uint64_t fl = 0;
                                if (!readProtobufVarint(nData, nSize, np, fl) || fl > nSize - np) return false;
                                if (nf == 1) {
                                    std::string inStr(reinterpret_cast<const char*>(nData + np), static_cast<size_t>(fl));
                                    np += static_cast<size_t>(fl);
                                    const auto it = inputAlias.find(inStr);
                                    const std::string& finalIn = (it != inputAlias.end()) ? it->second : inStr;
                                    appendProtobufLengthDelimited(
                                        rewrittenNode, 1,
                                        reinterpret_cast<const uint8_t*>(finalIn.data()), finalIn.size());
                                } else {
                                    np += static_cast<size_t>(fl);
                                    rewrittenNode.insert(rewrittenNode.end(), nData + nFieldStart, nData + np);
                                }
                            } else if (nw == 0) {
                                uint64_t dummy = 0;
                                if (!readProtobufVarint(nData, nSize, np, dummy)) return false;
                                rewrittenNode.insert(rewrittenNode.end(), nData + nFieldStart, nData + np);
                            } else if (nw == 5) {
                                if (nSize - np < 4) return false;
                                np += 4;
                                rewrittenNode.insert(rewrittenNode.end(), nData + nFieldStart, nData + np);
                            } else if (nw == 1) {
                                if (nSize - np < 8) return false;
                                np += 8;
                                rewrittenNode.insert(rewrittenNode.end(), nData + nFieldStart, nData + np);
                            } else {
                                return false;
                            }
                        }
                        appendProtobufLengthDelimited(newGraph, 1, rewrittenNode.data(), rewrittenNode.size());
                    }
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
    st.speedBytesPerSec = st.downloading ? m_speedBytesPerSec.load(std::memory_order_acquire) : 0.0;
    st.mirrorIndex = st.downloading ? m_mirrorIndex.load(std::memory_order_acquire) : 0;
    st.mirrorTotal = st.downloading ? m_mirrorTotal.load(std::memory_order_acquire) : 0;

    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        st.downloadingScriptPack = st.downloading ? m_downloadingScriptPack : std::string{};
        st.currentFile = m_currentDownloadFile;
        st.activeMirrorHost = st.downloading ? m_activeMirrorHost : std::string{};
        st.phase = st.downloading ? m_downloadPhase : std::string{};
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
    m_speedBytesPerSec.store(0.0, std::memory_order_release);
    m_mirrorIndex.store(0, std::memory_order_release);
    m_mirrorTotal.store(0, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        m_downloadingScriptPack.clear();
        m_currentDownloadFile.clear();
        m_activeMirrorHost.clear();
        m_downloadPhase = "probing";
        m_lastDownloadError.clear();
        m_downloadCancelToken = std::make_shared<tools3000::core::net::DownloadCancelToken>();
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
    m_speedBytesPerSec.store(0.0, std::memory_order_release);
    m_mirrorIndex.store(0, std::memory_order_release);
    m_mirrorTotal.store(0, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        m_downloadingScriptPack = packId;
        m_currentDownloadFile.clear();
        m_activeMirrorHost.clear();
        m_downloadPhase = "probing";
        m_lastDownloadError.clear();
        m_downloadCancelToken = std::make_shared<tools3000::core::net::DownloadCancelToken>();
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
    std::shared_ptr<tools3000::core::net::DownloadCancelToken> cancelToken;
    {
        std::lock_guard<std::mutex> lock(m_downloadMutex);
        cancelToken = m_downloadCancelToken;
        if (m_downloadThread.joinable()) {
            m_downloadThread.request_stop();
        }
    }
    if (cancelToken) {
        cancelToken->cancel();
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
                    !isCompleteOnnxModelProto(verifyBytes.data(), verifyBytes.size(), spec.minValidBytes)) {
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
                if (outByteLen) *outByteLen = static_cast<uint64_t>(verifyBytes.size());
                return true;
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

        std::shared_ptr<tools3000::core::net::DownloadCancelToken> cancelToken;
        {
            std::lock_guard<std::mutex> lock(m_downloadMutex);
            cancelToken = m_downloadCancelToken;
        }
        if (!cancelToken) {
            cancelToken = std::make_shared<tools3000::core::net::DownloadCancelToken>();
        }

        if (!fileDone) {
            auto mirrors = buildMirrorList(spec);
            const uint64_t baseBeforeFile = cumulativeBytes;
            uint64_t finalDownloadedFileBytes = 0;

            for (int verifyAttempt = 0; verifyAttempt < 2 && !mirrors.empty(); ++verifyAttempt) {
                if (stopToken.stop_requested() || m_cancelRequested.load(std::memory_order_acquire)) {
                    cancelToken->cancel();
                    break;
                }

                tools3000::core::net::ResilientDownloadOptions dlOpts;
                dlOpts.userAgent = L"Tools3000/1.0 (PP-OCRv6 Offline Model Installer)";
                dlOpts.maxFileBytes = 128ULL * 1024ULL * 1024ULL;
                dlOpts.expectedFileBytes = spec.estimatedBytes;

                const auto outcome = tools3000::core::net::ResilientModelDownloader::downloadToPartFile(
                    mirrors, partPath, dlOpts, cancelToken,
                    [&](const tools3000::core::net::DownloadProgress& prog) {
                        const uint64_t curTotal = baseBeforeFile + prog.fileBytesDownloaded;
                        m_downloadedBytes.store(curTotal, std::memory_order_release);
                        const uint64_t estTotal = (std::max)(curTotal + 65536ULL, expectedTotalBytes);
                        m_totalBytes.store(estTotal, std::memory_order_release);
                        const int pct = std::clamp(
                            static_cast<int>((curTotal * 98ULL) / estTotal),
                            1, 98);
                        m_progressPercent.store(pct, std::memory_order_release);
                        m_speedBytesPerSec.store(prog.speedBytesPerSec, std::memory_order_release);
                        m_mirrorIndex.store(prog.mirrorIndex, std::memory_order_release);
                        m_mirrorTotal.store(prog.mirrorTotal, std::memory_order_release);
                        {
                            std::lock_guard<std::mutex> lock(m_downloadMutex);
                            m_activeMirrorHost = prog.activeMirrorHost;
                            m_downloadPhase = prog.phase;
                        }
                    });

                if (outcome.cancelled || stopToken.stop_requested() || m_cancelRequested.load(std::memory_order_acquire)) {
                    break;
                }
                if (!outcome.success) {
                    failReason = outcome.error;
                    break;
                }

                {
                    std::lock_guard<std::mutex> lock(m_downloadMutex);
                    m_downloadPhase = "verifying";
                }
                m_speedBytesPerSec.store(0.0, std::memory_order_release);

                // 严格验证文件完整性 (ONNX 魔数 + 全模型 IR<=8 无损钳位 + 内嵌词典校验)
                if (!verifyCandidatePath(partPath, &finalDownloadedFileBytes, /*normalizeIrInPlace=*/spec.isOnnx)) {
                    LOG_WARN("DeepOcrEngine: downloaded file failed integrity check: {} (mirror={})",
                             spec.displayName, outcome.activeMirrorHost);
                    std::filesystem::remove(partPath, ec);
                    mirrors.erase(
                        std::remove_if(mirrors.begin(), mirrors.end(), [&](const auto& ep) {
                            return tools3000::core::net::ResilientModelDownloader::extractUrlHostUtf8(ep.url) ==
                                   outcome.activeMirrorHost;
                        }),
                        mirrors.end());
                    continue;
                }

                if (MoveFileExW(partPath.c_str(), finalPath.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
                    cumulativeBytes += (finalDownloadedFileBytes > 0 ? finalDownloadedFileBytes : outcome.bytesWritten);
                    fileDone = true;
                    break;
                }
                std::filesystem::remove(partPath, ec);
            }
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
        m_activeMirrorHost.clear();
        m_downloadPhase.clear();
        m_speedBytesPerSec.store(0.0, std::memory_order_release);
        m_mirrorIndex.store(0, std::memory_order_release);
        m_mirrorTotal.store(0, std::memory_order_release);
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
