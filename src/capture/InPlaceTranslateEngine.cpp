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
#include <climits>
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
constexpr int kMaxCenterProbeSpanPx = 240;
constexpr int kMaxSafeExpandBottomMultiLinePx = 22;
constexpr int kMaxSafeExpandBottomSingleLinePx = 4;
constexpr int kMaxEraseInkExpandXPx = 8;
constexpr int kMaxEraseInkExpandYPx = 4;
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
    // 首字母大写纯字母前缀(>=3)紧接尾随数字段(2~6位)的品牌/产品/项目名（如 Tools3000 / Win11 / Office365 / Llama31），
    // 排除小写介词+年份 OCR 粘连（如 for2026 / since1992）与 2 字母词（如 In2026）
    if (otherPunct == 0 && digits >= 2 && digits <= 6 &&
        t[0] >= 'A' && t[0] <= 'Z' && (upper + lower) >= 3 && (upper + lower) <= 18) {
        size_t alphaPrefix = 0;
        while (alphaPrefix < t.size() && std::isalpha(static_cast<unsigned char>(t[alphaPrefix]))) {
            ++alphaPrefix;
        }
        size_t digitSuffix = alphaPrefix;
        while (digitSuffix < t.size() && std::isdigit(static_cast<unsigned char>(t[digitSuffix]))) {
            ++digitSuffix;
        }
        if (alphaPrefix >= 3 && digitSuffix == t.size() && (digitSuffix - alphaPrefix) >= 2) {
            return true;
        }
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

    // 4. API 树节点条目（支持可选树形折叠箭头前缀 `>` / `v`，如 `> POST ProductionOrders Copy`、`GET UserObjectsMD`）
    std::string_view apiView = t;
    if (apiView.size() >= 3 && (apiView[0] == '>' || apiView[0] == 'v' || apiView[0] == 'V') && apiView[1] == ' ') {
        apiView.remove_prefix(2);
        while (!apiView.empty() && apiView.front() == ' ') apiView.remove_prefix(1);
    }
    const size_t firstSp = apiView.find(' ');
    if (firstSp >= 3 && firstSp <= 6) {
        const std::string_view verb = apiView.substr(0, firstSp);
        if (verb == "GET" || verb == "POST" || verb == "PUT" || verb == "PATCH" || verb == "DEL" || verb == "DELETE") {
            std::string_view rest = apiView.substr(firstSp + 1);
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

struct BorderBgStats {
    cv::Vec3b medianBgr{0, 0, 0};
    float stdDev = 0.0f;
};

BorderBgStats sampleLineBorderBgStats(const cv::Mat& bgr, const cv::Rect& rc) noexcept {
    BorderBgStats out{};
    if (bgr.empty() || rc.width <= 0 || rc.height <= 0) return out;
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
    if (bs.empty()) {
        out.medianBgr = bgr.at<cv::Vec3b>(std::clamp(rc.y, 0, bgr.rows - 1), std::clamp(rc.x, 0, bgr.cols - 1));
        return out;
    }
    auto bsCopy = bs;
    auto gsCopy = gs;
    auto rsCopy = rs;
    const size_t mid = bsCopy.size() / 2;
    std::nth_element(bsCopy.begin(), bsCopy.begin() + static_cast<ptrdiff_t>(mid), bsCopy.end());
    std::nth_element(gsCopy.begin(), gsCopy.begin() + static_cast<ptrdiff_t>(mid), gsCopy.end());
    std::nth_element(rsCopy.begin(), rsCopy.begin() + static_cast<ptrdiff_t>(mid), rsCopy.end());
    out.medianBgr = cv::Vec3b(bsCopy[mid], gsCopy[mid], rsCopy[mid]);

    double varSum = 0.0;
    for (size_t i = 0; i < bs.size(); ++i) {
        const double db = static_cast<double>(bs[i]) - out.medianBgr[0];
        const double dg = static_cast<double>(gs[i]) - out.medianBgr[1];
        const double dr = static_cast<double>(rs[i]) - out.medianBgr[2];
        varSum += (db * db + dg * dg + dr * dr) / 3.0;
    }
    out.stdDev = static_cast<float>(std::sqrt(varSum / static_cast<double>(bs.size())));
    return out;
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
    samples.reserve(160);
    const int stepX = (std::max)(1, clamped.width / 32);
    const int stepY = (clamped.height <= 22) ? 1 : (std::max)(1, clamped.height / 8);
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
    std::vector<uint8_t> bs, gs, rs;
    bs.reserve(take);
    gs.reserve(take);
    rs.reserve(take);
    for (size_t i = 0; i < take; ++i) {
        bs.push_back(samples[i].px[0]);
        gs.push_back(samples[i].px[1]);
        rs.push_back(samples[i].px[2]);
    }
    const size_t mid = take / 2;
    std::nth_element(bs.begin(), bs.begin() + static_cast<ptrdiff_t>(mid), bs.end());
    std::nth_element(gs.begin(), gs.begin() + static_cast<ptrdiff_t>(mid), gs.end());
    std::nth_element(rs.begin(), rs.begin() + static_cast<ptrdiff_t>(mid), rs.end());
    return cv::Vec3b(bs[mid], gs[mid], rs[mid]);
}

/// 0.05ms 视觉分隔墙检测：阻止跨背景色块、跨搜索框/表格横线、以及跨灰/黑信息层级的文字行误合并；
/// 对高方差照片纹理背景自动免疫（避免照片明暗变化把 3 行白色大图标题撕裂成孤立单行）
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

    const BorderBgStats upStats = sampleLineBorderBgStats(bgrView, upper);
    const BorderBgStats loStats = sampleLineBorderBgStats(bgrView, lower);
    const cv::Vec3b upBg = upStats.medianBgr;
    const cv::Vec3b loBg = loStats.medianBgr;
    const bool texturedPhotoBg = (upStats.stdDev > 18.0f || loStats.stdDev > 18.0f);

    if (!texturedPhotoBg) {
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
    }

    // 前景字色与对比度跳变（如 HN 浅灰作者/时间 meta 行 vs 纯黑正文，卡片深黑标题 vs 浅灰副标题）
    const cv::Vec3b upFg = sampleLineCoreFgBgr(bgrView, upper, upBg);
    const cv::Vec3b loFg = sampleLineCoreFgBgr(bgrView, lower, loBg);
    const int fgDiff = std::abs(static_cast<int>(upFg[0]) - static_cast<int>(loFg[0])) +
                       std::abs(static_cast<int>(upFg[1]) - static_cast<int>(loFg[1])) +
                       std::abs(static_cast<int>(upFg[2]) - static_cast<int>(loFg[2]));
    const int fgThresh = texturedPhotoBg ? 115 : 96;
    if (fgDiff > fgThresh) {
        return true;
    }
    return false;
}

/// 识别全大写短眉标 / 元数据标签（如 OPINION / BREAKING / ANALYSIS / 4 MIN READ / FROM THE ATHLETIC），
/// 防止其错误向下吞并常规大小写主标题，导致整个大标题被眉标的小字号和浅色带偏
bool isAllCapsShortEyebrow(std::string_view utf8) noexcept {
    const std::string t = trimWhitespace(utf8);
    if (t.empty() || t.size() > 22) return false;
    size_t upper = 0;
    for (unsigned char c : t) {
        if (c >= 0x80) return false;
        if (c >= 'a' && c <= 'z') return false;
        if (c >= 'A' && c <= 'Z') ++upper;
    }
    return upper >= 2;
}

bool containsAsciiLowercase(std::string_view utf8) noexcept {
    for (unsigned char c : utf8) {
        if (c >= 'a' && c <= 'z') return true;
    }
    return false;
}

bool canMergeIntoParagraph(
    const TranslationParagraph& para,
    const PinOcrBlock& next,
    const cv::Mat& sourceImage,
    bool* outMergedByCenter = nullptr) noexcept {
    if (outMergedByCenter) *outMergedByCenter = false;
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

    // 1.5. 眉标防火墙：单行全大写短眉标（如 OPINION / 4 MIN READ）绝不吞并下方大小写标题/正文
    if (para.isSingleLine && isAllCapsShortEyebrow(para.originalText) && containsAsciiLowercase(next.text)) {
        if (next.origBox.height > lastRect.height + 1 || next.origBox.width > static_cast<int>(lastRect.width * 1.35)) {
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
    // 严格限制小字向下吞并大字（上限 1.26x）；对严格左对齐且非句末的自然尾行允许轻微放宽下限至 0.68x（兼容无下伸部短尾行）
    const bool strictTailLeft = (std::abs(next.origBox.x - lastRect.x) <= 5) &&
                                (lastRect.width >= 110) &&
                                !endsWithSentenceStop(para.originalText);
    const float minHRatio = strictTailLeft ? 0.68f : 0.78f;
    if (hRatio < minHRatio || hRatio > 1.26f) {
        return false; // 标题字号与正文字号差异大，独立成段
    }

    const int gapY = next.origBox.y - (lastRect.y + lastRect.height);
    const float avgH = 0.5f * (lastH + nextH);
    // 自然折行行距通常为 0.20x ~ 0.62x 行高；严格卡紧至 0.72x 杜绝将相邻独立段落/评论强行粘合
    if (gapY < -static_cast<int>(avgH * 0.25f) || gapY > static_cast<int>(avgH * 0.72f)) {
        return false;
    }

    // 3. 严守同左边缘对齐 / 列表悬挂缩进 / 同中轴居中多行标题，彻底杜绝跨列跨缩进误合并
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

    const float paraCenterX = static_cast<float>(para.unionRect.x) + static_cast<float>(para.unionRect.width) * 0.5f;
    const float nextCenterX = static_cast<float>(next.origBox.x) + static_cast<float>(next.origBox.width) * 0.5f;
    const bool strictCenterAligned =
        !strictLeftAligned && !bulletHangingIndent &&
        (std::abs(nextCenterX - paraCenterX) <= (std::max)(avgH * 0.65f, 12.0f)) &&
        (gapY <= static_cast<int>(avgH * 0.55f)) &&
        ((std::min)(lastRect.width, next.origBox.width) >= 48) &&
        !endsWithSentenceStop(para.originalText);

    if (!strictLeftAligned && !bulletHangingIndent && !strictCenterAligned) {
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

    // 5. 列表新条目与独立短 UI 标签/动作按钮（DPI 无关行高比例约束，杜绝 2K 高分屏误合并）
    if (startsWithListBulletOrNumber(next.text)) {
        return false;
    }
    const size_t avgLineVisualLen = countVisualChars(para.originalText) / (std::max<size_t>)(1u, para.lineRects.size());
    const size_t nextVisualLen = countVisualChars(next.text);
    const int shortUiMaxW = (std::max)(165, static_cast<int>(std::lround(avgH * 10.0f)));
    const bool bothShortUiItems =
        (avgLineVisualLen <= 8 && nextVisualLen <= 8) &&
        (lastRect.width <= shortUiMaxW && next.origBox.width <= shortUiMaxW);
    if (bothShortUiItems) {
        return false;
    }

    // 较宽松行距的短菜单项或独立短动作按钮（如 HN "reply" / "share"，严格按行高自适应宽容度，兼容 100%~250% DPI）
    if (avgLineVisualLen <= 10 && nextVisualLen <= 10 && gapY > static_cast<int>(avgH * 0.38f)) {
        return false;
    }
    const int compactActionMaxW = (std::max)(72, static_cast<int>(std::lround(avgH * 4.5f)));
    const bool nextIsSingleShortWord = (nextVisualLen <= 6 && trimWhitespace(next.text).find(' ') == std::string::npos);
    if (nextIsSingleShortWord && next.origBox.width <= compactActionMaxW &&
        (gapY > static_cast<int>(avgH * 0.26f) || endsWithSentenceStop(para.originalText))) {
        return false;
    }

    // 6. 视觉分隔墙检测（背景色跳变 / 输入框与卡片边框线 / 灰黑字色层级跳变；照片纹理背景自动免疫）
    if (hasVisualSeamBetween(sourceImage, lastRect, next.origBox)) {
        return false;
    }

    if (outMergedByCenter && strictCenterAligned) {
        *outMergedByCenter = true;
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

/// 忠实保留原图深/浅色次级灰字气质（阈值 0.16f），仅在真实极低对比度时沿原色对比方向温和推升，
/// 彻底杜绝深色模式次级灰字被粗暴替换为刺眼纯白
cv::Vec3b ensureReadableTextBgr(const cv::Vec3b& textBgr, const cv::Vec3b& bgBgr, bool darkBg) noexcept {
    const float lText = relativeLuminance(textBgr);
    const float lBg = relativeLuminance(bgBgr);
    const float contrast = std::abs(lText - lBg);
    if (contrast >= 0.16f) {
        return textBgr;
    }
    const cv::Vec3b target = darkBg ? cv::Vec3b(225, 222, 218) : cv::Vec3b(42, 38, 34);
    const float lTarget = relativeLuminance(target);
    const float denom = (std::max)(0.05f, std::abs(lTarget - lBg));
    const float desiredContrast = 0.28f;
    const float alpha = std::clamp((desiredContrast - contrast) / denom, 0.30f, 0.72f);
    auto lerpU8 = [&](uint8_t a, uint8_t b) noexcept -> uint8_t {
        const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * alpha;
        return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
    };
    return cv::Vec3b(lerpU8(textBgr[0], target[0]), lerpU8(textBgr[1], target[1]), lerpU8(textBgr[2], target[2]));
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
    int safeTopY = 0;     // 上方允许透明绘制的洁净边界（防裁切腰斩，不扩大擦除）
    int safeBottomY = 0;  // 下方允许透明绘制的洁净边界
};

/// 0.05ms 像素级向右洁净空白探测：遇到垂直表格线、低对比度按钮 3D 边框、图标轮廓或相邻文字立刻刹车
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
    const bool cleanBg = (style.bgPlane.complexity <= 7.5f);
    const bool ultraFlatBg = (style.bgPlane.complexity <= 4.0f);

    int lastCleanX = baseRect.x + baseRect.width;
    for (int x = startX; x < endX; ++x) {
        int strongDiff = 0;
        int mildDiff = 0;
        int subtleShift = 0;
        int stepJump = 0;
        for (int y = y0; y < y1; ++y) {
            const cv::Vec3b curPx = originalBgr.at<cv::Vec3b>(y, x);
            const int d = pixelDiffFromPlane(curPx, style.bgPlane, x, y);
            if (d > 66) ++strongDiff;
            if (d >= 26) ++mildDiff;
            if (d >= 14) ++subtleShift;
            if (x > startX) {
                const cv::Vec3b prevPx = originalBgr.at<cv::Vec3b>(y, x - 1);
                const int sd = std::abs(static_cast<int>(curPx[0]) - static_cast<int>(prevPx[0])) +
                               std::abs(static_cast<int>(curPx[1]) - static_cast<int>(prevPx[1])) +
                               std::abs(static_cast<int>(curPx[2]) - static_cast<int>(prevPx[2]));
                if (sd >= 12) ++stepJump;
            }
        }
        // 平缓光照渐变豁免：仅当本列相较前一列也存在真实边沿跃变（stepJump）或总体已显著偏离底色（strongDiff）时，
        // 才按低对比度边框刹车；防止实拍屏幕/渐变背景因拟合平面微斜率外推误差在无边界空白处提前误刹
        const bool hasLocalEdge = (x == startX) || (stepJump * 100 >= bandH * 28);
        const bool colDirty = (strongDiff * 6 >= bandH) ||
                              (strongDiff >= 3 && bandH <= 18) ||
                              (cleanBg && hasLocalEdge && bandH >= 6 && mildDiff * 100 >= bandH * 70) ||
                              (ultraFlatBg && hasLocalEdge && bandH >= 6 && subtleShift * 100 >= bandH * 88);
        if (colDirty) {
            return {(std::max)(baseRect.x + baseRect.width, x - kMinObstacleMarginPx), true};
        }
        lastCleanX = x;
    }
    return {lastCleanX, false};
}

/// 0.03ms 像素级向下方洁净空白探测：遇到水平表格线、按钮底边、分隔线或下方图标/文字立即刹车
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
    const bool cleanBg = (style.bgPlane.complexity <= 7.5f);

    int lastCleanY = baseRect.y + baseRect.height;
    for (int y = startY; y < endY; ++y) {
        const auto* row = originalBgr.ptr<cv::Vec3b>(y);
        int samples = 0;
        int strongCount = 0;
        int mildCount = 0;
        for (int x = x0; x < x1; x += step) {
            ++samples;
            const int d = pixelDiffFromPlane(row[x], style.bgPlane, x, y);
            if (d > 60) ++strongCount;
            if (d >= 26) ++mildCount;
        }
        if (samples > 0 && (strongCount * 5 >= samples || (cleanBg && spanW >= 20 && mildCount * 100 >= samples * 72))) {
            return (std::max)(baseRect.y + baseRect.height, y - 2);
        }
        lastCleanY = y;
    }
    return lastCleanY;
}

/// 0.02ms 像素级向上方洁净空白探测（为单行 DirectWrite 绘制预留透明防裁切呼吸区，遇上横线/邻行立刻刹车）
int probeCleanTopBound(
    const cv::Mat& originalBgr,
    const cv::Rect& baseRect,
    int limitTop,
    const ExtractedTextStyle& style) noexcept {
    if (originalBgr.empty() || style.complexBackground) {
        return (std::max)(limitTop, baseRect.y - 2);
    }
    const int startY = baseRect.y - 1;
    const int endY = (std::max)(1, limitTop);
    if (startY <= endY) return baseRect.y;

    const int x0 = (std::max)(0, baseRect.x + 1);
    const int x1 = (std::min)(originalBgr.cols, baseRect.x + baseRect.width - 1);
    if (x1 <= x0) return baseRect.y;
    const int spanW = x1 - x0;
    const int step = (std::max)(1, spanW / 32);
    const bool cleanBg = (style.bgPlane.complexity <= 7.5f);

    int lastCleanY = baseRect.y;
    for (int y = startY; y >= endY; --y) {
        const auto* row = originalBgr.ptr<cv::Vec3b>(y);
        int samples = 0;
        int strongCount = 0;
        int mildCount = 0;
        for (int x = x0; x < x1; x += step) {
            ++samples;
            const int d = pixelDiffFromPlane(row[x], style.bgPlane, x, y);
            if (d > 60) ++strongCount;
            if (d >= 26) ++mildCount;
        }
        if (samples > 0 && (strongCount * 5 >= samples || (cleanBg && spanW >= 20 && mildCount * 100 >= samples * 72))) {
            return (std::min)(baseRect.y, y + 2);
        }
        lastCleanY = y;
    }
    return lastCleanY;
}

/// 0.03ms 像素级向左洁净空白探测（用于检测居中按钮/居中表格单元格/居中广告卡片的真实左侧边界）
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
    const bool cleanBg = (style.bgPlane.complexity <= 7.5f);
    const bool ultraFlatBg = (style.bgPlane.complexity <= 4.0f);

    int lastCleanX = baseRect.x;
    for (int x = startX; x >= endX; --x) {
        int strongDiff = 0;
        int mildDiff = 0;
        int subtleShift = 0;
        for (int y = y0; y < y1; ++y) {
            const int d = pixelDiffFromPlane(originalBgr.at<cv::Vec3b>(y, x), style.bgPlane, x, y);
            if (d > 66) ++strongDiff;
            if (d >= 26) ++mildDiff;
            if (d >= 14) ++subtleShift;
        }
        const bool colDirty = (strongDiff * 6 >= bandH) ||
                              (strongDiff >= 3 && bandH <= 18) ||
                              (cleanBg && bandH >= 6 && mildDiff * 100 >= bandH * 70) ||
                              (ultraFlatBg && bandH >= 6 && subtleShift * 100 >= bandH * 88);
        if (colDirty) {
            return {(std::min)(baseRect.x, x + kMinObstacleMarginPx), true};
        }
        lastCleanX = x;
    }
    return {lastCleanX, false};
}

/// 格式塔左对齐列守恒检测：判断是否存在垂直邻近、左边缘严丝合缝对齐但宽度不同（非同宽居中堆叠）的同列兄弟块
bool hasLeftAlignedColumnPeer(
    const cv::Rect& baseRect,
    size_t selfIdx,
    const std::vector<TranslationParagraph>& allParagraphs,
    const std::vector<cv::Rect>& extraObstacles) noexcept {
    const float selfCenterX = static_cast<float>(baseRect.x) + static_cast<float>(baseRect.width) * 0.5f;
    auto isPeer = [&](const cv::Rect& other) noexcept -> bool {
        if (other.width < 24 || other.height < 6) return false;
        // 排除与自身垂直重叠的同行元素
        if (other.y < baseRect.y + baseRect.height + 2 && other.y + other.height > baseRect.y - 2) {
            return false;
        }
        const int dy = (other.y >= baseRect.y + baseRect.height)
            ? (other.y - (baseRect.y + baseRect.height))
            : (baseRect.y - (other.y + other.height));
        if (dy > 450) return false;
        if (std::abs(other.x - baseRect.x) > 8) return false;
        // 排除上下两枚同宽同中轴的居中堆叠按钮
        const float otherCenterX = static_cast<float>(other.x) + static_cast<float>(other.width) * 0.5f;
        if (std::abs(otherCenterX - selfCenterX) <= 8.0f && std::abs(other.width - baseRect.width) <= 14) {
            return false;
        }
        return true;
    };

    for (size_t i = 0; i < allParagraphs.size(); ++i) {
        if (i == selfIdx) continue;
        if (isPeer(allParagraphs[i].unionRect)) return true;
    }
    for (const cv::Rect& obs : extraObstacles) {
        if (isPeer(obs)) return true;
    }
    return false;
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
    // 多行正文严守原版面栏宽（仅允许微扩 28px 呼吸留白，防跨栏侵占右侧留白）；单行标签保留最大 128px 避障扩展上限
    const int rightExpandCap = selfPara.isSingleLine
        ? kMaxSafeExpandRightPx
        : (std::min)(28, (std::max)(10, baseRect.width / 10));
    int maxRight = (std::min)(imageSize.width - 2, baseRect.x + baseRect.width + rightExpandCap);
    bool rightObstacleHit = false; // 严禁把画布边缘视为按钮/表格真实边框！

    // 居中探测专用的对称宽跨度（使大灰框内孤立居中词如 Advertisement 可触及左右灰框边框）
    const int centerProbeSpan = (std::min)(kMaxCenterProbeSpanPx, (std::max)(48, imageSize.width / 2));
    int centerMaxRight = (std::min)(imageSize.width - 2, baseRect.x + baseRect.width + centerProbeSpan);
    bool centerRightObstacleHit = false;
    int centerMinLeft = (std::max)(2, baseRect.x - centerProbeSpan);
    bool centerLeftObstacleHit = false;

    const int bottomCap = selfPara.isSingleLine
        ? (baseRect.height >= 26 ? (std::min)(baseRect.height, 26) : kMaxSafeExpandBottomSingleLinePx)
        : kMaxSafeExpandBottomMultiLinePx;
    int maxBottom = (std::min)(imageSize.height - 2, baseRect.y + baseRect.height + bottomCap);
    int safeTopLimit = (std::max)(1, baseRect.y - 5);
    int safeBottomLimit = (std::min)(imageSize.height - 1, baseRect.y + baseRect.height + 5);

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
            if (candidateRight < centerMaxRight) {
                centerMaxRight = candidateRight;
                centerRightObstacleHit = true;
            }
        }
        if (verticalOverlap && other.x + other.width <= baseRect.x + 2) {
            const int candidateLeft = other.x + other.width + kMinObstacleMarginPx;
            if (candidateLeft > centerMinLeft) {
                centerMinLeft = candidateLeft;
                centerLeftObstacleHit = true;
            }
        }
        const bool horizOverlap =
            (other.x < maxRight) &&
            (other.x + other.width > baseRect.x);
        if (horizOverlap && other.y >= baseRect.y + baseRect.height - 2) {
            maxBottom = (std::min)(maxBottom, other.y - kMinObstacleMarginPx);
            safeBottomLimit = (std::min)(safeBottomLimit, other.y - 1);
        }
        if (horizOverlap && other.y + other.height <= baseRect.y + 2) {
            safeTopLimit = (std::max)(safeTopLimit, other.y + other.height + 1);
        }
    };

    for (size_t i = 0; i < allParagraphs.size(); ++i) {
        if (i == selfIdx) continue;
        applyObstacle(allParagraphs[i].unionRect);
    }
    for (const cv::Rect& obs : extraObstacles) {
        applyObstacle(obs);
    }

    // 像素级边界刹车（表格线 / 低对比度按钮 3D 边框 / 图标 / 二维码边缘）
    const CleanBoundProbe rightProbe = probeCleanRightBound(originalBgr, baseRect, maxRight, style);
    maxRight = rightProbe.bound;
    maxBottom = probeCleanBottomBound(originalBgr, baseRect, maxBottom, style);
    const int safeTopY = selfPara.isSingleLine
        ? probeCleanTopBound(originalBgr, baseRect, safeTopLimit, style)
        : baseRect.y;
    const int safeBottomY = selfPara.isSingleLine
        ? (std::max)(maxBottom, probeCleanBottomBound(originalBgr, baseRect, safeBottomLimit, style))
        : maxBottom;

    int finalX = baseRect.x;
    int finalRight = (std::max)(baseRect.x + baseRect.width, maxRight);
    bool isCentered = false;

    if (selfPara.preferCentered) {
        // 多行居中标题聚类（如居中图表标题）直接沿中心轴对称延展安全域
        const CleanBoundProbe cLeft = probeCleanLeftBound(originalBgr, baseRect, centerMinLeft, style);
        const CleanBoundProbe cRight = probeCleanRightBound(originalBgr, baseRect, centerMaxRight, style);
        const int leftGap = (std::max)(0, baseRect.x - cLeft.bound);
        const int rightGap = (std::max)(0, cRight.bound - (baseRect.x + baseRect.width));
        const int symMargin = (std::min)(24, (std::min)(leftGap, rightGap));
        finalX = baseRect.x - symMargin;
        finalRight = baseRect.x + baseRect.width + symMargin;
        isCentered = true;
    } else if (selfPara.isSingleLine && !style.complexBackground) {
        // 格式塔左对齐列守恒：只要存在同列左对齐兄弟行，100% 强制严守 baseRect.x 左对齐，绝不漂移居中！
        const bool lockedLeftColumn = hasLeftAlignedColumnPeer(baseRect, selfIdx, allParagraphs, extraObstacles);
        if (!lockedLeftColumn) {
            const CleanBoundProbe leftProbe = probeCleanLeftBound(originalBgr, baseRect, centerMinLeft, style);
            const CleanBoundProbe cRightProbe = (centerMaxRight > maxRight)
                ? probeCleanRightBound(originalBgr, baseRect, centerMaxRight, style)
                : rightProbe;
            const bool leftBounded = leftProbe.hitBoundary || centerLeftObstacleHit;
            const bool rightBounded = cRightProbe.hitBoundary || centerRightObstacleHit || rightObstacleHit;
            const int leftGap = (std::max)(0, baseRect.x - leftProbe.bound);
            const int rightGap = (std::max)(0, cRightProbe.bound - (baseRect.x + baseRect.width));
            const int containerW = (std::max)(baseRect.width, cRightProbe.bound - leftProbe.bound);

            // 满宽行禁居中：文字本身已填满绝大多数画布/容器宽度时绝不误判为居中短按钮
            const bool notFullWidthRow =
                (static_cast<float>(baseRect.width) < static_cast<float>(imageSize.width) * 0.60f) &&
                (static_cast<float>(baseRect.width) < static_cast<float>(containerW) * 0.78f ||
                 (leftProbe.hitBoundary && cRightProbe.hitBoundary && baseRect.width <= 180));

            // 场景 A：居中按钮 / 居中表格单元格 / 居中广告块（左右双侧均撞到真实视觉边框或内部障碍，且留白对称）
            const bool boundedSymmetricCenter =
                notFullWidthRow &&
                leftBounded && rightBounded &&
                leftGap >= 6 && rightGap >= 6 &&
                std::abs(leftGap - rightGap) <= (std::max)(14, (leftGap + rightGap) / 3);

            // 场景 B：整图水平居中主标语（中心点贴近画布中轴 ±4%，且左右均有宽裕对称留白）
            const float selfCenterX = static_cast<float>(baseRect.x) + static_cast<float>(baseRect.width) * 0.5f;
            const float canvasCenterX = static_cast<float>(imageSize.width) * 0.5f;
            const bool canvasAxisCenter =
                (baseRect.width >= 90) &&
                (static_cast<float>(baseRect.width) < static_cast<float>(imageSize.width) * 0.65f) &&
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
    }

    const int finalW = (std::max)(baseRect.width, finalRight - finalX);
    const int finalH = (std::max)(baseRect.height, maxBottom - baseRect.y);
    const cv::Rect clamped = cv::Rect(finalX, baseRect.y, finalW, finalH) & cv::Rect(0, 0, imageSize.width, imageSize.height);
    return SafeLayoutBox{
        clamped,
        isCentered,
        std::clamp(safeTopY, 0, clamped.y),
        std::clamp(safeBottomY, clamped.y + clamped.height, imageSize.height)
    };
}

/// 4 向像素级墨水连通封顶擦除框计算（彻底消灭 OCR 紧包框遗漏首尾字母斜撇/竖杆/升部导致的“毛边”，
/// 同时严格受网格边框线刹车与未翻译障碍物边界保护，绝不擦花按钮边框、表格线或相邻图标）
cv::Rect computeInkSealedEraseRect(
    const cv::Mat& originalBgr,
    const cv::Rect& rawLine,
    const ExtractedTextStyle& style,
    const std::vector<cv::Rect>& protectedObstacles) noexcept {
    const cv::Rect imgBounds(0, 0, originalBgr.cols, originalBgr.rows);
    const cv::Rect base = rawLine & imgBounds;
    if (base.width <= 0 || base.height <= 0) return {};

    // 1. 计算受未翻译障碍物约束的四向硬边界（按行高自适应扩张上限，兼容 100%~250% DPI 高分屏彻底封净首尾毛边）
    const int maxExpandX = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(base.height) * 0.45f)),
        kMaxEraseInkExpandXPx, 22);
    const int maxExpandY = std::clamp(
        static_cast<int>(std::lround(static_cast<float>(base.height) * 0.25f)),
        kMaxEraseInkExpandYPx, 12);
    int hardLeft = (std::max)(0, base.x - maxExpandX);
    int hardRight = (std::min)(originalBgr.cols, base.x + base.width + maxExpandX);
    int hardTop = (std::max)(0, base.y - maxExpandY);
    int hardBottom = (std::min)(originalBgr.rows, base.y + base.height + maxExpandY);

    for (const cv::Rect& obs : protectedObstacles) {
        if (obs.width <= 0 || obs.height <= 0) continue;
        const cv::Rect inter = base & obs;
        if (inter.area() > 0 && inter.area() * 2 >= (std::min)(base.area(), obs.area())) continue;
        const bool vOverlap = (obs.y < base.y + base.height + 2) && (obs.y + obs.height > base.y - 2);
        if (vOverlap && obs.x >= base.x + base.width - 1) {
            hardRight = (std::min)(hardRight, (std::max)(base.x + base.width, obs.x - 1));
        }
        if (vOverlap && obs.x + obs.width <= base.x + 1) {
            hardLeft = (std::max)(hardLeft, (std::min)(base.x, obs.x + obs.width + 1));
        }
        const bool hOverlap = (obs.x < base.x + base.width + 2) && (obs.x + obs.width > base.x - 2);
        if (hOverlap && obs.y >= base.y + base.height - 1) {
            hardBottom = (std::min)(hardBottom, (std::max)(base.y + base.height, obs.y - 1));
        }
        if (hOverlap && obs.y + obs.height <= base.y + 1) {
            hardTop = (std::max)(hardTop, (std::min)(base.y, obs.y + obs.height + 1));
        }
    }

    const cv::Vec3b centerBg = style.bgPlane.evalAt(
        static_cast<float>(base.x + base.width * 0.5f),
        static_cast<float>(base.y + base.height * 0.5f));
    const int fgBgDist = std::abs(static_cast<int>(style.textBgr[0]) - static_cast<int>(centerBg[0])) +
                         std::abs(static_cast<int>(style.textBgr[1]) - static_cast<int>(centerBg[1])) +
                         std::abs(static_cast<int>(style.textBgr[2]) - static_cast<int>(centerBg[2]));
    const int inkThresh = std::clamp(fgBgDist / 4, 24, 48);

    // 判断是否为贯穿型 UI 竖边框线（按钮边、表格竖线、模态窗边框）
    auto isVerticalBorderWall = [&](int x) noexcept -> bool {
        if (x < 0 || x >= originalBgr.cols) return true;
        const int extY0 = (std::max)(0, base.y - 4);
        const int extY1 = (std::min)(originalBgr.rows, base.y + base.height + 4);
        const int extH = extY1 - extY0;
        if (extH < 8) return false;
        int hits = 0;
        for (int y = extY0; y < extY1; ++y) {
            if (pixelDiffFromPlane(originalBgr.at<cv::Vec3b>(y, x), style.bgPlane, x, y) >= 24) {
                ++hits;
            }
        }
        return (hits * 100 >= extH * 82);
    };

    auto colInkCount = [&](int x) noexcept -> int {
        if (x < 0 || x >= originalBgr.cols) return 0;
        const int y0 = (std::max)(0, base.y - 1);
        const int y1 = (std::min)(originalBgr.rows, base.y + base.height + 1);
        int cnt = 0;
        for (int y = y0; y < y1; ++y) {
            if (pixelDiffFromPlane(originalBgr.at<cv::Vec3b>(y, x), style.bgPlane, x, y) >= inkThresh) {
                ++cnt;
            }
        }
        return cnt;
    };

    auto scanHoriz = [&](int startX, int step, int maxDist) noexcept -> int {
        if (maxDist <= 0) return 0;
        int best = 0;
        int minInk = INT_MAX;
        int minInkDist = 0;
        bool hitZero = false;
        bool hitWall = false;
        int consecZero = 0;
        for (int d = 1; d <= maxDist; ++d) {
            const int x = startX + step * d;
            if (isVerticalBorderWall(x)) {
                hitWall = true;
                break;
            }
            const int c = colInkCount(x);
            if (c == 0) {
                ++consecZero;
                hitZero = true;
                if (best > 0) {
                    // 已吸收外延字形残边后遇纯背景封顶：额外多收 1px 抗锯齿淡边，绝不越过墙
                    if (d + 1 <= maxDist && !isVerticalBorderWall(startX + step * (d + 1))) {
                        best = d;
                    }
                    break;
                }
                // 紧贴框边第 1px 就是空白，窥探 1px 防斜撇悬空点（如 W 左顶角、感叹号点）
                if (consecZero >= 2) break;
                continue;
            }
            consecZero = 0;
            if (c < minInk) {
                minInk = c;
                minInkDist = d;
            }
            best = d;
        }
        if (!hitZero && !hitWall && best >= maxDist) {
            // 连续扫满上限仍无空白封顶：外面可能是实心图标/色块而非孤立漏框字母，退回谷底或安全 2px
            best = (minInk <= 2) ? minInkDist : (std::min)(2, maxDist);
        }
        // 在无竖墙阻挡前提下保留最少 2px 抗锯齿安全缓冲
        if (!hitWall && best < (std::min)(2, maxDist)) {
            int safeMin = 0;
            for (int d = 1; d <= (std::min)(2, maxDist); ++d) {
                if (isVerticalBorderWall(startX + step * d)) break;
                safeMin = d;
            }
            best = (std::max)(best, safeMin);
        }
        return best;
    };

    const int expandLeft = scanHoriz(base.x, -1, base.x - hardLeft);
    const int expandRight = scanHoriz(base.x + base.width - 1, +1, hardRight - (base.x + base.width));
    const int x0 = (std::max)(hardLeft, base.x - expandLeft);
    const int x1 = (std::min)(hardRight, base.x + base.width + expandRight);

    auto isHorizontalBorderWall = [&](int y) noexcept -> bool {
        if (y < 0 || y >= originalBgr.rows || x1 - x0 < 16) return false;
        const auto* row = originalBgr.ptr<cv::Vec3b>(y);
        int hits = 0;
        const int spanW = x1 - x0;
        for (int x = x0; x < x1; ++x) {
            if (pixelDiffFromPlane(row[x], style.bgPlane, x, y) >= 22) {
                ++hits;
            }
        }
        return (hits * 100 >= spanW * 76);
    };

    auto rowInkCount = [&](int y) noexcept -> int {
        if (y < 0 || y >= originalBgr.rows) return 0;
        const auto* row = originalBgr.ptr<cv::Vec3b>(y);
        int cnt = 0;
        for (int x = x0; x < x1; ++x) {
            if (pixelDiffFromPlane(row[x], style.bgPlane, x, y) >= inkThresh) {
                ++cnt;
            }
        }
        return cnt;
    };

    auto scanVert = [&](int startY, int step, int maxDist) noexcept -> int {
        if (maxDist <= 0) return 0;
        int best = 0;
        bool hitZero = false;
        bool hitWall = false;
        int consecZero = 0;
        for (int d = 1; d <= maxDist; ++d) {
            const int y = startY + step * d;
            if (isHorizontalBorderWall(y)) {
                hitWall = true;
                break;
            }
            const int c = rowInkCount(y);
            if (c == 0) {
                ++consecZero;
                hitZero = true;
                if (best > 0) {
                    best = d;
                    break;
                }
                if (consecZero >= 2) break;
                continue;
            }
            consecZero = 0;
            best = d;
        }
        if (!hitZero && !hitWall && best >= maxDist) {
            best = (std::min)(1, maxDist);
        }
        if (!hitWall && best < (std::min)(1, maxDist)) {
            if (!isHorizontalBorderWall(startY + step)) {
                best = 1;
            }
        }
        return best;
    };

    const int expandTop = scanVert(base.y, -1, base.y - hardTop);
    const int expandBottom = scanVert(base.y + base.height - 1, +1, hardBottom - (base.y + base.height));
    const int y0 = (std::max)(hardTop, base.y - expandTop);
    const int y1 = (std::min)(hardBottom, base.y + base.height + expandBottom);
    return cv::Rect(x0, y0, (std::max)(1, x1 - x0), (std::max)(1, y1 - y0)) & imgBounds;
}

