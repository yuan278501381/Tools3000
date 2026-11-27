// ─────────────────────────────────────────────────────────────────────────────
// ResilientModelDownloader.cpp — 全球双栈自适应离线模型下载引擎实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "core/network/ResilientModelDownloader.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <thread>
#include <utility>

namespace tools3000::core::net {

namespace {

std::atomic<uint8_t> g_sessionPreferredRegion{static_cast<uint8_t>(MirrorRegion::Auto)};

struct TrackedWinHttpHandle {
    HINTERNET handle = nullptr;
    std::shared_ptr<DownloadCancelToken> token;

    TrackedWinHttpHandle() = default;
    TrackedWinHttpHandle(HINTERNET h, std::shared_ptr<DownloadCancelToken> tok)
        : handle(h), token(std::move(tok)) {
        if (handle && token) {
            if (!token->registerActiveHandle(handle)) {
                WinHttpCloseHandle(handle);
                handle = nullptr;
            }
        }
    }

    ~TrackedWinHttpHandle() {
        close();
    }

    TrackedWinHttpHandle(const TrackedWinHttpHandle&) = delete;
    TrackedWinHttpHandle& operator=(const TrackedWinHttpHandle&) = delete;

    void close() noexcept {
        HINTERNET toClose = nullptr;
        if (handle) {
            if (token) {
                toClose = static_cast<HINTERNET>(token->detachActiveHandle(handle));
            } else {
                toClose = handle;
            }
            handle = nullptr;
        }
        if (toClose) {
            WinHttpCloseHandle(toClose);
        }
    }

