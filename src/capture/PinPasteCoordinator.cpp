// ─────────────────────────────────────────────────────────────────────────────
// PinPasteCoordinator.cpp — 世界级连续历史贴图会话调度中枢实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinPasteCoordinator.h"
#include "capture/PinSpawnCalculator.h"
#include "capture/CaptureHistory.h"
#include "capture/ClipboardUtils.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include <algorithm>
#include <cwctype>

namespace tools3000::capture {

namespace {

bool parseHexColorLocal(const std::wstring& in, cv::Scalar& bgr) {
    size_t a = in.find_first_not_of(L" \t\r\n");
    size_t b = in.find_last_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return false;
    std::wstring s = in.substr(a, b - a + 1);
    if (s.size() < 4 || s[0] != L'#') return false;
    std::wstring hex = s.substr(1);
    if (hex.size() != 3 && hex.size() != 6) return false;
    for (wchar_t c : hex) if (!std::iswxdigit(c)) return false;

    auto hv = [](wchar_t c) -> int {
        c = std::towlower(c);
        return (c <= L'9') ? (c - L'0') : (c - L'a' + 10);
    };
    int r, g, bl;
    if (hex.size() == 3) { r = hv(hex[0]) * 17; g = hv(hex[1]) * 17; bl = hv(hex[2]) * 17; }
    else { r = hv(hex[0]) * 16 + hv(hex[1]); g = hv(hex[2]) * 16 + hv(hex[3]); bl = hv(hex[4]) * 16 + hv(hex[5]); }
    bgr = cv::Scalar(bl, g, r);
    return true;
}

cv::Mat renderColorSwatchLocal(const cv::Scalar& bgr) {
    const int w = 180, h = 130;
    return cv::Mat(h, w, CV_8UC3, bgr);
}

} // anonymous namespace

PinPasteCoordinator& PinPasteCoordinator::instance() {
    static PinPasteCoordinator coord;
    return coord;
}

void PinPasteCoordinator::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_reverseIndex = 0;
    m_lastTriggerTime = {};
    m_lastClipboardSeq = 0;
    m_lastHistoryCount = 0;
}

int PinPasteCoordinator::currentReverseIndex() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_reverseIndex;
}

bool PinPasteCoordinator::isSessionActive() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lastTriggerTime.time_since_epoch().count() <= 0) return false;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTriggerTime).count();
    return elapsed <= m_sessionTimeoutMs;
}

void PinPasteCoordinator::setSessionTimeoutMs(int timeoutMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessionTimeoutMs = (std::max)(500, timeoutMs);
}

std::vector<PinPasteCandidate> PinPasteCoordinator::resolveCandidates([[maybe_unused]] DWORD currentSeq, int historyCount) {
    std::vector<PinPasteCandidate> candidates;

    // 1. 读取系统剪贴板中的图像/元数据
    Tools3000PinMetadata meta{};
    cv::Mat clipImg = ClipboardUtils::readImageFromClipboard(&meta);

    if (clipImg.empty()) {
        std::wstring clipText;
        if (OpenClipboard(nullptr)) {
            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                        clipText = p;
                        GlobalUnlock(h);
                    }
                }
            }
            CloseClipboard();
        }

        if (!clipText.empty()) {
            cv::Scalar c;
            if (parseHexColorLocal(clipText, c)) {
                clipImg = renderColorSwatchLocal(c);
            }
        }
    }

    // 2. 检查 CaptureHistory 中的条目
    auto& history = CaptureHistory::instance();
    const int count = (historyCount >= 0) ? historyCount : history.count();

    // 检查剪贴板内容是否已在 CaptureHistory[0] 中匹配
    bool clipboardMatchesHistory0 = false;
    if (!clipImg.empty() && count > 0) {
        auto latestOpt = history.get(0);
        if (latestOpt.has_value() && !latestOpt->image.empty()) {
            const auto& latest = *latestOpt;
            if (meta.magic == 0x54333030 &&
                meta.regionW == latest.region.width &&
                meta.regionH == latest.region.height &&
                clipImg.cols == latest.image.cols &&
                clipImg.rows == latest.image.rows) {
                clipboardMatchesHistory0 = true;
            } else if (clipImg.cols == latest.image.cols &&
                       clipImg.rows == latest.image.rows &&
                       clipImg.type() == latest.image.type()) {
                clipboardMatchesHistory0 = true;
            }
        }
    }

    // 3. 构造候选列表
    // 若剪贴板中有独立于历史的内容（例如外部复制图片或颜色），将其作为首选候选项（倒数第 1 个内容）
    if (!clipImg.empty() && !clipboardMatchesHistory0) {
        PinPasteCandidate clipCand;
        clipCand.image = clipImg;
        if (meta.magic == 0x54333030 && meta.regionW > 0 && meta.regionH > 0) {
            clipCand.region = CaptureRegion{meta.regionX, meta.regionY, meta.regionW, meta.regionH, meta.cornerRadius};
            clipCand.padX = meta.padX;
            clipCand.padY = meta.padY;
        }
        clipCand.isFromClipboard = true;
        candidates.push_back(std::move(clipCand));
    }

    // 依序推入 CaptureHistory 中的所有截图（从 0 到 count-1，即按保存时间倒序排列）
    for (int i = 0; i < count; ++i) {
        auto entryOpt = history.get(i);
        if (!entryOpt.has_value() || entryOpt->image.empty()) continue;
        const auto& entry = *entryOpt;
        PinPasteCandidate histCand;
        histCand.image = entry.image;
        histCand.region = entry.region;
        histCand.timestamp = entry.timestamp;
        if (entry.region.width > 0 && entry.image.cols > entry.region.width) {
            histCand.padX = (entry.image.cols - entry.region.width) / 2;
        }
        if (entry.region.height > 0 && entry.image.rows > entry.region.height) {
            histCand.padY = (entry.image.rows - entry.region.height) / 2;
        }
        histCand.isFromClipboard = false;
        candidates.push_back(std::move(histCand));
    }

    // 若历史为空且剪贴板仅包含与已清空历史无关的有效图片
    if (candidates.empty() && !clipImg.empty()) {
        PinPasteCandidate clipCand;
        clipCand.image = clipImg;
        clipCand.isFromClipboard = true;
        candidates.push_back(std::move(clipCand));
    }

    return candidates;
}

