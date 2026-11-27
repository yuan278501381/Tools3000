// ─────────────────────────────────────────────────────────────────────────────
// PinMemoryGovernor.cpp — 贴图内存封顶治理中枢实现
//
// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinMemoryGovernor.h"

#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace tools3000::capture {

namespace {

bool isTestModeEnvEnabled() noexcept {
    wchar_t buf[8]{};
    const DWORD len = GetEnvironmentVariableW(
        L"TOOLS3000_TEST_MODE", buf, static_cast<DWORD>(std::size(buf)));
    return len > 0 && buf[0] == L'1';
}

void cleanupIdleSchedulerWorkingSet() noexcept {
    if (!isTestModeEnvEnabled()) {
        tools3000::core::WinUtils::trimWorkingSet();
    }
}

bool isDuplicateOverlapLine(
    const tools3000::ocr::OcrResult& a,
    const tools3000::ocr::OcrResult& b) noexcept {
    const int interX1 = (std::max)(a.box.x, b.box.x);
    const int interY1 = (std::max)(a.box.y, b.box.y);
    const int interX2 = (std::min)(a.box.x + a.box.width, b.box.x + b.box.width);
    const int interY2 = (std::min)(a.box.y + a.box.height, b.box.y + b.box.height);
    if (interX2 <= interX1 || interY2 <= interY1) {
        return false;
    }
    const double interArea = static_cast<double>(interX2 - interX1) * static_cast<double>(interY2 - interY1);
    const double areaA = static_cast<double>((std::max)(1, a.box.width)) * static_cast<double>((std::max)(1, a.box.height));
    const double areaB = static_cast<double>((std::max)(1, b.box.width)) * static_cast<double>((std::max)(1, b.box.height));
    const double minArea = (std::min)(areaA, areaB);
    if (interArea / minArea >= 0.55) {
        return true;
    }
    const int cyA = a.box.y + a.box.height / 2;
    const int cyB = b.box.y + b.box.height / 2;
    const int minH = (std::max)(1, (std::min)(a.box.height, b.box.height));
    const double horizOverlap = static_cast<double>(interX2 - interX1) /
                                static_cast<double>((std::max)(1, (std::min)(a.box.width, b.box.width)));
    return std::abs(cyA - cyB) <= (minH / 2) && horizOverlap >= 0.65 && a.text == b.text;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// PinLosslessImageSlot
// ─────────────────────────────────────────────────────────────────────────────

void PinLosslessImageSlot::assignHot(const cv::Mat& img) {
    m_compressedPng.clear();
    m_compressedPng.shrink_to_fit();
    if (img.empty()) {
        m_hot.release();
        m_width = 0;
        m_height = 0;
        m_channels = 0;
        return;
    }
    m_hot = img;
    m_width = img.cols;
    m_height = img.rows;
    m_channels = img.channels();
}

void PinLosslessImageSlot::clear() noexcept {
    m_hot.release();
    m_compressedPng.clear();
    m_compressedPng.shrink_to_fit();
    m_width = 0;
    m_height = 0;
    m_channels = 0;
}

uint64_t PinLosslessImageSlot::compressToCold() {
    if (m_hot.empty()) {
        return 0;
    }
    const uint64_t beforeBytes = hotBytes();
    std::vector<uint8_t> encoded;
    try {
        // Zlib Level 1 极速无损编码：4K 桌面截图 ~15ms 完成，压缩比通常达 8:1 ~ 20:1，100% 保留 BGRA Alpha
        const std::vector<int> params = {cv::IMWRITE_PNG_COMPRESSION, 1};
        if (!cv::imencode(".png", m_hot, encoded, params) || encoded.empty()) {
            return 0;
        }
    } catch (...) {
        return 0;
    }

    encoded.shrink_to_fit();
    if (encoded.capacity() >= beforeBytes) {
        return 0;
    }

    m_compressedPng = std::move(encoded);
    m_hot.release();
    return beforeBytes - compressedBytes();
}

const cv::Mat& PinLosslessImageSlot::materializeHot() {
    if (!m_hot.empty() || m_compressedPng.empty()) {
        return m_hot;
    }
    try {
        cv::Mat decoded = cv::imdecode(m_compressedPng, cv::IMREAD_UNCHANGED);
        if (!decoded.empty()) {
            m_hot = std::move(decoded);
            m_width = m_hot.cols;
            m_height = m_hot.rows;
            m_channels = m_hot.channels();
            m_compressedPng.clear();
            m_compressedPng.shrink_to_fit();
        }
    } catch (...) {
        // 保持异常安全，保留压缩字节不丢数据
    }
    return m_hot;
}

// ─────────────────────────────────────────────────────────────────────────────
// extractPinPageMemoryBounded — 超长截图安全条带分片与全局去重合并
// ─────────────────────────────────────────────────────────────────────────────

tools3000::ocr::OcrPageExtraction extractPinPageMemoryBounded(
    const cv::Mat& image,
    const std::string& scriptModeOverride) {
    tools3000::ocr::OcrPageExtraction merged{};
    if (image.empty()) {
        return merged;
    }

    const bool needVerticalTiling =
        (image.rows > PinMemoryConstants::kTiledOcrTriggerHeight) &&
        (image.rows > static_cast<int>(image.cols * 1.5));

    if (!needVerticalTiling) {
        return tools3000::ocr::OcrEngine::instance().extractPage(image, scriptModeOverride);
    }

    const int sliceH = PinMemoryConstants::kTiledOcrSliceHeight;
    const int overlap = PinMemoryConstants::kTiledOcrSliceOverlap;
    const int stepY = (std::max)(256, sliceH - overlap);

    int sliceCount = 0;
    for (int y0 = 0; y0 < image.rows && sliceCount < PinMemoryConstants::kTiledOcrMaxSlices; y0 += stepY) {
        const int curH = (std::min)(sliceH, image.rows - y0);
        if (curH < 24) {
            break;
        }
        ++sliceCount;

        // 使用连续局部拷贝确保共享内存步长紧凑且单次 IPC 严格 <= ~16-24MB
        const cv::Mat slice = image(cv::Rect(0, y0, image.cols, curH)).clone();
        auto part = tools3000::ocr::OcrEngine::instance().extractPage(slice, scriptModeOverride);
        if (merged.activeEngine.empty() && !part.activeEngine.empty()) {
            merged.activeEngine = part.activeEngine;
        }

        for (auto& line : part.lines) {
            if (line.text.empty()) continue;
            line.box.y += y0;
            for (auto& cb : line.charBoxes) {
                cb.y += y0;
            }
            // 重置局部行对齐编号，待全图合并后由 normalizePinOcrBlocksAndTableState 统一进行版面推断
            line.alignedRowId = -1;

            bool duplicate = false;
            for (auto& existing : merged.lines) {
                if (isDuplicateOverlapLine(existing, line)) {
                    // 优先保留字符数更完整或置信度更高的重叠带识别结果
                    if (line.text.size() > existing.text.size() ||
                        (line.text.size() == existing.text.size() && line.confidence > existing.confidence)) {
                        existing = std::move(line);
                    }
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                merged.lines.push_back(std::move(line));
            }
        }

        for (auto& qr : part.qrCodes) {
            if (qr.text.empty()) continue;
            qr.boundingBox.y += y0;
            bool exists = false;
            for (const auto& eq : merged.qrCodes) {
                if (eq.text == qr.text) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                merged.qrCodes.push_back(std::move(qr));
            }
        }

        if (y0 + curH >= image.rows) {
            break;
        }
    }

    LOG_INFO("PinMemoryGovernor: tiled OCR completed on {}x{} across {} slices ({} lines, {} QRs)",
             image.cols, image.rows, sliceCount, merged.lines.size(), merged.qrCodes.size());
    return merged;
}

// ─────────────────────────────────────────────────────────────────────────────
// PinOcrScheduler — 全局单飞三级优先级调度器
// ─────────────────────────────────────────────────────────────────────────────

PinOcrScheduler& PinOcrScheduler::instance() {
    static PinOcrScheduler inst;
    return inst;
}

PinOcrScheduler::~PinOcrScheduler() {
    (void)shutdownAndDrain(2000);
}

void PinOcrScheduler::resetShutdownGate() noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_shuttingDown = false;
}

void PinOcrScheduler::setCustomExtractorForTest(CustomExtractFn extractor) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_customExtractor = std::move(extractor);
}

uint32_t PinOcrScheduler::pendingAndActiveCount() const noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<uint32_t>(m_queue.size()) + (m_jobActive ? 1u : 0u);
}

