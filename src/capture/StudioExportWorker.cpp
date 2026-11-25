// ─────────────────────────────────────────────────────────────────────────────
// StudioExportWorker.cpp — 演播室多格式媒体导出短命子进程隔离实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/StudioExportWorker.h"
#include "capture/CaptureWorkerCommon.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace tools3000::capture {

namespace {

using json = nlohmann::json;

enum class ExportProgressState : int32_t {
    Running  = 0,
    DoneOk   = 1,
    DoneFail = 2,
    Canceled = 3,
};

struct ExportProgressPacket {
    double progress01 = 0.0;
    int32_t state = 0;
    int32_t errorBytes = 0; // DoneFail 时紧随本包之后的 UTF-8 错误摘要字节数（<= 1024）
};

json watermarkToJson(const WatermarkConfig& wm) {
    return json{
        {"enabled", wm.enabled},
        {"mode", static_cast<int>(wm.mode)},
        {"imagePathUtf8", wm.imagePathUtf8},
        {"textTemplate", wm.textTemplate},
        {"anchor", static_cast<int>(wm.anchor)},
        {"customNormX", wm.customNormX},
        {"customNormY", wm.customNormY},
        {"scale", wm.scale},
        {"opacity", wm.opacity},
        {"marginRatio", wm.marginRatio},
        {"policyForced", wm.policyForced},
    };
}

WatermarkConfig watermarkFromJson(const json& j) {
    WatermarkConfig wm{};
    if (!j.is_object()) return wm;
    wm.enabled = j.value("enabled", wm.enabled);
    wm.mode = static_cast<WatermarkMode>(j.value("mode", static_cast<int>(wm.mode)));
    wm.imagePathUtf8 = j.value("imagePathUtf8", wm.imagePathUtf8);
    wm.textTemplate = j.value("textTemplate", wm.textTemplate);
    wm.anchor = static_cast<WatermarkAnchor>(j.value("anchor", static_cast<int>(wm.anchor)));
    wm.customNormX = j.value("customNormX", wm.customNormX);
    wm.customNormY = j.value("customNormY", wm.customNormY);
    wm.scale = j.value("scale", wm.scale);
    wm.opacity = j.value("opacity", wm.opacity);
    wm.marginRatio = j.value("marginRatio", wm.marginRatio);
    wm.policyForced = j.value("policyForced", wm.policyForced);
    return wm;
}

std::filesystem::path createTempJobFilePath() {
    std::error_code ec;
    std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
    if (ec || tempDir.empty()) {
        tempDir = tools3000::core::WinUtils::getAppDataDirectory() / L"temp";
        std::filesystem::create_directories(tempDir, ec);
    }
    return tempDir / (L"tools3000_studio_export_" + worker::makeUniqueSessionSuffix() + L".json");
}

} // namespace

