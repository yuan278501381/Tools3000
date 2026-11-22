// ─────────────────────────────────────────────────────────────────────────────
// InPlaceTranslateEngine.cpp — 微信级截图识别原位翻译排版与无痕合成引擎实现
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/InPlaceTranslateEngine.h"

#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <unordered_set>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace tools3000::capture {

namespace {

constexpr float kMinReadableFontPx = 8.0f;
constexpr float kMinFontScaleFactor = 0.45f;
constexpr float kMaxFontScaleFactor = 1.00f;
constexpr int kMaxSafeExpandRightPx = 128;
constexpr int kMaxSafeExpandBottomMultiLinePx = 22;
constexpr int kMaxSafeExpandBottomSingleLinePx = 3;
constexpr int kMinObstacleMarginPx = 3;

void ensureThreadComInitialized() noexcept {
    thread_local bool s_comInit = false;
    if (!s_comInit) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
            s_comInit = true;
        }
    }
}

struct TranslateD2dFactories {
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IWICImagingFactory> wic;

    bool init() noexcept {
        if (dwrite && d2d && wic) return true;
        ensureThreadComInitialized();
        if (!dwrite) {
            DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
        }
        if (!d2d) {
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
        }
        if (!wic) {
            CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wic.GetAddressOf()));
        }
        return dwrite && d2d && wic;
    }
};

TranslateD2dFactories& threadFactories() noexcept {
    thread_local TranslateD2dFactories s_fac;
    return s_fac;
}

bool isCjkWideChar(wchar_t ch) noexcept {
    const uint32_t cp = static_cast<uint32_t>(ch);
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x3040 && cp <= 0x30FF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF);
}

bool endsWithCjkOrFullwidth(const std::string& utf8) noexcept {
    if (utf8.empty()) return false;
    const std::wstring w = tools3000::core::WinUtils::utf8ToWstring(utf8);
    for (auto it = w.rbegin(); it != w.rend(); ++it) {
        if (!std::iswspace(*it)) {
            return isCjkWideChar(*it) || (*it >= 0x3000 && *it <= 0x303F) || (*it >= 0xFF00 && *it <= 0xFFEF);
        }
    }
    return false;
}

bool startsWithCjk(const std::string& utf8) noexcept {
    if (utf8.empty()) return false;
    const std::wstring w = tools3000::core::WinUtils::utf8ToWstring(utf8);
    for (wchar_t ch : w) {
        if (!std::iswspace(ch)) {
            return isCjkWideChar(ch);
        }
    }
    return false;
}

std::string trimWhitespace(std::string_view sv) {
    size_t s = 0;
    while (s < sv.size() && std::isspace(static_cast<unsigned char>(sv[s]))) ++s;
    size_t e = sv.size();
    while (e > s && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(s, e - s));
}

bool looksLikeUrlOrFilePath(std::string_view sv) noexcept {
    const std::string t = trimWhitespace(sv);
    if (t.size() < 4) return false;
    if (t.find(' ') != std::string::npos) return false;
    if (t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0 ||
        t.rfind("ftp://", 0) == 0 || t.rfind("www.", 0) == 0) {
        return true;
    }
    if (t.size() >= 3 && std::isalpha(static_cast<unsigned char>(t[0])) &&
        t[1] == ':' && (t[2] == '\\' || t[2] == '/')) {
        return true;
    }
    return false;
}

/// 跳过无空格的工业料号、序列号、工单号与软件版本号（如 ZZ00445707、501023856AA、AS0002、V8.6.0.724）
bool looksLikeIdentifierOrSerialCode(std::string_view sv) noexcept {
    const std::string t = trimWhitespace(sv);
    if (t.size() < 4) return false;
    if (t.find(' ') != std::string::npos) return false;

    size_t upper = 0;
    size_t lower = 0;
    size_t digits = 0;
    size_t otherPunct = 0;
    for (unsigned char c : t) {
        if (c >= 0x80) return false; // 含非 ASCII 文本（如中文型号“奇瑞T1Q项目”）必须保留翻译
        if (c >= 'A' && c <= 'Z') {
            ++upper;
        } else if (c >= 'a' && c <= 'z') {
            ++lower;
        } else if (c >= '0' && c <= '9') {
            ++digits;
        } else if (c == '-' || c == '_' || c == '.' || c == '/' || c == ':') {
            ++otherPunct;
        } else {
            return false;
        }
    }
    if (digits < 2) return false;
    // 版本号 (v8.6.0.724 / V1.4.0)
    if ((t[0] == 'v' || t[0] == 'V') && (upper + lower == 1) && digits >= 2) {
        return true;
    }
    // 纯大写字母+数字混合编码 (ZZ00445707 / 501023856AA / HB0502000441 / AS0002)
    if (lower == 0 && upper >= 1 && digits >= 2) {
        return true;
    }
    // 带多个数字与点/斜杠/下划线混合的紧凑技术标识
    if (digits >= 4 && otherPunct >= 1 && lower <= 2) {
        return true;
    }
    return false;
}

bool isSingleCodeIdentifierToken(std::string_view token) noexcept {
    if (token.size() < 2) return false;
    bool hasAlpha = false;
    bool hasLower = false;
    bool hasInternalUpper = false;
    bool hasUnderscore = false;
    bool hasUpper = false;
    for (size_t i = 0; i < token.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(token[i]);
        if (c >= 0x80) return false;
        if (c >= 'A' && c <= 'Z') {
            hasAlpha = true;
            hasUpper = true;
            if (i > 0 && hasLower) {
                hasInternalUpper = true; // 驼峰 / 帕斯卡 (BaseUrl, postUDFs, AlternativeTo, PostgreSQL)
            }
        } else if (c >= 'a' && c <= 'z') {
            hasAlpha = true;
            hasLower = true;
            if (i > 1 && hasUpper && static_cast<unsigned char>(token[i - 1]) >= 'A' &&
                static_cast<unsigned char>(token[i - 1]) <= 'Z' &&
                static_cast<unsigned char>(token[i - 2]) >= 'A' &&
                static_cast<unsigned char>(token[i - 2]) <= 'Z') {
                hasInternalUpper = true; // 首缩写+驼峰 (HTTPServer, XMLParser)
            }
        } else if (c >= '0' && c <= '9') {
            // allow digits
        } else if (c == '_') {
            hasUnderscore = true;
        } else if (c == '$' || c == '-' || c == '.') {
            // allow identifier punct
        } else {
            return false;
        }
    }
    if (!hasAlpha) return false;
    if (hasUnderscore) return true;
    if (hasInternalUpper) return true;
    if (token[0] == '$') return true;
    return false;
}

