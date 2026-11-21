// ─────────────────────────────────────────────────────────────────────────────
// WatermarkRenderer.cpp — 企业泄密溯源平铺水印与品牌图片/文字角标统一合成引擎实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/WatermarkRenderer.h"
#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <lmcons.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace tools3000::capture {

namespace {

using Microsoft::WRL::ComPtr;

struct CachedLogoEntry {
    std::filesystem::file_time_type mtime{};
    uintmax_t fileSize = 0;
    cv::Mat bgra;
};

std::mutex g_logoCacheMutex;
std::unordered_map<std::string, CachedLogoEntry> g_logoCache;

struct ThreadLocalWatermarkD2D {
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> dwFactory;
    ComPtr<ID2D1DCRenderTarget> dcRt;
    HDC memDc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldObj = nullptr;
    uint8_t* bits = nullptr;
    int surfW = 0;
    int surfH = 0;

    ~ThreadLocalWatermarkD2D() {
        releaseSurface();
    }

    void releaseSurface() noexcept {
        dcRt.Reset();
        if (memDc && oldObj) {
            SelectObject(memDc, oldObj);
            oldObj = nullptr;
        }
        if (dib) {
            DeleteObject(dib);
            dib = nullptr;
        }
        if (memDc) {
            DeleteDC(memDc);
            memDc = nullptr;
        }
        bits = nullptr;
        surfW = 0;
        surfH = 0;
    }

    bool ensureFactories() {
        if (!d2dFactory) {
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf());
        }
        if (!dwFactory) {
            DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));
        }
        return d2dFactory && dwFactory;
    }

    bool ensureSurface(int w, int h) {
        if (w <= 0 || h <= 0) return false;
        if (!ensureFactories()) return false;

        if (w > surfW || h > surfH || !memDc || !dib || !bits || !dcRt) {
            releaseSurface();
            const int allocW = (std::max)(w, 256);
            const int allocH = (std::max)(h, 128);

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = allocW;
            bmi.bmiHeader.biHeight = -allocH; // top-down BGRA
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            memDc = CreateCompatibleDC(nullptr);
            if (!memDc) return false;

            void* rawBits = nullptr;
            dib = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &rawBits, nullptr, 0);
            if (!dib || !rawBits) {
                releaseSurface();
                return false;
            }
            oldObj = SelectObject(memDc, dib);
            bits = static_cast<uint8_t*>(rawBits);
            surfW = allocW;
            surfH = allocH;

            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            if (FAILED(d2dFactory->CreateDCRenderTarget(&props, dcRt.GetAddressOf())) || !dcRt) {
                releaseSurface();
                return false;
            }
        }

        RECT bindRc{0, 0, w, h};
        return SUCCEEDED(dcRt->BindDC(memDc, &bindRc));
    }
};

thread_local ThreadLocalWatermarkD2D t_wmD2D;

std::wstring queryCurrentUserName() {
    wchar_t buf[UNLEN + 1]{};
    DWORD len = static_cast<DWORD>(std::size(buf));
    if (GetUserNameW(buf, &len) && buf[0] != L'\0') {
        return std::wstring(buf);
    }
    return L"User";
}

std::wstring queryComputerHostName() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD len = static_cast<DWORD>(std::size(buf));
    if (GetComputerNameW(buf, &len) && buf[0] != L'\0') {
        return std::wstring(buf);
    }
    return L"Workstation";
}

std::wstring queryCachedLocalIpv4() {
    static std::mutex s_ipMutex;
    static std::wstring s_cachedIp;
    static std::chrono::steady_clock::time_point s_lastCheck{};

    std::lock_guard lock(s_ipMutex);
    const auto now = std::chrono::steady_clock::now();
    if (!s_cachedIp.empty() && (now - s_lastCheck) < std::chrono::seconds(15)) {
        return s_cachedIp;
    }
    s_lastCheck = now;

    WSADATA wsa{};
    const bool wsaOk = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    std::wstring bestIp = L"127.0.0.1";
    char hostName[256]{};
    if (gethostname(hostName, sizeof(hostName) - 1) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(hostName, nullptr, &hints, &res) == 0 && res) {
            for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
                auto* ipv4 = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                if (!ipv4) continue;
                wchar_t ipBuf[64]{};
                if (InetNtopW(AF_INET, &ipv4->sin_addr, ipBuf, std::size(ipBuf))) {
                    std::wstring cand(ipBuf);
                    if (cand != L"127.0.0.1" && cand.rfind(L"169.254.", 0) != 0) {
                        bestIp = std::move(cand);
                        break;
                    }
                }
            }
            freeaddrinfo(res);
        }
    }
    if (wsaOk) {
        WSACleanup();
    }
    s_cachedIp = bestIp;
    return s_cachedIp;
}

