// ─────────────────────────────────────────────────────────────────────────────
// OcrWorkerClient.cpp — 宿主侧独立短命 OCR Worker 进程客户端实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/OcrWorkerClient.h"
#include "ocr/OcrWorkerProtocol.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <chrono>
#include <cstring>

namespace tools3000::ocr {

namespace {

constexpr DWORD kOcrRequestTimeoutMs = 45000; // 单图冷启/推理安全超时 45s，防止驱动挂死长期阻塞

} // namespace

OcrWorkerClient& OcrWorkerClient::instance() {
    static OcrWorkerClient inst;
    return inst;
}

OcrWorkerClient::~OcrWorkerClient() {
    shutdownWorker();
}

void OcrWorkerClient::setTestOverride(
    bool enabled,
    const std::filesystem::path& workerExePath,
    int idleSecondsOverride) {
    shutdownWorker();
    m_safeCpuModeLatched.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_testOverrideSet = true;
    m_testOverrideEnabled = enabled;
    m_testWorkerExePath = workerExePath;
    m_idleSecondsOverride = idleSecondsOverride;
}

bool OcrWorkerClient::isSafeCpuModeLatched() const noexcept {
    return m_safeCpuModeLatched.load(std::memory_order_acquire);
}

void OcrWorkerClient::resetSafeCpuModeForTest() noexcept {
    m_safeCpuModeLatched.store(false, std::memory_order_release);
}

bool OcrWorkerClient::isOutOfProcessAvailable() const {
    if (worker::isRunningInsideWorkerProcess()) {
        return false;
    }
    if (worker::envFlagEnabled(L"TOOLS3000_OCR_INPROC")) {
        return false;
    }
    std::filesystem::path overrideExe;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        if (m_testOverrideSet) {
            if (!m_testOverrideEnabled) return false;
            overrideExe = m_testWorkerExePath;
        }
    }
    return !worker::resolveHostWorkerExecutablePath(overrideExe).empty();
}

bool OcrWorkerClient::isChildAliveLocked() const noexcept {
    if (!m_childProcess) return false;
    return WaitForSingleObject(m_childProcess.get(), 0) == WAIT_TIMEOUT;
}

bool OcrWorkerClient::isWorkerRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return isChildAliveLocked();
}

DWORD OcrWorkerClient::activeWorkerPid() const noexcept {
    return m_childPid.load(std::memory_order_acquire);
}

void OcrWorkerClient::closeWorkerLocked(bool sendShutdownCmd) {
    if (m_pipe) {
        if (sendShutdownCmd && isChildAliveLocked()) {
            worker_proto::RequestHeader req{};
            req.command = static_cast<uint32_t>(worker_proto::Command::Shutdown);
            worker::writePod(m_pipe.get(), req, /*overlapped=*/true);
            FlushFileBuffers(m_pipe.get());
        }
        DisconnectNamedPipe(m_pipe.get());
        m_pipe.reset();
    }
    if (m_childProcess) {
        if (WaitForSingleObject(m_childProcess.get(), worker::Constants::kWorkerGracefulExitWaitMs) == WAIT_TIMEOUT) {
            TerminateProcess(m_childProcess.get(), 0);
            WaitForSingleObject(m_childProcess.get(), 1000);
        }
        m_childProcess.reset();
    }
    m_childPid.store(0, std::memory_order_release);
    m_workerTier.clear();
    m_workerModelDir.clear();
    m_workerSafeCpuMode = false;
}

void OcrWorkerClient::shutdownWorker() {
    std::lock_guard<std::mutex> lock(m_mutex);
    closeWorkerLocked(/*sendShutdownCmd=*/true);
}

