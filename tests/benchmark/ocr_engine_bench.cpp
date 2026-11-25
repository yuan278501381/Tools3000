// ─────────────────────────────────────────────────────────────────────────────
// ocr_engine_bench.cpp — Tools3000 DeepOcrEngine 原生跨版本实机对比基准测试工具
//
// 用途：
//   直接在真实 Tools3000 DeepOcrEngine（WinML DirectML GPU / WinML CPU / OpenCV DNN）上
//   评测 PP-OCRv4 / PP-OCRv5 / PP-OCRv6 各档位及多语种外挂包的精度、延迟与全生命周期内存。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "ocr/DeepOcrEngine.h"

#include <windows.h>
#include <psapi.h>

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

struct MemSnapshot {
    double privWsMb = 0.0;
    double wsMb = 0.0;
    double peakWsMb = 0.0;
    double commitMb = 0.0;
};

MemSnapshot sampleProcessMemory() {
    MemSnapshot m{};
    HANDLE hProc = ::GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX2 pmc2{};
    pmc2.cb = sizeof(pmc2);
    if (::GetProcessMemoryInfo(hProc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc2), sizeof(pmc2))) {
        m.privWsMb = static_cast<double>(pmc2.PrivateWorkingSetSize) / (1024.0 * 1024.0);
        m.wsMb = static_cast<double>(pmc2.WorkingSetSize) / (1024.0 * 1024.0);
        m.peakWsMb = static_cast<double>(pmc2.PeakWorkingSetSize) / (1024.0 * 1024.0);
        m.commitMb = static_cast<double>(pmc2.PrivateUsage) / (1024.0 * 1024.0);
        return m;
    }
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(hProc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        m.wsMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        m.peakWsMb = static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
        m.commitMb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        m.privWsMb = m.wsMb;
    }
    return m;
}