std::string serializeStudioExportJobJson(const StudioExportJobPayload& job) {
    json j;
    j["schemaVersion"] = 1;
    j["rawInputPathUtf8"] = job.rawInputPathUtf8;
    j["finalOutputPathUtf8"] = job.finalOutputPathUtf8;
    j["traceId"] = job.traceId;

    // 1. ExportOptions
    j["exportOpts"] = json{
        {"format", static_cast<int>(job.exportOpts.format)},
        {"resolution", static_cast<int>(job.exportOpts.resolution)},
        {"fps", job.exportOpts.fps},
        {"quality", static_cast<int>(job.exportOpts.quality)},
        {"baseBitrateMbps", job.exportOpts.baseBitrateMbps},
    };

    // 2. RenderConfig
    const auto& c = job.config;
    j["config"] = json{
        {"autoZoomEnabled", c.autoZoomEnabled},
        {"zoomMode", static_cast<int>(c.zoomMode)},
        {"typingAutoZoom", c.typingAutoZoom},
        {"defaultZoomFactor", c.defaultZoomFactor},
        {"zoomPacing", c.zoomPacing},
        {"autoZoomSensitivity", c.autoZoomSensitivity},
        {"focusSpotlightDim", c.focusSpotlightDim},
        {"lockCameraWhileZoomed", c.lockCameraWhileZoomed},
        {"motionBlurEnabled", c.motionBlurEnabled},
        {"smoothCursorEnabled", c.smoothCursorEnabled},
        {"cursorScale", c.cursorScale},
        {"cursorHighlightHalo", c.cursorHighlightHalo},
        {"showClickRipple", c.showClickRipple},
        {"clickEffectStyle", static_cast<int>(c.clickEffectStyle)},
        {"autoFrameEnabled", c.autoFrameEnabled},
        {"aspectRatio", static_cast<int>(c.aspectRatio)},
        {"backgroundTheme", static_cast<int>(c.backgroundTheme)},
        {"backgroundPresetKey", c.backgroundPresetKey},
        {"customBackgroundPath", c.customBackgroundPath},
        {"paddingRatio", c.paddingRatio},
        {"insetBorderWidth", c.insetBorderWidth},
        {"insetBorderColorHex", c.insetBorderColorHex},
        {"backgroundBlur", c.backgroundBlur},
        {"cornerRadius", c.cornerRadius},
        {"shadowIntensity", c.shadowIntensity},
        {"autoCaptionsEnabled", c.autoCaptionsEnabled},
        {"exportSrtSidecar", c.exportSrtSidecar},
        {"watermark", watermarkToJson(c.watermark)},
    };

    // 3. Timeline
    const auto& t = job.timeline;
    json cursorArr = json::array();
    for (const auto& s : t.cursorSamples) {
        cursorArr.push_back(json::array({s.ptsMs, s.x, s.y, static_cast<int>(s.kind), s.leftDown ? 1 : 0, s.rightDown ? 1 : 0}));
    }
    json clickArr = json::array();
    for (const auto& e : t.clickEvents) {
        clickArr.push_back(json::array({e.ptsMs, e.x, e.y, e.isRight ? 1 : 0}));
    }
    json focusArr = json::array();
    for (const auto& e : t.focusEvents) {
        focusArr.push_back(json::array({e.ptsMs, e.x, e.y}));
    }
    json winArr = json::array();
    for (const auto& e : t.windowEvents) {
        winArr.push_back(json::array({e.ptsMs, e.windowId, e.x, e.y, e.w, e.h}));
    }
    json capArr = json::array();
    for (const auto& e : t.captions) {
        capArr.push_back(json{{"startMs", e.startMs}, {"endMs", e.endMs}, {"text", e.text}});
    }
    json zoomArr = json::array();
    for (const auto& e : t.zoomSegments) {
        zoomArr.push_back(json{
            {"startMs", e.startMs}, {"endMs", e.endMs},
            {"focusX", e.focusX}, {"focusY", e.focusY},
            {"targetZoom", e.targetZoom}, {"enabled", e.enabled},
            {"manualInserted", e.manualInserted},
        });
    }
    json cutArr = json::array();
    for (const auto& e : t.cutSegments) {
        cutArr.push_back(json{
            {"startMs", e.startMs}, {"endMs", e.endMs},
            {"speedFactor", e.speedFactor}, {"enabled", e.enabled},
        });
    }
    json annArr = json::array();
    for (const auto& e : t.annotations) {
        annArr.push_back(json{
            {"kind", static_cast<int>(e.kind)},
            {"startMs", e.startMs}, {"endMs", e.endMs},
            {"x0", e.x0}, {"y0", e.y0}, {"x1", e.x1}, {"y1", e.y1},
            {"colorHex", e.colorHex}, {"text", e.text},
            {"strokeWidth", e.strokeWidth}, {"enabled", e.enabled},
            {"customStepText", e.customStepText},
        });
    }

    j["timeline"] = json{
        {"regionX", t.regionX},
        {"regionY", t.regionY},
        {"sourceWidth", t.sourceWidth},
        {"sourceHeight", t.sourceHeight},
        {"fps", t.fps},
        {"durationMs", t.durationMs},
        {"cursorSamples", std::move(cursorArr)},
        {"clickEvents", std::move(clickArr)},
        {"focusEvents", std::move(focusArr)},
        {"windowEvents", std::move(winArr)},
        {"captions", std::move(capArr)},
        {"zoomSegments", std::move(zoomArr)},
        {"cutSegments", std::move(cutArr)},
        {"annotations", std::move(annArr)},
    };

    return j.dump();
}