    explicit operator bool() const noexcept { return handle != nullptr; }
    HINTERNET get() const noexcept { return handle; }
};

bool isHtmlContentType(HINTERNET hReq) {
    wchar_t buf[128]{};
    DWORD len = sizeof(buf);
    if (!WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_TYPE,
                             WINHTTP_HEADER_NAME_BY_INDEX, buf, &len, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    std::wstring lower(buf);
    for (wchar_t& ch : lower) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return lower.find(L"text/html") != std::wstring::npos;
}

bool crackHttpsUrl(const std::wstring& url, std::wstring& outHost, INTERNET_PORT& outPort, std::wstring& outPath) {
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }
    outHost.assign(hostBuf, uc.dwHostNameLength);
    outPort = uc.nPort;
    outPath.assign(pathBuf, uc.dwUrlPathLength);
    return !outHost.empty() && !outPath.empty();
}

bool probeEndpointFast(
    const MirrorEndpoint& ep,
    const ResilientDownloadOptions& options,
    const std::shared_ptr<DownloadCancelToken>& raceCancel) {
    if (!raceCancel || raceCancel->isCancelled()) return false;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (!crackHttpsUrl(ep.url, host, port, path)) return false;

    TrackedWinHttpHandle sess(
        WinHttpOpen(options.userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
        raceCancel);
    if (!sess) return false;

    const int probeMs = static_cast<int>((std::max<uint32_t>)(800u, options.probeTimeoutMs));
    WinHttpSetTimeouts(sess.get(), probeMs, probeMs, probeMs, probeMs);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(sess.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    TrackedWinHttpHandle conn(WinHttpConnect(sess.get(), host.c_str(), port, 0), raceCancel);
    if (!conn) return false;

    TrackedWinHttpHandle req(
        WinHttpOpenRequest(conn.get(), L"GET", path.c_str(), nullptr,
                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE),
        raceCancel);
    if (!req) return false;

    static constexpr wchar_t kRangeZero[] = L"Range: bytes=0-0\r\n";
    if (!WinHttpSendRequest(req.get(), kRangeZero, static_cast<DWORD>(-1L),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return false;
    }
    if (!WinHttpReceiveResponse(req.get(), nullptr)) {
        return false;
    }
    if (raceCancel->isCancelled()) return false;

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    if ((statusCode != 200 && statusCode != 206) || isHtmlContentType(req.get())) {
        return false;
    }
    return true;
}

MirrorRegion raceMirrorRegions(
    const std::vector<MirrorEndpoint>& mirrors,
    const ResilientDownloadOptions& options,
    const std::shared_ptr<DownloadCancelToken>& parentCancel) {
    const MirrorEndpoint* cnEp = nullptr;
    const MirrorEndpoint* glEp = nullptr;
    for (const auto& ep : mirrors) {
        if (ep.region == MirrorRegion::China && !cnEp) cnEp = &ep;
        if (ep.region == MirrorRegion::Global && !glEp) glEp = &ep;
    }
    if (!cnEp || !glEp) {
        return MirrorRegion::Auto;
    }

    auto raceCancel = std::make_shared<DownloadCancelToken>();
    std::atomic<uint8_t> winner{static_cast<uint8_t>(MirrorRegion::Auto)};
    std::atomic<int> finishedCount{0};
    std::mutex cvMutex;
    std::condition_variable cv;

    auto launchProbe = [&](const MirrorEndpoint ep, MirrorRegion region) {
        return std::thread([ep, region, options, raceCancel, parentCancel, &winner, &finishedCount, &cvMutex, &cv]() {
            if (parentCancel && parentCancel->isCancelled()) {
                raceCancel->cancel();
            } else if (probeEndpointFast(ep, options, raceCancel)) {
                uint8_t expected = static_cast<uint8_t>(MirrorRegion::Auto);
                if (winner.compare_exchange_strong(expected, static_cast<uint8_t>(region), std::memory_order_acq_rel)) {
                    raceCancel->cancel();
                }
            }
            finishedCount.fetch_add(1, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> lock(cvMutex);
            cv.notify_all();
        });
    };

    const auto startTp = std::chrono::steady_clock::now();
    std::thread tCn = launchProbe(*cnEp, MirrorRegion::China);
    std::thread tGl = launchProbe(*glEp, MirrorRegion::Global);

    const auto deadline = startTp + std::chrono::milliseconds(options.probeTimeoutMs);
    {
        std::unique_lock<std::mutex> lock(cvMutex);
        while (winner.load(std::memory_order_acquire) == static_cast<uint8_t>(MirrorRegion::Auto) &&
               finishedCount.load(std::memory_order_acquire) < 2) {
            if (parentCancel && parentCancel->isCancelled()) {
                raceCancel->cancel();
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                raceCancel->cancel();
                break;
            }
            cv.wait_for(lock, std::chrono::milliseconds(50));
        }
    }

    raceCancel->cancel();
    if (tCn.joinable()) tCn.join();
    if (tGl.joinable()) tGl.join();

    const auto resolved = static_cast<MirrorRegion>(winner.load(std::memory_order_acquire));
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTp).count();
    if (resolved != MirrorRegion::Auto) {
        ResilientModelDownloader::setSessionPreferredRegion(resolved);
        LOG_INFO("ResilientModelDownloader: region probe winner={} ({} ms)",
                 resolved == MirrorRegion::China ? "China" : "Global", elapsedMs);
    }
    return resolved;
}

enum class SingleAttemptStatus : uint8_t {
    Completed = 0,
    Cancelled = 1,
    RetryNextMirror = 2,
    RangeResetNeeded = 3,
};

struct SingleAttemptResult {
    SingleAttemptStatus status = SingleAttemptStatus::RetryNextMirror;
    uint64_t finalPartBytes = 0;
    bool madeProgress = false;
    std::string error;
};

SingleAttemptResult attemptDownloadFromMirror(
    const MirrorEndpoint& ep,
    int mirrorIndex,
    int mirrorTotal,
    const std::filesystem::path& partFilePath,
    const ResilientDownloadOptions& options,
    const std::shared_ptr<DownloadCancelToken>& cancelToken,
    const DownloadProgressCallback& onProgress) {
    SingleAttemptResult result{};
    const std::string hostUtf8 = ResilientModelDownloader::extractUrlHostUtf8(ep.url);

    if (cancelToken && cancelToken->isCancelled()) {
        result.status = SingleAttemptStatus::Cancelled;
        result.error = "cancelled";
        return result;
    }

    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (!crackHttpsUrl(ep.url, host, port, path)) {
        result.error = "invalid HTTPS URL";
        return result;
    }

    uint64_t existingPartSize = 0;
    if (options.enableRangeResume) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(partFilePath, ec) && !ec) {
            const auto fsz = std::filesystem::file_size(partFilePath, ec);
            if (!ec && fsz > 0 && fsz < options.maxFileBytes) {
                existingPartSize = static_cast<uint64_t>(fsz);
            }
        }
    }

    TrackedWinHttpHandle sess(
        WinHttpOpen(options.userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
        cancelToken);
    if (!sess) {
        result.status = (cancelToken && cancelToken->isCancelled())
            ? SingleAttemptStatus::Cancelled
            : SingleAttemptStatus::RetryNextMirror;
        result.error = "WinHttpOpen failed";
        return result;
    }

    WinHttpSetTimeouts(sess.get(),
                       static_cast<int>(options.resolveTimeoutMs),
                       static_cast<int>(options.connectTimeoutMs),
                       static_cast<int>(options.sendTimeoutMs),
                       static_cast<int>(options.receiveTimeoutMs));
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(sess.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    TrackedWinHttpHandle conn(WinHttpConnect(sess.get(), host.c_str(), port, 0), cancelToken);
    if (!conn) {
        result.status = (cancelToken && cancelToken->isCancelled())
            ? SingleAttemptStatus::Cancelled
            : SingleAttemptStatus::RetryNextMirror;
        result.error = "WinHttpConnect failed";
        return result;
    }

    TrackedWinHttpHandle req(
        WinHttpOpenRequest(conn.get(), L"GET", path.c_str(), nullptr,
                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE),
        cancelToken);
    if (!req) {
        result.status = (cancelToken && cancelToken->isCancelled())
            ? SingleAttemptStatus::Cancelled
            : SingleAttemptStatus::RetryNextMirror;
        result.error = "WinHttpOpenRequest failed";
        return result;
    }

    std::wstring extraHeaders;
    if (existingPartSize > 0) {
        extraHeaders = L"Range: bytes=" + std::to_wstring(existingPartSize) + L"-\r\n";
    }
    const wchar_t* headersPtr = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str();
    const DWORD headersLen = extraHeaders.empty() ? 0 : static_cast<DWORD>(-1L);

    if (!WinHttpSendRequest(req.get(), headersPtr, headersLen, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        if (cancelToken && cancelToken->isCancelled()) {
            result.status = SingleAttemptStatus::Cancelled;
            result.error = "cancelled";
        } else {
            result.error = "network request failed on " + hostUtf8;
        }
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        result.error = "missing HTTP status";
        return result;
    }

    if (statusCode == 416 && existingPartSize > 0) {
        result.status = SingleAttemptStatus::RangeResetNeeded;
        result.error = "HTTP 416 range not satisfiable";
        return result;
    }

    if ((statusCode != 200 && statusCode != 206) || isHtmlContentType(req.get())) {
        result.error = "HTTP " + std::to_string(statusCode) + " from " + hostUtf8;
        return result;
    }

    uint64_t expectedTotalFileBytes = 0;
    uint64_t expectedTransferBytes = 0;
    bool appendMode = false;

    wchar_t lenHeader[64]{};
    DWORD lenHeaderBytes = sizeof(lenHeader);
    if (WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, lenHeader, &lenHeaderBytes, WINHTTP_NO_HEADER_INDEX)) {
        expectedTransferBytes = _wcstoui64(lenHeader, nullptr, 10);
    }

    if (statusCode == 206 && existingPartSize > 0) {
        wchar_t crBuf[160]{};
        DWORD crSize = sizeof(crBuf);
        uint64_t crTotal = 0;
        if (WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_CONTENT_RANGE,
                                WINHTTP_HEADER_NAME_BY_INDEX, crBuf, &crSize, WINHTTP_NO_HEADER_INDEX) &&
            ResilientModelDownloader::parseContentRangeHeader(crBuf, existingPartSize, &crTotal)) {
            appendMode = true;
            expectedTotalFileBytes = crTotal;
            if (expectedTotalFileBytes == 0 && expectedTransferBytes > 0) {
                expectedTotalFileBytes = existingPartSize + expectedTransferBytes;
            }
            LOG_INFO("ResilientModelDownloader: resuming from {} bytes via {}", existingPartSize, hostUtf8);
        } else {
            // 服务端返回的 Content-Range 起始偏移不匹配，放弃断点续传改写整包
            result.status = SingleAttemptStatus::RangeResetNeeded;
            result.error = "invalid Content-Range from " + hostUtf8;
            return result;
        }
    } else {
        appendMode = false;
        existingPartSize = 0;
        expectedTotalFileBytes = expectedTransferBytes;
    }

    const uint64_t displayTotalBytes = expectedTotalFileBytes > 0
        ? expectedTotalFileBytes
        : options.expectedFileBytes;

    std::ofstream ofs(partFilePath,
                      appendMode ? (std::ios::binary | std::ios::app) : (std::ios::binary | std::ios::trunc));
    if (!ofs) {
        result.error = "failed to open part file";
        return result;
    }

    if (onProgress) {
        DownloadProgress p{};
        p.fileBytesDownloaded = existingPartSize;
        p.fileTotalBytes = displayTotalBytes;
        p.deltaBytes = 0;
        p.speedBytesPerSec = 0.0;
        p.activeMirrorHost = hostUtf8;
        p.mirrorIndex = mirrorIndex;
        p.mirrorTotal = mirrorTotal;
        p.phase = "downloading";
        onProgress(p);
    }

    constexpr DWORD kChunkSize = 64 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);
    uint64_t transferBytes = 0;
    uint64_t fileWrittenTotal = existingPartSize;

    using Clock = std::chrono::steady_clock;
    auto lastDataTp = Clock::now();
    auto speedSampleStartTp = lastDataTp;
    uint64_t speedSampleBytes = 0;
    double smoothedSpeedBps = 0.0;
    auto lowSpeedSinceTp = Clock::time_point{};
    auto lastProgressEmitTp = lastDataTp - std::chrono::milliseconds(300);

    while (true) {
        if (cancelToken && cancelToken->isCancelled()) {
            result.status = SingleAttemptStatus::Cancelled;
            result.error = "cancelled";
            return result;
        }

        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &avail)) {
            if (cancelToken && cancelToken->isCancelled()) {
                result.status = SingleAttemptStatus::Cancelled;
                result.error = "cancelled";
            } else {
                result.error = "connection interrupted on " + hostUtf8;
            }
            break;
        }
        if (avail == 0) {
            // EOF
            break;
        }

        const DWORD toRead = (std::min)(avail, kChunkSize);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(req.get(), buffer.data(), toRead, &bytesRead)) {
            if (cancelToken && cancelToken->isCancelled()) {
                result.status = SingleAttemptStatus::Cancelled;
                result.error = "cancelled";
            } else {
                result.error = "read failed on " + hostUtf8;
            }
            break;
        }
        if (bytesRead == 0) {
            break;
        }

