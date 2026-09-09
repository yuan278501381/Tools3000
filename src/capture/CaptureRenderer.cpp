#include "capture/CaptureRenderer.h"
#include "capture/CaptureHudRimStyle.h"
#include "capture/CaptureHudPalette.h"
#include "capture/CaptureInteractionPolicy.h"
#include "capture/CaptureVectorIcons.h"
#include "capture/FloatingGlassBar.h"
#include "capture/CornerRadiusHelper.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"
#include "core/config/ConfigManager.h"
#include "core/utils/ThemeUtils.h"
#include "capture/CaptureHistory.h"
#include "capture/CaptureDropdownMenu.h"
#include "capture/CaptureToolbarLayout.h"
#include "capture/HudAvoidanceEngine.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/ScrollCaptureEditorOverlay.h"
#include "capture/MarkupBaseHelper.h"
#include "capture/NumberBadgeFormatter.h"
#include "ui/ToastStyle.h"
#include <d2d1_1.h>
#include <format>
#include <algorithm>
#include <cmath>
#include <utility>
#include <opencv2/imgproc.hpp>
#include <dxgi.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

using namespace Microsoft::WRL;

namespace tools3000::capture {

namespace {

bool isCaptureUiChinese() {
    return tools3000::core::isUiLanguageChinese();
}

void rgbToHsl(int r, int g, int b, int& h, int& s, int& l) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float maxVal = std::max({rf, gf, bf}), minVal = std::min({rf, gf, bf});
    float delta = maxVal - minVal;
    float lf = (maxVal + minVal) * 0.5f;
    float sf = 0.0f, hf = 0.0f;
    if (delta > 1e-5f) {
        sf = lf > 0.5f ? delta / (2.0f - maxVal - minVal) : delta / (maxVal + minVal);
        if (maxVal == rf) {
            hf = (gf - bf) / delta + (gf < bf ? 6.0f : 0.0f);
        } else if (maxVal == gf) {
            hf = (bf - rf) / delta + 2.0f;
        } else {
            hf = (rf - gf) / delta + 4.0f;
        }
        hf /= 6.0f;
    }
    h = static_cast<int>(std::round(hf * 360.0f)) % 360;
    s = static_cast<int>(std::round(sf * 100.0f));
    l = static_cast<int>(std::round(lf * 100.0f));
}

void rgbToHsv(int r, int g, int b, int& h, int& s, int& v) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float maxVal = std::max({rf, gf, bf}), minVal = std::min({rf, gf, bf});
    float delta = maxVal - minVal;
    float vf = maxVal;
    float sf = maxVal > 1e-5f ? delta / maxVal : 0.0f;
    float hf = 0.0f;
    if (delta > 1e-5f) {
        if (maxVal == rf) {
            hf = (gf - bf) / delta + (gf < bf ? 6.0f : 0.0f);
        } else if (maxVal == gf) {
            hf = (bf - rf) / delta + 2.0f;
        } else {
            hf = (rf - gf) / delta + 4.0f;
        }
        hf /= 6.0f;
    }
    h = static_cast<int>(std::round(hf * 360.0f)) % 360;
    s = static_cast<int>(std::round(sf * 100.0f));
    v = static_cast<int>(std::round(vf * 100.0f));
}

void rgbToCmyk(int r, int g, int b, int& c, int& m, int& y, int& k) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float kf = 1.0f - std::max({rf, gf, bf});
    if (kf < 1.0f - 1e-5f) {
        c = static_cast<int>(std::round((1.0f - rf - kf) / (1.0f - kf) * 100.0f));
        m = static_cast<int>(std::round((1.0f - gf - kf) / (1.0f - kf) * 100.0f));
        y = static_cast<int>(std::round((1.0f - bf - kf) / (1.0f - kf) * 100.0f));
    } else {
        c = m = y = 0;
    }
    k = static_cast<int>(std::round(kf * 100.0f));
}

struct ColorFormatEntry {
    ColorFormatType type;
    const wchar_t* tag;
    std::wstring value;
    std::string clipText;
};

std::vector<ColorFormatEntry> getAllColorFormats(int r, int g, int b) {
    int hslH, hslS, hslL;
    rgbToHsl(r, g, b, hslH, hslS, hslL);

    int hsvH, hsvS, hsvV;
    rgbToHsv(r, g, b, hsvH, hsvS, hsvV);

    int cmykC, cmykM, cmykY, cmykK;
    rgbToCmyk(r, g, b, cmykC, cmykM, cmykY, cmykK);

    uint32_t decVal = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);

    std::vector<ColorFormatEntry> list;
    list.push_back({
        ColorFormatType::HEX, L"HEX",
        std::format(L"#{:02X}{:02X}{:02X}", r, g, b),
        std::format("#{:02X}{:02X}{:02X}", r, g, b)
    });
    list.push_back({
        ColorFormatType::RGB, L"RGB",
        std::format(L"rgb({}, {}, {})", r, g, b),
        std::format("rgb({}, {}, {})", r, g, b)
    });
    list.push_back({
        ColorFormatType::RGBA, L"RGBA",
        std::format(L"rgba({}, {}, {}, 1.0)", r, g, b),
        std::format("rgba({}, {}, {}, 1.0)", r, g, b)
    });
    list.push_back({
        ColorFormatType::HEX_0x, L"0xHEX",
        std::format(L"0x{:02X}{:02X}{:02X}", r, g, b),
        std::format("0x{:02X}{:02X}{:02X}", r, g, b)
    });
    list.push_back({
        ColorFormatType::HSL, L"HSL",
        std::format(L"hsl({}, {}%, {}%)", hslH, hslS, hslL),
        std::format("hsl({}, {}%, {}%)", hslH, hslS, hslL)
    });
    list.push_back({
        ColorFormatType::HSV, L"HSV",
        std::format(L"hsv({}, {}%, {}%)", hsvH, hsvS, hsvV),
        std::format("hsv({}, {}%, {}%)", hsvH, hsvS, hsvV)
    });
    list.push_back({
        ColorFormatType::CMYK, L"CMYK",
        std::format(L"{}%, {}%, {}%, {}%", cmykC, cmykM, cmykY, cmykK),
        std::format("cmyk({}%, {}%, {}%, {}%)", cmykC, cmykM, cmykY, cmykK)
    });
    list.push_back({
        ColorFormatType::DEC, L"DEC",
        std::format(L"{}", decVal),
        std::format("{}", decVal)
    });

    return list;
}

/// DirectWrite 文本格式作用域保护 RAII，确保文本对齐、段落对齐与换行策略不跨模块污染
struct TextFormatScope {
    IDWriteTextFormat* format = nullptr;
    DWRITE_TEXT_ALIGNMENT origText = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT origPara = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    DWRITE_WORD_WRAPPING origWrap = DWRITE_WORD_WRAPPING_NO_WRAP;

    TextFormatScope(IDWriteTextFormat* fmt,
                    DWRITE_TEXT_ALIGNMENT text = DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                    DWRITE_WORD_WRAPPING wrap = DWRITE_WORD_WRAPPING_NO_WRAP)
        : format(fmt) {
        if (format) {
            origText = format->GetTextAlignment();
            origPara = format->GetParagraphAlignment();
            origWrap = format->GetWordWrapping();
            format->SetTextAlignment(text);
            format->SetParagraphAlignment(para);
            format->SetWordWrapping(wrap);
        }
    }

    ~TextFormatScope() {
        if (format) {
            format->SetTextAlignment(origText);
            format->SetParagraphAlignment(origPara);
            format->SetWordWrapping(origWrap);
        }
    }
};

} // namespace

D2D1_SIZE_F CaptureRenderer::measureText(const std::wstring& text, IDWriteTextFormat* format, float maxWidth, float maxHeight) const {
    if (!m_dwriteFactory || text.empty()) return D2D1::SizeF(0.0f, 0.0f);
    IDWriteTextFormat* fmt = format ? format : m_infoTextFormat.Get();
    if (!fmt) return D2D1::SizeF(0.0f, 0.0f);

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), fmt,
            maxWidth, maxHeight, layout.GetAddressOf())) && layout) {
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(layout->GetMetrics(&m))) {
            return D2D1::SizeF(m.widthIncludingTrailingWhitespace, m.height);
        }
    }
    return D2D1::SizeF(static_cast<float>(text.size()) * 12.0f, 18.0f);
}

bool CaptureRenderer::initialize(HWND hwnd, CaptureState& state) {
    releaseWindowResources();
    m_hwnd = hwnd;
    if (createRenderResources(state)) return true;
    releaseWindowResources();
    return false;
}

void CaptureRenderer::shutdown() {
    releaseWindowResources();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
}

void CaptureRenderer::applyThemeColors() {
    if (!m_renderTarget) return;

    // 截图与录屏选区采用纯净零色偏白晶主线，杜绝彩色边框干扰被截画面原色判断
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f),
        m_borderBrush.ReleaseAndGetAddressOf()
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.62f),
        m_crosshairBrush.ReleaseAndGetAddressOf()
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f),
        m_windowHighlightBrush.ReleaseAndGetAddressOf()
    );
}

bool CaptureRenderer::createRenderResources(CaptureState& state) {
    HRESULT hr;

    if (!m_d2dFactory) {
        hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf());
        if (FAILED(hr)) return false;
    }

    if (!m_dwriteFactory) {
        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
        if (FAILED(hr)) return false;
    }

    if (!updateDpiScale(state.dpiScale)) return false;

    RECT rc;
    GetClientRect(m_hwnd, &rc);

    auto rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    auto hwndProps = D2D1::HwndRenderTargetProperties(
        m_hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top),
        D2D1_PRESENT_OPTIONS_IMMEDIATELY
    );

    hr = m_d2dFactory->CreateHwndRenderTarget(rtProps, hwndProps, m_renderTarget.GetAddressOf());
    if (FAILED(hr)) return false;
    
    // 禁用 D2D 的自动 DPI 缩放
    m_renderTarget->SetDpi(96.0f, 96.0f);

    // 画笔（信息浮层文本统一对齐雪瓷白底高对比曜石黑令牌）
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.5f), m_dimBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudBg, m_infoBgBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, m_infoTextBrush.GetAddressOf());
    applyThemeColors();

    return true;
}

void CaptureRenderer::releaseWindowResources() {
    m_windowHighlightBrush.Reset();
    m_crosshairBrush.Reset();
    m_infoTextBrush.Reset();
    m_infoBgBrush.Reset();
    m_borderBrush.Reset();
    m_dimBrush.Reset();
    m_screenBitmap.Reset();
    m_screenTiles.clear();
    m_screenTilesW = 0;
    m_screenTilesH = 0;
    m_markupCacheBitmap.Reset();
    m_markupCacheTiles.clear();
    m_markupTilesW = 0;
    m_markupTilesH = 0;
    m_minimapBitmap.Reset();
    m_minimapSrcW = 0;
    m_minimapSrcH = 0;
    m_historyBitmap.Reset();
    m_markupClipLayer.Reset();
    m_numberBadgeSmallFormat.Reset();
    m_numberBadgeMediumFormat.Reset();
    m_numberBadgeLargeFormat.Reset();
    m_textInputFormat.Reset();
    m_hudBadgeFormat.Reset();
    m_hudPrimaryFormat.Reset();
    m_infoTextFormat.Reset();
    m_textScale = 0.0f;
    m_renderTarget.Reset();
    m_hwnd = nullptr;
}

bool CaptureRenderer::createTextFormatsForScale(float scale) {
    if (!m_dwriteFactory) return false;

    const wchar_t* uiFontFamily = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());

    // 1. 通用工具栏胶囊按钮 / 弹出菜单 / 滑块控件字体：黄金 13.0px + 强制不折行，彻底消除文字撑破内框与折行重叠
    ComPtr<IDWriteTextFormat> infoFormat;
    HRESULT hr = m_dwriteFactory->CreateTextFormat(
        uiFontFamily, nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13.0f * scale, L"zh-CN",
        infoFormat.GetAddressOf());
    if (FAILED(hr) || !infoFormat) return false;
    infoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    infoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    infoFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 2. 尺寸仪表主读数字体 (W × H) 与顶部主提示：16.0px 醒目高对比
    ComPtr<IDWriteTextFormat> hudPrimaryFormat;
    hr = m_dwriteFactory->CreateTextFormat(
        uiFontFamily, nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 16.0f * scale, L"zh-CN",
        hudPrimaryFormat.GetAddressOf());
    if (FAILED(hr) || !hudPrimaryFormat) return false;
    hudPrimaryFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    hudPrimaryFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hudPrimaryFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 3. 尺寸单位后缀 (px / dp)、坐标、微晶徽章与辅助说明字体：12.5px 精致次级层级
    ComPtr<IDWriteTextFormat> hudBadgeFormat;
    hr = m_dwriteFactory->CreateTextFormat(
        uiFontFamily, nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.5f * scale, L"zh-CN",
        hudBadgeFormat.GetAddressOf());
    if (FAILED(hr) || !hudBadgeFormat) return false;
    hudBadgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    hudBadgeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hudBadgeFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 4. 画布文本标注输入基准字体
    ComPtr<IDWriteTextFormat> textInputFormat;
    hr = m_dwriteFactory->CreateTextFormat(
        uiFontFamily, nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 18.0f * scale, L"zh-CN",
        textInputFormat.GetAddressOf());
    if (FAILED(hr) || !textInputFormat) return false;
    textInputFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    textInputFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    // 5. 序号标注实时拖拽徽章三档缓存字体（字号底线 >= 12px，杜绝渲染热路径每帧 CreateTextFormat）
    auto makeNumberBadgeFormat = [&](float basePx, ComPtr<IDWriteTextFormat>& outFmt) -> bool {
        if (FAILED(m_dwriteFactory->CreateTextFormat(
                uiFontFamily, nullptr,
                DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, (std::max)(12.0f, basePx) * scale, L"zh-CN",
                outFmt.GetAddressOf())) || !outFmt) {
            return false;
        }
        outFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        outFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        outFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };

    ComPtr<IDWriteTextFormat> numLargeFmt, numMediumFmt, numSmallFmt;
    if (!makeNumberBadgeFormat(15.0f, numLargeFmt) ||
        !makeNumberBadgeFormat(13.5f, numMediumFmt) ||
        !makeNumberBadgeFormat(12.0f, numSmallFmt)) {
        return false;
    }

    m_infoTextFormat = std::move(infoFormat);
    m_hudPrimaryFormat = std::move(hudPrimaryFormat);
    m_hudBadgeFormat = std::move(hudBadgeFormat);
    m_textInputFormat = std::move(textInputFormat);
    m_numberBadgeLargeFormat = std::move(numLargeFmt);
    m_numberBadgeMediumFormat = std::move(numMediumFmt);
    m_numberBadgeSmallFormat = std::move(numSmallFmt);
    m_textScale = scale;
    return true;
}

bool CaptureRenderer::updateDpiScale(float scale) {
    scale = std::clamp(scale > 0.0f ? scale : 1.0f, 1.0f, 5.0f);
    if (m_infoTextFormat && m_hudPrimaryFormat && m_hudBadgeFormat && m_textInputFormat &&
        m_numberBadgeLargeFormat && m_numberBadgeMediumFormat && m_numberBadgeSmallFormat &&
        std::abs(scale - m_textScale) < 0.01f) {
        return true;
    }
    return createTextFormatsForScale(scale);
}

bool CaptureRenderer::rebuildBitmapTiles(const cv::Mat& bgra,
                                         D2D1_ALPHA_MODE alphaMode,
                                         std::vector<BitmapTile>& tiles,
                                         int& outW,
                                         int& outH) {
    tiles.clear();
    outW = 0;
    outH = 0;
    if (!m_renderTarget || bgra.empty() || bgra.channels() != 4) {
        return false;
    }

    constexpr int kMaxTileHeight = 4096;
    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alphaMode));

    for (int y = 0; y < bgra.rows; y += kMaxTileHeight) {
        const int sliceH = (std::min)(kMaxTileHeight, bgra.rows - y);
        cv::Mat slice = bgra.rowRange(y, y + sliceH);
        if (!slice.isContinuous()) {
            slice = slice.clone();
        }
        BitmapTile tile;
        tile.yOffset = y;
        tile.width = slice.cols;
        tile.height = sliceH;
        const HRESULT hr = m_renderTarget->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(slice.cols), static_cast<UINT32>(sliceH)),
            slice.data,
            static_cast<UINT32>(slice.step[0]),
            props,
            tile.bitmap.GetAddressOf());
        if (FAILED(hr) || !tile.bitmap) {
            tiles.clear();
            return false;
        }
        tiles.push_back(std::move(tile));
    }

    outW = bgra.cols;
    outH = bgra.rows;
    return !tiles.empty();
}

void CaptureRenderer::drawTiledRegion(const std::vector<BitmapTile>& tiles,
                                      int totalW,
                                      int totalH,
                                      const D2D1_RECT_F& dstRect,
                                      const D2D1_RECT_F& srcRect,
                                      D2D1_BITMAP_INTERPOLATION_MODE interp,
                                      bool highQualityCubic) {
    if (!m_renderTarget || tiles.empty() || totalW <= 0 || totalH <= 0) return;
    const float srcW = srcRect.right - srcRect.left;
    const float srcH = srcRect.bottom - srcRect.top;
    const float dstW = dstRect.right - dstRect.left;
    const float dstH = dstRect.bottom - dstRect.top;
    if (srcW <= 0.001f || srcH <= 0.001f || dstW <= 0.001f || dstH <= 0.001f) return;

    const float scaleX = dstW / srcW;
    const float scaleY = dstH / srcH;
    const float clampedSx0 = std::clamp(srcRect.left, 0.0f, static_cast<float>(totalW));
    const float clampedSx1 = std::clamp(srcRect.right, 0.0f, static_cast<float>(totalW));
    if (clampedSx1 <= clampedSx0) return;

    const float tileDstLeft = dstRect.left + (clampedSx0 - srcRect.left) * scaleX;
    const float tileDstRight = dstRect.left + (clampedSx1 - srcRect.left) * scaleX;

    ComPtr<ID2D1DeviceContext> dc;
    if (highQualityCubic) {
        m_renderTarget.As(&dc);
    }

    for (const auto& tile : tiles) {
        if (!tile.bitmap || tile.height <= 0) continue;
        const float ty0 = static_cast<float>(tile.yOffset);
        const float ty1 = static_cast<float>(tile.yOffset + tile.height);
        const float iy0 = (std::max)(srcRect.top, ty0);
        const float iy1 = (std::min)(srcRect.bottom, ty1);
        if (iy1 <= iy0) continue;

        const D2D1_RECT_F tileSrc = D2D1::RectF(clampedSx0, iy0 - ty0, clampedSx1, iy1 - ty0);
        const D2D1_RECT_F tileDst = D2D1::RectF(
            tileDstLeft,
            dstRect.top + (iy0 - srcRect.top) * scaleY,
            tileDstRight,
            dstRect.top + (iy1 - srcRect.top) * scaleY);

        if (dc) {
            dc->DrawBitmap(tile.bitmap.Get(), &tileDst, 1.0f,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &tileSrc, nullptr);
        } else {
            m_renderTarget->DrawBitmap(tile.bitmap.Get(), tileDst, 1.0f, interp, tileSrc);
        }
    }
}

bool CaptureRenderer::updateScreenBitmap(const cv::Mat& image) {
    m_screenBitmap.Reset();
    m_screenTiles.clear();
    m_screenTilesW = 0;
    m_screenTilesH = 0;
    m_minimapBitmap.Reset();
    m_minimapSrcW = 0;
    m_minimapSrcH = 0;
    if (!m_renderTarget || image.empty()) {
        LOG_ERROR("截图底图上传失败: 渲染目标或图像为空");
        return false;
    }

    cv::Mat bgra;
    D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    if (image.channels() == 4) {
        bgra = image;
        alphaMode = D2D1_ALPHA_MODE_IGNORE;
    } else if (image.channels() == 3) {
        cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
    } else if (image.channels() == 1) {
        cv::cvtColor(image, bgra, cv::COLOR_GRAY2BGRA);
    } else {
        LOG_ERROR("截图底图上传失败: 不支持的通道数={}", image.channels());
        return false;
    }

    if (!bgra.isContinuous()) bgra = bgra.clone();
    if (!rebuildBitmapTiles(bgra, alphaMode, m_screenTiles, m_screenTilesW, m_screenTilesH)) {
        LOG_ERROR("截图底图上传 Direct2D 失败, hr=0x{:08X}", static_cast<unsigned>(E_FAIL));
        return false;
    }

    // 单屏常规尺寸同步创建/复用单纹理快路径（超过 16384px 的超长拼接图完全依赖 4096px 瓦片流，零溢出）
    if (bgra.cols <= 16384 && bgra.rows <= 16384) {
        if (m_screenTiles.size() == 1) {
            m_screenBitmap = m_screenTiles.front().bitmap;
        } else {
            const auto props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alphaMode));
            m_renderTarget->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(bgra.cols), static_cast<UINT32>(bgra.rows)),
                bgra.data, static_cast<UINT32>(bgra.step[0]), props, m_screenBitmap.GetAddressOf());
        }
    }

    invalidate();
    return true;
}

void CaptureRenderer::drawDimOverlay(const D2D1_RECT_F& selRect, CaptureState& state) {
    auto size = m_renderTarget->GetSize();
    const float scale = (state.dpiScale > 0.0f ? state.dpiScale : 1.0f);
    
    // 如果启用了美化外壳且留白大于0，遮罩打洞区域需扩展至外壳边界
    D2D1_RECT_F holeRect = selRect;
    float holeRadius = state.cornerRadius * scale;
    if (state.beautyShell.enabled && state.beautyShell.padding > 0 && state.mode != OverlayMode::RecordRegion) {
        float pad = state.beautyShell.padding * scale;
        holeRect = D2D1::RectF(selRect.left - pad, selRect.top - pad, selRect.right + pad, selRect.bottom + pad);
        holeRadius = (std::max)(10.0f * scale, (state.beautyShell.cornerRadius + 6.0f) * scale);
    } else if (state.beautyShell.enabled) {
        holeRadius = state.beautyShell.cornerRadius * scale;
    }

    if (holeRadius > 0.5f && m_d2dFactory) {
        // 圆角遮罩打洞：排除选区圆角矩形
        auto rounded = D2D1::RoundedRect(holeRect, holeRadius, holeRadius);
        ComPtr<ID2D1RoundedRectangleGeometry> geo;
        if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(rounded, geo.GetAddressOf())) && geo) {
            ComPtr<ID2D1RectangleGeometry> fullGeo;
            if (SUCCEEDED(m_d2dFactory->CreateRectangleGeometry(D2D1::RectF(0, 0, size.width, size.height), fullGeo.GetAddressOf())) && fullGeo) {
                ComPtr<ID2D1PathGeometry> path;
                if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(path.GetAddressOf())) && path) {
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(path->Open(sink.GetAddressOf())) && sink) {
                        fullGeo->CombineWithGeometry(geo.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
                        sink->Close();
                        m_renderTarget->FillGeometry(path.Get(), m_dimBrush.Get());
                        return;
                    }
                }
            }
        }
    }

    // 上
    m_renderTarget->FillRectangle(D2D1::RectF(0, 0, size.width, holeRect.top), m_dimBrush.Get());
    // 下
    m_renderTarget->FillRectangle(D2D1::RectF(0, holeRect.bottom, size.width, size.height), m_dimBrush.Get());
    // 左
    m_renderTarget->FillRectangle(D2D1::RectF(0, holeRect.top, holeRect.left, holeRect.bottom), m_dimBrush.Get());
    // 右
    m_renderTarget->FillRectangle(D2D1::RectF(holeRect.right, holeRect.top, size.width, holeRect.bottom), m_dimBrush.Get());
}

void CaptureRenderer::drawSelection(const D2D1_RECT_F& rect, CaptureState& state) {
    const float scale = std::clamp(
        state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    float effectiveRadius = state.effectiveCornerRadius();
    float radius = effectiveRadius * scale;

    ComPtr<ID2D1SolidColorBrush> slateShadowBrush, slateRingBrush, handleWhiteBrush;
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.34f),
        slateShadowBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, 0.86f),
        slateRingBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        handleWhiteBrush.GetAddressOf());

    // 1. 选区边框：全天候无色偏双层白晶框（外衬细腻石板微影防纯白网页融边，内叠清脆白晶主线零色偏）
    if (!(state.beautyShell.enabled && state.beautyShell.padding > 0 && state.mode != OverlayMode::RecordRegion)) {
        if (radius > 0.5f) {
            auto rrect = D2D1::RoundedRect(rect, radius, radius);
            if (slateShadowBrush) {
                m_renderTarget->DrawRoundedRectangle(rrect, slateShadowBrush.Get(), 2.6f * scale);
            }
            m_renderTarget->DrawRoundedRectangle(rrect, m_borderBrush.Get(), 1.6f * scale);
        } else {
            if (slateShadowBrush) {
                m_renderTarget->DrawRectangle(rect, slateShadowBrush.Get(), 2.6f * scale);
            }
            m_renderTarget->DrawRectangle(rect, m_borderBrush.Get(), 1.6f * scale);
        }
    }

    // 2. 控制点：直角选区展示八个锚点；开启圆角 (effectiveRadius > 2px) 时隐藏四角悬空圆点，仅展示紧贴四条直边的四个中点锚点
    float cx = (rect.left + rect.right) / 2;
    float cy = (rect.top + rect.bottom) / 2;

    D2D1_POINT_2F controls[] = {
        {rect.left, rect.top}, {cx, rect.top}, {rect.right, rect.top},
        {rect.left, cy}, {rect.right, cy},
        {rect.left, rect.bottom}, {cx, rect.bottom}, {rect.right, rect.bottom}
    };

    const bool hideCornerDots = (effectiveRadius > 2.0f * scale);
    const float dotRadius = 4.5f * scale;
    for (size_t idx = 0; idx < 8; ++idx) {
        if (hideCornerDots && (idx == 0 || idx == 2 || idx == 5 || idx == 7)) {
            continue;
        }
        const auto& pt = controls[idx];
        if (slateShadowBrush) {
            m_renderTarget->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(pt.x, pt.y + 0.8f * scale), dotRadius + 0.8f * scale, dotRadius + 0.8f * scale),
                slateShadowBrush.Get());
        }
        auto ellipse = D2D1::Ellipse(pt, dotRadius, dotRadius);
        if (handleWhiteBrush) {
            m_renderTarget->FillEllipse(ellipse, handleWhiteBrush.Get());
        }
        if (slateRingBrush) {
            m_renderTarget->DrawEllipse(ellipse, slateRingBrush.Get(), 1.35f * scale);
        }
    }

    // 3. Figma / PixPin 级内侧圆角调节手柄（小选区内部彻底隐藏手柄，避免遮挡微型内容，改由外部工具栏调节）
    drawCornerRadiusHandleVisual(rect, effectiveRadius, scale, state.isAdjustingCornerRadius, state.cornerDragStartPos, state.currentCursor, 64.0f, state.cornerDragIndex);
}

void CaptureRenderer::drawCornerRadiusHandleVisual(
    const D2D1_RECT_F& rect,
    float cornerRadius,
    float scale,
    bool isDragging,
    POINT dragStartPos,
    POINT currentCursor,
    float minDimension,
    int cornerIndex)
{
    const float selW = rect.right - rect.left;
    const float selH = rect.bottom - rect.top;
    if (!CornerRadiusHelper::canShowHandles(selW, selH, scale, minDimension)) {
        return;
    }

    auto handles = CornerRadiusHelper::getCornerHandles(
        rect.left, rect.top, rect.right, rect.bottom,
        cornerRadius, scale, false);
    if (handles.empty()) return;

    // Figma 纯净隐形流：平时选区内部 100% 干净零圆点，仅在鼠标移近内角或拖拽时轻盈浮现当前单角
    const CornerProximityState proximity = CornerRadiusHelper::evaluateProximity(
        handles,
        static_cast<float>(currentCursor.x),
        static_cast<float>(currentCursor.y),
        scale,
        isDragging,
        cornerIndex,
        static_cast<float>(dragStartPos.x),
        static_cast<float>(dragStartPos.y));

    if (proximity.activeCornerIndex < 0 ||
        proximity.activeCornerIndex >= static_cast<int>(handles.size()) ||
        proximity.visibilityAlpha <= 0.01f) {
        return;
    }

    const float s = (std::max)(0.5f, scale);
    const float vis = std::clamp(proximity.visibilityAlpha, 0.0f, 1.0f);
    const bool activeFocus = proximity.isDirectHover || isDragging;
    const auto& cpt = handles[proximity.activeCornerIndex];

    const auto withAlpha = [](D2D1_COLOR_F c, float a) noexcept {
        c.a = std::clamp(c.a * a, 0.0f, 1.0f);
        return c;
    };

    ComPtr<ID2D1StrokeStyle> roundStroke;
    if (m_d2dFactory) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND);
        m_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, roundStroke.GetAddressOf());
    }

    // 1. 贴合选区真实圆角的 90 度丝滑曲率高光导弧 (Osculating Corner Curvature Arc)
    if (m_d2dFactory) {
        const bool hasRealRadius = (cornerRadius > 0.5f);
        const float inset = hasRealRadius ? 0.0f : (3.2f * s);
        const float maxArcR = (std::max)(4.0f * s, (std::min)(selW, selH) * 0.5f - inset);
        const float arcR = hasRealRadius
            ? std::clamp(cornerRadius * s, 4.0f * s, maxArcR)
            : (std::min)(9.5f * s, maxArcR);

        D2D1_POINT_2F arcStart{};
        D2D1_POINT_2F arcEnd{};
        switch (cpt.cornerIndex) {
            case 0: // LT: 左切点 -> 上切点 (顺时针)
                arcStart = D2D1::Point2F(rect.left + inset, rect.top + inset + arcR);
                arcEnd   = D2D1::Point2F(rect.left + inset + arcR, rect.top + inset);
                break;
            case 1: // RT: 上切点 -> 右切点 (顺时针)
                arcStart = D2D1::Point2F(rect.right - inset - arcR, rect.top + inset);
                arcEnd   = D2D1::Point2F(rect.right - inset, rect.top + inset + arcR);
                break;
            case 2: // RB: 右切点 -> 下切点 (顺时针)
                arcStart = D2D1::Point2F(rect.right - inset, rect.bottom - inset - arcR);
                arcEnd   = D2D1::Point2F(rect.right - inset - arcR, rect.bottom - inset);
                break;
            default: // LB: 下切点 -> 左切点 (顺时针)
                arcStart = D2D1::Point2F(rect.left + inset + arcR, rect.bottom - inset);
                arcEnd   = D2D1::Point2F(rect.left + inset, rect.bottom - inset - arcR);
                break;
        }

        ComPtr<ID2D1PathGeometry> arcGeo;
        if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(arcGeo.GetAddressOf())) && arcGeo) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(arcGeo->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(arcStart, D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment(
                    arcEnd,
                    D2D1::SizeF(arcR, arcR),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
            }

            const float arcAlpha = activeFocus ? (hasRealRadius ? 0.96f : 0.72f) : (0.55f * vis);
            ComPtr<ID2D1SolidColorBrush> arcHaloBrush, arcIrisBrush;
            m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.52f * arcAlpha), arcHaloBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(
                withAlpha(CaptureHudPalette::kCornerGuideArrow, arcAlpha), arcIrisBrush.GetAddressOf());

            if (arcHaloBrush) {
                m_renderTarget->DrawGeometry(
                    arcGeo.Get(), arcHaloBrush.Get(), (activeFocus ? 3.4f : 2.8f) * s, roundStroke.Get());
            }
            if (arcIrisBrush) {
                m_renderTarget->DrawGeometry(
                    arcGeo.Get(), arcIrisBrush.Get(), (activeFocus ? 2.1f : 1.6f) * s, roundStroke.Get());
            }
        }
    }

    // 2. 温润珍珠微晶圆环锚点 (Pearl & Graphite Ring Handle — 零杂色偏，明暗底皆清透立体)
    const float ringR = (isDragging ? 5.6f : (proximity.isDirectHover ? 5.0f : 4.3f)) * s;
    const D2D1_POINT_2F centerPt = D2D1::Point2F(cpt.x, cpt.y);

    // 2a. 悬停/拖拽纯净白晶呼吸柔光晕
    if (activeFocus) {
        ComPtr<ID2D1SolidColorBrush> haloBrush;
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, isDragging ? 0.34f : 0.24f),
            haloBrush.GetAddressOf());
        if (haloBrush) {
            m_renderTarget->FillEllipse(
                D2D1::Ellipse(centerPt, ringR + 3.2f * s, ringR + 3.2f * s), haloBrush.Get());
        }
    }

    // 2b. 极轻向下水滴悬浮微影 + 纯白珠体 + 深石墨精致内环
    ComPtr<ID2D1SolidColorBrush> shadowBrush, silverRimBrush, pearlWhiteBrush, irisRingBrush;
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.22f * vis), shadowBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(71.0f / 255.0f, 85.0f / 255.0f, 105.0f / 255.0f, 0.32f * vis), silverRimBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.99f * vis), pearlWhiteBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, (activeFocus ? 0.94f : 0.80f) * vis),
        irisRingBrush.GetAddressOf());

    if (shadowBrush) {
        m_renderTarget->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cpt.x, cpt.y + 1.0f * s), ringR + 1.1f * s, ringR + 1.1f * s),
            shadowBrush.Get());
    }
    if (pearlWhiteBrush) {
        m_renderTarget->FillEllipse(D2D1::Ellipse(centerPt, ringR, ringR), pearlWhiteBrush.Get());
    }
    if (silverRimBrush) {
        m_renderTarget->DrawEllipse(
            D2D1::Ellipse(centerPt, ringR + 0.35f * s, ringR + 0.35f * s), silverRimBrush.Get(), 0.7f * s);
    }
    if (irisRingBrush) {
        if (isDragging) {
            // 拖拽态点亮深石墨实心微芯
            m_renderTarget->FillEllipse(
                D2D1::Ellipse(centerPt, 2.3f * s, 2.3f * s), irisRingBrush.Get());
        } else {
            // 悬停/近场态呈现通透空心石墨内环
            const float innerRingR = (std::max)(1.6f * s, ringR - 1.65f * s);
            m_renderTarget->DrawEllipse(
                D2D1::Ellipse(centerPt, innerRingR, innerRingR), irisRingBrush.Get(), 1.35f * s);
        }
    }

    // 3. 温润雪瓷白药丸胶囊读数 [ R 12 ]（仅在直接悬停锚点或拖拽推拉中轻盈浮现，松手即隐）
    if (activeFocus && selW >= 120.0f * s && selH >= 120.0f * s && m_infoTextFormat && m_dwriteFactory) {
        const int radVal = static_cast<int>(std::round(cornerRadius));
        const std::wstring prefixStr = L"R ";
        const std::wstring numStr = std::format(L"{}", radVal);

        auto measureWidth = [&](const std::wstring& wstr) -> float {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    wstr.c_str(), static_cast<UINT32>(wstr.size()),
                    m_infoTextFormat.Get(), 240.0f * s, 40.0f * s, layout.GetAddressOf())) && layout) {
                DWRITE_TEXT_METRICS m{};
                if (SUCCEEDED(layout->GetMetrics(&m))) {
                    return (std::max)(m.width, m.widthIncludingTrailingWhitespace);
                }
            }
            return static_cast<float>(wstr.size()) * 7.5f * s;
        };

        const float prefixW = measureWidth(prefixStr);
        const float numW = measureWidth(numStr);
        const float padX = 8.5f * s;
        const float badgeW = std::ceil(prefixW + numW + padX * 2.0f);
        const float badgeH = 21.0f * s;
        const float pillRadius = badgeH * 0.5f;

        const float badgeHalfDiag = std::hypot(badgeW * 0.5f, badgeH * 0.5f);
        const float badgeDist = ringR + badgeHalfDiag + 8.5f * s;
        float bx = cpt.x + cpt.inDirX * badgeDist - badgeW * 0.5f;
        float by = cpt.y + cpt.inDirY * badgeDist - badgeH * 0.5f;

        const float minBx = rect.left + 6.0f * s;
        const float maxBx = rect.right - badgeW - 6.0f * s;
        const float minBy = rect.top + 6.0f * s;
        const float maxBy = rect.bottom - badgeH - 6.0f * s;
        if (minBx <= maxBx) bx = std::clamp(bx, minBx, maxBx);
        if (minBy <= maxBy) by = std::clamp(by, minBy, maxBy);

        const D2D1_RECT_F badgeRect = D2D1::RectF(bx, by, bx + badgeW, by + badgeH);
        const D2D1_ROUNDED_RECT badgePill = D2D1::RoundedRect(badgeRect, pillRadius, pillRadius);

        ComPtr<ID2D1SolidColorBrush> badgeShadow, badgeBg, badgeBorder, prefixBrush, numBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.16f), badgeShadow.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudBg, badgeBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudBorder, badgeBorder.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSecondaryText, prefixBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, numBrush.GetAddressOf());

        if (badgeShadow) {
            D2D1_RECT_F sRect = D2D1::RectF(badgeRect.left, badgeRect.top + 1.5f * s, badgeRect.right, badgeRect.bottom + 2.5f * s);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sRect, pillRadius, pillRadius), badgeShadow.Get());
        }
        if (badgeBg) m_renderTarget->FillRoundedRectangle(badgePill, badgeBg.Get());
        if (badgeBorder) m_renderTarget->DrawRoundedRectangle(badgePill, badgeBorder.Get(), 0.95f * s);

        auto drawCenteredSegment = [&](const std::wstring& seg, float segLeft, float segW, ID2D1Brush* brush) {
            if (!brush) return;
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    seg.c_str(), static_cast<UINT32>(seg.size()),
                    m_infoTextFormat.Get(), segW + 8.0f * s, badgeH, layout.GetAddressOf())) && layout) {
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawTextLayout(D2D1::Point2F(segLeft, by), layout.Get(), brush);
            }
        };

        drawCenteredSegment(prefixStr, bx + padX, prefixW, prefixBrush.Get());
        drawCenteredSegment(numStr, bx + padX + prefixW, numW, numBrush.Get());
    }
}