bool deserializeStudioExportJobJson(const std::string& jsonText, StudioExportJobPayload& outJob) {
    outJob = {};
    json j = json::parse(jsonText, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return false;
    }

    outJob.rawInputPathUtf8 = j.value("rawInputPathUtf8", std::string{});
    outJob.finalOutputPathUtf8 = j.value("finalOutputPathUtf8", std::string{});
    outJob.traceId = j.value("traceId", std::string{});
    if (outJob.rawInputPathUtf8.empty() || outJob.finalOutputPathUtf8.empty()) {
        return false;
    }

    if (const auto it = j.find("exportOpts"); it != j.end() && it->is_object()) {
        const auto& eo = *it;
        outJob.exportOpts.format = static_cast<StudioExportFormat>(eo.value("format", static_cast<int>(outJob.exportOpts.format)));
        outJob.exportOpts.resolution = static_cast<StudioExportResolution>(eo.value("resolution", static_cast<int>(outJob.exportOpts.resolution)));
        outJob.exportOpts.fps = eo.value("fps", outJob.exportOpts.fps);
        outJob.exportOpts.quality = static_cast<StudioExportQuality>(eo.value("quality", static_cast<int>(outJob.exportOpts.quality)));
        outJob.exportOpts.baseBitrateMbps = eo.value("baseBitrateMbps", outJob.exportOpts.baseBitrateMbps);
    }

    if (const auto it = j.find("config"); it != j.end() && it->is_object()) {
        const auto& c = *it;
        auto& dst = outJob.config;
        dst.autoZoomEnabled = c.value("autoZoomEnabled", dst.autoZoomEnabled);
        dst.zoomMode = static_cast<StudioZoomMode>(c.value("zoomMode", static_cast<int>(dst.zoomMode)));
        dst.typingAutoZoom = c.value("typingAutoZoom", dst.typingAutoZoom);
        dst.defaultZoomFactor = c.value("defaultZoomFactor", dst.defaultZoomFactor);
        dst.zoomPacing = c.value("zoomPacing", dst.zoomPacing);
        dst.autoZoomSensitivity = c.value("autoZoomSensitivity", dst.autoZoomSensitivity);
        dst.focusSpotlightDim = c.value("focusSpotlightDim", dst.focusSpotlightDim);
        dst.lockCameraWhileZoomed = c.value("lockCameraWhileZoomed", dst.lockCameraWhileZoomed);
        dst.motionBlurEnabled = c.value("motionBlurEnabled", dst.motionBlurEnabled);
        dst.smoothCursorEnabled = c.value("smoothCursorEnabled", dst.smoothCursorEnabled);
        dst.cursorScale = c.value("cursorScale", dst.cursorScale);
        dst.cursorHighlightHalo = c.value("cursorHighlightHalo", dst.cursorHighlightHalo);
        dst.showClickRipple = c.value("showClickRipple", dst.showClickRipple);
        dst.clickEffectStyle = static_cast<StudioClickEffectStyle>(c.value("clickEffectStyle", static_cast<int>(dst.clickEffectStyle)));
        dst.autoFrameEnabled = c.value("autoFrameEnabled", dst.autoFrameEnabled);
        dst.aspectRatio = static_cast<StudioAspectRatio>(c.value("aspectRatio", static_cast<int>(dst.aspectRatio)));
        dst.backgroundTheme = static_cast<BeautyBackgroundType>(c.value("backgroundTheme", static_cast<int>(dst.backgroundTheme)));
        dst.backgroundPresetKey = c.value("backgroundPresetKey", dst.backgroundPresetKey);
        dst.customBackgroundPath = c.value("customBackgroundPath", dst.customBackgroundPath);
        dst.paddingRatio = c.value("paddingRatio", dst.paddingRatio);
        dst.insetBorderWidth = c.value("insetBorderWidth", dst.insetBorderWidth);
        dst.insetBorderColorHex = c.value("insetBorderColorHex", dst.insetBorderColorHex);
        dst.backgroundBlur = c.value("backgroundBlur", dst.backgroundBlur);
        dst.cornerRadius = c.value("cornerRadius", dst.cornerRadius);
        dst.shadowIntensity = c.value("shadowIntensity", dst.shadowIntensity);
        dst.autoCaptionsEnabled = c.value("autoCaptionsEnabled", dst.autoCaptionsEnabled);
        dst.exportSrtSidecar = c.value("exportSrtSidecar", dst.exportSrtSidecar);
        if (const auto wIt = c.find("watermark"); wIt != c.end()) {
            dst.watermark = watermarkFromJson(*wIt);
        }
    }

    if (const auto it = j.find("timeline"); it != j.end() && it->is_object()) {
        const auto& t = *it;
        auto& dst = outJob.timeline;
        dst.regionX = t.value("regionX", dst.regionX);
        dst.regionY = t.value("regionY", dst.regionY);
        dst.sourceWidth = t.value("sourceWidth", dst.sourceWidth);
        dst.sourceHeight = t.value("sourceHeight", dst.sourceHeight);
        dst.fps = t.value("fps", dst.fps);
        dst.durationMs = t.value("durationMs", dst.durationMs);

        if (const auto arr = t.find("cursorSamples"); arr != t.end() && arr->is_array()) {
            dst.cursorSamples.reserve(arr->size());
            for (const auto& row : *arr) {
                if (!row.is_array() || row.size() < 6) continue;
                StudioCursorSample s{};
                s.ptsMs = row[0].get<int64_t>();
                s.x = row[1].get<float>();
                s.y = row[2].get<float>();
                s.kind = static_cast<StudioCursorKind>(row[3].get<int>());
                s.leftDown = row[4].get<int>() != 0;
                s.rightDown = row[5].get<int>() != 0;
                dst.cursorSamples.push_back(s);
            }
        }
        if (const auto arr = t.find("clickEvents"); arr != t.end() && arr->is_array()) {
            dst.clickEvents.reserve(arr->size());
            for (const auto& row : *arr) {
                if (!row.is_array() || row.size() < 4) continue;
                dst.clickEvents.push_back({row[0].get<int64_t>(), row[1].get<float>(), row[2].get<float>(), row[3].get<int>() != 0});
            }
        }
        if (const auto arr = t.find("focusEvents"); arr != t.end() && arr->is_array()) {
            dst.focusEvents.reserve(arr->size());
            for (const auto& row : *arr) {
                if (!row.is_array() || row.size() < 3) continue;
                dst.focusEvents.push_back({row[0].get<int64_t>(), row[1].get<float>(), row[2].get<float>()});
            }
        }
        if (const auto arr = t.find("windowEvents"); arr != t.end() && arr->is_array()) {
            dst.windowEvents.reserve(arr->size());
            for (const auto& row : *arr) {
                if (!row.is_array() || row.size() < 6) continue;
                dst.windowEvents.push_back({
                    row[0].get<int64_t>(), row[1].get<uint64_t>(),
                    row[2].get<float>(), row[3].get<float>(),
                    row[4].get<float>(), row[5].get<float>(),
                });
            }
        }
        if (const auto arr = t.find("captions"); arr != t.end() && arr->is_array()) {
            dst.captions.reserve(arr->size());
            for (const auto& obj : *arr) {
                if (!obj.is_object()) continue;
                dst.captions.push_back({obj.value("startMs", int64_t{0}), obj.value("endMs", int64_t{0}), obj.value("text", std::string{})});
            }
        }
        if (const auto arr = t.find("zoomSegments"); arr != t.end() && arr->is_array()) {
            dst.zoomSegments.reserve(arr->size());
            for (const auto& obj : *arr) {
                if (!obj.is_object()) continue;
                StudioZoomSegment z{};
                z.startMs = obj.value("startMs", z.startMs);
                z.endMs = obj.value("endMs", z.endMs);
                z.focusX = obj.value("focusX", z.focusX);
                z.focusY = obj.value("focusY", z.focusY);
                z.targetZoom = obj.value("targetZoom", z.targetZoom);
                z.enabled = obj.value("enabled", z.enabled);
                z.manualInserted = obj.value("manualInserted", z.manualInserted);
                dst.zoomSegments.push_back(z);
            }
        }
        if (const auto arr = t.find("cutSegments"); arr != t.end() && arr->is_array()) {
            dst.cutSegments.reserve(arr->size());
            for (const auto& obj : *arr) {
                if (!obj.is_object()) continue;
                StudioCutSegment cs{};
                cs.startMs = obj.value("startMs", cs.startMs);
                cs.endMs = obj.value("endMs", cs.endMs);
                cs.speedFactor = obj.value("speedFactor", cs.speedFactor);
                cs.enabled = obj.value("enabled", cs.enabled);
                dst.cutSegments.push_back(cs);
            }
        }
        if (const auto arr = t.find("annotations"); arr != t.end() && arr->is_array()) {
            dst.annotations.reserve(arr->size());
            for (const auto& obj : *arr) {
                if (!obj.is_object()) continue;
                StudioAnnotationItem a{};
                a.kind = static_cast<StudioAnnotationKind>(obj.value("kind", static_cast<int>(a.kind)));
                a.startMs = obj.value("startMs", a.startMs);
                a.endMs = obj.value("endMs", a.endMs);
                a.x0 = obj.value("x0", a.x0);
                a.y0 = obj.value("y0", a.y0);
                a.x1 = obj.value("x1", a.x1);
                a.y1 = obj.value("y1", a.y1);
                a.colorHex = obj.value("colorHex", a.colorHex);
                a.text = obj.value("text", a.text);
                a.strokeWidth = obj.value("strokeWidth", a.strokeWidth);
                a.enabled = obj.value("enabled", a.enabled);
                a.customStepText = obj.value("customStepText", a.customStepText);
                dst.annotations.push_back(std::move(a));
            }
        }
    }

    return true;
}