void eraseParagraphBackground(
    cv::Mat& bgrCanvas,
    const cv::Mat& originalBgrSnapshot,
    const TranslationParagraph& para,
    const ExtractedTextStyle& style,
    const std::vector<cv::Rect>& protectedObstacles) {
    const cv::Rect imgBounds(0, 0, bgrCanvas.cols, bgrCanvas.rows);
    auto fillRectWithPlane = [&](const cv::Rect& roi) {
        const cv::Rect clamped = roi & imgBounds;
        if (clamped.width <= 0 || clamped.height <= 0) return;
        for (int y = clamped.y; y < clamped.y + clamped.height; ++y) {
            auto* rowPtr = bgrCanvas.ptr<cv::Vec3b>(y);
            for (int x = clamped.x; x < clamped.x + clamped.width; ++x) {
                rowPtr[x] = style.bgPlane.evalAt(static_cast<float>(x), static_cast<float>(y));
            }
        }
    };

    // 3D 渐变按钮 / 垂直光照渐变逐行保真擦除：
    // 当文字框左右紧邻背景在逐行扫描线上高度一致（L1 <= 42 占比 >= 75%），且存在显著垂直渐变或与全局拟合平面偏离时
    // （如经典 Win32/SAP 3D 黄色渐变按钮因上下高光/窗体底色污染全局平面），直接逐行左右线性插值并做 3-tap 垂直中值滤波，
    // 100% 丝滑保留原有按钮高光与梯度质感；普通纯色背景或一侧紧贴障碍物时零损耗回退 bgPlane。
    auto tryFillRectWithScanlineGradient = [&](const cv::Rect& roi) -> bool {
        const cv::Rect clamped = roi & imgBounds;
        if (clamped.width <= 2 || clamped.height < 10) return false;
        if (clamped.x < 3 || clamped.x + clamped.width + 3 > originalBgrSnapshot.cols) return false;

        const int lx0 = clamped.x - 3;
        const int lx1 = clamped.x - 2;
        const int rx0 = clamped.x + clamped.width + 1;
        const int rx1 = clamped.x + clamped.width + 2;
        const int h = clamped.height;

        std::vector<cv::Vec3b> leftRow(static_cast<size_t>(h));
        std::vector<cv::Vec3b> rightRow(static_cast<size_t>(h));
        int coherentRows = 0;
        int planeDevSum = 0;

        for (int i = 0; i < h; ++i) {
            const int y = clamped.y + i;
            const auto* srcRow = originalBgrSnapshot.ptr<cv::Vec3b>(y);
            const cv::Vec3b lPx(
                static_cast<uint8_t>((static_cast<int>(srcRow[lx0][0]) + srcRow[lx1][0]) / 2),
                static_cast<uint8_t>((static_cast<int>(srcRow[lx0][1]) + srcRow[lx1][1]) / 2),
                static_cast<uint8_t>((static_cast<int>(srcRow[lx0][2]) + srcRow[lx1][2]) / 2));
            const cv::Vec3b rPx(
                static_cast<uint8_t>((static_cast<int>(srcRow[rx0][0]) + srcRow[rx1][0]) / 2),
                static_cast<uint8_t>((static_cast<int>(srcRow[rx0][1]) + srcRow[rx1][1]) / 2),
                static_cast<uint8_t>((static_cast<int>(srcRow[rx0][2]) + srcRow[rx1][2]) / 2));
            leftRow[static_cast<size_t>(i)] = lPx;
            rightRow[static_cast<size_t>(i)] = rPx;

            const int lrDiff = std::abs(static_cast<int>(lPx[0]) - static_cast<int>(rPx[0])) +
                               std::abs(static_cast<int>(lPx[1]) - static_cast<int>(rPx[1])) +
                               std::abs(static_cast<int>(lPx[2]) - static_cast<int>(rPx[2]));
            if (lrDiff <= 42) {
                ++coherentRows;
            }
            const cv::Vec3b planePx = style.bgPlane.evalAt(
                static_cast<float>(clamped.x + clamped.width * 0.5f), static_cast<float>(y));
            const int midB = (static_cast<int>(lPx[0]) + rPx[0]) / 2;
            const int midG = (static_cast<int>(lPx[1]) + rPx[1]) / 2;
            const int midR = (static_cast<int>(lPx[2]) + rPx[2]) / 2;
            planeDevSum += std::abs(midB - static_cast<int>(planePx[0])) +
                           std::abs(midG - static_cast<int>(planePx[1])) +
                           std::abs(midR - static_cast<int>(planePx[2]));
        }

        if (coherentRows * 100 < h * 78) return false;

        // 计算顶部 25% 与底部 25% 颜色差异，判断是否真为垂直渐变（或左右真实背景与偏色平面显著偏离）
        const int band = (std::max)(2, h / 4);
        int topSum[3] = {0, 0, 0};
        int botSum[3] = {0, 0, 0};
        for (int i = 0; i < band; ++i) {
            for (int c = 0; c < 3; ++c) {
                topSum[c] += (static_cast<int>(leftRow[static_cast<size_t>(i)][c]) + rightRow[static_cast<size_t>(i)][c]) / 2;
                botSum[c] += (static_cast<int>(leftRow[static_cast<size_t>(h - 1 - i)][c]) + rightRow[static_cast<size_t>(h - 1 - i)][c]) / 2;
            }
        }
        const int vertGrad = std::abs(topSum[0] - botSum[0]) / band +
                             std::abs(topSum[1] - botSum[1]) / band +
                             std::abs(topSum[2] - botSum[2]) / band;
        const int avgPlaneDev = planeDevSum / h;
        if (vertGrad < 20 && avgPlaneDev < 26) {
            return false;
        }

        auto median3 = [](uint8_t a, uint8_t b, uint8_t c) noexcept -> uint8_t {
            if (a > b) std::swap(a, b);
            if (b > c) std::swap(b, c);
            if (a > b) std::swap(a, b);
            return b;
        };

        const float denom = static_cast<float>((std::max)(1, clamped.width - 1));
        for (int i = 0; i < h; ++i) {
            const size_t iPrev = static_cast<size_t>((std::max)(0, i - 1));
            const size_t iCur = static_cast<size_t>(i);
            const size_t iNext = static_cast<size_t>((std::min)(h - 1, i + 1));
            cv::Vec3b lFilt, rFilt;
            for (int c = 0; c < 3; ++c) {
                lFilt[c] = median3(leftRow[iPrev][c], leftRow[iCur][c], leftRow[iNext][c]);
                rFilt[c] = median3(rightRow[iPrev][c], rightRow[iCur][c], rightRow[iNext][c]);
            }
            auto* dstRow = bgrCanvas.ptr<cv::Vec3b>(clamped.y + i);
            for (int dx = 0; dx < clamped.width; ++dx) {
                const float t = static_cast<float>(dx) / denom;
                dstRow[clamped.x + dx] = cv::Vec3b(
                    static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(lFilt[0] + (rFilt[0] - lFilt[0]) * t)), 0, 255)),
                    static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(lFilt[1] + (rFilt[1] - lFilt[1]) * t)), 0, 255)),
                    static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(lFilt[2] + (rFilt[2] - lFilt[2]) * t)), 0, 255)));
            }
        }
        return true;
    };

    if (!style.complexBackground) {
        std::vector<cv::Rect> sealedLines;
        sealedLines.reserve(para.lineRects.size());
        for (const cv::Rect& rawLine : para.lineRects) {
            const cv::Rect sealed = computeInkSealedEraseRect(
                originalBgrSnapshot, rawLine, style, protectedObstacles);
            sealedLines.push_back(sealed);
            if (!tryFillRectWithScanlineGradient(sealed)) {
                fillRectWithPlane(sealed);
            }
        }
        // 多行正文段：顺带清理相邻两行重叠水平宽度之间的窄行距缝隙（防行内徽章圆角残影），
        // 绝不盲目平推整个 unionRect 凹角以保护嵌在凹角中的小徽章/图标（如右侧 New 徽章）
        for (size_t k = 1; k < sealedLines.size(); ++k) {
            const cv::Rect& prev = sealedLines[k - 1];
            const cv::Rect& cur = sealedLines[k];
            const int gapTop = prev.y + prev.height;
            const int gapBot = cur.y;
            if (gapBot > gapTop && gapBot - gapTop <= (std::max)(8, prev.height / 2)) {
                const int gx0 = (std::max)(prev.x, cur.x);
                const int gx1 = (std::min)(prev.x + prev.width, cur.x + cur.width);
                if (gx1 > gx0) {
                    fillRectWithPlane(cv::Rect(gx0, gapTop, gx1 - gx0, gapBot - gapTop));
                }
            }
        }
        return;
    }

    // 复杂纹理/照片背景：使用 0 显存 0.8ms 局部紧凑 Telea 无痕修复（绝不拉起 DirectML MI-GAN 避免主进程暴涨 ~800MB 内存）
    const cv::Rect padded = cv::Rect(
        para.unionRect.x - 12, para.unionRect.y - 12,
        para.unionRect.width + 24, para.unionRect.height + 24) & imgBounds;
    if (padded.width <= 2 || padded.height <= 2) return;

    cv::Mat mask = cv::Mat::zeros(padded.size(), CV_8UC1);
    for (const cv::Rect& rawLine : para.lineRects) {
        const cv::Rect rel = (cv::Rect(rawLine.x - 3, rawLine.y - 2, rawLine.width + 6, rawLine.height + 4) & padded);
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
    float firstLineBaseline = 0.0f;
    uint32_t lineCount = 0;
    bool overflowed = false;
    bool wrapped = false;
};

