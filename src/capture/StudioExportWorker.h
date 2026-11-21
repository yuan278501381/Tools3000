#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// StudioExportWorker.h — 演播室多格式媒体导出短命子进程隔离 (--worker=studio-export)
//
// 职责:
//   1. 提供 StudioExportJob (Timeline + RenderConfig + ExportOptions) 的完整无损 JSON 序列化 SSOT
//   2. 宿主侧按需拉起短命子进程执行导出，经命名管道实时同步进度并支持毫秒级取消
//   3. 导出完毕子进程即刻退出，由 OS 100% 回收 NVENC/QSV/MF 硬件编码驱动与 4K 合成缓冲
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. MIT License.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_STUDIOEXPORTWORKER_H
#define TOOLS3000_CAPTURE_STUDIOEXPORTWORKER_H

#include "capture/StudioTimeline.h"
#include "capture/StudioVideoCompositor.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace tools3000::capture {

struct StudioExportJobPayload {
    std::string rawInputPathUtf8;
    std::string finalOutputPathUtf8;
    std::string traceId;
    StudioTimelineData timeline{};
    StudioRenderConfig config{};
    StudioExportOptions exportOpts{};
};

struct StudioExportWorkerOutcome {
    bool handledOutOfProcess = false; // true = 已由独立子进程执行完毕；false = 需调用方回退进程内执行
    bool succeeded = false;
};

/// 完整无损序列化 / 反序列化演播室导出作业（供进程隔离传输与单元测试往返校验）
std::string serializeStudioExportJobJson(const StudioExportJobPayload& job);
bool deserializeStudioExportJobJson(const std::string& jsonText, StudioExportJobPayload& outJob);

/// 在独立短命子进程 (--worker=studio-export) 中执行演播室导出（支持实时进度与取消）
StudioExportWorkerOutcome exportStudioMediaOutOfProcess(
    const std::string& rawInputPathUtf8,
    const std::string& finalOutputPathUtf8,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    const StudioExportOptions& exportOpts,
    const std::function<void(double progress01)>& onProgress = nullptr,
    const std::atomic<bool>* cancelFlag = nullptr,
    const std::filesystem::path& workerExeOverride = {});

/// 子进程侧入口：执行 --worker=studio-export 作业并回传实时进度
int runStudioExportWorkerProcess(int argc, const wchar_t* const* argv);

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_STUDIOEXPORTWORKER_H