void CaptureRenderer::drawSizeInfo(const D2D1_RECT_F& rect, CaptureState& state) {
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const bool zh = isCaptureUiChinese();
    const auto overlayState = state.state.load();
    const bool hasLockedSelection =
        ((int)overlayState == (int)OverlayState::Selected ||
         (int)overlayState == (int)OverlayState::Marking);

    int rawW = static_cast<int>(std::round(rect.right - rect.left));
    int rawH = static_cast<int>(std::round(rect.bottom - rect.top));
    int posX = hasLockedSelection
        ? static_cast<int>(std::round(rect.left))
        : static_cast<int>(state.currentCursor.x > 0 ? state.currentCursor.x : rect.left);
    int posY = hasLockedSelection
        ? static_cast<int>(std::round(rect.top))
        : static_cast<int>(state.currentCursor.y > 0 ? state.currentCursor.y : rect.top);

    // 根据 px / dp 单位换算，将主数字与单位后缀拆分为独立排印层级
    const bool isDpMode = (state.sizeUnit == CaptureState::SizeUnit::DeviceIndependentPixel);
    int displayW = isDpMode
        ? static_cast<int>(std::round(rawW / (state.dpiScale > 0.0f ? state.dpiScale : 1.0f))) : rawW;
    int displayH = isDpMode
        ? static_cast<int>(std::round(rawH / (state.dpiScale > 0.0f ? state.dpiScale : 1.0f))) : rawH;
    const std::wstring unitTagStr = state.showUnitInHud ? (isDpMode ? L"dp" : L"px") : L"";

    const std::wstring posStr = state.showPositionInHud
        ? std::format(L"{}, {}", posX, posY) : L"";
    const std::wstring mainSizeStr = std::format(L"{} \u00D7 {}", displayW, displayH);

    std::vector<std::wstring> badges;
    if (state.aspectRatio != AspectRatioPreset::Free) {
        switch (state.aspectRatio) {
            case AspectRatioPreset::Ratio_16_9:   badges.push_back(L"16:9"); break;
            case AspectRatioPreset::Ratio_4_3:    badges.push_back(L"4:3"); break;
            case AspectRatioPreset::Ratio_1_1:    badges.push_back(L"1:1"); break;
            case AspectRatioPreset::Ratio_Golden: badges.push_back(L"1.618:1"); break;
            default: break;
        }
    }
    const float effectiveRadius = state.effectiveCornerRadius();
    if (effectiveRadius > 0.5f) {
        badges.push_back(std::format(L"R{}px", static_cast<int>(effectiveRadius)));
    }
    if (state.beautyShell.enabled) {
        badges.push_back(zh ? L"外壳" : L"Shell");
    }

    auto measureSpanWidth = [&](const std::wstring& txt, IDWriteTextFormat* fmt, float fallbackPerChar) -> float {
        if (txt.empty()) return 0.0f;
        float w = static_cast<float>(txt.size()) * fallbackPerChar * scale;
        if (m_dwriteFactory && fmt) {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    txt.c_str(), static_cast<UINT32>(txt.size()),
                    fmt, 2000.0f * scale, 60.0f * scale, layout.GetAddressOf())) && layout) {
                DWRITE_TEXT_METRICS tm{};
                if (SUCCEEDED(layout->GetMetrics(&tm)) && tm.width > 0.0f) {
                    w = tm.widthIncludingTrailingWhitespace > tm.width
                        ? tm.widthIncludingTrailingWhitespace : tm.width;
                }
            }
        }
        return std::ceil(w);
    };

    IDWriteTextFormat* primaryFmt = m_hudPrimaryFormat ? m_hudPrimaryFormat.Get() : m_infoTextFormat.Get();
    IDWriteTextFormat* badgeFmt = m_hudBadgeFormat ? m_hudBadgeFormat.Get() : m_infoTextFormat.Get();

    const float padX = 13.0f * scale;
    const float iconSz = 14.0f * scale;
    const float iconGap = 8.0f * scale;
    const float dividerZone = 13.0f * scale;
    const float badgePadX = 7.0f * scale;
    const float badgeGap = 6.0f * scale;
    const float chevronZone = 18.0f * scale;

    const float posW = posStr.empty() ? 0.0f : measureSpanWidth(posStr, badgeFmt, 7.6f);
    const float mainW = measureSpanWidth(mainSizeStr, primaryFmt, 9.6f);
    const float unitGap = unitTagStr.empty() ? 0.0f : (5.0f * scale);
    const float rawUnitW = unitTagStr.empty() ? 0.0f : measureSpanWidth(unitTagStr, badgeFmt, 7.4f);
    const float unitW = unitTagStr.empty()
        ? 0.0f
        : (isDpMode ? (rawUnitW + 10.0f * scale) : (rawUnitW + 2.0f * scale));

    std::vector<float> badgeWidths;
    badgeWidths.reserve(badges.size());
    float totalBadgesW = 0.0f;
    for (const auto& b : badges) {
        float bw = measureSpanWidth(b, badgeFmt, 8.0f) + badgePadX * 2.0f;
        badgeWidths.push_back(bw);
        totalBadgesW += badgeGap + bw;
    }

    const float labelW = padX + iconSz + iconGap +
                         (posW > 0.0f ? (posW + dividerZone) : 0.0f) +
                         mainW + unitGap + unitW + totalBadgesW + chevronZone + padX;
    const float labelH = 36.0f * scale;

    const auto surfaceSize = m_renderTarget->GetSize();

    // 仅在选区已锁定（Selected/Marking）时构建工具栏几何布局并提供工具栏障碍物矩形；
    // 在初始未选区（Selecting）悬停探测窗口预览阶段不构建工具栏，也不驱赶左下角快捷键 HUD。
    if (hasLockedSelection) {
        rebuildCaptureToolbar(state, rect, surfaceSize);
    }

    std::vector<HudObstacle> obstacles;
    if (hasLockedSelection) {
        if (state.primaryToolbarRect.right > state.primaryToolbarRect.left &&
            state.primaryToolbarRect.bottom > state.primaryToolbarRect.top) {
            obstacles.push_back({state.primaryToolbarRect, HudObstacleType::PrimaryToolbar, 2.5f});
        }
        if (!state.secondaryToolbarButtons.empty() &&
            state.secondaryToolbarRect.right > state.secondaryToolbarRect.left &&
            state.secondaryToolbarRect.bottom > state.secondaryToolbarRect.top) {
            obstacles.push_back({state.secondaryToolbarRect, HudObstacleType::SecondaryToolbar, 2.0f});
        }
        if (!state.selectionSideButtons.empty() &&
            state.selectionSideRect.right > state.selectionSideRect.left &&
            state.selectionSideRect.bottom > state.selectionSideRect.top) {
            obstacles.push_back({state.selectionSideRect, HudObstacleType::SecondaryToolbar, 2.0f});
        }
        if (state.openSubmenu != SubmenuType::None &&
            state.openSubmenuRect.right > state.openSubmenuRect.left &&
            state.openSubmenuRect.bottom > state.openSubmenuRect.top) {
            obstacles.push_back({state.openSubmenuRect, HudObstacleType::SecondaryToolbar, 2.2f});
        }
        if (state.sliderPopup.type != SliderPopupType::None &&
            state.sliderPopup.popupRect.right > state.sliderPopup.popupRect.left &&
            state.sliderPopup.popupRect.bottom > state.sliderPopup.popupRect.top) {
            obstacles.push_back({state.sliderPopup.popupRect, HudObstacleType::SecondaryToolbar, 2.2f});
        }
        if (state.dropdownMenu.type != DropdownType::None && !state.dropdownMenu.items.empty()) {
            D2D1_RECT_F dmRect = state.dropdownMenu.items.front().rect;
            for (const auto& item : state.dropdownMenu.items) {
                dmRect.left = (std::min)(dmRect.left, item.rect.left);
                dmRect.top = (std::min)(dmRect.top, item.rect.top);
                dmRect.right = (std::max)(dmRect.right, item.rect.right);
                dmRect.bottom = (std::max)(dmRect.bottom, item.rect.bottom);
            }
            obstacles.push_back({dmRect, HudObstacleType::SecondaryToolbar, 2.2f});
        }
        if (!state.detectedQrText.empty() &&
            state.qrChipRect.right > state.qrChipRect.left &&
            state.qrChipRect.bottom > state.qrChipRect.top) {
            obstacles.push_back({state.qrChipRect, HudObstacleType::SecondaryToolbar, 2.0f});
        }
    }
    if (ShortcutHintOverlay::instance().isVisible()) {
        RECT hintRect = ShortcutHintOverlay::instance().getBounds();
        if (hintRect.right > hintRect.left && hintRect.bottom > hintRect.top) {
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            RECT winRc{};
            if (m_hwnd && IsWindow(m_hwnd) && GetWindowRect(m_hwnd, &winRc)) {
                vx = winRc.left;
                vy = winRc.top;
            }
            obstacles.push_back({
                D2D1::RectF(static_cast<float>(hintRect.left - vx), static_cast<float>(hintRect.top - vy),
                            static_cast<float>(hintRect.right - vx), static_cast<float>(hintRect.bottom - vy)),
                HudObstacleType::ActiveAnnotation,
                2.0f
            });
        }
    }

    // 1. 注册 8 个调节手柄热区为障碍物，杜绝尺寸胶囊遮挡把手与圆角指示点
    const float handleBoxR = 12.0f * scale;
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const D2D1_POINT_2F handlePoints[8] = {
        {rect.left, rect.top}, {cx, rect.top}, {rect.right, rect.top},
        {rect.left, cy},                       {rect.right, cy},
        {rect.left, rect.bottom}, {cx, rect.bottom}, {rect.right, rect.bottom}
    };
    for (const auto& hp : handlePoints) {
        obstacles.push_back({
            D2D1::RectF(hp.x - handleBoxR, hp.y - handleBoxR, hp.x + handleBoxR, hp.y + handleBoxR),
            HudObstacleType::SelectionHandle,
            1.5f
        });
    }

    // 2. 注册所有截图标注框（文本框、矩形、箭头、序列号等）为障碍物，彻底杜绝挡住标注
    for (const auto& elem : state.markup.elements()) {
        if (!elem) continue;
        cv::Rect bbox = elem->getBoundingBox();
        if (bbox.width <= 0 || bbox.height <= 0) continue;
        float bLeft = rect.left + static_cast<float>(bbox.x);
        float bTop = rect.top + static_cast<float>(bbox.y);
        float bRight = bLeft + static_cast<float>(bbox.width);
        float bBottom = bTop + static_cast<float>(bbox.height);
        obstacles.push_back({D2D1::RectF(bLeft, bTop, bRight, bBottom), HudObstacleType::ActiveAnnotation, 2.0f});
    }

    // 3. 正在绘制图元（isMarking）时的动态拉框预览避让
    if (state.isMarking) {
        float mx1 = (std::min)(static_cast<float>(state.markupStart.x), static_cast<float>(state.markupEnd.x));
        float my1 = (std::min)(static_cast<float>(state.markupStart.y), static_cast<float>(state.markupEnd.y));
        float mx2 = (std::max)(static_cast<float>(state.markupStart.x), static_cast<float>(state.markupEnd.x));
        float my2 = (std::max)(static_cast<float>(state.markupStart.y), static_cast<float>(state.markupEnd.y));
        const float pad = 8.0f * scale;
        obstacles.push_back({D2D1::RectF(mx1 - pad, my1 - pad, mx2 + pad, my2 + pad), HudObstacleType::ActiveAnnotation, 2.2f});
    }

    // 3.5. 预选/拖拽阶段像素取色放大镜（Loupe）与动态放大镜（Magnifier）几何避让
    if (!hasLockedSelection || state.dragging) {
        state.loupeHudRect = computeSelectionLoupeRect(
            static_cast<float>(state.currentCursor.x),
            static_cast<float>(state.currentCursor.y),
            surfaceSize, scale);
        obstacles.push_back({state.loupeHudRect, HudObstacleType::Cursor, 4.0f});
    } else if (state.currentTool == MarkupTool::Magnifier && !state.isMarking && !state.isManipulating) {
        const float magR = (static_cast<float>(state.dynamicMagnifierRadius) + 12.0f) * scale;
        const float curFx = static_cast<float>(state.currentCursor.x);
        const float curFy = static_cast<float>(state.currentCursor.y);
        obstacles.push_back({
            D2D1::RectF(curFx - magR, curFy - magR, curFx + magR, curFy + magR),
            HudObstacleType::Cursor,
            3.5f
        });
    }

    // 4. 光标避让策略：
    // 展开尺寸菜单时用户正在点击菜单选项，不主动躲避光标；
    // 处于预选探测(!hasLockedSelection)、拖拽、调整选区、手柄操作、图元绘制或光标进入选区内部/靠近把手时，强力避让光标
    const bool isInteracting = state.dragging || state.isMarking || state.isManipulating ||
                               state.isAdjustingSelection || state.isAdjustingCornerRadius;
    const float handleSafeR = 26.0f * scale;
    bool nearAnyHandle = false;
    for (const auto& hp : handlePoints) {
        if (std::hypot(static_cast<float>(state.currentCursor.x) - hp.x,
                       static_cast<float>(state.currentCursor.y) - hp.y) <= handleSafeR) {
            nearAnyHandle = true;
            break;
        }
    }
    const bool cursorInsideSelection = (state.currentCursor.x >= rect.left && state.currentCursor.x <= rect.right &&
                                        state.currentCursor.y >= rect.top && state.currentCursor.y <= rect.bottom);

    if (!state.isSizeMenuOpen) {
        float curSafeR = 28.0f * scale;
        D2D1_RECT_F cursorObstacle = D2D1::RectF(
            static_cast<float>(state.currentCursor.x) - curSafeR,
            static_cast<float>(state.currentCursor.y) - curSafeR,
            static_cast<float>(state.currentCursor.x) + curSafeR,
            static_cast<float>(state.currentCursor.y) + curSafeR);
        if (!hasLockedSelection || isInteracting || nearAnyHandle || cursorInsideSelection) {
            obstacles.push_back({cursorObstacle, HudObstacleType::Cursor, 3.0f});
        }
    }

    HudPlacementConfig hudCfg;
    hudCfg.edgeGap = 16.0f;
    hudCfg.screenMargin = 8.0f;
    hudCfg.cursorSafeRadius = 28.0f;
    hudCfg.stabilityBonus = 45.0f;
    hudCfg.allowInsideCandidates = true;
    hudCfg.avoidCursor = true;

    auto placement = HudAvoidanceEngine::solvePlacement(
        rect, labelW, labelH, surfaceSize, obstacles, hudCfg, scale, state.lastSizeHudEdge);

    state.lastSizeHudEdge = static_cast<int>(placement.edge);
    auto pillRect = placement.rect;
    state.sizeHudRect = pillRect;

    float labelX = pillRect.left;
    float labelY = pillRect.top;

    // 判定鼠标悬停状态
    bool isHovered = (state.currentCursor.x >= pillRect.left && state.currentCursor.x <= pillRect.right &&
                      state.currentCursor.y >= pillRect.top && state.currentCursor.y <= pillRect.bottom);
    state.isSizeHudHovered = isHovered;

    const float pillRadius = 10.0f * scale;
    auto pillRounded = D2D1::RoundedRect(pillRect, pillRadius, pillRadius);

    // 1. 统一复用温润雪瓷白微晶浮岛底板（与主工具栏材质、阴影、内高光环 100% 同宗同源）
    const bool activeState = isHovered || state.isSizeMenuOpen;
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), pillRect, pillRadius, false);

    ComPtr<ID2D1SolidColorBrush> pillBorder, pillText, posTextBrush, accentBrush, dividerBrush, subPillBg, subPillBorder;
    m_renderTarget->CreateSolidColorBrush(
        activeState ? CaptureHudPalette::kSizeHudHoverBorder : CaptureHudPalette::kSizeHudBorder,
        pillBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, pillText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSecondaryText, posTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kChipAccentText, accentBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudDivider, dividerBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        activeState ? CaptureHudPalette::kChipHoverBg : CaptureHudPalette::kChipAccentBg,
        subPillBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        activeState ? CaptureHudPalette::kChipHoverBorder : CaptureHudPalette::kChipAccentBorder,
        subPillBorder.GetAddressOf());

    if (activeState && pillBorder) {
        m_renderTarget->DrawRoundedRectangle(pillRounded, pillBorder.Get(), 1.0f * scale);
    }

    // 3. 多段式结构化仪表渲染
    float cursorX = labelX + padX;
    const float midY = labelY + labelH * 0.5f;

    // 3.1 左侧四角取景框矢量图标
    if (accentBrush) {
        const float ix1 = cursorX + 1.0f * scale;
        const float iy1 = midY - iconSz * 0.5f + 1.0f * scale;
        const float ix2 = cursorX + iconSz - 1.0f * scale;
        const float iy2 = midY + iconSz * 0.5f - 1.0f * scale;
        const float arm = 4.2f * scale;
        const float sw = 1.6f * scale;
        m_renderTarget->DrawLine(D2D1::Point2F(ix1, iy1 + arm), D2D1::Point2F(ix1, iy1), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix1, iy1), D2D1::Point2F(ix1 + arm, iy1), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix2 - arm, iy1), D2D1::Point2F(ix2, iy1), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix2, iy1), D2D1::Point2F(ix2, iy1 + arm), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix1, iy2 - arm), D2D1::Point2F(ix1, iy2), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix1, iy2), D2D1::Point2F(ix1 + arm, iy2), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix2 - arm, iy2), D2D1::Point2F(ix2, iy2), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix2, iy2), D2D1::Point2F(ix2, iy2 - arm), accentBrush.Get(), sw);
    }
    cursorX += iconSz + iconGap;

    // 3.2 可选光标/选区坐标段 + 竖向微晶刻痕
    if (!posStr.empty() && badgeFmt && posTextBrush) {
        TextFormatScope scope(badgeFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_renderTarget->DrawTextW(
            posStr.c_str(), static_cast<UINT32>(posStr.size()), badgeFmt,
            D2D1::RectF(cursorX, labelY, cursorX + posW + 4.0f * scale, labelY + labelH),
            posTextBrush.Get());
        cursorX += posW;
        if (dividerBrush) {
            const float divX = cursorX + dividerZone * 0.5f;
            m_renderTarget->DrawLine(
                D2D1::Point2F(divX, labelY + 9.5f * scale),
                D2D1::Point2F(divX, labelY + labelH - 9.5f * scale),
                dividerBrush.Get(), 1.0f * scale);
        }
        cursorX += dividerZone;
    }

    // 3.3 主尺寸数字段 (W × H) + 独立排印的单位后缀 (px / dp)
    if (primaryFmt && pillText) {
        TextFormatScope scope(primaryFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawTextW(
            mainSizeStr.c_str(), static_cast<UINT32>(mainSizeStr.size()), primaryFmt,
            D2D1::RectF(cursorX, labelY, cursorX + mainW + 6.0f * scale, labelY + labelH),
            pillText.Get());
        cursorX += mainW;
    }

    if (!unitTagStr.empty() && badgeFmt) {
        cursorX += unitGap;
        if (isDpMode) {
            // dp 逻辑像素换算态：以精致温润晨雾紫晶微晶胶囊凸显，一眼区分物理与逻辑尺寸
            const float unitChipH = 20.0f * scale;
            const float unitTop = midY - unitChipH * 0.5f;
            D2D1_RECT_F uRect = D2D1::RectF(cursorX, unitTop, cursorX + unitW, unitTop + unitChipH);
            auto uRound = D2D1::RoundedRect(uRect, 5.0f * scale, 5.0f * scale);
            if (subPillBg) m_renderTarget->FillRoundedRectangle(uRound, subPillBg.Get());
            if (subPillBorder) m_renderTarget->DrawRoundedRectangle(uRound, subPillBorder.Get(), 0.9f * scale);
            if (accentBrush) {
                TextFormatScope uScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawTextW(unitTagStr.c_str(), static_cast<UINT32>(unitTagStr.size()), badgeFmt, uRect, accentBrush.Get());
            }
        } else if (posTextBrush) {
            // px 物理像素默认态：采用暖月银次级字号并自然微沉 0.8px 对齐大数字视觉基线，清爽不抢戏
            TextFormatScope uScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawTextW(
                unitTagStr.c_str(), static_cast<UINT32>(unitTagStr.size()), badgeFmt,
                D2D1::RectF(cursorX, labelY + 0.8f * scale, cursorX + unitW + 4.0f * scale, labelY + labelH),
                posTextBrush.Get());
        }
        cursorX += unitW;
    }

    // 3.4 内嵌圆角微晶状态徽章 (比例锁定 / 圆角半径 / 美化外壳)
    if (badgeFmt && accentBrush) {
        const float subH = 22.0f * scale;
        const float subTop = midY - subH * 0.5f;
        const float subRadius = 5.5f * scale;
        TextFormatScope scope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        for (size_t i = 0; i < badges.size(); ++i) {
            cursorX += badgeGap;
            const float bw = badgeWidths[i];
            D2D1_RECT_F bRect = D2D1::RectF(cursorX, subTop, cursorX + bw, subTop + subH);
            auto bRounded = D2D1::RoundedRect(bRect, subRadius, subRadius);
            if (subPillBg) m_renderTarget->FillRoundedRectangle(bRounded, subPillBg.Get());
            if (subPillBorder) m_renderTarget->DrawRoundedRectangle(bRounded, subPillBorder.Get(), 1.0f * scale);
            m_renderTarget->DrawTextW(
                badges[i].c_str(), static_cast<UINT32>(badges[i].size()), badgeFmt,
                bRect, accentBrush.Get());
            cursorX += bw;
        }
    }

    // 3.5 右侧下拉菜单指示微箭头（悬停或展开时点亮晨雾紫晶微光）
    ID2D1Brush* chevBrush = activeState && accentBrush ? accentBrush.Get() : posTextBrush.Get();
    if (chevBrush) {
        const float chevCx = pillRect.right - padX - 3.5f * scale;
        const float chevHalfW = 3.4f * scale;
        const float chevHalfH = 2.0f * scale;
        const float dir = state.isSizeMenuOpen ? -1.0f : 1.0f;
        m_renderTarget->DrawLine(
            D2D1::Point2F(chevCx - chevHalfW, midY - chevHalfH * dir),
            D2D1::Point2F(chevCx, midY + chevHalfH * dir),
            chevBrush, 1.5f * scale);
        m_renderTarget->DrawLine(
            D2D1::Point2F(chevCx, midY + chevHalfH * dir),
            D2D1::Point2F(chevCx + chevHalfW, midY - chevHalfH * dir),
            chevBrush, 1.5f * scale);
    }
}

void CaptureRenderer::drawSizeMenu(const D2D1_RECT_F& hudRect, CaptureState& state) {
    if (!state.isSizeMenuOpen || !m_renderTarget) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const auto lay = computeSizeMenuLayout(hudRect, m_renderTarget->GetSize(), scale);
    const auto& menuRect = lay.menuRect;
    state.sizeMenuRect = menuRect;

    const float menuRadius = 11.0f * scale;
    // 1. 统一采用温润雪瓷白微晶卡片底板（彻底清退紫黑底与电光紫杂色）
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), menuRect, menuRadius, false);

    ComPtr<ID2D1SolidColorBrush> dividerBrush;
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudDivider, dividerBrush.GetAddressOf());

    ComPtr<ID2D1SolidColorBrush> segTrackBg, segTrackBorder, segActiveShadow, segActiveBg, segActiveBorder;
    ComPtr<ID2D1SolidColorBrush> primaryTextBrush, secondaryTextBrush, formulaTextBrush;
    ComPtr<ID2D1SolidColorBrush> rowHoverBg, switchOnBg, switchOffBg, knobShadowBrush, knobBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(241.0f / 255.0f, 245.0f / 255.0f, 249.0f / 255.0f, 0.98f), segTrackBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.08f), segTrackBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.07f), segActiveShadow.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), segActiveBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.13f), segActiveBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, primaryTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSecondaryText, secondaryTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(100.0f / 255.0f, 116.0f / 255.0f, 139.0f / 255.0f, 0.92f), formulaTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.045f), rowHoverBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, 0.95f), switchOnBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(203.0f / 255.0f, 213.0f / 255.0f, 225.0f / 255.0f, 0.95f), switchOffBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.14f), knobShadowBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), knobBrush.GetAddressOf());

    const float curX = static_cast<float>(state.currentCursor.x);
    const float curY = static_cast<float>(state.currentCursor.y);
    const auto isCursorIn = [&](const D2D1_RECT_F& r) {
        return curX >= r.left && curX <= r.right && curY >= r.top && curY <= r.bottom;
    };

    // 2. 第 1 区：Apple Studio 级纯白立体浮起双联分段滑块 (px | dp Segmented Control)
    const auto trackRound = D2D1::RoundedRect(lay.segmentTrackRect, 7.5f * scale, 7.5f * scale);
    if (segTrackBg) m_renderTarget->FillRoundedRectangle(trackRound, segTrackBg.Get());
    if (segTrackBorder) m_renderTarget->DrawRoundedRectangle(trackRound, segTrackBorder.Get(), 0.9f * scale);

    const bool isPx = (state.sizeUnit == CaptureState::SizeUnit::Pixel);
    const D2D1_RECT_F& activeSeg = isPx ? lay.pxSegmentRect : lay.dpSegmentRect;
    const D2D1_RECT_F& inactiveSeg = isPx ? lay.dpSegmentRect : lay.pxSegmentRect;
    if (isCursorIn(inactiveSeg) && rowHoverBg) {
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(inactiveSeg, 5.5f * scale, 5.5f * scale), rowHoverBg.Get());
    }
    if (segActiveShadow) {
        const D2D1_RECT_F sRect = D2D1::RectF(activeSeg.left, activeSeg.top + 1.0f * scale, activeSeg.right, activeSeg.bottom + 1.5f * scale);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sRect, 5.5f * scale, 5.5f * scale), segActiveShadow.Get());
    }
    const auto activeSegRound = D2D1::RoundedRect(activeSeg, 5.5f * scale, 5.5f * scale);
    if (segActiveBg) m_renderTarget->FillRoundedRectangle(activeSegRound, segActiveBg.Get());
    if (segActiveBorder) m_renderTarget->DrawRoundedRectangle(activeSegRound, segActiveBorder.Get(), 0.9f * scale);

    IDWriteTextFormat* ctrlFmt = m_infoTextFormat ? m_infoTextFormat.Get() : m_hudBadgeFormat.Get();
    IDWriteTextFormat* badgeFmt = m_hudBadgeFormat ? m_hudBadgeFormat.Get() : ctrlFmt;
    if (ctrlFmt && primaryTextBrush && secondaryTextBrush) {
        TextFormatScope segScope(ctrlFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        const std::wstring pxLabel = state.toolbarLayoutChinese ? L"px 物理" : L"px";
        const std::wstring dpLabel = state.toolbarLayoutChinese ? L"dp 逻辑" : L"dp";
        m_renderTarget->DrawTextW(pxLabel.c_str(), static_cast<UINT32>(pxLabel.size()), ctrlFmt,
            lay.pxSegmentRect, isPx ? primaryTextBrush.Get() : secondaryTextBrush.Get());
        m_renderTarget->DrawTextW(dpLabel.c_str(), static_cast<UINT32>(dpLabel.size()), ctrlFmt,
            lay.dpSegmentRect, !isPx ? primaryTextBrush.Get() : secondaryTextBrush.Get());
    }

    // 3. 第 2 区：单行换算说明条（动态呈现当前显示器真实缩放比，零折行零重叠）
    const int scalePct = static_cast<int>(std::round(scale * 100.0f));
    const std::wstring formulaText = state.toolbarLayoutChinese
        ? std::format(L"dp = px \u00F7 {:.2g} ({}% \u7F29\u653E)", scale, scalePct)
        : std::format(L"dp = px \u00F7 {:.2g} ({}% scale)", scale, scalePct);
    if (badgeFmt && formulaTextBrush) {
        TextFormatScope fScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawTextW(formulaText.c_str(), static_cast<UINT32>(formulaText.size()), badgeFmt,
            lay.formulaRect, formulaTextBrush.Get());
    }

    if (dividerBrush) {
        m_renderTarget->DrawLine(
            D2D1::Point2F(menuRect.left + 14.0f * scale, lay.dividerY),
            D2D1::Point2F(menuRect.right - 14.0f * scale, lay.dividerY),
            dividerBrush.Get(), 1.0f * scale);
    }

    // 4. 第 3 & 第 4 区：整行同轴对齐的开关项（深石墨开启态 + 浅银灰关闭态）
    auto drawToggleRow = [&](const D2D1_RECT_F& rowRect, const D2D1_RECT_F& swRect, const std::wstring& label, bool checked) {
        if (isCursorIn(rowRect) && rowHoverBg) {
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 6.0f * scale, 6.0f * scale), rowHoverBg.Get());
        }
        if (ctrlFmt && primaryTextBrush) {
            TextFormatScope rScope(ctrlFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            const D2D1_RECT_F labelRect = D2D1::RectF(
                menuRect.left + 14.0f * scale, rowRect.top,
                swRect.left - 8.0f * scale, rowRect.bottom);
            m_renderTarget->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), ctrlFmt,
                labelRect, primaryTextBrush.Get());
        }
        const float swH = swRect.bottom - swRect.top;
        const auto swRound = D2D1::RoundedRect(swRect, swH * 0.5f, swH * 0.5f);
        ID2D1Brush* trackBrush = checked ? switchOnBg.Get() : switchOffBg.Get();
        if (trackBrush) m_renderTarget->FillRoundedRectangle(swRound, trackBrush);
        if (knobBrush) {
            const float knobR = (swH - 4.0f * scale) * 0.5f;
            const float knobCx = checked ? (swRect.right - swH * 0.5f) : (swRect.left + swH * 0.5f);
            const float knobCy = (swRect.top + swRect.bottom) * 0.5f;
            if (knobShadowBrush) {
                m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobCx, knobCy + 0.8f * scale), knobR, knobR), knobShadowBrush.Get());
            }
            m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobCx, knobCy), knobR, knobR), knobBrush.Get());
        }
    };

    drawToggleRow(lay.positionRowRect, lay.positionSwitchRect,
                  state.toolbarLayoutChinese ? L"显示坐标" : L"Position",
                  state.showPositionInHud);
    drawToggleRow(lay.unitRowRect, lay.unitSwitchRect,
                  state.toolbarLayoutChinese ? L"显示单位" : L"Unit Suffix",
                  state.showUnitInHud);
}