bool OcrWorkerClient::ensureWorkerConnectedLocked(
    const std::filesystem::path& exePath,
    const std::string& modelTier,
    const std::filesystem::path& modelDir,
    bool safeCpuMode,
    bool* outReusedExisting) {
    if (outReusedExisting) *outReusedExisting = false;
    if (m_pipe && isChildAliveLocked() &&
        m_workerTier == modelTier &&
        m_workerModelDir == modelDir &&
        m_workerSafeCpuMode == safeCpuMode) {
        if (outReusedExisting) *outReusedExisting = true;
        return true;
    }
    closeWorkerLocked(/*sendShutdownCmd=*/true);

    const std::wstring pipeName = worker::makeLocalPipeName(L"OcrWorker");
    worker::ScopedHandle pipeServer(CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        worker::Constants::kPipeBufferBytes,
        worker::Constants::kPipeBufferBytes,
        0,
        nullptr));
    if (!pipeServer) {
        LOG_WARN("OcrWorkerClient: CreateNamedPipeW failed (err={})", GetLastError());
        return false;
    }

    int idleSec = worker::Constants::kDefaultOcrWorkerIdleSeconds;
    {
        std::lock_guard<std::mutex> cfgLock(m_configMutex);
        if (m_idleSecondsOverride > 0) {
            idleSec = m_idleSecondsOverride;
        }
    }

    std::vector<std::wstring> args = {
        std::wstring(worker::Constants::kFlagWorkerMode) + worker::Constants::kModeOcr,
        std::wstring(worker::Constants::kFlagPipeName) + pipeName,
        std::wstring(worker::Constants::kFlagParentPid) + std::to_wstring(GetCurrentProcessId()),
        std::wstring(worker::Constants::kFlagIdleSeconds) + std::to_wstring(idleSec),
    };
    if (safeCpuMode) {
        args.push_back(std::wstring(worker::Constants::kFlagSafeCpuMode) + L"1");
    }

    worker::ScopedHandle childProc;
    DWORD childPid = 0;
    if (!worker::launchHiddenWorkerProcess(
            exePath, args, childProc, &childPid, ABOVE_NORMAL_PRIORITY_CLASS)) {
        LOG_WARN("OcrWorkerClient: failed to spawn OCR worker process (err={})", GetLastError());
        return false;
    }

    if (!worker::waitForPipeClientWithTimeout(
            pipeServer.get(),
            childProc.get(),
            worker::Constants::kPipeConnectTimeoutMs)) {
        LOG_WARN("OcrWorkerClient: timed out waiting for OCR worker pipe handshake (pid={})", childPid);
        TerminateProcess(childProc.get(), 1);
        return false;
    }

    m_pipe = std::move(pipeServer);
    m_childProcess = std::move(childProc);
    m_childPid.store(childPid, std::memory_order_release);
    m_workerTier = modelTier;
    m_workerModelDir = modelDir;
    m_workerSafeCpuMode = safeCpuMode;
    LOG_INFO("OcrWorkerClient: isolated OCR worker started (pid={}, idleTimeoutSec={}, tier={}, safeCpu={})",
             childPid, idleSec, modelTier, safeCpuMode ? "true" : "false");
    return true;
}