void replaceTokenCaseInsensitive(std::wstring& target, std::wstring_view tokenLower, std::wstring_view replacement) {
    size_t pos = 0;
    while (pos < target.size()) {
        size_t openBrace = target.find(L'{', pos);
        if (openBrace == std::wstring::npos) break;
        size_t closeBrace = target.find(L'}', openBrace + 1);
        if (closeBrace == std::wstring::npos) break;

        const size_t spanLen = closeBrace - openBrace + 1;
        if (spanLen == tokenLower.size()) {
            bool match = true;
            for (size_t i = 0; i < spanLen; ++i) {
                wchar_t c = target[openBrace + i];
                if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
                if (c != tokenLower[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                target.replace(openBrace, spanLen, replacement);
                pos = openBrace + replacement.size();
                continue;
            }
        }
        pos = openBrace + 1;
    }
}

void applyRegistryEnterprisePolicyOverride(WatermarkConfig& cfg) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Policies\\Tools3000", 0, KEY_READ, &hKey) != ERROR_SUCCESS || !hKey) {
        return;
    }

    DWORD forceVal = 0;
    DWORD cbData = sizeof(forceVal);
    DWORD regType = 0;
    if (RegQueryValueExW(hKey, L"ForceWatermark", nullptr, &regType, reinterpret_cast<LPBYTE>(&forceVal), &cbData) == ERROR_SUCCESS &&
        regType == REG_DWORD && forceVal != 0) {
        cfg.enabled = true;
        cfg.policyForced = true;
    }

    DWORD modeVal = 0;
    cbData = sizeof(modeVal);
    if (RegQueryValueExW(hKey, L"WatermarkMode", nullptr, &regType, reinterpret_cast<LPBYTE>(&modeVal), &cbData) == ERROR_SUCCESS &&
        regType == REG_DWORD && modeVal <= 2) {
        cfg.mode = static_cast<WatermarkMode>(modeVal);
    }

    DWORD opacityPct = 0;
    cbData = sizeof(opacityPct);
    if (RegQueryValueExW(hKey, L"WatermarkOpacity", nullptr, &regType, reinterpret_cast<LPBYTE>(&opacityPct), &cbData) == ERROR_SUCCESS &&
        regType == REG_DWORD && opacityPct >= 4 && opacityPct <= 100) {
        cfg.opacity = static_cast<float>(opacityPct) / 100.0f;
    }

    wchar_t strBuf[1024]{};
    cbData = sizeof(strBuf) - sizeof(wchar_t);
    if (RegQueryValueExW(hKey, L"WatermarkTemplate", nullptr, &regType, reinterpret_cast<LPBYTE>(strBuf), &cbData) == ERROR_SUCCESS &&
        (regType == REG_SZ || regType == REG_EXPAND_SZ) && strBuf[0] != L'\0') {
        cfg.textTemplate = tools3000::core::WinUtils::wstringToUtf8(strBuf);
    }

    std::memset(strBuf, 0, sizeof(strBuf));
    cbData = sizeof(strBuf) - sizeof(wchar_t);
    if (RegQueryValueExW(hKey, L"WatermarkImagePath", nullptr, &regType, reinterpret_cast<LPBYTE>(strBuf), &cbData) == ERROR_SUCCESS &&
        (regType == REG_SZ || regType == REG_EXPAND_SZ) && strBuf[0] != L'\0') {
        cfg.imagePathUtf8 = tools3000::core::WinUtils::wstringToUtf8(strBuf);
    }

    RegCloseKey(hKey);
}

cv::Rect resolveAnchoredBadgeRect(
    int canvasW, int canvasH, int badgeW, int badgeH, const WatermarkConfig& cfg) {
    const float safeMarginRatio = std::clamp(cfg.marginRatio, 0.008f, 0.12f);
    const int marginPx = (std::max)(8, static_cast<int>(std::round((std::min)(canvasW, canvasH) * safeMarginRatio)));

    int x = marginPx;
    int y = marginPx;
    switch (cfg.anchor) {
        case WatermarkAnchor::TopLeft:
            x = marginPx;
            y = marginPx;
            break;
        case WatermarkAnchor::TopRight:
            x = canvasW - badgeW - marginPx;
            y = marginPx;
            break;
        case WatermarkAnchor::BottomLeft:
            x = marginPx;
            y = canvasH - badgeH - marginPx;
            break;
        case WatermarkAnchor::BottomRight:
            x = canvasW - badgeW - marginPx;
            y = canvasH - badgeH - marginPx;
            break;
        case WatermarkAnchor::Center:
            x = (canvasW - badgeW) / 2;
            y = (canvasH - badgeH) / 2;
            break;
        case WatermarkAnchor::CustomFree: {
            const int cx = static_cast<int>(std::round(std::clamp(cfg.customNormX, 0.02f, 0.98f) * canvasW));
            const int cy = static_cast<int>(std::round(std::clamp(cfg.customNormY, 0.02f, 0.98f) * canvasH));
            x = cx - badgeW / 2;
            y = cy - badgeH / 2;
            break;
        }
    }

    x = std::clamp(x, 0, (std::max)(0, canvasW - badgeW));
    y = std::clamp(y, 0, (std::max)(0, canvasH - badgeH));
    return cv::Rect(x, y, (std::min)(badgeW, canvasW - x), (std::min)(badgeH, canvasH - y));
}