void CaptureRenderer::drawToolbar(const D2D1_RECT_F& selectionRect, CaptureState& state) {
    rebuildCaptureToolbar(state, selectionRect, m_renderTarget->GetSize());
    if (state.toolbarButtons.empty()) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const bool isActiveDragInteraction =
        state.isMarking || state.isAdjustingSelection || state.isAdjustingCornerRadius || state.isManipulating;
    bool ghostFadeToolbar = false;
    if (isActiveDragInteraction) {
        const float pad = 28.0f * scale;
        const float cx = static_cast<float>(state.currentCursor.x);
        const float cy = static_cast<float>(state.currentCursor.y);
        D2D1_RECT_F activeBox = D2D1::RectF(cx - pad, cy - pad, cx + pad, cy + pad);
        if (state.isMarking) {
            const float mx1 = (std::min)(static_cast<float>(state.markupStart.x), cx) - pad;
            const float my1 = (std::min)(static_cast<float>(state.markupStart.y), cy) - pad;
            const float mx2 = (std::max)(static_cast<float>(state.markupStart.x), cx) + pad;
            const float my2 = (std::max)(static_cast<float>(state.markupStart.y), cy) + pad;
            activeBox = D2D1::RectF(mx1, my1, mx2, my2);
        }
        if (HudAvoidanceEngine::rectsIntersect(activeBox, state.primaryToolbarRect) ||
            (!state.secondaryToolbarButtons.empty() && HudAvoidanceEngine::rectsIntersect(activeBox, state.secondaryToolbarRect)) ||
            (!state.selectionSideButtons.empty() && HudAvoidanceEngine::rectsIntersect(activeBox, state.selectionSideRect))) {
            ghostFadeToolbar = true;
        }
    }

    ComPtr<ID2D1Layer> ghostLayer;
    bool pushedGhostLayer = false;
    if (ghostFadeToolbar && SUCCEEDED(m_renderTarget->CreateLayer(nullptr, ghostLayer.GetAddressOf())) && ghostLayer) {
        m_renderTarget->PushLayer(
            D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                  D2D1::IdentityMatrix(), 0.18f),
            ghostLayer.Get());
        pushedGhostLayer = true;
    }

    // 1. 绘制主工具栏浮岛卡片 (12px 现代光学双环微晶圆角)
    drawGlassPanel(state.primaryToolbarRect, 12.0f * scale, false);

    // 1b. 绘制主工具栏左侧 2x3 浅石板灰六点拖拽手柄 (PixPin 风格自由拖拽握把)
    if (state.toolbarGripRect.right > state.toolbarGripRect.left &&
        state.toolbarGripRect.bottom > state.toolbarGripRect.top) {
        const bool gripHovered = state.isDraggingToolbar ||
            (state.currentCursor.x >= state.toolbarGripRect.left &&
             state.currentCursor.x <= state.toolbarGripRect.right &&
             state.currentCursor.y >= state.toolbarGripRect.top &&
             state.currentCursor.y <= state.toolbarGripRect.bottom);
        ComPtr<ID2D1SolidColorBrush> gripDotBrush;
        m_renderTarget->CreateSolidColorBrush(
            gripHovered ? CaptureHudPalette::kGripDotHover : CaptureHudPalette::kGripDotIdle,
            gripDotBrush.GetAddressOf());
        if (gripDotBrush) {
            const float gcx = (state.toolbarGripRect.left + state.toolbarGripRect.right) * 0.5f;
            const float gcy = (state.toolbarGripRect.top + state.toolbarGripRect.bottom) * 0.5f;
            const float colOffset = 2.2f * scale;
            const float rowStep = 5.0f * scale;
            const float dotR = 1.45f * scale;
            for (int rIdx = -1; rIdx <= 1; ++rIdx) {
                const float dy = gcy + static_cast<float>(rIdx) * rowStep;
                m_renderTarget->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(gcx - colOffset, dy), dotR, dotR), gripDotBrush.Get());
                m_renderTarget->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(gcx + colOffset, dy), dotR, dotR), gripDotBrush.Get());
            }
        }
    }

    // 2. 如果存在二级属性栏，绘制二级属性栏浮岛卡片 (11px)
    if (!state.secondaryToolbarButtons.empty()) {
        drawGlassPanel(state.secondaryToolbarRect, 11.0f * scale, false);
    }

    // 截图与录屏工具栏家族统一采用温润雪瓷白亮色微晶模式 + 深石墨中性强调色（仅保留完成绿与录屏珊瑚红为主焦点）
    const bool isDark = false;
    const D2D1_COLOR_F graphiteAccent = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 1.0f);

    ComPtr<ID2D1SolidColorBrush> buttonBrush;
    ComPtr<ID2D1SolidColorBrush> activeBrush;
    ComPtr<ID2D1SolidColorBrush> activeBorderBrush;
    ComPtr<ID2D1SolidColorBrush> dangerHoverBrush;
    ComPtr<ID2D1SolidColorBrush> confirmBrush;
    ComPtr<ID2D1SolidColorBrush> confirmHoverBrush;
    ComPtr<ID2D1SolidColorBrush> confirmHighlightBrush;
    ComPtr<ID2D1SolidColorBrush> hoverBrush;
    ComPtr<ID2D1SolidColorBrush> iconBrush;
    ComPtr<ID2D1SolidColorBrush> disabledIconBrush;
    ComPtr<ID2D1SolidColorBrush> activeIconBrush;
    ComPtr<ID2D1SolidColorBrush> confirmIconBrush;
    ComPtr<ID2D1SolidColorBrush> dangerIconBrush;
    ComPtr<ID2D1SolidColorBrush> tipTextBrush;
    ComPtr<ID2D1SolidColorBrush> secondaryActiveBrush;
    ComPtr<ID2D1SolidColorBrush> separatorBrush;

    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), buttonBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.055f), hoverBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.085f), activeBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.10f), secondaryActiveBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.22f), activeBorderBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(graphiteAccent, activeIconBrush.GetAddressOf());

    // 完成按钮：立体翡翠绿胶囊与内高光
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.72f, 0.45f, 0.96f), confirmBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.64f, 0.39f, 1.0f), confirmHoverBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), confirmHighlightBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), confirmIconBrush.GetAddressOf());

    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.26f, 0.26f, 0.12f), dangerHoverBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.22f, 0.26f, 1.0f), dangerIconBrush.GetAddressOf());

    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, 0.94f), iconBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, 0.28f), disabledIconBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, tipTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.11f), separatorBrush.GetAddressOf());

    auto drawButtonGroup = [&](const std::vector<ToolbarButton>& btnList) {
        for (size_t idx = 0; idx < btnList.size(); ++idx) {
            const auto& button = btnList[idx];
            // 绘制前置微刻痕细腻分隔线 (1px 双层微刻痕，严格居中于相邻两枚按钮间距中点)
            if (button.isSeparatorBefore) {
                float sepX = (idx > 0)
                    ? (btnList[idx - 1].rect.right + button.rect.left) * 0.5f
                    : (button.rect.left - 6.0f * scale);
                float sepTop = button.rect.top + 5.0f * scale;
                float sepBottom = button.rect.bottom - 5.0f * scale;
                FloatingGlassBar::drawSeparator(m_renderTarget.Get(), sepX, sepTop, sepBottom, 1.0f, isDark);
            }

            bool isTool = button.command == ToolbarCommand::SelectTool;
            bool isActiveTool = false;
            if (isTool) {
                if (state.isMarkupToolActive || button.isSecondary) {
                    if (button.tool == MarkupTool::Rectangle || button.tool == MarkupTool::Ellipse ||
                        button.tool == MarkupTool::Line || button.tool == MarkupTool::Spotlight) {
                        isActiveTool = (state.currentTool == MarkupTool::Rectangle ||
                                        state.currentTool == MarkupTool::Ellipse ||
                                        state.currentTool == MarkupTool::Line ||
                                        state.currentTool == MarkupTool::Spotlight);
                    } else if (button.tool == MarkupTool::Pen || button.tool == MarkupTool::Highlight) {
                        isActiveTool = (state.currentTool == MarkupTool::Pen || state.currentTool == MarkupTool::Highlight);
                    } else if (button.tool == MarkupTool::Mosaic || button.tool == MarkupTool::Blur) {
                        isActiveTool = (state.currentTool == MarkupTool::Mosaic || state.currentTool == MarkupTool::Blur);
                    } else {
                        isActiveTool = (button.tool == state.currentTool);
                    }
                    if (button.isSecondary) {
                        isActiveTool = (button.tool == state.currentTool);
                    }
                }
            }
            if (button.command == ToolbarCommand::ToggleFill) {
                if (button.tool == MarkupTool::Magnifier) {
                    isActiveTool = (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier)
                        ? state.activeElement->fill
                        : state.magnifierShowBadge;
                } else if (button.tool == MarkupTool::Spotlight) {
                    isActiveTool = (state.activeElement && state.activeElement->tool == MarkupTool::Spotlight)
                        ? state.activeElement->spotlightShowBorder
                        : state.currentSpotlightBorder;
                } else {
                    isActiveTool = state.currentFillMode;
                }
            }
            if (button.command == ToolbarCommand::SelectMosaicType) {
                isActiveTool = (button.intParam == state.currentMosaicType);
            }
            if (button.command == ToolbarCommand::ToggleBeautyShell) {
                isActiveTool = state.beautyShell.enabled;
            }
            if (button.command == ToolbarCommand::SideCycleAspectRatio) {
                isActiveTool = (state.aspectRatio != AspectRatioPreset::Free);
            }
            if (button.command == ToolbarCommand::RecordToggleKeycast ||
                button.command == ToolbarCommand::RecordToggleSystemAudio ||
                button.command == ToolbarCommand::RecordToggleMicrophone) {
                isActiveTool = button.boolParam;
            }
            if (button.command == ToolbarCommand::RecordCycleClickEffect) {
                isActiveTool = (state.recordSetup.showClickEffects || state.recordSetup.showClickZoom);
            }

            if (button.command == ToolbarCommand::ToggleTextOutline) {
                isActiveTool = state.currentTextOutline;
            }
            if (button.command == ToolbarCommand::CycleElementCornerRadius) {
                isActiveTool = (button.intParam > 0 || state.sliderPopup.type == SliderPopupType::CornerRadius);
            }
            if (button.command == ToolbarCommand::ToggleCornerRadius) {
                isActiveTool = (state.effectiveCornerRadius() > 0.5f ||
                                state.sliderPopup.type == SliderPopupType::SelectionCornerRadius);
            }
            if (button.command == ToolbarCommand::CycleStrokeWidth) {
                isActiveTool = (state.sliderPopup.type == SliderPopupType::StrokeWidth ||
                                state.sliderPopup.type == SliderPopupType::TextFontSize ||
                                state.sliderPopup.type == SliderPopupType::MosaicBlockSize);
            }
            if (button.command == ToolbarCommand::CycleSpotlightDim) {
                isActiveTool = (state.sliderPopup.type == SliderPopupType::SpotlightDim);
            }
            if (button.command == ToolbarCommand::ToggleLineStyleDropdown) {
                isActiveTool = (state.openSubmenu == SubmenuType::LineStyle);
            }
            if (button.command == ToolbarCommand::ToggleArrowStyleDropdown) {
                isActiveTool = (state.openSubmenu == SubmenuType::ArrowStyle);
            }
            if (button.command == ToolbarCommand::ToggleNumberFormatDropdown) {
                isActiveTool = (state.dropdownMenu.type == DropdownType::NumberFormat);
            }
            if (button.command == ToolbarCommand::CycleBeautyBg) {
                isActiveTool = (state.dropdownMenu.type == DropdownType::BeautyShellBg);
            }
            if (button.command == ToolbarCommand::CycleBeautyPadding) {
                isActiveTool = (state.dropdownMenu.type == DropdownType::BeautyShellPadding);
            }
            if (button.command == ToolbarCommand::CycleBeautyRadius) {
                isActiveTool = (state.dropdownMenu.type == DropdownType::BeautyShellRadius);
            }

            bool isDisabled = false;
            if (button.command == ToolbarCommand::Undo) isDisabled = !state.markup.canUndo();
            else if (button.command == ToolbarCommand::Redo) isDisabled = !state.markup.canRedo();
            else if (button.command == ToolbarCommand::Clear) isDisabled = (state.markup.elementCount() == 0);
            else if ((button.command == ToolbarCommand::RecordToggleSystemAudio ||
                      button.command == ToolbarCommand::RecordToggleMicrophone) &&
                     state.recordSetup.format == RecordFormat::GIF) {
                isDisabled = true;
            }

            bool isColor = button.command == ToolbarCommand::SelectColor;
            bool isOutlineColor = button.command == ToolbarCommand::SelectTextOutlineColor;
            bool isDanger = button.command == ToolbarCommand::Cancel;
            bool isConfirm = button.command == ToolbarCommand::Confirm ||
                             button.command == ToolbarCommand::RecordStartConfirm;
            bool isStepper = (button.command == ToolbarCommand::CycleStrokeWidth ||
                              button.command == ToolbarCommand::CycleMagnifierScale ||
                              button.command == ToolbarCommand::CycleElementCornerRadius ||
                              button.command == ToolbarCommand::ToggleCornerRadius ||
                              button.command == ToolbarCommand::CycleSpotlightDim ||
                              button.command == ToolbarCommand::ToggleNumberFormatDropdown ||
                              button.command == ToolbarCommand::CycleBeautyBg ||
                              button.command == ToolbarCommand::CycleBeautyPadding ||
                              button.command == ToolbarCommand::CycleBeautyRadius ||
                              button.command == ToolbarCommand::RecordToggleFormat ||
                              button.command == ToolbarCommand::RecordCycleFps ||
                              button.command == ToolbarCommand::RecordCycleQuality ||
                              button.command == ToolbarCommand::RecordCycleClickEffect);
            bool isHovered = !isDisabled &&
                             state.currentCursor.x >= button.rect.left && state.currentCursor.x <= button.rect.right &&
                             state.currentCursor.y >= button.rect.top  && state.currentCursor.y <= button.rect.bottom;

            const float btnRadius = 6.5f * scale;
            auto rounded = D2D1::RoundedRect(button.rect, btnRadius, btnRadius);

            if (isColor) {
                // 颜色色板: 圆形色球 + 双层高亮光环
                float cx = (button.rect.left + button.rect.right) * 0.5f;
                float cy = (button.rect.top + button.rect.bottom) * 0.5f;
                float r = std::min(button.rect.right - button.rect.left, button.rect.bottom - button.rect.top) * 0.39f;
                auto colorCircle = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);

                ComPtr<ID2D1SolidColorBrush> swatch;
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(button.color.r / 255.0f, button.color.g / 255.0f,
                                 button.color.b / 255.0f, 1.0f),
                    swatch.GetAddressOf());
                if (swatch) m_renderTarget->FillEllipse(colorCircle, swatch.Get());

                const bool isNearWhiteSwatch = (button.color.r > 235 && button.color.g > 235 && button.color.b > 235);
                const bool isNearBlackSwatch = (button.color.r < 45 && button.color.g < 45 && button.color.b < 45);
                if ((isNearWhiteSwatch || (isDark && isNearBlackSwatch)) && separatorBrush) {
                    m_renderTarget->DrawEllipse(colorCircle, separatorBrush.Get(), 1.0f * scale);
                }

                const MarkupColor& refColor = (button.tool == MarkupTool::Magnifier)
                    ? ((state.activeElement && state.activeElement->tool == MarkupTool::Magnifier)
                           ? state.activeElement->color
                           : state.currentMagnifierColor)
                    : state.currentColor;

                bool isActiveColor = (button.color.r == refColor.r &&
                                     button.color.g == refColor.g &&
                                     button.color.b == refColor.b);
                if (isActiveColor) {
                    // 内部背景色隔离环 + 外部高反差扩散光晕（浅色面板上的纯白色球/深色面板上的纯黑色球自动切换为主题强调色光环）
                    auto innerRing = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 1.2f * scale, r + 1.2f * scale);
                    ComPtr<ID2D1SolidColorBrush> gapRing;
                    m_renderTarget->CreateSolidColorBrush(
                        isDark ? D2D1::ColorF(0.12f, 0.14f, 0.18f, 0.95f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                        gapRing.GetAddressOf());
                    if (gapRing) m_renderTarget->DrawEllipse(innerRing, gapRing.Get(), 1.2f * scale);

                    auto outerRing = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.8f * scale, r + 2.8f * scale);
                    const bool useAccentRing = (!isDark && isNearWhiteSwatch) || (isDark && isNearBlackSwatch);
                    auto* ringBrush = (useAccentRing && activeIconBrush) ? activeIconBrush.Get() : swatch.Get();
                    if (ringBrush) m_renderTarget->DrawEllipse(outerRing, ringBrush, 1.8f * scale);
                } else if (isHovered && hoverBrush) {
                    auto outerRing = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.2f * scale, r + 2.2f * scale);
                    m_renderTarget->DrawEllipse(outerRing, hoverBrush.Get(), 1.5f * scale);
                }
            } else if (isOutlineColor) {
                // 文字描边色板：采用统一尺寸空心圆环 (Hollow Ring Swatch) 视觉形态，与右侧实心字色圆点形成直觉级区分
                float cx = (button.rect.left + button.rect.right) * 0.5f;
                float cy = (button.rect.top + button.rect.bottom) * 0.5f;
                float r = std::min({button.rect.right - button.rect.left, button.rect.bottom - button.rect.top, 20.0f * scale}) * 0.36f;
                float ringStroke = std::max(2.2f * scale, r * 0.46f);
                float midR = r - ringStroke * 0.35f;
                auto ringEllipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), midR, midR);
                auto outerBorder = D2D1::Ellipse(D2D1::Point2F(cx, cy), midR + ringStroke * 0.5f, midR + ringStroke * 0.5f);
                auto innerBorder = D2D1::Ellipse(D2D1::Point2F(cx, cy), std::max(1.0f, midR - ringStroke * 0.5f), std::max(1.0f, midR - ringStroke * 0.5f));

                bool isActive = button.color.isAuto()
                    ? state.currentTextOutlineColor.isAuto()
                    : (!state.currentTextOutlineColor.isAuto() && button.color == state.currentTextOutlineColor);

                if (button.color.isAuto()) {
                    auto leftClip = D2D1::RectF(cx - r - 2.0f * scale, cy - r - 2.0f * scale, cx, cy + r + 2.0f * scale);
                    auto rightClip = D2D1::RectF(cx, cy - r - 2.0f * scale, cx + r + 2.0f * scale, cy + r + 2.0f * scale);

                    ComPtr<ID2D1SolidColorBrush> blackBrush, whiteBrush;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.12f, 0.16f, 1.0f), blackBrush.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.98f, 0.98f, 1.0f, 1.0f), whiteBrush.GetAddressOf());

                    if (blackBrush && whiteBrush) {
                        m_renderTarget->PushAxisAlignedClip(leftClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                        m_renderTarget->DrawEllipse(ringEllipse, blackBrush.Get(), ringStroke);
                        m_renderTarget->PopAxisAlignedClip();

                        m_renderTarget->PushAxisAlignedClip(rightClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                        m_renderTarget->DrawEllipse(ringEllipse, whiteBrush.Get(), ringStroke);
                        m_renderTarget->PopAxisAlignedClip();
                    }
                    if (separatorBrush) {
                        m_renderTarget->DrawEllipse(outerBorder, separatorBrush.Get(), 0.9f * scale);
                        m_renderTarget->DrawEllipse(innerBorder, separatorBrush.Get(), 0.9f * scale);
                    }
                } else {
                    ComPtr<ID2D1SolidColorBrush> swatch;
                    m_renderTarget->CreateSolidColorBrush(
                        D2D1::ColorF(button.color.r / 255.0f, button.color.g / 255.0f,
                                     button.color.b / 255.0f, 1.0f),
                        swatch.GetAddressOf());
                    if (swatch) {
                        m_renderTarget->DrawEllipse(ringEllipse, swatch.Get(), ringStroke);
                    }
                    if (separatorBrush) {
                        m_renderTarget->DrawEllipse(outerBorder, separatorBrush.Get(), 0.9f * scale);
                        m_renderTarget->DrawEllipse(innerBorder, separatorBrush.Get(), 0.9f * scale);
                    }
                }

                if (isActive) {
                    auto activeOuter = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.4f * scale, r + 2.4f * scale);
                    if (activeIconBrush) m_renderTarget->DrawEllipse(activeOuter, activeIconBrush.Get(), 1.6f * scale);
                } else if (isHovered && hoverBrush) {
                    auto hoverOuter = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.0f * scale, r + 2.0f * scale);
                    m_renderTarget->DrawEllipse(hoverOuter, hoverBrush.Get(), 1.4f * scale);
                }
            } else {
                ComPtr<ID2D1SolidColorBrush> stepperBg;
                if (isStepper) {
                    m_renderTarget->CreateSolidColorBrush(
                        isDark
                            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, isHovered ? 0.105f : 0.058f)
                            : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, isHovered ? 0.075f : 0.044f),
                        stepperBg.GetAddressOf());
                }

                if (isConfirm) {
                    const bool isRecordStart = (button.command == ToolbarCommand::RecordStartConfirm);
                    // 录制启动采用温润珊瑚绯红微晶胶囊，截图确认保持翡翠绿微晶胶囊
                    D2D1_GRADIENT_STOP cStops[2] = {
                        {0.0f, isRecordStart
                            ? (isHovered ? D2D1::ColorF(251.0f / 255.0f, 75.0f / 255.0f, 102.0f / 255.0f, 1.0f)
                                         : D2D1::ColorF(244.0f / 255.0f, 63.0f / 255.0f, 94.0f / 255.0f, 0.96f))
                            : (isHovered ? D2D1::ColorF(0.08f, 0.76f, 0.47f, 1.0f)
                                         : D2D1::ColorF(0.10f, 0.80f, 0.50f, 0.98f))},
                        {1.0f, isRecordStart
                            ? (isHovered ? D2D1::ColorF(216.0f / 255.0f, 24.0f / 255.0f, 66.0f / 255.0f, 1.0f)
                                         : D2D1::ColorF(225.0f / 255.0f, 29.0f / 255.0f, 72.0f / 255.0f, 0.96f))
                            : (isHovered ? D2D1::ColorF(0.03f, 0.58f, 0.35f, 1.0f)
                                         : D2D1::ColorF(0.04f, 0.64f, 0.39f, 0.98f))}
                    };
                    ComPtr<ID2D1GradientStopCollection> cStopCol;
                    ComPtr<ID2D1LinearGradientBrush> cGrad;
                    if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(cStops, 2, cStopCol.GetAddressOf())) &&
                        SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(
                            D2D1::LinearGradientBrushProperties(
                                D2D1::Point2F(button.rect.left, button.rect.top),
                                D2D1::Point2F(button.rect.left, button.rect.bottom)),
                            cStopCol.Get(), cGrad.GetAddressOf())) && cGrad) {
                        m_renderTarget->FillRoundedRectangle(rounded, cGrad.Get());
                    } else if (confirmBrush) {
                        m_renderTarget->FillRoundedRectangle(rounded, isHovered ? confirmHoverBrush.Get() : confirmBrush.Get());
                    }
                    if (confirmHighlightBrush) {
                        const float innerRad = (std::max)(1.0f, btnRadius - 0.6f * scale);
                        auto innerR = D2D1::RoundedRect(
                            D2D1::RectF(button.rect.left + 0.6f * scale, button.rect.top + 0.6f * scale,
                                        button.rect.right - 0.6f * scale, button.rect.bottom - 0.6f * scale),
                            innerRad, innerRad);
                        m_renderTarget->DrawRoundedRectangle(innerR, confirmHighlightBrush.Get(), 0.9f * scale);
                    }
                } else if (isActiveTool) {
                    // 沉稳浅石板灰垂直微光胶囊 + 深石墨描边与底部微指示条
                    D2D1_GRADIENT_STOP aStops[2] = {
                        {0.0f, D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.105f)},
                        {1.0f, D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.055f)}
                    };
                    ComPtr<ID2D1GradientStopCollection> aStopCol;
                    ComPtr<ID2D1LinearGradientBrush> aGrad;
                    if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(aStops, 2, aStopCol.GetAddressOf())) &&
                        SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(
                            D2D1::LinearGradientBrushProperties(
                                D2D1::Point2F(button.rect.left, button.rect.top),
                                D2D1::Point2F(button.rect.left, button.rect.bottom)),
                            aStopCol.Get(), aGrad.GetAddressOf())) && aGrad) {
                        m_renderTarget->FillRoundedRectangle(rounded, aGrad.Get());
                    } else {
                        auto* fallback = button.isSecondary ? secondaryActiveBrush.Get() : activeBrush.Get();
                        if (fallback) m_renderTarget->FillRoundedRectangle(rounded, fallback);
                    }
                    if (activeBorderBrush) {
                        m_renderTarget->DrawRoundedRectangle(rounded, activeBorderBrush.Get(), 0.95f * scale);
                    }
                    if (!button.isSecondary && button.label.empty() && activeIconBrush) {
                        const float bcx = (button.rect.left + button.rect.right) * 0.5f;
                        const float pillHalfW = 4.8f * scale;
                        const float pillH = 2.0f * scale;
                        auto indPill = D2D1::RoundedRect(
                            D2D1::RectF(bcx - pillHalfW, button.rect.bottom - 3.2f * scale,
                                        bcx + pillHalfW, button.rect.bottom - 3.2f * scale + pillH),
                            1.0f * scale, 1.0f * scale);
                        m_renderTarget->FillRoundedRectangle(indPill, activeIconBrush.Get());
                    }
                } else {
                    auto* fillBrush = isStepper ? stepperBg.Get()
                                    : (isDanger && isHovered) ? dangerHoverBrush.Get()
                                    : isHovered ? hoverBrush.Get()
                                    : buttonBrush.Get();
                    if (fillBrush) m_renderTarget->FillRoundedRectangle(rounded, fillBrush);
                    // 常态去除生硬内边框，避免外框套一排硬小框；仅悬停时浮现柔和微光边
                    if (isStepper && isHovered && separatorBrush) {
                        m_renderTarget->DrawRoundedRectangle(rounded, separatorBrush.Get(), 0.85f * scale);
                    }
                }

                auto* currentIconBrush = isDisabled ? disabledIconBrush.Get()
                                       : isConfirm ? confirmIconBrush.Get()
                                       : isActiveTool ? activeIconBrush.Get()
                                       : (isDanger && isHovered) ? dangerIconBrush.Get()
                                       : iconBrush.Get();
                drawVectorButtonIcon(button, button.rect, currentIconBrush, scale);
            }

            // 如果带有下拉指示器：主工具栏正方形图标按钮采用右下角微斜切角标；二级胶囊采用右侧 V 型折角箭头
            if (button.hasDropdown) {
                auto* chevBrush = isActiveTool ? activeIconBrush.Get() : iconBrush.Get();
                if (chevBrush) {
                    if (!button.isSecondary && button.label.empty()) {
                        const float cx2 = button.rect.right - 3.2f * scale;
                        const float cy2 = button.rect.bottom - 3.2f * scale;
                        ComPtr<ID2D1PathGeometry> cornerTri;
                        if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(cornerTri.GetAddressOf())) && cornerTri) {
                            ComPtr<ID2D1GeometrySink> sink;
                            if (SUCCEEDED(cornerTri->Open(sink.GetAddressOf())) && sink) {
                                sink->BeginFigure(D2D1::Point2F(cx2, cy2 - 3.6f * scale), D2D1_FIGURE_BEGIN_FILLED);
                                sink->AddLine(D2D1::Point2F(cx2, cy2));
                                sink->AddLine(D2D1::Point2F(cx2 - 3.6f * scale, cy2));
                                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                                sink->Close();
                                m_renderTarget->FillGeometry(cornerTri.Get(), chevBrush);
                            }
                        }
                    } else {
                        float tx = button.rect.right - 5.2f * scale;
                        float ty = (button.rect.top + button.rect.bottom) * 0.5f;
                        m_renderTarget->DrawLine(D2D1::Point2F(tx - 2.0f * scale, ty - 0.8f * scale),
                                                 D2D1::Point2F(tx, ty + 1.2f * scale), chevBrush, 1.25f * scale);
                        m_renderTarget->DrawLine(D2D1::Point2F(tx, ty + 1.2f * scale),
                                                 D2D1::Point2F(tx + 2.0f * scale, ty - 0.8f * scale), chevBrush, 1.25f * scale);
                    }
                }
            }
        }
    };

    // 绘制主工具栏、二级属性栏与选区侧边独立悬浮正圆微晶圆钮 (PixPin 标杆：无笨重白底板，曜石黑圆底 + 天际蓝高亮激活态)
    drawButtonGroup(state.toolbarButtons);
    if (!state.secondaryToolbarButtons.empty()) {
        drawButtonGroup(state.secondaryToolbarButtons);
    }
    if (!state.selectionSideButtons.empty()) {
        ComPtr<ID2D1SolidColorBrush> sideShadowBrush, sideIdleBg, sideHoverBg, sideActiveBg, sideActiveHoverBg, sideRimBrush, sideIconBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.24f), sideShadowBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.72f), sideIdleBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.88f), sideHoverBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(14.0f / 255.0f, 165.0f / 255.0f, 233.0f / 255.0f, 0.96f), sideActiveBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(2.0f / 255.0f, 132.0f / 255.0f, 199.0f / 255.0f, 0.98f), sideActiveHoverBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f), sideRimBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), sideIconBrush.GetAddressOf());

        for (const auto& sbtn : state.selectionSideButtons) {
            const float cxBtn = (sbtn.rect.left + sbtn.rect.right) * 0.5f;
            const float cyBtn = (sbtn.rect.top + sbtn.rect.bottom) * 0.5f;
            const float radBtn = (sbtn.rect.right - sbtn.rect.left) * 0.5f;
            const float dxCur = static_cast<float>(state.currentCursor.x) - cxBtn;
            const float dyCur = static_cast<float>(state.currentCursor.y) - cyBtn;
            const bool isHovered = (dxCur * dxCur + dyCur * dyCur <= (radBtn + 1.0f * scale) * (radBtn + 1.0f * scale));

            bool isActive = false;
            if (sbtn.command == ToolbarCommand::SideToggleCornerRadius) {
                isActive = (state.effectiveCornerRadius() > 0.5f ||
                            state.sliderPopup.type == SliderPopupType::SelectionCornerRadius);
            } else if (sbtn.command == ToolbarCommand::SideCycleAspectRatio) {
                isActive = (state.aspectRatio != AspectRatioPreset::Free);
            } else if (sbtn.command == ToolbarCommand::SideInvertSelection) {
                isActive = state.hasSavedSelectionBeforeInvert;
            }

            if (sideShadowBrush) {
                m_renderTarget->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(cxBtn, cyBtn + 1.4f * scale), radBtn + 0.8f * scale, radBtn + 0.8f * scale),
                    sideShadowBrush.Get());
            }
            const auto disc = D2D1::Ellipse(D2D1::Point2F(cxBtn, cyBtn), radBtn, radBtn);
            ID2D1SolidColorBrush* fillBr = isActive
                ? (isHovered ? sideActiveHoverBg.Get() : sideActiveBg.Get())
                : (isHovered ? sideHoverBg.Get() : sideIdleBg.Get());
            if (fillBr) {
                m_renderTarget->FillEllipse(disc, fillBr);
            }
            if (sideRimBrush) {
                m_renderTarget->DrawEllipse(disc, sideRimBrush.Get(), 1.0f * scale);
            }
            if (sideIconBrush) {
                drawVectorButtonIcon(sbtn, sbtn.rect, sideIconBrush.Get(), scale);
            }
        }
    }

    // 绘制展开的二级下拉悬浮菜单
    drawSubmenu(state);

    // 绘制无级滑块与预设悬浮弹窗
    drawSliderPopup(state);

    // 绘制多态微晶下拉菜单
    drawDropdownMenu(state);

    // 悬停浮层提示：世界级微晶胶囊 (DirectWrite 精确排版度量 + 严苛 ASCII 快捷键检测 + 跨双栏/侧边栏防遮挡)
    auto showTooltip = [&](const std::vector<ToolbarButton>& btnList, bool isVerticalSideBar = false) -> bool {
        for (const auto& button : btnList) {
            if (state.currentCursor.x < button.rect.left || state.currentCursor.x > button.rect.right ||
                state.currentCursor.y < button.rect.top  || state.currentCursor.y > button.rect.bottom) {
                continue;
            }
            std::wstring tip = tooltipForButton(button, state.toolbarLayoutChinese);
            if (tip.empty()) continue;

            std::wstring labelPart = tip;
            std::wstring kbdPart = L"";
            auto openPos = tip.rfind(L'(');
            auto closePos = tip.rfind(L')');
            if (openPos != std::wstring::npos && closePos != std::wstring::npos && closePos > openPos) {
                std::wstring cand = tip.substr(openPos + 1, closePos - openPos - 1);
                bool isAsciiShortcut = !cand.empty();
                for (wchar_t ch : cand) {
                    if (ch > 127 || ch == L'（' || ch == L'）') {
                        isAsciiShortcut = false;
                        break;
                    }
                }
                if (isAsciiShortcut) {
                    labelPart = tip.substr(0, openPos);
                    while (!labelPart.empty() && labelPart.back() == L' ') labelPart.pop_back();
                    kbdPart = cand;
                }
            }

            float labelW = 0.0f;
            float textH = 14.0f * scale;
            if (m_dwriteFactory && m_infoTextFormat) {
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    labelPart.c_str(), static_cast<UINT32>(labelPart.size()),
                    m_infoTextFormat.Get(), 800.0f * scale, 100.0f * scale, layout.GetAddressOf())) && layout) {
                    DWRITE_TEXT_METRICS m{};
                    if (SUCCEEDED(layout->GetMetrics(&m))) {
                        labelW = m.width;
                        textH = (std::max)(textH, m.height);
                    }
                }
            }
            if (labelW <= 0.0f) {
                labelW = static_cast<float>(labelPart.size()) * 11.5f * scale;
            }

            float kbdW = 0.0f;
            if (!kbdPart.empty()) {
                if (m_dwriteFactory && m_infoTextFormat) {
                    ComPtr<IDWriteTextLayout> kLayout;
                    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                        kbdPart.c_str(), static_cast<UINT32>(kbdPart.size()),
                        m_infoTextFormat.Get(), 400.0f * scale, 100.0f * scale, kLayout.GetAddressOf())) && kLayout) {
                        DWRITE_TEXT_METRICS km{};
                        if (SUCCEEDED(kLayout->GetMetrics(&km))) {
                            kbdW = km.width + 10.0f * scale;
                        }
                    }
                }
                if (kbdW <= 0.0f) {
                    kbdW = (static_cast<float>(kbdPart.size()) * 7.5f + 10.0f) * scale;
                }
            }

            const float padX = 10.0f * scale;
            const float kbdGap = kbdPart.empty() ? 0.0f : 8.0f * scale;
            float tw = padX * 2.0f + labelW + kbdGap + kbdW;
            float th = 26.0f * scale;

            auto sz = m_renderTarget->GetSize();
            auto tipRect = calculateTooltipRect(button.rect, tw, th, scale, sz,
                                                state.toolbarLayoutSelection,
                                                state.secondaryToolbarRect,
                                                state.primaryToolbarRect,
                                                isVerticalSideBar);

            FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), tipRect, 7.0f * scale, false);

            ComPtr<ID2D1SolidColorBrush> textBrush, kbdBgBrush, kbdBorderBrush, kbdTextBrush;
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, textBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kShortcutKeyBg, kbdBgBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kShortcutKeyBorder, kbdBorderBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kShortcutKeyText, kbdTextBrush.GetAddressOf());

            float curLeft = tipRect.left + padX;
            if (m_infoTextFormat && textBrush) {
                TextFormatScope scopeLabel(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(labelPart.c_str(), static_cast<UINT32>(labelPart.size()),
                    m_infoTextFormat.Get(),
                    D2D1::RectF(curLeft, tipRect.top, curLeft + labelW + 2.0f * scale, tipRect.bottom),
                    textBrush.Get());
            }

            if (!kbdPart.empty() && kbdBgBrush && kbdBorderBrush && kbdTextBrush && m_infoTextFormat) {
                float kx = curLeft + labelW + kbdGap;
                float kh = 17.0f * scale;
                float ky = tipRect.top + (th - kh) * 0.5f;
                auto kbdRect = D2D1::RectF(kx, ky, kx + kbdW, ky + kh);
                auto kbdRound = D2D1::RoundedRect(kbdRect, 4.0f * scale, 4.0f * scale);
                m_renderTarget->FillRoundedRectangle(kbdRound, kbdBgBrush.Get());
                m_renderTarget->DrawRoundedRectangle(kbdRound, kbdBorderBrush.Get(), 1.0f * scale);
                TextFormatScope scopeKbd(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(kbdPart.c_str(), static_cast<UINT32>(kbdPart.size()),
                    m_infoTextFormat.Get(),
                    kbdRect,
                    kbdTextBrush.Get());
            }
            return true;
        }
        return false;
    };

    const bool isEditingTextNow = (state.activeElement != nullptr &&
                                   state.activeElement->tool == MarkupTool::Text &&
                                   state.activeElement->isEditing);
    if (!hasAnyCapturePopupOpen(state) && !isActiveDragInteraction &&
        !state.isDraggingToolbar && !isEditingTextNow) {
        if (!showTooltip(state.toolbarButtons, false)) {
            if (!showTooltip(state.secondaryToolbarButtons, false)) {
                showTooltip(state.selectionSideButtons, true);
            }
        }
    }

    if (pushedGhostLayer) {
        m_renderTarget->PopLayer();
    }
}

static D2D1_RECT_F resolveSecondaryPopupRect(const CaptureState& state,
                                             ToolbarCommand triggerCmd,
                                             float popupW,
                                             float popupH,
                                             D2D1_SIZE_F surfaceSize,
                                             float scale) {
    const float margin = 8.0f * scale;
    const float gapY = 6.0f * scale;

    D2D1_RECT_F anchorRect{};
    bool foundAnchor = false;
    for (const auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == triggerCmd && btn.rect.right > btn.rect.left) {
            anchorRect = btn.rect;
            foundAnchor = true;
            break;
        }
    }
    if (!foundAnchor) {
        for (const auto& btn : state.toolbarButtons) {
            if (btn.command == triggerCmd && btn.rect.right > btn.rect.left) {
                anchorRect = btn.rect;
                foundAnchor = true;
                break;
            }
        }
    }

    const bool hasSecondary = (state.secondaryToolbarRect.right > state.secondaryToolbarRect.left &&
                               state.secondaryToolbarRect.bottom > state.secondaryToolbarRect.top);
    const bool hasPrimary = (state.primaryToolbarRect.right > state.primaryToolbarRect.left &&
                             state.primaryToolbarRect.bottom > state.primaryToolbarRect.top);
    const D2D1_RECT_F baseBarRect = hasSecondary ? state.secondaryToolbarRect : state.primaryToolbarRect;

    const float rawX = foundAnchor ? anchorRect.left : (baseBarRect.left + margin);
    const float maxPopupX = (std::max)(margin, surfaceSize.width - popupW - margin);
    const float popupX = std::clamp(rawX, margin, maxPopupX);

    float bottomAnchor = baseBarRect.bottom;
    float topAnchor = baseBarRect.top;
    if (hasPrimary && hasSecondary) {
        const bool stacksAdjacent =
            std::abs(state.primaryToolbarRect.bottom - state.secondaryToolbarRect.top) <= 22.0f * scale ||
            std::abs(state.secondaryToolbarRect.bottom - state.primaryToolbarRect.top) <= 22.0f * scale;
        if (stacksAdjacent) {
            bottomAnchor = (std::max)(state.primaryToolbarRect.bottom, state.secondaryToolbarRect.bottom);
            topAnchor = (std::min)(state.primaryToolbarRect.top, state.secondaryToolbarRect.top);
        }
    }

    float popupY = bottomAnchor + gapY;
    if (popupY + popupH > surfaceSize.height - margin) {
        popupY = topAnchor - popupH - gapY;
    }
    popupY = std::clamp(popupY, margin, (std::max)(margin, surfaceSize.height - popupH - margin));

    return D2D1::RectF(popupX, popupY, popupX + popupW, popupY + popupH);
}

void CaptureRenderer::drawSubmenu(CaptureState& state) {
    if (state.openSubmenu == SubmenuType::None || !m_renderTarget) return;

    float scale = (state.dpiScale > 0.1f) ? state.dpiScale : 1.0f;
    
    // 线条样式下拉菜单 (实线、长虚线、点虚线、点划线)
    if (state.openSubmenu == SubmenuType::LineStyle) {
        float menuW = 152.0f * scale;
        float itemH = 32.0f * scale;
        float itemGap = 4.0f * scale;
        float padX = 8.0f * scale;
        float padY = 8.0f * scale;

        struct StyleItem { LineStyle style; std::wstring label; };
        std::vector<StyleItem> items = {
            { LineStyle::Solid, state.toolbarLayoutChinese ? L"实线" : L"Solid" },
            { LineStyle::Dashed, state.toolbarLayoutChinese ? L"长虚线" : L"Dashed" },
            { LineStyle::Dotted, state.toolbarLayoutChinese ? L"点虚线" : L"Dotted" },
            { LineStyle::DashDot, state.toolbarLayoutChinese ? L"点划线" : L"DashDot" },
        };

        float menuH = items.size() * itemH + (items.size() - 1) * itemGap + 2.0f * padY;

        auto sz = m_renderTarget->GetSize();
        auto menuRect = resolveSecondaryPopupRect(
            state, ToolbarCommand::ToggleLineStyleDropdown, menuW, menuH, sz, scale);
        float menuX = menuRect.left;
        float menuY = menuRect.top;
        state.openSubmenuRect = menuRect;
        state.submenuButtons.clear();

        drawGlassPanel(menuRect, 9.0f * scale, false);

        ComPtr<ID2D1SolidColorBrush> itemHoverBg, itemActiveBg, textBrush, lineBrush, activeLineBrush;
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.045f),
            itemHoverBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.085f),
            itemActiveBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            CaptureHudPalette::kSecondaryText,
            textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(51.0f / 255.0f, 65.0f / 255.0f, 85.0f / 255.0f, 0.92f),
            lineBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            CaptureHudPalette::kPrimaryText,
            activeLineBrush.GetAddressOf());

        for (size_t i = 0; i < items.size(); ++i) {
            float iy = menuY + padY + i * (itemH + itemGap);
            auto itemRect = D2D1::RectF(menuX + padX, iy, menuX + menuW - padX, iy + itemH);

            ToolbarButton sbtn;
            sbtn.command = ToolbarCommand::SelectLineStyle;
            sbtn.lineStyleParam = items[i].style;
            sbtn.rect = itemRect;
            state.submenuButtons.push_back(sbtn);

            bool isSelected = (state.currentLineStyle == items[i].style);
            bool isHovered = (state.currentCursor.x >= itemRect.left && state.currentCursor.x <= itemRect.right &&
                              state.currentCursor.y >= itemRect.top  && state.currentCursor.y <= itemRect.bottom);

            if (isSelected) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 5.5f * scale, 5.5f * scale), itemActiveBg.Get());
            } else if (isHovered) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 5.5f * scale, 5.5f * scale), itemHoverBg.Get());
            }

            // 绘制线条样式矢量线段
            auto iconId = (items[i].style == LineStyle::Dashed) ? CaptureIconId::PropDashedLine
                        : (items[i].style == LineStyle::Dotted) ? CaptureIconId::PropDottedLine
                        : (items[i].style == LineStyle::DashDot) ? CaptureIconId::PropDashDotLine
                        : CaptureIconId::PropSolidLine;
            auto lineRect = D2D1::RectF(itemRect.left + 6.0f * scale, itemRect.top, itemRect.left + 48.0f * scale, itemRect.bottom);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, lineRect,
                                           isSelected ? activeLineBrush.Get() : lineBrush.Get(), scale);

            // 绘制文字说明
            if (m_infoTextFormat && textBrush) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(items[i].label.c_str(), static_cast<UINT32>(items[i].label.size()),
                    m_infoTextFormat.Get(),
                    D2D1::RectF(itemRect.left + 54.0f * scale, iy, itemRect.right - 20.0f * scale, iy + itemH),
                    isSelected ? activeLineBrush.Get() : textBrush.Get());
            }

            // 选中项右侧微对勾 ✓
            if (isSelected) {
                float rcx = itemRect.right - 14.0f * scale;
                float rcy = iy + itemH * 0.5f;
                D2D1_POINT_2F p1{rcx - 3.5f * scale, rcy};
                D2D1_POINT_2F p2{rcx - 1.0f * scale, rcy + 2.8f * scale};
                D2D1_POINT_2F p3{rcx + 3.8f * scale, rcy - 3.2f * scale};
                m_renderTarget->DrawLine(p1, p2, activeLineBrush.Get(), 1.5f * scale);
                m_renderTarget->DrawLine(p2, p3, activeLineBrush.Get(), 1.5f * scale);
            }
        }
    }
    // 箭头样式下拉菜单 (标准单向、细线折角、圆润机翼、双向对称)
    else if (state.openSubmenu == SubmenuType::ArrowStyle) {
        float menuW = 152.0f * scale;
        float itemH = 32.0f * scale;
        float itemGap = 4.0f * scale;
        float padX = 8.0f * scale;
        float padY = 8.0f * scale;

        struct ArrowItem { ArrowStyle style; std::wstring label; };
        std::vector<ArrowItem> items = {
            { ArrowStyle::Standard, state.toolbarLayoutChinese ? L"标准单向" : L"Standard" },
            { ArrowStyle::Thin, state.toolbarLayoutChinese ? L"细线箭头" : L"Thin" },
            { ArrowStyle::Tech, state.toolbarLayoutChinese ? L"圆润机翼" : L"Swept Wing" },
            { ArrowStyle::DoubleEnded, state.toolbarLayoutChinese ? L"双向箭头" : L"Double" },
        };

        float menuH = items.size() * itemH + (items.size() - 1) * itemGap + 2.0f * padY;

        auto sz = m_renderTarget->GetSize();
        auto menuRect = resolveSecondaryPopupRect(
            state, ToolbarCommand::ToggleArrowStyleDropdown, menuW, menuH, sz, scale);
        float menuX = menuRect.left;
        float menuY = menuRect.top;
        state.openSubmenuRect = menuRect;
        state.submenuButtons.clear();

        drawGlassPanel(menuRect, 9.0f * scale, false);

        ComPtr<ID2D1SolidColorBrush> itemHoverBg, itemActiveBg, textBrush, lineBrush, activeLineBrush;
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.045f),
            itemHoverBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.085f),
            itemActiveBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            CaptureHudPalette::kSecondaryText,
            textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(51.0f / 255.0f, 65.0f / 255.0f, 85.0f / 255.0f, 0.92f),
            lineBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            CaptureHudPalette::kPrimaryText,
            activeLineBrush.GetAddressOf());

        for (size_t i = 0; i < items.size(); ++i) {
            float iy = menuY + padY + i * (itemH + itemGap);
            auto itemRect = D2D1::RectF(menuX + padX, iy, menuX + menuW - padX, iy + itemH);

            ToolbarButton sbtn;
            sbtn.command = ToolbarCommand::SelectArrowStyle;
            sbtn.arrowStyleParam = items[i].style;
            sbtn.rect = itemRect;
            state.submenuButtons.push_back(sbtn);

            bool isSelected = (state.currentArrowStyle == items[i].style);
            bool isHovered = (state.currentCursor.x >= itemRect.left && state.currentCursor.x <= itemRect.right &&
                              state.currentCursor.y >= itemRect.top  && state.currentCursor.y <= itemRect.bottom);

            if (isSelected) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 5.5f * scale, 5.5f * scale), itemActiveBg.Get());
            } else if (isHovered) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 5.5f * scale, 5.5f * scale), itemHoverBg.Get());
            }

            auto arrowIconId = (items[i].style == ArrowStyle::DoubleEnded) ? CaptureIconId::ToolArrowDouble
                             : (items[i].style == ArrowStyle::Thin) ? CaptureIconId::ToolArrowThin
                             : (items[i].style == ArrowStyle::Tech) ? CaptureIconId::ToolArrowWing
                             : CaptureIconId::ToolArrowStandard;
            auto arrowRect = D2D1::RectF(itemRect.left + 6.0f * scale, itemRect.top, itemRect.left + 42.0f * scale, itemRect.bottom);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), arrowIconId, arrowRect,
                                           isSelected ? activeLineBrush.Get() : lineBrush.Get(), scale);

            if (m_infoTextFormat && textBrush) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(items[i].label.c_str(), static_cast<UINT32>(items[i].label.size()),
                    m_infoTextFormat.Get(),
                    D2D1::RectF(itemRect.left + 48.0f * scale, iy, itemRect.right - 20.0f * scale, iy + itemH),
                    isSelected ? activeLineBrush.Get() : textBrush.Get());
            }

            // 选中项右侧微对勾 ✓
            if (isSelected) {
                float rcx = itemRect.right - 14.0f * scale;
                float rcy = iy + itemH * 0.5f;
                D2D1_POINT_2F p1{rcx - 3.5f * scale, rcy};
                D2D1_POINT_2F p2{rcx - 1.0f * scale, rcy + 2.8f * scale};
                D2D1_POINT_2F p3{rcx + 3.8f * scale, rcy - 3.2f * scale};
                m_renderTarget->DrawLine(p1, p2, activeLineBrush.Get(), 1.5f * scale);
                m_renderTarget->DrawLine(p2, p3, activeLineBrush.Get(), 1.5f * scale);
            }
        }
    }
}

