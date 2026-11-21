// ─────────────────────────────────────────────────────────────────────────────
// OcrWorkerHost.cpp — 独立短命 OCR Worker 进程服务端主循环实现
//
// 职责:
//   1. 连接宿主创建的命名管道，等待零拷贝共享内存图像识别请求
//   2. 复用进程内 DeepOcrEngine (DirectML GPU + CompactBatchCascade) 完成高精度识别并序列化回传
//   3. 连续 idleSec 秒（默认 60s）无新请求、收到 Shutdown 或父进程退出时直接结束进程，
//      由操作系统 100% 彻底回收 DirectML / ONNX Runtime / 显卡驱动全部虚拟提交与线程
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. MIT License.
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/OcrWorkerHost.h"
#include "ocr/OcrWorkerProtocol.h"
#include "ocr/DeepOcrEngine.h"
#include "capture/CaptureWorkerCommon.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tools3000::ocr {

namespace {

worker::ScopedHandle connectOverlappedPipeClient(const std::wstring& pipeName, DWORD timeoutMs) {
    const uint64_t deadline = GetTickCount64() + timeoutMs;
    while (true) {
        HANDLE h = CreateFileW(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            return worker::ScopedHandle(h);
        }
        const DWORD err = GetLastError();
        if (GetTickCount64() >= deadline) {
            return {};
        }
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipeName.c_str(), 100);
        } else if (err == ERROR_FILE_NOT_FOUND) {
            Sleep(10);
        } else {
            return {};
        }
    }
}

} // namespace

int runOcrWorkerProcess(int argc, const wchar_t* const* argv) {
    const std::wstring pipeName = worker::findCliOptionValue(argc, argv, worker::Constants::kFlagPipeName);
    if (pipeName.empty()) {
        return 2;
    }

    const std::wstring parentPidStr = worker::findCliOptionValue(argc, argv, worker::Constants::kFlagParentPid);
    const DWORD parentPid = parentPidStr.empty() ? 0u : static_cast<DWORD>(std::wcstoul(parentPidStr.c_str(), nullptr, 10));

    const std::wstring idleStr = worker::findCliOptionValue(argc, argv, worker::Constants::kFlagIdleSeconds);
    int idleSec = worker::Constants::kDefaultOcrWorkerIdleSeconds;
    if (!idleStr.empty()) {
        const int parsed = std::wcstol(idleStr.c_str(), nullptr, 10);
        if (parsed >= 1 && parsed <= 3600) {
            idleSec = parsed;
        }
    }

    worker::ScopedHandle abortEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    worker::ParentProcessWatchdog watchdog(parentPid, [&abortEvent]() {
        if (abortEvent) SetEvent(abortEvent.get());
    });

    // Worker 自身靠进程退出回收 100% 驱动内存，禁用内部二次闲置卸载线程以免重复竞态
    DeepOcrEngine::instance().setIdleUnloadTimeoutForTest(0);

    worker::ScopedHandle pipe = connectOverlappedPipeClient(pipeName, worker::Constants::kPipeConnectTimeoutMs);
    if (!pipe) {
        return 3;
    }

    const DWORD idleTimeoutMs = static_cast<DWORD>(idleSec) * 1000u;

    while (true) {
        worker_proto::RequestHeader req{};
        if (!worker::readPod(pipe.get(), req, /*overlapped=*/true, idleTimeoutMs, abortEvent.get())) {
            // 超时（60s 无请求）、管道关闭或父进程死亡均视为正常收官退场
            break;
        }
        if (req.magic != worker_proto::kProtocolMagic || req.version != worker_proto::kProtocolVersion) {
            break;
        }

        const auto cmd = static_cast<worker_proto::Command>(req.command);
        if (cmd == worker_proto::Command::Shutdown) {
            break;
        }
        if (cmd == worker_proto::Command::Ping) {
            worker_proto::ResponseHeader resp{};
            resp.status = 0;
            resp.lineCount = 0;
            if (!worker::writePod(pipe.get(), resp, /*overlapped=*/true)) {
                break;
            }
            continue;
        }
        if (cmd != worker_proto::Command::Recognize) {
            break;
        }

        worker_proto::RecognizeMeta meta{};
        std::string shmNameUtf8;
        std::string modelDirUtf8;
        std::string modelTier;
        std::string traceId;
        if (!worker::readPod(pipe.get(), meta, /*overlapped=*/true, 10000, abortEvent.get()) ||
            !worker::readString(pipe.get(), shmNameUtf8, /*overlapped=*/true, 10000, abortEvent.get()) ||
            !worker::readString(pipe.get(), modelDirUtf8, /*overlapped=*/true, 10000, abortEvent.get()) ||
            !worker::readString(pipe.get(), modelTier, /*overlapped=*/true, 10000, abortEvent.get()) ||
            !worker::readString(pipe.get(), traceId, /*overlapped=*/true, 10000, abortEvent.get())) {
            break;
        }

        if (meta.width <= 0 || meta.height <= 0 || meta.channels != 3 || meta.stepBytes < meta.width * 3 ||
            meta.imageBytes < static_cast<uint64_t>(meta.stepBytes) * static_cast<uint64_t>(meta.height)) {
            worker_proto::ResponseHeader errResp{};
            errResp.status = 1;
            worker::writePod(pipe.get(), errResp, /*overlapped=*/true);
            continue;
        }

        const std::wstring shmName = tools3000::core::WinUtils::utf8ToWstring(shmNameUtf8);
        worker::ScopedHandle shm(OpenFileMappingW(FILE_MAP_READ, FALSE, shmName.c_str()));
        worker::ScopedMapView view(shm ? MapViewOfFile(shm.get(), FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(meta.imageBytes)) : nullptr);
        if (!view) {
            worker_proto::ResponseHeader errResp{};
            errResp.status = 2;
            worker::writePod(pipe.get(), errResp, /*overlapped=*/true);
            continue;
        }

        // 透传 TraceID、自定义模型目录与档位配置
        std::optional<tools3000::core::TraceId::Scope> traceScope;
        if (!traceId.empty()) {
            traceScope.emplace(traceId);
        }
        if (!modelDirUtf8.empty()) {
            DeepOcrEngine::instance().setCustomModelDirectoryForTest(
                std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(modelDirUtf8)));
        }
        if (!modelTier.empty()) {
            DeepOcrEngine::instance().setModelTierOverrideForWorker(modelTier);
        }

        // 只读共享内存直接包装 cv::Mat 头（零拷贝！）
        cv::Mat sharedView(meta.height, meta.width, CV_8UC3, view.bytes(), static_cast<size_t>(meta.stepBytes));
        const std::vector<OcrResult> results = DeepOcrEngine::instance().extractText(sharedView);
        const DeepOcrRunStats stats = DeepOcrEngine::instance().lastRunStats();
        const std::string activeTier = DeepOcrEngine::instance().effectiveModelTier();

        view.reset();
        shm.reset();

        if (!worker_proto::writeOcrResults(pipe.get(), results, stats, activeTier, /*overlapped=*/true)) {
            break;
        }
    }

    pipe.reset();
    DeepOcrEngine::instance().shutdown();
    watchdog.stop();
    return 0;
}

} // namespace tools3000::ocr
