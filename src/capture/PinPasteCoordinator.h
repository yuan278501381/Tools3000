#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinPasteCoordinator.h — 世界级连续历史贴图会话调度中枢
//
// 架构职责与算法契约:
//   1. 连续历史倒序出图 (Reverse History Rollback):
//      在连续触发贴图快捷键（如按第 1 次、第 2 次、第 3 次...）的会话窗口内，
//      依次决策贴出倒数第 1 张、倒数第 2 张、倒数第 3 张... 直至第 N 张；
//   2. 剪贴板与历史变动自愈 (Mutation Invalidation):
//      若在会话期间产生新截图或外部剪贴板序列号递增，自动将回溯游标重置为 0；
//   3. 超时重置机制 (Session Expiry):
//      超过设定阈值（默认 3000ms）未按贴图快捷键，会话自动过期重置；
//   4. 外部剪贴板与截图历史融合 (Seamless Fusion):
//      若系统剪贴板包含外部复制图片/文本，将其作为倒数第 1 项，
//      历史截图顺延为倒数第 2、3... 项，两类操作浑然一体；
//   5. 级联瀑布流避让与原位还原 (Smart Waterfall Placement):
//      依托 PinSpawnCalculator，不同区域截图原位还原，重合区域自动阶梯下沉避让；
//   6. 零 Emoji 极客体验与环回保护 (Zero Emoji & Wrap-around):
//      超出最早一张后安全循环回最新一张并返回完整决策上下文。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_PINPASTECOORDINATOR_H
#define TOOLS3000_CAPTURE_PINPASTECOORDINATOR_H

#include <windows.h>
#include <memory>
#include <chrono>
#include <mutex>
#include <vector>
#include <string>
#include <optional>
#include <opencv2/core.hpp>
#include "capture/ScreenCapture.h"

namespace tools3000::capture {

/// 贴图候选条目
struct PinPasteCandidate {
    cv::Mat image;
    CaptureRegion region{};
    int padX = 0;
    int padY = 0;
    bool isFromClipboard = false;
    std::string timestamp;
};

/// 贴图决策结果
struct PinPasteDecision {
    PinPasteCandidate candidate;
    POINT spawnPos{0, 0};
    int reverseIndex = 0;       // 本次会话触发的累计步数 (0, 1, 2...)
    int targetIndex = 0;        // 实际映射到的候选索引 (0..totalCount-1)
    int totalCount = 0;         // 可用总候选数
    bool isLooped = false;       // 是否发生了环回至最新
};

class PinPasteCoordinator {
public:
    static PinPasteCoordinator& instance();

    /// 步进并计算下一次贴图的完整决策信息（状态机步进、原位/避让坐标计算）
    std::optional<PinPasteDecision> stepNext(const POINT* fallbackCursor = nullptr);

    /// 重置连续贴图会话
    void reset();

    /// 当前回溯索引（0 = 倒数第 1 张，1 = 倒数第 2 张...）
    int currentReverseIndex() const;

    /// 会话是否处于激活状态（最近一次按键在超时窗口内）
    bool isSessionActive() const;

    /// 设置会话超时时间（毫秒）
    void setSessionTimeoutMs(int timeoutMs);

    /// 解析当前所有候选贴图列表（供单元测试与算法校验）
    std::vector<PinPasteCandidate> resolveCandidates(DWORD currentSeq, int historyCount);

private:
    PinPasteCoordinator() = default;
    PinPasteCoordinator(const PinPasteCoordinator&) = delete;
    PinPasteCoordinator& operator=(const PinPasteCoordinator&) = delete;

    mutable std::mutex m_mutex;
    int m_reverseIndex = 0;
    std::chrono::steady_clock::time_point m_lastTriggerTime{};
    DWORD m_lastClipboardSeq = 0;
    int m_lastHistoryCount = 0;
    int m_sessionTimeoutMs = 3000;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_PINPASTECOORDINATOR_H