void CaptureRenderer::drawSliderPopup(CaptureState& state) {
    if (state.sliderPopup.type == SliderPopupType::None || !m_renderTarget) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);

    std::wstring title = (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius)
        ? (state.beautyShell.enabled
            ? (state.toolbarLayoutChinese ? L"外壳圆角半径" : L"Shell Radius")
            : (state.toolbarLayoutChinese ? L"选区圆角半径" : L"Selection Radius"))
        : (state.sliderPopup.type == SliderPopupType::CornerRadius)
        ? (state.toolbarLayoutChinese ? L"标注圆角" : L"Corner Radius")
        : (state.sliderPopup.type == SliderPopupType::SpotlightDim)
        ? (state.toolbarLayoutChinese ? L"聚光灯暗化" : L"Spotlight Dim")
        : (state.sliderPopup.type == SliderPopupType::TextFontSize)
        ? (state.toolbarLayoutChinese ? L"文本字号" : L"Font Size")
        : (state.sliderPopup.type == SliderPopupType::MosaicBlockSize)
        ? (state.toolbarLayoutChinese ? L"马赛克粗细" : L"Mosaic Size")
        : (state.toolbarLayoutChinese ? L"标注线宽" : L"Stroke Width");

    int curVal = (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius)
        ? static_cast<int>(std::round(state.effectiveCornerRadius()))
        : (state.sliderPopup.type == SliderPopupType::CornerRadius)
        ? static_cast<int>(std::round(
              (state.activeElement &&
               (state.activeElement->tool == MarkupTool::Rectangle ||
                state.activeElement->tool == MarkupTool::Spotlight))
                  ? state.activeElement->cornerRadius
                  : state.currentElementCornerRadius))
        : (state.sliderPopup.type == SliderPopupType::SpotlightDim)
        ? std::clamp(static_cast<int>(std::round(state.currentSpotlightDimAlpha * 100.0f)), 15, 85)
        : (state.sliderPopup.type == SliderPopupType::TextFontSize)
        ? static_cast<int>(std::round(state.activeElement && state.activeElement->tool == MarkupTool::Text ? state.activeElement->fontSize : tools3000::core::ConfigManager::instance().get<double>("/capture/markup/text/fontSize", 18.0)))
        : state.currentStrokeWidth;

    state.sliderPopup.currentValue = curVal;
    int minV = (state.sliderPopup.type == SliderPopupType::SpotlightDim) ? 15 :
               (state.sliderPopup.type == SliderPopupType::TextFontSize) ? 10 :
               (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius || state.sliderPopup.type == SliderPopupType::CornerRadius) ? 0 : 1;
    int maxV = (state.sliderPopup.type == SliderPopupType::SpotlightDim) ? 85 :
               (state.sliderPopup.type == SliderPopupType::TextFontSize) ? 72 :
               (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius) ? 80 :
               (state.sliderPopup.type == SliderPopupType::CornerRadius) ? 40 :
               (state.sliderPopup.type == SliderPopupType::MosaicBlockSize) ? 32 : 28;
    state.sliderPopup.minValue = minV;
    state.sliderPopup.maxValue = maxV;
    state.sliderPopup.presetButtons.clear();

    const std::wstring valStr = (state.sliderPopup.type == SliderPopupType::SpotlightDim)
        ? std::format(L"{}%", curVal)
        : std::format(L"{} px", curVal);
    const std::wstring wheelHint = state.toolbarLayoutChinese ? L"也可以用滚轮调整" : L"Scroll wheel also adjusts";
    const float titleW = measureText(title, m_infoTextFormat.Get(), 800.0f * scale, 64.0f * scale).width;
    const float valTextW = measureText(valStr, m_infoTextFormat.Get(), 400.0f * scale, 64.0f * scale).width;
    const float hintW = measureText(wheelHint, m_infoTextFormat.Get(), 800.0f * scale, 64.0f * scale).width;
    const float badgeW = (std::max)(44.0f * scale, valTextW + 14.0f * scale);
    const float popW = std::max({204.0f * scale, titleW + badgeW + 52.0f * scale, hintW + 28.0f * scale});
    const float popH = 72.0f * scale;

    auto sz = m_renderTarget->GetSize();
    const float margin = 8.0f * scale;
    D2D1_RECT_F popRect{};
    if (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius) {
        float popX = 0.0f;
        if (state.selectionSideRect.left >= state.toolbarLayoutSelection.right &&
            state.selectionSideRect.right + margin + popW <= sz.width - margin) {
            popX = state.selectionSideRect.right + margin;
        } else {
            popX = state.selectionSideRect.left - popW - margin;
        }
        popX = std::clamp(popX, margin, (std::max)(margin, sz.width - popW - margin));
        const float popY = std::clamp(state.selectionSideRect.top - 10.0f * scale,
                                      margin, (std::max)(margin, sz.height - popH - margin));
        popRect = D2D1::RectF(popX, popY, popX + popW, popY + popH);
    } else {
        const ToolbarCommand triggerCmd = (state.sliderPopup.type == SliderPopupType::CornerRadius)
            ? ToolbarCommand::CycleElementCornerRadius
            : (state.sliderPopup.type == SliderPopupType::SpotlightDim)
                ? ToolbarCommand::CycleSpotlightDim
                : ToolbarCommand::CycleStrokeWidth;
        popRect = resolveSecondaryPopupRect(state, triggerCmd, popW, popH, sz, scale);
    }

    const float popX = popRect.left;
    const float popY = popRect.top;
    state.sliderPopup.popupRect = popRect;

    // 1. 温润雪瓷白微晶浮岛
    drawGlassPanel(popRect, 10.0f * scale, false);

    ComPtr<ID2D1SolidColorBrush> titleBrush, badgeBg, badgeText, trackBg, trackFill, thumbBg, thumbBorder, thumbGlow, hintBrush;
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, titleBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kChipAccentBg, badgeBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kChipAccentText, badgeText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(226.0f / 255.0f, 232.0f / 255.0f, 240.0f / 255.0f, 0.98f), trackBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(37.0f / 255.0f, 99.0f / 255.0f, 235.0f / 255.0f, 0.92f), trackFill.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), thumbBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(37.0f / 255.0f, 99.0f / 255.0f, 235.0f / 255.0f, 0.96f), thumbBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(37.0f / 255.0f, 99.0f / 255.0f, 235.0f / 255.0f, 0.14f), thumbGlow.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSecondaryText, hintBrush.GetAddressOf());

    // 1. 顶部前置矢量微图标、标题与实时无级数值徽章（图标左边缘与下方滑轨严格垂直对齐）
    float topY = popY + 8.0f * scale;
    float badgeH = 19.0f * scale;
    auto valBadgeRect = D2D1::RectF(popX + popW - badgeW - 12.0f * scale, topY, popX + popW - 12.0f * scale, topY + badgeH);
    if (titleBrush) {
        const CaptureIconId popupIcon =
            (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius ||
             state.sliderPopup.type == SliderPopupType::CornerRadius)
                ? CaptureIconId::PropCornerRadius
                : (state.sliderPopup.type == SliderPopupType::SpotlightDim)
                    ? CaptureIconId::ToolSpotlight
                    : (state.sliderPopup.type == SliderPopupType::TextFontSize)
                        ? CaptureIconId::PropFontSize
                        : CaptureIconId::PropStrokeWidth;
        const float iconBoxSz = 23.0f * scale;
        const float iconCx = popX + 16.8f * scale;
        const float iconCy = topY + badgeH * 0.5f;
        const D2D1_RECT_F popupIconBox = D2D1::RectF(
            iconCx - iconBoxSz * 0.5f, iconCy - iconBoxSz * 0.5f,
            iconCx + iconBoxSz * 0.5f, iconCy + iconBoxSz * 0.5f);
        CaptureVectorIcons::renderIcon(
            m_renderTarget.Get(), m_d2dFactory.Get(), popupIcon, popupIconBox, titleBrush.Get(), scale);
    }
    if (m_infoTextFormat && titleBrush) {
        const float titleLeft = popX + 27.0f * scale;
        const D2D1_RECT_F titleRect = D2D1::RectF(
            titleLeft, topY, (std::max)(titleLeft, valBadgeRect.left - 8.0f * scale), topY + badgeH);
        TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawText(title.c_str(), static_cast<UINT32>(title.size()), m_infoTextFormat.Get(),
            titleRect, titleBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (badgeBg) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(valBadgeRect, 4.0f * scale, 4.0f * scale), badgeBg.Get());
    if (m_infoTextFormat && badgeText) {
        TextFormatScope scopeBadge(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawText(valStr.c_str(), static_cast<UINT32>(valStr.size()), m_infoTextFormat.Get(),
            valBadgeRect,
            badgeText.Get());
    }

    // 2. 中部无级滑动轨道与发光游标
    float trackX = popX + 12.0f * scale;
    float trackY = topY + 25.0f * scale;
    float trackW = popW - 24.0f * scale;
    float trackH = 5.0f * scale;
    auto trackRect = D2D1::RectF(trackX, trackY, trackX + trackW, trackY + trackH);
    state.sliderPopup.trackRect = trackRect;

    if (trackBg) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trackRect, 2.5f * scale, 2.5f * scale), trackBg.Get());

    float pct = (maxV > minV) ? std::clamp(static_cast<float>(curVal - minV) / static_cast<float>(maxV - minV), 0.0f, 1.0f) : 0.0f;
    float thumbX = trackX + pct * trackW;
    float thumbY = trackY + trackH * 0.5f;

    auto fillRect = D2D1::RectF(trackX, trackY, thumbX, trackY + trackH);
    if (trackFill && thumbX > trackX) {
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(fillRect, 2.5f * scale, 2.5f * scale), trackFill.Get());
    }

    float thumbR = (state.sliderPopup.isDragging ? 7.2f : 6.0f) * scale;
    auto thumbEllipse = D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR, thumbR);
    state.sliderPopup.thumbRect = D2D1::RectF(thumbX - thumbR - 2.0f * scale, thumbY - thumbR - 2.0f * scale, thumbX + thumbR + 2.0f * scale, thumbY + thumbR + 2.0f * scale);

    if (thumbGlow) m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR + 3.0f * scale, thumbR + 3.0f * scale), thumbGlow.Get());
    if (thumbBg) m_renderTarget->FillEllipse(thumbEllipse, thumbBg.Get());
    if (thumbBorder) m_renderTarget->DrawEllipse(thumbEllipse, thumbBorder.Get(), 1.5f * scale);

    // 3. 底部温和滚轮微调引导（替代刻板固定档位按钮行）
    if (m_infoTextFormat && hintBrush) {
        const float hintTop = trackY + 11.0f * scale;
        const D2D1_RECT_F hintRect = D2D1::RectF(popX + 12.0f * scale, hintTop, popX + popW - 12.0f * scale, popY + popH - 5.0f * scale);
        TextFormatScope hScope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawText(wheelHint.c_str(), static_cast<UINT32>(wheelHint.size()), m_infoTextFormat.Get(),
            hintRect, hintBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

static void getBeautyColors(BeautyBackgroundType type, D2D1_COLOR_F& c1, D2D1_COLOR_F& c2, D2D1_COLOR_F& c3) {
    switch (type) {
        case BeautyBackgroundType::StudioSlate:
            // 极光星云: Indigo (#6366F1) -> Violet (#8B5CF6) -> Rose (#EC4899)
            c1 = D2D1::ColorF(99.0f / 255.0f, 102.0f / 255.0f, 241.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(139.0f / 255.0f, 92.0f / 255.0f, 246.0f / 255.0f, 1.0f);
            c3 = D2D1::ColorF(236.0f / 255.0f, 72.0f / 255.0f, 153.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::PaperChalk:
            // 晨曦落日: Coral (#FB7185) -> Amber (#F97316) -> Gold (#FACC15)
            c1 = D2D1::ColorF(251.0f / 255.0f, 113.0f / 255.0f, 133.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(249.0f / 255.0f, 115.0f / 255.0f, 22.0f / 255.0f, 1.0f);
            c3 = D2D1::ColorF(250.0f / 255.0f, 204.0f / 255.0f, 21.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::SilkMist:
            // 碧海极光: Sky (#0EA5E9) -> Teal (#14B8A6) -> Emerald (#10B981)
            c1 = D2D1::ColorF(14.0f / 255.0f, 165.0f / 255.0f, 233.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(20.0f / 255.0f, 184.0f / 255.0f, 166.0f / 255.0f, 1.0f);
            c3 = D2D1::ColorF(16.0f / 255.0f, 185.0f / 255.0f, 129.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::MidnightGraphite:
            // 曜石黑晶: Slate (#334155) -> Obsidian (#1E293B) -> Carbon (#090D16)
            c1 = D2D1::ColorF(51.0f / 255.0f, 65.0f / 255.0f, 85.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, 1.0f);
            c3 = D2D1::ColorF(9.0f / 255.0f, 13.0f / 255.0f, 22.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::PureMinimal:
        default:
            // 钛金画廊: Alabaster (#F8FAFC) -> Silver (#E2E8F0) -> Titanium (#CBD5E1)
            c1 = D2D1::ColorF(248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(226.0f / 255.0f, 232.0f / 255.0f, 240.0f / 255.0f, 1.0f);
            c3 = D2D1::ColorF(203.0f / 255.0f, 213.0f / 255.0f, 225.0f / 255.0f, 1.0f);
            break;
    }
}

void CaptureRenderer::drawBeautyShellPreview(const D2D1_RECT_F& rect, CaptureState& state) {
    if (!state.beautyShell.enabled || !m_renderTarget) return;
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const float pad = state.beautyShell.padding * scale;
    if (pad <= 2.0f) return;

    D2D1_RECT_F shellRect = D2D1::RectF(rect.left - pad, rect.top - pad, rect.right + pad, rect.bottom + pad);
    const float shellCornerR = (std::max)(12.0f * scale, (state.beautyShell.cornerRadius + 6.0f) * scale);
    auto roundedShell = D2D1::RoundedRect(shellRect, shellCornerR, shellCornerR);

    // 1. 绘制三色段高饱和网格渐变底板画布
    D2D1_COLOR_F c1, c2, c3;
    getBeautyColors(state.beautyShell.bgType, c1, c2, c3);

    D2D1_GRADIENT_STOP stops[3];
    stops[0].position = 0.0f; stops[0].color = c1;
    stops[1].position = 0.5f; stops[1].color = c2;
    stops[2].position = 1.0f; stops[2].color = c3;

    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stopCol;
    if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(stops, 3, stopCol.GetAddressOf())) && stopCol) {
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gradBrush;
        auto gradProps = D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(shellRect.left, shellRect.top),
            D2D1::Point2F(shellRect.right, shellRect.bottom));
        if (SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(gradProps, stopCol.Get(), gradBrush.GetAddressOf())) && gradBrush) {
            m_renderTarget->FillRoundedRectangle(roundedShell, gradBrush.Get());
        }
    }

    // 叠加左上角环境柔光高光 (Mesh Light Bloom)
    D2D1_GRADIENT_STOP bloomStops[2];
    const float bloomAlpha = (state.beautyShell.bgType == BeautyBackgroundType::MidnightGraphite) ? 0.10f : 0.22f;
    bloomStops[0].position = 0.0f; bloomStops[0].color = D2D1::ColorF(1.0f, 1.0f, 1.0f, bloomAlpha);
    bloomStops[1].position = 1.0f; bloomStops[1].color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f);
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> bloomStopCol;
    if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(bloomStops, 2, bloomStopCol.GetAddressOf())) && bloomStopCol) {
        const float shellW = shellRect.right - shellRect.left;
        const float shellH = shellRect.bottom - shellRect.top;
        Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> bloomBrush;
        auto radialProps = D2D1::RadialGradientBrushProperties(
            D2D1::Point2F(shellRect.left + shellW * 0.18f, shellRect.top + shellH * 0.15f),
            D2D1::Point2F(0.0f, 0.0f),
            shellW * 0.65f, shellH * 0.65f);
        if (SUCCEEDED(m_renderTarget->CreateRadialGradientBrush(radialProps, bloomStopCol.Get(), bloomBrush.GetAddressOf())) && bloomBrush) {
            m_renderTarget->FillRoundedRectangle(roundedShell, bloomBrush.Get());
        }
    }

    // 外壳边缘微晶高光描边
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shellBorder;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), shellBorder.GetAddressOf());
    if (shellBorder) {
        m_renderTarget->DrawRoundedRectangle(roundedShell, shellBorder.Get(), 1.2f * scale);
    }

    // 2. 渲染 10 层平滑指数衰减立体悬浮弥散阴影 (True Multi-Step Soft Elevation Shadow)
    float r = state.beautyShell.cornerRadius * scale;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadowBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.04f, 0.09f, 1.0f), shadowBrush.GetAddressOf());
    if (shadowBrush) {
        const float maxSpread = std::min(pad * 0.78f, 26.0f * scale);
        const float maxDropY = std::min(pad * 0.42f, 12.0f * scale);
        constexpr int kShadowSteps = 10;
        for (int i = kShadowSteps; i >= 1; --i) {
            const float t = static_cast<float>(i) / static_cast<float>(kShadowSteps);
            const float spread = maxSpread * (t * t);
            const float dropY = maxDropY * std::pow(t, 1.35f);
            const float layerAlpha = (1.0f - t * 0.72f) * 0.052f;
            shadowBrush->SetOpacity(layerAlpha);
            auto sRect = D2D1::RoundedRect(
                D2D1::RectF(rect.left - spread, rect.top + dropY - spread * 0.45f,
                            rect.right + spread, rect.bottom + dropY + spread),
                r + spread, r + spread);
            m_renderTarget->FillRoundedRectangle(sRect, shadowBrush.Get());
        }
    }

    // 3. 将原图裁切为指定圆角后重新绘制于 rect 内部，并叠加双层微晶玻璃立体倒角
    if (hasScreenContent() && m_d2dFactory) {
        const float radiusInCanvas = state.isLongImageMode() ? state.beautyShell.cornerRadius : r;
        auto roundedClip = D2D1::RoundedRect(rect, radiusInCanvas, radiusInCanvas);
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
        if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(roundedClip, clipGeo.GetAddressOf())) && clipGeo) {
            Microsoft::WRL::ComPtr<ID2D1Layer> layer;
            if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
                m_renderTarget->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), layer.Get());
                if (!m_screenTiles.empty() && m_screenTilesW > 0 && m_screenTilesH > 0) {
                    const D2D1_RECT_F fullDocRect = D2D1::RectF(
                        0.0f, 0.0f, static_cast<float>(m_screenTilesW), static_cast<float>(m_screenTilesH));
                    drawTiledRegion(m_screenTiles, m_screenTilesW, m_screenTilesH,
                                    fullDocRect, fullDocRect, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                } else if (m_screenBitmap) {
                    auto sz = m_renderTarget->GetSize();
                    m_renderTarget->DrawBitmap(m_screenBitmap.Get(), D2D1::RectF(0, 0, sz.width, sz.height));
                }
                m_renderTarget->PopLayer();
            }
        }

        // 外沿 1px 暗部接触环 + 内沿 1.2px 高光玻璃环
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> outerRimBrush, innerSpecBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f), outerRimBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f), innerSpecBrush.GetAddressOf());
        if (outerRimBrush) {
            auto outerRing = D2D1::RoundedRect(
                D2D1::RectF(rect.left - 0.5f * scale, rect.top - 0.5f * scale, rect.right + 0.5f * scale, rect.bottom + 0.5f * scale),
                r + 0.5f * scale, r + 0.5f * scale);
            m_renderTarget->DrawRoundedRectangle(outerRing, outerRimBrush.Get(), 1.0f * scale);
        }
        if (innerSpecBrush) {
            auto innerRing = D2D1::RoundedRect(
                D2D1::RectF(rect.left + 0.6f * scale, rect.top + 0.6f * scale, rect.right - 0.6f * scale, rect.bottom - 0.6f * scale),
                std::max(0.0f, r - 0.6f * scale), std::max(0.0f, r - 0.6f * scale));
            m_renderTarget->DrawRoundedRectangle(innerRing, innerSpecBrush.Get(), 1.2f * scale);
        }
    }
}

void CaptureRenderer::drawDropdownMenu(CaptureState& state) {
    if (state.dropdownMenu.type == DropdownType::None || state.dropdownMenu.items.empty() || !m_renderTarget) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const bool isDark = false;

    // 1. 响应式微晶双列排版框架：严格测量全项主标题与辅助描述的最大宽度
    float maxLabelW = 0.0f;
    float maxHintW = 0.0f;
    bool hasAnySwatch = false;

    for (const auto& it : state.dropdownMenu.items) {
        if (!it.label.empty()) {
            D2D1_SIZE_F lsz = measureText(it.label, m_infoTextFormat.Get());
            if (lsz.width > maxLabelW) maxLabelW = lsz.width;
        }
        if (!it.hint.empty()) {
            D2D1_SIZE_F hsz = measureText(it.hint, m_infoTextFormat.Get());
            if (hsz.width > maxHintW) maxHintW = hsz.width;
        }
        if (it.hasSwatch) hasAnySwatch = true;
    }

    // 几何规格与自适应留白 (MECE 双列对齐，杜绝重叠与横向跑动)
    const float itemH = 32.0f * scale;
    const float padY = 7.0f * scale;
    const float padX = 10.0f * scale;
    const float swatchSpace = hasAnySwatch ? (12.0f * scale + 10.0f * scale) : 0.0f;
    const float columnGap = (maxHintW > 0.0f) ? 20.0f * scale : 0.0f; // 保证主副文本之间有绝对安全的呼吸间距
    const float checkSpace = 24.0f * scale; // 选中对勾区域

    float contentW = swatchSpace + maxLabelW + columnGap + maxHintW + checkSpace;
    float menuW = contentW + padX * 2.0f + 16.0f * scale;
    if (menuW < 148.0f * scale) menuW = 148.0f * scale;

    float menuH = padY * 2.0f + static_cast<float>(state.dropdownMenu.items.size()) * itemH;

    const auto& anchor = state.dropdownMenu.anchorButtonRect;
    auto rtSize = m_renderTarget->GetSize();
    float menuX = anchor.left;
    if (menuX + menuW > rtSize.width - 4.0f * scale) {
        menuX = rtSize.width - menuW - 4.0f * scale;
    }
    if (menuX < 4.0f * scale) menuX = 4.0f * scale;

    // 双向跨工具栏避让：若触发按钮正上方紧贴另一条工具栏（如二级栏在主工具栏下方），优先向下弹出；
    // 若优先向上但顶部空间不足向下翻转时，自动跨过紧贴的下层二级属性栏，绝不遮挡相邻工具栏！
    const float gapY = 6.0f * scale;
    float combinedTop = anchor.top;
    float combinedBottom = anchor.bottom;
    bool barAboveAnchor = false;
    bool barBelowAnchor = false;
    auto inspectBarForDropdown = [&](const D2D1_RECT_F& bar) {
        if (bar.right <= bar.left || bar.bottom <= bar.top) return;
        combinedTop = (std::min)(combinedTop, bar.top);
        combinedBottom = (std::max)(combinedBottom, bar.bottom);
        const bool xOverlap = (menuX < bar.right + 4.0f * scale) && (menuX + menuW > bar.left - 4.0f * scale);
        if (!xOverlap) return;
        if (bar.bottom <= anchor.top + 4.0f * scale && anchor.top - bar.bottom <= 22.0f * scale) {
            barAboveAnchor = true;
        }
        if (bar.top >= anchor.bottom - 4.0f * scale && bar.top - anchor.bottom <= 22.0f * scale) {
            barBelowAnchor = true;
        }
    };
    inspectBarForDropdown(state.primaryToolbarRect);
    inspectBarForDropdown(state.secondaryToolbarRect);

    float menuY = 0.0f;
    if (barAboveAnchor && !barBelowAnchor) {
        menuY = anchor.bottom + gapY;
        if (menuY + menuH > rtSize.height - 4.0f * scale) {
            menuY = combinedTop - menuH - gapY;
        }
    } else {
        menuY = (barAboveAnchor ? combinedTop : anchor.top) - menuH - gapY;
        if (menuY < 4.0f * scale) {
            menuY = (barBelowAnchor ? combinedBottom : anchor.bottom) + gapY;
        }
    }
    menuY = std::clamp(menuY, 4.0f * scale, (std::max)(4.0f * scale, rtSize.height - menuH - 4.0f * scale));

    state.dropdownMenu.menuRect = D2D1::RectF(menuX, menuY, menuX + menuW, menuY + menuH);

    // 2. 绘制微晶玻璃浮岛底板
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), state.dropdownMenu.menuRect, 8.0f * scale, isDark);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush, hintBrush, checkBrush;
    m_renderTarget->CreateSolidColorBrush(
        CaptureHudPalette::kPrimaryText,
        textBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        CaptureHudPalette::kSecondaryText,
        hintBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        CaptureHudPalette::kPrimaryText,
        checkBrush.GetAddressOf());

    // 确保文本格式作用域隔离为左对齐、垂直居中、不自动换行
    TextFormatScope menuTextScope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);

    // 3. 绘制各选项 (双列严格对齐)
    float curY = menuY + padY;
    const float labelStartX = menuX + padX + 8.0f * scale + swatchSpace;
    const float hintStartX = labelStartX + maxLabelW + columnGap;

    for (size_t i = 0; i < state.dropdownMenu.items.size(); ++i) {
        auto& item = state.dropdownMenu.items[i];
        item.rect = D2D1::RectF(menuX + padX, curY, menuX + menuW - padX, curY + itemH);

        bool isHover = (static_cast<int>(i) == state.dropdownMenu.hoveredIndex);
        item.isHovered = isHover;

        if (isHover || item.isSelected) {
            FloatingGlassBar::drawButtonPill(
                m_renderTarget.Get(), item.rect, 5.0f * scale,
                isHover, item.isSelected, false, false, 1.0f, isDark);
        }

        // 微型渐变色板圆球
        if (item.hasSwatch) {
            float swatchRadius = 6.0f * scale;
            float swatchCx = item.rect.left + 8.0f * scale + swatchRadius;
            float swatchCy = curY + itemH * 0.5f;

            D2D1_GRADIENT_STOP swatchStops[2];
            swatchStops[0].position = 0.0f; swatchStops[0].color = item.swatchColor1;
            swatchStops[1].position = 1.0f; swatchStops[1].color = item.swatchColor2;

            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> scCol;
            if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(swatchStops, 2, scCol.GetAddressOf())) && scCol) {
                Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> scBrush;
                auto scProps = D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(swatchCx - swatchRadius, swatchCy - swatchRadius),
                    D2D1::Point2F(swatchCx + swatchRadius, swatchCy + swatchRadius));
                if (SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(scProps, scCol.Get(), scBrush.GetAddressOf())) && scBrush) {
                    auto ellipse = D2D1::Ellipse(D2D1::Point2F(swatchCx, swatchCy), swatchRadius, swatchRadius);
                    m_renderTarget->FillEllipse(ellipse, scBrush.Get());
                    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> scBorder;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f), scBorder.GetAddressOf());
                    if (scBorder) m_renderTarget->DrawEllipse(ellipse, scBorder.Get(), 1.0f);
                }
            }
        }

        // 绘制标题 Label (首列)
        if (m_infoTextFormat && textBrush && !item.label.empty()) {
            D2D1_RECT_F textRect = D2D1::RectF(labelStartX, curY, labelStartX + maxLabelW + 2.0f * scale, curY + itemH);
            m_renderTarget->DrawText(item.label.c_str(), static_cast<UINT32>(item.label.size()),
                                     m_infoTextFormat.Get(), textRect, textBrush.Get());
        }

        // 绘制副标题 Hint (次列：全列表横向绝对统一对齐，杜绝跑动与重叠)
        if (m_infoTextFormat && hintBrush && !item.hint.empty()) {
            D2D1_RECT_F hintRect = D2D1::RectF(hintStartX, curY, item.rect.right - checkSpace, curY + itemH);
            m_renderTarget->DrawText(item.hint.c_str(), static_cast<UINT32>(item.hint.size()),
                                     m_infoTextFormat.Get(), hintRect, hintBrush.Get());
        }

        // 绘制选中对勾 (Checkmark)
        if (item.isSelected && checkBrush) {
            float ckX = item.rect.right - 14.0f * scale;
            float ckY = curY + itemH * 0.5f;
            D2D1_POINT_2F p1{ckX - 4.0f * scale, ckY};
            D2D1_POINT_2F p2{ckX - 1.0f * scale, ckY + 3.0f * scale};
            D2D1_POINT_2F p3{ckX + 5.0f * scale, ckY - 3.0f * scale};
            m_renderTarget->DrawLine(p1, p2, checkBrush.Get(), 1.8f * scale);
            m_renderTarget->DrawLine(p2, p3, checkBrush.Get(), 1.8f * scale);
        }

        curY += itemH;
    }
}

