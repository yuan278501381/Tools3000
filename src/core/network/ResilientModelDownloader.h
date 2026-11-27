#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ResilientModelDownloader — 全球双栈自适应离线模型下载引擎
//
// 核心能力:
//   1. Happy Eyeballs 3s 并发轻探针 (China vs Global) 实测选路，进程会话记忆胜出集群
//   2. 低速 / 停滞看门狗 (8s 0字节断流、12s < 30KB/s 龟速自动断开切源)
//   3. HTTP Range 断点续传 (兼容 206 Partial Content / 200 回退 / 416 自愈 / Chunked 响应)
//   4. 句柄级 <50ms 瞬时取消 (原子标志 + 线程安全关闭活跃 WinHTTP 句柄)
//   5. 实时网速、已下载字节、活跃镜像域名与阶段回调，彻底消除 UI "看似卡死" 黑盒
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_NETWORK_RESILIENTMODELDOWNLOADER_H
#define TOOLS3000_CORE_NETWORK_RESILIENTMODELDOWNLOADER_H

#include "core/utils/Export.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::core::net {

enum class MirrorRegion : uint8_t {
    Auto = 0,
    China = 1,
    Global = 2,
};

struct MirrorEndpoint {
    std::wstring url;
    MirrorRegion region = MirrorRegion::Auto;
    std::string label;
};

struct DownloadProgress {
    uint64_t fileBytesDownloaded = 0;
    uint64_t fileTotalBytes = 0;
    uint64_t deltaBytes = 0;
    double speedBytesPerSec = 0.0;
    std::string activeMirrorHost;
    int mirrorIndex = 0; // 1-based index of current mirror attempt
    int mirrorTotal = 0;
    std::string phase;   // "probing" | "downloading" | "switching" | "verifying"
};

struct ResilientDownloadOptions {
    std::wstring userAgent = L"Tools3000/1.0 (ResilientModelDownloader)";
    uint64_t maxFileBytes = 160ULL * 1024ULL * 1024ULL;
    uint64_t expectedFileBytes = 0;
    uint32_t resolveTimeoutMs = 5000;
    uint32_t connectTimeoutMs = 6000;
    uint32_t sendTimeoutMs = 8000;
    uint32_t receiveTimeoutMs = 6000;
    uint32_t stallTimeoutMs = 8000;
    uint32_t lowSpeedWindowMs = 12000;
    double lowSpeedMinBytesPerSec = 30.0 * 1024.0;
    uint32_t probeTimeoutMs = 3000;
    int maxResumeRounds = 2;
    bool enableRangeResume = true;
    bool enableRegionRace = true;
};

struct DownloadOutcome {
    bool success = false;
    bool cancelled = false;
    uint64_t bytesWritten = 0;
    std::string activeMirrorHost;
    std::string error;
};

enum class WatchdogDecision : uint8_t {
    Continue = 0,
    StallTimeout = 1,
    LowSpeedTimeout = 2,
};

/// 线程安全的句柄级下载取消令牌：cancel() 立即置位并强行中断正在阻塞的 WinHTTP 句柄
class TOOLS3000CORE_API DownloadCancelToken {
public:
    DownloadCancelToken();
    ~DownloadCancelToken();

    DownloadCancelToken(const DownloadCancelToken&) = delete;
    DownloadCancelToken& operator=(const DownloadCancelToken&) = delete;

    void reset();
    void cancel();
    bool isCancelled() const noexcept;

    /// 内部供下载引擎登记/摘除活跃 WinHTTP 句柄（传入 HINTERNET 原始指针，避免头文件暴露 winhttp.h）
    bool registerActiveHandle(void* handle);
    void* detachActiveHandle(void* handle);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

using DownloadProgressCallback = std::function<void(const DownloadProgress&)>;

/// 通用高鲁棒多镜像模型下载器（无状态核心 + 进程会话区域偏好记忆）
class TOOLS3000CORE_API ResilientModelDownloader {
public:
    /// 下载目标文件到 partFilePath（自动选路、断点续传、低速切源、瞬时取消）
    static DownloadOutcome downloadToPartFile(
        const std::vector<MirrorEndpoint>& mirrors,
        const std::filesystem::path& partFilePath,
        const ResilientDownloadOptions& options,
        const std::shared_ptr<DownloadCancelToken>& cancelToken,
        const DownloadProgressCallback& onProgress);

    // ── 纯算法策略与协议解析原语（同时供离线单元测试直接断言） ───────────────

    /// 提取 HTTPS/HTTP URL 中的主机名（UTF-8 小写）
    static std::string extractUrlHostUtf8(std::wstring_view url);

    /// 解析 HTTP Content-Range 头（如 "bytes 4194304-13488747/13488748"），严格校验起始偏移
    static bool parseContentRangeHeader(
        std::wstring_view header,
        uint64_t expectedStart,
        uint64_t* outTotalLength = nullptr) noexcept;

    /// 评估停滞与低速看门狗决策（remainingBytes <= 512KB 时豁免收尾低速误判）
    static WatchdogDecision evaluateWatchdog(
        uint64_t elapsedNoDataMs,
        uint64_t lowSpeedElapsedMs,
        double windowBytesPerSec,
        uint64_t remainingBytes,
        const ResilientDownloadOptions& options) noexcept;

    /// 按区域偏好重排镜像列表（胜出区域靠前，备选区域紧随其后，保证全量兜底无遗漏）
    static std::vector<MirrorEndpoint> orderMirrorsByRegionPreference(
        const std::vector<MirrorEndpoint>& mirrors,
        MirrorRegion preferredRegion);

    /// 读写当前进程会话的竞速胜出区域缓存
    static MirrorRegion sessionPreferredRegion() noexcept;
    static void setSessionPreferredRegion(MirrorRegion region) noexcept;
    static void resetSessionPreferredRegionForTest() noexcept;
};

} // namespace tools3000::core::net

#endif // TOOLS3000_CORE_NETWORK_RESILIENTMODELDOWNLOADER_H
