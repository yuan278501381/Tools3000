// ─────────────────────────────────────────────────────────────────────────────
// CaptureWorkerEntry.cpp — Plugin_Capture.dll 隔离短命 Worker 统一导出分发器
//
// 职责:
//   以策略表多态分发 Tools3000.exe --worker=<mode> 请求至:
//     - ocr            -> runOcrWorkerProcess
//     - studio-export  -> runStudioExportWorkerProcess
//     - record-encode  -> runRecordEncodeWorkerProcess
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. MIT License.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/CaptureWorkerCommon.h"
#include "capture/RecordEncodeWorker.h"
#include "capture/StudioExportWorker.h"
#include "core/plugin/IPlugin.h"
#include "ocr/OcrWorkerHost.h"

#include <array>
#include <string_view>

namespace {

using WorkerHandlerFn = int (*)(int argc, const wchar_t* const* argv);

struct WorkerStrategyEntry {
    std::wstring_view mode;
    WorkerHandlerFn handler;
};

constexpr std::array<WorkerStrategyEntry, 3> kWorkerStrategies = {{
    {tools3000::worker::Constants::kModeOcr,          &tools3000::ocr::runOcrWorkerProcess},
    {tools3000::worker::Constants::kModeStudioExport, &tools3000::capture::runStudioExportWorkerProcess},
    {tools3000::worker::Constants::kModeRecordEncode, &tools3000::capture::runRecordEncodeWorkerProcess},
}};

} // namespace

extern "C" PLUGIN_API int Tools3000RunCaptureWorker(int argc, const wchar_t* const* argv) {
    tools3000::worker::markRunningInsideWorkerProcess();
    const std::wstring mode = tools3000::worker::findCliOptionValue(
        argc, argv, tools3000::worker::Constants::kFlagWorkerMode);
    if (mode.empty()) {
        return 2;
    }
    for (const auto& strategy : kWorkerStrategies) {
        if (strategy.mode == mode && strategy.handler != nullptr) {
            return strategy.handler(argc, argv);
        }
    }
    return 4;
}