void CaptureRenderer::drawVectorButtonIcon(const ToolbarButton& button, const D2D1_RECT_F& rect, ID2D1Brush* brush, float scale) {
    if (!m_renderTarget || !brush) return;

    float cx = (rect.left + rect.right) * 0.5f;
    float cy = (rect.top + rect.bottom) * 0.5f;

    // 二级属性栏「左矢量图标 + 右数值文本」步进胶囊黄金光学排版（避免 18px 窄槽位触发 0.63 二次微缩失真）
    auto drawStepperIconAndLabel = [&](CaptureIconId iconId) {
        if (button.label.empty()) {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, rect, brush, scale);
            return;
        }
        const float btnH = rect.bottom - rect.top;
        const float iconBoxSz = (std::min)(btnH * 0.84f, 26.8f * scale);
        const float iconCx = rect.left + 13.0f * scale;
        const D2D1_RECT_F iconBox = D2D1::RectF(
            iconCx - iconBoxSz * 0.5f, cy - iconBoxSz * 0.5f,
            iconCx + iconBoxSz * 0.5f, cy + iconBoxSz * 0.5f);
        CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, iconBox, brush, scale);
        if (m_infoTextFormat) {
            const float rightInset = button.hasDropdown ? 10.5f * scale : 4.5f * scale;
            const float textLeft = rect.left + 21.5f * scale;
            const D2D1_RECT_F textRect = D2D1::RectF(
                textLeft, rect.top, (std::max)(textLeft, rect.right - rightInset), rect.bottom);
            TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER,
                                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                m_infoTextFormat.Get(), textRect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    };

    switch (button.command) {
        case ToolbarCommand::Copy:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionCopy, rect, brush, scale);
            return;
        case ToolbarCommand::Confirm:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionConfirm, rect, brush, scale);
            return;
        case ToolbarCommand::Cancel:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionCancel, rect, brush, scale);
            return;
        case ToolbarCommand::Undo:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionUndo, rect, brush, scale);
            return;
        case ToolbarCommand::Redo:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionRedo, rect, brush, scale);
            return;
        case ToolbarCommand::Clear:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionClear, rect, brush, scale);
            return;
        case ToolbarCommand::SideToggleCornerRadius:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropCornerRadius, rect, brush, scale);
            return;
        case ToolbarCommand::SideInvertSelection:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionInvertSelection, rect, brush, scale);
            return;
        case ToolbarCommand::SideResetSelection:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionResetSelection, rect, brush, scale);
            return;
        case ToolbarCommand::ExtractText:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionExtractText, rect, brush, scale);
            return;
        case ToolbarCommand::PinWindow:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionPinWindow, rect, brush, scale);
            return;
        case ToolbarCommand::ScrollCapture:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionScrollCapture, rect, brush, scale);
            return;
        case ToolbarCommand::StartRecord:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionRecordVideo, rect, brush, scale);
            return;
        case ToolbarCommand::SelectTool: {
            switch (button.tool) {
                case MarkupTool::Rectangle:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolRectangle, rect, brush, scale);
                    return;
                case MarkupTool::Line:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolLine, rect, brush, scale);
                    return;
                case MarkupTool::Ellipse:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolEllipse, rect, brush, scale);
                    return;
                case MarkupTool::Arrow:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolArrow, rect, brush, scale);
                    return;
                case MarkupTool::Pen:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolPen, rect, brush, scale);
                    return;
                case MarkupTool::Highlight:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolHighlight, rect, brush, scale);
                    return;
                case MarkupTool::Mosaic:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolMosaic, rect, brush, scale);
                    return;
                case MarkupTool::Text:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolText, rect, brush, scale);
                    return;
                case MarkupTool::Number:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolNumber, rect, brush, scale);
                    return;
                case MarkupTool::Magnifier:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolMagnifier, rect, brush, scale);
                    return;
                case MarkupTool::Spotlight:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolSpotlight, rect, brush, scale);
                    return;
                case MarkupTool::Inpaint:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolInpaint, rect, brush, scale);
                    return;
                default: break;
            }
            break;
        }
        case ToolbarCommand::ToggleFill: {
            // 文字工具下为背景胶囊开关；局部放大下为倍率角标开关；聚光灯下为描边外框开关；几何形状下为实心/空心切换
            const auto fillIcon = (button.tool == MarkupTool::Text)
                ? CaptureIconId::PropTextBackground
                : (button.tool == MarkupTool::Magnifier)
                    ? CaptureIconId::PropMagnifierBadge
                    : (button.tool == MarkupTool::Spotlight)
                        ? CaptureIconId::PropFillOutline
                        : (button.boolParam ? CaptureIconId::PropFillSolid : CaptureIconId::PropFillOutline);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), fillIcon, rect, brush, scale);
            return;
        }
        case ToolbarCommand::ToggleSpotlightShape: {
            CaptureVectorIcons::renderIcon(
                m_renderTarget.Get(), m_d2dFactory.Get(),
                button.boolParam ? CaptureIconId::ToolEllipse : CaptureIconId::ToolRectangle,
                rect, brush, scale);
            return;
        }
        case ToolbarCommand::CycleSpotlightDim: {
            drawStepperIconAndLabel(CaptureIconId::ToolSpotlight);
            return;
        }
        case ToolbarCommand::ToggleNumberFormatDropdown: {
            if (!button.label.empty() && m_infoTextFormat) {
                const float textLeft = rect.left + 4.0f * scale;
                const float rightInset = button.hasDropdown ? 10.0f * scale : 3.0f * scale;
                const D2D1_RECT_F textRect = D2D1::RectF(
                    textLeft, rect.top, (std::max)(textLeft, rect.right - rightInset), rect.bottom);
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(), textRect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            return;
        }
        case ToolbarCommand::ToggleTextOutline: {
            // 文字描边开关图标
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                CaptureIconId::PropTextOutline, rect, brush, scale);
            return;
        }
        case ToolbarCommand::ToggleLineStyleDropdown: {
            auto iconId = (button.lineStyleParam == LineStyle::Dashed) ? CaptureIconId::PropDashedLine
                        : (button.lineStyleParam == LineStyle::Dotted) ? CaptureIconId::PropDottedLine
                        : (button.lineStyleParam == LineStyle::DashDot) ? CaptureIconId::PropDashDotLine
                        : CaptureIconId::PropSolidLine;
            auto lineRect = D2D1::RectF(rect.left + 3.0f * scale, rect.top, rect.right - 9.0f * scale, rect.bottom);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, lineRect, brush, scale);
            return;
        }
        case ToolbarCommand::ChooseCustomColor: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropRainbowWheel, rect, brush, scale);
            if (button.boolParam) {
                // 用户已选取自定义颜色: 在彩虹环中央绘制高亮实心微晶圆点 (button.color) 与 1px 纯白边框
                float dotR = 2.6f * scale;
                ComPtr<ID2D1SolidColorBrush> customBrush, whiteBorder;
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(button.color.r / 255.0f, button.color.g / 255.0f, button.color.b / 255.0f, 1.0f),
                    customBrush.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f),
                    whiteBorder.GetAddressOf());
                if (customBrush && whiteBorder) {
                    m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), dotR, dotR), customBrush.Get());
                    m_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), dotR, dotR), whiteBorder.Get(), 1.0f * scale);
                }
            }
            return;
        }
        case ToolbarCommand::ToggleArrowStyleDropdown: {
            auto iconId = (button.arrowStyleParam == ArrowStyle::DoubleEnded) ? CaptureIconId::ToolArrowDouble
                        : (button.arrowStyleParam == ArrowStyle::Thin) ? CaptureIconId::ToolArrowThin
                        : (button.arrowStyleParam == ArrowStyle::Tech) ? CaptureIconId::ToolArrowWing
                        : CaptureIconId::ToolArrowStandard;
            auto arrowRect = D2D1::RectF(rect.left + 3.0f * scale, rect.top, rect.right - 9.0f * scale, rect.bottom);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, arrowRect, brush, scale);
            return;
        }
        case ToolbarCommand::CycleStrokeWidth: {
            const auto stepperIcon = (button.tool == MarkupTool::Text)
                ? CaptureIconId::PropFontSize
                : CaptureIconId::PropStrokeWidth;
            drawStepperIconAndLabel(stepperIcon);
            return;
        }
        case ToolbarCommand::ToggleCornerRadius:
        case ToolbarCommand::CycleElementCornerRadius:
        case ToolbarCommand::CycleBeautyRadius: {
            drawStepperIconAndLabel(CaptureIconId::PropCornerRadius);
            return;
        }
        case ToolbarCommand::SelectMosaicType: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                button.intParam == 0 ? CaptureIconId::ToolMosaic : CaptureIconId::ToolBlur,
                rect, brush, scale);
            return;
        }
        case ToolbarCommand::ToggleBeautyShell: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolBeautyShell, rect, brush, scale);
            return;
        }
        case ToolbarCommand::SideCycleAspectRatio: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropAspectRatio, rect, brush, scale);
            return;
        }
        case ToolbarCommand::ToggleNumberShape: {
            CaptureIconId iconId;
            if (button.tool == MarkupTool::Magnifier) {
                iconId = (button.intParam == static_cast<int>(NumberBadgeShape::RoundedSquare))
                    ? CaptureIconId::PropMagnifierSquare
                    : CaptureIconId::ToolMagnifier;
            } else {
                iconId = (button.intParam == static_cast<int>(NumberBadgeShape::RoundedSquare))
                    ? CaptureIconId::PropNumberSquare
                    : CaptureIconId::ToolNumber;
            }
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, rect, brush, scale);
            return;
        }
        case ToolbarCommand::CycleMagnifierScale: {
            if (!button.label.empty() && m_infoTextFormat) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(), rect, brush);
            }
            return;
        }
        case ToolbarCommand::ToggleMagnifierCallout: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                CaptureIconId::PropMagnifierCallout, rect, brush, scale);
            return;
        }
        case ToolbarCommand::ResetNumberCounter: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionResetNumber, rect, brush, scale);
            return;
        }
        case ToolbarCommand::CycleBeautyBg: {
            D2D1_COLOR_F c1{}, c2{}, c3{};
            getBeautyColors(static_cast<BeautyBackgroundType>(
                                std::clamp(button.intParam, 0, static_cast<int>(BeautyBackgroundType::COUNT) - 1)),
                            c1, c2, c3);
            const float dotR = 3.8f * scale;
            const float dotX = rect.left + 8.5f * scale;
            const float dotY = (rect.top + rect.bottom) * 0.5f;
            ComPtr<ID2D1LinearGradientBrush> dotGrad;
            ComPtr<ID2D1GradientStopCollection> stops;
            D2D1_GRADIENT_STOP gs[2] = {{0.0f, c1}, {1.0f, c3}};
            if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(gs, 2, stops.GetAddressOf())) &&
                SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(dotX - dotR, dotY - dotR),
                        D2D1::Point2F(dotX + dotR, dotY + dotR)),
                    stops.Get(), dotGrad.GetAddressOf()))) {
                m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), dotR, dotR), dotGrad.Get());
            }
            if (!button.label.empty() && m_infoTextFormat) {
                const float rightInset = button.hasDropdown ? 9.5f * scale : 3.0f * scale;
                const float textLeft = dotX + dotR + 3.5f * scale;
                const D2D1_RECT_F textRect = D2D1::RectF(
                    textLeft, rect.top, (std::max)(textLeft, rect.right - rightInset), rect.bottom);
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(), textRect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            return;
        }
        case ToolbarCommand::CycleBeautyPadding:
        case ToolbarCommand::RecordToggleFormat:
        case ToolbarCommand::RecordCycleFps:
        case ToolbarCommand::RecordCycleQuality:
        case ToolbarCommand::RecordCycleClickEffect: {
            if (!button.label.empty() && m_infoTextFormat) {
                const float textLeft = rect.left + 4.0f * scale;
                const D2D1_RECT_F textRect = button.hasDropdown
                    ? D2D1::RectF(textLeft, rect.top, (std::max)(textLeft, rect.right - 9.5f * scale), rect.bottom)
                    : rect;
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(),
                    textRect,
                    brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            return;
        }
        case ToolbarCommand::RecordStartConfirm: {
            if (!button.label.empty() && m_infoTextFormat) {
                const D2D1_SIZE_F tsz = measureText(button.label, m_infoTextFormat.Get());
                const float dotR = 3.3f * scale;
                const float haloR = 5.6f * scale;
                const float dotSpan = haloR * 2.0f;
                const float gap = 5.5f * scale;
                const float totalW = dotSpan + gap + tsz.width;
                const float startX = rect.left + ((rect.right - rect.left) - totalW) * 0.5f;
                const D2D1_POINT_2F dotPt = D2D1::Point2F(startX + haloR, cy);

                ComPtr<ID2D1SolidColorBrush> haloBrush;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), haloBrush.GetAddressOf());
                if (haloBrush) {
                    m_renderTarget->FillEllipse(D2D1::Ellipse(dotPt, haloR, haloR), haloBrush.Get());
                }
                if (brush) {
                    m_renderTarget->FillEllipse(D2D1::Ellipse(dotPt, dotR, dotR), brush);
                }

                const float textLeft = startX + dotSpan + gap;
                const D2D1_RECT_F textRect = D2D1::RectF(
                    textLeft, rect.top, (std::max)(textLeft, rect.right - 4.0f * scale), rect.bottom);
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(), textRect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            return;
        }
        case ToolbarCommand::RecordToggleKeycast: {
            const float s = (std::min)(rect.right - rect.left, rect.bottom - rect.top) / 24.0f * 0.68f;
            const float stroke = (std::max)(1.6f, 1.8f * scale);
            auto p = [&](float x, float y) -> D2D1_POINT_2F {
                return D2D1::Point2F(cx + (x - 12.0f) * s, cy + (y - 12.0f) * s);
            };
            auto kbBox = D2D1::RectF(p(3, 7).x, p(3, 7).y, p(21, 17).x, p(21, 17).y);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(kbBox, 2.0f * s, 2.0f * s), brush, stroke);
            m_renderTarget->DrawLine(p(6, 11), p(8, 11), brush, stroke);
            m_renderTarget->DrawLine(p(11, 11), p(13, 11), brush, stroke);
            m_renderTarget->DrawLine(p(16, 11), p(18, 11), brush, stroke);
            m_renderTarget->DrawLine(p(8, 14), p(16, 14), brush, stroke);
            return;
        }
        case ToolbarCommand::RecordToggleSystemAudio: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionToggleSpeaker, rect, brush, scale);
            return;
        }
        case ToolbarCommand::RecordToggleMicrophone: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionToggleMic, rect, brush, scale);
            return;
        }
        default: break;
    }
}

void CaptureRenderer::drawGlassPanel(const D2D1_RECT_F& rect, float radius, bool seeThrough) {
    (void)seeThrough;
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), rect, radius, false);
}

void CaptureRenderer::drawFloatingToast(const CaptureState& state) {
    if (!m_renderTarget || state.loupeToastUntil == 0 || state.loupeToastMessage.empty()) {
        return;
    }
    const DWORD now = GetTickCount();
    if (now >= state.loupeToastUntil) {
        return;
    }

    const float scale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
    const bool isDark = false;
    IDWriteTextFormat* toastFmt = m_hudPrimaryFormat ? m_hudPrimaryFormat.Get() : m_infoTextFormat.Get();
    const std::wstring cleanMsg = std::wstring(
        tools3000::ui::ToastStyle::stripMachineStatusPrefix(state.loupeToastMessage));
    if (cleanMsg.empty()) {
        return;
    }

    // 1. 测量文本尺寸
    float textW = 140.0f * scale;
    float textH = 22.0f * scale;
    if (m_dwriteFactory && toastFmt) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
            cleanMsg.c_str(),
            static_cast<UINT32>(cleanMsg.size()),
            toastFmt, 1400.0f * scale, 100.0f * scale, layout.GetAddressOf())) && layout) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                textW = metrics.widthIncludingTrailingWhitespace > metrics.width
                    ? metrics.widthIncludingTrailingWhitespace : metrics.width;
                textH = metrics.height;
            }
        }
    }

    // 2. 居中大气微晶浮岛几何布局
    const float padH = 22.0f * scale;
    const float padV = 10.0f * scale;
    const float dotZone = 18.0f * scale;
    const float toastW = (std::max)(textW + dotZone + padH * 2.0f, 148.0f * scale);
    const float toastH = (std::max)(textH + padV * 2.0f, 40.0f * scale);

    const auto rtSize = m_renderTarget->GetSize();
    const float left = (rtSize.width - toastW) * 0.5f;
    float top = 34.0f * scale;

    // 智能避让顶部尺寸胶囊、展开菜单与上移后的主/副工具栏，杜绝浮层 Toast 遮挡活动控件
    const bool hasSelNow = (state.state.load() == OverlayState::Selected ||
                            state.state.load() == OverlayState::Marking);
    auto pushDownIfOverlaps = [&](const D2D1_RECT_F& obs) {
        if (obs.right <= obs.left || obs.bottom <= obs.top) return;
        const bool xHit = (left < obs.right + 8.0f * scale) && (left + toastW > obs.left - 8.0f * scale);
        const bool yHit = (top < obs.bottom + 8.0f * scale) && (top + toastH > obs.top - 8.0f * scale);
        if (xHit && yHit) {
            top = obs.bottom + 10.0f * scale;
        }
    };
    if (hasSelNow) {
        for (int pass = 0; pass < 3; ++pass) {
            if (state.isLongImageMode()) pushDownIfOverlaps(state.longCanvas.topHeaderRect);
            pushDownIfOverlaps(state.sizeHudRect);
            if (state.isSizeMenuOpen) pushDownIfOverlaps(state.sizeMenuRect);
            pushDownIfOverlaps(state.primaryToolbarRect);
            pushDownIfOverlaps(state.secondaryToolbarRect);
            if (state.dropdownMenu.type != DropdownType::None) pushDownIfOverlaps(state.dropdownMenu.menuRect);
            if (state.sliderPopup.type != SliderPopupType::None) pushDownIfOverlaps(state.sliderPopup.popupRect);
            if (state.openSubmenu != SubmenuType::None) pushDownIfOverlaps(state.openSubmenuRect);
        }
        if (top + toastH > rtSize.height * 0.68f) {
            top = (std::max)(12.0f * scale, rtSize.height - toastH - 42.0f * scale);
        }
    }
    const auto toastRect = D2D1::RectF(left, top, left + toastW, top + toastH);
    const float cornerRadius = 12.0f * scale;

    // 3. 淡出透明度计算 (最后 300ms 线性淡出)
    const DWORD remaining = state.loupeToastUntil - now;
    float alpha = 1.0f;
    if (remaining < 300) {
        alpha = static_cast<float>(remaining) / 300.0f;
    }

    // 4. 使用 Direct2D Layer 实现全局平滑透明度合成
    ComPtr<ID2D1Layer> layer;
    bool pushedLayer = false;
    if (alpha < 0.999f && m_d2dFactory) {
        if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
            m_renderTarget->PushLayer(
                D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                    alpha, nullptr, D2D1_LAYER_OPTIONS_NONE),
                layer.Get());
            pushedLayer = true;
        }
    }

    // 5. 绘制微晶玻璃浮岛底板
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), toastRect, cornerRadius, isDark);

    // 6. 左侧翡翠绿发光状态灯 + 高对比加粗微晶文字
    const float midY = (toastRect.top + toastRect.bottom) * 0.5f;
    const float contentW = dotZone + textW;
    const float startX = toastRect.left + (toastW - contentW) * 0.5f;

    ComPtr<ID2D1SolidColorBrush> dotGlowBrush, dotBrush, textBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.73f, 0.51f, 0.26f), dotGlowBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.85f, 0.60f, 1.0f), dotBrush.GetAddressOf());
    const auto textColor = isDark ? D2D1::ColorF(0.98f, 0.99f, 1.0f, 0.98f)
                                  : D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.98f);
    m_renderTarget->CreateSolidColorBrush(textColor, textBrush.GetAddressOf());

    const D2D1_POINT_2F dotCenter = D2D1::Point2F(startX + 5.0f * scale, midY);
    if (dotGlowBrush) {
        m_renderTarget->FillEllipse(D2D1::Ellipse(dotCenter, 7.5f * scale, 7.5f * scale), dotGlowBrush.Get());
    }
    if (dotBrush) {
        m_renderTarget->FillEllipse(D2D1::Ellipse(dotCenter, 4.0f * scale, 4.0f * scale), dotBrush.Get());
    }

    if (toastFmt && textBrush) {
        TextFormatScope scope(toastFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_renderTarget->DrawTextW(
            cleanMsg.c_str(),
            static_cast<UINT32>(cleanMsg.size()),
            toastFmt,
            D2D1::RectF(startX + dotZone, toastRect.top, toastRect.right - 8.0f * scale, toastRect.bottom),
            textBrush.Get()
        );
    }

    if (pushedLayer) {
        m_renderTarget->PopLayer();
    }

    if (m_hwnd && now < state.loupeToastUntil) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void CaptureRenderer::drawMarkupPreview(const D2D1_RECT_F& selectionRect, CaptureState& state) {
    if (!state.markupBaseReady || state.markup.elementCount() == 0 || !m_renderTarget) return;

    // 仅在标注内容变化时才重新合成并更新 4096px 分片 D2D 位图缓存，突破 16384px 单纹理上限且保持 60fps 零拷贝原地刷新
    if (m_markupCacheDirty || m_markupCacheTiles.empty()) {
        cv::Mat composite = state.markup.getCompositeImage(true);
        if (composite.empty()) return;

        cv::Mat bgra;
        if (composite.channels() == 3) {
            cv::cvtColor(composite, bgra, cv::COLOR_BGR2BGRA);
        } else if (composite.channels() == 4) {
            bgra = composite;
        } else {
            return;
        }

        if (!m_markupCacheTiles.empty() &&
            m_markupTilesW == bgra.cols &&
            m_markupTilesH == bgra.rows) {
            bool updatedInPlace = true;
            for (auto& tile : m_markupCacheTiles) {
                if (!tile.bitmap || tile.yOffset + tile.height > bgra.rows || tile.width != bgra.cols) {
                    updatedInPlace = false;
                    break;
                }
                cv::Mat slice = bgra.rowRange(tile.yOffset, tile.yOffset + tile.height);
                if (!slice.isContinuous()) slice = slice.clone();
                if (FAILED(tile.bitmap->CopyFromMemory(nullptr, slice.data, static_cast<UINT32>(slice.step[0])))) {
                    updatedInPlace = false;
                    break;
                }
            }
            if (updatedInPlace) {
                m_markupCacheDirty = false;
            } else {
                m_markupCacheTiles.clear();
            }
        } else {
            m_markupCacheTiles.clear();
        }

        if (m_markupCacheTiles.empty()) {
            if (!rebuildBitmapTiles(bgra, D2D1_ALPHA_MODE_PREMULTIPLIED,
                                    m_markupCacheTiles, m_markupTilesW, m_markupTilesH)) {
                return;
            }
            m_markupCacheDirty = false;
        }
    }

    if (!m_markupCacheTiles.empty() && m_markupTilesW > 0 && m_markupTilesH > 0) {
        D2D1_RECT_F destRect = calculateMarkupDestRect(
            selectionRect, static_cast<UINT32>(m_markupTilesW), static_cast<UINT32>(m_markupTilesH));
        D2D1_RECT_F srcRect = calculateMarkupSrcRect(
            static_cast<UINT32>(m_markupTilesW), static_cast<UINT32>(m_markupTilesH));

        const float scale = (state.dpiScale > 0.0f ? state.dpiScale : 1.0f);
        float effectiveRadius = state.effectiveCornerRadius();
        float radius = effectiveRadius * (state.isLongImageMode() ? 1.0f : scale);
        const auto interpMode = (state.isLongImageMode() && std::abs(state.longCanvas.zoom - 1.0f) > 0.01f)
            ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
            : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;

        if (radius > 0.5f && m_d2dFactory) {
            auto rounded = D2D1::RoundedRect(destRect, radius, radius);
            ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
            if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(rounded, clipGeo.GetAddressOf())) && clipGeo) {
                if (!m_markupClipLayer) {
                    m_renderTarget->CreateLayer(m_markupClipLayer.GetAddressOf());
                }
                if (m_markupClipLayer) {
                    m_renderTarget->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), m_markupClipLayer.Get());
                    drawTiledRegion(m_markupCacheTiles, m_markupTilesW, m_markupTilesH, destRect, srcRect, interpMode);
                    m_renderTarget->PopLayer();
                } else {
                    drawTiledRegion(m_markupCacheTiles, m_markupTilesW, m_markupTilesH, destRect, srcRect, interpMode);
                }
            } else {
                drawTiledRegion(m_markupCacheTiles, m_markupTilesW, m_markupTilesH, destRect, srcRect, interpMode);
            }
        } else {
            drawTiledRegion(m_markupCacheTiles, m_markupTilesW, m_markupTilesH, destRect, srcRect, interpMode);
        }
    }

    // Figma / CleanShot X 级交互：当处于激活态的元素为矩形或矩形聚光灯时，在 Direct2D 层叠加动态微晶圆角手柄与 [ ╭ R ] 胶囊反馈
    if (state.activeElement && state.activeElement->isActive &&
        (state.activeElement->tool == MarkupTool::Rectangle ||
         (state.activeElement->tool == MarkupTool::Spotlight && !state.activeElement->fill))) {
        const float uiScale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
        const float handleScale = state.isLongImageMode()
            ? (uiScale / (std::max)(0.15f, state.longCanvas.zoom))
            : uiScale;
        cv::Rect bbox = state.activeElement->getBoundingBox();
        float destLeft = std::round(selectionRect.left);
        float destTop  = std::round(selectionRect.top);
        D2D1_RECT_F elemRect = D2D1::RectF(
            destLeft + static_cast<float>(bbox.x),
            destTop  + static_cast<float>(bbox.y),
            destLeft + static_cast<float>(bbox.x + bbox.width),
            destTop  + static_cast<float>(bbox.y + bbox.height)
        );
        bool isDragging = (state.isManipulating && state.dragHandle == HitArea::CornerRadius);
        const POINT dragStartPt = state.isLongImageMode()
            ? state.screenToDocPoint(state.cornerDragStartPos)
            : state.cornerDragStartPos;
        const POINT cursorPt = state.isLongImageMode()
            ? state.screenToDocPoint(state.currentCursor)
            : state.currentCursor;
        drawCornerRadiusHandleVisual(
            elemRect,
            state.activeElement->cornerRadius,
            handleScale,
            isDragging,
            dragStartPt,
            cursorPt,
            40.0f,
            state.cornerDragIndex
        );
    }
}