void PinOcrScheduler::ensureWorkerThreadStartedLocked() {
    if (m_workerRunning) {
        return;
    }
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_workerRunning = true;
    m_workerThread = std::thread(&PinOcrScheduler::workerLoop, this);
}

void PinOcrScheduler::submit(JobRequest request) {
    if (request.pinId == 0 || !request.acquireInput) {
        if (request.onComplete) {
            tools3000::ocr::OcrPageExtraction emptyPage{};
            request.onComplete(std::move(emptyPage), /*cancelled=*/true);
        }
        return;
    }

    CompleteTaskFn replacedCallback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_shuttingDown) {
            replacedCallback = std::move(request.onComplete);
        } else {
            // 检查队列中是否已有同一张贴图的待执行任务：原地合并并支持 SilentWarmup -> Interactive 插队提权
            bool mergedInPlace = false;
            for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
                if (it->pinId == request.pinId) {
                    const bool upgradeToInteractive =
                        (request.priority == PinOcrPriority::Interactive &&
                         it->priority != PinOcrPriority::Interactive);
                    replacedCallback = std::move(it->onComplete);
                    it->version = request.version;
                    it->acquireInput = std::move(request.acquireInput);
                    it->onComplete = std::move(request.onComplete);
                    if (upgradeToInteractive) {
                        it->priority = PinOcrPriority::Interactive;
                        JobRequest promoted = std::move(*it);
                        m_queue.erase(it);
                        auto insertPos = m_queue.begin();
                        while (insertPos != m_queue.end() &&
                               insertPos->priority == PinOcrPriority::Interactive) {
                            ++insertPos;
                        }
                        m_queue.insert(insertPos, std::move(promoted));
                    }
                    mergedInPlace = true;
                    break;
                }
            }

            if (!mergedInPlace) {
                if (request.priority == PinOcrPriority::Interactive) {
                    auto insertPos = m_queue.begin();
                    while (insertPos != m_queue.end() &&
                           insertPos->priority == PinOcrPriority::Interactive) {
                        ++insertPos;
                    }
                    m_queue.insert(insertPos, std::move(request));
                } else {
                    m_queue.push_back(std::move(request));
                }
            }

            ensureWorkerThreadStartedLocked();
            m_cv.notify_one();
        }
    }

    if (replacedCallback) {
        try {
            tools3000::ocr::OcrPageExtraction emptyPage{};
            replacedCallback(std::move(emptyPage), /*cancelled=*/true);
        } catch (...) {}
    }
}