bool isCjkTargetLanguage(std::string_view targetLangCode) noexcept {
    return targetLangCode == "zh-CN" || targetLangCode == "zh-TW" ||
           targetLangCode == "ja" || targetLangCode == "ko";
}

/// 判断是否为不可拦腰劈断的单个无空格拉丁单词（如 "question"、"Cancel"、"Update"），
/// 防止 DirectWrite 在极窄容器内将其强行拆词折行成 "quest / ion"
bool isSingleUnbreakableLatinWord(const std::wstring& s) noexcept {
    if (s.size() < 2) return false;
    bool hasLatin = false;
    for (wchar_t ch : s) {
        if (std::iswspace(ch) || ch == L'-' || ch == L'/' || ch == L'\\') return false;
        if (isCjkWideChar(ch)) return false;
        const uint32_t cp = static_cast<uint32_t>(ch);
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x00C0 && cp <= 0x024F)) {
            hasLatin = true;
        }
    }
    return hasLatin;
}

/// 零字体变形排版内核（Typography Purity Law）：
/// 严禁任何非等比水平拉伸/压缩，恒定 1:1 原生字形比例 (DWRITE_FONT_STRETCH_NORMAL)；
/// 单行模式按真实可见字形墨水高度（不含 DirectWrite 35% 透明 leading）做溢出判定，彻底消除英文译中文缩水 25% 顽疾！
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
    const float safeMaxH = (std::max)(10.0f, maxHeight);
    const bool forbidWordSplit = isSingleUnbreakableLatinWord(text);
    const float heightCapFactor = singleLine ? 1.06f : 0.94f;
    const float hiInit = (std::max)(kMinReadableFontPx, (std::min)(preferredFontPx * kMaxFontScaleFactor, safeMaxH * heightCapFactor));
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
            // 使用 UNIFORM 精确行高 1.22*fontPx（替代 PROPORTIONAL 1.24 叠乘雅黑自带 1.34x 变成 1.66x 虚高行距），
            // 彻底根治双行紧凑卡片标题被冤枉判定溢出进而截断省略号、以及大标题被迫缩字的顽疾
            fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, fontPx * 1.22f, fontPx * 0.96f);
        }
        if (attachEllipsis) {
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(fmt.Get(), ellipsis.GetAddressOf())) && ellipsis) {
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                fmt->SetTrimming(&trim, ellipsis.Get());
            }
        }

        ComPtr<IDWriteTextLayout> layout;
        const float boxH = allowWrap ? (attachEllipsis ? safeMaxH : safeMaxH * 3.0f) : safeMaxH * 2.5f;
        if (FAILED(dwrite->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()),
                fmt.Get(), safeMaxW, boxH, layout.GetAddressOf())) || !layout) {
            return r;
        }
        DWRITE_TEXT_METRICS m{};
        if (FAILED(layout->GetMetrics(&m))) return r;

        DWRITE_LINE_METRICS lineMetric{};
        UINT32 actualLines = 0;
        if (SUCCEEDED(layout->GetLineMetrics(&lineMetric, 1, &actualLines)) && actualLines >= 1) {
            r.firstLineBaseline = lineMetric.baseline;
        } else {
            r.firstLineBaseline = fontPx * 0.88f;
        }

        r.layout = std::move(layout);
        r.textWidth = m.widthIncludingTrailingWhitespace;
        r.textHeight = m.height;
        r.lineCount = m.lineCount;

        // 单行可见字形墨水高度约等于 1.02 * fontPx（排除 DirectWrite 上下 ~35% 透明行距 leading），
        // 避免拿含大片透明空白的 m.height 死卡 OCR 紧包框而冤枉把 13px 中文压成 10px！
        const bool isEffectiveSingleLine = (!allowWrap || m.lineCount <= 1);
        const float effectiveHeight = isEffectiveSingleLine
            ? (std::min)(m.height, fontPx * 1.02f)
            : m.height;
        const bool brokenSingleWord = forbidWordSplit && allowWrap && (m.lineCount > 1);
        r.overflowed = brokenSingleWord || (effectiveHeight > safeMaxH + 1.5f) || (m.widthIncludingTrailingWhitespace > safeMaxW + 1.0f);
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
        if (forbidWordSplit || bestSingle.fontPx >= bestWrap.fontPx * 0.88f) {
            return bestSingle;
        }
        return bestWrap;
    }
    if (bestSingle.layout) return bestSingle;
    if (bestWrap.layout) return bestWrap;

    // 极端超长兜底：保持 1:1 原生字形不变形，采用最小可读等比字号 + 省略号优雅裁切，绝不压盖邻行且绝不拦腰劈断单英文词
    const bool fallbackWrap = !forbidWordSplit && (!singleLine || (safeMaxH >= kMinReadableFontPx * 2.35f));
    FittedLayoutResult fallback = createLayout(loInit, fallbackWrap, true);
    fallback.overflowed = true;
    return fallback;
}