void CaptureRenderer::drawActiveMarkupPreview(const D2D1_RECT_F& selectionRect, CaptureState& state) {
    if (!state.isMarking || !m_renderTarget) return;

    float x1 = static_cast<float>(state.markupStart.x);
    float y1 = static_cast<float>(state.markupStart.y);
    float x2 = static_cast<float>(state.markupEnd.x);
    float y2 = static_cast<float>(state.markupEnd.y);

    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        if (state.currentTool == MarkupTool::Rectangle || state.currentTool == MarkupTool::Ellipse ||
            state.currentTool == MarkupTool::Mosaic || state.currentTool == MarkupTool::Blur ||
            state.currentTool == MarkupTool::Highlight || state.currentTool == MarkupTool::Inpaint ||
            state.currentTool == MarkupTool::Spotlight || state.currentTool == MarkupTool::Watermark) {
            float w = std::abs(x2 - x1);
            float h = std::abs(y2 - y1);
            float side = std::max(w, h);
            x2 = x1 + (x2 >= x1 ? side : -side);
            y2 = y1 + (y2 >= y1 ? side : -side);
        } else if (state.currentTool == MarkupTool::Arrow || state.currentTool == MarkupTool::Line) {
            double dx = x2 - x1;
            double dy = y2 - y1;
            double dist = std::hypot(dx, dy);
            if (dist >= 1.0) {
                double angle = std::atan2(dy, dx);
                constexpr double step = 3.14159265358979323846 / 4.0;
                double snappedAngle = std::round(angle / step) * step;
                x2 = static_cast<float>(x1 + dist * std::cos(snappedAngle));
                y2 = static_cast<float>(y1 + dist * std::sin(snappedAngle));
            }
        }
    }

    auto rect = D2D1::RectF(std::min(x1, x2), std::min(y1, y2), std::max(x1, x2), std::max(y1, y2));
    const float strokeW = std::max(1.0f, static_cast<float>(state.currentStrokeWidth));
    ComPtr<ID2D1SolidColorBrush> activeColorBrush;
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(state.currentColor.r / 255.0f,
                     state.currentColor.g / 255.0f,
                     state.currentColor.b / 255.0f,
                     1.0f),
        activeColorBrush.GetAddressOf());
    ID2D1Brush* toolBrush = activeColorBrush ? activeColorBrush.Get() : m_borderBrush.Get();

    ComPtr<ID2D1StrokeStyle> customStrokeStyle;
    if (m_d2dFactory && state.currentLineStyle != LineStyle::Solid) {
        D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f,
            state.currentLineStyle == LineStyle::Dashed ? D2D1_DASH_STYLE_DASH
                : (state.currentLineStyle == LineStyle::Dotted ? D2D1_DASH_STYLE_DOT : D2D1_DASH_STYLE_DASH_DOT),
            0.0f);
        m_d2dFactory->CreateStrokeStyle(ssp, nullptr, 0, customStrokeStyle.GetAddressOf());
    }

    switch (state.currentTool) {
        case MarkupTool::Rectangle: {
            const float elemRad = std::max(0.0f, state.currentElementCornerRadius);
            if (elemRad > 0.5f) {
                auto rrect = D2D1::RoundedRect(rect, elemRad, elemRad);
                if (state.currentFillMode) {
                    m_renderTarget->FillRoundedRectangle(rrect, toolBrush);
                }
                if (!state.currentFillMode || strokeW > 1.0f) {
                    m_renderTarget->DrawRoundedRectangle(rrect, toolBrush, strokeW, customStrokeStyle.Get());
                }
            } else {
                if (state.currentFillMode) {
                    m_renderTarget->FillRectangle(rect, toolBrush);
                }
                if (!state.currentFillMode || strokeW > 1.0f) {
                    m_renderTarget->DrawRectangle(rect, toolBrush, strokeW, customStrokeStyle.Get());
                }
            }
            break;
        }

        case MarkupTool::Mosaic:
        case MarkupTool::Blur:
        case MarkupTool::Inpaint:
        case MarkupTool::Watermark: {
            ComPtr<ID2D1SolidColorBrush> boxFill;
            m_renderTarget->CreateSolidColorBrush(
                (state.currentTool == MarkupTool::Inpaint)
                    ? D2D1::ColorF(0.16f, 0.78f, 0.56f, 0.16f)
                    : D2D1::ColorF(0.20f, 0.52f, 0.96f, 0.14f),
                boxFill.GetAddressOf());
            if (boxFill) m_renderTarget->FillRectangle(rect, boxFill.Get());
            m_renderTarget->DrawRectangle(rect, m_borderBrush.Get(), 1.5f);
            break;
        }

        case MarkupTool::Spotlight: {
            const bool isEllipseSpot = state.currentSpotlightEllipse;
            const float elemRad = (!isEllipseSpot) ? std::max(0.0f, state.currentElementCornerRadius) : 0.0f;
            const float dimAlpha = std::clamp(state.currentSpotlightDimAlpha, 0.15f, 0.85f);

            bool hasExistingSpotlight = false;
            for (const auto& elem : state.markup.elements()) {
                if (elem && elem->tool == MarkupTool::Spotlight) {
                    hasExistingSpotlight = true;
                    break;
                }
            }

            if (m_d2dFactory && rect.right - rect.left >= 2.0f && rect.bottom - rect.top >= 2.0f) {
                ComPtr<ID2D1Geometry> litGeo;
                if (isEllipseSpot) {
                    ComPtr<ID2D1EllipseGeometry> ellGeo;
                    m_d2dFactory->CreateEllipseGeometry(
                        D2D1::Ellipse(D2D1::Point2F((x1 + x2) * 0.5f, (y1 + y2) * 0.5f),
                                      std::abs(x2 - x1) * 0.5f,
                                      std::abs(y2 - y1) * 0.5f),
                        ellGeo.GetAddressOf());
                    litGeo = ellGeo;
                } else if (elemRad > 0.5f) {
                    ComPtr<ID2D1RoundedRectangleGeometry> rrGeo;
                    m_d2dFactory->CreateRoundedRectangleGeometry(
                        D2D1::RoundedRect(rect, elemRad, elemRad), rrGeo.GetAddressOf());
                    litGeo = rrGeo;
                } else {
                    ComPtr<ID2D1RectangleGeometry> rGeo;
                    m_d2dFactory->CreateRectangleGeometry(rect, rGeo.GetAddressOf());
                    litGeo = rGeo;
                }

                if (!hasExistingSpotlight && litGeo) {
                    ComPtr<ID2D1RectangleGeometry> selGeo;
                    ComPtr<ID2D1PathGeometry> maskGeo;
                    if (SUCCEEDED(m_d2dFactory->CreateRectangleGeometry(selectionRect, selGeo.GetAddressOf())) && selGeo &&
                        SUCCEEDED(m_d2dFactory->CreatePathGeometry(maskGeo.GetAddressOf())) && maskGeo) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(maskGeo->Open(sink.GetAddressOf())) && sink) {
                            selGeo->CombineWithGeometry(litGeo.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
                            sink->Close();
                            ComPtr<ID2D1SolidColorBrush> dimBrush;
                            m_renderTarget->CreateSolidColorBrush(
                                D2D1::ColorF(0.0f, 0.0f, 0.0f, dimAlpha), dimBrush.GetAddressOf());
                            if (dimBrush) {
                                m_renderTarget->FillGeometry(maskGeo.Get(), dimBrush.Get());
                            }
                        }
                    }
                } else if (hasExistingSpotlight && litGeo && (m_screenBitmap || !m_screenTiles.empty())) {
                    // 画布已有聚光灯时，在当前新亮区内实时回填原图，实现多聚光灯并集亮区预览（兼容超长图瓦片流）
                    ComPtr<ID2D1Layer> litLayer;
                    if (SUCCEEDED(m_renderTarget->CreateLayer(litLayer.GetAddressOf())) && litLayer) {
                        m_renderTarget->PushLayer(
                            D2D1::LayerParameters(rect, litGeo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                            litLayer.Get());
                        if (!m_screenTiles.empty()) {
                            drawTiledRegion(
                                m_screenTiles, m_screenTilesW, m_screenTilesH,
                                rect, rect,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        } else if (m_screenBitmap) {
                            auto bmpSz = m_screenBitmap->GetPixelSize();
                            m_renderTarget->DrawBitmap(
                                m_screenBitmap.Get(),
                                D2D1::RectF(0.0f, 0.0f, static_cast<float>(bmpSz.width), static_cast<float>(bmpSz.height)),
                                1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                        }
                        m_renderTarget->PopLayer();
                    }
                }
            }

            ComPtr<ID2D1SolidColorBrush> guideBrush;
            if (!state.currentSpotlightBorder) {
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f), guideBrush.GetAddressOf());
            }
            ID2D1Brush* rimBrush = state.currentSpotlightBorder
                ? toolBrush
                : (guideBrush ? static_cast<ID2D1Brush*>(guideBrush.Get()) : static_cast<ID2D1Brush*>(m_borderBrush.Get()));
            const float rimW = state.currentSpotlightBorder ? strokeW : 1.5f;

            if (isEllipseSpot) {
                auto ell = D2D1::Ellipse(
                    D2D1::Point2F((x1 + x2) * 0.5f, (y1 + y2) * 0.5f),
                    std::abs(x2 - x1) * 0.5f,
                    std::abs(y2 - y1) * 0.5f);
                m_renderTarget->DrawEllipse(ell, rimBrush, rimW,
                    state.currentSpotlightBorder ? customStrokeStyle.Get() : nullptr);
            } else if (elemRad > 0.5f) {
                m_renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(rect, elemRad, elemRad), rimBrush, rimW,
                    state.currentSpotlightBorder ? customStrokeStyle.Get() : nullptr);
            } else {
                m_renderTarget->DrawRectangle(rect, rimBrush, rimW,
                    state.currentSpotlightBorder ? customStrokeStyle.Get() : nullptr);
            }
            break;
        }

        case MarkupTool::Line:
            m_renderTarget->DrawLine(
                D2D1::Point2F(x1, y1),
                D2D1::Point2F(x2, y2),
                toolBrush,
                strokeW,
                customStrokeStyle.Get()
            );
            break;

        case MarkupTool::Highlight: {
            ComPtr<ID2D1SolidColorBrush> highlightBrush;
            m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(state.currentColor.r / 255.0f,
                             state.currentColor.g / 255.0f,
                             state.currentColor.b / 255.0f,
                             0.35f),
                highlightBrush.GetAddressOf());
            if (highlightBrush) m_renderTarget->FillRectangle(rect, highlightBrush.Get());
            m_renderTarget->DrawRectangle(rect, toolBrush, 1.0f);
            break;
        }

        case MarkupTool::Arrow: {
            float dist = std::hypot(x2 - x1, y2 - y1);
            if (dist >= 2.0f) {
                double angle = std::atan2(y2 - y1, x2 - x1);
                double headLen = std::max(14.0, static_cast<double>(strokeW) * 3.5);
                if (headLen > dist * 0.45) headLen = dist * 0.45;
                ID2D1Brush* aBrush = toolBrush;

                auto drawHeadAt = [&](float tipX, float tipY, double a, ArrowStyle style, D2D1_POINT_2F& shaftPt) {
                    double cA = std::cos(a);
                    double sA = std::sin(a);
                    double nX = -sA;
                    double nY = cA;

                    if (style == ArrowStyle::Thin) {
                        double wingAngle = 3.14159265358979323846 / 6.0;
                        D2D1_POINT_2F tip = D2D1::Point2F(tipX, tipY);
                        D2D1_POINT_2F w1 = D2D1::Point2F(static_cast<float>(tipX - headLen * std::cos(a - wingAngle)),
                                                        static_cast<float>(tipY - headLen * std::sin(a - wingAngle)));
                        D2D1_POINT_2F w2 = D2D1::Point2F(static_cast<float>(tipX - headLen * std::cos(a + wingAngle)),
                                                        static_cast<float>(tipY - headLen * std::sin(a + wingAngle)));
                        m_renderTarget->DrawLine(tip, w1, aBrush, strokeW);
                        m_renderTarget->DrawLine(tip, w2, aBrush, strokeW);
                        shaftPt = tip;
                    } else if (style == ArrowStyle::Tech) {
                        double halfW = headLen * 0.55;
                        D2D1_POINT_2F tip = D2D1::Point2F(tipX, tipY);
                        D2D1_POINT_2F w1 = D2D1::Point2F(static_cast<float>(tipX - headLen * cA + halfW * nX),
                                                        static_cast<float>(tipY - headLen * sA + halfW * nY));
                        D2D1_POINT_2F w2 = D2D1::Point2F(static_cast<float>(tipX - headLen * cA - halfW * nX),
                                                        static_cast<float>(tipY - headLen * sA - halfW * nY));
                        D2D1_POINT_2F notch = D2D1::Point2F(static_cast<float>(tipX - headLen * 0.65 * cA),
                                                           static_cast<float>(tipY - headLen * 0.65 * sA));
                        ComPtr<ID2D1PathGeometry> wingGeo;
                        if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(wingGeo.GetAddressOf())) && wingGeo) {
                            ComPtr<ID2D1GeometrySink> sink;
                            if (SUCCEEDED(wingGeo->Open(sink.GetAddressOf())) && sink) {
                                sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
                                sink->AddLine(w1);
                                sink->AddLine(notch);
                                sink->AddLine(w2);
                                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                                sink->Close();
                                m_renderTarget->FillGeometry(wingGeo.Get(), aBrush);
                            }
                        }
                        shaftPt = notch;
                    } else {
                        double halfW = headLen * 0.45;
                        D2D1_POINT_2F tip = D2D1::Point2F(tipX, tipY);
                        D2D1_POINT_2F basePt = D2D1::Point2F(static_cast<float>(tipX - headLen * cA), static_cast<float>(tipY - headLen * sA));
                        D2D1_POINT_2F w1 = D2D1::Point2F(static_cast<float>(basePt.x + halfW * nX), static_cast<float>(basePt.y + halfW * nY));
                        D2D1_POINT_2F w2 = D2D1::Point2F(static_cast<float>(basePt.x - halfW * nX), static_cast<float>(basePt.y - halfW * nY));
                        ComPtr<ID2D1PathGeometry> headGeo;
                        if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(headGeo.GetAddressOf())) && headGeo) {
                            ComPtr<ID2D1GeometrySink> sink;
                            if (SUCCEEDED(headGeo->Open(sink.GetAddressOf())) && sink) {
                                sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
                                sink->AddLine(w1);
                                sink->AddLine(w2);
                                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                                sink->Close();
                                m_renderTarget->FillGeometry(headGeo.Get(), aBrush);
                            }
                        }
                        shaftPt = basePt;
                    }
                };

                if (state.currentArrowStyle == ArrowStyle::DoubleEnded) {
                    D2D1_POINT_2F shaftStart = D2D1::Point2F(x1, y1);
                    D2D1_POINT_2F shaftEnd = D2D1::Point2F(x2, y2);
                    drawHeadAt(x2, y2, angle, ArrowStyle::Standard, shaftEnd);
                    drawHeadAt(x1, y1, angle + 3.14159265358979323846, ArrowStyle::Standard, shaftStart);
                    m_renderTarget->DrawLine(shaftStart, shaftEnd, aBrush, strokeW, customStrokeStyle.Get());
                } else {
                    D2D1_POINT_2F shaftEnd = D2D1::Point2F(x2, y2);
                    drawHeadAt(x2, y2, angle, state.currentArrowStyle, shaftEnd);
                    m_renderTarget->DrawLine(D2D1::Point2F(x1, y1), shaftEnd, aBrush, strokeW, customStrokeStyle.Get());
                }
            }
            break;
        }

        case MarkupTool::Ellipse: {
            auto ell = D2D1::Ellipse(
                D2D1::Point2F((x1 + x2) / 2.0f, (y1 + y2) / 2.0f),
                std::abs(x2 - x1) / 2.0f,
                std::abs(y2 - y1) / 2.0f
            );
            if (state.currentFillMode) {
                m_renderTarget->FillEllipse(ell, toolBrush);
            }
            if (!state.currentFillMode || strokeW > 1.0f) {
                m_renderTarget->DrawEllipse(ell, toolBrush, strokeW, customStrokeStyle.Get());
            }
            break;
        }

        case MarkupTool::Pen:
            if (state.penPoints.size() >= 2) {
                for (size_t i = 1; i < state.penPoints.size(); ++i) {
                    m_renderTarget->DrawLine(
                        D2D1::Point2F(selectionRect.left + static_cast<float>(state.penPoints[i - 1].x),
                                      selectionRect.top + static_cast<float>(state.penPoints[i - 1].y)),
                        D2D1::Point2F(selectionRect.left + static_cast<float>(state.penPoints[i].x),
                                      selectionRect.top + static_cast<float>(state.penPoints[i].y)),
                        toolBrush,
                        strokeW,
                        customStrokeStyle.Get()
                    );
                }
            }
            break;

        case MarkupTool::Number: {
            const float baseDpi = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
            const float sizeFactor = std::clamp(0.76f + static_cast<float>(state.currentStrokeWidth) * 0.08f, 0.80f, 3.00f);
            float scale = baseDpi * sizeFactor;
            float radius = 15.0f * scale;
            float dist = std::hypot(x2 - x1, y2 - y1);

            const float luminance = (0.299f * state.currentColor.r +
                                     0.587f * state.currentColor.g +
                                     0.114f * state.currentColor.b) / 255.0f;
            const bool isBrightColor = luminance > 0.72f;
            const bool fillBadge = state.currentFillMode;

            // 绘制起点序号标号徽章画刷与文字画刷（与 MarkupEngine 落盘效果 1:1 对齐）
            ComPtr<ID2D1SolidColorBrush> accentBrush, bgBrush, ringBrush, badgeTextBrush;
            m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(state.currentColor.r / 255.0f, state.currentColor.g / 255.0f, state.currentColor.b / 255.0f, 1.0f),
                accentBrush.GetAddressOf());
            if (fillBadge) {
                m_renderTarget->CreateSolidColorBrush(
                    isBrightColor ? D2D1::ColorF(15 / 255.0f, 23 / 255.0f, 42 / 255.0f, 0.30f)
                                  : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.86f),
                    ringBrush.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(
                    isBrightColor ? D2D1::ColorF(15 / 255.0f, 23 / 255.0f, 42 / 255.0f, 1.0f)
                                  : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                    badgeTextBrush.GetAddressOf());
            } else {
                m_renderTarget->CreateSolidColorBrush(
                    isBrightColor ? D2D1::ColorF(15 / 255.0f, 23 / 255.0f, 42 / 255.0f, 0.86f)
                                  : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f),
                    bgBrush.GetAddressOf());
                badgeTextBrush = accentBrush;
            }
            ID2D1Brush* arrowBrush = accentBrush ? static_cast<ID2D1Brush*>(accentBrush.Get()) : static_cast<ID2D1Brush*>(m_borderBrush.Get());

            const int curNum = state.markup.currentNumber(state.currentNumberFormat);
            const std::wstring numStr = formatNumberBadgeLabel(curNum, state.currentNumberFormat);
            float halfW = radius;
            if (state.currentNumberShape == NumberBadgeShape::RoundedSquare && numStr.size() > 1) {
                halfW = radius + static_cast<float>(numStr.size() - 1) * 4.8f * scale;
            }

            // 如果拖拽距离大于阈值 (10px)，实时绘制指向目标点的引出箭头
            if (dist >= 10.0f * baseDpi) {
                double angle = std::atan2(y2 - y1, x2 - x1);
                float numStrokeW = std::max(2.0f, 2.2f * scale);
                double headLen = std::max(11.0 * static_cast<double>(scale), 4.2 * numStrokeW);
                if (headLen > dist * 0.45) headLen = dist * 0.45;
                double cA = std::cos(angle), sA = std::sin(angle);
                double nX = -sA, nY = cA;
                double arrowHalfW = headLen * 0.45;
                D2D1_POINT_2F tip = D2D1::Point2F(x2, y2);
                D2D1_POINT_2F basePt = D2D1::Point2F(static_cast<float>(x2 - headLen * cA), static_cast<float>(y2 - headLen * sA));
                D2D1_POINT_2F w1 = D2D1::Point2F(static_cast<float>(basePt.x + arrowHalfW * nX), static_cast<float>(basePt.y + arrowHalfW * nY));
                D2D1_POINT_2F w2 = D2D1::Point2F(static_cast<float>(basePt.x - arrowHalfW * nX), static_cast<float>(basePt.y - arrowHalfW * nY));

                const double startReach = (state.currentNumberShape == NumberBadgeShape::RoundedSquare)
                    ? std::hypot(halfW * cA, radius * sA)
                    : static_cast<double>(radius);
                D2D1_POINT_2F lineStart = D2D1::Point2F(static_cast<float>(x1 + startReach * cA), static_cast<float>(y1 + startReach * sA));

                m_renderTarget->DrawLine(
                    lineStart,
                    basePt,
                    arrowBrush,
                    numStrokeW
                );

                ComPtr<ID2D1PathGeometry> headGeo;
                if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(headGeo.GetAddressOf())) && headGeo) {
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(headGeo->Open(sink.GetAddressOf())) && sink) {
                        sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddLine(w1);
                        sink->AddLine(w2);
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        m_renderTarget->FillGeometry(headGeo.Get(), arrowBrush);
                    }
                }
            }

            if (accentBrush) {
                if (state.currentNumberShape == NumberBadgeShape::RoundedSquare) {
                    const float boxRad = std::max(5.5f * scale, radius * 0.42f);
                    auto sqBox = D2D1::RectF(x1 - halfW, y1 - radius, x1 + halfW, y1 + radius);
                    if (fillBadge) {
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sqBox, boxRad, boxRad), accentBrush.Get());
                        if (ringBrush) {
                            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(sqBox, boxRad, boxRad), ringBrush.Get(), 1.2f * scale);
                        }
                    } else {
                        if (bgBrush) {
                            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sqBox, boxRad, boxRad), bgBrush.Get());
                        }
                        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(sqBox, boxRad, boxRad), accentBrush.Get(), 2.2f * scale);
                    }
                } else {
                    auto circle = D2D1::Ellipse(D2D1::Point2F(x1, y1), radius, radius);
                    if (fillBadge) {
                        m_renderTarget->FillEllipse(circle, accentBrush.Get());
                        if (ringBrush) {
                            auto innerCircle = D2D1::Ellipse(D2D1::Point2F(x1, y1), radius - 1.0f * scale, radius - 1.0f * scale);
                            m_renderTarget->DrawEllipse(innerCircle, ringBrush.Get(), 1.5f * scale);
                        }
                    } else {
                        if (bgBrush) {
                            m_renderTarget->FillEllipse(circle, bgBrush.Get());
                        }
                        auto ringCircle = D2D1::Ellipse(D2D1::Point2F(x1, y1), radius - 1.0f * scale, radius - 1.0f * scale);
                        m_renderTarget->DrawEllipse(ringCircle, accentBrush.Get(), 2.2f * scale);
                    }
                }
            }

            if (badgeTextBrush) {
                IDWriteTextFormat* badgeFmt = (numStr.size() >= 3)
                    ? m_numberBadgeSmallFormat.Get()
                    : (numStr.size() == 2 ? m_numberBadgeMediumFormat.Get() : m_numberBadgeLargeFormat.Get());
                if (!badgeFmt) badgeFmt = m_infoTextFormat.Get();
                if (badgeFmt) {
                    TextFormatScope badgeScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                    m_renderTarget->DrawTextW(
                        numStr.c_str(), static_cast<UINT32>(numStr.size()),
                        badgeFmt,
                        D2D1::RectF(x1 - halfW, y1 - radius - 0.5f * scale, x1 + halfW, y1 + radius - 0.5f * scale),
                        badgeTextBrush.Get());
                }
            }
            break;
        }

        case MarkupTool::Magnifier: {
            if (!hasScreenContent() || !m_d2dFactory) break;
            const float scale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
            const float sqrtScale = std::sqrt(scale);
            const float magScale = std::clamp(state.dynamicMagnifierScale, 1.5f, 6.0f);
            const float dragDist = std::hypot(x2 - x1, y2 - y1);
            const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            // 默认直接拖拽即进入零遮挡引出式详图放大；按住 Shift 拖拽则为原位同心自定义半径
            const bool isOffsetMode = !shiftHeld && (dragDist >= 10.0f * scale);

            const float srcCx = x1;
            const float srcCy = y1;
            const float r = (!isOffsetMode && shiftHeld && dragDist > 6.0f * scale)
                ? std::clamp(dragDist, 24.0f * scale, 420.0f)
                : std::max(28.0f * scale, static_cast<float>(state.dynamicMagnifierRadius) * scale);

            float dstCx = isOffsetMode ? x2 : x1;
            float dstCy = isOffsetMode ? y2 : y1;
            if (isOffsetMode) {
                const float margin = 4.0f * scale;
                const float minLx = selectionRect.left + r + margin;
                const float maxLx = selectionRect.right - r - margin;
                const float minLy = selectionRect.top + r + margin;
                const float maxLy = selectionRect.bottom - r - margin;
                dstCx = (minLx <= maxLx) ? std::clamp(dstCx, minLx, maxLx) : (selectionRect.left + selectionRect.right) * 0.5f;
                dstCy = (minLy <= maxLy) ? std::clamp(dstCy, minLy, maxLy) : (selectionRect.top + selectionRect.bottom) * 0.5f;
            }

            const float srcR = r / magScale;
            const D2D1_RECT_F srcRect = D2D1::RectF(srcCx - srcR, srcCy - srcR, srcCx + srcR, srcCy + srcR);
            const D2D1_RECT_F dstRect = D2D1::RectF(dstCx - r, dstCy - r, dstCx + r, dstCy + r);
            const bool isSquare = (state.currentMagnifierShape == NumberBadgeShape::RoundedSquare);
            const float cornerR = std::clamp(r * 0.28f, 8.0f * scale, 32.0f * scale);
            const float rimLum = (0.299f * state.currentMagnifierColor.r +
                                  0.587f * state.currentMagnifierColor.g +
                                  0.114f * state.currentMagnifierColor.b) / 255.0f;

            ComPtr<ID2D1SolidColorBrush> rimBrush, darkHaloBrush, innerRimBrush, jewelCoreBrush, glareBrush;
            m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(state.currentMagnifierColor.r / 255.0f,
                             state.currentMagnifierColor.g / 255.0f,
                             state.currentMagnifierColor.b / 255.0f, 0.98f),
                rimBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.03f, 0.05f, 0.09f, 0.46f), darkHaloBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(
                rimLum > 0.72f ? D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.35f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.62f),
                innerRimBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(
                rimLum > 0.72f ? D2D1::ColorF(0.12f, 0.16f, 0.23f, 0.94f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f),
                jewelCoreBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f), glareBrush.GetAddressOf());

            auto drawD2DJewelAnchor = [&](D2D1_POINT_2F pt, float jr) {
                if (darkHaloBrush) {
                    m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(pt.x, pt.y + 0.5f * sqrtScale),
                                                              jr + 1.1f * sqrtScale, jr + 1.1f * sqrtScale),
                                                darkHaloBrush.Get());
                }
                if (rimBrush) {
                    m_renderTarget->FillEllipse(D2D1::Ellipse(pt, jr, jr), rimBrush.Get());
                }
                if (jewelCoreBrush) {
                    const float cr = std::max(1.0f, jr * 0.44f);
                    m_renderTarget->FillEllipse(D2D1::Ellipse(pt, cr, cr), jewelCoreBrush.Get());
                }
            };

            bool hasBridge = false;
            D2D1_POINT_2F pLensAnchor{};

            // 1. 源取景三明治纤细小环 + 单轴流光渐变引线（与 MarkupEngine 落盘效果 100% 视觉同构，零面积遮挡正文）
            if (isOffsetMode && rimBrush) {
                const float smallR = std::clamp(srcR, 8.0f * scale, r * 0.67f);
                const float srCorner = std::max(4.0f * sqrtScale, smallR * 0.28f);
                const float srInner = std::max(3.0f, smallR - 1.0f * sqrtScale);
                if (isSquare) {
                    auto srr = D2D1::RoundedRect(D2D1::RectF(srcCx - smallR, srcCy - smallR, srcCx + smallR, srcCy + smallR), srCorner, srCorner);
                    auto srrIn = D2D1::RoundedRect(D2D1::RectF(srcCx - srInner, srcCy - srInner, srcCx + srInner, srcCy + srInner),
                                                   std::max(2.0f, srCorner - 1.0f * sqrtScale), std::max(2.0f, srCorner - 1.0f * sqrtScale));
                    if (darkHaloBrush) m_renderTarget->DrawRoundedRectangle(srr, darkHaloBrush.Get(), 2.5f * sqrtScale);
                    m_renderTarget->DrawRoundedRectangle(srr, rimBrush.Get(), 1.45f * sqrtScale);
                    if (innerRimBrush) m_renderTarget->DrawRoundedRectangle(srrIn, innerRimBrush.Get(), 0.8f * sqrtScale);
                } else {
                    auto selEl = D2D1::Ellipse(D2D1::Point2F(srcCx, srcCy), smallR, smallR);
                    auto selIn = D2D1::Ellipse(D2D1::Point2F(srcCx, srcCy), srInner, srInner);
                    if (darkHaloBrush) m_renderTarget->DrawEllipse(selEl, darkHaloBrush.Get(), 2.5f * sqrtScale);
                    m_renderTarget->DrawEllipse(selEl, rimBrush.Get(), 1.45f * sqrtScale);
                    if (innerRimBrush) m_renderTarget->DrawEllipse(selIn, innerRimBrush.Get(), 0.8f * sqrtScale);
                }

                const float dx = dstCx - srcCx;
                const float dy = dstCy - srcCy;
                const float dist = std::hypot(dx, dy);
                if (dist > 1.0f) {
                    const float ux = dx / dist;
                    const float uy = dy / dist;
                    const float srcEdgeD = computeLoupeBoundaryDistance(ux, uy, smallR, srCorner, isSquare);
                    const float lensEdgeD = computeLoupeBoundaryDistance(-ux, -uy, r, cornerR, isSquare);
                    if (dist > srcEdgeD + lensEdgeD + 4.0f * sqrtScale) {
                        hasBridge = true;
                        const D2D1_POINT_2F pSrc = D2D1::Point2F(srcCx + ux * srcEdgeD, srcCy + uy * srcEdgeD);
                        pLensAnchor = D2D1::Point2F(dstCx - ux * lensEdgeD, dstCy - uy * lensEdgeD);

                        if (darkHaloBrush) {
                            m_renderTarget->DrawLine(pSrc, pLensAnchor, darkHaloBrush.Get(), 2.8f * sqrtScale);
                        }

                        D2D1_GRADIENT_STOP stops[2] = {
                            {0.0f, D2D1::ColorF(state.currentMagnifierColor.r / 255.0f,
                                                state.currentMagnifierColor.g / 255.0f,
                                                state.currentMagnifierColor.b / 255.0f, 0.66f)},
                            {1.0f, D2D1::ColorF(state.currentMagnifierColor.r / 255.0f,
                                                state.currentMagnifierColor.g / 255.0f,
                                                state.currentMagnifierColor.b / 255.0f, 0.98f)}
                        };
                        ComPtr<ID2D1GradientStopCollection> stopCol;
                        ComPtr<ID2D1LinearGradientBrush> gradBr;
                        if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(stops, 2, stopCol.GetAddressOf())) && stopCol &&
                            SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(
                                D2D1::LinearGradientBrushProperties(pSrc, pLensAnchor), stopCol.Get(), gradBr.GetAddressOf())) && gradBr) {
                            m_renderTarget->DrawLine(pSrc, pLensAnchor, gradBr.Get(), 1.65f * sqrtScale);
                        } else {
                            m_renderTarget->DrawLine(pSrc, pLensAnchor, rimBrush.Get(), 1.65f * sqrtScale);
                        }
                        drawD2DJewelAnchor(pSrc, 2.5f * sqrtScale);
                    }
                }
            }

            // 2. 多层漫射悬浮投影
            struct D2DShadowStep { float dy; float expand; float alpha; };
            static constexpr D2DShadowStep kD2DShadows[] = {
                {1.2f, 1.0f, 0.20f}, {3.0f, 3.2f, 0.14f}, {5.6f, 6.4f, 0.09f},
                {8.8f, 10.5f, 0.05f}, {12.0f, 15.0f, 0.025f}
            };
            for (const auto& sh : kD2DShadows) {
                ComPtr<ID2D1SolidColorBrush> shBr;
                if (SUCCEEDED(m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.04f, 0.08f, sh.alpha), shBr.GetAddressOf())) && shBr) {
                    const float ex = sh.expand * sqrtScale;
                    const float dy = sh.dy * sqrtScale;
                    if (isSquare) {
                        auto sRect = D2D1::RectF(dstCx - r - ex, dstCy - r - ex + dy, dstCx + r + ex, dstCy + r + ex + dy);
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sRect, cornerR + ex, cornerR + ex), shBr.Get());
                    } else {
                        m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dstCx, dstCy + dy), r + ex, r + ex), shBr.Get());
                    }
                }
            }

            ComPtr<ID2D1Geometry> clipGeo;
            if (isSquare) {
                ComPtr<ID2D1RoundedRectangleGeometry> rrGeo;
                m_d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(dstRect, cornerR, cornerR), rrGeo.GetAddressOf());
                clipGeo = rrGeo;
            } else {
                ComPtr<ID2D1EllipseGeometry> elGeo;
                m_d2dFactory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(dstCx, dstCy), r, r), elGeo.GetAddressOf());
                clipGeo = elGeo;
            }

            if (clipGeo) {
                ComPtr<ID2D1Layer> layer;
                if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
                    m_renderTarget->PushLayer(D2D1::LayerParameters(
                        D2D1::InfiniteRect(), clipGeo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                        D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE), layer.Get());
                    if (!m_screenTiles.empty() && m_screenTilesW > 0 && m_screenTilesH > 0) {
                        drawTiledRegion(m_screenTiles, m_screenTilesW, m_screenTilesH,
                                        dstRect, srcRect, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, true);
                    } else if (m_screenBitmap) {
                        ComPtr<ID2D1DeviceContext> dc;
                        if (SUCCEEDED(m_renderTarget.As(&dc)) && dc) {
                            dc->DrawBitmap(m_screenBitmap.Get(), &dstRect, 1.0f,
                                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &srcRect, nullptr);
                        } else {
                            m_renderTarget->DrawBitmap(m_screenBitmap.Get(), dstRect, 1.0f,
                                                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, srcRect);
                        }
                    }
                    // 顶部微高光折射线
                    if (glareBrush && r >= 28.0f) {
                        m_renderTarget->DrawLine(
                            D2D1::Point2F(dstCx - r * 0.55f, dstCy - r + 3.5f * sqrtScale),
                            D2D1::Point2F(dstCx + r * 0.55f, dstCy - r + 3.5f * sqrtScale),
                            glareBrush.Get(), 1.4f * sqrtScale);
                    }
                    m_renderTarget->PopLayer();
                }

                const float rimW = std::clamp((1.55f + static_cast<float>(state.currentStrokeWidth) * 0.34f) * sqrtScale,
                                              2.2f, 4.8f * sqrtScale);
                const float inR = std::max(4.0f, r - rimW * 0.52f);
                if (isSquare) {
                    auto rr = D2D1::RoundedRect(dstRect, cornerR, cornerR);
                    auto rrIn = D2D1::RoundedRect(D2D1::RectF(dstCx - inR, dstCy - inR, dstCx + inR, dstCy + inR),
                                                  std::max(2.0f, cornerR - rimW * 0.5f), std::max(2.0f, cornerR - rimW * 0.5f));
                    if (darkHaloBrush) m_renderTarget->DrawRoundedRectangle(rr, darkHaloBrush.Get(), rimW + 1.8f * sqrtScale);
                    if (rimBrush) m_renderTarget->DrawRoundedRectangle(rr, rimBrush.Get(), rimW);
                    if (innerRimBrush) m_renderTarget->DrawRoundedRectangle(rrIn, innerRimBrush.Get(), 0.95f * sqrtScale);
                } else {
                    auto el = D2D1::Ellipse(D2D1::Point2F(dstCx, dstCy), r, r);
                    auto elIn = D2D1::Ellipse(D2D1::Point2F(dstCx, dstCy), inR, inR);
                    if (darkHaloBrush) m_renderTarget->DrawEllipse(el, darkHaloBrush.Get(), rimW + 1.8f * sqrtScale);
                    if (rimBrush) m_renderTarget->DrawEllipse(el, rimBrush.Get(), rimW);
                    if (innerRimBrush) m_renderTarget->DrawEllipse(elIn, innerRimBrush.Get(), 0.95f * sqrtScale);
                }

                if (hasBridge) {
                    drawD2DJewelAnchor(pLensAnchor, 2.9f * sqrtScale);
                }
            }
            break;
        }

        default:
            break;
    }
}

void CaptureRenderer::drawDynamicMagnifier(CaptureState& state) {
    if (!m_renderTarget || !hasScreenContent() || !m_d2dFactory) return;
    if (!state.isMarkupToolActive || state.currentTool != MarkupTool::Magnifier) return;
    if (state.isMarking || state.isManipulating) return;

    const float cx = static_cast<float>(state.currentCursor.x);
    const float cy = static_cast<float>(state.currentCursor.y);
    const D2D1_POINT_2F docPt = state.screenToDoc(cx, cy);

    // 仅当光标在选区内部（长图模式下还须在主视口 stageRect 内部）且未悬停在任何悬浮工具栏/菜单面板上时显示实时光学瞄准器
    if (state.isLongImageMode()) {
        const auto& st = state.longCanvas.stageRect;
        if (cx < st.left || cx > st.right || cy < st.top || cy > st.bottom) return;
    }
    const float sx1 = static_cast<float>(std::min(state.dragStart.x, state.dragEnd.x));
    const float sy1 = static_cast<float>(std::min(state.dragStart.y, state.dragEnd.y));
    const float sx2 = static_cast<float>(std::max(state.dragStart.x, state.dragEnd.x));
    const float sy2 = static_cast<float>(std::max(state.dragStart.y, state.dragEnd.y));
    if (docPt.x < sx1 || docPt.x > sx2 || docPt.y < sy1 || docPt.y > sy2) return;

    auto inRect = [&](const D2D1_RECT_F& rc) {
        return rc.right > rc.left && cx >= rc.left && cx <= rc.right && cy >= rc.top && cy <= rc.bottom;
    };
    if (inRect(state.primaryToolbarRect) || inRect(state.secondaryToolbarRect) ||
        inRect(state.selectionSideRect) || hasAnyCapturePopupOpen(state)) {
        return;
    }

    const POINT cursorForPolicy = state.isLongImageMode() ? state.screenToDocPoint(state.currentCursor) : state.currentCursor;
    if (shouldSuppressDynamicMagnifierPreview(state, cursorForPolicy)) {
        return;
    }

    const float uiScale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
    const float r = std::max(28.0f * uiScale, static_cast<float>(state.dynamicMagnifierRadius) * uiScale);
    const float magScale = std::clamp(state.dynamicMagnifierScale, 1.5f, 6.0f);
    const float srcR = r / magScale;
    const float docSrcR = state.isLongImageMode() ? (srcR / (std::max)(0.15f, state.longCanvas.zoom)) : srcR;
    const bool isSquare = (state.currentMagnifierShape == NumberBadgeShape::RoundedSquare);

    // 引出式详图模式下：光标悬停时仅绘制 1:1 真实采样范围的晶透三明治光学取景环/框，内部 100% 透明零遮挡
    if (state.magnifierCalloutMode) {
        const float sqrtScale = std::sqrt(std::max(1.0f, uiScale));
        const float ringR = std::clamp(srcR, 10.0f * uiScale, r * 0.67f);
        ComPtr<ID2D1SolidColorBrush> darkHaloBrush, rimBrush, innerRimBrush, tickBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.03f, 0.05f, 0.09f, 0.56f), darkHaloBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(state.currentMagnifierColor.r / 255.0f,
                         state.currentMagnifierColor.g / 255.0f,
                         state.currentMagnifierColor.b / 255.0f, 0.96f),
            rimBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f), innerRimBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.78f), tickBrush.GetAddressOf());

        const float inR = std::max(3.0f, ringR - 0.95f * sqrtScale);
        if (isSquare) {
            const float srCorner = std::max(4.0f * sqrtScale, ringR * 0.28f);
            auto srr = D2D1::RoundedRect(D2D1::RectF(cx - ringR, cy - ringR, cx + ringR, cy + ringR), srCorner, srCorner);
            auto srrIn = D2D1::RoundedRect(D2D1::RectF(cx - inR, cy - inR, cx + inR, cy + inR),
                                           std::max(2.0f, srCorner - 0.8f), std::max(2.0f, srCorner - 0.8f));
            if (darkHaloBrush) m_renderTarget->DrawRoundedRectangle(srr, darkHaloBrush.Get(), 2.8f * sqrtScale);
            if (rimBrush) m_renderTarget->DrawRoundedRectangle(srr, rimBrush.Get(), 1.35f * sqrtScale);
            if (innerRimBrush) m_renderTarget->DrawRoundedRectangle(srrIn, innerRimBrush.Get(), 0.7f * sqrtScale);
        } else {
            auto el = D2D1::Ellipse(D2D1::Point2F(cx, cy), ringR, ringR);
            auto elIn = D2D1::Ellipse(D2D1::Point2F(cx, cy), inR, inR);
            if (darkHaloBrush) m_renderTarget->DrawEllipse(el, darkHaloBrush.Get(), 2.8f * sqrtScale);
            if (rimBrush) m_renderTarget->DrawEllipse(el, rimBrush.Get(), 1.35f * sqrtScale);
            if (innerRimBrush) m_renderTarget->DrawEllipse(elIn, innerRimBrush.Get(), 0.7f * sqrtScale);
        }

        if (tickBrush) {
            const float arm = 3.8f * sqrtScale;
            const float gap = 1.5f * sqrtScale;
            m_renderTarget->DrawLine(D2D1::Point2F(cx - arm, cy), D2D1::Point2F(cx - gap, cy), tickBrush.Get(), 0.9f * sqrtScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx + gap, cy), D2D1::Point2F(cx + arm, cy), tickBrush.Get(), 0.9f * sqrtScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx, cy - arm), D2D1::Point2F(cx, cy - gap), tickBrush.Get(), 0.9f * sqrtScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx, cy + gap), D2D1::Point2F(cx, cy + arm), tickBrush.Get(), 0.9f * sqrtScale);
        }
        return;
    }

    const D2D1_RECT_F srcRect = D2D1::RectF(docPt.x - docSrcR, docPt.y - docSrcR, docPt.x + docSrcR, docPt.y + docSrcR);
    const D2D1_RECT_F dstRect = D2D1::RectF(cx - r, cy - r, cx + r, cy + r);
    const float cornerR = r * 0.28f;
    ComPtr<ID2D1Geometry> clipGeo;
    if (isSquare) {
        ComPtr<ID2D1RoundedRectangleGeometry> rrGeo;
        m_d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(dstRect, cornerR, cornerR), rrGeo.GetAddressOf());
        clipGeo = rrGeo;
    } else {
        ComPtr<ID2D1EllipseGeometry> elGeo;
        m_d2dFactory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), elGeo.GetAddressOf());
        clipGeo = elGeo;
    }

    if (clipGeo) {
        ComPtr<ID2D1Layer> layer;
        if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
            m_renderTarget->PushLayer(D2D1::LayerParameters(
                D2D1::InfiniteRect(), clipGeo.Get(),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(),
                1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE), layer.Get());

            if (!m_screenTiles.empty() && m_screenTilesW > 0 && m_screenTilesH > 0) {
                drawTiledRegion(m_screenTiles, m_screenTilesW, m_screenTilesH,
                                dstRect, srcRect, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, true);
            } else if (m_screenBitmap) {
                ComPtr<ID2D1DeviceContext> d2dDc;
                if (SUCCEEDED(m_renderTarget.As(&d2dDc)) && d2dDc) {
                    d2dDc->DrawBitmap(m_screenBitmap.Get(), &dstRect, 1.0f,
                                      D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &srcRect);
                } else {
                    m_renderTarget->DrawBitmap(m_screenBitmap.Get(), dstRect, 1.0f,
                                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, srcRect);
                }
            }
            m_renderTarget->PopLayer();
        }

        ComPtr<ID2D1SolidColorBrush> darkHaloBrush, rimBrush, crossBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.06f, 0.10f, 0.38f), darkHaloBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(state.currentMagnifierColor.r / 255.0f,
                         state.currentMagnifierColor.g / 255.0f,
                         state.currentMagnifierColor.b / 255.0f, 0.94f),
            rimBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f), crossBrush.GetAddressOf());

        const float rimW = std::max(2.2f * uiScale, static_cast<float>(state.currentStrokeWidth) * uiScale);
        if (isSquare) {
            auto rr = D2D1::RoundedRect(dstRect, cornerR, cornerR);
            if (darkHaloBrush) m_renderTarget->DrawRoundedRectangle(rr, darkHaloBrush.Get(), rimW + 2.2f * uiScale);
            if (rimBrush) m_renderTarget->DrawRoundedRectangle(rr, rimBrush.Get(), rimW);
        } else {
            auto el = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);
            if (darkHaloBrush) m_renderTarget->DrawEllipse(el, darkHaloBrush.Get(), rimW + 2.2f * uiScale);
            if (rimBrush) m_renderTarget->DrawEllipse(el, rimBrush.Get(), rimW);
        }

        // 极细微光中心锚点准星，不遮挡被放大主体
        if (crossBrush) {
            const float arm = 4.5f * uiScale;
            const float gap = 1.8f * uiScale;
            m_renderTarget->DrawLine(D2D1::Point2F(cx - arm, cy), D2D1::Point2F(cx - gap, cy), crossBrush.Get(), 1.0f * uiScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx + gap, cy), D2D1::Point2F(cx + arm, cy), crossBrush.Get(), 1.0f * uiScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx, cy - arm), D2D1::Point2F(cx, cy - gap), crossBrush.Get(), 1.0f * uiScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx, cy + gap), D2D1::Point2F(cx, cy + arm), crossBrush.Get(), 1.0f * uiScale);
        }
    }
}

D2D1_RECT_F CaptureRenderer::computeSelectionLoupeRect(
    float cx, float cy, D2D1_SIZE_F surfaceSize, float scale) {
    return HudAvoidanceEngine::computeSelectionLoupeRect(cx, cy, surfaceSize, scale);
}