void blendStraightBgraOntoTarget(
    const cv::Mat& srcBgra, cv::Mat& dst, int dstX, int dstY, float masterOpacity) {
    if (srcBgra.empty() || srcBgra.type() != CV_8UC4 || dst.empty()) return;
    const int ch = dst.channels();
    if (ch != 3 && ch != 4) return;

    const float clampedOp = std::clamp(masterOpacity, 0.02f, 1.0f);
    const int op256 = static_cast<int>(std::round(clampedOp * 256.0f));
    if (op256 <= 0) return;

    const int x0 = (std::max)(0, dstX);
    const int y0 = (std::max)(0, dstY);
    const int x1 = (std::min)(dst.cols, dstX + srcBgra.cols);
    const int y1 = (std::min)(dst.rows, dstY + srcBgra.rows);
    if (x1 <= x0 || y1 <= y0) return;

    for (int y = y0; y < y1; ++y) {
        const uint8_t* sRow = srcBgra.ptr<uint8_t>(y - dstY);
        uint8_t* dRow = dst.ptr<uint8_t>(y);
        for (int x = x0; x < x1; ++x) {
            const int sx = x - dstX;
            const uint32_t sa = (static_cast<uint32_t>(sRow[sx * 4 + 3]) * static_cast<uint32_t>(op256)) >> 8u;
            if (sa == 0) continue;
            const uint32_t inv = 255u - sa;
            uint8_t* dp = dRow + x * ch;
            dp[0] = static_cast<uint8_t>((sRow[sx * 4 + 0] * sa + dp[0] * inv + 127u) / 255u);
            dp[1] = static_cast<uint8_t>((sRow[sx * 4 + 1] * sa + dp[1] * inv + 127u) / 255u);
            dp[2] = static_cast<uint8_t>((sRow[sx * 4 + 2] * sa + dp[2] * inv + 127u) / 255u);
            if (ch == 4) {
                dp[3] = static_cast<uint8_t>(sa + ((dp[3] * inv + 127u) / 255u));
            }
        }
    }
}

void blendPremultipliedBgraOntoTarget(
    const uint8_t* premulBits, int surfStride, int boxW, int boxH, cv::Mat& dst, int dstX, int dstY) {
    const int ch = dst.channels();
    if (!premulBits || (ch != 3 && ch != 4)) return;

    const int x0 = (std::max)(0, dstX);
    const int y0 = (std::max)(0, dstY);
    const int x1 = (std::min)(dst.cols, dstX + boxW);
    const int y1 = (std::min)(dst.rows, dstY + boxH);
    if (x1 <= x0 || y1 <= y0) return;

    for (int y = y0; y < y1; ++y) {
        const uint8_t* sRow = premulBits + static_cast<size_t>(y - dstY) * static_cast<size_t>(surfStride);
        uint8_t* dRow = dst.ptr<uint8_t>(y);
        for (int x = x0; x < x1; ++x) {
            const int sx = x - dstX;
            const uint32_t sa = sRow[sx * 4 + 3];
            if (sa == 0) continue;
            const uint32_t inv = 255u - sa;
            uint8_t* dp = dRow + x * ch;
            dp[0] = static_cast<uint8_t>((std::min)(255u, static_cast<uint32_t>(sRow[sx * 4 + 0]) + (dp[0] * inv + 127u) / 255u));
            dp[1] = static_cast<uint8_t>((std::min)(255u, static_cast<uint32_t>(sRow[sx * 4 + 1]) + (dp[1] * inv + 127u) / 255u));
            dp[2] = static_cast<uint8_t>((std::min)(255u, static_cast<uint32_t>(sRow[sx * 4 + 2]) + (dp[2] * inv + 127u) / 255u));
            if (ch == 4) {
                dp[3] = static_cast<uint8_t>((std::min)(255u, sa + (dp[3] * inv + 127u) / 255u));
            }
        }
    }
}

void applyMultiLineTypography(IDWriteTextLayout* layout, const std::wstring& resolved, float baseFontPx) {
    if (!layout) return;
    const size_t firstNl = resolved.find(L'\n');
    if (firstNl == std::wstring::npos) return;

    layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, baseFontPx * 1.18f, baseFontPx * 0.94f);
    if (firstNl + 1 < resolved.size()) {
        const DWRITE_TEXT_RANGE subRange{
            static_cast<UINT32>(firstNl + 1),
            static_cast<UINT32>(resolved.size() - (firstNl + 1))
        };
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_MEDIUM, subRange);
        layout->SetFontSize((std::max)(7.5f, baseFontPx * 0.86f), subRange);
    }
}

bool measureCornerTextBadgeSize(
    int canvasW, int canvasH, const WatermarkConfig& cfg,
    std::wstring& outResolved, float& outFontPx, int& outBadgeW, int& outBadgeH,
    ComPtr<IDWriteTextFormat>& outFmt, ComPtr<IDWriteTextLayout>& outLayout) {
    outResolved = WatermarkRenderer::resolveDynamicText(cfg.textTemplate);
    if (outResolved.empty()) return false;
    if (!t_wmD2D.ensureFactories()) return false;

    const float refSide = std::clamp(std::sqrt(static_cast<float>(canvasW) * static_cast<float>(canvasH)), 240.0f, 3840.0f);
    const float safeScale = std::clamp(cfg.scale, 0.35f, 2.80f);
    outFontPx = std::clamp(refSide * 0.020f * safeScale, 8.0f, 92.0f);

    const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(t_wmD2D.dwFactory.Get());
    if (FAILED(t_wmD2D.dwFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            outFontPx, L"zh-CN", outFmt.GetAddressOf())) || !outFmt) {
        return false;
    }
    outFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    const float maxLayoutW = (std::max)(120.0f, canvasW * 0.85f);
    const float maxLayoutH = (std::max)(outFontPx * 10.0f, canvasH * 0.65f);
    if (FAILED(t_wmD2D.dwFactory->CreateTextLayout(
            outResolved.c_str(), static_cast<UINT32>(outResolved.size()),
            outFmt.Get(), maxLayoutW, maxLayoutH, outLayout.GetAddressOf())) || !outLayout) {
        return false;
    }
    applyMultiLineTypography(outLayout.Get(), outResolved, outFontPx);

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(outLayout->GetMetrics(&metrics))) return false;

    const float tightW = (std::max)(16.0f, std::ceil(metrics.widthIncludingTrailingWhitespace) + 2.0f);
    const float tightH = (std::max)(outFontPx, std::ceil(metrics.height) + 2.0f);
    outLayout->SetMaxWidth(tightW);
    outLayout->SetMaxHeight(tightH);
    const bool isMultiLine = (outResolved.find(L'\n') != std::wstring::npos);
    outLayout->SetTextAlignment(isMultiLine ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);

    const int padX = (std::max)(8, static_cast<int>(std::round(outFontPx * 0.62f)));
    const int padY = (std::max)(5, static_cast<int>(std::round(outFontPx * 0.36f)));
    outBadgeW = std::clamp(static_cast<int>(std::ceil(tightW)) + padX * 2, 24, canvasW);
    outBadgeH = std::clamp(static_cast<int>(std::ceil(tightH)) + padY * 2, 18, canvasH);
    return true;
}

} // namespace