std::optional<PinPasteDecision> PinPasteCoordinator::stepNext(const POINT* fallbackCursor) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::steady_clock::now();
    DWORD currentSeq = GetClipboardSequenceNumber();
    int historyCount = CaptureHistory::instance().count();

    bool sessionValid = false;
    if (m_lastTriggerTime.time_since_epoch().count() > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTriggerTime).count();
        if (elapsed <= m_sessionTimeoutMs &&
            currentSeq == m_lastClipboardSeq &&
            historyCount == m_lastHistoryCount) {
            sessionValid = true;
        }
    }

    if (!sessionValid) {
        m_reverseIndex = 0;
    } else {
        m_reverseIndex++;
    }

    auto candidates = resolveCandidates(currentSeq, historyCount);
    if (candidates.empty()) {
        LOG_INFO("PinPasteCoordinator: 剪贴板与历史记录中无可贴内容");
        m_reverseIndex = 0;
        return std::nullopt;
    }

    const int totalCandidates = static_cast<int>(candidates.size());
    const int targetIndex = m_reverseIndex % totalCandidates;
    const auto& candidate = candidates[targetIndex];

    POINT cursor{};
    if (fallbackCursor) {
        cursor = *fallbackCursor;
    } else {
        GetCursorPos(&cursor);
    }

    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    RECT work = tools3000::core::dpi::workArea(mon);

    int minVx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int minVy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int maxVw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int maxVh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT vScreen{minVx, minVy, minVx + maxVw, minVy + maxVh};

    PinSpawnInput input;
    input.imageWidth = candidate.image.cols;
    input.imageHeight = candidate.image.rows;
    input.padX = candidate.padX;
    input.padY = candidate.padY;
    input.cursor = cursor;
    input.workArea = work;
    input.virtualScreen = vScreen;

    if (candidate.region.width > 0 && candidate.region.height > 0) {
        input.preferredRegion = PinSpawnRegion{
            candidate.region.x, candidate.region.y,
            candidate.region.width, candidate.region.height
        };
    }

    // 动态探测桌面所有现有活跃贴图窗口，用于阶梯瀑布避让
    HWND hExisting = nullptr;
    while ((hExisting = FindWindowExW(nullptr, hExisting, L"Tools3000_PinWindow", nullptr)) != nullptr) {
        if (IsWindowVisible(hExisting)) {
            RECT rc{};
            GetWindowRect(hExisting, &rc);
            input.existingPins.push_back(rc);
        }
    }

    POINT spawnPos = PinSpawnCalculator::calculate(input);

    PinPasteDecision decision;
    decision.candidate = candidate;
    decision.spawnPos = spawnPos;
    decision.reverseIndex = m_reverseIndex;
    decision.targetIndex = targetIndex;
    decision.totalCount = totalCandidates;
    decision.isLooped = (m_reverseIndex > 0 && targetIndex == 0);

    m_lastTriggerTime = now;
    m_lastClipboardSeq = currentSeq;
    m_lastHistoryCount = historyCount;

    LOG_INFO("PinPasteCoordinator: 贴出历史条目 [{}/{}], 尺寸={}x{}, 坐标=({}, {})",
             targetIndex + 1, totalCandidates, candidate.image.cols, candidate.image.rows, spawnPos.x, spawnPos.y);

    return decision;
}

} // namespace tools3000::capture
