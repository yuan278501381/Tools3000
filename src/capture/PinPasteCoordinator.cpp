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
#include <opencv2/imgproc.hpp>

namespace tools3000::capture {

bool PinPasteCoordinator::parseHexColor(const std::wstring& in, cv::Scalar& bgr, std::wstring& label) {
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
    label = s;
    return true;
}

cv::Mat PinPasteCoordinator::renderColorSwatch(const cv::Scalar& bgr, const std::wstring& label) {
    const int w = 180, h = 130;
    cv::Mat img(h, w, CV_8UC3, bgr);
    double lum = 0.114 * bgr[0] + 0.587 * bgr[1] + 0.299 * bgr[2];
    cv::Scalar tc = lum > 140 ? cv::Scalar(20, 20, 20) : cv::Scalar(240, 240, 240);
    std::string lbl;
    for (auto c : label) lbl.push_back(static_cast<char>(c));
    cv::putText(img, lbl, cv::Point(14, h - 18), cv::FONT_HERSHEY_SIMPLEX, 0.7, tc, 1, cv::LINE_AA);
    return img;
}

cv::Mat PinPasteCoordinator::renderTextToImage(const std::wstring& text) {
    size_t firstNonWs = text.find_first_not_of(L" \t\r\n");
    if (firstNonWs == std::wstring::npos) {
        return {};
    }
    // 剔除尾部纯空白与换行，避免卡片底部产生多余空行留白
    size_t lastNonWs = text.find_last_not_of(L" \t\r\n");
    std::wstring cleanText = (lastNonWs != std::wstring::npos) ? text.substr(0, lastNonWs + 1) : text;

    HDC screen = GetDC(nullptr);
    HDC memdc = CreateCompatibleDC(screen);

    bool isCode = (cleanText.find(L'\n') != std::wstring::npos) ||
                  (cleanText.find(L'{') != std::wstring::npos) ||
                  (cleanText.find(L"def ") != std::wstring::npos) ||
                  (cleanText.find(L"const ") != std::wstring::npos) ||
                  (cleanText.find(L"function") != std::wstring::npos) ||
                  (cleanText.find(L"#include") != std::wstring::npos) ||
                  (cleanText.find(L"import ") != std::wstring::npos) ||
                  (cleanText.find(L"class ") != std::wstring::npos);

    bool hasCjk = false;
    for (wchar_t ch : cleanText) {
        if (ch >= 0x2E80) {
            hasCjk = true;
            break;
        }
    }

    LPCWSTR fontFace = (isCode && !hasCjk) ? L"Cascadia Code" : L"Microsoft YaHei UI";
    HFONT font = CreateFontW(-19, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, fontFace);
    HGDIOBJ oldFont = SelectObject(memdc, font);

    const int headerH = 28;     // 顶部 Mac 窗口控制栏高度
    const int padX = 18;        // 水平内边距
    const int padY = 14;        // 垂直内边距
    const int maxW = 720;
    const UINT drawFlags = DT_WORDBREAK | DT_EDITCONTROL | DT_EXPANDTABS | DT_NOPREFIX;

    RECT rc{0, 0, maxW, 0};
    DrawTextW(memdc, cleanText.c_str(), -1, &rc, drawFlags | DT_CALCRECT);
    int contentW = std::min<LONG>(rc.right, maxW);
    int contentH = rc.bottom;

    int w = std::clamp(contentW + padX * 2 + 10, 160, maxW + padX * 2 + 10);
    int h = std::clamp(headerH + contentH + padY * 2, 64, 2400);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    cv::Mat result;
    if (dib && bits) {
        HGDIOBJ oldBm = SelectObject(memdc, dib);

        // 1. 卡片深邃背景 (#18181B - Zinc 900)
        RECT full{0, 0, w, h};
        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 27));
        FillRect(memdc, &full, bg);
        DeleteObject(bg);

        // 2. 顶部 Mac 红黄绿三色微圆点 (10px 直径)
        HBRUSH redDot = CreateSolidBrush(RGB(255, 95, 86));
        HBRUSH yellowDot = CreateSolidBrush(RGB(255, 189, 46));
        HBRUSH greenDot = CreateSolidBrush(RGB(39, 201, 63));
        HPEN nullPen = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ oldPen = SelectObject(memdc, nullPen);

        SelectObject(memdc, redDot);
        Ellipse(memdc, 14, 10, 24, 20);
        SelectObject(memdc, yellowDot);
        Ellipse(memdc, 30, 10, 40, 20);
        SelectObject(memdc, greenDot);
        Ellipse(memdc, 46, 10, 56, 20);

        DeleteObject(redDot);
        DeleteObject(yellowDot);
        DeleteObject(greenDot);

