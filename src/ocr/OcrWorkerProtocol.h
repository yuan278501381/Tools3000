#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// OcrWorkerProtocol.h — 独立短命 OCR Worker 进程二进制 IPC 与共享内存协议 (SSOT)
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_OCR_OCRWORKERPROTOCOL_H
#define TOOLS3000_OCR_OCRWORKERPROTOCOL_H

#include "capture/CaptureWorkerCommon.h"
#include "ocr/DeepOcrEngine.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tools3000::ocr::worker_proto {

inline constexpr uint32_t kProtocolMagic   = 0x54334F52u; // "T3OR"
inline constexpr uint32_t kProtocolVersion = 3u;
inline constexpr uint32_t kMaxOcrLines     = 100000u;
inline constexpr uint32_t kMaxCharBoxes    = 100000u;
inline constexpr uint32_t kStatusOk         = 0u;
inline constexpr uint32_t kStatusStageEvent = 100u; // 可选前导慢路径阶段通知帧（常规快识别不发送）

struct WireStageEvent {
    uint32_t stage = 0;
    uint32_t count = 0;
};

enum class Command : uint32_t {
    Ping      = 1u,
    Recognize = 2u,
    Shutdown  = 3u,
};

struct RequestHeader {
    uint32_t magic = kProtocolMagic;
    uint32_t version = kProtocolVersion;
    uint32_t command = 0;
    uint32_t reserved = 0;
};

struct RecognizeMeta {
    int32_t width = 0;
    int32_t height = 0;
    int32_t channels = 3;
    int32_t stepBytes = 0;
    uint64_t imageBytes = 0;
};

struct ResponseHeader {
    uint32_t magic = kProtocolMagic;
    uint32_t version = kProtocolVersion;
    uint32_t status = 0; // 0 = OK, non-zero = error
    uint32_t lineCount = 0;
};

struct WireRunStats {
    int64_t totalMs = 0;
    int64_t loadMs = 0;
    int64_t detMs = 0;
    int64_t packMs = 0;
    int64_t recMs = 0;
    int64_t refineMs = 0;
    uint32_t detectedQuads = 0;
    uint32_t stripCount = 0;
    uint32_t serverRefineCandidateCount = 0;
    uint32_t serverRefinedCount = 0;
    uint32_t activeSessions = 0;
    uint32_t flags = 0; // bit0: autoScriptSwitched
};

struct WireRect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct WireLineHeader {
    float confidence = 0.0f;
    WireRect box{};
    int32_t layoutRegionId = 0;
    int32_t alignedRowId = -1;
    uint32_t textBytes = 0;
    uint32_t charBoxCount = 0;
};

inline bool writeStageEvent(
    HANDLE pipe,
    OcrRecognitionStage stage,
    uint32_t count,
    const std::string& detail,
    bool overlappedHandle = false) {
    ResponseHeader hdr{};
    hdr.status = kStatusStageEvent;
    hdr.lineCount = 0;
    if (!worker::writePod(pipe, hdr, overlappedHandle)) return false;
    WireStageEvent ev{};
    ev.stage = static_cast<uint32_t>(stage);
    ev.count = count;
    if (!worker::writePod(pipe, ev, overlappedHandle)) return false;
    return worker::writeString(pipe, detail, overlappedHandle);
}

inline bool writeOcrResults(
    HANDLE pipe,
    const std::vector<OcrResult>& lines,
    const DeepOcrRunStats& stats,
    const std::string& activeTier,
    bool overlappedHandle = false) {
    if (lines.size() > kMaxOcrLines) return false;
    ResponseHeader hdr{};
    hdr.status = kStatusOk;
    hdr.lineCount = static_cast<uint32_t>(lines.size());
    if (!worker::writePod(pipe, hdr, overlappedHandle)) return false;

    WireRunStats wireStats{};
    wireStats.totalMs = stats.totalMs;
    wireStats.loadMs = stats.loadMs;
    wireStats.detMs = stats.detMs;
    wireStats.packMs = stats.packMs;
    wireStats.recMs = stats.recMs;
    wireStats.refineMs = stats.refineMs;
    wireStats.detectedQuads = static_cast<uint32_t>(stats.detectedQuads);
    wireStats.stripCount = static_cast<uint32_t>(stats.stripCount);
    wireStats.serverRefineCandidateCount = static_cast<uint32_t>(stats.serverRefineCandidateCount);
    wireStats.serverRefinedCount = static_cast<uint32_t>(stats.serverRefinedCount);
    wireStats.activeSessions = static_cast<uint32_t>(stats.activeSessions);
    wireStats.flags = stats.autoScriptSwitched ? 1u : 0u;
    if (!worker::writePod(pipe, wireStats, overlappedHandle)) return false;
    if (!worker::writeString(pipe, stats.bucketProfile, overlappedHandle)) return false;
    if (!worker::writeString(pipe, stats.detectedScript, overlappedHandle)) return false;
    if (!worker::writeString(pipe, activeTier, overlappedHandle)) return false;

    for (const auto& line : lines) {
        if (line.charBoxes.size() > kMaxCharBoxes || line.text.size() > worker::Constants::kMaxIpcStringBytes) {
            return false;
        }
        WireLineHeader lh{};
        lh.confidence = line.confidence;
        lh.box = {line.box.x, line.box.y, line.box.width, line.box.height};
        lh.layoutRegionId = line.layoutRegionId;
        lh.alignedRowId = line.alignedRowId;
        lh.textBytes = static_cast<uint32_t>(line.text.size());
        lh.charBoxCount = static_cast<uint32_t>(line.charBoxes.size());
        if (!worker::writePod(pipe, lh, overlappedHandle)) return false;
        if (lh.textBytes > 0 && !worker::writeExact(pipe, line.text.data(), lh.textBytes, overlappedHandle)) {
            return false;
        }
        if (lh.charBoxCount > 0) {
            std::vector<WireRect> rects(lh.charBoxCount);
            for (size_t i = 0; i < lh.charBoxCount; ++i) {
                rects[i] = {line.charBoxes[i].x, line.charBoxes[i].y, line.charBoxes[i].width, line.charBoxes[i].height};
            }
            if (!worker::writeExact(pipe, rects.data(), rects.size() * sizeof(WireRect), overlappedHandle)) {
                return false;
            }
        }
    }
    return true;
}