DWRITE_FONT_WEIGHT resolveRenderFontWeight(
    int extractedWeight,
    float fontPx,
    std::string_view targetLangCode) noexcept {
    // 防止 11~14px 细英文因次像素抗锯齿墨水占比偏高被误判为刺眼粗黑体：
    // 仅对大标题或高置信度加粗使用 BOLD；常规 CJK 正文恒定清晰锐利的 MEDIUM(500) / REGULAR(400)
    if (extractedWeight >= 700 && fontPx >= 15.0f) {
        return DWRITE_FONT_WEIGHT_BOLD;
    }
    if (extractedWeight >= 600 && fontPx >= 14.0f) {
        return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    }
    if (isCjkTargetLanguage(targetLangCode)) {
        return (extractedWeight >= 650 && fontPx >= 13.0f)
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

    float baseFontPx = std::clamp(style.fontSizePx, 9.0f, 72.0f);
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
    const bool effectiveSingleLine = (para.isSingleLine && (!fitted.wrapped || fitted.lineCount <= 1));

    // 单行在严格不突破上下障碍物/横线的前提下，向安全洁净区域借用最多 4px 透明绘制缓冲垫，
    // 彻底杜绝 DirectWrite 上下抗锯齿与字形边缘被 PushAxisAlignedClip 腰斩截字（透明像素 alpha=0 绝不改动背景）
    cv::Rect paintRoi = layoutRect & imgBounds;
    if (effectiveSingleLine) {
        const int padTopY = (std::max)(layoutBox.safeTopY, layoutRect.y - 4);
        const int padBotY = (std::min)(layoutBox.safeBottomY, layoutRect.y + layoutRect.height + 4);
        paintRoi = cv::Rect(
            layoutRect.x,
            padTopY,
            layoutRect.width,
            (std::max)(layoutRect.height, padBotY - padTopY)) & imgBounds;
    }
    if (paintRoi.width <= 1 || paintRoi.height <= 1) return {};

    ComPtr<IWICBitmap> wicBmp;
    if (FAILED(fac.wic->CreateBitmap(
            static_cast<UINT>(paintRoi.width), static_cast<UINT>(paintRoi.height),
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

    // 计算精确光学垂直原点（单行把真实字形墨水中心精确对准原图文字墨水中心，消除上下错落与截字）
    const bool cjkTarget = isCjkTargetLanguage(targetLangCode);
    const float inkTopLocal = fitted.firstLineBaseline - fitted.fontPx * (cjkTarget ? 0.84f : 0.76f);
    const float inkBotLocal = fitted.firstLineBaseline + fitted.fontPx * (cjkTarget ? 0.16f : 0.24f);
    float offsetY = 0.0f;
    if (effectiveSingleLine) {
        const float glyphInkCenterLocalY = 0.5f * (inkTopLocal + inkBotLocal);
        const float origCenterY = (style.inkBottomY > style.inkTopY + 3.0f)
            ? (0.5f * (style.inkTopY + style.inkBottomY))
            : (static_cast<float>(para.unionRect.y) + static_cast<float>(para.unionRect.height) * 0.5f);
        offsetY = (origCenterY - static_cast<float>(paintRoi.y)) - glyphInkCenterLocalY;
        if (offsetY + inkBotLocal > static_cast<float>(paintRoi.height) - 0.5f) {
            offsetY = static_cast<float>(paintRoi.height) - 0.5f - inkBotLocal;
        }
        if (offsetY + inkTopLocal < 0.5f) {
            offsetY = 0.5f - inkTopLocal;
        }
    } else if (fitted.textHeight < static_cast<float>(paintRoi.height)) {
        offsetY = (std::min)(4.0f, (static_cast<float>(paintRoi.height) - fitted.textHeight) * 0.20f);
    }

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    rt->PushAxisAlignedClip(
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(paintRoi.width), static_cast<float>(paintRoi.height)),
        D2D1_ANTIALIAS_MODE_ALIASED);

    if (style.complexBackground) {
        // 复杂照片背景圆角托底紧贴实际译文排版包围盒（告别安全域空长尾巴拖影）
        ComPtr<ID2D1SolidColorBrush> bgBrush;
        rt->CreateSolidColorBrush(
            D2D1::ColorF(
                static_cast<float>(centerBg[2]) / 255.0f,
                static_cast<float>(centerBg[1]) / 255.0f,
                static_cast<float>(centerBg[0]) / 255.0f,
                0.76f),
            bgBrush.GetAddressOf());
        if (bgBrush) {
            const float textLeft = layoutBox.centerHoriz
                ? (std::max)(0.0f, (static_cast<float>(paintRoi.width) - fitted.textWidth) * 0.5f)
                : 0.0f;
            const float bgL = (std::max)(0.5f, textLeft - 5.0f);
            const float bgR = (std::min)(static_cast<float>(paintRoi.width) - 0.5f, textLeft + fitted.textWidth + 5.0f);
            const float bgT = effectiveSingleLine
                ? (std::max)(0.5f, offsetY + inkTopLocal - 3.0f)
                : (std::max)(0.5f, offsetY - 2.0f);
            const float bgB = effectiveSingleLine
                ? (std::min)(static_cast<float>(paintRoi.height) - 0.5f, offsetY + inkBotLocal + 3.0f)
                : (std::min)(static_cast<float>(paintRoi.height) - 0.5f, offsetY + fitted.textHeight + 2.0f);
            if (bgR > bgL + 2.0f && bgB > bgT + 2.0f) {
                const float r = (std::min)(5.0f, (std::min)(bgR - bgL, bgB - bgT) * 0.22f);
                const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(bgL, bgT, bgR, bgB), r, r);
                rt->FillRoundedRectangle(rr, bgBrush.Get());
            }
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
        rt->DrawTextLayout(
            D2D1::Point2F(0.0f, offsetY),
            fitted.layout.Get(),
            textBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }

    rt->PopAxisAlignedClip();
    if (FAILED(rt->EndDraw())) return {};

    // 将 Premultiplied BGRA 精准 Alpha 混合写回 bgrCanvas
    WICRect lockRc{0, 0, paintRoi.width, paintRoi.height};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(wicBmp->Lock(&lockRc, WICBitmapLockRead, lock.GetAddressOf())) || !lock) return {};

    UINT bufSize = 0;
    UINT stride = 0;
    BYTE* srcData = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&bufSize, &srcData)) || !srcData) {
        return {};
    }

    for (int y = 0; y < paintRoi.height; ++y) {
        const BYTE* sRow = srcData + static_cast<size_t>(y) * stride;
        auto* dRow = bgrCanvas.ptr<cv::Vec3b>(paintRoi.y + y) + paintRoi.x;
        for (int x = 0; x < paintRoi.width; ++x) {
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
    return layoutRect & imgBounds;
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

/// 将 UTF-8 字符串按码点切分，同时记录每个码点在原字符串中的起止字节偏移（供与 charBoxes 1:1 精确同步）
struct Utf8CpSpan {
    size_t byteStart = 0;
    size_t byteLen = 0;
};

std::vector<Utf8CpSpan> splitUtf8CodepointSpans(std::string_view s) {
    std::vector<Utf8CpSpan> spans;
    spans.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) len = s.size() - i;
        spans.push_back(Utf8CpSpan{i, len});
        i += len;
    }
    return spans;
}

cv::Rect unionRectOfCharBoxes(const std::vector<cv::Rect>& boxes, size_t startIdx, size_t endIdxExclusive) noexcept {
    if (startIdx >= endIdxExclusive || endIdxExclusive > boxes.size()) return {};
    int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
    for (size_t i = startIdx; i < endIdxExclusive; ++i) {
        const auto& b = boxes[i];
        if (b.width <= 0 || b.height <= 0) continue;
        x1 = (std::min)(x1, b.x);
        y1 = (std::min)(y1, b.y);
        x2 = (std::max)(x2, b.x + b.width);
        y2 = (std::max)(y2, b.y + b.height);
    }
    if (x1 >= x2 || y1 >= y2) return {};
    return cv::Rect(x1, y1, x2 - x1, y2 - y1);
}

/// 判断是否为右侧独立紧凑时间/状态元标记（如 8d / 12d / 38m / 21h / 5s / 2w / 3y）
bool isCompactTrailingMetaToken(std::string_view token) noexcept {
    if (token.size() < 2 || token.size() > 5) return false;
    size_t i = 0;
    while (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i]))) ++i;
    if (i == 0 || i > 3 || i + 1 != token.size()) return false;
    const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(token[i])));
    return unit == 's' || unit == 'm' || unit == 'h' || unit == 'd' || unit == 'w' || unit == 'y';
}