std::wstring WatermarkRenderer::resolveDynamicText(std::string_view utf8Template) {
    std::wstring text = utf8Template.empty()
        ? L"{user} · {host} · {date} {time}"
        : tools3000::core::WinUtils::utf8ToWstring(std::string(utf8Template));

    // 统一支持多行企业防泄密水印换行语法：真实换行符、字面量 "\n" 以及 "{nl}" 变量
    {
        size_t pos = 0;
        while ((pos = text.find(L"\r\n", pos)) != std::wstring::npos) {
            text.replace(pos, 2, L"\n");
            ++pos;
        }
        pos = 0;
        while ((pos = text.find(L"\\n", pos)) != std::wstring::npos) {
            text.replace(pos, 2, L"\n");
            ++pos;
        }
        replaceTokenCaseInsensitive(text, L"{nl}", L"\n");
    }

    if (text.find(L'{') != std::wstring::npos) {
        const auto now = std::chrono::system_clock::now();
        const std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::tm localTm{};
        localtime_s(&localTm, &tt);

        wchar_t dateBuf[32]{};
        wchar_t timeBuf[32]{};
        wchar_t dtBuf[64]{};
        std::swprintf(dateBuf, std::size(dateBuf), L"%04d-%02d-%02d",
                      localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday);
        std::swprintf(timeBuf, std::size(timeBuf), L"%02d:%02d",
                      localTm.tm_hour, localTm.tm_min);
        std::swprintf(dtBuf, std::size(dtBuf), L"%s %s", dateBuf, timeBuf);

        if (text.find(L"{") != std::wstring::npos) {
            const std::wstring user = queryCurrentUserName();
            const std::wstring host = queryComputerHostName();
            replaceTokenCaseInsensitive(text, L"{user}", user);
            replaceTokenCaseInsensitive(text, L"{username}", user);
            replaceTokenCaseInsensitive(text, L"{host}", host);
            replaceTokenCaseInsensitive(text, L"{hostname}", host);
            replaceTokenCaseInsensitive(text, L"{pc}", host);
            replaceTokenCaseInsensitive(text, L"{datetime}", dtBuf);
            replaceTokenCaseInsensitive(text, L"{date}", dateBuf);
            replaceTokenCaseInsensitive(text, L"{time}", timeBuf);
            if (text.find(L'{') != std::wstring::npos) {
                const std::wstring ip = queryCachedLocalIpv4();
                replaceTokenCaseInsensitive(text, L"{ip}", ip);
            }
        }
    }
    return text;
}

cv::Mat WatermarkRenderer::loadCachedLogoBgra(const std::string& imagePathUtf8) {
    if (imagePathUtf8.empty()) return {};

    std::error_code ec;
    const std::filesystem::path fsPath(tools3000::core::WinUtils::utf8ToWstring(imagePathUtf8));
    if (!std::filesystem::is_regular_file(fsPath, ec) || ec) {
        return {};
    }
    const auto mtime = std::filesystem::last_write_time(fsPath, ec);
    const auto fsize = std::filesystem::file_size(fsPath, ec);
    if (ec || fsize == 0 || fsize > 64ULL * 1024ULL * 1024ULL) {
        return {};
    }

    {
        std::lock_guard lock(g_logoCacheMutex);
        auto it = g_logoCache.find(imagePathUtf8);
        if (it != g_logoCache.end() &&
            it->second.mtime == mtime &&
            it->second.fileSize == fsize &&
            !it->second.bgra.empty()) {
            return it->second.bgra;
        }
    }

    std::ifstream ifs(fsPath, std::ios::binary);
    if (!ifs) return {};
    std::vector<uint8_t> rawBytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (rawBytes.empty()) return {};

    cv::Mat decoded = cv::imdecode(rawBytes, cv::IMREAD_UNCHANGED);
    if (decoded.empty()) return {};

    cv::Mat bgra;
    if (decoded.channels() == 4) {
        if (decoded.depth() == CV_16U) {
            decoded.convertTo(bgra, CV_8UC4, 1.0 / 257.0);
        } else {
            bgra = decoded;
        }
    } else if (decoded.channels() == 3) {
        cv::Mat bgr8 = decoded;
        if (decoded.depth() == CV_16U) decoded.convertTo(bgr8, CV_8UC3, 1.0 / 257.0);
        cv::cvtColor(bgr8, bgra, cv::COLOR_BGR2BGRA);
    } else if (decoded.channels() == 1) {
        cv::Mat g8 = decoded;
        if (decoded.depth() == CV_16U) decoded.convertTo(g8, CV_8UC1, 1.0 / 257.0);
        cv::cvtColor(g8, bgra, cv::COLOR_GRAY2BGRA);
    } else {
        return {};
    }

    {
        std::lock_guard lock(g_logoCacheMutex);
        if (g_logoCache.size() >= 16) {
            g_logoCache.clear();
        }
        g_logoCache[imagePathUtf8] = CachedLogoEntry{mtime, fsize, bgra};
    }
    return bgra;
}