        fileWrittenTotal += bytesRead;
        transferBytes += bytesRead;
        if (fileWrittenTotal > options.maxFileBytes) {
            result.error = "file exceeds maximum allowed size";
            break;
        }

        ofs.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(bytesRead));
        if (!ofs) {
            result.error = "disk write failed";
            break;
        }

        const auto nowTp = Clock::now();
        lastDataTp = nowTp;
        speedSampleBytes += bytesRead;
        const auto sampleMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowTp - speedSampleStartTp).count();
        if (sampleMs >= 500) {
            const double instBps = (static_cast<double>(speedSampleBytes) * 1000.0) / static_cast<double>(sampleMs);
            smoothedSpeedBps = (smoothedSpeedBps <= 1.0) ? instBps : (smoothedSpeedBps * 0.55 + instBps * 0.45);
            speedSampleStartTp = nowTp;
            speedSampleBytes = 0;

            const uint64_t remaining = (displayTotalBytes > fileWrittenTotal)
                ? (displayTotalBytes - fileWrittenTotal)
                : options.expectedFileBytes;
            if (smoothedSpeedBps < options.lowSpeedMinBytesPerSec) {
                if (lowSpeedSinceTp == Clock::time_point{}) {
                    lowSpeedSinceTp = nowTp;
                }
            } else {
                lowSpeedSinceTp = Clock::time_point{};
            }

            const uint64_t lowSpeedElapsedMs = (lowSpeedSinceTp == Clock::time_point{})
                ? 0ULL
                : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(nowTp - lowSpeedSinceTp).count());
            const auto decision = ResilientModelDownloader::evaluateWatchdog(
                0ULL, lowSpeedElapsedMs, smoothedSpeedBps, remaining, options);
            if (decision == WatchdogDecision::LowSpeedTimeout) {
                LOG_WARN("ResilientModelDownloader: low speed watchdog triggered on {} ({:.1f} KB/s for {} ms), switching mirror",
                         hostUtf8, smoothedSpeedBps / 1024.0, lowSpeedElapsedMs);
                result.error = "low speed timeout on " + hostUtf8;
                break;
            }
        }

        const auto emitMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowTp - lastProgressEmitTp).count();
        if (onProgress && (emitMs >= 160 || fileWrittenTotal == displayTotalBytes)) {
            lastProgressEmitTp = nowTp;
            DownloadProgress p{};
            p.fileBytesDownloaded = fileWrittenTotal;
            p.fileTotalBytes = (std::max)(displayTotalBytes, fileWrittenTotal);
            p.deltaBytes = bytesRead;
            p.speedBytesPerSec = smoothedSpeedBps;
            p.activeMirrorHost = hostUtf8;
            p.mirrorIndex = mirrorIndex;
            p.mirrorTotal = mirrorTotal;
            p.phase = "downloading";
            onProgress(p);
        }

        // 检查停滞超时（一般由 receiveTimeoutMs=6s 触发退出，此处再加兜底判定）
        const uint64_t noDataMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastDataTp).count());
        if (ResilientModelDownloader::evaluateWatchdog(noDataMs, 0ULL, smoothedSpeedBps, UINT64_MAX, options) ==
            WatchdogDecision::StallTimeout) {
            result.error = "stall timeout on " + hostUtf8;
            break;
        }
    }

    ofs.flush();
    ofs.close();

    result.finalPartBytes = fileWrittenTotal;
    result.madeProgress = (transferBytes > 0);

    if (result.status == SingleAttemptStatus::Cancelled || (cancelToken && cancelToken->isCancelled())) {
        result.status = SingleAttemptStatus::Cancelled;
        result.error = "cancelled";
        return result;
    }

    if (!result.error.empty()) {
        result.status = SingleAttemptStatus::RetryNextMirror;
        return result;
    }

    if (fileWrittenTotal == 0) {
        result.status = SingleAttemptStatus::RetryNextMirror;
        result.error = "empty response from " + hostUtf8;
        return result;
    }

    if (expectedTotalFileBytes > 0 && fileWrittenTotal != expectedTotalFileBytes) {
        LOG_WARN("ResilientModelDownloader: truncated stream detected (got={} bytes, expected={} bytes) from {}",
                 fileWrittenTotal, expectedTotalFileBytes, hostUtf8);
        result.status = SingleAttemptStatus::RetryNextMirror;
        result.error = "truncated download from " + hostUtf8;
        return result;
    }

    if (expectedTransferBytes > 0 && transferBytes != expectedTransferBytes) {
        LOG_WARN("ResilientModelDownloader: partial chunk stream (got={} bytes, expected={} bytes) from {}",
                 transferBytes, expectedTransferBytes, hostUtf8);
        result.status = SingleAttemptStatus::RetryNextMirror;
        result.error = "truncated transfer from " + hostUtf8;
        return result;
    }

    if (onProgress) {
        DownloadProgress p{};
        p.fileBytesDownloaded = fileWrittenTotal;
        p.fileTotalBytes = fileWrittenTotal;
        p.deltaBytes = 0;
        p.speedBytesPerSec = smoothedSpeedBps;
        p.activeMirrorHost = hostUtf8;
        p.mirrorIndex = mirrorIndex;
        p.mirrorTotal = mirrorTotal;
        p.phase = "downloading";
        onProgress(p);
    }

    result.status = SingleAttemptStatus::Completed;
    return result;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// DownloadCancelToken