void PinOcrScheduler::cancelPending(uint64_t pinId) {
    if (pinId == 0) return;
    std::vector<CompleteTaskFn> cancelledCallbacks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_queue.begin(); it != m_queue.end();) {
            if (it->pinId == pinId) {
                if (it->onComplete) {
                    cancelledCallbacks.push_back(std::move(it->onComplete));
                }
                it = m_queue.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& cb : cancelledCallbacks) {
        tools3000::ocr::OcrPageExtraction emptyPage{};
        cb(std::move(emptyPage), /*cancelled=*/true);
    }
}

bool PinOcrScheduler::shutdownAndDrain(uint32_t timeoutMs) noexcept {
    std::vector<CompleteTaskFn> droppedCallbacks;
    std::thread threadToJoin;
    bool drained = true;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_shuttingDown = true;
        while (!m_queue.empty()) {
            if (m_queue.front().onComplete) {
                droppedCallbacks.push_back(std::move(m_queue.front().onComplete));
            }
            m_queue.pop_front();
        }
        m_cv.notify_all();

        drained = m_doneCv.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [this]() { return !m_workerRunning && !m_jobActive; });
        if (!m_workerRunning && m_workerThread.joinable()) {
            threadToJoin = std::move(m_workerThread);
        }
    }

    for (auto& cb : droppedCallbacks) {
        try {
            tools3000::ocr::OcrPageExtraction emptyPage{};
            cb(std::move(emptyPage), /*cancelled=*/true);
        } catch (...) {}
    }

    if (threadToJoin.joinable()) {
        try {
            threadToJoin.join();
        } catch (...) {}
    }
    return drained;
}