inline bool readOcrResults(
    HANDLE pipe,
    std::vector<OcrResult>& outLines,
    DeepOcrRunStats& outStats,
    std::string& outActiveTier,
    bool overlappedHandle = false,
    DWORD timeoutMs = INFINITE,
    HANDLE abortEvent = nullptr,
    const OcrStageCallback& onStage = {}) {
    outLines.clear();
    outStats = {};
    outActiveTier.clear();

    ResponseHeader hdr{};
    while (true) {
        if (!worker::readPod(pipe, hdr, overlappedHandle, timeoutMs, abortEvent)) return false;
        if (hdr.magic != kProtocolMagic || hdr.version != kProtocolVersion) {
            return false;
        }
        if (hdr.status == kStatusStageEvent) {
            WireStageEvent ev{};
            std::string detail;
            if (!worker::readPod(pipe, ev, overlappedHandle, timeoutMs, abortEvent) ||
                !worker::readString(pipe, detail, overlappedHandle, timeoutMs, abortEvent)) {
                return false;
            }
            if (onStage) {
                onStage(static_cast<OcrRecognitionStage>(ev.stage), ev.count, detail);
            }
            continue;
        }
        break;
    }
    if (hdr.status != kStatusOk || hdr.lineCount > kMaxOcrLines) {
        return false;
    }

    WireRunStats wireStats{};
    if (!worker::readPod(pipe, wireStats, overlappedHandle, timeoutMs, abortEvent)) return false;
    outStats.totalMs = wireStats.totalMs;
    outStats.loadMs = wireStats.loadMs;
    outStats.detMs = wireStats.detMs;
    outStats.packMs = wireStats.packMs;
    outStats.recMs = wireStats.recMs;
    outStats.refineMs = wireStats.refineMs;
    outStats.detectedQuads = wireStats.detectedQuads;
    outStats.stripCount = wireStats.stripCount;
    outStats.serverRefineCandidateCount = wireStats.serverRefineCandidateCount;
    outStats.serverRefinedCount = wireStats.serverRefinedCount;
    outStats.activeSessions = wireStats.activeSessions;
    outStats.autoScriptSwitched = ((wireStats.flags & 1u) != 0u);
    if (!worker::readString(pipe, outStats.bucketProfile, overlappedHandle, timeoutMs, abortEvent)) return false;
    if (!worker::readString(pipe, outStats.detectedScript, overlappedHandle, timeoutMs, abortEvent)) return false;
    if (!worker::readString(pipe, outActiveTier, overlappedHandle, timeoutMs, abortEvent)) return false;

    outLines.reserve(hdr.lineCount);
    for (uint32_t i = 0; i < hdr.lineCount; ++i) {
        WireLineHeader lh{};
        if (!worker::readPod(pipe, lh, overlappedHandle, timeoutMs, abortEvent)) return false;
        if (lh.textBytes > worker::Constants::kMaxIpcStringBytes || lh.charBoxCount > kMaxCharBoxes) {
            return false;
        }
        OcrResult item{};
        item.confidence = lh.confidence;
        item.box = cv::Rect(lh.box.x, lh.box.y, lh.box.width, lh.box.height);
        item.layoutRegionId = lh.layoutRegionId;
        item.alignedRowId = lh.alignedRowId;
        if (lh.textBytes > 0) {
            item.text.resize(lh.textBytes);
            if (!worker::readExact(pipe, item.text.data(), lh.textBytes, overlappedHandle, timeoutMs, abortEvent)) {
                return false;
            }
        }
        if (lh.charBoxCount > 0) {
            std::vector<WireRect> rects(lh.charBoxCount);
            if (!worker::readExact(pipe, rects.data(), rects.size() * sizeof(WireRect), overlappedHandle, timeoutMs, abortEvent)) {
                return false;
            }
            item.charBoxes.reserve(lh.charBoxCount);
            for (const auto& r : rects) {
                item.charBoxes.emplace_back(r.x, r.y, r.width, r.height);
            }
        }
        outLines.push_back(std::move(item));
    }
    return true;
}

} // namespace tools3000::ocr::worker_proto

#endif // TOOLS3000_OCR_OCRWORKERPROTOCOL_H