        // 3. 顶部微弱分割线 (#27272A)
        HPEN divPen = CreatePen(PS_SOLID, 1, RGB(39, 39, 42));
        SelectObject(memdc, divPen);
        MoveToEx(memdc, 0, headerH, nullptr);
        LineTo(memdc, w, headerH);
        DeleteObject(divPen);

        // 4. 外边框微高光 (#3F3F46)
        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(63, 63, 70));
        SelectObject(memdc, borderPen);
        MoveToEx(memdc, 0, 0, nullptr);
        LineTo(memdc, w - 1, 0);
        LineTo(memdc, w - 1, h - 1);
        LineTo(memdc, 0, h - 1);
        LineTo(memdc, 0, 0);
        DeleteObject(borderPen);

        SelectObject(memdc, oldPen);
        DeleteObject(nullPen);

        // 5. 文本与代码渲染
        SetBkMode(memdc, TRANSPARENT);
        SetTextColor(memdc, RGB(244, 244, 245));
        RECT tr{padX, headerH + padY, w - padX, h - padY};
        DrawTextW(memdc, cleanText.c_str(), -1, &tr, drawFlags);
        GdiFlush();

        cv::Mat bgra(h, w, CV_8UC4, bits);
        cv::cvtColor(bgra, result, cv::COLOR_BGRA2BGR);

        SelectObject(memdc, oldBm);
        DeleteObject(dib);
    }

    SelectObject(memdc, oldFont);
    DeleteObject(font);
    DeleteDC(memdc);
    ReleaseDC(nullptr, screen);
    return result;
}

namespace {

void inspectClipboardTextAndPriority(std::wstring& outText, bool& outPreferTextOverImage) {
    outText.clear();
    outPreferTextOverImage = false;

    bool opened = false;
    for (int retry = 0; retry < 10; ++retry) {
        if (OpenClipboard(nullptr)) {
            opened = true;
            break;
        }
        Sleep(5);
    }
    if (!opened) return;

    const UINT formatPinMeta = RegisterClipboardFormatW(L"Tools3000_PinMetadata");
    const UINT formatPng = RegisterClipboardFormatW(L"PNG");
    const bool hasPinMeta = (formatPinMeta != 0 && IsClipboardFormatAvailable(formatPinMeta));

    if (!hasPinMeta) {
        for (UINT fmt = EnumClipboardFormats(0); fmt != 0; fmt = EnumClipboardFormats(fmt)) {
            if (fmt == CF_UNICODETEXT || fmt == CF_TEXT || fmt == CF_OEMTEXT) {
                outPreferTextOverImage = true;
                break;
            }
            if ((formatPng != 0 && fmt == formatPng) ||
                fmt == CF_DIBV5 || fmt == CF_DIB || fmt == CF_BITMAP) {
                outPreferTextOverImage = false;
                break;
            }
        }
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                outText = p;
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
}

cv::Mat renderClipboardTextOrColor(const std::wstring& clipText) {
    if (clipText.empty()) return {};
    cv::Scalar c;
    std::wstring hexLabel;
    if (PinPasteCoordinator::parseHexColor(clipText, c, hexLabel)) {
        return PinPasteCoordinator::renderColorSwatch(c, hexLabel);
    }
    return PinPasteCoordinator::renderTextToImage(clipText);
}

} // namespace

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

    // 1. 探测系统剪贴板中的文本与图像优先级（当富文本编辑器/Office 将 CF_UNICODETEXT 置于首位且附带预览位图时，优先转为文字贴图卡片）
    std::wstring clipText;
    bool preferTextOverImage = false;
    inspectClipboardTextAndPriority(clipText, preferTextOverImage);

    Tools3000PinMetadata meta{};
    meta.magic = 0;
    cv::Mat clipImg;
    bool clipFromTextOrColor = false;

    if (preferTextOverImage && !clipText.empty()) {
        clipImg = renderClipboardTextOrColor(clipText);
        clipFromTextOrColor = !clipImg.empty();
    }

    if (clipImg.empty()) {
        clipImg = ClipboardUtils::readImageFromClipboard(&meta);
    }

    if (clipImg.empty() && !clipText.empty()) {
        clipImg = renderClipboardTextOrColor(clipText);
        clipFromTextOrColor = !clipImg.empty();
    }

    // 2. 检查 CaptureHistory 中的条目
    auto& history = CaptureHistory::instance();
    const int count = (historyCount >= 0) ? historyCount : history.count();

    // 检查剪贴板图像是否已在 CaptureHistory[0] 中匹配（文本/色卡生成的图像绝不与历史截图判重）
    bool clipboardMatchesHistory0 = false;
    if (!clipImg.empty() && !clipFromTextOrColor && count > 0) {
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