bool OcrWorkerClient::runSingleRequestLocked(
    const cv::Mat& bgrImage,
    const std::filesystem::path& modelDir,
    const std::string& modelTier,
    const std::string& scriptMode,
    std::vector<OcrResult>& outResults,
    DeepOcrRunStats& outStats,
    std::string& outActiveTier,
    const OcrStageCallback& onStage) {
    if (!m_pipe || !isChildAliveLocked()) return false;
    if (bgrImage.empty() || bgrImage.type() != CV_8UC3) return false;

    const cv::Mat contiguous = bgrImage.isContinuous() ? bgrImage : bgrImage.clone();
    const uint64_t imageBytes = static_cast<uint64_t>(contiguous.total()) * static_cast<uint64_t>(contiguous.elemSize());
    if (imageBytes == 0 || imageBytes > (256ULL * 1024ULL * 1024ULL)) {
        return false;
    }

    const std::wstring shmName = worker::makeLocalObjectName(L"OcrImg");
    const DWORD sizeHigh = static_cast<DWORD>(imageBytes >> 32);
    const DWORD sizeLow = static_cast<DWORD>(imageBytes & 0xFFFFFFFFULL);
    worker::ScopedHandle shmHandle(CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        sizeHigh,
        sizeLow,
        shmName.c_str()));
    if (!shmHandle) {
        return false;
    }

    worker::ScopedMapView view(MapViewOfFile(shmHandle.get(), FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(imageBytes)));
    if (!view) {
        return false;
    }
    std::memcpy(view.bytes(), contiguous.data, static_cast<size_t>(imageBytes));
    view.reset();

    worker_proto::RequestHeader req{};
    req.command = static_cast<uint32_t>(worker_proto::Command::Recognize);
    worker_proto::RecognizeMeta meta{};
    meta.width = contiguous.cols;
    meta.height = contiguous.rows;
    meta.channels = contiguous.channels();
    meta.stepBytes = static_cast<int32_t>(contiguous.step[0]);
    meta.imageBytes = imageBytes;

    const std::string shmNameUtf8 = tools3000::core::WinUtils::wstringToUtf8(shmName);
    const std::string modelDirUtf8 = tools3000::core::WinUtils::wstringToUtf8(modelDir.wstring());
    const std::string traceId = tools3000::core::TraceId::current();

    if (!worker::writePod(m_pipe.get(), req, /*overlapped=*/true) ||
        !worker::writePod(m_pipe.get(), meta, /*overlapped=*/true) ||
        !worker::writeString(m_pipe.get(), shmNameUtf8, /*overlapped=*/true) ||
        !worker::writeString(m_pipe.get(), modelDirUtf8, /*overlapped=*/true) ||
        !worker::writeString(m_pipe.get(), modelTier, /*overlapped=*/true) ||
        !worker::writeString(m_pipe.get(), scriptMode, /*overlapped=*/true) ||
        !worker::writeString(m_pipe.get(), traceId, /*overlapped=*/true)) {
        return false;
    }

    return worker_proto::readOcrResults(
        m_pipe.get(),
        outResults,
        outStats,
        outActiveTier,
        /*overlapped=*/true,
        kOcrRequestTimeoutMs,
        m_childProcess.get(),
        onStage);
}