/// 开发软件与代码语义防火墙：精准拦截 JSON 键值行、模板变量、代码标识符、密钥/邮箱与 HTTP/技术缩写
bool looksLikeCodeOrTechToken(std::string_view sv) noexcept {
    const std::string t = trimWhitespace(sv);
    if (t.empty()) return false;

    // 1. JSON 键值对代码行（如 `"SessionId": "203ac..."` / `"SessionTimeout": 30` / `"@odata.context": ...`）
    if ((t.front() == '"' || t.front() == '\'') && t.find("\":") != std::string::npos) {
        return true;
    }
    if (t.front() == '"' && t.find("\":") == std::string::npos && t.find("\" :") != std::string::npos) {
        return true;
    }

    // 2. 模板占位符（如 `{{BaseUrl}}/Login`）
    const size_t openTpl = t.find("{{");
    if (openTpl != std::string::npos && t.find("}}", openTpl + 2) != std::string::npos) {
        return true;
    }

    const bool hasSpace = (t.find(' ') != std::string::npos || t.find('\t') != std::string::npos);

    // 3. 无空格单 Token 精准判定
    if (!hasSpace) {
        // 以技术符号开头（如 `$batch`、`/api/v1/login`、`@odata.context`）
        if ((t[0] == '$' || t[0] == '/' || t[0] == '@') && t.size() >= 2) {
            bool allAscii = true;
            for (unsigned char c : t) {
                if (c >= 0x80) { allAscii = false; break; }
            }
            if (allAscii) return true;
        }
        // 含 `@` 的密码/邮箱/连接串（如 `Sino@24Ch`、`dev@company.com`）
        if (t.find('@') != std::string::npos) {
            bool allAscii = true;
            for (unsigned char c : t) {
                if (c >= 0x80) { allAscii = false; break; }
            }
            if (allAscii) return true;
        }
        // 驼峰/帕斯卡/下划线代码标识符（如 `BaseUrl`, `CmpnyDB`, `ChGateWay`, `Team_Test`, `SINO_SAP`, `postUDFs`）
        if (isSingleCodeIdentifierToken(t)) {
            return true;
        }

        // 剥离首尾轻微标点后检查纯全大写技术缩写 / HTTP 动词 / 非常规无元音代号
        size_t s = 0;
        size_t e = t.size();
        while (s < e && (t[s] == '[' || t[s] == '(' || t[s] == '{' || t[s] == '<' || t[s] == '"')) ++s;
        while (e > s && (t[e - 1] == ']' || t[e - 1] == ')' || t[e - 1] == '}' || t[e - 1] == '>' ||
                         t[e - 1] == '"' || t[e - 1] == ':' || t[e - 1] == ',')) --e;
        const std::string_view core = std::string_view(t).substr(s, e - s);
        if (core.size() >= 2 && core.size() <= 10) {
            bool allUpperAlpha = true;
            bool hasVowel = false;
            int trailingConsonants = 0;
            for (size_t i = 0; i < core.size(); ++i) {
                const unsigned char c = static_cast<unsigned char>(core[i]);
                if (c < 'A' || c > 'Z') {
                    allUpperAlpha = false;
                    break;
                }
                const bool isVowel = (c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U' || c == 'Y');
                if (isVowel) {
                    hasVowel = true;
                    trailingConsonants = 0;
                } else {
                    ++trailingConsonants;
                }
            }
            if (allUpperAlpha) {
                static const std::unordered_set<std::string_view> kTechAcronyms = {
                    "GET", "POST", "PUT", "PATCH", "DEL", "DELETE", "HEAD", "OPTIONS",
                    "PWD", "UID", "PID", "TID", "ID", "URL", "URI", "URN", "API", "SDK",
                    "CLI", "GUI", "MCP", "LLM", "AI", "UI", "UX", "SQL", "JSON", "XML",
                    "YAML", "TOML", "HTML", "CSS", "JS", "TS", "PHP", "CPP", "HTTP", "HTTPS",
                    "TCP", "UDP", "IP", "DNS", "TLS", "SSL", "SSH", "FTP", "JWT", "OAUTH",
                    "CORS", "CRUD", "REST", "GRPC", "CPU", "GPU", "RAM", "ROM", "SSD", "HDD",
                    "USB", "HDMI", "DPI", "FPS", "OK", "EOF", "NULL", "NAN", "ENV", "DEV",
                    "PROD", "UAT", "QA", "CI", "CD", "UTF", "ASCII", "PNG", "JPG", "JPEG",
                    "GIF", "SVG", "WEBP", "PDF", "ZIP", "RAR", "TAR", "EXE", "DLL", "MSI",
                    "NGINX", "REDIS", "MYSQL", "UDT", "UDF", "UDO", "KCC", "ERP", "SAP",
                    "CRM", "OA", "BI"
                };
                if (kTechAcronyms.contains(core)) {
                    return true;
                }
                // 2~5 字符全大写且完全无元音的紧凑代号（如 `PWD`、`SQL`、`KCC`、`BTN`、`CFG`）
                if (core.size() <= 5 && !hasVowel) {
                    return true;
                }
                // 3~6 字符全大写且以 3+ 非常规辅音簇收尾的内部代号（如 `WEBCH`，排除英语常见复数/动词词尾）
                if (core.size() >= 3 && core.size() <= 6 && trailingConsonants >= 3) {
                    const std::string_view tail3 = core.substr(core.size() - 3);
                    static const std::unordered_set<std::string_view> kEnglishTail3 = {
                        "NGS", "NTS", "STS", "RKS", "LDS", "NDS", "RTS", "PTS", "CTS",
                        "SKS", "SPS", "MPS", "LPS", "LTS", "FTS", "XTS", "NCH", "TCH",
                        "SCH", "RCH", "LCH", "GHT", "GTH", "CKS", "NKS", "RNS", "RMS",
                        "RTH", "NTH", "LTH", "WNS", "RLS", "LFS", "LVS"
                    };
                    if (!kEnglishTail3.contains(tail3)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // 4. API 树节点条目（如 `GET UserObjectsMD`、`POST postUDFs`、`DEL deleteUDF`、`PATCH DocumentAdditionalExpenses Copy`）
    const size_t firstSp = t.find(' ');
    if (firstSp >= 3 && firstSp <= 6) {
        const std::string_view verb = std::string_view(t).substr(0, firstSp);
        if (verb == "GET" || verb == "POST" || verb == "PUT" || verb == "PATCH" || verb == "DEL" || verb == "DELETE") {
            std::string_view rest = std::string_view(t).substr(firstSp + 1);
            while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
            const size_t nextSp = rest.find(' ');
            const std::string_view firstArg = (nextSp == std::string_view::npos) ? rest : rest.substr(0, nextSp);
            if (isSingleCodeIdentifierToken(firstArg) || looksLikeIdentifierOrSerialCode(firstArg)) {
                return true;
            }
        }
    }
    return false;
}

size_t countVisualChars(std::string_view utf8) noexcept {
    return tools3000::core::WinUtils::utf8ToWstring(trimWhitespace(utf8)).size();
}

bool startsWithListBulletOrNumber(std::string_view utf8) noexcept {
    const std::wstring w = tools3000::core::WinUtils::utf8ToWstring(trimWhitespace(utf8));
    if (w.empty()) return false;
    const wchar_t c0 = w[0];
    // 常见项目符号与序号前缀：• · ◦ ▪ ▫ ● ○ ■ □ ◆ ◇ ▶ ▸ ①..⑳
    if (c0 == L'•' || c0 == L'·' || c0 == L'◦' || c0 == L'▪' || c0 == L'▫' ||
        c0 == L'●' || c0 == L'○' || c0 == L'■' || c0 == L'□' || c0 == L'◆' ||
        c0 == L'◇' || c0 == L'▶' || c0 == L'▸' || (c0 >= 0x2460 && c0 <= 0x2473)) {
        return true;
    }
    if ((c0 == L'-' || c0 == L'*' || c0 == L'+') && w.size() >= 2 && std::iswspace(w[1])) {
        return true;
    }
    // 有序列表前缀 "1. " / "1、" / "1) "
    size_t i = 0;
    while (i < w.size() && i < 3 && w[i] >= L'0' && w[i] <= L'9') ++i;
    if (i > 0 && i < w.size() && (w[i] == L'.' || w[i] == L'、' || w[i] == L')' || w[i] == L'）')) {
        if (i + 1 < w.size() && (std::iswspace(w[i + 1]) || w[i] == L'、' || w[i] == L'）')) {
            return true;
        }
    }
    return false;
}

bool endsWithSentenceStop(std::string_view utf8) noexcept {
    const std::wstring w = tools3000::core::WinUtils::utf8ToWstring(trimWhitespace(utf8));
    if (w.empty()) return false;
    const wchar_t last = w.back();
    return last == L'。' || last == L'！' || last == L'？' || last == L'：' ||
           last == L'.' || last == L'!' || last == L'?' || last == L':';
}

cv::Vec3b sampleLineBorderBgBgr(const cv::Mat& bgr, const cv::Rect& rc) noexcept {
    if (bgr.empty() || rc.width <= 0 || rc.height <= 0) return cv::Vec3b(0, 0, 0);
    std::vector<uint8_t> bs, gs, rs;
    bs.reserve(32);
    gs.reserve(32);
    rs.reserve(32);
    auto pushPx = [&](int x, int y) {
        if (x < 0 || x >= bgr.cols || y < 0 || y >= bgr.rows) return;
        const auto& p = bgr.at<cv::Vec3b>(y, x);
        bs.push_back(p[0]);
        gs.push_back(p[1]);
        rs.push_back(p[2]);
    };
    const int stepX = (std::max)(1, rc.width / 8);
    for (int x = rc.x; x < rc.x + rc.width; x += stepX) {
        pushPx(x, rc.y - 2);
        pushPx(x, rc.y + rc.height + 1);
    }
    pushPx(rc.x - 2, rc.y + rc.height / 2);
    pushPx(rc.x + rc.width + 1, rc.y + rc.height / 2);
    if (bs.empty()) return bgr.at<cv::Vec3b>(std::clamp(rc.y, 0, bgr.rows - 1), std::clamp(rc.x, 0, bgr.cols - 1));
    const size_t mid = bs.size() / 2;
    std::nth_element(bs.begin(), bs.begin() + static_cast<ptrdiff_t>(mid), bs.end());
    std::nth_element(gs.begin(), gs.begin() + static_cast<ptrdiff_t>(mid), gs.end());
    std::nth_element(rs.begin(), rs.begin() + static_cast<ptrdiff_t>(mid), rs.end());
    return cv::Vec3b(bs[mid], gs[mid], rs[mid]);
}

cv::Vec3b sampleLineCoreFgBgr(const cv::Mat& bgr, const cv::Rect& rc, const cv::Vec3b& bg) noexcept {
    if (bgr.empty() || rc.width <= 0 || rc.height <= 0) return bg;
    const cv::Rect clamped = rc & cv::Rect(0, 0, bgr.cols, bgr.rows);
    if (clamped.width <= 0 || clamped.height <= 0) return bg;

    struct InkSample {
        int dist;
        cv::Vec3b px;
    };
    std::vector<InkSample> samples;
    samples.reserve(96);
    const int stepX = (std::max)(1, clamped.width / 24);
    const int stepY = (std::max)(1, clamped.height / 4);
    for (int y = clamped.y; y < clamped.y + clamped.height; y += stepY) {
        const auto* row = bgr.ptr<cv::Vec3b>(y);
        for (int x = clamped.x; x < clamped.x + clamped.width; x += stepX) {
            const auto& p = row[x];
            const int d = std::abs(static_cast<int>(p[0]) - static_cast<int>(bg[0])) +
                          std::abs(static_cast<int>(p[1]) - static_cast<int>(bg[1])) +
                          std::abs(static_cast<int>(p[2]) - static_cast<int>(bg[2]));
            if (d > 24) {
                samples.push_back(InkSample{d, p});
            }
        }
    }
    if (samples.empty()) return bg;
    std::sort(samples.begin(), samples.end(), [](const InkSample& a, const InkSample& b) {
        return a.dist > b.dist;
    });
    const size_t take = (std::max<size_t>)(1u, (std::min<size_t>)(samples.size(), (samples.size() + 3) / 4));
    int sb = 0, sg = 0, sr = 0;
    for (size_t i = 0; i < take; ++i) {
        sb += samples[i].px[0];
        sg += samples[i].px[1];
        sr += samples[i].px[2];
    }
    return cv::Vec3b(
        static_cast<uint8_t>(sb / static_cast<int>(take)),
        static_cast<uint8_t>(sg / static_cast<int>(take)),
        static_cast<uint8_t>(sr / static_cast<int>(take)));
}

/// 0.05ms 视觉分隔墙检测：阻止跨背景色块、跨搜索框/表格横线、以及跨灰/黑信息层级的文字行误合并
bool hasVisualSeamBetween(
    const cv::Mat& sourceImage,
    const cv::Rect& upper,
    const cv::Rect& lower) noexcept {
    if (sourceImage.empty() || sourceImage.channels() < 3) return false;
    cv::Mat bgrView = sourceImage;
    cv::Mat bgrConverted;
    if (sourceImage.channels() == 4) {
        // 仅转换极小外包 ROI，绝不整图拷贝
        const cv::Rect roi = (upper | lower);
        const cv::Rect pad = cv::Rect(roi.x - 4, roi.y - 4, roi.width + 8, roi.height + 8) &
                             cv::Rect(0, 0, sourceImage.cols, sourceImage.rows);
        if (pad.width <= 2 || pad.height <= 2) return false;
        cv::cvtColor(sourceImage(pad), bgrConverted, cv::COLOR_BGRA2BGR);
        const cv::Rect relUp(upper.x - pad.x, upper.y - pad.y, upper.width, upper.height);
        const cv::Rect relLo(lower.x - pad.x, lower.y - pad.y, lower.width, lower.height);
        return hasVisualSeamBetween(bgrConverted, relUp, relLo);
    }

    const cv::Vec3b upBg = sampleLineBorderBgBgr(bgrView, upper);
    const cv::Vec3b loBg = sampleLineBorderBgBgr(bgrView, lower);
    const int bgDiff = std::abs(static_cast<int>(upBg[0]) - static_cast<int>(loBg[0])) +
                       std::abs(static_cast<int>(upBg[1]) - static_cast<int>(loBg[1])) +
                       std::abs(static_cast<int>(upBg[2]) - static_cast<int>(loBg[2]));
    if (bgDiff > 38) {
        return true; // 背景色块跳变（如 HN 橘色顶栏 vs 正文、浅蓝横幅 vs 白色搜索框）
    }

    // 水平分隔线 / 输入框边框扫描
    const int gapY0 = (std::max)(0, upper.y + upper.height);
    const int gapY1 = (std::min)(bgrView.rows - 1, lower.y);
    if (gapY1 > gapY0 + 1) {
        const int x0 = (std::max)(0, (std::min)(upper.x, lower.x));
        const int x1 = (std::min)(bgrView.cols, (std::max)(upper.x + upper.width, lower.x + lower.width));
        if (x1 - x0 >= 16) {
            const int step = (std::max)(1, (x1 - x0) / 36);
            for (int y = gapY0; y < gapY1; ++y) {
                const auto* row = bgrView.ptr<cv::Vec3b>(y);
                int samples = 0;
                int borderHits = 0;
                for (int x = x0; x < x1; x += step) {
                    ++samples;
                    const auto& p = row[x];
                    const int dUp = std::abs(static_cast<int>(p[0]) - static_cast<int>(upBg[0])) +
                                    std::abs(static_cast<int>(p[1]) - static_cast<int>(upBg[1])) +
                                    std::abs(static_cast<int>(p[2]) - static_cast<int>(upBg[2]));
                    const int dLo = std::abs(static_cast<int>(p[0]) - static_cast<int>(loBg[0])) +
                                    std::abs(static_cast<int>(p[1]) - static_cast<int>(loBg[1])) +
                                    std::abs(static_cast<int>(p[2]) - static_cast<int>(loBg[2]));
                    if (dUp > 36 && dLo > 36) {
                        ++borderHits;
                    }
                }
                if (samples > 0 && borderHits * 100 >= samples * 55) {
                    return true; // 命中输入框边框 / 水平分隔线
                }
            }
        }
    }

    // 前景字色与对比度跳变（如 HN 浅灰作者/时间 meta 行 vs 纯黑正文，AlternativeTo 浅灰小分类 vs 深黑标题）
    const cv::Vec3b upFg = sampleLineCoreFgBgr(bgrView, upper, upBg);
    const cv::Vec3b loFg = sampleLineCoreFgBgr(bgrView, lower, loBg);
    const int fgDiff = std::abs(static_cast<int>(upFg[0]) - static_cast<int>(loFg[0])) +
                       std::abs(static_cast<int>(upFg[1]) - static_cast<int>(loFg[1])) +
                       std::abs(static_cast<int>(upFg[2]) - static_cast<int>(loFg[2]));
    if (fgDiff > 82) {
        return true;
    }
    return false;
}

bool canMergeIntoParagraph(
    const TranslationParagraph& para,
    const PinOcrBlock& next,
    const cv::Mat& sourceImage) noexcept {
    if (para.lineRects.empty()) return false;
    if (para.layoutRegionId != next.layoutRegionId) return false;

    const cv::Rect& lastRect = para.lineRects.back();

    // 1. 同一对齐行的左右并列列绝不横向合并（保护表格列与横向导航栏）
    if (para.alignedRowId >= 0 && next.alignedRowId == para.alignedRowId) {
        const int horizOverlap = (std::min)(lastRect.x + lastRect.width, next.origBox.x + next.origBox.width) -
                                 (std::max)(lastRect.x, next.origBox.x);
        if (horizOverlap <= 0) {
            return false;
        }
    }

    // 2. 新列表项/新序号独立起段，绝不并入上一条目
    if (startsWithListBulletOrNumber(next.text)) {
        return false;
    }

    const float lastH = static_cast<float>((std::max)(1, lastRect.height));
    const float nextH = static_cast<float>((std::max)(1, next.origBox.height));
    const float hRatio = nextH / lastH;
    if (hRatio < 0.76f || hRatio > 1.32f) {
        return false; // 标题字号与正文字号差异大，独立成段
    }

    const int gapY = next.origBox.y - (lastRect.y + lastRect.height);
    const float avgH = 0.5f * (lastH + nextH);
    // 自然折行行距通常为 0.20x ~ 0.62x 行高；严格卡紧至 0.72x 杜绝将相邻独立段落/评论强行粘合
    if (gapY < -static_cast<int>(avgH * 0.25f) || gapY > static_cast<int>(avgH * 0.72f)) {
        return false;
    }

    // 3. 严守同左边缘对齐（或列表首行后的悬挂缩进续行），彻底杜绝 HN 跨嵌套缩进评论误合并
    const int xOverlap = (std::min)(para.unionRect.x + para.unionRect.width,
                                    next.origBox.x + next.origBox.width) -
                         (std::max)(para.unionRect.x, next.origBox.x);
    if (xOverlap <= 0) {
        return false;
    }
    const int leftDiffFromPara = next.origBox.x - para.unionRect.x;
    const int leftDiffFromLast = std::abs(next.origBox.x - lastRect.x);
    const bool bulletHangingIndent =
        startsWithListBulletOrNumber(para.originalText) &&
        leftDiffFromPara >= -4 &&
        static_cast<float>(leftDiffFromPara) <= (std::max)(avgH * 2.2f, 34.0f);
    const bool strictLeftAligned =
        static_cast<float>(std::abs(leftDiffFromPara)) <= (std::max)(avgH * 0.65f, 11.0f) ||
        static_cast<float>(leftDiffFromLast) <= (std::max)(avgH * 0.60f, 10.0f);
    if (!strictLeftAligned && !bulletHangingIndent) {
        return false;
    }

    // 4. 上一行明显提前收尾（短尾句+终止标点）或本身以终止标点结束且两行间距偏大，视为独立段落结束
    if (para.lineRects.size() >= 2 &&
        lastRect.width < static_cast<int>(para.unionRect.width * 0.72) &&
        endsWithSentenceStop(para.originalText)) {
        return false;
    }
    if (para.lineRects.size() == 1 &&
        endsWithSentenceStop(para.originalText) &&
        (lastRect.width < static_cast<int>(next.origBox.width * 0.78) || gapY > static_cast<int>(avgH * 0.48f))) {
        return false;
    }

    // 5. 短 UI 菜单/表格单元格标签（如“安全配置”、“在线监控”、“reply”、“是”）上下排列保持独立，绝不误合并
    const size_t avgLineVisualLen = countVisualChars(para.originalText) / (std::max<size_t>)(1u, para.lineRects.size());
    const size_t nextVisualLen = countVisualChars(next.text);
    const bool bothShortUiItems =
        (avgLineVisualLen <= 8 && nextVisualLen <= 8) &&
        (lastRect.width <= 165 && next.origBox.width <= 165);
    if (bothShortUiItems) {
        return false;
    }

    // 较宽松行距的短菜单项或独立短按钮（如 HN "reply"）保持独立
    if (avgLineVisualLen <= 10 && nextVisualLen <= 10 && gapY > static_cast<int>(avgH * 0.38f)) {
        return false;
    }
    if (nextVisualLen <= 6 && next.origBox.width <= 72 && gapY > static_cast<int>(avgH * 0.28f)) {
        return false;
    }

    // 6. 视觉分隔墙检测（背景色跳变 / 输入框与卡片边框线 / 灰黑字色层级跳变）
    if (hasVisualSeamBetween(sourceImage, lastRect, next.origBox)) {
        return false;
    }

    return true;
}

void appendLineTextToParagraph(TranslationParagraph& para, const std::string& nextLine) {
    const std::string cleanNext = trimWhitespace(nextLine);
    if (cleanNext.empty()) return;
    if (para.originalText.empty()) {
        para.originalText = cleanNext;
        return;
    }

    // 英文行末连字符断词自动合并
    if (para.originalText.size() >= 2 &&
        para.originalText.back() == '-' &&
        std::isalpha(static_cast<unsigned char>(para.originalText[para.originalText.size() - 2])) &&
        !cleanNext.empty() &&
        std::isalpha(static_cast<unsigned char>(cleanNext.front()))) {
        para.originalText.pop_back();
        para.originalText += cleanNext;
        return;
    }

    if (endsWithCjkOrFullwidth(para.originalText) || startsWithCjk(cleanNext)) {
        para.originalText += cleanNext;
    } else {
        para.originalText += ' ';
        para.originalText += cleanNext;
    }
}

float relativeLuminance(const cv::Vec3b& bgr) noexcept {
    return (0.114f * static_cast<float>(bgr[0]) +
            0.587f * static_cast<float>(bgr[1]) +
            0.299f * static_cast<float>(bgr[2])) / 255.0f;
}

cv::Vec3b ensureReadableTextBgr(const cv::Vec3b& textBgr, const cv::Vec3b& bgBgr, bool darkBg) noexcept {
    const float lText = relativeLuminance(textBgr);
    const float lBg = relativeLuminance(bgBgr);
    const float contrast = std::abs(lText - lBg);
    if (contrast >= 0.32f) {
        return textBgr;
    }
    // 对比度偏弱时平滑校准至高可读色，同时保留原色系气质
    return darkBg ? cv::Vec3b(245, 241, 236) : cv::Vec3b(32, 24, 18);
}

const wchar_t* resolveTranslateFontFamily(
    IDWriteFactory* factory,
    TextReplaceFontClass fontClass,
    std::string_view targetLangCode) noexcept {
    if (fontClass == TextReplaceFontClass::Monospace) {
        return L"Cascadia Mono";
    }
    if (fontClass == TextReplaceFontClass::Serif) {
        if (targetLangCode == "zh-CN" || targetLangCode == "zh-TW") {
            return L"SimSun";
        }
        return L"Georgia";
    }
    if (targetLangCode == "ja") return L"Yu Gothic UI";
    if (targetLangCode == "ko") return L"Malgun Gothic";
    return tools3000::core::resolveInstalledUiFontFamily(factory);
}

const wchar_t* resolveDWriteLocale(std::string_view targetLangCode) noexcept {
    if (targetLangCode == "zh-TW") return L"zh-TW";
    if (targetLangCode == "en") return L"en-US";
    if (targetLangCode == "ja") return L"ja-JP";
    if (targetLangCode == "ko") return L"ko-KR";
    if (targetLangCode == "ru") return L"ru-RU";
    if (targetLangCode == "pt-BR") return L"pt-BR";
    if (targetLangCode == "es") return L"es-ES";
    return L"zh-CN";
}

int pixelDiffFromPlane(const cv::Vec3b& px, const LinearBgPlane& plane, int x, int y) noexcept {
    const cv::Vec3b bg = plane.evalAt(static_cast<float>(x), static_cast<float>(y));
    return std::abs(static_cast<int>(px[0]) - static_cast<int>(bg[0])) +
           std::abs(static_cast<int>(px[1]) - static_cast<int>(bg[1])) +
           std::abs(static_cast<int>(px[2]) - static_cast<int>(bg[2]));
}

struct CleanBoundProbe {
    int bound = 0;
    bool hitBoundary = false;
};

struct SafeLayoutBox {
    cv::Rect rect;
    bool centerHoriz = false;
};

/// 0.05ms 像素级向右洁净空白探测：遇到垂直表格线、按钮边框、图标轮廓或相邻文字立刻刹车
CleanBoundProbe probeCleanRightBound(
    const cv::Mat& originalBgr,
    const cv::Rect& baseRect,
    int limitRight,
    const ExtractedTextStyle& style) noexcept {
    if (originalBgr.empty() || style.complexBackground) {
        return {(std::min)(limitRight, baseRect.x + baseRect.width + 12), false};
    }
    const int startX = baseRect.x + baseRect.width + 2;
    const int endX = (std::min)(originalBgr.cols - 2, limitRight);
    if (startX >= endX) return {baseRect.x + baseRect.width, false};

    const int y0 = (std::max)(0, baseRect.y + (std::max)(1, baseRect.height / 6));
    const int y1 = (std::min)(originalBgr.rows, baseRect.y + baseRect.height - (std::max)(1, baseRect.height / 6));
    if (y1 <= y0) return {baseRect.x + baseRect.width, false};
    const int bandH = y1 - y0;

    int lastCleanX = baseRect.x + baseRect.width;
    for (int x = startX; x < endX; ++x) {
        int diffCount = 0;
        for (int y = y0; y < y1; ++y) {
            if (pixelDiffFromPlane(originalBgr.at<cv::Vec3b>(y, x), style.bgPlane, x, y) > 66) {
                ++diffCount;
            }
        }
        // 单列中超过 16% 像素偏离背景色（或超过 3px 的连续垂直线/图标笔画），视为碰到边界/图标
        const bool colDirty = (diffCount * 6 >= bandH) || (diffCount >= 3 && bandH <= 18);
        if (colDirty) {
            return {(std::max)(baseRect.x + baseRect.width, x - kMinObstacleMarginPx), true};
        }
        lastCleanX = x;
    }
    return {lastCleanX, false};
}

/// 0.03ms 像素级向下方洁净空白探测：遇到水平表格线、分隔线或下方图标/文字立即刹车
int probeCleanBottomBound(
    const cv::Mat& originalBgr,
    const cv::Rect& baseRect,
    int limitBottom,
    const ExtractedTextStyle& style) noexcept {
    if (originalBgr.empty() || style.complexBackground) {
        return (std::min)(limitBottom, baseRect.y + baseRect.height + 2);
    }
    const int startY = baseRect.y + baseRect.height + 1;
    const int endY = (std::min)(originalBgr.rows - 2, limitBottom);
    if (startY >= endY) return baseRect.y + baseRect.height;

    const int x0 = (std::max)(0, baseRect.x + 1);
    const int x1 = (std::min)(originalBgr.cols, baseRect.x + baseRect.width - 1);
    if (x1 <= x0) return baseRect.y + baseRect.height;
    const int spanW = x1 - x0;
    const int step = (std::max)(1, spanW / 32);

    int lastCleanY = baseRect.y + baseRect.height;
    for (int y = startY; y < endY; ++y) {
        const auto* row = originalBgr.ptr<cv::Vec3b>(y);
        int samples = 0;
        int diffCount = 0;
        for (int x = x0; x < x1; x += step) {
            ++samples;
            if (pixelDiffFromPlane(row[x], style.bgPlane, x, y) > 60) {
                ++diffCount;
            }
        }
        if (samples > 0 && diffCount * 5 >= samples) {
            return (std::max)(baseRect.y + baseRect.height, y - 2);
        }
        lastCleanY = y;
    }
    return lastCleanY;
}

/// 0.03ms 像素级向左洁净空白探测（用于检测居中按钮/居中表格单元格的真实左侧边界）
CleanBoundProbe probeCleanLeftBound(
    const cv::Mat& originalBgr,
    const cv::Rect& baseRect,
    int limitLeft,
    const ExtractedTextStyle& style) noexcept {
    if (originalBgr.empty() || style.complexBackground) return {baseRect.x, false};
    const int startX = baseRect.x - 2;
    const int endX = (std::max)(1, limitLeft);
    if (startX <= endX) return {baseRect.x, false};

    const int y0 = (std::max)(0, baseRect.y + (std::max)(1, baseRect.height / 6));
    const int y1 = (std::min)(originalBgr.rows, baseRect.y + baseRect.height - (std::max)(1, baseRect.height / 6));
    if (y1 <= y0) return {baseRect.x, false};
    const int bandH = y1 - y0;

    int lastCleanX = baseRect.x;
    for (int x = startX; x >= endX; --x) {
        int diffCount = 0;
        for (int y = y0; y < y1; ++y) {
            if (pixelDiffFromPlane(originalBgr.at<cv::Vec3b>(y, x), style.bgPlane, x, y) > 66) {
                ++diffCount;
            }
        }
        if ((diffCount * 6 >= bandH) || (diffCount >= 3 && bandH <= 18)) {
            return {(std::min)(baseRect.x, x + kMinObstacleMarginPx), true};
        }
        lastCleanX = x;
    }
    return {lastCleanX, false};
}

SafeLayoutBox computeSafeLayoutRect(
    const cv::Mat& originalBgr,
    const TranslationParagraph& selfPara,
    size_t selfIdx,
    const std::vector<TranslationParagraph>& allParagraphs,
    const std::vector<cv::Rect>& extraObstacles,
    const ExtractedTextStyle& style,
    const cv::Size& imageSize) noexcept {
    const cv::Rect& baseRect = selfPara.unionRect;
    // 多行正文严守原版面栏宽（仅允许微扩 24px 呼吸留白，防跨栏侵占右侧留白）；单行标签保留最大 128px 避障扩展上限
    const int rightExpandCap = selfPara.isSingleLine
        ? kMaxSafeExpandRightPx
        : (std::min)(28, (std::max)(10, baseRect.width / 10));
    int maxRight = (std::min)(imageSize.width - 2, baseRect.x + baseRect.width + rightExpandCap);
    bool rightObstacleHit = (baseRect.x + baseRect.width + rightExpandCap >= imageSize.width - 2);

    const int bottomCap = selfPara.isSingleLine
        ? (baseRect.height >= 26 ? (std::min)(baseRect.height, 26) : kMaxSafeExpandBottomSingleLinePx)
        : kMaxSafeExpandBottomMultiLinePx;
    int maxBottom = (std::min)(imageSize.height - 2, baseRect.y + baseRect.height + bottomCap);

    // 左侧探测上限仅用于对称居中判定（绝不直接用来移动左对齐正文起始 X！）
    const int leftProbeSpan = selfPara.isSingleLine ? (std::min)(128, (std::max)(36, baseRect.width)) : 8;
    int minLeft = (std::max)(2, baseRect.x - leftProbeSpan);
    bool leftObstacleHit = (baseRect.x - leftProbeSpan <= 2);

    auto applyObstacle = [&](const cv::Rect& other) noexcept {
        if (other.width <= 0 || other.height <= 0) return;
        const cv::Rect inter = baseRect & other;
        if (inter.area() > 0 && inter.area() * 2 >= (std::min)(baseRect.area(), other.area())) {
            return;
        }
        const bool verticalOverlap =
            (other.y < baseRect.y + baseRect.height + 2) &&
            (other.y + other.height > baseRect.y - 2);
        if (verticalOverlap && other.x >= baseRect.x + baseRect.width - 2) {
            const int candidateRight = other.x - kMinObstacleMarginPx;
            if (candidateRight < maxRight) {
                maxRight = candidateRight;
                rightObstacleHit = true;
            }
        }
        if (verticalOverlap && other.x + other.width <= baseRect.x + 2) {
            const int candidateLeft = other.x + other.width + kMinObstacleMarginPx;
            if (candidateLeft > minLeft) {
                minLeft = candidateLeft;
                leftObstacleHit = true;
            }
        }
        const bool horizOverlap =
            (other.x < maxRight) &&
            (other.x + other.width > baseRect.x);
        if (horizOverlap && other.y >= baseRect.y + baseRect.height - 2) {
            maxBottom = (std::min)(maxBottom, other.y - kMinObstacleMarginPx);
        }
    };

    for (size_t i = 0; i < allParagraphs.size(); ++i) {
        if (i == selfIdx) continue;
        applyObstacle(allParagraphs[i].unionRect);
    }
    for (const cv::Rect& obs : extraObstacles) {
        applyObstacle(obs);
    }

    // 像素级边界刹车（表格线 / 按钮框 / 图标 / 二维码边缘）
    const CleanBoundProbe rightProbe = probeCleanRightBound(originalBgr, baseRect, maxRight, style);
    maxRight = rightProbe.bound;
    const bool rightBounded = rightProbe.hitBoundary || rightObstacleHit;
    maxBottom = probeCleanBottomBound(originalBgr, baseRect, maxBottom, style);

    // 精准对齐判定（彻底消除 HN 缩进评论与网页左对齐卡片的向右漂移）：
    // 仅在以下两种高置信度居中场景启用居中与对称扩展，其余 99% 严守 baseRect.x 左对齐！
    int finalX = baseRect.x;
    int finalRight = (std::max)(baseRect.x + baseRect.width, maxRight);
    bool isCentered = false;

    if (selfPara.isSingleLine && !style.complexBackground) {
        const CleanBoundProbe leftProbe = probeCleanLeftBound(originalBgr, baseRect, minLeft, style);
        const bool leftBounded = leftProbe.hitBoundary || leftObstacleHit;
        const int leftGap = baseRect.x - leftProbe.bound;
        const int rightGap = maxRight - (baseRect.x + baseRect.width);

        // 场景 A：居中按钮 / 居中表格单元格（左右双侧均撞到真实视觉边框或障碍，且留白对称）
        const bool boundedSymmetricCenter =
            leftBounded && rightBounded &&
            leftGap >= 6 && rightGap >= 6 &&
            std::abs(leftGap - rightGap) <= (std::max)(14, (leftGap + rightGap) / 3);

        // 场景 B：整图水平居中主标语（中心点贴近画布中轴 ±4%，且左右均有宽裕对称留白）
        const float selfCenterX = static_cast<float>(baseRect.x) + static_cast<float>(baseRect.width) * 0.5f;
        const float canvasCenterX = static_cast<float>(imageSize.width) * 0.5f;
        const bool canvasAxisCenter =
            (baseRect.width >= 100) &&
            (std::abs(selfCenterX - canvasCenterX) <= static_cast<float>(imageSize.width) * 0.04f) &&
            leftGap >= 36 && rightGap >= 36 &&
            std::abs(leftGap - rightGap) <= (std::max)(24, (leftGap + rightGap) / 3);

        if (boundedSymmetricCenter || canvasAxisCenter) {
            const int symMargin = (std::min)(leftGap, rightGap);
            finalX = baseRect.x - symMargin;
            finalRight = baseRect.x + baseRect.width + symMargin;
            isCentered = true;
        }
    }

    const int finalW = (std::max)(baseRect.width, finalRight - finalX);
    const int finalH = (std::max)(baseRect.height, maxBottom - baseRect.y);
    const cv::Rect clamped = cv::Rect(finalX, baseRect.y, finalW, finalH) & cv::Rect(0, 0, imageSize.width, imageSize.height);
    return SafeLayoutBox{clamped, isCentered};
}

void eraseParagraphBackground(
    cv::Mat& bgrCanvas,
    const TranslationParagraph& para,
    const ExtractedTextStyle& style) {
    const cv::Rect imgBounds(0, 0, bgrCanvas.cols, bgrCanvas.rows);
    if (!style.complexBackground) {
        // 多行正文段在整段包络区微扩 3px 擦净行内代码徽章/标签圆角灰边残影；
        // 单行单元格/按钮保持紧贴行框（padX=2, padY=1），保护紧邻表格横线与外围图标。
        if (!para.isSingleLine && para.lineRects.size() >= 2) {
            const cv::Rect roi = cv::Rect(
                para.unionRect.x - 3, para.unionRect.y - 3,
                para.unionRect.width + 6, para.unionRect.height + 6) & imgBounds;
            for (int y = roi.y; y < roi.y + roi.height; ++y) {
                auto* rowPtr = bgrCanvas.ptr<cv::Vec3b>(y);
                for (int x = roi.x; x < roi.x + roi.width; ++x) {
                    rowPtr[x] = style.bgPlane.evalAt(static_cast<float>(x), static_cast<float>(y));
                }
            }
            return;
        }
        for (const cv::Rect& rawLine : para.lineRects) {
            const cv::Rect expanded(rawLine.x - 2, rawLine.y - 1, rawLine.width + 4, rawLine.height + 2);
            const cv::Rect roi = expanded & imgBounds;
            if (roi.width <= 0 || roi.height <= 0) continue;
            for (int y = roi.y; y < roi.y + roi.height; ++y) {
                auto* rowPtr = bgrCanvas.ptr<cv::Vec3b>(y);
                for (int x = roi.x; x < roi.x + roi.width; ++x) {
                    rowPtr[x] = style.bgPlane.evalAt(static_cast<float>(x), static_cast<float>(y));
                }
            }
        }
        return;
    }

    // 复杂纹理/照片背景：使用 0 显存 0.8ms 局部紧凑 Telea 无痕修复（绝不拉起 DirectML MI-GAN 避免主进程暴涨 ~800MB 内存）
    const cv::Rect padded = cv::Rect(
        para.unionRect.x - 10, para.unionRect.y - 10,
        para.unionRect.width + 20, para.unionRect.height + 20) & imgBounds;
    if (padded.width <= 2 || padded.height <= 2) return;

    cv::Mat mask = cv::Mat::zeros(padded.size(), CV_8UC1);
    for (const cv::Rect& rawLine : para.lineRects) {
        const cv::Rect rel = (cv::Rect(rawLine.x - 2, rawLine.y - 1, rawLine.width + 4, rawLine.height + 2) & padded);
        if (rel.width <= 0 || rel.height <= 0) continue;
        cv::rectangle(
            mask,
            cv::Rect(rel.x - padded.x, rel.y - padded.y, rel.width, rel.height),
            cv::Scalar(255),
            cv::FILLED);
    }

    cv::Mat repaired;
    cv::inpaint(bgrCanvas(padded), mask, repaired, 3.0, cv::INPAINT_TELEA);
    if (!repaired.empty() && repaired.size() == padded.size()) {
        repaired.copyTo(bgrCanvas(padded), mask);
    }
}

struct FittedLayoutResult {
    ComPtr<IDWriteTextLayout> layout;
    float fontPx = 14.0f;
    float textWidth = 0.0f;
    float textHeight = 0.0f;
    bool overflowed = false;
    bool wrapped = false;
};

/// 零字体变形排版内核（Typography Purity Law）：
/// 严禁任何非等比水平拉伸/压缩，恒定 1:1 原生字形比例 (DWRITE_FONT_STRETCH_NORMAL)；
/// 仅通过等比字号二分搜索 + 自然换行与单行择优来优雅适配安全避障域。
FittedLayoutResult fitParagraphTextLayout(
    IDWriteFactory* dwrite,
    const std::wstring& text,
    const wchar_t* fontFamily,
    const wchar_t* localeName,
    DWRITE_FONT_WEIGHT weight,
    float preferredFontPx,
    float maxWidth,
    float maxHeight,
    bool singleLine,
    bool centerHorizontally) {
    const float safeMaxW = (std::max)(18.0f, maxWidth);
    const float safeMaxH = (std::max)(11.0f, maxHeight);
    const float hiInit = (std::max)(kMinReadableFontPx, (std::min)(preferredFontPx * kMaxFontScaleFactor, safeMaxH * 0.94f));
    const float loInit = (std::max)(kMinReadableFontPx, (std::min)(preferredFontPx * kMinFontScaleFactor, hiInit));

    auto createLayout = [&](float fontPx, bool allowWrap, bool attachEllipsis) -> FittedLayoutResult {
        FittedLayoutResult r{};
        r.fontPx = fontPx;
        r.wrapped = allowWrap;
        ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(dwrite->CreateTextFormat(
                fontFamily, nullptr, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                fontPx, localeName, fmt.GetAddressOf())) || !fmt) {
            return r;
        }
        fmt->SetWordWrapping(allowWrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        fmt->SetTextAlignment(centerHorizontally ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (allowWrap) {
            fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, 1.24f, 0.86f);
        }
        if (attachEllipsis) {
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(fmt.Get(), ellipsis.GetAddressOf())) && ellipsis) {
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                fmt->SetTrimming(&trim, ellipsis.Get());
            }
        }

        ComPtr<IDWriteTextLayout> layout;
        const float boxH = allowWrap ? (attachEllipsis ? safeMaxH : safeMaxH * 3.0f) : safeMaxH * 2.0f;
        if (FAILED(dwrite->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()),
                fmt.Get(), safeMaxW, boxH, layout.GetAddressOf())) || !layout) {
            return r;
        }
        DWRITE_TEXT_METRICS m{};
        if (FAILED(layout->GetMetrics(&m))) return r;

        r.layout = std::move(layout);
        r.textWidth = m.widthIncludingTrailingWhitespace;
        r.textHeight = m.height;
        r.overflowed = (m.height > safeMaxH + 1.0f) || (m.widthIncludingTrailingWhitespace > safeMaxW + 1.0f);
        return r;
    };

    auto searchBestFont = [&](bool allowWrap) -> FittedLayoutResult {
        const float startHi = std::clamp(preferredFontPx, loInit, hiInit);
        FittedLayoutResult first = createLayout(startHi, allowWrap, false);
        if (first.layout && !first.overflowed) {
            return first;
        }
        float lo = loInit;
        float hi = startHi;
        FittedLayoutResult bestFit{};
        for (int iter = 0; iter < 7; ++iter) {
            const float mid = 0.5f * (lo + hi);
            FittedLayoutResult cur = createLayout(mid, allowWrap, false);
            if (cur.layout && !cur.overflowed) {
                bestFit = std::move(cur);
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return bestFit;
    };

    // 多行自然段：统一等比字号自然换行排版
    FittedLayoutResult bestWrap = searchBestFont(/*allowWrap=*/true);
    if (!singleLine && bestWrap.layout) {
        return bestWrap;
    }

    // 单行 UI 标签/表格单元格：在 1:1 零变形前提下，对比“自然单行等比缩放”与“不溢出多行折行”，择字号更舒展者
    FittedLayoutResult bestSingle = searchBestFont(/*allowWrap=*/false);
    if (bestWrap.layout && bestSingle.layout) {
        if (bestSingle.fontPx >= bestWrap.fontPx * 0.88f) {
            return bestSingle;
        }
        return bestWrap;
    }
    if (bestSingle.layout) return bestSingle;
    if (bestWrap.layout) return bestWrap;

    // 极端超长兜底：保持 1:1 原生字形不变形，采用最小可读等比字号 + 省略号优雅裁切，绝不压盖邻行
    const bool fallbackWrap = !singleLine || (safeMaxH >= kMinReadableFontPx * 2.35f);
    FittedLayoutResult fallback = createLayout(loInit, fallbackWrap, true);
    fallback.overflowed = true;
    return fallback;
}

bool isCjkTargetLanguage(std::string_view targetLangCode) noexcept {
    return targetLangCode == "zh-CN" || targetLangCode == "zh-TW" ||
           targetLangCode == "ja" || targetLangCode == "ko";
}

DWRITE_FONT_WEIGHT resolveRenderFontWeight(
    int extractedWeight,
    float fontPx,
    std::string_view targetLangCode) noexcept {
    // 防止 11~14px 细英文因次像素抗锯齿墨水占比偏高被误判为刺眼粗黑体：
    // 仅对大标题或高置信度加粗使用 BOLD；常规 CJK 正文恒定清晰锐利的 MEDIUM(500) / REGULAR(400)
    if (extractedWeight >= 700 && fontPx >= 16.0f) {
        return DWRITE_FONT_WEIGHT_BOLD;
    }
    if (extractedWeight >= 650 && fontPx >= 15.0f) {
        return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    }
    if (isCjkTargetLanguage(targetLangCode)) {
        return (extractedWeight >= 650 && fontPx >= 13.5f)
            ? DWRITE_FONT_WEIGHT_SEMI_BOLD
            : DWRITE_FONT_WEIGHT_MEDIUM;
    }
    return (extractedWeight >= 650) ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR;
}

cv::Rect renderParagraphOntoBgr(
    cv::Mat& bgrCanvas,
    const TranslationParagraph& para,
    const SafeLayoutBox& layoutBox,
    const ExtractedTextStyle& style,
    std::string_view targetLangCode,
    float harmonizedPreferredPx = 0.0f) {
    auto& fac = threadFactories();
    if (!fac.init()) return {};

    const std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(trimWhitespace(para.translatedText));
    if (wText.empty()) return {};

    const cv::Rect& layoutRect = layoutBox.rect;
    const wchar_t* fontFamily = resolveTranslateFontFamily(fac.dwrite.Get(), style.fontClass, targetLangCode);
    const wchar_t* localeName = resolveDWriteLocale(targetLangCode);

    float baseFontPx = std::clamp(style.fontSizePx, 10.0f, 72.0f);
    if (harmonizedPreferredPx >= kMinReadableFontPx) {
        baseFontPx = harmonizedPreferredPx;
    }
    const DWRITE_FONT_WEIGHT weight = resolveRenderFontWeight(style.fontWeight, baseFontPx, targetLangCode);
    FittedLayoutResult fitted = fitParagraphTextLayout(
        fac.dwrite.Get(), wText, fontFamily, localeName, weight, baseFontPx,
        static_cast<float>(layoutRect.width), static_cast<float>(layoutRect.height),
        para.isSingleLine, layoutBox.centerHoriz);
    if (!fitted.layout) return {};

    const cv::Rect imgBounds(0, 0, bgrCanvas.cols, bgrCanvas.rows);
    const cv::Rect renderRoi = layoutRect & imgBounds;
    if (renderRoi.width <= 1 || renderRoi.height <= 1) return {};

    ComPtr<IWICBitmap> wicBmp;
    if (FAILED(fac.wic->CreateBitmap(
            static_cast<UINT>(renderRoi.width), static_cast<UINT>(renderRoi.height),
            GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, wicBmp.GetAddressOf())) || !wicBmp) {
        return {};
    }

    const D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(fac.d2d->CreateWicBitmapRenderTarget(wicBmp.Get(), rtProps, rt.GetAddressOf())) || !rt) {
        return {};
    }

    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const cv::Vec3b centerBg = style.bgPlane.evalAt(
        static_cast<float>(layoutRect.x + layoutRect.width * 0.5f),
        static_cast<float>(layoutRect.y + layoutRect.height * 0.5f));
    const cv::Vec3b safeTextBgr = ensureReadableTextBgr(style.textBgr, centerBg, style.darkBackground);

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    rt->PushAxisAlignedClip(
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(renderRoi.width), static_cast<float>(renderRoi.height)),
        D2D1_ANTIALIAS_MODE_ALIASED);

    if (style.complexBackground) {
        ComPtr<ID2D1SolidColorBrush> bgBrush;
        rt->CreateSolidColorBrush(
            D2D1::ColorF(
                static_cast<float>(centerBg[2]) / 255.0f,
                static_cast<float>(centerBg[1]) / 255.0f,
                static_cast<float>(centerBg[0]) / 255.0f,
                0.78f),
            bgBrush.GetAddressOf());
        if (bgBrush) {
            const float r = (std::min)(4.0f, static_cast<float>((std::min)(renderRoi.width, renderRoi.height)) * 0.2f);
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                D2D1::RectF(0.5f, 0.5f, static_cast<float>(renderRoi.width) - 0.5f,
                            static_cast<float>(renderRoi.height) - 0.5f),
                r, r);
            rt->FillRoundedRectangle(rr, bgBrush.Get());
        }
    }

    ComPtr<ID2D1SolidColorBrush> textBrush;
    rt->CreateSolidColorBrush(
        D2D1::ColorF(
            static_cast<float>(safeTextBgr[2]) / 255.0f,
            static_cast<float>(safeTextBgr[1]) / 255.0f,
            static_cast<float>(safeTextBgr[0]) / 255.0f,
            1.0f),
        textBrush.GetAddressOf());

    if (textBrush) {
        float offsetY = 0.0f;
        if (fitted.textHeight < static_cast<float>(renderRoi.height)) {
            offsetY = para.isSingleLine
                ? (static_cast<float>(renderRoi.height) - fitted.textHeight) * 0.5f
                : (std::min)(4.0f, (static_cast<float>(renderRoi.height) - fitted.textHeight) * 0.25f);
        }
        rt->DrawTextLayout(
            D2D1::Point2F(0.0f, offsetY),
            fitted.layout.Get(),
            textBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }

    rt->PopAxisAlignedClip();
    if (FAILED(rt->EndDraw())) return {};

    // 将 Premultiplied BGRA 精准 Alpha 混合写回 bgrCanvas
    WICRect lockRc{0, 0, renderRoi.width, renderRoi.height};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(wicBmp->Lock(&lockRc, WICBitmapLockRead, lock.GetAddressOf())) || !lock) return {};

    UINT bufSize = 0;
    UINT stride = 0;
    BYTE* srcData = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&bufSize, &srcData)) || !srcData) {
        return {};
    }

    for (int y = 0; y < renderRoi.height; ++y) {
        const BYTE* sRow = srcData + static_cast<size_t>(y) * stride;
        auto* dRow = bgrCanvas.ptr<cv::Vec3b>(renderRoi.y + y) + renderRoi.x;
        for (int x = 0; x < renderRoi.width; ++x) {
            const uint8_t sa = sRow[x * 4 + 3];
            if (sa == 0) continue;
            if (sa == 255) {
                dRow[x][0] = sRow[x * 4 + 0];
                dRow[x][1] = sRow[x * 4 + 1];
                dRow[x][2] = sRow[x * 4 + 2];
            } else {
                const uint32_t invA = 255u - sa;
                dRow[x][0] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sRow[x * 4 + 0] + (dRow[x][0] * invA + 127u) / 255u));
                dRow[x][1] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sRow[x * 4 + 1] + (dRow[x][1] * invA + 127u) / 255u));
                dRow[x][2] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sRow[x * 4 + 2] + (dRow[x][2] * invA + 127u) / 255u));
            }
        }
    }
    return renderRoi;
}