StudioExportWorkerOutcome exportStudioMediaOutOfProcess(
    const std::string& rawInputPathUtf8,
    const std::string& finalOutputPathUtf8,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    const StudioExportOptions& exportOpts,
    const std::function<void(double progress01)>& onProgress,
    const std::atomic<bool>* cancelFlag,
    const std::filesystem::path& workerExeOverride) {
    StudioExportWorkerOutcome outcome{};
    if (worker::isRunningInsideWorkerProcess() || worker::envFlagEnabled(L"TOOLS3000_STUDIO_EXPORT_INPROC")) {
        return outcome;
    }

    const std::filesystem::path exePath = worker::resolveHostWorkerExecutablePath(workerExeOverride);
    if (exePath.empty()) {
        return outcome;
    }

    StudioExportJobPayload job{};
    job.rawInputPathUtf8 = rawInputPathUtf8;
    job.finalOutputPathUtf8 = finalOutputPathUtf8;
    job.traceId = tools3000::core::TraceId::current();
    job.timeline = timeline;
    job.config = config;
    job.exportOpts = exportOpts;

    const std::filesystem::path jobFilePath = createTempJobFilePath();
    struct TempFileCleanup {
        std::filesystem::path path;
        ~TempFileCleanup() {
            std::error_code ec;
            if (!path.empty()) std::filesystem::remove(path, ec);
        }
    } tempGuard{jobFilePath};

    {
        const std::string payload = serializeStudioExportJobJson(job);
        std::ofstream ofs(jobFilePath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return outcome;
        }
        ofs.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        ofs.flush();
        if (!ofs) {
            return outcome;
        }
    }

    const std::wstring pipeName = worker::makeLocalPipeName(L"StudioExport");
    const std::wstring cancelEvtName = worker::makeLocalObjectName(L"StudioCancel");

    worker::ScopedHandle pipeServer(CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        worker::Constants::kPipeBufferBytes,
        worker::Constants::kPipeBufferBytes,
        0,
        nullptr));
    worker::ScopedHandle cancelEvent(CreateEventW(nullptr, TRUE, FALSE, cancelEvtName.c_str()));
    if (!pipeServer || !cancelEvent) {
        return outcome;
    }

    const std::vector<std::wstring> args = {
        std::wstring(worker::Constants::kFlagWorkerMode) + worker::Constants::kModeStudioExport,
        std::wstring(worker::Constants::kFlagJobFile) + jobFilePath.wstring(),
        std::wstring(worker::Constants::kFlagPipeName) + pipeName,
        std::wstring(worker::Constants::kFlagCancelEvent) + cancelEvtName,
        std::wstring(worker::Constants::kFlagParentPid) + std::to_wstring(GetCurrentProcessId()),
    };

    worker::ScopedHandle childProc;
    DWORD childPid = 0;
    if (!worker::launchHiddenWorkerProcess(exePath, args, childProc, &childPid)) {
        LOG_WARN("StudioExportWorker: failed to spawn isolated export worker (err={})", GetLastError());
        return outcome;
    }

    if (!worker::waitForPipeClientWithTimeout(
            pipeServer.get(),
            childProc.get(),
            worker::Constants::kPipeConnectTimeoutMs)) {
        LOG_WARN("StudioExportWorker: timed out waiting for export worker pipe handshake (pid={})", childPid);
        TerminateProcess(childProc.get(), 1);
        return outcome;
    }

    outcome.handledOutOfProcess = true;
    LOG_INFO("StudioExportWorker: isolated export worker connected (pid={}, format={})",
             childPid, static_cast<int>(exportOpts.format));

    // 取消标志同步监听线程：每 35ms 映射宿主 cancelFlag -> 命名取消事件
    std::atomic<bool> stopCancelRelay{false};
    std::thread cancelRelay;
    if (cancelFlag != nullptr) {
        cancelRelay = std::thread([cancelFlag, &stopCancelRelay, evt = cancelEvent.get()]() {
            while (!stopCancelRelay.load(std::memory_order_acquire)) {
                if (cancelFlag->load(std::memory_order_acquire)) {
                    SetEvent(evt);
                    break;
                }
                Sleep(35);
            }
        });
    }

    bool sawTerminalPacket = false;
    while (true) {
        ExportProgressPacket pkt{};
        if (!worker::readPod(pipeServer.get(), pkt, /*overlapped=*/true)) {
            break;
        }
        if (onProgress) {
            onProgress(std::clamp(pkt.progress01, 0.0, 1.0));
        }
        const auto st = static_cast<ExportProgressState>(pkt.state);
        if (st == ExportProgressState::DoneOk) {
            outcome.succeeded = true;
            sawTerminalPacket = true;
            break;
        }
        if (st == ExportProgressState::DoneFail || st == ExportProgressState::Canceled) {
            outcome.succeeded = false;
            if (st == ExportProgressState::DoneFail && pkt.errorBytes > 0 && pkt.errorBytes <= 1024) {
                std::string errBuf(static_cast<size_t>(pkt.errorBytes), '\0');
                if (worker::readExact(pipeServer.get(), errBuf.data(), static_cast<DWORD>(pkt.errorBytes), /*overlapped=*/true)) {
                    outcome.errorDetail = std::move(errBuf);
                }
            }
            sawTerminalPacket = true;
            break;
        }
    }

    stopCancelRelay.store(true, std::memory_order_release);
    if (cancelRelay.joinable()) {
        cancelRelay.join();
    }

    DisconnectNamedPipe(pipeServer.get());
    pipeServer.reset();

    if (WaitForSingleObject(childProc.get(), worker::Constants::kWorkerGracefulExitWaitMs) == WAIT_TIMEOUT) {
        TerminateProcess(childProc.get(), 1);
        WaitForSingleObject(childProc.get(), 1000);
    }
    if (!sawTerminalPacket) {
        DWORD exitCode = 1;
        if (GetExitCodeProcess(childProc.get(), &exitCode) && exitCode == 0) {
            outcome.succeeded = true;
        } else if (outcome.errorDetail.empty()) {
            outcome.errorDetail = "worker_exited_unexpectedly(code=" + std::to_string(exitCode) + ")";
        }
    }
    if (!outcome.succeeded && (!cancelFlag || !cancelFlag->load(std::memory_order_acquire))) {
        LOG_WARN("StudioExportWorker: export failed (pid={}, format={}, res={}, fps={}, quality={}, detail={})",
                 childPid,
                 static_cast<int>(exportOpts.format),
                 static_cast<int>(exportOpts.resolution),
                 exportOpts.fps,
                 static_cast<int>(exportOpts.quality),
                 outcome.errorDetail.empty() ? "unknown" : outcome.errorDetail);
    }
    return outcome;
}