void PinOcrScheduler::workerLoop() {
    bool dirtySinceLastTrim = false;

    while (true) {
        JobRequest currentJob{};
        CustomExtractFn customExtractorCopy;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_queue.empty() && !m_shuttingDown) {
                if (dirtySinceLastTrim) {
                    // 阶段 1：队列排空后防抖等待 3s；若期间无新贴图涌入，仅执行 1 次统一物理工作集修剪
                    const bool wokeEarly = m_cv.wait_for(
                        lock,
                        std::chrono::milliseconds(PinMemoryConstants::kSchedulerIdleTrimDelayMs),
                        [this]() { return !m_queue.empty() || m_shuttingDown; });
                    if (!wokeEarly && m_queue.empty() && !m_shuttingDown) {
                        dirtySinceLastTrim = false;
                        m_trimCount.fetch_add(1, std::memory_order_acq_rel);
                        lock.unlock();
                        cleanupIdleSchedulerWorkingSet();
                        lock.lock();
                    }
                } else {
                    // 阶段 2：修剪后继续热待命 20s，超时无新任务则优雅退场归零后台线程
                    const bool hasWork = m_cv.wait_for(
                        lock,
                        std::chrono::milliseconds(PinMemoryConstants::kSchedulerThreadKeepAliveMs),
                        [this]() { return !m_queue.empty() || m_shuttingDown; });
                    if (!hasWork && m_queue.empty()) {
                        m_workerRunning = false;
                        m_doneCv.notify_all();
                        return;
                    }
                }
            }

            if (m_shuttingDown) {
                m_workerRunning = false;
                m_doneCv.notify_all();
                return;
            }
            if (m_queue.empty()) {
                continue;
            }

            currentJob = std::move(m_queue.front());
            m_queue.pop_front();
            m_jobActive = true;
            m_activePinId = currentJob.pinId;
            customExtractorCopy = m_customExtractor;
        }

        const uint32_t runningNow = m_currentConcurrency.fetch_add(1, std::memory_order_acq_rel) + 1;
        uint32_t prevPeak = m_peakConcurrency.load(std::memory_order_relaxed);
        while (runningNow > prevPeak &&
               !m_peakConcurrency.compare_exchange_weak(prevPeak, runningNow, std::memory_order_release)) {
        }

        tools3000::core::TraceId::Scope traceScope;
        tools3000::ocr::OcrPageExtraction page{};
        bool cancelledOrSkipped = false;

        try {
            AcquiredTaskInput input = currentJob.acquireInput ? currentJob.acquireInput() : AcquiredTaskInput{};
            if (input.image.empty()) {
                cancelledOrSkipped = true;
            } else {
                dirtySinceLastTrim = true;
                struct StageGuard {
                    bool installed = false;
                    ~StageGuard() {
                        if (installed) {
                            tools3000::ocr::DeepOcrEngine::instance().setStageCallback({});
                        }
                    }
                } stageGuard{};
                if (currentJob.onStage) {
                    tools3000::ocr::DeepOcrEngine::instance().setStageCallback(currentJob.onStage);
                    stageGuard.installed = true;
                }
                if (customExtractorCopy) {
                    page = customExtractorCopy(input.image, input.scriptModeOverride);
                } else {
                    page = extractPinPageMemoryBounded(input.image, input.scriptModeOverride);
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("PinOcrScheduler: task exception on pin {}: {}", currentJob.pinId, e.what());
            cancelledOrSkipped = true;
        } catch (...) {
            LOG_ERROR("PinOcrScheduler: unknown exception on pin {}", currentJob.pinId);
            cancelledOrSkipped = true;
        }

        m_currentConcurrency.fetch_sub(1, std::memory_order_acq_rel);

        if (currentJob.onComplete) {
            try {
                currentJob.onComplete(std::move(page), cancelledOrSkipped);
            } catch (...) {}
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_jobActive = false;
            m_activePinId = 0;
            m_doneCv.notify_all();
        }
    }
}

} // namespace tools3000::capture