bool WatermarkRenderer::computeSingleBadgeBounds(
    int canvasWidth,
    int canvasHeight,
    const WatermarkConfig& config,
    cv::Rect& outBoundsPx) {
    outBoundsPx = {};
    if (!config.enabled || canvasWidth < 16 || canvasHeight < 16) return false;

    const float refSide = std::clamp(std::sqrt(static_cast<float>(canvasWidth) * static_cast<float>(canvasHeight)), 240.0f, 3840.0f);
    const float safeScale = std::clamp(config.scale, 0.35f, 2.80f);

    if (config.mode == WatermarkMode::ImageLogo) {
        cv::Mat logo = loadCachedLogoBgra(config.imagePathUtf8);
        if (logo.empty()) return false;
        const float maxTargetSide = std::clamp(refSide * 0.145f * safeScale, 20.0f, (std::min)(canvasWidth, canvasHeight) * 0.75f);
        const float ratio = maxTargetSide / static_cast<float>((std::max)(logo.cols, logo.rows));
        const int bw = std::clamp(static_cast<int>(std::round(logo.cols * ratio)), 12, canvasWidth);
        const int bh = std::clamp(static_cast<int>(std::round(logo.rows * ratio)), 12, canvasHeight);
        outBoundsPx = resolveAnchoredBadgeRect(canvasWidth, canvasHeight, bw, bh, config);
        return outBoundsPx.width > 0 && outBoundsPx.height > 0;
    }

    if (config.mode == WatermarkMode::CornerText) {
        std::wstring resolved;
        float fontPx = 16.0f;
        int bw = 0, bh = 0;
        ComPtr<IDWriteTextFormat> fmt;
        ComPtr<IDWriteTextLayout> layout;
        if (!measureCornerTextBadgeSize(canvasWidth, canvasHeight, config, resolved, fontPx, bw, bh, fmt, layout)) {
            return false;
        }
        outBoundsPx = resolveAnchoredBadgeRect(canvasWidth, canvasHeight, bw, bh, config);
        return outBoundsPx.width > 0 && outBoundsPx.height > 0;
    }

    return false;
}