// ─────────────────────────────────────────────────────────────────────────────

struct DownloadCancelToken::Impl {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::vector<void*> activeHandles;
};

DownloadCancelToken::DownloadCancelToken()
    : m_impl(std::make_unique<Impl>()) {}

DownloadCancelToken::~DownloadCancelToken() {
    cancel();
}

void DownloadCancelToken::reset() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->cancelled.store(false, std::memory_order_release);
}

void DownloadCancelToken::cancel() {
    std::vector<void*> handlesToClose;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->cancelled.store(true, std::memory_order_release);
        handlesToClose.swap(m_impl->activeHandles);
    }
    // 逆序关闭（先关 Request 再关 Connect 再关 Session），立即打断阻塞 I/O
    for (auto it = handlesToClose.rbegin(); it != handlesToClose.rend(); ++it) {
        if (*it) {
            WinHttpCloseHandle(static_cast<HINTERNET>(*it));
        }
    }
}

bool DownloadCancelToken::isCancelled() const noexcept {
    return m_impl->cancelled.load(std::memory_order_acquire);
}

bool DownloadCancelToken::registerActiveHandle(void* handle) {
    if (!handle) return false;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->cancelled.load(std::memory_order_acquire)) {
        return false;
    }
    m_impl->activeHandles.push_back(handle);
    return true;
}