/// 从单行 OCR 块尾部剥离因超长标题逼近而横向粘连的右对齐紧凑时间戳（如 "...Artwork C...  8d"）
bool detachTrailingMetaFromBlock(PinOcrBlock& blk, cv::Rect& outDetachedRect) {
    outDetachedRect = {};
    const std::string& txt = blk.text;
    if (txt.size() < 10) return false;

    const auto spans = splitUtf8CodepointSpans(txt);
    if (spans.size() < 6) return false;

    size_t cpEnd = spans.size();
    while (cpEnd > 0 && spans[cpEnd - 1].byteLen == 1 &&
           std::isspace(static_cast<unsigned char>(txt[spans[cpEnd - 1].byteStart]))) {
        --cpEnd;
    }
    if (cpEnd < 4) return false;

    size_t tailStartCp = cpEnd;
    while (tailStartCp > 0 && !(spans[tailStartCp - 1].byteLen == 1 &&
           std::isspace(static_cast<unsigned char>(txt[spans[tailStartCp - 1].byteStart])))) {
        --tailStartCp;
    }
    if (tailStartCp == 0 || tailStartCp >= cpEnd) return false;

    const size_t tailByteStart = spans[tailStartCp].byteStart;
    const size_t tailByteEnd = spans[cpEnd - 1].byteStart + spans[cpEnd - 1].byteLen;
    const std::string_view tailSv = std::string_view(txt).substr(tailByteStart, tailByteEnd - tailByteStart);
    if (!isCompactTrailingMetaToken(tailSv)) return false;

    size_t leftEndCp = tailStartCp;
    size_t spaceCount = 0;
    while (leftEndCp > 0 && spans[leftEndCp - 1].byteLen == 1 &&
           std::isspace(static_cast<unsigned char>(txt[spans[leftEndCp - 1].byteStart]))) {
        --leftEndCp;
        ++spaceCount;
    }
    if (leftEndCp < 3) return false;

    const size_t leftByteEnd = spans[leftEndCp - 1].byteStart + spans[leftEndCp - 1].byteLen;
    const std::string leftText = trimWhitespace(std::string_view(txt).substr(0, leftByteEnd));
    if (leftText.empty()) return false;

    const bool leftTruncatedByEllipsis =
        (leftText.size() >= 2 && leftText.substr(leftText.size() - 2) == "..") ||
        (leftText.size() >= 3 && leftText.substr(leftText.size() - 3) == "\xE2\x80\xA6");

    const bool hasExactCharBoxes = (blk.charBoxes.size() == spans.size());
    int pixelGap = 0;
    if (hasExactCharBoxes) {
        const auto& prevBox = blk.charBoxes[leftEndCp - 1];
        const auto& tailBox = blk.charBoxes[tailStartCp];
        pixelGap = tailBox.x - (prevBox.x + prevBox.width);
    }
    const bool wideVisualGap =
        (spaceCount >= 2) ||
        (leftTruncatedByEllipsis && spaceCount >= 1) ||
        (hasExactCharBoxes && pixelGap >= (std::max)(8, static_cast<int>(blk.origBox.height * 0.65f)));
    if (!wideVisualGap) return false;

    if (hasExactCharBoxes) {
        const cv::Rect leftRect = unionRectOfCharBoxes(blk.charBoxes, 0, leftEndCp);
        const cv::Rect rightRect = unionRectOfCharBoxes(blk.charBoxes, tailStartCp, cpEnd);
        if (leftRect.width > 0 && leftRect.height > 0 && rightRect.width > 0 && rightRect.height > 0) {
            blk.text = leftText;
            blk.origBox = leftRect;
            blk.charBoxes.assign(blk.charBoxes.begin(), blk.charBoxes.begin() + static_cast<ptrdiff_t>(leftEndCp));
            outDetachedRect = rightRect;
            return true;
        }
    }

    // 无精确 charBoxes 时按比例安全切分
    const float ratio = static_cast<float>(leftEndCp) / static_cast<float>(cpEnd);
    const int leftW = (std::max)(4, static_cast<int>(std::lround(blk.origBox.width * ratio)));
    const int tailW = (std::max)(4, static_cast<int>(std::lround(blk.origBox.width * (static_cast<float>(cpEnd - tailStartCp) / static_cast<float>(cpEnd)))));
    outDetachedRect = cv::Rect(
        (std::max)(blk.origBox.x + leftW + 2, blk.origBox.x + blk.origBox.width - tailW),
        blk.origBox.y,
        tailW,
        blk.origBox.height);
    blk.origBox.width = (std::min)(blk.origBox.width, (std::max)(4, outDetachedRect.x - blk.origBox.x - 4));
    blk.text = leftText;
    blk.charBoxes.clear();
    return true;
}