void CaptureRenderer::drawSelectionLoupe(float cx, float cy, CaptureState& state) {
    if (!m_renderTarget || !m_screenBitmap) return;

    const bool zh = isCaptureUiChinese();
    int px = static_cast<int>(cx);
    int py = static_cast<int>(cy);
    int r = 0, g = 0, b = 0;
    bool hasColor = sampleScreenColor(px, py, r, g, b, state);

    float scale = (state.dpiScale > 0) ? state.dpiScale : 1.0f;
    constexpr float zoom = 8.0f;

    // 严格保持单一事实源外框尺寸 216x186 * scale，确保 HudAvoidanceEngine 与测试 100% 精确对齐
    int gridCountX = 27;
    int gridCountY = 13;
    float loupeBoxW = gridCountX * zoom * scale; // 216px * scale
    float loupeBoxH = gridCountY * zoom * scale; // 104px * scale
    float panelH = 82.0f * scale;                // 82px * scale
    const float totalH = loupeBoxH + panelH;
    const float outerRadius = 12.0f * scale;

    auto size = m_renderTarget->GetSize();
    D2D1_RECT_F fullLoupeRect = computeSelectionLoupeRect(cx, cy, size, scale);
    state.loupeHudRect = fullLoupeRect;
    float lx = fullLoupeRect.left;
    float ly = fullLoupeRect.top;

    // 0. 轻盈向下自然微投影（仅在底部提供柔和悬浮层次，杜绝四周浓黑阴影包裹）
    ComPtr<ID2D1SolidColorBrush> shadowOuter, shadowInner;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f), shadowOuter.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f), shadowInner.GetAddressOf());
    if (shadowOuter) {
        D2D1_RECT_F s1 = D2D1::RectF(fullLoupeRect.left - 1.2f * scale, fullLoupeRect.top + 3.0f * scale,
                                     fullLoupeRect.right + 1.2f * scale, fullLoupeRect.bottom + 6.0f * scale);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(s1, outerRadius + 1.2f * scale, outerRadius + 1.2f * scale), shadowOuter.Get());
    }
    if (shadowInner) {
        D2D1_RECT_F s2 = D2D1::RectF(fullLoupeRect.left, fullLoupeRect.top + 1.5f * scale,
                                     fullLoupeRect.right, fullLoupeRect.bottom + 3.0f * scale);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(s2, outerRadius, outerRadius), shadowInner.Get());
    }

    // 启用圆角几何裁剪层，使上半区像素网格与下半区信息舱融合为一体式圆角微晶光学舱
    ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
    ComPtr<ID2D1Layer> clipLayer;
    bool pushedClip = false;
    if (m_d2dFactory &&
        SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(fullLoupeRect, outerRadius, outerRadius), clipGeo.GetAddressOf())) &&
        clipGeo &&
        SUCCEEDED(m_renderTarget->CreateLayer(clipLayer.GetAddressOf())) && clipLayer) {
        m_renderTarget->PushLayer(
            D2D1::LayerParameters(fullLoupeRect, clipGeo.Get(),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
            clipLayer.Get());
        pushedClip = true;
    }

    D2D1_RECT_F dst = D2D1::RectF(lx, ly, lx + loupeBoxW, ly + loupeBoxH);
    float srcHalfX = gridCountX / 2.0f;
    float srcHalfY = gridCountY / 2.0f;
    D2D1_RECT_F src = D2D1::RectF(cx - srcHalfX, cy - srcHalfY, cx + srcHalfX, cy + srcHalfY);

    m_renderTarget->DrawBitmap(m_screenBitmap.Get(), dst, 1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, src);

    // 1. 细腻半透明像素网格线
    ComPtr<ID2D1SolidColorBrush> gridBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.76f, 0.80f, 0.86f, 0.24f), gridBrush.GetAddressOf());
    if (gridBrush) {
        for (int i = 0; i <= gridCountY; ++i) {
            float lineOffset = i * zoom * scale;
            m_renderTarget->DrawLine(D2D1::Point2F(lx, ly + lineOffset), D2D1::Point2F(lx + loupeBoxW, ly + lineOffset), gridBrush.Get(), 0.75f);
        }
        for (int i = 0; i <= gridCountX; ++i) {
            float lineOffset = i * zoom * scale;
            m_renderTarget->DrawLine(D2D1::Point2F(lx + lineOffset, ly), D2D1::Point2F(lx + lineOffset, ly + loupeBoxH), gridBrush.Get(), 0.75f);
        }
    }

    // 2. 中心十字柔雾紫晶准星带
    ComPtr<ID2D1SolidColorBrush> loupeCrosshair;
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupeCrosshairBand, loupeCrosshair.GetAddressOf());

    float pixelSz = zoom * scale;
    float mcx = lx + (gridCountX / 2) * pixelSz;
    float mcy = ly + (gridCountY / 2) * pixelSz;

    if (loupeCrosshair) {
        m_renderTarget->FillRectangle(D2D1::RectF(mcx, ly, mcx + pixelSz, mcy), loupeCrosshair.Get());
        m_renderTarget->FillRectangle(D2D1::RectF(mcx, mcy + pixelSz, mcx + pixelSz, ly + loupeBoxH), loupeCrosshair.Get());
        m_renderTarget->FillRectangle(D2D1::RectF(lx, mcy, mcx, mcy + pixelSz), loupeCrosshair.Get());
        m_renderTarget->FillRectangle(D2D1::RectF(mcx + pixelSz, mcy, lx + loupeBoxW, mcy + pixelSz), loupeCrosshair.Get());
    }

    // 3. 基础画刷定义（高通透曜石纯黑信息舱 + 100% 纯白高对比度文字与实心键帽）
    ComPtr<ID2D1SolidColorBrush> whiteBrush, blackBrush, whiteTextBrush, secondaryTextBrush, tagAccentBrush, loupePanelBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.94f), blackBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupePrimaryText, whiteTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupeSecondaryText, secondaryTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupePrimaryText, tagAccentBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupeInfoBg, loupePanelBrush.GetAddressOf());

    // 4. 中心单像素目标双层高反差锁定框
    if (whiteBrush && blackBrush) {
        D2D1_RECT_F centerCell = D2D1::RectF(mcx, mcy, mcx + pixelSz, mcy + pixelSz);
        m_renderTarget->DrawRectangle(centerCell, blackBrush.Get(), 2.2f);
        m_renderTarget->DrawRectangle(centerCell, whiteBrush.Get(), 1.25f);
    }

    // 5. 下半区高对比曜石纯黑信息舱（与左下角快捷键 HUD 家族统一，明亮清脆）
    D2D1_RECT_F panelRect = D2D1::RectF(lx, ly + loupeBoxH, lx + loupeBoxW, ly + totalH);
    if (loupePanelBrush) {
        m_renderTarget->FillRectangle(panelRect, loupePanelBrush.Get());
    }

    // 上下半区交界处：微晶亮白发丝分隔线
    ComPtr<ID2D1SolidColorBrush> seamHairline;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f), seamHairline.GetAddressOf());
    if (seamHairline) {
        m_renderTarget->DrawLine(
            D2D1::Point2F(dst.left, dst.bottom),
            D2D1::Point2F(dst.right, dst.bottom),
            seamHairline.Get(), 1.0f * scale);
    }

    // 6. 三行大气仪表排版（色晶+色值+格式徽章 / 坐标 / 纯白实心键帽引导栏）
    IDWriteTextFormat* primaryFmt = m_hudPrimaryFormat ? m_hudPrimaryFormat.Get() : m_infoTextFormat.Get();
    IDWriteTextFormat* badgeFmt = m_hudBadgeFormat ? m_hudBadgeFormat.Get() : m_infoTextFormat.Get();
    if (hasColor && primaryFmt && badgeFmt && m_dwriteFactory) {
        auto formats = getAllColorFormats(r, g, b);
        size_t curIdx = static_cast<size_t>(state.colorFormat) % formats.size();
        const auto& curEntry = formats[curIdx];

        const float padX = 10.0f * scale;
        const float innerLeft = lx + padX;
        const float innerRight = lx + loupeBoxW - padX;
        const float innerW = innerRight - innerLeft;

        // 第 1 行：左侧圆角采样色晶 + 中间加粗纯白色值 + 右侧格式微晶徽章
        const float row1Top = panelRect.top + 7.0f * scale;
        const float row1H = 23.0f * scale;
        const float swatchSz = 16.0f * scale;
        const float swatchTop = row1Top + (row1H - swatchSz) * 0.5f;
        const D2D1_RECT_F swatchRect = D2D1::RectF(innerLeft, swatchTop, innerLeft + swatchSz, swatchTop + swatchSz);

        ComPtr<ID2D1SolidColorBrush> colorBox, swatchBorder;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f), colorBox.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f), swatchBorder.GetAddressOf());
        if (colorBox) {
            auto swatchRounded = D2D1::RoundedRect(swatchRect, 4.5f * scale, 4.5f * scale);
            m_renderTarget->FillRoundedRectangle(swatchRounded, colorBox.Get());
            if (swatchBorder) m_renderTarget->DrawRoundedRectangle(swatchRounded, swatchBorder.Get(), 1.0f * scale);
        }

        // 右侧格式标签小胶囊 (HEX / RGB / HSL...)
        const std::wstring tagStr = curEntry.tag;
        const float tagW = (tagStr.size() >= 4 ? 42.0f : 34.0f) * scale;
        const float tagH = 18.0f * scale;
        const float tagTop = row1Top + (row1H - tagH) * 0.5f;
        const D2D1_RECT_F tagRect = D2D1::RectF(innerRight - tagW, tagTop, innerRight, tagTop + tagH);
        ComPtr<ID2D1SolidColorBrush> tagBg, tagBorder;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f), tagBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), tagBorder.GetAddressOf());
        if (tagBg) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(tagRect, 4.5f * scale, 4.5f * scale), tagBg.Get());
        if (tagBorder) m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(tagRect, 4.5f * scale, 4.5f * scale), tagBorder.Get(), 0.9f * scale);
        if (tagAccentBrush) {
            TextFormatScope scope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_renderTarget->DrawTextW(tagStr.c_str(), static_cast<UINT32>(tagStr.size()), badgeFmt, tagRect, tagAccentBrush.Get());
        }

        // 色值文本（长格式如 CMYK / RGBA 自动平滑使用紧凑字号，确保单行零折行零截断）
        const std::wstring& valStr = curEntry.value;
        IDWriteTextFormat* valFmt = (valStr.size() > 14) ? badgeFmt : primaryFmt;
        if (whiteTextBrush) {
            const float valLeft = swatchRect.right + 7.0f * scale;
            const D2D1_RECT_F valRect = D2D1::RectF(
                valLeft, row1Top, (std::max)(valLeft, tagRect.left - 4.0f * scale), row1Top + row1H);
            TextFormatScope scope(valFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawTextW(
                valStr.c_str(), static_cast<UINT32>(valStr.size()), valFmt,
                valRect, whiteTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // 第 2 行：精准像素坐标
        const float row2Top = row1Top + row1H + 2.0f * scale;
        const float row2H = 19.0f * scale;
        const std::wstring coordText = std::format(L"({}, {})", px, py);
        if (secondaryTextBrush) {
            TextFormatScope scope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawTextW(
                coordText.c_str(), static_cast<UINT32>(coordText.size()), badgeFmt,
                D2D1::RectF(innerLeft, row2Top, innerLeft + innerW, row2Top + row2H),
                secondaryTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // 第 3 行：PixPin 同款 100% 纯白实心圆角小键帽 + 纯白清晰说明 / 复制成功翡翠绿胶囊
        const bool toast = (state.loupeToastUntil != 0 && GetTickCount() < state.loupeToastUntil);
        const std::wstring fallbackOk = zh ? L"已复制到剪贴板" : L"Copied to clipboard";
        const std::wstring cleanedToast = std::wstring(
            tools3000::ui::ToastStyle::stripMachineStatusPrefix(state.loupeToastMessage));

        const float row3Top = row2Top + row2H + 3.0f * scale;
        const float row3H = 22.0f * scale;
        const D2D1_RECT_F footerRect = D2D1::RectF(lx + 7.0f * scale, row3Top, lx + loupeBoxW - 7.0f * scale, row3Top + row3H);
        const auto footerRounded = D2D1::RoundedRect(footerRect, 5.0f * scale, 5.0f * scale);

        if (toast) {
            const std::wstring footerText = cleanedToast.empty() ? fallbackOk : cleanedToast;
            ComPtr<ID2D1SolidColorBrush> footerBg, footerBorder, footerTextBrush;
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kMintToastBg, footerBg.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kMintToastBorder, footerBorder.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kMintToastText, footerTextBrush.GetAddressOf());
            if (footerBg) m_renderTarget->FillRoundedRectangle(footerRounded, footerBg.Get());
            if (footerBorder) m_renderTarget->DrawRoundedRectangle(footerRounded, footerBorder.Get(), 0.9f * scale);
            if (footerTextBrush) {
                const D2D1_RECT_F footerTextRect = D2D1::RectF(
                    footerRect.left + 4.0f * scale, footerRect.top,
                    (std::max)(footerRect.left + 4.0f * scale, footerRect.right - 4.0f * scale), footerRect.bottom);
                TextFormatScope scope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawTextW(
                    footerText.c_str(), static_cast<UINT32>(footerText.size()), badgeFmt,
                    footerTextRect, footerTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        } else {
            ComPtr<ID2D1SolidColorBrush> keycapBgBrush, keycapTextBrush;
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupeKeycapBg, keycapBgBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupeKeycapText, keycapTextBrush.GetAddressOf());

            const std::wstring k1 = L"Shift";
            const std::wstring d1 = zh ? L"切换格式" : L"Fmt";
            const std::wstring k2 = L"C";
            const std::wstring d2 = zh ? L"复制色值" : L"Copy";

            const float k1W = 34.0f * scale;
            const float k2W = 16.0f * scale;
            const float capH = 16.5f * scale;
            const float capY = row3Top + (row3H - capH) * 0.5f;
            const float d1W = measureText(d1, badgeFmt, 200.0f * scale, 32.0f * scale).width;
            const float d2W = measureText(d2, badgeFmt, 200.0f * scale, 32.0f * scale).width;
            const float gapIn = 4.5f * scale;
            const float gapGroup = 10.0f * scale;
            const float totalRowW = k1W + gapIn + d1W + gapGroup + k2W + gapIn + d2W;
            float curX = lx + (loupeBoxW - totalRowW) * 0.5f;

            auto drawKeycapPair = [&](const std::wstring& kText, float kw, const std::wstring& desc, float dw) {
                const D2D1_RECT_F capRect = D2D1::RectF(curX, capY, curX + kw, capY + capH);
                if (keycapBgBrush) {
                    m_renderTarget->FillRoundedRectangle(
                        D2D1::RoundedRect(capRect, 3.8f * scale, 3.8f * scale), keycapBgBrush.Get());
                }
                if (keycapTextBrush) {
                    TextFormatScope cScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                    m_renderTarget->DrawTextW(kText.c_str(), static_cast<UINT32>(kText.size()), badgeFmt, capRect, keycapTextBrush.Get());
                }
                curX += kw + gapIn;
                if (whiteTextBrush) {
                    const D2D1_RECT_F descRect = D2D1::RectF(curX, row3Top, curX + dw + 2.0f * scale, row3Top + row3H);
                    TextFormatScope dScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                    m_renderTarget->DrawTextW(desc.c_str(), static_cast<UINT32>(desc.size()), badgeFmt, descRect, whiteTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                curX += dw + gapGroup;
            };

            drawKeycapPair(k1, k1W, d1, d1W);
            drawKeycapPair(k2, k2W, d2, d2W);
        }
    }

    if (pushedClip) {
        m_renderTarget->PopLayer();
    }

    // 7. 全天候明暗背景自适应双层微晶纯白外框（亮色背景显银灰分离丝不融底，暗色背景呈现清脆透亮纯白边框）
    drawAllWeatherWhiteRim(
        m_renderTarget.Get(), fullLoupeRect, outerRadius, scale, CaptureHudRimTokens::forLoupe());
}

void CaptureRenderer::drawSmartAlignmentGuides(const D2D1_RECT_F& selRect, CaptureState& state) {
    if (!m_renderTarget) return;
    auto size = m_renderTarget->GetSize();
    float scale = (state.dpiScale > 0) ? state.dpiScale : 1.0f;

    ComPtr<ID2D1SolidColorBrush> guideBrush;
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSmartAlignmentGuide, guideBrush.GetAddressOf());
    if (!guideBrush) return;

    float cx = (selRect.left + selRect.right) * 0.5f;
    float cy = (selRect.top + selRect.bottom) * 0.5f;
    float midScreenX = size.width * 0.5f;
    float midScreenY = size.height * 0.5f;

    // 1. 水平中线对齐 (50% Screen Y)
    if (std::abs(cy - midScreenY) < 4.0f * scale) {
        float seg = 6.0f * scale, sp = 4.0f * scale;
        for (float x = 0; x < size.width; x += seg + sp) {
            m_renderTarget->DrawLine(D2D1::Point2F(x, midScreenY), D2D1::Point2F(std::min(size.width, x + seg), midScreenY), guideBrush.Get(), 1.0f * scale);
        }
    }

    // 2. 垂直中线对齐 (50% Screen X)
    if (std::abs(cx - midScreenX) < 4.0f * scale) {
        float seg = 6.0f * scale, sp = 4.0f * scale;
        for (float y = 0; y < size.height; y += seg + sp) {
            m_renderTarget->DrawLine(D2D1::Point2F(midScreenX, y), D2D1::Point2F(midScreenX, std::min(size.height, y + seg)), guideBrush.Get(), 1.0f * scale);
        }
    }
}

void CaptureRenderer::drawQrChip(const D2D1_RECT_F& selRect, CaptureState& state) {
    if (state.detectedQrText.empty() || !m_renderTarget || !m_dwriteFactory) {
        state.qrChipRect = D2D1::RectF(0, 0, 0, 0);
        return;
    }

    float scale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
    float chipH = 26.0f * scale;
    float pad = 8.0f * scale;
    const bool zh = isCaptureUiChinese();

    std::wstring displayMsg = (zh ? L"二维码: " : L"QR: ") + tools3000::core::WinUtils::utf8ToWstring(state.detectedQrText);
    if (displayMsg.size() > 28) {
        displayMsg = displayMsg.substr(0, 26) + L"...";
    }

    // 测量文字宽度（maxWidth 随 DPI scale 放大并强制 NO_WRAP，防止高分屏测宽折行偏窄）
    float textW = 120.0f * scale;
    if (m_infoTextFormat) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                displayMsg.c_str(), static_cast<UINT32>(displayMsg.size()),
                m_infoTextFormat.Get(), 2000.0f * scale, chipH * 2.0f, layout.GetAddressOf())) && layout) {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                textW = metrics.widthIncludingTrailingWhitespace;
            }
        }
    }

    auto surfaceSize = m_renderTarget->GetSize();
    const float margin = 6.0f * scale;
    float chipW = (std::min)(24.0f * scale + textW + pad * 2.0f,
                             (std::max)(80.0f * scale, surfaceSize.width - 2.0f * margin));
    const float cxRight = std::clamp(selRect.right - chipW, margin, (std::max)(margin, surfaceSize.width - chipW - margin));
    const float cxLeft = std::clamp(selRect.left, margin, (std::max)(margin, surfaceSize.width - chipW - margin));

    std::vector<D2D1_RECT_F> candidates = {
        D2D1::RectF(cxRight, selRect.top - chipH - margin, cxRight + chipW, selRect.top - margin),
        D2D1::RectF(cxRight, selRect.top + margin, cxRight + chipW, selRect.top + margin + chipH),
        D2D1::RectF(cxLeft, selRect.bottom + margin, cxLeft + chipW, selRect.bottom + margin + chipH),
        D2D1::RectF(cxLeft, selRect.top - chipH - margin, cxLeft + chipW, selRect.top - margin),
        D2D1::RectF(cxRight, selRect.bottom - chipH - margin, cxRight + chipW, selRect.bottom - margin),
    };

    const bool isInteracting = state.dragging || state.isMarking || state.isManipulating ||
                               state.isAdjustingSelection || state.isAdjustingCornerRadius;
    const float curFx = static_cast<float>(state.currentCursor.x);
    const float curFy = static_cast<float>(state.currentCursor.y);
    const float curR = 28.0f * scale;
    const D2D1_RECT_F cursorBox = D2D1::RectF(curFx - curR, curFy - curR, curFx + curR, curFy + curR);

    auto rectsOverlap = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b, float gap) -> bool {
        if (b.right <= b.left || b.bottom <= b.top) return false;
        return (a.left < b.right + gap && a.right > b.left - gap &&
                a.top < b.bottom + gap && a.bottom > b.top - gap);
    };

    D2D1_RECT_F chosen = candidates.front();
    if (chosen.top < margin) {
        chosen = candidates[1];
    }
    for (const auto& cand : candidates) {
        if (cand.top < margin || cand.bottom > surfaceSize.height - margin) continue;
        bool collides = rectsOverlap(cand, state.sizeHudRect, 4.0f * scale) ||
                        rectsOverlap(cand, state.primaryToolbarRect, 4.0f * scale) ||
                        rectsOverlap(cand, state.secondaryToolbarRect, 4.0f * scale) ||
                        rectsOverlap(cand, state.selectionSideRect, 4.0f * scale) ||
                        (isInteracting && rectsOverlap(cand, cursorBox, 4.0f * scale));
        if (!collides) {
            chosen = cand;
            break;
        }
    }
    state.qrChipRect = chosen;

    // 绘制温润雪瓷白二维码胶囊底板与细腻石板描边
    ComPtr<ID2D1SolidColorBrush> chipShadow, chipBg, chipBorder, iconBrush, textBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.09f), chipShadow.GetAddressOf());
    if (state.isQrChipHovered) {
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudHoverBg, chipBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudHoverBorder, chipBorder.GetAddressOf());
    } else {
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudBg, chipBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudBorder, chipBorder.GetAddressOf());
    }
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kChipAccentText, iconBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, textBrush.GetAddressOf());

    if (chipShadow) {
        auto shRect = D2D1::RectF(state.qrChipRect.left, state.qrChipRect.top + 1.5f * scale,
                                  state.qrChipRect.right, state.qrChipRect.bottom + 2.5f * scale);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(shRect, 6.5f * scale, 6.5f * scale), chipShadow.Get());
    }
    if (chipBg && chipBorder) {
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(state.qrChipRect, 6.5f * scale, 6.5f * scale), chipBg.Get());
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(state.qrChipRect, 6.5f * scale, 6.5f * scale), chipBorder.Get(), 1.0f * scale);
    }

    // 绘制二维码纯矢量图标
    auto iconRect = D2D1::RectF(state.qrChipRect.left + 6.0f * scale, state.qrChipRect.top + 3.0f * scale,
                                state.qrChipRect.left + 22.0f * scale, state.qrChipRect.bottom - 3.0f * scale);
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropQrCode, iconRect, iconBrush.Get(), scale);

    // 绘制文字
    if (m_infoTextFormat && textBrush) {
        TextFormatScope qrScope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        const float qrTextLeft = state.qrChipRect.left + 25.0f * scale;
        auto textRect = D2D1::RectF(qrTextLeft, state.qrChipRect.top,
                                    (std::max)(qrTextLeft, state.qrChipRect.right - 6.0f * scale), state.qrChipRect.bottom);
        m_renderTarget->DrawTextW(displayMsg.c_str(), static_cast<UINT32>(displayMsg.size()),
                                  m_infoTextFormat.Get(), textRect, textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void CaptureRenderer::drawCrosshair(float x, float y, CaptureState* pState) {
    if (!m_renderTarget) return;
    auto size = m_renderTarget->GetSize();
    ComPtr<ID2D1SolidColorBrush> crossShadow;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.22f), crossShadow.GetAddressOf());
    if (crossShadow) {
        m_renderTarget->DrawLine(D2D1::Point2F(x, 0), D2D1::Point2F(x, size.height), crossShadow.Get(), 2.0f);
        m_renderTarget->DrawLine(D2D1::Point2F(0, y), D2D1::Point2F(size.width, y), crossShadow.Get(), 2.0f);
    }
    m_renderTarget->DrawLine(D2D1::Point2F(x, 0), D2D1::Point2F(x, size.height), m_crosshairBrush.Get(), 1.0f);
    m_renderTarget->DrawLine(D2D1::Point2F(0, y), D2D1::Point2F(size.width, y), m_crosshairBrush.Get(), 1.0f);

    if (pState && m_infoTextFormat) {
        int px = static_cast<int>(x);
        int py = static_cast<int>(y);
        int r = 0, g = 0, b = 0;
        if (sampleScreenColor(px, py, r, g, b, *pState)) {
            const float scale = std::clamp(pState->dpiScale > 0.0f ? pState->dpiScale : 1.0f, 1.0f, 5.0f);
            std::wstring hudText = std::format(L"X: {}  Y: {}  #{:02X}{:02X}{:02X}", px, py, r, g, b);

            IDWriteTextFormat* crossFmt = m_hudBadgeFormat ? m_hudBadgeFormat.Get() : m_infoTextFormat.Get();
            const D2D1_SIZE_F crossTsz = measureText(hudText, crossFmt);
            float badgeW = (crossTsz.width > 0.0f ? crossTsz.width : static_cast<float>(hudText.size()) * 7.2f * scale) + 26.0f * scale;
            float badgeH = 22.0f * scale;
            const float margin = 8.0f * scale;

            auto rectsOverlap = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b, float gap) -> bool {
                if (b.right <= b.left || b.bottom <= b.top) return false;
                return (a.left < b.right + gap && a.right > b.left - gap &&
                        a.top < b.bottom + gap && a.bottom > b.top - gap);
            };

            const D2D1_POINT_2F quadOrigins[] = {
                {x + 14.0f * scale, y - badgeH - 8.0f * scale},          // 1. 右上象限 (默认)
                {x - badgeW - 14.0f * scale, y - badgeH - 8.0f * scale}, // 2. 左上象限
                {x - badgeW - 14.0f * scale, y + 14.0f * scale},         // 3. 左下象限
                {x + 14.0f * scale, y + 14.0f * scale},                  // 4. 右下象限
            };

            float badgeX = std::clamp(quadOrigins[0].x, margin, (std::max)(margin, size.width - badgeW - margin));
            float badgeY = std::clamp(quadOrigins[0].y, margin, (std::max)(margin, size.height - badgeH - margin));
            for (const auto& orig : quadOrigins) {
                if (orig.x < margin || orig.x + badgeW > size.width - margin ||
                    orig.y < margin || orig.y + badgeH > size.height - margin) {
                    continue;
                }
                D2D1_RECT_F cand = D2D1::RectF(orig.x, orig.y, orig.x + badgeW, orig.y + badgeH);
                if (!rectsOverlap(cand, pState->loupeHudRect, 6.0f * scale) &&
                    !rectsOverlap(cand, pState->sizeHudRect, 6.0f * scale)) {
                    badgeX = orig.x;
                    badgeY = orig.y;
                    break;
                }
            }

            auto badgeRect = D2D1::RectF(badgeX, badgeY, badgeX + badgeW, badgeY + badgeH);
            auto badgeRound = D2D1::RoundedRect(badgeRect, 6.0f * scale, 6.0f * scale);

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadowBrush, bgBrush, borderBrush, textBrush, colorChipBrush;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.09f), shadowBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudBg, bgBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudBorder, borderBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, textBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f), colorChipBrush.GetAddressOf());

            if (shadowBrush) {
                auto shRect = D2D1::RectF(badgeRect.left, badgeRect.top + 1.5f * scale, badgeRect.right, badgeRect.bottom + 2.5f * scale);
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(shRect, 6.0f * scale, 6.0f * scale), shadowBrush.Get());
            }
            if (bgBrush) m_renderTarget->FillRoundedRectangle(badgeRound, bgBrush.Get());
            if (borderBrush) m_renderTarget->DrawRoundedRectangle(badgeRound, borderBrush.Get(), 1.0f * scale);

            float chipSz = 9.0f * scale;
            float chipX = badgeX + 6.0f * scale;
            float chipY = badgeY + (badgeH - chipSz) * 0.5f;
            auto chipRect = D2D1::RectF(chipX, chipY, chipX + chipSz, chipY + chipSz);
            if (colorChipBrush) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(chipRect, 2.0f * scale, 2.0f * scale), colorChipBrush.Get());
            if (borderBrush) m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(chipRect, 2.0f * scale, 2.0f * scale), borderBrush.Get(), 0.8f * scale);

            TextFormatScope crossScope(crossFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawTextW(hudText.c_str(), static_cast<UINT32>(hudText.size()), crossFmt,
                D2D1::RectF(chipX + chipSz + 5.0f * scale, badgeY, badgeX + badgeW - 4.0f * scale, badgeY + badgeH),
                textBrush.Get());
        }
    }
}

void CaptureRenderer::updateShortcutHintAvoidance(const CaptureState& state) {
    if (!ShortcutHintOverlay::instance().isVisible() || !m_renderTarget) return;

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    RECT winRc{};
    if (m_hwnd && IsWindow(m_hwnd) && GetWindowRect(m_hwnd, &winRc)) {
        vx = winRc.left;
        vy = winRc.top;
    }
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const auto surfaceSize = m_renderTarget->GetSize();

    auto toScreenRect = [&](const D2D1_RECT_F& r) -> RECT {
        return RECT{
            static_cast<LONG>(std::lround(r.left)) + vx,
            static_cast<LONG>(std::lround(r.top)) + vy,
            static_cast<LONG>(std::lround(r.right)) + vx,
            static_cast<LONG>(std::lround(r.bottom)) + vy
        };
    };
    auto pushIfValid = [&](std::vector<RECT>& vec, const D2D1_RECT_F& r) {
        if (r.right > r.left && r.bottom > r.top) {
            vec.push_back(toScreenRect(r));
        }
    };

    std::vector<RECT> avoidRects;
    avoidRects.reserve(12);

    // 1. 鼠标光标高优先级安全避让区（任何阶段只要鼠标靠近左下角快捷键卡片，提前 72px*scale 主动预判避让！）
    if (state.currentCursor.x >= 0 && state.currentCursor.y >= 0) {
        const LONG curScreenX = state.currentCursor.x + vx;
        const LONG curScreenY = state.currentCursor.y + vy;
        const LONG cursorPad = static_cast<LONG>(std::lround(72.0f * scale));
        avoidRects.push_back(RECT{
            curScreenX - cursorPad,
            curScreenY - cursorPad,
            curScreenX + cursorPad,
            curScreenY + cursorPad
        });
    }

    // 2. 像素取色放大镜（Selection Loupe）与动态放大镜（Dynamic Magnifier）
    pushIfValid(avoidRects, state.loupeHudRect);
    const auto overlayState = state.state.load();
    const bool hasLockedSelection =
        ((int)overlayState == (int)OverlayState::Selected ||
         (int)overlayState == (int)OverlayState::Marking);
    if (hasLockedSelection && state.currentTool == MarkupTool::Magnifier &&
        !state.isMarking && !state.isManipulating) {
        const float magR = (static_cast<float>(state.dynamicMagnifierRadius) + 14.0f) * scale;
        const float curFx = static_cast<float>(state.currentCursor.x);
        const float curFy = static_cast<float>(state.currentCursor.y);
        pushIfValid(avoidRects, D2D1::RectF(curFx - magR, curFy - magR, curFx + magR, curFy + magR));
    }

    // 3. 尺寸胶囊、尺寸菜单、层级提示胶囊与二维码胶囊
    pushIfValid(avoidRects, state.sizeHudRect);
    if (state.isSizeMenuOpen) {
        pushIfValid(avoidRects, state.sizeMenuRect);
    }
    pushIfValid(avoidRects, state.hierarchyBadgeRect);
    if (!state.detectedQrText.empty()) {
        pushIfValid(avoidRects, state.qrChipRect);
    }

    // 4. 主工具栏、二级属性栏、侧边悬浮栏及展开的二级弹窗/滑块/下拉菜单
    if (hasLockedSelection) {
        pushIfValid(avoidRects, state.primaryToolbarRect);
        if (!state.secondaryToolbarButtons.empty()) {
            pushIfValid(avoidRects, state.secondaryToolbarRect);
        }
        if (!state.selectionSideButtons.empty()) {
            pushIfValid(avoidRects, state.selectionSideRect);
        }
        if (state.openSubmenu != SubmenuType::None) {
            pushIfValid(avoidRects, state.openSubmenuRect);
        }
        if (state.sliderPopup.type != SliderPopupType::None) {
            pushIfValid(avoidRects, state.sliderPopup.popupRect);
        }
        if (state.dropdownMenu.type != DropdownType::None && !state.dropdownMenu.items.empty()) {
            D2D1_RECT_F dmRect = state.dropdownMenu.items.front().rect;
            for (const auto& item : state.dropdownMenu.items) {
                dmRect.left = (std::min)(dmRect.left, item.rect.left);
                dmRect.top = (std::min)(dmRect.top, item.rect.top);
                dmRect.right = (std::max)(dmRect.right, item.rect.right);
                dmRect.bottom = (std::max)(dmRect.bottom, item.rect.bottom);
            }
            pushIfValid(avoidRects, dmRect);
        }
    }

    // 5. 当前正在拖拽或已锁定的选区矩形（长图模式下仅避让顶栏与右侧 Minimap，使左下角快捷键提示安稳驻留）
    if (state.isLongImageMode()) {
        pushIfValid(avoidRects, state.longCanvas.topHeaderRect);
        pushIfValid(avoidRects, state.longCanvas.minimapPanelRect);
    } else if (hasLockedSelection || state.dragging) {
        float x1 = static_cast<float>(std::min(state.dragStart.x, state.dragEnd.x));
        float y1 = static_cast<float>(std::min(state.dragStart.y, state.dragEnd.y));
        float x2 = static_cast<float>(std::max(state.dragStart.x, state.dragEnd.x));
        float y2 = static_cast<float>(std::max(state.dragStart.y, state.dragEnd.y));
        pushIfValid(avoidRects, D2D1::RectF(x1, y1, x2, y2));
    }

    ShortcutHintOverlay::instance().updateAvoidance(avoidRects);
}

bool CaptureRenderer::sampleScreenColor(int x, int y, int& r, int& g, int& b, CaptureState& state) const {
    if (state.frozenScreen.empty()) return false;
    if (x < 0 || y < 0 || x >= state.frozenScreen.cols || y >= state.frozenScreen.rows) return false;
    if (state.frozenScreen.channels() == 4) {
        const cv::Vec4b& px = state.frozenScreen.at<cv::Vec4b>(y, x);
        b = px[0]; g = px[1]; r = px[2];
    } else if (state.frozenScreen.channels() == 3) {
        const cv::Vec3b& px = state.frozenScreen.at<cv::Vec3b>(y, x);
        b = px[0]; g = px[1]; r = px[2];
    } else {
        return false;
    }
    return true;
}

void CaptureRenderer::updateHistoryBitmap(CaptureState& state) {
    m_historyBitmap.Reset();
    auto entry = CaptureHistory::instance().get(state.historyIndex);
    if (entry && !entry->image.empty() && m_renderTarget) {
        cv::Mat bgra;
        if (entry->image.channels() == 3) {
            cv::cvtColor(entry->image, bgra, cv::COLOR_BGR2BGRA);
        } else if (entry->image.channels() == 4) {
            bgra = entry->image;
        }
        if (!bgra.empty()) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );
            m_renderTarget->CreateBitmap(
                D2D1::SizeU(bgra.cols, bgra.rows),
                bgra.data, bgra.cols * 4, props, m_historyBitmap.GetAddressOf()
            );
        }
    }
    m_needsRender = true;
}

void CaptureRenderer::updateLongCanvasLayout(CaptureState& state) {
    if (!m_renderTarget || !state.isLongImageMode()) return;
    const auto sz = m_renderTarget->GetSize();
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);

    auto lay = ScrollCaptureEditorOverlay::computeEditorLayout(
        sz.width, sz.height, state.longCanvas.docWidth, state.longCanvas.docHeight, scale);

    // 底部为全功能标准主工具栏 + 二级属性栏预留充足工作台底座高度 (106px * scale)
    const float bottomDockReserve = 108.0f * scale;
    const float topReserve = 58.0f * scale;
    lay.topHeaderRect.top = 10.0f * scale;
    lay.topHeaderRect.bottom = 50.0f * scale;
    lay.stageRect.top = topReserve;
    lay.stageRect.bottom = (std::max)(lay.stageRect.top + 120.0f * scale, sz.height - bottomDockReserve);
    if (lay.minimapPanelRect.right > lay.minimapPanelRect.left) {
        lay.minimapPanelRect.top = lay.stageRect.top;
        lay.minimapPanelRect.bottom = lay.stageRect.bottom;
        const float mmPad = 8.0f * scale;
        const float availW = (std::max)(16.0f, (lay.minimapPanelRect.right - lay.minimapPanelRect.left) - mmPad * 2.0f);
        const float availH = (std::max)(48.0f, (lay.minimapPanelRect.bottom - lay.minimapPanelRect.top) - mmPad * 2.0f);
        const float aspect = static_cast<float>(state.longCanvas.docWidth) /
                             static_cast<float>((std::max)(1, state.longCanvas.docHeight));
        float imgH = availH;
        float imgW = imgH * aspect;
        if (imgW > availW) {
            imgW = availW;
            imgH = (std::max)(36.0f, imgW / (std::max)(0.01f, aspect));
        }
        imgW = std::clamp(imgW, 18.0f * scale, availW);
        const float cx = (lay.minimapPanelRect.left + lay.minimapPanelRect.right) * 0.5f;
        const float cy = (lay.minimapPanelRect.top + lay.minimapPanelRect.bottom) * 0.5f;
        lay.minimapImageRect = D2D1::RectF(cx - imgW * 0.5f, cy - imgH * 0.5f, cx + imgW * 0.5f, cy + imgH * 0.5f);
    }

    state.longCanvas.stageRect = lay.stageRect;
    state.longCanvas.topHeaderRect = lay.topHeaderRect;
    state.longCanvas.minimapPanelRect = lay.minimapPanelRect;
    state.longCanvas.minimapImageRect = lay.minimapImageRect;

    const float stageW = (std::max)(80.0f, lay.stageRect.right - lay.stageRect.left - 48.0f * scale);
    state.longCanvas.fitZoom = std::clamp(
        stageW / static_cast<float>((std::max)(1, state.longCanvas.docWidth)), 0.15f, 1.0f);
    if (state.longCanvas.zoom <= 0.01f) {
        state.longCanvas.zoom = state.longCanvas.fitZoom;
    }
    state.clampLongCanvasScroll();
}

void CaptureRenderer::drawLongCanvasTopHeader(const D2D1_RECT_F& docSelRect, CaptureState& state) {
    if (!m_renderTarget || !m_infoTextFormat) return;
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const bool zh = isCaptureUiChinese();
    const auto sz = m_renderTarget->GetSize();

    const int netW = (std::max)(1, static_cast<int>(std::lround(docSelRect.right - docSelRect.left)));
    const int netH = (std::max)(1, static_cast<int>(std::lround(docSelRect.bottom - docSelRect.top)));
    const int trimmedPx = (std::max)(0, state.longCanvas.docHeight - netH) +
                          (std::max)(0, state.longCanvas.docWidth - netW);
    const bool isCropped =
        (std::lround(docSelRect.top) > 0 ||
         std::lround(docSelRect.left) > 0 ||
         std::lround(docSelRect.bottom) < state.longCanvas.docHeight ||
         std::lround(docSelRect.right) < state.longCanvas.docWidth);
    const int zoomPct = static_cast<int>(std::lround(state.longCanvas.zoom * 100.0f));
    const bool near100 = std::abs(state.longCanvas.zoom - 1.0f) < 0.03f;

    const std::wstring titleText = zh
        ? (state.longCanvas.frameCount > 1
               ? std::format(L"长截图精修 ({} 帧)", state.longCanvas.frameCount)
               : L"长截图精修")
        : (state.longCanvas.frameCount > 1
               ? std::format(L"Long Capture ({} frames)", state.longCanvas.frameCount)
               : L"Long Capture");
    const std::wstring sizeBadge = isCropped
        ? (zh ? std::format(L"{} × {} px (已裁 -{}px)", netW, netH, trimmedPx)
              : std::format(L"{} × {} px (-{}px)", netW, netH, trimmedPx))
        : std::format(L"{} × {} px", netW, netH);

    const std::wstring lblTop = zh ? L"头部 Home" : L"Top";
    const std::wstring lblBot = zh ? L"尾部 End" : L"Bottom";
    const std::wstring lblReset = zh ? L"重置裁切" : L"Reset Crop";
    const std::wstring lblZoom = near100
        ? (zh ? std::format(L"{}% · 适宽", zoomPct) : std::format(L"{}% · Fit", zoomPct))
        : std::format(L"{}% · 1:1", zoomPct);

    IDWriteTextFormat* badgeFmt = m_hudBadgeFormat ? m_hudBadgeFormat.Get() : m_infoTextFormat.Get();
    const float padX = 12.0f * scale;
    const float btnH = 26.0f * scale;
    const float btnGap = 6.0f * scale;
    const float barH = 38.0f * scale;

    const float titleW = measureText(titleText, m_infoTextFormat.Get(), 400.0f * scale, barH).width + 6.0f * scale;
    const float sizeW = measureText(sizeBadge, badgeFmt, 420.0f * scale, barH).width + 16.0f * scale;
    const float wTop = measureText(lblTop, badgeFmt, 200.0f * scale, btnH).width + 20.0f * scale;
    const float wBot = measureText(lblBot, badgeFmt, 200.0f * scale, btnH).width + 20.0f * scale;
    const float wReset = measureText(lblReset, badgeFmt, 200.0f * scale, btnH).width + 20.0f * scale;
    const float wZoom = measureText(lblZoom, badgeFmt, 200.0f * scale, btnH).width + 20.0f * scale;

    const float totalW = padX + titleW + 8.0f * scale + sizeW + 14.0f * scale +
                         wTop + btnGap + wBot + btnGap + wReset + btnGap + wZoom + padX;
    const float stageCx = (state.longCanvas.stageRect.left + state.longCanvas.stageRect.right) * 0.5f;
    const float barLeft = std::clamp(stageCx - totalW * 0.5f, 8.0f * scale,
                                     (std::max)(8.0f * scale, sz.width - totalW - 8.0f * scale));
    const float barTop = 10.0f * scale;
    const D2D1_RECT_F headerRect = D2D1::RectF(barLeft, barTop, barLeft + totalW, barTop + barH);
    state.longCanvas.topHeaderRect = headerRect;

    drawGlassPanel(headerRect, 11.0f * scale, false);

    ComPtr<ID2D1SolidColorBrush> priText, secText, chipBg, chipHoverBg, chipBorder, accentText, disabledText, dividerBr;
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, priText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSecondaryText, secText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kChipAccentBg, chipBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kChipHoverBg, chipHoverBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kChipAccentBorder, chipBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kChipAccentText, accentText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.58f, 0.64f, 0.72f, 0.55f), disabledText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudDivider, dividerBr.GetAddressOf());

    float curX = barLeft + padX;
    const float midY = barTop + barH * 0.5f;

    if (priText) {
        TextFormatScope s(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawTextW(titleText.c_str(), static_cast<UINT32>(titleText.size()),
                                  m_infoTextFormat.Get(),
                                  D2D1::RectF(curX, barTop, curX + titleW, barTop + barH),
                                  priText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    curX += titleW + 8.0f * scale;

    // 净尺寸胶囊（点击同样可展开 px/dp 尺寸菜单）
    const D2D1_RECT_F sizeChipRect = D2D1::RectF(curX, midY - btnH * 0.5f, curX + sizeW, midY + btnH * 0.5f);
    state.sizeHudRect = sizeChipRect;
    const bool sizeHov = (state.currentCursor.x >= sizeChipRect.left && state.currentCursor.x <= sizeChipRect.right &&
                          state.currentCursor.y >= sizeChipRect.top && state.currentCursor.y <= sizeChipRect.bottom);
    state.isSizeHudHovered = sizeHov;
    if (sizeHov ? chipHoverBg : chipBg) {
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sizeChipRect, 6.0f * scale, 6.0f * scale),
                                             (sizeHov ? chipHoverBg : chipBg).Get());
    }
    if (chipBorder) {
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(sizeChipRect, 6.0f * scale, 6.0f * scale),
                                             chipBorder.Get(), 0.9f * scale);
    }
    if (badgeFmt && accentText) {
        TextFormatScope s(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawTextW(sizeBadge.c_str(), static_cast<UINT32>(sizeBadge.size()),
                                  badgeFmt, sizeChipRect, accentText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    curX += sizeW + 7.0f * scale;

    if (dividerBr) {
        m_renderTarget->DrawLine(D2D1::Point2F(curX, barTop + 9.0f * scale),
                                 D2D1::Point2F(curX, barTop + barH - 9.0f * scale),
                                 dividerBr.Get(), 1.0f * scale);
    }
    curX += 7.0f * scale;

    state.longCanvas.hoveredHeaderBtn = -1;
    auto drawActionChip = [&](int idx, float w, const std::wstring& label, bool enabled, D2D1_RECT_F& outRect) {
        outRect = D2D1::RectF(curX, midY - btnH * 0.5f, curX + w, midY + btnH * 0.5f);
        const bool hov = enabled &&
                         state.currentCursor.x >= outRect.left && state.currentCursor.x <= outRect.right &&
                         state.currentCursor.y >= outRect.top && state.currentCursor.y <= outRect.bottom;
        if (hov) {
            state.longCanvas.hoveredHeaderBtn = idx;
            if (chipHoverBg) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(outRect, 6.5f * scale, 6.5f * scale), chipHoverBg.Get());
            }
            if (chipBorder) {
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(outRect, 6.5f * scale, 6.5f * scale), chipBorder.Get(), 1.0f * scale);
            }
        }
        ID2D1Brush* tBr = !enabled ? disabledText.Get() : (hov ? priText.Get() : secText.Get());
        if (badgeFmt && tBr) {
            TextFormatScope s(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                                      badgeFmt, outRect, tBr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        curX += w + btnGap;
    };

    drawActionChip(0, wTop, lblTop, true, state.longCanvas.btnJumpTopRect);
    drawActionChip(1, wBot, lblBot, true, state.longCanvas.btnJumpBottomRect);
    drawActionChip(2, wReset, lblReset, isCropped, state.longCanvas.btnResetCropRect);
    drawActionChip(3, wZoom, lblZoom, true, state.longCanvas.btnZoomToggleRect);
}

void CaptureRenderer::drawLongCanvasMinimap(const D2D1_RECT_F& docSelRect, CaptureState& state) {
    if (!m_renderTarget) return;
    const auto& panel = state.longCanvas.minimapPanelRect;
    const auto& imgRect = state.longCanvas.minimapImageRect;
    if (panel.right <= panel.left || imgRect.bottom <= imgRect.top) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);

    // 惰性构建高品质全景缩略位图 (最大高度 1024px)
    if ((!m_minimapBitmap || m_minimapSrcW != state.longCanvas.docWidth || m_minimapSrcH != state.longCanvas.docHeight) &&
        !state.frozenScreen.empty()) {
        m_minimapBitmap.Reset();
        const int maxThumbH = 1024;
        const double ratio = (state.frozenScreen.rows > maxThumbH)
            ? (static_cast<double>(maxThumbH) / static_cast<double>(state.frozenScreen.rows))
            : 1.0;
        const int tw = (std::max)(1, static_cast<int>(std::lround(state.frozenScreen.cols * ratio)));
        const int th = (std::max)(1, static_cast<int>(std::lround(state.frozenScreen.rows * ratio)));
        cv::Mat thumb;
        if (ratio < 0.999) {
            cv::resize(state.frozenScreen, thumb, cv::Size(tw, th), 0.0, 0.0, cv::INTER_AREA);
        } else {
            thumb = state.frozenScreen;
        }
        cv::Mat bgra;
        D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        if (thumb.channels() == 4) {
            bgra = thumb;
            alphaMode = D2D1_ALPHA_MODE_IGNORE;
        } else if (thumb.channels() == 3) {
            cv::cvtColor(thumb, bgra, cv::COLOR_BGR2BGRA);
        }
        if (!bgra.empty()) {
            if (!bgra.isContinuous()) bgra = bgra.clone();
            const auto props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alphaMode));
            if (SUCCEEDED(m_renderTarget->CreateBitmap(
                    D2D1::SizeU(static_cast<UINT32>(bgra.cols), static_cast<UINT32>(bgra.rows)),
                    bgra.data, static_cast<UINT32>(bgra.step[0]), props, m_minimapBitmap.GetAddressOf()))) {
                m_minimapSrcW = state.longCanvas.docWidth;
                m_minimapSrcH = state.longCanvas.docHeight;
            }
        }
    }

    ComPtr<ID2D1SolidColorBrush> panelBg, panelBorder, trimMaskBr, cropBarBr, viewFillBr, viewBorderBr;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.86f), panelBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f), panelBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.03f, 0.05f, 0.09f, 0.72f), trimMaskBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.68f, 1.0f, 0.98f), cropBarBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f), viewFillBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f), viewBorderBr.GetAddressOf());

    const auto panelRound = D2D1::RoundedRect(panel, 11.0f * scale, 11.0f * scale);
    if (panelBg) m_renderTarget->FillRoundedRectangle(panelRound, panelBg.Get());
    if (panelBorder) m_renderTarget->DrawRoundedRectangle(panelRound, panelBorder.Get(), 1.0f * scale);

    if (m_minimapBitmap) {
        m_renderTarget->DrawBitmap(m_minimapBitmap.Get(), imgRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    const int topRow = std::clamp(static_cast<int>(std::lround(docSelRect.top)), 0, state.longCanvas.docHeight);
    const int botRow = std::clamp(static_cast<int>(std::lround(docSelRect.bottom)), topRow, state.longCanvas.docHeight);
    const float mmCropTopY = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(topRow, imgRect, state.longCanvas.docHeight);
    const float mmCropBotY = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(botRow, imgRect, state.longCanvas.docHeight);

    // 缩略图上下被裁除区间压暗
    if (trimMaskBr) {
        if (mmCropTopY > imgRect.top + 0.5f) {
            m_renderTarget->FillRectangle(D2D1::RectF(imgRect.left, imgRect.top, imgRect.right, mmCropTopY), trimMaskBr.Get());
        }
        if (mmCropBotY < imgRect.bottom - 0.5f) {
            m_renderTarget->FillRectangle(D2D1::RectF(imgRect.left, mmCropBotY, imgRect.right, imgRect.bottom), trimMaskBr.Get());
        }
    }

    // 当前主画布可见视口高亮框
    const auto visTopDoc = state.screenToDoc(state.longCanvas.stageRect.left, state.longCanvas.stageRect.top);
    const auto visBotDoc = state.screenToDoc(state.longCanvas.stageRect.left, state.longCanvas.stageRect.bottom);
    const int visRow0 = std::clamp(static_cast<int>(std::lround(visTopDoc.y)), 0, state.longCanvas.docHeight);
    const int visRow1 = std::clamp(static_cast<int>(std::lround(visBotDoc.y)), visRow0, state.longCanvas.docHeight);
    const float mmVisTop = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(visRow0, imgRect, state.longCanvas.docHeight);
    const float mmVisBot = (std::max)(mmVisTop + 8.0f * scale,
        ScrollCaptureEditorOverlay::mapImageRowToMinimapY(visRow1, imgRect, state.longCanvas.docHeight));
    const D2D1_RECT_F vpBox = D2D1::RectF(
        imgRect.left - 2.0f * scale,
        std::clamp(mmVisTop, imgRect.top, imgRect.bottom - 6.0f * scale),
        imgRect.right + 2.0f * scale,
        std::clamp(mmVisBot, imgRect.top + 6.0f * scale, imgRect.bottom));
    if (viewFillBr) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(vpBox, 3.0f * scale, 3.0f * scale), viewFillBr.Get());
    if (viewBorderBr) m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(vpBox, 3.0f * scale, 3.0f * scale), viewBorderBr.Get(), 1.4f * scale);

    // 缩略图顶/底 Smartisan 裁切滑块横梁
    if (cropBarBr) {
        const float barOverhang = 4.0f * scale;
        const float pillH = 4.0f * scale;
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(imgRect.left - barOverhang, mmCropTopY - pillH * 0.5f,
                                          imgRect.right + barOverhang, mmCropTopY + pillH * 0.5f),
                              2.0f * scale, 2.0f * scale),
            cropBarBr.Get());
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(imgRect.left - barOverhang, mmCropBotY - pillH * 0.5f,
                                          imgRect.right + barOverhang, mmCropBotY + pillH * 0.5f),
                              2.0f * scale, 2.0f * scale),
            cropBarBr.Get());
    }
}