void* DownloadCancelToken::detachActiveHandle(void* handle) {
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto it = std::find(m_impl->activeHandles.begin(), m_impl->activeHandles.end(), handle);
    if (it == m_impl->activeHandles.end()) {
        return nullptr; // 已被 cancel() 抢先接管并关闭，避免双重关闭
    }
    m_impl->activeHandles.erase(it);
    return handle;
}

// ─────────────────────────────────────────────────────────────────────────────
// ResilientModelDownloader
// ─────────────────────────────────────────────────────────────────────────────

std::string ResilientModelDownloader::extractUrlHostUtf8(std::wstring_view url) {
    const size_t schemePos = url.find(L"://");
    const size_t start = (schemePos == std::wstring_view::npos) ? 0 : (schemePos + 3);
    size_t end = url.find_first_of(L"/?#:", start);
    if (end == std::wstring_view::npos) end = url.size();
    if (end <= start) return {};
    std::wstring host(url.substr(start, end - start));
    for (wchar_t& ch : host) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return WinUtils::wstringToUtf8(host);
}

bool ResilientModelDownloader::parseContentRangeHeader(
    std::wstring_view header,
    uint64_t expectedStart,
    uint64_t* outTotalLength) noexcept {
    if (outTotalLength) *outTotalLength = 0;
    // 标准格式: "bytes <start>-<end>/<total>" 或 "bytes <start>-<end>/*"
    while (!header.empty() && (header.front() == L' ' || header.front() == L'\t')) {
        header.remove_prefix(1);
    }
    if (header.size() < 7) return false;
    auto eqIgnoreCase = [](wchar_t a, wchar_t b) noexcept {
        return std::towlower(a) == std::towlower(b);
    };
    const wchar_t kPrefix[] = L"bytes ";
    for (size_t i = 0; i < 6; ++i) {
        if (!eqIgnoreCase(header[i], kPrefix[i])) return false;
    }
    header.remove_prefix(6);
    while (!header.empty() && header.front() == L' ') {
        header.remove_prefix(1);
    }

    const size_t dashPos = header.find(L'-');
    const size_t slashPos = header.find(L'/');
    if (dashPos == std::wstring_view::npos || slashPos == std::wstring_view::npos || dashPos >= slashPos) {
        return false;
    }

    auto parseUint64 = [](std::wstring_view s, uint64_t& val) noexcept -> bool {
        if (s.empty()) return false;
        uint64_t acc = 0;
        for (wchar_t c : s) {
            if (c < L'0' || c > L'9') return false;
            const uint64_t digit = static_cast<uint64_t>(c - L'0');
            if (acc > (UINT64_MAX - digit) / 10ULL) return false;
            acc = acc * 10ULL + digit;
        }
        val = acc;
        return true;
    };

    uint64_t rangeStart = 0;
    uint64_t rangeEnd = 0;
    if (!parseUint64(header.substr(0, dashPos), rangeStart) ||
        !parseUint64(header.substr(dashPos + 1, slashPos - dashPos - 1), rangeEnd)) {
        return false;
    }
    if (rangeStart != expectedStart || rangeEnd < rangeStart) {
        return false;
    }

    std::wstring_view totalPart = header.substr(slashPos + 1);
    while (!totalPart.empty() && (totalPart.back() == L' ' || totalPart.back() == L'\r' || totalPart.back() == L'\n')) {
        totalPart.remove_suffix(1);
    }
    if (totalPart == L"*") {
        if (outTotalLength) *outTotalLength = 0;
        return true;
    }
    uint64_t totalLen = 0;
    if (!parseUint64(totalPart, totalLen) || totalLen <= rangeEnd) {
        return false;
    }
    if (outTotalLength) *outTotalLength = totalLen;
    return true;
}