/// 剥离同行相邻 OCR 块因切片微重叠产生的头部重复尾字母（如左块 "...Voted" 与右块 "ed  Precinct Maps"）
void dedupHorizontalOverlapPrefix(const PinOcrBlock& prev, PinOcrBlock& cur) {
    if (prev.layoutRegionId != cur.layoutRegionId) return;
    const int vOverlap = (std::min)(prev.origBox.y + prev.origBox.height, cur.origBox.y + cur.origBox.height) -
                         (std::max)(prev.origBox.y, cur.origBox.y);
    const int minH = (std::min)(prev.origBox.height, cur.origBox.height);
    if (minH <= 0 || vOverlap * 10 < minH * 6) return;

    const int hOverlap = (prev.origBox.x + prev.origBox.width) - cur.origBox.x;
    if (hOverlap <= 0 || hOverlap > 36) return;

    const std::string& ct = cur.text;
    size_t spPos = ct.find("  ");
    if (spPos == std::string::npos || spPos == 0 || spPos > 3) return;

    const std::string_view prefix = std::string_view(ct).substr(0, spPos);
    for (unsigned char c : prefix) {
        if (!std::isalpha(c)) return;
    }
    const std::string pt = trimWhitespace(prev.text);
    if (pt.size() < prefix.size()) return;
    const std::string_view prevSuffix = std::string_view(pt).substr(pt.size() - prefix.size());
    for (size_t k = 0; k < prefix.size(); ++k) {
        if (std::tolower(static_cast<unsigned char>(prefix[k])) !=
            std::tolower(static_cast<unsigned char>(prevSuffix[k]))) {
            return;
        }
    }

    size_t newStart = spPos;
    while (newStart < ct.size() && ct[newStart] == ' ') ++newStart;
    if (newStart >= ct.size()) return;

    const auto spans = splitUtf8CodepointSpans(ct);
    size_t newStartCp = 0;
    while (newStartCp < spans.size() && spans[newStartCp].byteStart < newStart) ++newStartCp;
    if (newStartCp >= spans.size()) return;

    if (cur.charBoxes.size() == spans.size()) {
        const cv::Rect subRect = unionRectOfCharBoxes(cur.charBoxes, newStartCp, spans.size());
        if (subRect.width > 0 && subRect.height > 0) {
            cur.origBox = subRect;
            cur.charBoxes.erase(cur.charBoxes.begin(), cur.charBoxes.begin() + static_cast<ptrdiff_t>(newStartCp));
        }
    } else {
        const int shiftX = (std::min)(cur.origBox.width - 4, (std::max)(hOverlap, 4));
        cur.origBox.x += shiftX;
        cur.origBox.width -= shiftX;
        cur.charBoxes.clear();
    }
    cur.text = ct.substr(newStart);
}