json memToJson(const MemSnapshot& m) {
    return json{
        {"priv_ws_mb", std::round(m.privWsMb * 100.0) / 100.0},
        {"ws_mb", std::round(m.wsMb * 100.0) / 100.0},
        {"peak_ws_mb", std::round(m.peakWsMb * 100.0) / 100.0},
        {"commit_mb", std::round(m.commitMb * 100.0) / 100.0},
    };
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

cv::Mat readImageUtf8(const std::string& utf8Path) {
    std::filesystem::path p(utf8ToWide(utf8Path));
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (buf.empty()) return {};
    return cv::imdecode(buf, cv::IMREAD_COLOR);
}

std::string joinOcrLines(const std::vector<tools3000::ocr::OcrResult>& res) {
    std::string out;
    for (size_t i = 0; i < res.size(); ++i) {
        if (i > 0) out.push_back('\n');
        out += res[i].text;
    }
    return out;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::string modelDirUtf8;
    std::string tier = "mobile";
    std::string scriptMode = "cjk";
    std::string manifestPathUtf8;
    std::string outPathUtf8;
    std::string logPathUtf8;
    bool cpuOnly = false;
    int warmupRuns = 0;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        auto nextUtf8 = [&]() -> std::string {
            if (i + 1 >= argc) return {};
            return tools3000::core::WinUtils::wstringToUtf8(argv[++i]);
        };
        if (arg == L"--model-dir") modelDirUtf8 = nextUtf8();
        else if (arg == L"--tier") tier = nextUtf8();
        else if (arg == L"--script") scriptMode = nextUtf8();
        else if (arg == L"--manifest") manifestPathUtf8 = nextUtf8();
        else if (arg == L"--out") outPathUtf8 = nextUtf8();
        else if (arg == L"--log") logPathUtf8 = nextUtf8();
        else if (arg == L"--cpu-only") cpuOnly = (nextUtf8() == "1");
        else if (arg == L"--warmup") warmupRuns = std::stoi(nextUtf8());
    }

    if (modelDirUtf8.empty() || manifestPathUtf8.empty() || outPathUtf8.empty()) {
        std::fprintf(stderr, "Usage: Tools3000OcrBench --model-dir <dir> --manifest <json> --out <json> [--tier mobile|server] [--script cjk|auto|...] [--cpu-only 0|1] [--log <file>]\n");
        return 2;
    }

    if (!logPathUtf8.empty()) {
        std::filesystem::path lp(utf8ToWide(logPathUtf8));
        if (lp.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(lp.parent_path(), ec);
        }
        tools3000::core::LoggerConfig lcfg;
        lcfg.logDir = lp.has_parent_path() ? lp.parent_path() : std::filesystem::current_path();
        lcfg.logFileName = lp.stem().string();
        lcfg.consoleLevel = spdlog::level::info;
        lcfg.fileLevel = spdlog::level::info;
        lcfg.enableConsole = true;
        lcfg.enableMsvcSink = false;
        lcfg.enableCompression = false;
        tools3000::core::Logger::initialize(lcfg);
    }

    json manifest;
    {
        std::ifstream ifs(std::filesystem::path(utf8ToWide(manifestPathUtf8)), std::ios::binary);
        if (!ifs) {
            std::fprintf(stderr, "[ERROR] Failed to open manifest: %s\n", manifestPathUtf8.c_str());
            return 3;
        }
        ifs >> manifest;
    }

    auto& engine = tools3000::ocr::DeepOcrEngine::instance();
    engine.setIdleUnloadTimeoutForTest(0);
    engine.setForceCpuOnlyModeForWorker(cpuOnly);
    engine.setModelTierOverrideForWorker(tier);
    engine.setScriptModeOverrideForWorker(scriptMode);
    engine.setCustomModelDirectoryForTest(std::filesystem::path(utf8ToWide(modelDirUtf8)));

    const MemSnapshot memBefore = sampleProcessMemory();
    const bool modelInstalled = engine.isModelInstalled();
    const bool serverInstalled = engine.isServerModelInstalled();

    json report;
    report["model_dir"] = modelDirUtf8;
    report["tier"] = tier;
    report["script_mode"] = scriptMode;
    report["cpu_only"] = cpuOnly;
    report["model_installed"] = modelInstalled;
    report["server_installed"] = serverInstalled;
    report["mem_before_load"] = memToJson(memBefore);

    if (!modelInstalled) {
        report["error"] = "isModelInstalled() returned false";
        std::ofstream ofs(std::filesystem::path(utf8ToWide(outPathUtf8)), std::ios::binary | std::ios::trunc);
        ofs << report.dump(2);
        return 4;
    }

    json itemsOut = json::array();
    bool firstDone = false;

    for (const auto& entry : manifest["images"]) {
        const std::string id = entry.value("id", "");
        const std::string lang = entry.value("lang", "");
        const std::string scene = entry.value("scene", "");
        const std::string imgPath = entry.value("path", "");
        const std::string perImgScript = entry.value("script_override", scriptMode);

        cv::Mat img = readImageUtf8(imgPath);
        if (img.empty()) {
            itemsOut.push_back(json{
                {"id", id}, {"lang", lang}, {"scene", scene}, {"path", imgPath},
                {"error", "cv::imdecode failed"}
            });
            continue;
        }

        if (!firstDone && warmupRuns > 0) {
            for (int w = 0; w < warmupRuns; ++w) {
                (void)engine.extractText(img, perImgScript);
            }
        }

        const auto t0 = std::chrono::steady_clock::now();
        auto results = engine.extractText(img, perImgScript);
        const auto t1 = std::chrono::steady_clock::now();
        const double wallMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const auto stats = engine.lastRunStats();
        const auto memNow = sampleProcessMemory();

        if (!firstDone) {
            report["mem_after_first_run"] = memToJson(memNow);
            report["first_run_load_ms"] = stats.loadMs;
            report["gpu_blacklisted"] = engine.isGpuRuntimeBlacklisted();
            report["active_sessions_after_first"] = engine.activeSessionCount();
            firstDone = true;
        }

        double avgConf = 0.0;
        if (!results.empty()) {
            for (const auto& r : results) avgConf += r.confidence;
            avgConf /= static_cast<double>(results.size());
        }

        json linesArr = json::array();
        for (const auto& r : results) {
            linesArr.push_back(json{
                {"text", r.text},
                {"conf", std::round(r.confidence * 10000.0) / 10000.0},
                {"box", {r.box.x, r.box.y, r.box.width, r.box.height}}
            });
        }

        itemsOut.push_back(json{
            {"id", id},
            {"lang", lang},
            {"scene", scene},
            {"width", img.cols},
            {"height", img.rows},
            {"wall_ms", std::round(wallMs * 100.0) / 100.0},
            {"load_ms", stats.loadMs},
            {"det_ms", stats.detMs},
            {"pack_ms", stats.packMs},
            {"rec_ms", stats.recMs},
            {"refine_ms", stats.refineMs},
            {"detected_quads", stats.detectedQuads},
            {"strip_count", stats.stripCount},
            {"server_refined_count", stats.serverRefinedCount},
            {"active_sessions", stats.activeSessions},
            {"bucket_profile", stats.bucketProfile},
            {"detected_script", stats.detectedScript},
            {"line_count", results.size()},
            {"avg_conf", std::round(avgConf * 10000.0) / 10000.0},
            {"full_text", joinOcrLines(results)},
            {"lines", std::move(linesArr)},
            {"mem_after", memToJson(memNow)}
        });
    }

    report["results"] = std::move(itemsOut);
    report["mem_peak_end_of_batch"] = memToJson(sampleProcessMemory());
    report["active_sessions_end"] = engine.activeSessionCount();
    report["gpu_blacklisted_end"] = engine.isGpuRuntimeBlacklisted();

    engine.trimIdleMemory();
    report["mem_after_trim"] = memToJson(sampleProcessMemory());

    engine.releaseLoadedModels();
    report["mem_after_release"] = memToJson(sampleProcessMemory());
    report["active_sessions_after_release"] = engine.activeSessionCount();

    engine.shutdown();

    {
        std::filesystem::path op(utf8ToWide(outPathUtf8));
        if (op.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(op.parent_path(), ec);
        }
        std::ofstream ofs(op, std::ios::binary | std::ios::trunc);
        ofs << report.dump(2);
    }

    if (!logPathUtf8.empty()) {
        tools3000::core::Logger::shutdown();
    }
    return 0;
}