std::string normalizeForEqualityCompare(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    bool prevSpace = false;
    for (unsigned char c : sv) {
        if (std::isspace(c)) {
            if (!out.empty() && !prevSpace) {
                out.push_back(' ');
                prevSpace = true;
            }
        } else {
            out.push_back(static_cast<char>(std::tolower(c)));
            prevSpace = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool isTranslationEffectivelyChanged(std::string_view original, std::string_view translated) noexcept {
    const std::string t = trimWhitespace(translated);
    if (t.empty()) return false;
    return normalizeForEqualityCompare(original) != normalizeForEqualityCompare(t);
}

} // namespace

bool InPlaceTranslateEngine::isTranslatableText(std::string_view utf8Text) noexcept {
    const std::string trimmed = trimWhitespace(utf8Text);
    if (trimmed.empty()) return false;
    if (looksLikeUrlOrFilePath(trimmed)) return false;
    if (looksLikeIdentifierOrSerialCode(trimmed)) return false;
    if (looksLikeCodeOrTechToken(trimmed)) return false;

    const std::wstring w = tools3000::core::WinUtils::utf8ToWstring(trimmed);
    size_t alphaOrCjk = 0;
    size_t digits = 0;
    for (wchar_t ch : w) {
        const uint32_t cp = static_cast<uint32_t>(ch);
        if (isCjkWideChar(ch) ||
            (cp >= 0x0400 && cp <= 0x04FF) ||
            (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
            (cp >= 0x00C0 && cp <= 0x024F)) {
            ++alphaOrCjk;
        } else if (cp >= '0' && cp <= '9') {
            ++digits;
        }
    }
    if (alphaOrCjk == 0) return false;
    // 跳过如 "12px" / "4.8MB" / "v1.3" 等纯度量单位或版本号短标签
    if (digits > 0 && alphaOrCjk <= 2 && w.size() <= 8) return false;
    return true;
}

std::vector<TranslationParagraph> InPlaceTranslateEngine::buildTranslationParagraphs(
    const std::vector<PinOcrBlock>& blocks,
    const cv::Size& imageSize,
    const std::vector<size_t>& selectedBlockIndices,
    const cv::Mat& sourceImage) {
    std::vector<TranslationParagraph> result;
    if (blocks.empty() || imageSize.width <= 0 || imageSize.height <= 0) return result;

    const cv::Rect imgBounds(0, 0, imageSize.width, imageSize.height);
    std::unordered_set<size_t> filterSet(selectedBlockIndices.begin(), selectedBlockIndices.end());
    const bool useFilter = !filterSet.empty();

    std::vector<size_t> activeIndices;
    activeIndices.reserve(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (useFilter && !filterSet.contains(i)) continue;
        if (!isTranslatableText(blocks[i].text)) continue;
        const cv::Rect clamped = blocks[i].origBox & imgBounds;
        if (clamped.width <= 1 || clamped.height <= 1) continue;
        activeIndices.push_back(i);
    }
    if (activeIndices.empty()) return result;

    // 按 layoutRegionId -> Y -> X 阅读顺序稳定排序
    std::stable_sort(activeIndices.begin(), activeIndices.end(), [&](size_t a, size_t b) {
        const auto& ba = blocks[a];
        const auto& bb = blocks[b];
        if (ba.layoutRegionId != bb.layoutRegionId) return ba.layoutRegionId < bb.layoutRegionId;
        const int yTol = (std::max)(4, (std::min)(ba.origBox.height, bb.origBox.height) / 2);
        if (std::abs(ba.origBox.y - bb.origBox.y) > yTol) return ba.origBox.y < bb.origBox.y;
        return ba.origBox.x < bb.origBox.x;
    });

    for (size_t idx : activeIndices) {
        const auto& blk = blocks[idx];
        const cv::Rect clampedBox = blk.origBox & imgBounds;
        if (!result.empty() && canMergeIntoParagraph(result.back(), blk, sourceImage)) {
            auto& cur = result.back();
            cur.unionRect |= clampedBox;
            cur.lineRects.push_back(clampedBox);
            cur.charBoxes.insert(cur.charBoxes.end(), blk.charBoxes.begin(), blk.charBoxes.end());
            cur.isSingleLine = false;
            appendLineTextToParagraph(cur, blk.text);
        } else {
            TranslationParagraph para{};
            para.unionRect = clampedBox;
            para.lineRects.push_back(clampedBox);
            para.charBoxes = blk.charBoxes;
            para.layoutRegionId = blk.layoutRegionId;
            para.alignedRowId = blk.alignedRowId;
            para.isSingleLine = true;
            para.originalText = trimWhitespace(blk.text);
            result.push_back(std::move(para));
        }
    }
    return result;
}

std::string InPlaceTranslateEngine::joinTranslatedTexts(
    const std::vector<TranslationParagraph>& paragraphs) {
    std::string out;
    for (const auto& p : paragraphs) {
        const std::string t = trimWhitespace(p.translatedText);
        if (t.empty()) continue;
        if (!out.empty()) out += "\r\n";
        out += t;
    }
    return out;
}

InPlaceTranslateRenderResult InPlaceTranslateEngine::renderTranslatedCanvas(
    const cv::Mat& cleanCanvas,
    std::vector<TranslationParagraph> paragraphs,
    std::string_view targetLangCode,
    const std::vector<cv::Rect>& extraObstacleRects) {
    InPlaceTranslateRenderResult res{};
    if (cleanCanvas.empty() || (cleanCanvas.channels() != 3 && cleanCanvas.channels() != 4)) {
        return res;
    }

    cv::Mat bgrCanvas;
    cv::Mat preservedAlpha;
    if (cleanCanvas.channels() == 4) {
        cv::cvtColor(cleanCanvas, bgrCanvas, cv::COLOR_BGRA2BGR);
        cv::extractChannel(cleanCanvas, preservedAlpha, 3);
    } else {
        bgrCanvas = cleanCanvas.clone();
    }

    const cv::Mat originalBgrSnapshot = bgrCanvas.clone();
    const cv::Size imgSize(bgrCanvas.cols, bgrCanvas.rows);

    // 先从干净原图提取全部段落视觉 DNA，避免前一段擦除/渲染影响相邻段采样
    std::vector<ExtractedTextStyle> styles;
    styles.reserve(paragraphs.size());
    for (const auto& para : paragraphs) {
        const cv::Rect sampleRect = para.lineRects.empty() ? para.unionRect : para.lineRects.front();
        styles.push_back(TextReplaceEngine::extractStyle(
            originalBgrSnapshot, sampleRect, para.charBoxes, para.originalText));
    }

    // 阶段 1：仅擦除真正产生译文变化的段落（文件名/路径/专有名词等未变段落 100% 保留原图清晰像素）
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        if (!isTranslationEffectivelyChanged(paragraphs[i].originalText, paragraphs[i].translatedText)) {
            continue;
        }
        eraseParagraphBackground(bgrCanvas, paragraphs[i], styles[i]);
    }

    // 阶段 2：计算全部有效段落的安全避障域（含精准居中判定）与 1:1 原生拟合字号，
    // 再对全图常规正文簇做视觉字号平滑归一（Typographic Scale Harmonization），消除大小跳崖与小英文行蚂蚁字
    std::vector<SafeLayoutBox> safeBoxes(paragraphs.size());
    std::vector<float> fittedFontSizes(paragraphs.size(), 0.0f);
    std::vector<bool> activeChanged(paragraphs.size(), false);
    auto& fac = threadFactories();
    const bool facReady = fac.init();

    for (size_t i = 0; i < paragraphs.size(); ++i) {
        if (!isTranslationEffectivelyChanged(paragraphs[i].originalText, paragraphs[i].translatedText)) {
            continue;
        }
        activeChanged[i] = true;
        safeBoxes[i] = computeSafeLayoutRect(
            originalBgrSnapshot, paragraphs[i], i, paragraphs, extraObstacleRects, styles[i], imgSize);
        if (facReady) {
            const std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(trimWhitespace(paragraphs[i].translatedText));
            if (!wText.empty()) {
                const wchar_t* fontFamily = resolveTranslateFontFamily(fac.dwrite.Get(), styles[i].fontClass, targetLangCode);
                const wchar_t* localeName = resolveDWriteLocale(targetLangCode);
                const float basePx = std::clamp(styles[i].fontSizePx, 10.0f, 72.0f);
                const DWRITE_FONT_WEIGHT weight = resolveRenderFontWeight(styles[i].fontWeight, basePx, targetLangCode);
                const auto preFit = fitParagraphTextLayout(
                    fac.dwrite.Get(), wText, fontFamily, localeName, weight, basePx,
                    static_cast<float>(safeBoxes[i].rect.width), static_cast<float>(safeBoxes[i].rect.height),
                    paragraphs[i].isSingleLine, safeBoxes[i].centerHoriz);
                if (preFit.layout) {
                    fittedFontSizes[i] = preFit.fontPx;
                }
            }
        }
    }

    // 收集正文基准簇：优先多行正文段，其次常规宽度正文段（>=80px 且单行高 <=30px），求稳健中位数锚点
    std::vector<float> harmonizedPreferred(paragraphs.size(), 0.0f);
    std::vector<float> multiLineFitted;
    std::vector<float> multiLineOrig;
    std::vector<float> regularBodyFitted;
    std::vector<float> regularBodyOrig;
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        if (!activeChanged[i] || fittedFontSizes[i] < kMinReadableFontPx) continue;
        const float origPx = std::clamp(styles[i].fontSizePx, 10.0f, 72.0f);
        if (!paragraphs[i].isSingleLine) {
            multiLineFitted.push_back(fittedFontSizes[i]);
            multiLineOrig.push_back(origPx);
        }
        const int firstLineH = paragraphs[i].lineRects.empty()
            ? paragraphs[i].unionRect.height
            : paragraphs[i].lineRects.front().height;
        if (paragraphs[i].unionRect.width >= 80 && firstLineH >= 9 && firstLineH <= 30) {
            regularBodyFitted.push_back(fittedFontSizes[i]);
            regularBodyOrig.push_back(origPx);
        }
    }

    auto medianOf = [](std::vector<float> v) noexcept -> float {
        if (v.empty()) return 0.0f;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };

    const bool useMultiAnchor = (multiLineFitted.size() >= 2);
    const bool useRegularAnchor = (!useMultiAnchor && regularBodyFitted.size() >= 4);
    if (useMultiAnchor || useRegularAnchor) {
        const float anchorOrigPx = useMultiAnchor ? medianOf(multiLineOrig) : medianOf(regularBodyOrig);
        float targetBodyPx = 0.0f;
        if (useMultiAnchor) {
            auto sortedMulti = multiLineFitted;
            std::sort(sortedMulti.begin(), sortedMulti.end());
            const float medianFitted = sortedMulti[sortedMulti.size() / 2];
            targetBodyPx = (std::max)(sortedMulti.front(), medianFitted * 0.92f);
        } else {
            targetBodyPx = medianOf(regularBodyFitted);
        }

        const bool cjkTarget = isCjkTargetLanguage(targetLangCode);
        for (size_t i = 0; i < paragraphs.size(); ++i) {
            if (!activeChanged[i] || fittedFontSizes[i] < kMinReadableFontPx) continue;
            const float origPx = std::clamp(styles[i].fontSizePx, 10.0f, 72.0f);
            // 属于同一常规正文簇（排除大标题 > 1.24x 与极小灰注 < 0.72x）
            if (origPx >= anchorOrigPx * 0.72f && origPx <= anchorOrigPx * 1.24f) {
                if (origPx > targetBodyPx) {
                    // 高于正文目标的段落压平至统一正文字号，彻底消灭阶梯割裂感
                    harmonizedPreferred[i] = targetBodyPx;
                } else if (cjkTarget && origPx < targetBodyPx * 0.94f && paragraphs[i].unionRect.width >= 60) {
                    // CJK 译文遇到纯小写英文缺下伸部导致 OCR 框偏矮时，温和抬升至同簇正文标准字号（仍受安全框 1:1 校验）
                    const float safeHeightCap = static_cast<float>(safeBoxes[i].rect.height) * 0.92f;
                    harmonizedPreferred[i] = (std::min)(targetBodyPx, (std::max)(origPx, safeHeightCap));
                }
            }
        }
    }

    size_t rendered = 0;
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        if (!activeChanged[i]) continue;
        const cv::Rect drawn = renderParagraphOntoBgr(
            bgrCanvas, paragraphs[i], safeBoxes[i], styles[i], targetLangCode, harmonizedPreferred[i]);
        if (drawn.width > 0 && drawn.height > 0) {
            paragraphs[i].renderedRect = drawn;
            ++rendered;
        }
    }

    if (cleanCanvas.channels() == 4) {
        cv::cvtColor(bgrCanvas, res.translatedCanvas, cv::COLOR_BGR2BGRA);
        cv::insertChannel(preservedAlpha, res.translatedCanvas, 3);
    } else {
        res.translatedCanvas = std::move(bgrCanvas);
    }

    res.paragraphs = std::move(paragraphs);
    res.renderedCount = rendered;
    res.success = (rendered > 0);
    return res;
}

} // namespace tools3000::capture