bool WatermarkRenderer::applyInPlace(
    cv::Mat& targetBgrOrBgra,
    const WatermarkConfig& config,
    cv::Rect* outBadgeBoundsPx) {
    if (outBadgeBoundsPx) *outBadgeBoundsPx = {};
    if (!config.enabled || targetBgrOrBgra.empty()) return false;

    const int W = targetBgrOrBgra.cols;
    const int H = targetBgrOrBgra.rows;
    const int ch = targetBgrOrBgra.channels();
    if (W < 16 || H < 16 || (ch != 3 && ch != 4)) return false;

    const float safeScale = std::clamp(config.scale, 0.35f, 2.80f);
    const float safeOpacity = std::clamp(config.opacity, 0.04f, 1.0f);
    const float refSide = std::clamp(std::sqrt(static_cast<float>(W) * static_cast<float>(H)), 240.0f, 3840.0f);

    if (config.mode == WatermarkMode::ImageLogo) {
        cv::Mat logo = loadCachedLogoBgra(config.imagePathUtf8);
        if (logo.empty()) return false;

        const float maxTargetSide = std::clamp(refSide * 0.145f * safeScale, 20.0f, (std::min)(W, H) * 0.75f);
        const float ratio = maxTargetSide / static_cast<float>((std::max)(logo.cols, logo.rows));
        const int bw = std::clamp(static_cast<int>(std::round(logo.cols * ratio)), 12, W);
        const int bh = std::clamp(static_cast<int>(std::round(logo.rows * ratio)), 12, H);
        const cv::Rect dstRc = resolveAnchoredBadgeRect(W, H, bw, bh, config);
        if (dstRc.width <= 0 || dstRc.height <= 0) return false;

        cv::Mat resizedLogo;
        cv::resize(logo, resizedLogo, cv::Size(dstRc.width, dstRc.height), 0.0, 0.0,
                   ratio < 1.0f ? cv::INTER_AREA : cv::INTER_CUBIC);
        blendStraightBgraOntoTarget(resizedLogo, targetBgrOrBgra, dstRc.x, dstRc.y, safeOpacity);
        if (outBadgeBoundsPx) *outBadgeBoundsPx = dstRc;
        return true;
    }

    if (config.mode == WatermarkMode::CornerText) {
        std::wstring resolved;
        float fontPx = 16.0f;
        int bw = 0, bh = 0;
        ComPtr<IDWriteTextFormat> fmt;
        ComPtr<IDWriteTextLayout> layout;
        if (!measureCornerTextBadgeSize(W, H, config, resolved, fontPx, bw, bh, fmt, layout)) {
            return false;
        }
        const cv::Rect dstRc = resolveAnchoredBadgeRect(W, H, bw, bh, config);
        if (dstRc.width <= 0 || dstRc.height <= 0) return false;
        if (!t_wmD2D.ensureSurface(dstRc.width, dstRc.height)) return false;

        // 采样底层区域平均亮度，自适应切换深色/浅色微晶胶囊底衬防隐身
        uint64_t lumSum = 0;
        uint64_t lumCnt = 0;
        for (int sy = dstRc.y; sy < dstRc.y + dstRc.height; sy += 3) {
            const uint8_t* rPtr = targetBgrOrBgra.ptr<uint8_t>(sy);
            for (int sx = dstRc.x; sx < dstRc.x + dstRc.width; sx += 4) {
                const uint8_t* px = rPtr + sx * ch;
                lumSum += static_cast<uint64_t>(px[2] * 2 + px[1] * 5 + px[0]) >> 3u;
                ++lumCnt;
            }
        }
        const bool brightUnder = (lumCnt == 0) || ((lumSum / lumCnt) >= 138);

        auto* rt = t_wmD2D.dcRt.Get();
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        ComPtr<ID2D1SolidColorBrush> br;
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), br.GetAddressOf());
        if (br) {
            const float rad = std::clamp((std::min)(dstRc.height * 0.28f, fontPx * 0.72f), 4.0f, 12.0f);
            const D2D1_RECT_F pillRc = D2D1::RectF(0.5f, 0.5f, dstRc.width - 0.5f, dstRc.height - 0.5f);
            br->SetColor(brightUnder
                ? D2D1::ColorF(0.07f, 0.10f, 0.17f, 0.62f * safeOpacity)
                : D2D1::ColorF(0.06f, 0.09f, 0.15f, 0.68f * safeOpacity));
            rt->FillRoundedRectangle(D2D1::RoundedRect(pillRc, rad, rad), br.Get());

            br->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f * safeOpacity));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(pillRc, rad, rad), br.Get(), 1.0f);

            DWRITE_TEXT_METRICS tm{};
            const float drawW = SUCCEEDED(layout->GetMetrics(&tm)) ? layout->GetMaxWidth() : static_cast<float>(dstRc.width);
            const float drawH = (tm.height > 0.5f) ? tm.height : static_cast<float>(dstRc.height);
            const float ox = (std::max)(0.0f, (static_cast<float>(dstRc.width) - drawW) * 0.5f);
            const float oy = (std::max)(0.0f, (static_cast<float>(dstRc.height) - drawH) * 0.5f);
            br->SetColor(D2D1::ColorF(0.98f, 0.99f, 1.0f, 0.96f * safeOpacity));
            rt->DrawTextLayout(D2D1::Point2F(ox, oy), layout.Get(), br.Get());
        }
        if (FAILED(rt->EndDraw())) {
            t_wmD2D.releaseSurface();
            return false;
        }

        blendPremultipliedBgraOntoTarget(
            t_wmD2D.bits, t_wmD2D.surfW * 4, dstRc.width, dstRc.height, targetBgrOrBgra, dstRc.x, dstRc.y);
        if (outBadgeBoundsPx) *outBadgeBoundsPx = dstRc;
        return true;
    }

    // Mode 2: TiledTraceability — 全屏 -28 度斜向平铺合规溯源矩阵（局部平滑亮度场自适应显影，深浅背景均清晰柔和）
    const std::wstring resolved = resolveDynamicText(config.textTemplate);
    if (resolved.empty()) return false;
    if (!t_wmD2D.ensureFactories()) return false;

    const int maxMaskSide = 2048;
    const float maskScale = ((std::max)(W, H) > maxMaskSide)
        ? (static_cast<float>(maxMaskSide) / static_cast<float>((std::max)(W, H)))
        : 1.0f;
    const int mW = (std::max)(32, static_cast<int>(std::round(W * maskScale)));
    const int mH = (std::max)(32, static_cast<int>(std::round(H * maskScale)));
    if (!t_wmD2D.ensureSurface(mW, mH)) return false;

    const float maskRef = std::clamp(std::sqrt(static_cast<float>(mW) * static_cast<float>(mH)), 200.0f, 2048.0f);
    const float fontPx = std::clamp(maskRef * 0.020f * safeScale, 7.5f, 68.0f);

    ComPtr<IDWriteTextFormat> fmt;
    const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(t_wmD2D.dwFactory.Get());
    if (FAILED(t_wmD2D.dwFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontPx, L"zh-CN", fmt.GetAddressOf())) || !fmt) {
        return false;
    }
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(t_wmD2D.dwFactory->CreateTextLayout(
            resolved.c_str(), static_cast<UINT32>(resolved.size()),
            fmt.Get(), 2800.0f, fontPx * 10.0f, layout.GetAddressOf())) || !layout) {
        return false;
    }
    applyMultiLineTypography(layout.Get(), resolved, fontPx);

    DWRITE_TEXT_METRICS tm{};
    if (FAILED(layout->GetMetrics(&tm))) return false;

    // 先以实际最宽行锁定紧凑盒宽；企业双行/多行水印采用标准左对齐（首行账号与次行日期时间左缘齐平），单行保持居中
    const float tightW = (std::max)(28.0f, std::ceil(tm.widthIncludingTrailingWhitespace) + 4.0f);
    layout->SetMaxWidth(tightW);
    const bool isMultiLine = (resolved.find(L'\n') != std::wstring::npos);
    layout->SetTextAlignment(isMultiLine ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
    if (FAILED(layout->GetMetrics(&tm))) return false;

    const float textW = (std::max)(28.0f, tm.widthIncludingTrailingWhitespace);
    const float textH = (std::max)(fontPx, tm.height);
    const float stepX = textW + (std::max)(28.0f, fontPx * 3.8f);
    const float stepY = (std::max)(26.0f, textH + fontPx * 3.6f);
    const float diag = std::hypot(static_cast<float>(mW), static_cast<float>(mH));

    auto* rt = t_wmD2D.dcRt.Get();
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    ComPtr<ID2D1SolidColorBrush> whiteBr;
    rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteBr.GetAddressOf());
    if (whiteBr) {
        rt->SetTransform(D2D1::Matrix3x2F::Rotation(-28.0f, D2D1::Point2F(mW * 0.5f, mH * 0.5f)));
        const float startX = mW * 0.5f - diag * 0.65f;
        const float endX   = mW * 0.5f + diag * 0.65f;
        const float startY = mH * 0.5f - diag * 0.65f;
        const float endY   = mH * 0.5f + diag * 0.65f;

        int rowIdx = 0;
        for (float y = startY; y <= endY; y += stepY, ++rowIdx) {
            const float rowOffset = (rowIdx & 1) ? (stepX * 0.5f) : 0.0f;
            for (float x = startX - rowOffset; x <= endX; x += stepX) {
                rt->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), whiteBr.Get());
            }
        }
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    if (FAILED(rt->EndDraw())) {
        t_wmD2D.releaseSurface();
        return false;
    }

    // 提纯覆盖 Alpha 通道并结合 8x 局部平滑背景亮度场执行双极自适应显影（消除单字内黑白碎点跳变与中间灰隐身）
    cv::Mat alphaMask;
    {
        cv::Mat bgraView(mH, mW, CV_8UC4, t_wmD2D.bits, static_cast<size_t>(t_wmD2D.surfW) * 4);
        cv::extractChannel(bgraView, alphaMask, 3);
        if (mW != W || mH != H) {
            cv::Mat upMask;
            cv::resize(alphaMask, upMask, cv::Size(W, H), 0.0, 0.0, cv::INTER_LINEAR);
            alphaMask = std::move(upMask);
        } else {
            alphaMask = alphaMask.clone();
        }
    }

    const int op256 = static_cast<int>(std::round(safeOpacity * 256.0f));
    if (op256 <= 0) return false;

    const int coarseW = (std::max)(4, W / 8);
    const int coarseH = (std::max)(4, H / 8);
    cv::Mat smallTarget, smallGray, smoothGray;
    cv::resize(targetBgrOrBgra, smallTarget, cv::Size(coarseW, coarseH), 0.0, 0.0, cv::INTER_AREA);
    cv::cvtColor(smallTarget, smallGray, (ch == 4) ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(smallGray, smallGray, cv::Size(5, 5), 1.2);
    cv::resize(smallGray, smoothGray, cv::Size(W, H), 0.0, 0.0, cv::INTER_LINEAR);

    for (int y = 0; y < H; ++y) {
        const uint8_t* mRow = alphaMask.ptr<uint8_t>(y);
        const uint8_t* lRow = smoothGray.ptr<uint8_t>(y);
        uint8_t* dRow = targetBgrOrBgra.ptr<uint8_t>(y);
        for (int x = 0; x < W; ++x) {
            const uint8_t cov = mRow[x];
            if (cov == 0) continue;

            const int bgLum = static_cast<int>(lRow[x]);
            // 中间灰区域自动提升约 28% 有效显影权重，保障彩色渐变与灰底同样柔和可辨
            const int midDist = std::abs(bgLum - 134);
            const uint32_t midBoost256 = (midDist < 48)
                ? static_cast<uint32_t>(256 + ((48 - midDist) * 76) / 48)
                : 256u;
            const uint32_t boostedCov = (std::min)(255u, (static_cast<uint32_t>(cov) * 296u) >> 8u);
            const uint32_t sa = (std::min)(
                255u,
                (((boostedCov * static_cast<uint32_t>(op256)) >> 8u) * midBoost256) >> 8u);
            if (sa == 0) continue;

            uint8_t* dp = dRow + x * ch;
            // 亮背景混入沉稳深石板墨色 BGR(42, 28, 20)，暗背景混入清透钛银白 BGR(254, 250, 246)
            const bool useDarkInk = (bgLum >= 136);
            const uint32_t inkB = useDarkInk ? 42u : 254u;
            const uint32_t inkG = useDarkInk ? 28u : 250u;
            const uint32_t inkR = useDarkInk ? 20u : 246u;
            const uint32_t inv = 255u - sa;

            dp[0] = static_cast<uint8_t>((inkB * sa + dp[0] * inv + 127u) / 255u);
            dp[1] = static_cast<uint8_t>((inkG * sa + dp[1] * inv + 127u) / 255u);
            dp[2] = static_cast<uint8_t>((inkR * sa + dp[2] * inv + 127u) / 255u);
        }
    }
    return true;
}