bool isPureAsciiLatinText(std::string_view utf8) noexcept {
    bool hasAlpha = false;
    for (unsigned char c : utf8) {
        if (c >= 0x80) return false;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) hasAlpha = true;
    }
    return hasAlpha;
}

bool containsTargetLangGlyphs(std::string_view utf8, std::string_view targetLangCode) noexcept {
    const std::wstring w = tools3000::core::WinUtils::utf8ToWstring(std::string(utf8));
    if (isCjkTargetLanguage(targetLangCode)) {
        for (wchar_t ch : w) {
            if (isCjkWideChar(ch)) return true;
        }
        return false;
    }
    for (wchar_t ch : w) {
        const uint32_t cp = static_cast<uint32_t>(ch);
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x0400 && cp <= 0x04FF)) {
            return true;
        }
    }
    return false;
}

/// 精准逐行视觉 DNA 采样：多行段落绝不把整段多行字符串传给单行矩形，彻底根治大标题字号/字重腰斩，
/// 同时施加几何高度物理上限防图标污染暴走，并对英文->CJK 引入克制等高补偿
ExtractedTextStyle extractParagraphStyle(
    const cv::Mat& originalBgrSnapshot,
    const TranslationParagraph& para,
    std::string_view targetLangCode) {
    const bool hasPerLineMeta =
        !para.lineRects.empty() &&
        para.lineTexts.size() == para.lineRects.size() &&
        para.lineCharBoxes.size() == para.lineRects.size();

    ExtractedTextStyle style{};
    if (!hasPerLineMeta) {
        const cv::Rect sampleRect = para.lineRects.empty() ? para.unionRect : para.lineRects.front();
        style = TextReplaceEngine::extractStyle(
            originalBgrSnapshot, sampleRect, para.charBoxes, para.originalText);
    } else if (para.lineRects.size() == 1) {
        style = TextReplaceEngine::extractStyle(
            originalBgrSnapshot, para.lineRects[0], para.lineCharBoxes[0], para.lineTexts[0]);
    } else {
        std::vector<ExtractedTextStyle> lineStyles;
        lineStyles.reserve((std::min<size_t>)(3u, para.lineRects.size()));
        for (size_t k = 0; k < para.lineRects.size() && lineStyles.size() < 3u; ++k) {
            if (countVisualChars(para.lineTexts[k]) < 3 && para.lineRects.size() > 1 && k + 1 < para.lineRects.size()) {
                continue;
            }
            lineStyles.push_back(TextReplaceEngine::extractStyle(
                originalBgrSnapshot, para.lineRects[k], para.lineCharBoxes[k], para.lineTexts[k]));
        }
        if (lineStyles.empty()) {
            lineStyles.push_back(TextReplaceEngine::extractStyle(
                originalBgrSnapshot, para.lineRects[0], para.lineCharBoxes[0], para.lineTexts[0]));
        }
        style = lineStyles.front();
        if (lineStyles.size() >= 2) {
            std::vector<float> ems;
            std::vector<int> weights;
            ems.reserve(lineStyles.size());
            weights.reserve(lineStyles.size());
            for (const auto& ls : lineStyles) {
                ems.push_back(ls.fontSizePx);
                weights.push_back(ls.fontWeight);
            }
            std::sort(ems.begin(), ems.end());
            std::sort(weights.begin(), weights.end());
            style.fontSizePx = ems[ems.size() / 2];
            style.fontWeight = weights[weights.size() / 2];
        }
    }

    // 物理几何上限护栏：任何正常文字字号绝不可能远超其紧包 OCR 行高（防止左侧彩色圆点/徽章底色拉爆字号）
    int repLineH = para.unionRect.height;
    if (!para.lineRects.empty()) {
        std::vector<int> hs;
        hs.reserve(para.lineRects.size());
        for (const auto& lr : para.lineRects) hs.push_back(lr.height);
        std::sort(hs.begin(), hs.end());
        repLineH = hs[hs.size() / 2];
    }
    if (repLineH >= 8) {
        const size_t visualChars = countVisualChars(para.originalText);
        const float maxAllowedEm = (visualChars <= 4)
            ? (static_cast<float>(repLineH) * 0.94f)
            : (static_cast<float>(repLineH) * 1.06f);
        if (style.fontSizePx > maxAllowedEm) {
            style.fontSizePx = (std::max)(9.0f, maxAllowedEm);
        }

        // 英文 -> CJK 视觉等高克制补偿：同 CSS 字号下拉丁无下伸部紧包框反解出的 em 常比同屏原生中文低 ~12%
        if (isCjkTargetLanguage(targetLangCode) &&
            isPureAsciiLatinText(para.originalText) &&
            visualChars >= 4 &&
            repLineH >= 12 &&
            style.fontSizePx < static_cast<float>(repLineH) * 0.84f) {
            style.fontSizePx = (std::min)(
                static_cast<float>(repLineH) * 0.86f,
                style.fontSizePx * 1.14f);
        }
    }

    return style;
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
    size_t nonLatinAlphaOrCjk = 0;
    size_t digits = 0;
    size_t curLatinRun = 0;
    size_t maxLatinWordLen = 0;
    for (wchar_t ch : w) {
        const uint32_t cp = static_cast<uint32_t>(ch);
        const bool isArabicDigit = (cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9);
        const bool isThaiDigit = (cp >= 0x0E50 && cp <= 0x0E59);
        const bool isArabicLetter =
            !isArabicDigit &&
            ((cp >= 0x0600 && cp <= 0x06FF) ||
             (cp >= 0x0750 && cp <= 0x077F) ||
             (cp >= 0x08A0 && cp <= 0x08FF) ||
             (cp >= 0xFB50 && cp <= 0xFDFF) ||
             (cp >= 0xFE70 && cp <= 0xFEFF));
        const bool isThaiLetter = !isThaiDigit && (cp >= 0x0E00 && cp <= 0x0E7F);
        const bool isOtherNonLatinLetter =
            isCjkWideChar(ch) ||
            (cp >= 0x0370 && cp <= 0x03FF) || // Greek & Coptic
            (cp >= 0x0400 && cp <= 0x052F) || // Cyrillic + Supplement
            (cp >= 0x0530 && cp <= 0x05FF) || // Armenian + Hebrew
            isArabicLetter ||
            (cp >= 0x0900 && cp <= 0x0DFF) || // Indic scripts (Devanagari..Sinhala)
            isThaiLetter ||
            (cp >= 0x0E80 && cp <= 0x10FF) || // Lao, Tibetan, Myanmar, Georgian
            (cp >= 0x1780 && cp <= 0x17FF) || // Khmer
            (cp >= 0x1F00 && cp <= 0x1FFF);   // Greek Extended
        const bool isLatinLetter =
            (cp >= 'A' && cp <= 'Z') ||
            (cp >= 'a' && cp <= 'z') ||
            (cp >= 0x00C0 && cp <= 0x024F) || // Latin-1 Supplement + Extended-A/B
            (cp >= 0x1E00 && cp <= 0x1EFF);   // Latin Extended Additional (Vietnamese)

        if (isOtherNonLatinLetter) {
            ++alphaOrCjk;
            ++nonLatinAlphaOrCjk;
            curLatinRun = 0;
        } else if (isLatinLetter) {
            ++alphaOrCjk;
            ++curLatinRun;
            if (curLatinRun > maxLatinWordLen) maxLatinWordLen = curLatinRun;
        } else if (cp >= 0x0300 && cp <= 0x036F) {
            // 组合变音符号（NFD 越南语/法语/德语重音）不打断拉丁词连续计数
        } else if ((cp >= '0' && cp <= '9') || isArabicDigit || isThaiDigit) {
            ++digits;
            curLatinRun = 0;
        } else {
            curLatinRun = 0;
        }
    }
    if (alphaOrCjk == 0) return false;
    // 跳过不含任何多字母单词的孤立单字母图标噪声与统计代号（如放大镜误识 "Q"、"s"、"257 D, 178 R"）
    if (nonLatinAlphaOrCjk == 0 && maxLatinWordLen < 2) return false;
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

    struct WorkingBlock {
        PinOcrBlock blk;
        cv::Rect detachedMetaBox;
    };
    std::vector<WorkingBlock> activeBlocks;
    activeBlocks.reserve(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (useFilter && !filterSet.contains(i)) continue;
        PinOcrBlock copy = blocks[i];
        copy.origBox = copy.origBox & imgBounds;
        if (copy.origBox.width <= 1 || copy.origBox.height <= 1) continue;

        cv::Rect detachedMeta{};
        detachTrailingMetaFromBlock(copy, detachedMeta);
        detachedMeta = detachedMeta & imgBounds;
        if (!isTranslatableText(copy.text)) continue;
        copy.origBox = copy.origBox & imgBounds;
        if (copy.origBox.width <= 1 || copy.origBox.height <= 1) continue;
        activeBlocks.push_back(WorkingBlock{std::move(copy), detachedMeta});
    }
    if (activeBlocks.empty()) return result;

    // 按 layoutRegionId -> Y -> X 阅读顺序稳定排序
    std::stable_sort(activeBlocks.begin(), activeBlocks.end(), [](const WorkingBlock& a, const WorkingBlock& b) {
        const auto& ba = a.blk;
        const auto& bb = b.blk;
        if (ba.layoutRegionId != bb.layoutRegionId) return ba.layoutRegionId < bb.layoutRegionId;
        const int yTol = (std::max)(4, (std::min)(ba.origBox.height, bb.origBox.height) / 2);
        if (std::abs(ba.origBox.y - bb.origBox.y) > yTol) return ba.origBox.y < bb.origBox.y;
        return ba.origBox.x < bb.origBox.x;
    });

    // 同行水平微重叠块去重（如 "...Voted" 与 "ed  Precinct Maps"）
    for (size_t i = 1; i < activeBlocks.size(); ++i) {
        dedupHorizontalOverlapPrefix(activeBlocks[i - 1].blk, activeBlocks[i].blk);
    }

    for (auto& wb : activeBlocks) {
        const auto& blk = wb.blk;
        const cv::Rect clampedBox = blk.origBox & imgBounds;
        if (clampedBox.width <= 1 || clampedBox.height <= 1) continue;
        if (!isTranslatableText(blk.text)) continue;

        // 多栏回溯查找最佳父段（最多倒序回溯同 layoutRegionId 的近 12 个段落），
        // 彻底根治并排卡片/右侧坐标轴/徽章插队导致的自然段落撕裂
        size_t bestParentIdx = result.size();
        int bestGapY = INT_MAX;
        bool bestMergedByCenter = false;
        const size_t scanLimit = (std::min<size_t>)(result.size(), 12u);
        for (size_t offset = 0; offset < scanLimit; ++offset) {
            const size_t candIdx = result.size() - 1 - offset;
            const auto& cand = result[candIdx];
            if (cand.layoutRegionId != blk.layoutRegionId) break;
            const cv::Rect& lastRc = cand.lineRects.back();
            const int gapY = clampedBox.y - (lastRc.y + lastRc.height);
            if (gapY > (std::max)(clampedBox.height, lastRc.height) * 3) continue;
            bool byCenter = false;
            if (canMergeIntoParagraph(cand, blk, sourceImage, &byCenter)) {
                if (gapY < bestGapY) {
                    bestGapY = gapY;
                    bestParentIdx = candIdx;
                    bestMergedByCenter = byCenter;
                }
            }
        }

        if (bestParentIdx < result.size()) {
            auto& cur = result[bestParentIdx];
            cur.unionRect |= clampedBox;
            cur.lineRects.push_back(clampedBox);
            cur.lineTexts.push_back(trimWhitespace(blk.text));
            cur.lineCharBoxes.push_back(blk.charBoxes);
            cur.charBoxes.insert(cur.charBoxes.end(), blk.charBoxes.begin(), blk.charBoxes.end());
            if (wb.detachedMetaBox.width > 0 && wb.detachedMetaBox.height > 0) {
                cur.detachedObstacleRects.push_back(wb.detachedMetaBox);
            }
            cur.isSingleLine = false;
            if (bestMergedByCenter) cur.preferCentered = true;
            appendLineTextToParagraph(cur, blk.text);
        } else {
            TranslationParagraph para{};
            para.unionRect = clampedBox;
            para.lineRects.push_back(clampedBox);
            para.lineTexts.push_back(trimWhitespace(blk.text));
            para.lineCharBoxes.push_back(blk.charBoxes);
            para.charBoxes = blk.charBoxes;
            if (wb.detachedMetaBox.width > 0 && wb.detachedMetaBox.height > 0) {
                para.detachedObstacleRects.push_back(wb.detachedMetaBox);
            }
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

    // 合并所有外部障碍物与从粘连行中剥离出的尾部独立元信息框（如右对齐时间戳 8d）
    std::vector<cv::Rect> allObstacles = extraObstacleRects;
    for (const auto& para : paragraphs) {
        for (const auto& det : para.detachedObstacleRects) {
            if (det.width > 0 && det.height > 0) {
                allObstacles.push_back(det);
            }
        }
    }

    // 先从干净原图逐行精准提取全部段落视觉 DNA，避免前一段擦除/渲染影响相邻段采样
    std::vector<ExtractedTextStyle> styles;
    styles.reserve(paragraphs.size());
    for (const auto& para : paragraphs) {
        styles.push_back(extractParagraphStyle(originalBgrSnapshot, para, targetLangCode));
    }

    std::vector<bool> activeChanged(paragraphs.size(), false);
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        activeChanged[i] = isTranslationEffectivelyChanged(paragraphs[i].originalText, paragraphs[i].translatedText);
    }

    // 收集擦除时需要严格避让的未翻译原图障碍物（含未发生变化的保留原图段落、外部障碍物、二维码与剥离时间戳）
    std::vector<cv::Rect> protectedEraseObstacles;
    protectedEraseObstacles.reserve(allObstacles.size() + paragraphs.size());
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        if (!activeChanged[i]) {
            protectedEraseObstacles.push_back(paragraphs[i].unionRect);
        }
    }
    for (const cv::Rect& obs : allObstacles) {
        bool coveredByChangedPara = false;
        for (size_t i = 0; i < paragraphs.size(); ++i) {
            if (!activeChanged[i]) continue;
            const cv::Rect inter = paragraphs[i].unionRect & obs;
            if (inter.area() > 0 && inter.area() * 2 >= obs.area()) {
                coveredByChangedPara = true;
                break;
            }
        }
        if (!coveredByChangedPara) {
            protectedEraseObstacles.push_back(obs);
        }
    }

    // 阶段 1：4 向墨水连通封顶精准擦除（仅擦除真正产生译文变化的段落，彻底消灭首尾漏框毛边）
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        if (!activeChanged[i]) continue;
        eraseParagraphBackground(bgrCanvas, originalBgrSnapshot, paragraphs[i], styles[i], protectedEraseObstacles);
    }

    // 阶段 1.5：同列未翻译目标语言兄弟行字号锚定（例如中英混排列表中以同列原生中文作为最权威的字号标尺）
    struct NativeColumnAnchor {
        int x = 0;
        int lineH = 0;
        float fgLum = 0.0f;
        float fontSizePx = 0.0f;
    };
    std::vector<NativeColumnAnchor> nativeAnchors;
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        if (activeChanged[i]) continue;
        if (!containsTargetLangGlyphs(paragraphs[i].originalText, targetLangCode)) continue;
        const int lh = paragraphs[i].lineRects.empty()
            ? paragraphs[i].unionRect.height
            : paragraphs[i].lineRects.front().height;
        if (lh < 10 || lh > 36) continue;
        nativeAnchors.push_back(NativeColumnAnchor{
            paragraphs[i].unionRect.x,
            lh,
            relativeLuminance(styles[i].textBgr),
            styles[i].fontSizePx
        });
    }
    if (!nativeAnchors.empty()) {
        for (size_t i = 0; i < paragraphs.size(); ++i) {
            if (!activeChanged[i] || !paragraphs[i].isSingleLine) continue;
            const int lh = paragraphs[i].lineRects.empty()
                ? paragraphs[i].unionRect.height
                : paragraphs[i].lineRects.front().height;
            const float lum = relativeLuminance(styles[i].textBgr);
            std::vector<float> matchedEms;
            for (const auto& na : nativeAnchors) {
                if (std::abs(na.x - paragraphs[i].unionRect.x) <= 10 &&
                    na.lineH >= static_cast<int>(lh * 0.80f) &&
                    na.lineH <= static_cast<int>(lh * 1.25f) &&
                    std::abs(na.fgLum - lum) <= 0.16f) {
                    matchedEms.push_back(na.fontSizePx);
                }
            }
            if (matchedEms.size() >= 2) {
                std::sort(matchedEms.begin(), matchedEms.end());
                const float anchorEm = matchedEms[matchedEms.size() / 2];
                const float maxSafe = static_cast<float>(lh) * 0.96f;
                styles[i].fontSizePx = (std::min)(maxSafe, (std::max)(styles[i].fontSizePx, anchorEm));
            }
        }
    }

    // 阶段 2：计算全部有效段落的安全避障域（含格式塔列守恒对齐与细边框刹车）与 1:1 原生拟合字号，
    // 再做层级保护的同级正文簇平滑归一（绝不把粗黑标题压成浅灰小正文！）
    std::vector<SafeLayoutBox> safeBoxes(paragraphs.size());
    std::vector<float> fittedFontSizes(paragraphs.size(), 0.0f);
    auto& fac = threadFactories();
    const bool facReady = fac.init();

    for (size_t i = 0; i < paragraphs.size(); ++i) {
        if (!activeChanged[i]) continue;
        safeBoxes[i] = computeSafeLayoutRect(
            originalBgrSnapshot, paragraphs[i], i, paragraphs, allObstacles, styles[i], imgSize);
        if (facReady) {
            const std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(trimWhitespace(paragraphs[i].translatedText));
            if (!wText.empty()) {
                const wchar_t* fontFamily = resolveTranslateFontFamily(fac.dwrite.Get(), styles[i].fontClass, targetLangCode);
                const wchar_t* localeName = resolveDWriteLocale(targetLangCode);
                const float basePx = std::clamp(styles[i].fontSizePx, 9.0f, 72.0f);
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

    // 分层保护的正文簇字号归一：仅在同字号层级（[0.84x, 1.16x]）且同亮度/字重层级内平滑归一
    std::vector<float> harmonizedPreferred(paragraphs.size(), 0.0f);
    std::vector<float> multiLineFitted;
    std::vector<float> multiLineOrig;
    std::vector<float> multiLineLum;
    std::vector<float> regularBodyFitted;
    std::vector<float> regularBodyOrig;
    std::vector<float> regularBodyLum;
    for (size_t i = 0; i < paragraphs.size(); ++i) {
        if (!activeChanged[i] || fittedFontSizes[i] < kMinReadableFontPx) continue;
        const float origPx = std::clamp(styles[i].fontSizePx, 9.0f, 72.0f);
        const float lum = relativeLuminance(styles[i].textBgr);
        const int firstLineH = paragraphs[i].lineRects.empty()
            ? paragraphs[i].unionRect.height
            : paragraphs[i].lineRects.front().height;
        if (!paragraphs[i].isSingleLine && firstLineH <= 22) {
            multiLineFitted.push_back(fittedFontSizes[i]);
            multiLineOrig.push_back(origPx);
            multiLineLum.push_back(lum);
        }
        if (paragraphs[i].unionRect.width >= 72 && firstLineH >= 9 && firstLineH <= 26) {
            regularBodyFitted.push_back(fittedFontSizes[i]);
            regularBodyOrig.push_back(origPx);
            regularBodyLum.push_back(lum);
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
        const float anchorLum = useMultiAnchor ? medianOf(multiLineLum) : medianOf(regularBodyLum);
        float targetBodyPx = 0.0f;
        if (useMultiAnchor) {
            auto sortedMulti = multiLineFitted;
            std::sort(sortedMulti.begin(), sortedMulti.end());
            const float medianFitted = sortedMulti[sortedMulti.size() / 2];
            targetBodyPx = (std::max)(sortedMulti.front(), medianFitted * 0.94f);
        } else {
            targetBodyPx = medianOf(regularBodyFitted);
        }

        const bool cjkTarget = isCjkTargetLanguage(targetLangCode);
        for (size_t i = 0; i < paragraphs.size(); ++i) {
            if (!activeChanged[i] || fittedFontSizes[i] < kMinReadableFontPx) continue;
            const float origPx = std::clamp(styles[i].fontSizePx, 9.0f, 72.0f);
            const float lum = relativeLuminance(styles[i].textBgr);
            // 层级守卫：排除大标题（> 1.16x 或明显更深/粗）与极小灰注（< 0.84x），严防把报纸粗标题压平到小正文！
            const bool sameHierarchy =
                (origPx >= anchorOrigPx * 0.84f && origPx <= anchorOrigPx * 1.16f) &&
                (std::abs(lum - anchorLum) <= 0.22f);
            if (sameHierarchy) {
                if (origPx > targetBodyPx && styles[i].fontWeight < 650) {
                    harmonizedPreferred[i] = targetBodyPx;
                } else if (cjkTarget && origPx < targetBodyPx * 0.94f && paragraphs[i].unionRect.width >= 60) {
                    const float safeHeightCap = static_cast<float>(safeBoxes[i].rect.height) * 0.98f;
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