void CaptureRenderer::renderLongImageStudio(CaptureState& state) {
    updateLongCanvasLayout(state);

    const auto surfaceSize = m_renderTarget->GetSize();
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const bool zh = isCaptureUiChinese();

    // 1. 专业暗房曜石灰底衬
    ComPtr<ID2D1SolidColorBrush> studioBg, stageShadowBr;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.055f, 0.072f, 0.110f, 0.97f), studioBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.01f, 0.02f, 0.05f, 0.42f), stageShadowBr.GetAddressOf());
    if (studioBg) {
        m_renderTarget->FillRectangle(D2D1::RectF(0.0f, 0.0f, surfaceSize.width, surfaceSize.height), studioBg.Get());
    }

    state.loupeHudRect = D2D1::RectF(0, 0, 0, 0);
    state.hierarchyBadgeRect = D2D1::RectF(0, 0, 0, 0);

    const float x1 = static_cast<float>((std::min)(state.dragStart.x, state.dragEnd.x));
    const float y1 = static_cast<float>((std::min)(state.dragStart.y, state.dragEnd.y));
    const float x2 = static_cast<float>((std::max)(state.dragStart.x, state.dragEnd.x));
    const float y2 = static_cast<float>((std::max)(state.dragStart.y, state.dragEnd.y));
    const D2D1_RECT_F docSelRect = D2D1::RectF(x1, y1, x2, y2);
    const D2D1_RECT_F fullDocRect = D2D1::RectF(
        0.0f, 0.0f, static_cast<float>(state.longCanvas.docWidth), static_cast<float>(state.longCanvas.docHeight));
    const D2D1_RECT_F docScreenRect = state.docRectToScreen(fullDocRect);
    const D2D1_RECT_F selScreenRect = state.docRectToScreen(docSelRect);

    // 2. 主画布裁剪视口渲染（支持缩放与平移矩阵，使全部标注工具在文档空间 1:1 精确工作）
    m_renderTarget->PushAxisAlignedClip(state.longCanvas.stageRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (stageShadowBr) {
        const D2D1_RECT_F sh = D2D1::RectF(
            docScreenRect.left - 3.0f * scale, docScreenRect.top + 2.0f * scale,
            docScreenRect.right + 3.0f * scale, docScreenRect.bottom + 8.0f * scale);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sh, 6.0f * scale, 6.0f * scale), stageShadowBr.Get());
    }

    const float z = (std::max)(0.05f, state.longCanvas.zoom);
    const auto orig = state.longCanvasOrigin();
    const D2D1_MATRIX_3X2_F canvasXform =
        D2D1::Matrix3x2F::Scale(z, z) * D2D1::Matrix3x2F::Translation(orig.x, orig.y);
    m_renderTarget->SetTransform(canvasXform);

    // 2.1 绘制完整长图底图瓦片（僅对视口相交瓦片提交 DrawBitmap，60fps 零冗余开销）
    if (!m_screenTiles.empty() && m_screenTilesW > 0 && m_screenTilesH > 0) {
        const auto visLt = state.screenToDoc(state.longCanvas.stageRect.left - 8.0f, state.longCanvas.stageRect.top - 8.0f);
        const auto visRb = state.screenToDoc(state.longCanvas.stageRect.right + 8.0f, state.longCanvas.stageRect.bottom + 8.0f);
        const D2D1_RECT_F visDoc = D2D1::RectF(
            std::clamp(visLt.x, 0.0f, static_cast<float>(m_screenTilesW)),
            std::clamp(visLt.y, 0.0f, static_cast<float>(m_screenTilesH)),
            std::clamp(visRb.x, 0.0f, static_cast<float>(m_screenTilesW)),
            std::clamp(visRb.y, 0.0f, static_cast<float>(m_screenTilesH)));
        const auto interp = (std::abs(z - 1.0f) < 0.01f)
            ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
        drawTiledRegion(m_screenTiles, m_screenTilesW, m_screenTilesH, visDoc, visDoc, interp);
    }

    // 2.2 美化外壳底板 + 已合成标注缓存瓦片 + 正在拖拽的实时标注预览（100% 复用标准截图引擎！）
    if (state.beautyShell.enabled) {
        drawBeautyShellPreview(docSelRect, state);
    }
    drawMarkupPreview(docSelRect, state);
    drawActiveMarkupPreview(docSelRect, state);

    // 恢复 1:1 屏幕像素空间绘制非破坏性裁切遮罩与 Smartisan 推拉握把，确保任意缩放下握把清晰恒定
    m_renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());

    // 2.3 Smartisan 非破坏性已裁除区域半透暗罩 + 45 度导引斜线（可随时往回拉无损恢复像素）
    ComPtr<ID2D1SolidColorBrush> cropMaskBr, hatchBr, borderShadowBr, handleFillBr, handleRingBr, handleAccentBr, gripLineBr;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.03f, 0.05f, 0.09f, 0.66f), cropMaskBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f), hatchBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.42f), borderShadowBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f), handleFillBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.16f, 0.23f, 0.85f), handleRingBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.23f, 0.56f, 0.98f, 0.98f), handleAccentBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.28f, 0.34f, 0.43f, 0.85f), gripLineBr.GetAddressOf());

    auto drawTrimmedRegion = [&](const D2D1_RECT_F& r) {
        const D2D1_RECT_F clipped = D2D1::RectF(
            (std::max)(r.left, state.longCanvas.stageRect.left),
            (std::max)(r.top, state.longCanvas.stageRect.top),
            (std::min)(r.right, state.longCanvas.stageRect.right),
            (std::min)(r.bottom, state.longCanvas.stageRect.bottom));
        if (clipped.right <= clipped.left + 0.5f || clipped.bottom <= clipped.top + 0.5f) return;
        if (cropMaskBr) m_renderTarget->FillRectangle(clipped, cropMaskBr.Get());
        if (hatchBr) {
            m_renderTarget->PushAxisAlignedClip(clipped, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const float step = 16.0f * scale;
            const float h = clipped.bottom - clipped.top;
            for (float x = clipped.left - h; x < clipped.right + step; x += step) {
                m_renderTarget->DrawLine(
                    D2D1::Point2F(x, clipped.bottom),
                    D2D1::Point2F(x + h, clipped.top),
                    hatchBr.Get(), 1.0f * scale);
            }
            m_renderTarget->PopAxisAlignedClip();
        }
    };

    drawTrimmedRegion(D2D1::RectF(docScreenRect.left, docScreenRect.top, docScreenRect.right, selScreenRect.top));
    drawTrimmedRegion(D2D1::RectF(docScreenRect.left, selScreenRect.bottom, docScreenRect.right, docScreenRect.bottom));
    drawTrimmedRegion(D2D1::RectF(docScreenRect.left, selScreenRect.top, selScreenRect.left, selScreenRect.bottom));
    drawTrimmedRegion(D2D1::RectF(selScreenRect.right, selScreenRect.top, docScreenRect.right, selScreenRect.bottom));

    // 2.4 保留区高对比白晶边框与 Smartisan 顶/底推拉裁切胶囊标尺
    const float effRadiusScreen = state.effectiveCornerRadius() * z;
    if (!(state.beautyShell.enabled && state.beautyShell.padding > 0)) {
        if (effRadiusScreen > 0.5f) {
            const auto rr = D2D1::RoundedRect(selScreenRect, effRadiusScreen, effRadiusScreen);
            if (borderShadowBr) m_renderTarget->DrawRoundedRectangle(rr, borderShadowBr.Get(), 2.8f * scale);
            if (m_borderBrush) m_renderTarget->DrawRoundedRectangle(rr, m_borderBrush.Get(), 1.7f * scale);
        } else {
            if (borderShadowBr) m_renderTarget->DrawRectangle(selScreenRect, borderShadowBr.Get(), 2.8f * scale);
            if (m_borderBrush) m_renderTarget->DrawRectangle(selScreenRect, m_borderBrush.Get(), 1.7f * scale);
        }
    }

    const float pillW = (std::min)(92.0f * scale, (std::max)(44.0f * scale, (selScreenRect.right - selScreenRect.left) * 0.35f));
    const float pillH = 14.0f * scale;
    const float selMidX = (selScreenRect.left + selScreenRect.right) * 0.5f;
    const float curX = static_cast<float>(state.currentCursor.x);
    const float curY = static_cast<float>(state.currentCursor.y);

    auto drawSmartisanCropBar = [&](float yEdge, bool isTop) {
        const bool draggingThis =
            (state.isAdjustingSelection && state.selAdjustHandle == (isTop ? HitArea::T : HitArea::B)) ||
            (state.longCanvas.dragMode == (isTop ? CaptureState::LongCanvasDragMode::MinimapCropTop
                                                 : CaptureState::LongCanvasDragMode::MinimapCropBottom));
        const D2D1_RECT_F pill = D2D1::RectF(selMidX - pillW * 0.5f, yEdge - pillH * 0.5f,
                                             selMidX + pillW * 0.5f, yEdge + pillH * 0.5f);
        const bool hovered = draggingThis ||
            (curX >= selScreenRect.left && curX <= selScreenRect.right &&
             std::abs(curY - yEdge) <= 10.0f * scale);

        if (hovered && handleAccentBr) {
            m_renderTarget->DrawLine(D2D1::Point2F(selScreenRect.left, yEdge),
                                     D2D1::Point2F(selScreenRect.right, yEdge),
                                     handleAccentBr.Get(), 2.2f * scale);
        }
        const auto pillRound = D2D1::RoundedRect(pill, pillH * 0.5f, pillH * 0.5f);
        if (borderShadowBr) {
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(pill.left, pill.top + 1.2f * scale, pill.right, pill.bottom + 1.8f * scale),
                                  pillH * 0.5f, pillH * 0.5f),
                borderShadowBr.Get());
        }
        if (handleFillBr) m_renderTarget->FillRoundedRectangle(pillRound, handleFillBr.Get());
        ID2D1Brush* ring = (hovered && handleAccentBr) ? handleAccentBr.Get() : handleRingBr.Get();
        if (ring) m_renderTarget->DrawRoundedRectangle(pillRound, ring, (hovered ? 1.6f : 1.1f) * scale);

        if (gripLineBr) {
            const float gw = 18.0f * scale;
            m_renderTarget->DrawLine(D2D1::Point2F(selMidX - gw * 0.5f, yEdge - 2.0f * scale),
                                     D2D1::Point2F(selMidX + gw * 0.5f, yEdge - 2.0f * scale),
                                     (hovered && handleAccentBr) ? handleAccentBr.Get() : gripLineBr.Get(), 1.2f * scale);
            m_renderTarget->DrawLine(D2D1::Point2F(selMidX - gw * 0.5f, yEdge + 2.0f * scale),
                                     D2D1::Point2F(selMidX + gw * 0.5f, yEdge + 2.0f * scale),
                                     (hovered && handleAccentBr) ? handleAccentBr.Get() : gripLineBr.Get(), 1.2f * scale);
        }

        if (draggingThis && m_hudBadgeFormat) {
            const int cutPx = isTop
                ? (std::max)(0, static_cast<int>(std::lround(docSelRect.top)))
                : (std::max)(0, state.longCanvas.docHeight - static_cast<int>(std::lround(docSelRect.bottom)));
            const std::wstring tip = isTop
                ? (zh ? std::format(L"顶部裁去 {} px", cutPx) : std::format(L"Trim Top {} px", cutPx))
                : (zh ? std::format(L"底部裁去 {} px", cutPx) : std::format(L"Trim Bottom {} px", cutPx));
            const float tw = measureText(tip, m_hudBadgeFormat.Get(), 260.0f * scale, 28.0f * scale).width + 18.0f * scale;
            const float th = 24.0f * scale;
            const float ty = isTop ? (pill.bottom + 6.0f * scale) : (pill.top - th - 6.0f * scale);
            const D2D1_RECT_F tipRect = D2D1::RectF(selMidX - tw * 0.5f, ty, selMidX + tw * 0.5f, ty + th);
            drawGlassPanel(tipRect, 6.0f * scale, false);
            if (m_infoTextBrush) {
                TextFormatScope s(m_hudBadgeFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawTextW(tip.c_str(), static_cast<UINT32>(tip.size()),
                                          m_hudBadgeFormat.Get(), tipRect, m_infoTextBrush.Get());
            }
        }
    };

    drawSmartisanCropBar(selScreenRect.top, true);
    drawSmartisanCropBar(selScreenRect.bottom, false);

    // 左右中点微调锚点与四角内侧圆角手柄
    const float selMidY = (selScreenRect.top + selScreenRect.bottom) * 0.5f;
    const float sideDotR = 4.2f * scale;
    for (float sxEdge : {selScreenRect.left, selScreenRect.right}) {
        auto el = D2D1::Ellipse(D2D1::Point2F(sxEdge, selMidY), sideDotR, sideDotR);
        if (handleFillBr) m_renderTarget->FillEllipse(el, handleFillBr.Get());
        if (handleRingBr) m_renderTarget->DrawEllipse(el, handleRingBr.Get(), 1.25f * scale);
    }
    drawCornerRadiusHandleVisual(
        selScreenRect, effRadiusScreen / scale, scale, state.isAdjustingCornerRadius,
        state.cornerDragStartPos, state.currentCursor, 64.0f, state.cornerDragIndex);

    m_renderTarget->PopAxisAlignedClip();

    // 3. 实时光学放大镜瞄准器 + 顶部快捷导航舱 + 右侧全景缩略导航条 + 全功能标准主/副/侧边工具栏
    if (state.isMarkupToolActive && state.currentTool == MarkupTool::Magnifier && !state.isMarking && !state.isManipulating) {
        drawDynamicMagnifier(state);
    }

    drawLongCanvasTopHeader(docSelRect, state);
    drawLongCanvasMinimap(docSelRect, state);

    const D2D1_RECT_F anchorRect = state.toolbarAnchorRect(docSelRect);
    drawQrChip(anchorRect, state);
    drawToolbar(anchorRect, state);
    drawSizeMenu(state.sizeHudRect, state);
    updateShortcutHintAvoidance(state);
    drawFloatingToast(state);
}

void CaptureRenderer::render(CaptureState& state) {
    if (!m_renderTarget && m_hwnd && IsWindow(m_hwnd)) {
        if (!createRenderResources(state)) return;
        if (!state.frozenScreen.empty()) {
            updateScreenBitmap(state.frozenScreen);
        }
        if (state.historyMode) {
            updateHistoryBitmap(state);
        }
        m_markupCacheDirty = true;
    }
    if (!m_renderTarget) return;

    const auto recoverFromDeviceLoss = [this](HRESULT hr) {
        if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            LOG_WARN("CaptureRenderer: EndDraw device lost (hr=0x{:08X}), scheduling target recovery", static_cast<uint32_t>(hr));
            const HWND savedHwnd = m_hwnd;
            releaseWindowResources();
            m_hwnd = savedHwnd;
            m_markupCacheDirty = true;
            m_needsRender = true;
            if (m_hwnd && IsWindow(m_hwnd)) {
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        }
    };

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (state.isLongImageMode()) {
        renderLongImageStudio(state);
        recoverFromDeviceLoss(m_renderTarget->EndDraw());
        return;
    }

    if (state.historyMode) {
        const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
        const bool zh = isCaptureUiChinese();
        auto size = m_renderTarget->GetSize();
        m_renderTarget->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), m_dimBrush.Get());
        
        if (m_historyBitmap) {
            auto bmpSize = m_historyBitmap->GetSize();
            float x = (size.width - bmpSize.width) / 2.0f;
            float y = (size.height - bmpSize.height) / 2.0f;
            D2D1_RECT_F dstRect = D2D1::RectF(x, y, x + bmpSize.width, y + bmpSize.height);
            m_renderTarget->DrawBitmap(m_historyBitmap.Get(), dstRect);
            if (m_borderBrush) {
                m_renderTarget->DrawRectangle(dstRect, m_borderBrush.Get(), 2.0f * scale);
            }
            
            const std::wstring info = zh
                ? std::format(L"历史回放: {} / {}", state.historyIndex + 1, CaptureHistory::instance().count())
                : std::format(L"History: {} / {}", state.historyIndex + 1, CaptureHistory::instance().count());
            const float textW = measureText(info, m_infoTextFormat.Get(), 1000.0f * scale, 64.0f * scale).width;
            const float pillW = (std::max)(160.0f * scale, textW + 28.0f * scale);
            const float pillH = 28.0f * scale;
            const float margin = 8.0f * scale;
            const float pillX = std::clamp(x, margin, (std::max)(margin, size.width - pillW - margin));
            float pillY = y - pillH - 8.0f * scale;
            if (pillY < margin) {
                pillY = (std::min)(y + 8.0f * scale, (std::max)(margin, size.height - pillH - margin));
            }
            const D2D1_RECT_F pillRect = D2D1::RectF(pillX, pillY, pillX + pillW, pillY + pillH);
            drawGlassPanel(pillRect, 7.0f * scale, false);
            if (m_infoTextFormat && m_infoTextBrush) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(info.c_str(), static_cast<UINT32>(info.size()),
                                         m_infoTextFormat.Get(), pillRect,
                                         m_infoTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        } else {
            const std::wstring info = zh ? L"暂无历史截图" : L"No capture history";
            if (m_infoTextFormat && m_infoTextBrush) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(info.c_str(), static_cast<UINT32>(info.size()), m_infoTextFormat.Get(),
                                         D2D1::RectF(0, 0, size.width, size.height), m_infoTextBrush.Get());
            }
        }
        
        recoverFromDeviceLoss(m_renderTarget->EndDraw());
        return;
    }

    // 绘制冻结的屏幕
    if (m_screenBitmap) {
        auto size = m_renderTarget->GetSize();
        m_renderTarget->DrawBitmap(m_screenBitmap.Get(),
                                   D2D1::RectF(0, 0, size.width, size.height));
    }

    state.loupeHudRect = D2D1::RectF(0, 0, 0, 0);
    state.hierarchyBadgeRect = D2D1::RectF(0, 0, 0, 0);

    if ((int)state.state.load() == (int)OverlayState::Selecting && state.dragging) {
        // 正在拖拽选区
        float x1 = static_cast<float>(std::min(state.dragStart.x, state.dragEnd.x));
        float y1 = static_cast<float>(std::min(state.dragStart.y, state.dragEnd.y));
        float x2 = static_cast<float>(std::max(state.dragStart.x, state.dragEnd.x));
        float y2 = static_cast<float>(std::max(state.dragStart.y, state.dragEnd.y));

        D2D1_RECT_F selRect = D2D1::RectF(x1, y1, x2, y2);
        drawDimOverlay(selRect, state);
        if (state.beautyShell.enabled && state.mode != OverlayMode::RecordRegion) {
            drawBeautyShellPreview(selRect, state);
        }
        drawSmartAlignmentGuides(selRect, state);
        drawSelection(selRect, state);
        drawSizeInfo(selRect, state);
        // 拖拽中也显示取色放大镜，便于像素级对齐选区边缘
        drawSelectionLoupe(static_cast<float>(state.currentCursor.x), static_cast<float>(state.currentCursor.y), state);
    } else if ((int)state.state.load() == (int)OverlayState::Selected || (int)state.state.load() == (int)OverlayState::Marking) {
        // 选区已确认
        float x1 = static_cast<float>(std::min(state.dragStart.x, state.dragEnd.x));
        float y1 = static_cast<float>(std::min(state.dragStart.y, state.dragEnd.y));
        float x2 = static_cast<float>(std::max(state.dragStart.x, state.dragEnd.x));
        float y2 = static_cast<float>(std::max(state.dragStart.y, state.dragEnd.y));

        D2D1_RECT_F selRect = D2D1::RectF(x1, y1, x2, y2);
        drawDimOverlay(selRect, state);

        // 如果启用了美化外壳模式，必须在标注之前绘制底板与圆角原图，作为底层画布，绝不可覆盖标注！
        if (state.beautyShell.enabled && state.mode != OverlayMode::RecordRegion) {
            drawBeautyShellPreview(selRect, state);
        }

        drawMarkupPreview(selRect, state);
        drawActiveMarkupPreview(selRect, state);

        if (state.isMarkupToolActive && state.currentTool == MarkupTool::Magnifier && !state.isMarking && !state.isManipulating) {
            drawDynamicMagnifier(state);
        }

        drawSmartAlignmentGuides(selRect, state);
        drawSelection(selRect, state);
        drawSizeInfo(selRect, state);
        drawQrChip(selRect, state);
        drawToolbar(selRect, state);
        drawSizeMenu(state.sizeHudRect, state);
    } else {
        // 空闲/选区前 — 全屏变暗 + 十字准星 + 窗口高亮
        auto size = m_renderTarget->GetSize();
        float scale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
        state.loupeHudRect = computeSelectionLoupeRect(
            static_cast<float>(state.currentCursor.x),
            static_cast<float>(state.currentCursor.y),
            size, scale);
        state.sizeHudRect = D2D1::RectF(0, 0, 0, 0);

        m_renderTarget->FillRectangle(
            D2D1::RectF(0, 0, size.width, size.height), m_dimBrush.Get()
        );

        // 检测光标下的窗口并高亮其边界（采用 Windows 11 现代圆角贴合与 Spring 阻尼平滑过渡）
        bool hasDetectedWindow = (state.animDetectedWindowRect.right > state.animDetectedWindowRect.left &&
                                  state.animDetectedWindowRect.bottom > state.animDetectedWindowRect.top) ||
                                 (state.detectedWindow.right > state.detectedWindow.left &&
                                  state.detectedWindow.bottom > state.detectedWindow.top);
        if (hasDetectedWindow) {
            D2D1_RECT_F winRect;
            if (state.animDetectedWindowRect.right > state.animDetectedWindowRect.left) {
                winRect = state.animDetectedWindowRect;
            } else {
                winRect = D2D1::RectF(
                    static_cast<float>(state.detectedWindow.left),
                    static_cast<float>(state.detectedWindow.top),
                    static_cast<float>(state.detectedWindow.right),
                    static_cast<float>(state.detectedWindow.bottom)
                );
            }
            
            float r = state.detectedWindowCornerRadius * scale;
            auto roundedWin = D2D1::RoundedRect(winRect, r, r);

            // 窗口区域采用圆角几何裁剪透出原始画面（去掉变暗效果）
            if (m_screenBitmap && m_d2dFactory) {
                ComPtr<ID2D1RoundedRectangleGeometry> geo;
                if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(roundedWin, geo.GetAddressOf())) && geo) {
                    ComPtr<ID2D1Layer> layer;
                    if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
                        m_renderTarget->PushLayer(
                            D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get(),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
                            layer.Get());
                        m_renderTarget->DrawBitmap(m_screenBitmap.Get(), winRect, 1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &winRect);
                        m_renderTarget->PopLayer();
                    }
                }
            }
            // 全天候无色偏双层白晶圆角高亮外框（细腻石板外衬 + 纯白晶主线，白底/暗底皆清晰锐利）
            {
                ComPtr<ID2D1SolidColorBrush> winOutlineShadow;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.28f), winOutlineShadow.GetAddressOf());
                if (winOutlineShadow) {
                    m_renderTarget->DrawRoundedRectangle(roundedWin, winOutlineShadow.Get(), 3.0f * scale);
                }
                m_renderTarget->DrawRoundedRectangle(roundedWin, m_borderBrush.Get(), 1.8f * scale);
            }
            // 显示窗口尺寸与圆角提示
            drawSizeInfo(winRect, state);

            // 动态渲染温润雪瓷白层级提示徽章 (如 "[2/4] 控件: 搜索框 (680x42)")，自动躲避尺寸胶囊、取色放大镜与鼠标光标
            if (GetTickCount() < state.hierarchyBadgeFadeOutUntil && !state.hierarchyBadgeText.empty() && m_dwriteFactory && m_infoTextFormat) {
                float badgeH = 34.0f * scale;
                float padH = 16.0f * scale;
                float dotZone = 16.0f * scale;
                ComPtr<IDWriteTextLayout> badgeLayout;
                m_dwriteFactory->CreateTextLayout(
                    state.hierarchyBadgeText.c_str(),
                    static_cast<UINT32>(state.hierarchyBadgeText.size()),
                    m_infoTextFormat.Get(), 1000.0f * scale, badgeH, badgeLayout.GetAddressOf());
                float textW = 140.0f * scale;
                if (badgeLayout) {
                    DWRITE_TEXT_METRICS m{};
                    badgeLayout->GetMetrics(&m);
                    textW = m.widthIncludingTrailingWhitespace > m.width ? m.widthIncludingTrailingWhitespace : m.width;
                }
                float badgeW = textW + dotZone + padH * 2.0f;
                const float margin = 8.0f * scale;
                const float centerBx = std::clamp(
                    winRect.left + (winRect.right - winRect.left - badgeW) * 0.5f,
                    margin, (std::max)(margin, size.width - badgeW - margin));
                const float rightBx = std::clamp(
                    winRect.right - badgeW,
                    margin, (std::max)(margin, size.width - badgeW - margin));
                const float afterHudBx = std::clamp(
                    state.sizeHudRect.right + 12.0f * scale,
                    margin, (std::max)(margin, size.width - badgeW - margin));
                const float topOutY = winRect.top - badgeH - 10.0f * scale;
                const float topInY = winRect.top + 10.0f * scale;
                const float botOutY = winRect.bottom + 10.0f * scale;
                const float botInY = winRect.bottom - badgeH - 10.0f * scale;

                std::vector<D2D1_RECT_F> badgeCandidates = {
                    D2D1::RectF(centerBx, topOutY >= margin ? topOutY : topInY, centerBx + badgeW, (topOutY >= margin ? topOutY : topInY) + badgeH),
                    D2D1::RectF(afterHudBx, topOutY >= margin ? topOutY : topInY, afterHudBx + badgeW, (topOutY >= margin ? topOutY : topInY) + badgeH),
                    D2D1::RectF(rightBx, topOutY >= margin ? topOutY : topInY, rightBx + badgeW, (topOutY >= margin ? topOutY : topInY) + badgeH),
                    D2D1::RectF(centerBx, botOutY + badgeH <= size.height - margin ? botOutY : botInY, centerBx + badgeW, (botOutY + badgeH <= size.height - margin ? botOutY : botInY) + badgeH),
                    D2D1::RectF(centerBx, topInY, centerBx + badgeW, topInY + badgeH),
                };

                const float curFx = static_cast<float>(state.currentCursor.x);
                const float curFy = static_cast<float>(state.currentCursor.y);
                const D2D1_RECT_F cursorBox = D2D1::RectF(curFx - 32.0f * scale, curFy - 32.0f * scale, curFx + 32.0f * scale, curFy + 32.0f * scale);
                auto rectsOverlap = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b, float gap) -> bool {
                    if (b.right <= b.left || b.bottom <= b.top) return false;
                    return (a.left < b.right + gap && a.right > b.left - gap &&
                            a.top < b.bottom + gap && a.bottom > b.top - gap);
                };

                D2D1_RECT_F badgeRect = badgeCandidates.front();
                for (const auto& cand : badgeCandidates) {
                    if (cand.top < margin || cand.bottom > size.height - margin) continue;
                    if (!rectsOverlap(cand, state.sizeHudRect, 8.0f * scale) &&
                        !rectsOverlap(cand, state.loupeHudRect, 8.0f * scale) &&
                        !rectsOverlap(cand, cursorBox, 6.0f * scale)) {
                        badgeRect = cand;
                        break;
                    }
                }
                state.hierarchyBadgeRect = badgeRect;

                if (m_infoTextBrush) {
                    DWORD now = GetTickCount();
                    DWORD remaining = (state.hierarchyBadgeFadeOutUntil > now) ? (state.hierarchyBadgeFadeOutUntil - now) : 0;
                    float alphaMultiplier = 1.0f;
                    if (remaining < 300) {
                        alphaMultiplier = static_cast<float>(remaining) / 300.0f;
                    }

                    const float badgeRadius = 9.5f * scale;
                    ComPtr<ID2D1SolidColorBrush> badgeShadow, badgeBg, badgeBorder, badgeDot;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.11f * alphaMultiplier), badgeShadow.GetAddressOf());
                    D2D1_COLOR_F bgCol = CaptureHudPalette::kSizeHudBg;
                    bgCol.a *= alphaMultiplier;
                    D2D1_COLOR_F bdCol = CaptureHudPalette::kSizeHudBorder;
                    bdCol.a *= alphaMultiplier;
                    D2D1_COLOR_F dotCol = CaptureHudPalette::kShortcutDotAccent;
                    dotCol.a *= alphaMultiplier;
                    m_renderTarget->CreateSolidColorBrush(bgCol, badgeBg.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(bdCol, badgeBorder.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(dotCol, badgeDot.GetAddressOf());

                    if (badgeShadow) {
                        D2D1_RECT_F sh = D2D1::RectF(badgeRect.left, badgeRect.top + 2.0f * scale, badgeRect.right, badgeRect.bottom + 3.5f * scale);
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sh, badgeRadius, badgeRadius), badgeShadow.Get());
                    }

                    float prevTextOpacity = m_infoTextBrush->GetOpacity();
                    m_infoTextBrush->SetOpacity(prevTextOpacity * alphaMultiplier);

                    if (badgeBg) {
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(badgeRect, badgeRadius, badgeRadius), badgeBg.Get());
                    }
                    if (badgeBorder) {
                        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(badgeRect, badgeRadius, badgeRadius), badgeBorder.Get(), 1.0f * scale);
                    }

                    const float midY = (badgeRect.top + badgeRect.bottom) * 0.5f;
                    if (badgeDot) {
                        m_renderTarget->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(badgeRect.left + padH + 3.5f * scale, midY), 3.6f * scale, 3.6f * scale),
                            badgeDot.Get());
                    }

                    TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    auto textR = D2D1::RectF(badgeRect.left + padH + dotZone, badgeRect.top, badgeRect.right - padH + 4.0f * scale, badgeRect.bottom);
                    m_renderTarget->DrawTextW(state.hierarchyBadgeText.c_str(),
                        static_cast<UINT32>(state.hierarchyBadgeText.size()),
                        m_infoTextFormat.Get(), textR, m_infoTextBrush.Get());

                    m_infoTextBrush->SetOpacity(prevTextOpacity);
                }
            }
        }

        if (state.options.showCrosshair) {
            drawCrosshair(static_cast<float>(state.currentCursor.x),
                          static_cast<float>(state.currentCursor.y),
                          &state);
        }
        // 预选悬停时显示像素级取色放大镜（坐标 + RGB/HEX，按 C 复制）
        drawSelectionLoupe(static_cast<float>(state.currentCursor.x), static_cast<float>(state.currentCursor.y), state);
    }

    // 全阶段实时同步左下角快捷键 HUD 几何避让（含 Selecting 悬停/拖拽与 Selected/Marking 标注阶段）
    updateShortcutHintAvoidance(state);

    // 渲染微晶浮层数值反馈 (Toast)
    drawFloatingToast(state);

    recoverFromDeviceLoss(m_renderTarget->EndDraw());
}

void CaptureRenderer::drawOutlinedText(ID2D1RenderTarget* rt,
                                       const std::wstring& text,
                                       IDWriteTextFormat* format,
                                       const D2D1_RECT_F& layoutRect,
                                       ID2D1Brush* textBrush,
                                       ID2D1Brush* outlineBrush,
                                       float strokeWidth) {
    if (!rt || !format || text.empty()) return;

    if (outlineBrush && strokeWidth > 0.0f) {
        // 8 方向偏移微晶外扩描边
        const float offsets[8][2] = {
            {-strokeWidth, 0.0f},
            { strokeWidth, 0.0f},
            {0.0f, -strokeWidth},
            {0.0f,  strokeWidth},
            {-strokeWidth * 0.7071f, -strokeWidth * 0.7071f},
            {-strokeWidth * 0.7071f,  strokeWidth * 0.7071f},
            { strokeWidth * 0.7071f, -strokeWidth * 0.7071f},
            { strokeWidth * 0.7071f,  strokeWidth * 0.7071f}
        };
        for (const auto& off : offsets) {
            D2D1_RECT_F oRect = layoutRect;
            oRect.left += off[0];
            oRect.right += off[0];
            oRect.top += off[1];
            oRect.bottom += off[1];
            rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, oRect, outlineBrush);
        }
    }

    if (textBrush) {
        rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, layoutRect, textBrush);
    }
}

} // namespace tools3000::capture