WatermarkConfig WatermarkRenderer::loadGlobalCaptureConfig() {
    auto& cm = tools3000::core::ConfigManager::instance();
    WatermarkConfig cfg{};
    cfg.enabled = cm.get<bool>("/capture/watermarkEnabled", false);
    cfg.mode = static_cast<WatermarkMode>(std::clamp(cm.get<int>("/capture/watermarkMode", 2), 0, 2));
    cfg.imagePathUtf8 = cm.get<std::string>("/capture/watermarkImagePath", "");
    cfg.textTemplate = cm.get<std::string>("/capture/watermarkText", "{user} · {host} · {date} {time}");
    cfg.anchor = static_cast<WatermarkAnchor>(std::clamp(cm.get<int>("/capture/watermarkAnchor", 3), 0, 5));
    cfg.scale = std::clamp(static_cast<float>(cm.get<double>("/capture/watermarkScale", 1.0)), 0.35f, 2.80f);
    const float defaultOp = (cfg.mode == WatermarkMode::TiledTraceability) ? 0.14f : 0.85f;
    cfg.opacity = std::clamp(static_cast<float>(cm.get<double>("/capture/watermarkOpacity", defaultOp)), 0.04f, 1.0f);
    applyRegistryEnterprisePolicyOverride(cfg);
    return cfg;
}