OcrWorkerExtractStatus OcrWorkerClient::extractTextEx(
    const cv::Mat& bgrImage,
    const std::filesystem::path& modelDir,
    const std::string& modelTier,
    const std::string& scriptMode,
    std::vector<OcrResult>& outResults,
    DeepOcrRunStats& outStats,
    std::string& outActiveTier,
    const OcrStageCallback& onStage) {
    if (!isOutOfProcessAvailable()) {
        return OcrWorkerExtractStatus::WorkerUnavailable;
    }

    std::filesystem::path overrideExe;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        if (m_testOverrideSet) {
            overrideExe = m_testWorkerExePath;
        }
    }
    const std::filesystem::path exePath = worker::resolveHostWorkerExecutablePath(overrideExe);
    if (exePath.empty()) {
        return OcrWorkerExtractStatus::WorkerUnavailable;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto tWallStart = std::chrono::steady_clock::now();
    bool safeCpu = m_safeCpuModeLatched.load(std::memory_order_acquire);
    bool triedFreshNormal = false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        const bool willColdSpawn = !(m_pipe && isChildAliveLocked() &&
            m_workerTier == modelTier &&
            m_workerModelDir == modelDir &&
            m_workerSafeCpuMode == safeCpu);
        if (willColdSpawn && onStage) {
            onStage(OcrRecognitionStage::DeepModelLoading, 0u, safeCpu ? "worker_cpu" : "worker");
        }
        bool reusedExisting = false;
        if (!ensureWorkerConnectedLocked(exePath, modelTier, modelDir, safeCpu, &reusedExisting)) {
            if (!safeCpu) {
                m_safeCpuModeLatched.store(true, std::memory_order_release);
                safeCpu = true;
                LOG_WARN("OcrWorkerClient: OCR worker startup/handshake failed; retrying in CPU-only safe mode");
                continue;
            }
            LOG_ERROR("OcrWorkerClient: isolated OCR worker unavailable even in CPU-only safe mode; refusing unsafe in-process fallback");
            return OcrWorkerExtractStatus::WorkerFailed;
        }

        if (runSingleRequestLocked(bgrImage, modelDir, modelTier, scriptMode, outResults, outStats, outActiveTier, onStage)) {
            const long long wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tWallStart).count();
            LOG_INFO("DeepOcrEngine: isolated worker (pid={}, safeCpu={}) recognized {} lines ({} quads -> {} strips, script={}, autoSwitched={}, refineCandidates={}, serverRefined={}) on {}x{} in {} ms (wall={} ms, load={} ms, det={} ms, pack={} ms, rec={} ms, refine={} ms, sessions={}, tier={})",
                     m_childPid.load(std::memory_order_acquire),
                     safeCpu ? "true" : "false",
                     outResults.size(),
                     outStats.detectedQuads,
                     outStats.stripCount,
                     outStats.detectedScript,
                     outStats.autoScriptSwitched ? "true" : "false",
                     outStats.serverRefineCandidateCount,
                     outStats.serverRefinedCount,
                     bgrImage.cols,
                     bgrImage.rows,
                     outStats.totalMs,
                     wallMs,
                     outStats.loadMs,
                     outStats.detMs,
                     outStats.packMs,
                     outStats.recMs,
                     outStats.refineMs,
                     outStats.activeSessions,
                     outActiveTier);
            return OcrWorkerExtractStatus::Succeeded;
        }

        DWORD exitCode = STILL_ACTIVE;
        if (m_childProcess) {
            GetExitCodeProcess(m_childProcess.get(), &exitCode);
        }
        const DWORD failedPid = m_childPid.load(std::memory_order_acquire);
        closeWorkerLocked(/*sendShutdownCmd=*/false);

        if (reusedExisting && !triedFreshNormal) {
            // 复用的老子进程可能刚好在请求临界点触发闲置自退出，先原模式干净重启一次
            triedFreshNormal = true;
            LOG_INFO("OcrWorkerClient: reused OCR worker (pid={}, exitCode=0x{:X}) disconnected, restarting fresh worker",
                     failedPid, static_cast<unsigned int>(exitCode));
            continue;
        }

        if (!safeCpu) {
            // 新拉起的子进程在推理中崩溃、异常退出或超时：自动闩锁纯 CPU 安全模式再隔离重试一次，彻底绕过问题 GPU/DirectML 驱动
            m_safeCpuModeLatched.store(true, std::memory_order_release);
            safeCpu = true;
            LOG_WARN("OcrWorkerClient: isolated OCR worker (pid={}, exitCode=0x{:X}) crashed or timed out during inference; latching CPU-only safe worker mode and retrying once",
                     failedPid, static_cast<unsigned int>(exitCode));
            continue;
        }

        LOG_ERROR("OcrWorkerClient: isolated OCR worker failed even in CPU-only safe mode (pid={}, exitCode=0x{:X}); refusing unsafe in-process fallback",
                  failedPid, static_cast<unsigned int>(exitCode));
        break;
    }

    return OcrWorkerExtractStatus::WorkerFailed;
}

bool OcrWorkerClient::extractText(
    const cv::Mat& bgrImage,
    const std::filesystem::path& modelDir,
    const std::string& modelTier,
    const std::string& scriptMode,
    std::vector<OcrResult>& outResults,
    DeepOcrRunStats& outStats,
    std::string& outActiveTier) {
    return extractTextEx(bgrImage, modelDir, modelTier, scriptMode, outResults, outStats, outActiveTier) ==
           OcrWorkerExtractStatus::Succeeded;
}

} // namespace tools3000::ocr