WatchdogDecision ResilientModelDownloader::evaluateWatchdog(
    uint64_t elapsedNoDataMs,
    uint64_t lowSpeedElapsedMs,
    double windowBytesPerSec,
    uint64_t remainingBytes,
    const ResilientDownloadOptions& options) noexcept {
    if (options.stallTimeoutMs > 0 && elapsedNoDataMs >= options.stallTimeoutMs) {
        return WatchdogDecision::StallTimeout;
    }
    constexpr uint64_t kTailExemptBytes = 512ULL * 1024ULL;
    if (options.lowSpeedWindowMs > 0 &&
        options.lowSpeedMinBytesPerSec > 0.0 &&
        remainingBytes > kTailExemptBytes &&
        lowSpeedElapsedMs >= options.lowSpeedWindowMs &&
        windowBytesPerSec < options.lowSpeedMinBytesPerSec) {
        return WatchdogDecision::LowSpeedTimeout;
    }
    return WatchdogDecision::Continue;
}

std::vector<MirrorEndpoint> ResilientModelDownloader::orderMirrorsByRegionPreference(
    const std::vector<MirrorEndpoint>& mirrors,
    MirrorRegion preferredRegion) {
    if (preferredRegion == MirrorRegion::Auto || mirrors.size() <= 1) {
        return mirrors;
    }
    std::vector<MirrorEndpoint> primary;
    std::vector<MirrorEndpoint> fallback;
    primary.reserve(mirrors.size());
    fallback.reserve(mirrors.size());
    for (const auto& m : mirrors) {
        if (m.region == preferredRegion) {
            primary.push_back(m);
        } else {
            fallback.push_back(m);
        }
    }
    primary.insert(primary.end(), fallback.begin(), fallback.end());
    return primary;
}