int runStudioExportWorkerProcess(int argc, const wchar_t* const* argv) {
    const std::wstring jobPathW = worker::findCliOptionValue(argc, argv, worker::Constants::kFlagJobFile);
    const std::wstring pipeName = worker::findCliOptionValue(argc, argv, worker::Constants::kFlagPipeName);
    const std::wstring cancelEvtName = worker::findCliOptionValue(argc, argv, worker::Constants::kFlagCancelEvent);
    const std::wstring parentPidStr = worker::findCliOptionValue(argc, argv, worker::Constants::kFlagParentPid);
    const DWORD parentPid = parentPidStr.empty() ? 0u : static_cast<DWORD>(std::wcstoul(parentPidStr.c_str(), nullptr, 10));
    if (jobPathW.empty() || pipeName.empty()) {
        return 2;
    }

    std::atomic<bool> cancelFlag{false};
    worker::ParentProcessWatchdog watchdog(parentPid, [&cancelFlag]() {
        cancelFlag.store(true, std::memory_order_release);
    });

    worker::ScopedHandle pipe = worker::connectPipeClientWithRetry(pipeName, worker::Constants::kPipeConnectTimeoutMs);
    if (!pipe) {
        return 3;
    }

    worker::ScopedHandle cancelEvent;
    std::atomic<bool> stopCancelMonitor{false};
    std::thread cancelMonitor;
    if (!cancelEvtName.empty()) {
        cancelEvent.reset(OpenEventW(SYNCHRONIZE, FALSE, cancelEvtName.c_str()));
        if (cancelEvent) {
            cancelMonitor = std::thread([evt = cancelEvent.get(), &cancelFlag, &stopCancelMonitor]() {
                while (!stopCancelMonitor.load(std::memory_order_acquire)) {
                    if (WaitForSingleObject(evt, 50) == WAIT_OBJECT_0) {
                        cancelFlag.store(true, std::memory_order_release);
                        break;
                    }
                }
            });
        }
    }

    auto sendPacket = [&pipe](double progress01, ExportProgressState st, const std::string& errDetail = {}) {
        const size_t clampedErrLen = (st == ExportProgressState::DoneFail)
            ? (std::min<size_t>)(errDetail.size(), 1024u)
            : 0u;
        ExportProgressPacket pkt{
            std::clamp(progress01, 0.0, 1.0),
            static_cast<int32_t>(st),
            static_cast<int32_t>(clampedErrLen)
        };
        if (worker::writePod(pipe.get(), pkt, /*overlapped=*/false) && clampedErrLen > 0) {
            worker::writeExact(pipe.get(), errDetail.data(), static_cast<DWORD>(clampedErrLen), /*overlapped=*/false);
        }
    };

    bool ok = false;
    std::string exportErrorDetail;
    {
        std::ifstream ifs(std::filesystem::path(jobPathW), std::ios::binary);
        std::ostringstream oss;
        if (ifs) {
            oss << ifs.rdbuf();
        }
        StudioExportJobPayload job{};
        if (deserializeStudioExportJobJson(oss.str(), job)) {
            std::optional<tools3000::core::TraceId::Scope> traceScope;
            if (!job.traceId.empty()) {
                traceScope.emplace(job.traceId);
            }
            double lastReported = -1.0;
            ok = StudioVideoCompositor::exportStudioMedia(
                job.rawInputPathUtf8,
                job.finalOutputPathUtf8,
                job.timeline,
                job.config,
                job.exportOpts,
                [&](double p) {
                    const double clamped = std::clamp(p, 0.0, 1.0);
                    if (clamped - lastReported >= 0.005 || clamped >= 0.999) {
                        lastReported = clamped;
                        sendPacket(clamped, ExportProgressState::Running);
                    }
                },
                &cancelFlag,
                &exportErrorDetail);
        } else {
            exportErrorDetail = "invalid_export_job_payload";
        }
    }

    stopCancelMonitor.store(true, std::memory_order_release);
    if (cancelMonitor.joinable()) {
        cancelMonitor.join();
    }

    const bool canceled = cancelFlag.load(std::memory_order_acquire);
    sendPacket(
        ok ? 1.0 : 0.0,
        canceled ? ExportProgressState::Canceled : (ok ? ExportProgressState::DoneOk : ExportProgressState::DoneFail),
        exportErrorDetail);
    FlushFileBuffers(pipe.get());
    pipe.reset();
    watchdog.stop();
    return (ok && !canceled) ? 0 : 1;
}

} // namespace tools3000::capture