cv::Mat WatermarkRenderer::applyGlobalCaptureWatermarkIfEnabled(const cv::Mat& cleanSource) {
    if (cleanSource.empty()) return cleanSource;
    const WatermarkConfig cfg = loadGlobalCaptureConfig();
    if (!cfg.enabled) return cleanSource;

    cv::Mat out = cleanSource.clone();
    applyInPlace(out, cfg, nullptr);
    return out;
}

WatermarkConfig WatermarkRenderer::loadStudioWatermarkConfig() {
    auto& cm = tools3000::core::ConfigManager::instance();
    WatermarkConfig cfg{};
    cfg.enabled = cm.get<bool>("/recording/studioWatermark/enabled", false);
    cfg.mode = static_cast<WatermarkMode>(std::clamp(cm.get<int>("/recording/studioWatermark/mode", 0), 0, 2));
    cfg.imagePathUtf8 = cm.get<std::string>("/recording/studioWatermark/imagePath", "");
    cfg.textTemplate = cm.get<std::string>("/recording/studioWatermark/text", "{user} · {date} {time}");
    cfg.anchor = static_cast<WatermarkAnchor>(std::clamp(cm.get<int>("/recording/studioWatermark/anchor", 3), 0, 5));
    cfg.customNormX = std::clamp(static_cast<float>(cm.get<double>("/recording/studioWatermark/normX", 0.88)), 0.02f, 0.98f);
    cfg.customNormY = std::clamp(static_cast<float>(cm.get<double>("/recording/studioWatermark/normY", 0.90)), 0.02f, 0.98f);
    cfg.scale = std::clamp(static_cast<float>(cm.get<double>("/recording/studioWatermark/scale", 1.0)), 0.35f, 2.80f);
    const float defaultOp = (cfg.mode == WatermarkMode::TiledTraceability) ? 0.14f : 0.88f;
    cfg.opacity = std::clamp(static_cast<float>(cm.get<double>("/recording/studioWatermark/opacity", defaultOp)), 0.04f, 1.0f);
    applyRegistryEnterprisePolicyOverride(cfg);
    return cfg;
}

void WatermarkRenderer::saveStudioWatermarkConfig(const WatermarkConfig& config) {
    auto& cm = tools3000::core::ConfigManager::instance();
    cm.set<bool>("/recording/studioWatermark/enabled", config.enabled);
    cm.set<int>("/recording/studioWatermark/mode", static_cast<int>(config.mode));
    cm.set<std::string>("/recording/studioWatermark/imagePath", config.imagePathUtf8);
    cm.set<std::string>("/recording/studioWatermark/text", config.textTemplate);
    cm.set<int>("/recording/studioWatermark/anchor", static_cast<int>(config.anchor));
    cm.set<double>("/recording/studioWatermark/normX", static_cast<double>(config.customNormX));
    cm.set<double>("/recording/studioWatermark/normY", static_cast<double>(config.customNormY));
    cm.set<double>("/recording/studioWatermark/scale", static_cast<double>(config.scale));
    cm.set<double>("/recording/studioWatermark/opacity", static_cast<double>(config.opacity));
}

std::vector<std::string> WatermarkRenderer::loadStudioRecentLogoPaths() {
    std::vector<std::string> out;
    auto& cm = tools3000::core::ConfigManager::instance();
    for (int i = 0; i < 6; ++i) {
        const std::string key = "/recording/studioWatermark/recentLogo" + std::to_string(i);
        const std::string val = cm.get<std::string>(key, "");
        if (val.empty()) continue;
        std::error_code ec;
        if (std::filesystem::is_regular_file(std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(val)), ec) && !ec) {
            out.push_back(val);
        }
    }
    return out;
}

void WatermarkRenderer::pushStudioRecentLogoPath(const std::string& imagePathUtf8) {
    if (imagePathUtf8.empty()) return;
    std::vector<std::string> cur = loadStudioRecentLogoPaths();
    cur.erase(std::remove(cur.begin(), cur.end(), imagePathUtf8), cur.end());
    cur.insert(cur.begin(), imagePathUtf8);
    if (cur.size() > 6) cur.resize(6);

    auto& cm = tools3000::core::ConfigManager::instance();
    for (size_t i = 0; i < 6; ++i) {
        const std::string key = "/recording/studioWatermark/recentLogo" + std::to_string(i);
        cm.set<std::string>(key, i < cur.size() ? cur[i] : "");
    }
}

} // namespace tools3000::capture