MirrorRegion ResilientModelDownloader::sessionPreferredRegion() noexcept {
    return static_cast<MirrorRegion>(g_sessionPreferredRegion.load(std::memory_order_acquire));
}

void ResilientModelDownloader::setSessionPreferredRegion(MirrorRegion region) noexcept {
    g_sessionPreferredRegion.store(static_cast<uint8_t>(region), std::memory_order_release);
}

void ResilientModelDownloader::resetSessionPreferredRegionForTest() noexcept {
    g_sessionPreferredRegion.store(static_cast<uint8_t>(MirrorRegion::Auto), std::memory_order_release);
}

DownloadOutcome ResilientModelDownloader::downloadToPartFile(
    const std::vector<MirrorEndpoint>& mirrors,
    const std::filesystem::path& partFilePath,
    const ResilientDownloadOptions& options,
    const std::shared_ptr<DownloadCancelToken>& cancelToken,
    const DownloadProgressCallback& onProgress) {
    DownloadOutcome outcome{};
    if (mirrors.empty()) {
        outcome.error = "no mirror endpoints configured";
        return outcome;
    }
    if (cancelToken && cancelToken->isCancelled()) {
        outcome.cancelled = true;
        outcome.error = "cancelled";
        return outcome;
    }

    MirrorRegion preferred = sessionPreferredRegion();
    if (options.enableRegionRace && preferred == MirrorRegion::Auto) {
        bool hasChina = false;
        bool hasGlobal = false;
        for (const auto& ep : mirrors) {
            if (ep.region == MirrorRegion::China) hasChina = true;
            if (ep.region == MirrorRegion::Global) hasGlobal = true;
        }
        if (hasChina && hasGlobal) {
            if (onProgress) {
                DownloadProgress p{};
                p.fileTotalBytes = options.expectedFileBytes;
                p.mirrorTotal = static_cast<int>(mirrors.size());
                p.phase = "probing";
                onProgress(p);
            }
            preferred = raceMirrorRegions(mirrors, options, cancelToken);
            if (cancelToken && cancelToken->isCancelled()) {
                outcome.cancelled = true;
                outcome.error = "cancelled";
                return outcome;
            }
        }
    }

    const auto ordered = orderMirrorsByRegionPreference(mirrors, preferred);
    const int mirrorTotal = static_cast<int>(ordered.size());
    const int maxRounds = (std::max)(1, options.maxResumeRounds);

    std::string lastError;
    for (int round = 0; round < maxRounds; ++round) {
        bool roundMadeProgress = false;

        for (size_t i = 0; i < ordered.size(); ++i) {
            if (cancelToken && cancelToken->isCancelled()) {
                outcome.cancelled = true;
                outcome.error = "cancelled";
                return outcome;
            }

            const auto& ep = ordered[i];
            const std::string hostUtf8 = extractUrlHostUtf8(ep.url);
            if ((i > 0 || round > 0) && onProgress) {
                std::error_code szEc;
                const uint64_t curPart = std::filesystem::is_regular_file(partFilePath, szEc) && !szEc
                    ? static_cast<uint64_t>(std::filesystem::file_size(partFilePath, szEc))
                    : 0ULL;
                DownloadProgress p{};
                p.fileBytesDownloaded = curPart;
                p.fileTotalBytes = (std::max)(options.expectedFileBytes, curPart);
                p.activeMirrorHost = hostUtf8;
                p.mirrorIndex = static_cast<int>(i + 1);
                p.mirrorTotal = mirrorTotal;
                p.phase = "switching";
                onProgress(p);
            }

            SingleAttemptResult attempt = attemptDownloadFromMirror(
                ep, static_cast<int>(i + 1), mirrorTotal, partFilePath, options, cancelToken, onProgress);

            if (attempt.status == SingleAttemptStatus::RangeResetNeeded) {
                std::error_code rmEc;
                std::filesystem::remove(partFilePath, rmEc);
                attempt = attemptDownloadFromMirror(
                    ep, static_cast<int>(i + 1), mirrorTotal, partFilePath, options, cancelToken, onProgress);
            }

            if (attempt.status == SingleAttemptStatus::Completed) {
                if (ep.region != MirrorRegion::Auto) {
                    setSessionPreferredRegion(ep.region);
                }
                outcome.success = true;
                outcome.bytesWritten = attempt.finalPartBytes;
                outcome.activeMirrorHost = hostUtf8;
                return outcome;
            }

            if (attempt.status == SingleAttemptStatus::Cancelled) {
                outcome.cancelled = true;
                outcome.error = "cancelled";
                return outcome;
            }

            if (attempt.madeProgress) {
                roundMadeProgress = true;
            }
            lastError = attempt.error;
            LOG_WARN("ResilientModelDownloader: mirror {}/{} ({}) failed: {}",
                     i + 1, ordered.size(), hostUtf8, attempt.error);
        }

        // 若整轮没有任何字节进展，说明网络确实不可达，无需重复轮询
        if (!roundMadeProgress) {
            break;
        }
    }

    outcome.success = false;
    outcome.error = lastError.empty() ? "all download mirrors failed" : lastError;
    return outcome;
}

} // namespace tools3000::core::net
