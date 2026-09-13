#define RENDER_TIMER_ID 1
#include <algorithm>
#include <windowsx.h>
#include "capture/CaptureInput.h"
#include "capture/CaptureHistory.h"
#include "capture/ClipboardUtils.h"
#include "capture/PinWindow.h"
#include "capture/ScrollCapture.h"
#include "capture/ScrollCaptureOverlay.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/CornerRadiusHelper.h"
#include "capture/CaptureToolbarLayout.h"
#include "capture/CaptureToolbarAccessibility.h"
#include "capture/CaptureDropdownMenu.h"
#include "core/config/ConfigManager.h"
#include "core/accessibility/OverlayAnnouncement.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/WinUtils.h"
#include "core/events/EventBus.h"
#include <windows.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <imm.h>
#include <array>
#include <cwchar>
#include <cwctype>
#include <cmath>
#include <filesystem>

namespace tools3000::capture {

namespace {

float getTargetAspectRatio(AspectRatioPreset preset) {
    switch (preset) {
        case AspectRatioPreset::Ratio_16_9: return 16.0f / 9.0f;
        case AspectRatioPreset::Ratio_4_3:  return 4.0f / 3.0f;
        case AspectRatioPreset::Ratio_1_1:  return 1.0f;
        case AspectRatioPreset::Ratio_Golden: return 1.61803398875f;
        default: return 0.0f;
    }
}

std::string formatColorText(ColorFormatType type, int r, int g, int b) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float maxVal = std::max({rf, gf, bf}), minVal = std::min({rf, gf, bf});
    float delta = maxVal - minVal;

    switch (type) {
        case ColorFormatType::HEX:
            return std::format("#{:02X}{:02X}{:02X}", r, g, b);
        case ColorFormatType::RGB:
            return std::format("rgb({}, {}, {})", r, g, b);
        case ColorFormatType::RGBA:
            return std::format("rgba({}, {}, {}, 1.0)", r, g, b);
        case ColorFormatType::HEX_0x:
            return std::format("0x{:02X}{:02X}{:02X}", r, g, b);
        case ColorFormatType::HSL: {
            float lf = (maxVal + minVal) * 0.5f;
            float sf = 0.0f, hf = 0.0f;
            if (delta > 1e-5f) {
                sf = lf > 0.5f ? delta / (2.0f - maxVal - minVal) : delta / (maxVal + minVal);
                if (maxVal == rf) hf = (gf - bf) / delta + (gf < bf ? 6.0f : 0.0f);
                else if (maxVal == gf) hf = (bf - rf) / delta + 2.0f;
                else hf = (rf - gf) / delta + 4.0f;
                hf /= 6.0f;
            }
            int h = static_cast<int>(std::round(hf * 360.0f)) % 360;
            int s = static_cast<int>(std::round(sf * 100.0f));
            int l = static_cast<int>(std::round(lf * 100.0f));
            return std::format("hsl({}, {}%, {}%)", h, s, l);
        }
        case ColorFormatType::HSV: {
            float vf = maxVal;
            float sf = maxVal > 1e-5f ? delta / maxVal : 0.0f;
            float hf = 0.0f;
            if (delta > 1e-5f) {
                if (maxVal == rf) hf = (gf - bf) / delta + (gf < bf ? 6.0f : 0.0f);
                else if (maxVal == gf) hf = (bf - rf) / delta + 2.0f;
                else hf = (rf - gf) / delta + 4.0f;
                hf /= 6.0f;
            }
            int h = static_cast<int>(std::round(hf * 360.0f)) % 360;
            int s = static_cast<int>(std::round(sf * 100.0f));
            int v = static_cast<int>(std::round(vf * 100.0f));
            return std::format("hsv({}, {}%, {}%)", h, s, v);
        }
        case ColorFormatType::CMYK: {
            float kf = 1.0f - std::max({rf, gf, bf});
            int c = 0, m = 0, y = 0, k = static_cast<int>(std::round(kf * 100.0f));
            if (kf < 1.0f - 1e-5f) {
                c = static_cast<int>(std::round((1.0f - rf - kf) / (1.0f - kf) * 100.0f));
                m = static_cast<int>(std::round((1.0f - gf - kf) / (1.0f - kf) * 100.0f));
                y = static_cast<int>(std::round((1.0f - bf - kf) / (1.0f - kf) * 100.0f));
            }
            return std::format("cmyk({}%, {}%, {}%, {}%)", c, m, y, k);
        }
        case ColorFormatType::DEC: {
            uint32_t decVal = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
            return std::format("{}", decVal);
        }
        default:
            return std::format("#{:02X}{:02X}{:02X}", r, g, b);
    }
}

} // namespace

HCURSOR CaptureInput::getBeautifulCrosshairCursor(float dpiScale) {
    int scaleBucket = std::clamp(static_cast<int>(std::round(dpiScale * 100.0f)), 100, 400);
    static std::unordered_map<int, HCURSOR> s_cursors;
    auto it = s_cursors.find(scaleBucket);
    if (it != s_cursors.end() && it->second) return it->second;

    const float effScale = scaleBucket / 100.0f;
    const int rawSz = std::clamp(static_cast<int>(std::round(48.0f * effScale)), 48, 128);
    const int sz = (rawSz / 2) * 2;
    const int mid = sz / 2;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sz;
    bmi.bmiHeader.biHeight = -sz; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t* pixels = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hbmColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    ReleaseDC(nullptr, hdc);

    if (!hbmColor || !pixels) {
        HCURSOR fallback = LoadCursor(nullptr, IDC_CROSS);
        s_cursors[scaleBucket] = fallback;
        return fallback;
    }

    // 初始化为完全透明 (0x00000000)
    std::fill_n(pixels, sz * sz, 0x00000000);

    auto setPixelARGB = [&](int x, int y, uint32_t argb) {
        if (x >= 0 && x < sz && y >= 0 && y < sz) {
            pixels[y * sz + x] = argb;
        }
    };

    const uint32_t COLOR_WHITE = 0xFFFFFFFF; // 100% 不透明纯白
    const uint32_t COLOR_BLACK = 0xFF000000; // 100% 不透明纯黑

    // 精致修长准星 (纤细美学)，随 DPI 缩放比例自适应臂长
    const int startDist = std::max(3, static_cast<int>(std::round(3.0f * effScale)));
    const int endDist = std::clamp(static_cast<int>(std::round(18.0f * effScale)), startDist + 6, mid - 3);

    // 水平臂 (左、右)
    for (int dist = startDist; dist <= endDist; ++dist) {
        int xs[2] = { mid - dist, mid + dist };
        for (int x : xs) {
            // 1px 纯白内芯
            setPixelARGB(x, mid, COLOR_WHITE);
            // 上下 1px 细黑描边
            setPixelARGB(x, mid - 1, COLOR_BLACK);
            setPixelARGB(x, mid + 1, COLOR_BLACK);
        }
    }

    // 垂直臂 (上、下)
    for (int dist = startDist; dist <= endDist; ++dist) {
        int ys[2] = { mid - dist, mid + dist };
        for (int y : ys) {
            // 1px 纯白内芯
            setPixelARGB(mid, y, COLOR_WHITE);
            // 左右 1px 细黑描边
            setPixelARGB(mid - 1, y, COLOR_BLACK);
            setPixelARGB(mid + 1, y, COLOR_BLACK);
        }
    }

    // 四个端点外侧封口黑边
    setPixelARGB(mid - (endDist + 1), mid, COLOR_BLACK);
    setPixelARGB(mid + (endDist + 1), mid, COLOR_BLACK);
    setPixelARGB(mid, mid - (endDist + 1), COLOR_BLACK);
    setPixelARGB(mid, mid + (endDist + 1), COLOR_BLACK);

    // 中心微孔 4 个内端封口黑边
    setPixelARGB(mid - (startDist - 1), mid, COLOR_BLACK);
    setPixelARGB(mid + (startDist - 1), mid, COLOR_BLACK);
    setPixelARGB(mid, mid - (startDist - 1), COLOR_BLACK);
    setPixelARGB(mid, mid + (startDist - 1), COLOR_BLACK);

    // 针对 32-bit ARGB 光标创建 1-bit 单色掩码
    // Windows GDI 对 32位 ARGB 图标的 AND 掩码要求：
    // 行步长按 2 字节（WORD, 16-bit）向上对齐，初始全部设为 0xFF（即透明）
    // 对于 alpha > 0 的不透明像素，对应位清零（0 表示采用彩色层 alpha 混合）
    const int maskStride = ((sz + 15) / 16) * 2;
    std::vector<uint8_t> maskBits(static_cast<size_t>(maskStride) * sz, 0xFF);
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            if ((pixels[y * sz + x] & 0xFF000000u) != 0) {
                maskBits[static_cast<size_t>(y) * maskStride + (x / 8)] &= static_cast<uint8_t>(~(0x80 >> (x % 8)));
            }
        }
    }

    HBITMAP hbmMask = CreateBitmap(sz, sz, 1, 1, maskBits.data());

    ICONINFO ii = {};
    ii.fIcon = FALSE;
    ii.xHotspot = mid;
    ii.yHotspot = mid;
    ii.hbmMask = hbmMask;
    ii.hbmColor = hbmColor;

    HCURSOR cursor = CreateIconIndirect(&ii);

    if (hbmMask) DeleteObject(hbmMask);
    if (hbmColor) DeleteObject(hbmColor);

    if (!cursor) cursor = LoadCursor(nullptr, IDC_CROSS);
    s_cursors[scaleBucket] = cursor;
    return cursor;
}

HCURSOR CaptureInput::getCornerRadiusLiveCursor(int cornerIndex) {
    static HCURSOR s_cursorNWSE = nullptr;
    static HCURSOR s_cursorNESW = nullptr;
    bool isNesw = (cornerIndex == 1 || cornerIndex == 3);
    if (isNesw && s_cursorNESW) return s_cursorNESW;
    if (!isNesw && s_cursorNWSE) return s_cursorNWSE;

    const int sz = 32;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz; // Top-down DIB
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    uint32_t* pixels = nullptr;
    HDC hdcScreen = GetDC(nullptr);
    HBITMAP hbmColor = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);

    if (!hbmColor || !pixels) {
        HCURSOR fallback = LoadCursor(nullptr, isNesw ? IDC_SIZENESW : IDC_SIZENWSE);
        if (isNesw) s_cursorNESW = fallback;
        else s_cursorNWSE = fallback;
        return fallback;
    }

    std::memset(pixels, 0, sz * sz * sizeof(uint32_t));

    auto setPixel = [&](int x, int y, uint32_t color) {
        if (x >= 0 && x < sz && y >= 0 && y < sz) {
            pixels[y * sz + x] = color;
        }
    };

    constexpr uint32_t C_DARK  = 0xFF0F172A; // 极客深曜黑
    constexpr uint32_t C_WHITE = 0xFFFFFFFF; // 纯白高反差轮廓
    constexpr uint32_t C_BLUE  = 0xFF0284C7; // 科技蓝
    constexpr uint32_t C_CYAN  = 0xFF38BDF8; // 发光青天蓝

    if (!isNesw) {
        // 1. 绘制主对角线 NW-SE 双向箭头 (从 6,6 到 26,26)
        for (int i = 6; i <= 26; ++i) {
            setPixel(i, i, C_DARK);
            setPixel(i - 1, i, C_WHITE);
            setPixel(i + 1, i, C_WHITE);
            setPixel(i, i - 1, C_WHITE);
            setPixel(i, i + 1, C_WHITE);
        }
        for (int i = 6; i <= 26; ++i) {
            setPixel(i, i, C_DARK);
        }

        // A. 西北向大箭头 (↖ 指向外侧直角方向)
        for (int k = 0; k <= 5; ++k) {
            setPixel(5 + k, 5, C_DARK);
            setPixel(5, 5 + k, C_DARK);
            setPixel(5 + k, 4, C_WHITE);
            setPixel(4, 5 + k, C_WHITE);
            setPixel(5 + k, 6, C_WHITE);
            setPixel(6, 5 + k, C_WHITE);
        }
        setPixel(5, 5, C_DARK);

        // B. 东南向大箭头 (↘ 指向内侧圆角方向)
        for (int k = 0; k <= 5; ++k) {
            setPixel(27 - k, 27, C_DARK);
            setPixel(27, 27 - k, C_DARK);
            setPixel(27 - k, 28, C_WHITE);
            setPixel(28, 27 - k, C_WHITE);
            setPixel(27 - k, 26, C_WHITE);
            setPixel(26, 27 - k, C_WHITE);
        }
        setPixel(27, 27, C_DARK);

        // 2. 绘制中央悬浮 Live Corner 90° 倒角发光圆弧
        int cx = 21, cy = 21, r = 10;
        for (int deg = 180; deg <= 270; ++deg) {
            double rad = deg * 3.14159265 / 180.0;
            int ax = cx + static_cast<int>(std::round(r * std::cos(rad)));
            int ay = cy + static_cast<int>(std::round(r * std::sin(rad)));
            setPixel(ax, ay, C_CYAN);
            setPixel(ax + 1, ay, C_BLUE);
            setPixel(ax, ay + 1, C_BLUE);
        }
    } else {
        // 1. 绘制反对角线 NE-SW 双向箭头 (从 26,6 到 6,26)
        for (int i = 6; i <= 26; ++i) {
            int x = 32 - i;
            int y = i;
            setPixel(x, y, C_DARK);
            setPixel(x - 1, y, C_WHITE);
            setPixel(x + 1, y, C_WHITE);
            setPixel(x, y - 1, C_WHITE);
            setPixel(x, y + 1, C_WHITE);
        }
        for (int i = 6; i <= 26; ++i) {
            int x = 32 - i;
            int y = i;
            setPixel(x, y, C_DARK);
        }

        // A. 东北向大箭头 (↗ 指向外侧直角方向)
        for (int k = 0; k <= 5; ++k) {
            setPixel(27 - k, 5, C_DARK);
            setPixel(27, 5 + k, C_DARK);
            setPixel(27 - k, 4, C_WHITE);
            setPixel(28, 5 + k, C_WHITE);
            setPixel(27 - k, 6, C_WHITE);
            setPixel(26, 5 + k, C_WHITE);
        }
        setPixel(27, 5, C_DARK);

        // B. 西南向大箭头 (↙ 指向内侧圆角方向)
        for (int k = 0; k <= 5; ++k) {
            setPixel(5 + k, 27, C_DARK);
            setPixel(5, 27 - k, C_DARK);
            setPixel(5 + k, 28, C_WHITE);
            setPixel(4, 27 - k, C_WHITE);
            setPixel(5 + k, 26, C_WHITE);
            setPixel(6, 27 - k, C_WHITE);
        }
        setPixel(5, 27, C_DARK);

        // 2. 绘制中央悬浮 Live Corner 90° 倒角发光圆弧
        int cx = 11, cy = 21, r = 10;
        for (int deg = 270; deg <= 360; ++deg) {
            double rad = deg * 3.14159265 / 180.0;
            int ax = cx + static_cast<int>(std::round(r * std::cos(rad)));
            int ay = cy + static_cast<int>(std::round(r * std::sin(rad)));
            setPixel(ax, ay, C_CYAN);
            setPixel(ax - 1, ay, C_BLUE);
            setPixel(ax, ay + 1, C_BLUE);
        }
    }

    // 3. 绘制中心微瞄准黑曜石微圆点 (白环 + 黑心)
    setPixel(15, 16, C_WHITE); setPixel(16, 15, C_WHITE);
    setPixel(17, 16, C_WHITE); setPixel(16, 17, C_WHITE);
    setPixel(16, 16, C_DARK);

    const int maskStride = ((sz + 15) / 16) * 2;
    std::vector<uint8_t> maskBits(static_cast<size_t>(maskStride) * sz, 0xFF);
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            if ((pixels[y * sz + x] & 0xFF000000u) != 0) {
                maskBits[static_cast<size_t>(y) * maskStride + (x / 8)] &= static_cast<uint8_t>(~(0x80 >> (x % 8)));
            }
        }
    }
    HBITMAP hbmMask = CreateBitmap(sz, sz, 1, 1, maskBits.data());
    ICONINFO ii = {};
    ii.fIcon = FALSE;
    ii.xHotspot = 16;
    ii.yHotspot = 16;
    ii.hbmMask = hbmMask;
    ii.hbmColor = hbmColor;

    HCURSOR cur = CreateIconIndirect(&ii);
    if (hbmMask) DeleteObject(hbmMask);
    if (hbmColor) DeleteObject(hbmColor);

    if (!cur) cur = LoadCursor(nullptr, isNesw ? IDC_SIZENESW : IDC_SIZENWSE);
    if (isNesw) s_cursorNESW = cur;
    else s_cursorNWSE = cur;
    return cur;
}

static HCURSOR cursorForArea(HitArea area, int cornerIndex = 0) {
    switch (area) {
        case HitArea::LT:
        case HitArea::RB:
            return LoadCursor(nullptr, IDC_SIZENWSE);
        case HitArea::RT:
        case HitArea::LB:
            return LoadCursor(nullptr, IDC_SIZENESW);
        case HitArea::T:
        case HitArea::B:
            return LoadCursor(nullptr, IDC_SIZENS);
        case HitArea::L:
        case HitArea::R:
            return LoadCursor(nullptr, IDC_SIZEWE);
        case HitArea::Body:
            return LoadCursor(nullptr, IDC_SIZEALL);
        case HitArea::CornerRadius:
            return CaptureInput::getCornerRadiusLiveCursor(cornerIndex);
        default:
            return LoadCursor(nullptr, IDC_ARROW);
    }
}

static DWORD filterIndexForFormat(ImageFormat format) {
    switch (format) {
        case ImageFormat::JPEG: return 2;
        case ImageFormat::BMP: return 3;
        case ImageFormat::WebP: return 4;
        default: return 1;
    }
}

static const wchar_t* extensionForFormat(ImageFormat format) {
    switch (format) {
        case ImageFormat::JPEG: return L"jpg";
        case ImageFormat::BMP: return L"bmp";
        case ImageFormat::WebP: return L"webp";
        default: return L"png";
    }
}

static ImageFormat formatForSavePath(const std::filesystem::path& path, DWORD filterIndex) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension == L".jpg" || extension == L".jpeg") return ImageFormat::JPEG;
    if (extension == L".bmp") return ImageFormat::BMP;
    if (extension == L".webp") return ImageFormat::WebP;
    if (extension == L".png") return ImageFormat::PNG;
    switch (filterIndex) {
        case 2: return ImageFormat::JPEG;
        case 3: return ImageFormat::BMP;
        case 4: return ImageFormat::WebP;
        default: return ImageFormat::PNG;
    }
}


void CaptureInput::initialize(HWND hwnd, CaptureState& state, CaptureRenderer& renderer,
                              std::function<void()> cancelCb,
                              std::function<void(CaptureCompletion)> confirmCb) {
    m_hwnd = hwnd;
    m_state = &state;
    m_renderer = &renderer;
    m_cancelCb = std::move(cancelCb);
    m_confirmCb = std::move(confirmCb);
}

void CaptureInput::ensureCursorVisible() {
    CURSORINFO ci{sizeof(CURSORINFO)};
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) == 0) {
        while (ShowCursor(TRUE) < 0) {}
    }
}

void CaptureInput::updateHoverCursor(POINT point) {
    HCURSOR cur = nullptr;
    auto st = m_state->state.load();
    if ((int)st == (int)OverlayState::Selected || (int)st == (int)OverlayState::Marking) {
        // ========== Level 1: 进行中的交互拖拽操作 (最高优先级) ==========
        if (m_state->isAdjustingCornerRadius) {
            cur = getCornerRadiusLiveCursor(m_state->cornerDragIndex);
        } else if (m_state->isAdjustingSelection) {
            cur = cursorForArea(m_state->selAdjustHandle);
        } else if (m_state->isManipulating) {
            if (m_state->dragHandle == HitArea::CornerRadius) {
                cur = getCornerRadiusLiveCursor(m_state->cornerDragIndex);
            } else if (m_state->dragHandle != HitArea::None) {
                cur = cursorForArea(m_state->dragHandle, m_state->cornerDragIndex);
            } else {
                cur = LoadCursor(nullptr, IDC_SIZEALL);
            }
        } else if (m_state->sliderPopup.isDragging) {
            cur = LoadCursor(nullptr, IDC_ARROW);
        }

        // ========== Level 2: UI 浮层与控制面板 (工具栏/滑块/下拉菜单/尺寸菜单/二维码) ==========
        if (!cur) {
            if (hitTestToolbar(point)) {
                cur = LoadCursor(nullptr, IDC_ARROW);
            } else if (m_state->sliderPopup.type != SliderPopupType::None) {
                auto pr = m_state->sliderPopup.popupRect;
                if (point.x >= pr.left && point.x <= pr.right && point.y >= pr.top && point.y <= pr.bottom) {
                    cur = LoadCursor(nullptr, IDC_ARROW);
                }
            } else if (m_state->dropdownMenu.type != DropdownType::None) {
                for (const auto& item : m_state->dropdownMenu.items) {
                    if (point.x >= item.rect.left && point.x <= item.rect.right &&
                        point.y >= item.rect.top && point.y <= item.rect.bottom) {
                        cur = LoadCursor(nullptr, IDC_ARROW);
                        break;
                    }
                }
            } else if (m_state->isSizeMenuOpen) {
                auto mr = m_state->sizeMenuRect;
                if (point.x >= mr.left && point.x <= mr.right && point.y >= mr.top && point.y <= mr.bottom) {
                    cur = LoadCursor(nullptr, IDC_ARROW);
                }
            } else if (!m_state->detectedQrText.empty() && m_state->qrChipRect.right > m_state->qrChipRect.left) {
                if (point.x >= m_state->qrChipRect.left && point.x <= m_state->qrChipRect.right &&
                    point.y >= m_state->qrChipRect.top && point.y <= m_state->qrChipRect.bottom) {
                    cur = LoadCursor(nullptr, IDC_ARROW);
                }
            }
        }

        // ========== Level 3: 选区外框手柄 (四角圆角微晶手柄 + 8 方向拉伸控制点) ==========
        // 关键纠偏：无论当前是否处于 Text 文本标注工具模式，只要悬停在四角圆角手柄或 8 处外框控制点上，
        // 必须优先显示对应的圆角/拉伸光标，绝不能被 IBEAM 吞噬或掩盖！
        if (!cur) {
            int cornerIdx = -1;
            HitArea sel = hitTestSelectionBox(point, &cornerIdx);
            if (sel == HitArea::CornerRadius) {
                cur = getCornerRadiusLiveCursor(cornerIdx);
            } else if (sel != HitArea::None && sel != HitArea::Body) {
                cur = cursorForArea(sel, cornerIdx);
            } else if (sel == HitArea::Body) {
                // 选区边框带 (6px)：在录屏选区模式下为整体移动；在绘制工具下选区内部优先绘制
                if (m_state->mode == OverlayMode::RecordRegion) {
                    cur = LoadCursor(nullptr, IDC_SIZEALL);
                } else if (!isPointInSelection(point)) {
                    cur = cursorForArea(HitArea::Body);
                }
            }
        }

        // ========== Level 4: 画布上已选中的激活元素或悬停元素 ==========
        if (!cur) {
            const bool inSelection = isPointInSelection(point);
            if (inSelection) {
                cv::Point local = toMarkupPoint(point);
                if (m_state->activeElement && m_state->activeElement->isActive) {
                    HitArea elemHit = m_state->activeElement->hitTestEx(local);
                    if (elemHit != HitArea::None) {
                        int cornerIdx = 0;
                        if (elemHit == HitArea::CornerRadius && m_state->activeElement->tool == MarkupTool::Rectangle) {
                            int x1 = (std::min)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                            int y1 = (std::min)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                            int x2 = (std::max)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                            int y2 = (std::max)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                            float w = static_cast<float>(x2 - x1);
                            float h = static_cast<float>(y2 - y1);
                            bool isSmall = (w < 80.0f || h < 80.0f);
                            int hitIdx = CornerRadiusHelper::hitTestHandles(
                                static_cast<float>(x1), static_cast<float>(y1),
                                static_cast<float>(x2), static_cast<float>(y2),
                                m_state->activeElement->cornerRadius,
                                static_cast<float>(local.x), static_cast<float>(local.y),
                                1.0f, 9.0f, isSmall);
                            if (hitIdx >= 0) cornerIdx = hitIdx;
                        }
                        if (elemHit == HitArea::CornerRadius) {
                            cur = getCornerRadiusLiveCursor(cornerIdx);
                        } else if (elemHit == HitArea::Body) {
                            if (m_state->activeElement->tool == MarkupTool::Text) {
                                cur = LoadCursor(nullptr, IDC_IBEAM);
                            } else {
                                cur = cursorForArea(HitArea::Body);
                            }
                        } else {
                            cur = cursorForArea(elemHit, cornerIdx);
                        }
                    }
                }

                if (!cur) {
                    HitResult hit = m_state->markup.getElementAtEx(local);
                    if (hit.element) {
                        if (hit.element->tool == MarkupTool::Text) {
                            cur = LoadCursor(nullptr, IDC_IBEAM);
                        } else {
                            cur = cursorForArea(hit.area);
                        }
                    }
                }
            }
        }

        // ========== Level 5: 选区内部画布 (当前标注工具分发) ==========
        if (!cur) {
            const float effDpi = (m_state && m_state->dpiScale > 0.0f) ? m_state->dpiScale : 1.0f;
            if (isPointInSelection(point)) {
                if (m_state->mode == OverlayMode::RecordRegion) {
                    cur = LoadCursor(nullptr, IDC_SIZEALL);
                } else if (m_state->currentTool == MarkupTool::Text) {
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                        cur = LoadCursor(nullptr, IDC_ARROW);
                    } else {
                        cur = LoadCursor(nullptr, IDC_IBEAM);
                    }
                } else {
                    cur = getBeautifulCrosshairCursor(effDpi);
                }
            } else {
                // ========== Level 6: 选区外部区域 (彻底杜绝十字准星泄漏) ==========
                cur = LoadCursor(nullptr, IDC_ARROW);
            }
        }
    }

    // ========== Level 7: 未建立选区初始态或兜底 ==========
    if (!cur) {
        const float effDpi = (m_state && m_state->dpiScale > 0.0f) ? m_state->dpiScale : 1.0f;
        if ((int)st == (int)OverlayState::Selecting) {
            cur = getBeautifulCrosshairCursor(effDpi);
        } else if ((int)st == (int)OverlayState::Idle) {
            cur = (m_state->mode == OverlayMode::RecordRegion)
                ? LoadCursor(nullptr, IDC_ARROW)
                : getBeautifulCrosshairCursor(effDpi);
        } else {
            cur = LoadCursor(nullptr, IDC_ARROW);
        }
    }

    if (!cur) {
        cur = LoadCursor(nullptr, IDC_ARROW);
    }
    ensureCursorVisible();
    SetCursor(cur);
}

HitArea CaptureInput::hitTestSelectionBox(POINT point, int* outCornerIndex) const {
    if (outCornerIndex) *outCornerIndex = -1;
    auto r = currentSelectionRect();
    if (r.right - r.left < 1.0f || r.bottom - r.top < 1.0f) return HitArea::None;

    const float dpiScale = std::clamp(
        m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
    const float hw = 9.0f * dpiScale;   // 控制点命中半径
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;

    struct H { float x, y; HitArea area; };
    const H handles[8] = {
        {r.left,  r.top,    HitArea::LT}, {cx,      r.top,    HitArea::T},
        {r.right, r.top,    HitArea::RT}, {r.right, cy,       HitArea::R},
        {r.right, r.bottom, HitArea::RB}, {cx,      r.bottom, HitArea::B},
        {r.left,  r.bottom, HitArea::LB}, {r.left,  cy,       HitArea::L},
    };
    float px = static_cast<float>(point.x);
    float py = static_cast<float>(point.y);

    // 1. 优先检测 4 个内角圆角手柄 (只要满足展示手柄尺寸且非录屏模式，左上/右上/右下/左下均可命中)
    float selW = r.right - r.left;
    float selH = r.bottom - r.top;
    if (m_state->mode != OverlayMode::RecordRegion &&
        CornerRadiusHelper::canShowHandles(selW, selH, dpiScale, 64.0f)) {
        float effectiveRadius = m_state->effectiveCornerRadius();
        int hitIdx = CornerRadiusHelper::hitTestHandles(
            r.left, r.top, r.right, r.bottom,
            effectiveRadius, px, py, dpiScale, 11.0f * dpiScale, false);
        if (hitIdx >= 0) {
            if (outCornerIndex) *outCornerIndex = hitIdx;
            return HitArea::CornerRadius;
        }
    }

    // 2. 检测 8 个外框拉伸调整控制点
    for (const auto& h : handles) {
        if (std::abs(px - h.x) <= hw && std::abs(py - h.y) <= hw) return h.area;
    }

    // 边框带（用于整体移动）：靠近任意一条边即可拖动整块选区
    const float band = 6.0f * dpiScale;
    bool insideX = px >= r.left - band && px <= r.right + band;
    bool insideY = py >= r.top - band && py <= r.bottom + band;
    bool nearLeft   = std::abs(px - r.left)   <= band && insideY;
    bool nearRight  = std::abs(px - r.right)  <= band && insideY;
    bool nearTop    = std::abs(py - r.top)    <= band && insideX;
    bool nearBottom = std::abs(py - r.bottom) <= band && insideX;
    if (nearLeft || nearRight || nearTop || nearBottom) return HitArea::Body;
    return HitArea::None;
}

void CaptureInput::adjustSelection(HitArea handle, int dx, int dy) {
    // 记录调整前的选区左上角，用于已有标注的坐标重映射
    int oldLeft = std::min(m_state->dragStart.x, m_state->dragEnd.x);
    int oldTop  = std::min(m_state->dragStart.y, m_state->dragEnd.y);

    float l = static_cast<float>(std::min(m_state->dragStart.x, m_state->dragEnd.x));
    float t = static_cast<float>(std::min(m_state->dragStart.y, m_state->dragEnd.y));
    float r = static_cast<float>(std::max(m_state->dragStart.x, m_state->dragEnd.x));
    float b = static_cast<float>(std::max(m_state->dragStart.y, m_state->dragEnd.y));

    if (handle == HitArea::Body) {
        l += dx; r += dx; t += dy; b += dy;
    } else {
        switch (handle) {
            case HitArea::LT: l += dx; t += dy; break;
            case HitArea::T:  t += dy; break;
            case HitArea::RT: r += dx; t += dy; break;
            case HitArea::R:  r += dx; break;
            case HitArea::RB: r += dx; b += dy; break;
            case HitArea::B:  b += dy; break;
            case HitArea::LB: l += dx; b += dy; break;
            case HitArea::L:  l += dx; break;
            default: break;
        }
    }

    float maxW = 1.0e6f, maxH = 1.0e6f;
    if (m_renderer->getRenderTarget()) {
        auto s = m_renderer->getRenderTarget()->GetSize();
        maxW = s.width; maxH = s.height;
    }

    if (handle == HitArea::Body) {
        // 移动：整体夹取到屏幕内，保持尺寸不变
        float w = r - l, h = b - t;
        if (l < 0)     { l = 0;        r = w; }
        if (t < 0)     { t = 0;        b = h; }
        if (r > maxW)  { r = maxW;     l = maxW - w; }
        if (b > maxH)  { b = maxH;     t = maxH - h; }
    } else {
        // 缩放：若锁定固定比例，按比例联动
        float targetRatio = getTargetAspectRatio(m_state->aspectRatio);
        if (targetRatio > 0.0f) {
            float w = r - l;
            float h = b - t;
            bool widthDriven = (handle == HitArea::L || handle == HitArea::R);
            bool heightDriven = (handle == HitArea::T || handle == HitArea::B);
            if (widthDriven) {
                float newH = w / targetRatio;
                float dH = newH - h;
                t -= dH * 0.5f;
                b += dH * 0.5f;
            } else if (heightDriven) {
                float newW = h * targetRatio;
                float dW = newW - w;
                l -= dW * 0.5f;
                r += dW * 0.5f;
            } else {
                float newH = w / targetRatio;
                if (handle == HitArea::LT || handle == HitArea::RT) {
                    t = b - newH;
                } else {
                    b = t + newH;
                }
            }
        }

        // 缩放：夹取边界并保证最小尺寸
        l = std::clamp(l, 0.0f, maxW); r = std::clamp(r, 0.0f, maxW);
        t = std::clamp(t, 0.0f, maxH); b = std::clamp(b, 0.0f, maxH);
        const float minSize = 8.0f;
        if (r - l < minSize) {
            if (handle == HitArea::L || handle == HitArea::LT || handle == HitArea::LB) l = r - minSize;
            else r = l + minSize;
        }
        if (b - t < minSize) {
            if (handle == HitArea::T || handle == HitArea::LT || handle == HitArea::RT) t = b - minSize;
            else b = t + minSize;
        }
    }

    m_state->dragStart = { static_cast<LONG>(l), static_cast<LONG>(t) };
    m_state->dragEnd   = { static_cast<LONG>(r), static_cast<LONG>(b) };

    int newLeft = std::min(m_state->dragStart.x, m_state->dragEnd.x);
    int newTop  = std::min(m_state->dragStart.y, m_state->dragEnd.y);

    if (m_state->markup.hasAnyMarkup()) {
        // 已有标注（含撤销栈）：按左上角位移反向平移，使其"钉"在原屏幕内容上；并按新选区重裁底图
        m_state->markup.translateAll(oldLeft - newLeft, oldTop - newTop);
        rebuildMarkupBase();
        m_renderer->markMarkupDirty();
    } else {
        // 无任何标注：底图在下次标注时按新选区再裁（避免每帧重裁）
        m_state->markupBaseReady = false;
    }
    m_renderer->invalidate();
}

void CaptureInput::enforceAspectRatioOnCurrentSelection() {
    float targetRatio = getTargetAspectRatio(m_state->aspectRatio);
    if (targetRatio <= 0.0f) return;

    float l = static_cast<float>(std::min(m_state->dragStart.x, m_state->dragEnd.x));
    float t = static_cast<float>(std::min(m_state->dragStart.y, m_state->dragEnd.y));
    float r = static_cast<float>(std::max(m_state->dragStart.x, m_state->dragEnd.x));
    float b = static_cast<float>(std::max(m_state->dragStart.y, m_state->dragEnd.y));

    float w = r - l;
    float h = b - t;
    if (w < 8.0f || h < 8.0f) return;

    float curRatio = w / h;
    float cx = (l + r) * 0.5f;
    float cy = (t + b) * 0.5f;

    float newW = w;
    float newH = h;
    if (curRatio > targetRatio) {
        newW = h * targetRatio;
    } else {
        newH = w / targetRatio;
    }

    float maxW = 1.0e6f, maxH = 1.0e6f;
    if (m_renderer->getRenderTarget()) {
        auto s = m_renderer->getRenderTarget()->GetSize();
        maxW = s.width; maxH = s.height;
    }

    if (newW > maxW) {
        newW = maxW;
        newH = newW / targetRatio;
    }
    if (newH > maxH) {
        newH = maxH;
        newW = newH * targetRatio;
    }

    l = cx - newW * 0.5f;
    r = cx + newW * 0.5f;
    if (l < 0.0f) {
        r += (0.0f - l);
        l = 0.0f;
    }
    if (r > maxW) {
        l -= (r - maxW);
        r = maxW;
        if (l < 0.0f) l = 0.0f;
    }

    t = cy - newH * 0.5f;
    b = cy + newH * 0.5f;
    if (t < 0.0f) {
        b += (0.0f - t);
        t = 0.0f;
    }
    if (b > maxH) {
        t -= (b - maxH);
        b = maxH;
        if (t < 0.0f) t = 0.0f;
    }

    m_state->dragStart = { static_cast<LONG>(std::round(l)), static_cast<LONG>(std::round(t)) };
    m_state->dragEnd   = { static_cast<LONG>(std::round(r)), static_cast<LONG>(std::round(b)) };
    prepareMarkupBase();
}

ToolbarButton* CaptureInput::hitTestToolbar(POINT point) {
    rebuildToolbarButtons(currentSelectionRect());
    // 1. 如果下拉菜单已打开，优先命中下拉菜单项
    if (m_state->openSubmenu != SubmenuType::None) {
        for (auto& button : m_state->submenuButtons) {
            if (point.x >= button.rect.left && point.x <= button.rect.right &&
                point.y >= button.rect.top && point.y <= button.rect.bottom) {
                return &button;
            }
        }
    }
    // 2. 命中二级属性栏按钮
    for (auto& button : m_state->secondaryToolbarButtons) {
        if (point.x >= button.rect.left && point.x <= button.rect.right &&
            point.y >= button.rect.top && point.y <= button.rect.bottom) {
            return &button;
        }
    }
    // 3. 命中选区侧边浮动菜单按钮 (PixPin 标杆)
    for (auto& button : m_state->selectionSideButtons) {
        if (point.x >= button.rect.left && point.x <= button.rect.right &&
            point.y >= button.rect.top && point.y <= button.rect.bottom) {
            return &button;
        }
    }
    // 4. 命中主工具栏按钮
    for (auto& button : m_state->toolbarButtons) {
        if (point.x >= button.rect.left && point.x <= button.rect.right &&
            point.y >= button.rect.top && point.y <= button.rect.bottom) {
            return &button;
        }
    }
    return nullptr;
}

void CaptureInput::rebuildToolbarButtons(const D2D1_RECT_F& selectionRect) {
    if (!m_renderer->getRenderTarget()) return;
    rebuildCaptureToolbar(*m_state, selectionRect,
                          m_renderer->getRenderTarget()->GetSize());
}

bool CaptureInput::invokeToolbarButton(std::size_t index) {
    if (!m_state || !m_renderer ||
        (m_state->state != OverlayState::Selected && m_state->state != OverlayState::Marking)) {
        return false;
    }
    rebuildToolbarButtons(currentSelectionRect());
    if (index >= m_state->toolbarButtons.size()) return false;

    // Copy before execution: confirm/cancel callbacks may synchronously change
    // the overlay state and invalidate the backing vector.
    const ToolbarButton button = m_state->toolbarButtons[index];
    executeToolbarCommand(button);
    if (m_renderer) m_renderer->invalidate();
    return true;
}

void CaptureInput::executeToolbarCommand(const ToolbarButton& button) {
    // CaptureInput only receives window messages on the overlay UI thread. The
    // announcement therefore never calls UIA/Win32 from a hook or worker
    // thread, and remains invisible to sighted users.
    tools3000::core::accessibility::announceOverlay(
        m_hwnd, toolbarButtonAccessibleName(button));

    // 仅在切换其他工具或执行重置/确认时退出选中态；改颜色/属性或选择相同工具时保留当前激活元素
    if (button.command != ToolbarCommand::SelectColor &&
        button.command != ToolbarCommand::ToggleFill &&
        button.command != ToolbarCommand::ToggleTextOutline &&
        button.command != ToolbarCommand::SelectTextOutlineColor &&
        button.command != ToolbarCommand::CycleStrokeWidth &&
        button.command != ToolbarCommand::CycleElementCornerRadius &&
        button.command != ToolbarCommand::ToggleLineStyleDropdown &&
        button.command != ToolbarCommand::SelectLineStyle &&
        button.command != ToolbarCommand::ToggleArrowStyleDropdown &&
        button.command != ToolbarCommand::SelectArrowStyle &&
        button.command != ToolbarCommand::ToggleNumberShape &&
        button.command != ToolbarCommand::ToggleBeautyShell &&
        button.command != ToolbarCommand::CycleBeautyBg &&
        button.command != ToolbarCommand::CycleBeautyPadding &&
        button.command != ToolbarCommand::SideCycleAspectRatio &&
        button.command != ToolbarCommand::RecordToggleKeycast &&
        button.command != ToolbarCommand::RecordToggleSystemAudio &&
        button.command != ToolbarCommand::RecordToggleMicrophone &&
        button.command != ToolbarCommand::RecordToggleFormat &&
        button.command != ToolbarCommand::RecordCycleFps &&
        button.command != ToolbarCommand::RecordCycleQuality &&
        button.command != ToolbarCommand::RecordCycleClickEffect &&
        button.command != ToolbarCommand::SelectTool) {
        if (m_state->activeElement) {
            m_state->activeElement->isActive = false;
            m_state->activeElement->isEditing = false;
            if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->text.empty()) {
                m_state->markup.removeElement(m_state->activeElement->id);
            }
            m_state->activeElement = nullptr;
        }
    }

    switch (button.command) {
        case ToolbarCommand::SelectTool:
            setCurrentTool(button.tool);
            return;

        case ToolbarCommand::SelectColor:
            m_state->currentColor = button.color;
            if (m_state->activeElement) {
                m_state->activeElement->color = button.color;
                m_renderer->markMarkupDirty();
                m_renderer->invalidate();
            }
            m_state->toolbarLayoutValid = false;
            break;

        case ToolbarCommand::ToggleFill:
            m_state->currentFillMode = !m_state->currentFillMode;
            if (m_state->activeElement) {
                m_state->activeElement->fill = m_state->currentFillMode;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            break;

        case ToolbarCommand::ToggleTextOutline: {
            m_state->currentTextOutline = !m_state->currentTextOutline;
            tools3000::core::ConfigManager::instance().set<bool>(
                "/capture/markup/text/outline", m_state->currentTextOutline);
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                m_state->activeElement->textOutline = m_state->currentTextOutline;
                m_renderer->markMarkupDirty();
            }
            const bool zh = tools3000::core::WinUtils::isSystemLanguageChinese();
            m_state->loupeToastUntil = GetTickCount() + 1500;
            m_state->loupeToastMessage = m_state->currentTextOutline
                ? (zh ? L"文字描边: 已开启" : L"Text Outline: ON")
                : (zh ? L"文字描边: 已关闭" : L"Text Outline: OFF");
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SelectTextOutlineColor: {
            m_state->currentTextOutlineColor = button.color;
            if (button.color.isAuto()) {
                tools3000::core::ConfigManager::instance().set<std::string>(
                    "/capture/markup/text/outlineColor", "auto");
            } else if (button.color == MarkupColor::Black()) {
                tools3000::core::ConfigManager::instance().set<std::string>(
                    "/capture/markup/text/outlineColor", "black");
            } else if (button.color == MarkupColor::White()) {
                tools3000::core::ConfigManager::instance().set<std::string>(
                    "/capture/markup/text/outlineColor", "white");
            }
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                m_state->activeElement->textOutlineColor = button.color;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::CycleStrokeWidth: {
            if (m_state->sliderPopup.type == SliderPopupType::StrokeWidth ||
                m_state->sliderPopup.type == SliderPopupType::MosaicBlockSize ||
                m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                m_state->sliderPopup.type = SliderPopupType::None;
            } else {
                m_state->openSubmenu = SubmenuType::None;
                m_state->sliderPopup.type = (m_state->currentTool == MarkupTool::Mosaic)
                    ? SliderPopupType::MosaicBlockSize
                    : (m_state->currentTool == MarkupTool::Text)
                    ? SliderPopupType::TextFontSize
                    : SliderPopupType::StrokeWidth;
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SideToggleCornerRadius: {
            if (m_state->sliderPopup.type == SliderPopupType::SelectionCornerRadius) {
                m_state->sliderPopup.type = SliderPopupType::None;
            } else {
                m_state->openSubmenu = SubmenuType::None;
                m_state->sliderPopup.type = SliderPopupType::SelectionCornerRadius;
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SideResetSelection: {
            m_state->cornerRadius = 0.0f;
            m_state->sliderPopup.type = SliderPopupType::None;
            m_state->toolbarLayoutValid = false;
            prepareMarkupBase();
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SideInvertSelection: {
            // 反转选区/全屏扩展
            m_state->cornerRadius = 0.0f;
            m_state->toolbarLayoutValid = false;
            prepareMarkupBase();
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::CycleElementCornerRadius: {
            if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                m_state->sliderPopup.type = SliderPopupType::None;
            } else {
                m_state->openSubmenu = SubmenuType::None;
                m_state->sliderPopup.type = SliderPopupType::CornerRadius;
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleLineStyleDropdown:
            m_state->openSubmenu = (m_state->openSubmenu == SubmenuType::LineStyle) ? SubmenuType::None : SubmenuType::LineStyle;
            break;

        case ToolbarCommand::SelectLineStyle:
            m_state->currentLineStyle = button.lineStyleParam;
            m_state->openSubmenu = SubmenuType::None;
            if (m_state->activeElement) {
                m_state->activeElement->lineStyle = m_state->currentLineStyle;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            break;

        case ToolbarCommand::ToggleArrowStyleDropdown:
            m_state->openSubmenu = (m_state->openSubmenu == SubmenuType::ArrowStyle) ? SubmenuType::None : SubmenuType::ArrowStyle;
            break;

        case ToolbarCommand::SelectArrowStyle:
            m_state->currentArrowStyle = button.arrowStyleParam;
            m_state->openSubmenu = SubmenuType::None;
            if (m_state->activeElement) {
                m_state->activeElement->arrowStyle = m_state->currentArrowStyle;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            break;

        case ToolbarCommand::SelectMosaicType:
            m_state->currentMosaicType = button.intParam;
            m_state->openSubmenu = SubmenuType::None;
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleBeautyShell: {
            m_state->beautyShell.enabled = !m_state->beautyShell.enabled;
            m_state->loupeToastUntil = GetTickCount() + 1500;
            m_state->loupeToastMessage = m_state->beautyShell.enabled ? L"✓ 已开启美化外壳模式" : L"✕ 已关闭美化外壳模式";
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::CycleBeautyBg: {
            openDropdownMenu(DropdownType::BeautyShellBg, button.rect);
            break;
        }

        case ToolbarCommand::CycleBeautyPadding: {
            openDropdownMenu(DropdownType::BeautyShellPadding, button.rect);
            break;
        }

        case ToolbarCommand::CycleBeautyRadius: {
            openDropdownMenu(DropdownType::BeautyShellRadius, button.rect);
            break;
        }

        case ToolbarCommand::RecordToggleFormat: {
            openDropdownMenu(DropdownType::RecordFormat, button.rect);
            break;
        }

        case ToolbarCommand::RecordCycleFps: {
            openDropdownMenu(DropdownType::RecordFps, button.rect);
            break;
        }

        case ToolbarCommand::RecordCycleQuality: {
            openDropdownMenu(DropdownType::RecordQuality, button.rect);
            break;
        }

        case ToolbarCommand::RecordCycleClickEffect: {
            openDropdownMenu(DropdownType::RecordClickEffect, button.rect);
            break;
        }

        case ToolbarCommand::RecordToggleKeycast: {
            m_state->recordSetup.includeKeycast = !m_state->recordSetup.includeKeycast;
            tools3000::core::ConfigManager::instance().set<bool>(
                "/recording/includeKeycast", m_state->recordSetup.includeKeycast);
            const bool zh = tools3000::core::WinUtils::isSystemLanguageChinese();
            m_state->loupeToastUntil = GetTickCount() + 1500;
            m_state->loupeToastMessage = m_state->recordSetup.includeKeycast
                ? (zh ? L"按键回显: 已开启" : L"Keycast: ON")
                : (zh ? L"按键回显: 已关闭" : L"Keycast: OFF");
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::RecordToggleSystemAudio: {
            if (m_state->recordSetup.format != RecordFormat::GIF) {
                m_state->recordSetup.captureSystemAudio = !m_state->recordSetup.captureSystemAudio;
                tools3000::core::ConfigManager::instance().set<bool>(
                    "/recording/captureSystemAudio", m_state->recordSetup.captureSystemAudio);
                const bool zh = tools3000::core::WinUtils::isSystemLanguageChinese();
                m_state->loupeToastUntil = GetTickCount() + 1500;
                m_state->loupeToastMessage = m_state->recordSetup.captureSystemAudio
                    ? (zh ? L"系统声音: 已开启" : L"System Audio: ON")
                    : (zh ? L"系统声音: 已关闭" : L"System Audio: OFF");
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
            }
            break;
        }

        case ToolbarCommand::RecordToggleMicrophone: {
            if (m_state->recordSetup.format != RecordFormat::GIF) {
                m_state->recordSetup.captureMicrophone = !m_state->recordSetup.captureMicrophone;
                tools3000::core::ConfigManager::instance().set<bool>(
                    "/recording/captureMicrophone", m_state->recordSetup.captureMicrophone);
                const bool zh = tools3000::core::WinUtils::isSystemLanguageChinese();
                m_state->loupeToastUntil = GetTickCount() + 1500;
                m_state->loupeToastMessage = m_state->recordSetup.captureMicrophone
                    ? (zh ? L"麦克风: 已开启" : L"Microphone: ON")
                    : (zh ? L"麦克风: 已关闭" : L"Microphone: OFF");
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
            }
            break;
        }

        case ToolbarCommand::RecordStartConfirm: {
            if (m_confirmCb) m_confirmCb({});
            break;
        }

        case ToolbarCommand::SideCycleAspectRatio: {
            int cur = static_cast<int>(m_state->aspectRatio);
            int next = (cur + 1) % static_cast<int>(AspectRatioPreset::COUNT);
            m_state->aspectRatio = static_cast<AspectRatioPreset>(next);
            enforceAspectRatioOnCurrentSelection();
            static const wchar_t* ratioNames[] = { L"自由比例", L"16:9 宽屏", L"4:3 标清", L"1:1 正方形", L"黄金比例 (1.618:1)" };
            std::wstring rname = (next >= 0 && next < 5) ? ratioNames[next] : L"自由比例";
            m_state->loupeToastUntil = GetTickCount() + 1500;
            m_state->loupeToastMessage = L"选区比例: " + rname;
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleNumberShape: {
            if (m_state->currentNumberShape == NumberBadgeShape::Circle) {
                m_state->currentNumberShape = NumberBadgeShape::RoundedSquare;
            } else {
                m_state->currentNumberShape = NumberBadgeShape::Circle;
            }
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number) {
                m_state->activeElement->numberShape = m_state->currentNumberShape;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::Undo:
            if (m_state->activeElement) {
                m_state->activeElement->isActive = false;
                m_state->activeElement->isEditing = false;
                m_state->activeElement = nullptr;
            }
            m_state->markup.undo();
            m_renderer->markMarkupDirty();
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::Redo:
            if (m_state->activeElement) {
                m_state->activeElement->isActive = false;
                m_state->activeElement->isEditing = false;
                m_state->activeElement = nullptr;
            }
            m_state->markup.redo();
            m_renderer->markMarkupDirty();
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::Clear:
            if (m_state->activeElement) {
                m_state->activeElement->isActive = false;
                m_state->activeElement->isEditing = false;
                m_state->activeElement = nullptr;
            }
            m_state->markup.clearAll();
            m_renderer->markMarkupDirty();
            m_state->toolbarLayoutValid = false;
            prepareMarkupBase();
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleCornerRadius: {
            if (m_state->beautyShell.enabled) {
                static const std::array<float, 5> radiuses = {0.0f, 10.0f, 16.0f, 24.0f, 32.0f};
                auto it = std::find_if(radiuses.begin(), radiuses.end(), [&](float r) {
                    return std::abs(r - m_state->beautyShell.cornerRadius) < 1.0f;
                });
                if (it == radiuses.end()) {
                    m_state->beautyShell.cornerRadius = 16.0f;
                } else {
                    size_t nextIdx = (std::distance(radiuses.begin(), it) + 1) % radiuses.size();
                    m_state->beautyShell.cornerRadius = radiuses[nextIdx];
                }
                tools3000::core::ConfigManager::instance().set<int>(
                    "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
            } else {
                float cfgMaxRadius = static_cast<float>(tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0));
                static const std::array<float, 6> radiuses = {0.0f, 8.0f, 14.0f, 24.0f, 40.0f, 60.0f};
                auto it = std::find_if(radiuses.begin(), radiuses.end(), [&](float r) {
                    return std::abs(r - m_state->cornerRadius) < 1.0f;
                });
                if (it == radiuses.end()) {
                    m_state->cornerRadius = 8.0f;
                } else {
                    size_t nextIdx = (std::distance(radiuses.begin(), it) + 1) % radiuses.size();
                    m_state->cornerRadius = std::min(radiuses[nextIdx], cfgMaxRadius);
                }
                tools3000::core::ConfigManager::instance().set<double>(
                    "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
            }
            prepareMarkupBase();
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ExtractText:
            if (m_state->ocrCallback) {
                int x1 = std::min(m_state->dragStart.x, m_state->dragEnd.x);
                int y1 = std::min(m_state->dragStart.y, m_state->dragEnd.y);
                int w = std::abs(m_state->dragEnd.x - m_state->dragStart.x);
                int h = std::abs(m_state->dragEnd.y - m_state->dragStart.y);
                
                cv::Mat cropped;
                if (m_state->markup.elementCount() > 0) cropped = m_state->markup.getCompositeImage();
                else {
                    cv::Rect roi(x1, y1, w, h);
                    roi &= cv::Rect(0, 0, m_state->frozenScreen.cols, m_state->frozenScreen.rows);
                    if (roi.area() > 0) m_state->frozenScreen(roi).copyTo(cropped);
                }
                if (cropped.channels() == 4) {
                    cv::cvtColor(cropped, cropped, cv::COLOR_BGRA2BGR);
                }

                int offsetX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                int offsetY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                CaptureRegion region{x1 + offsetX, y1 + offsetY, w, h};
                auto ocrCb = m_state->ocrCallback;
                if(m_cancelCb) m_cancelCb(); 
                ocrCb(region, cropped);
            }
            break;

        case ToolbarCommand::PinWindow: {
            if (m_state->isMarking) {
                finishMarkup(m_state->markupEnd);
            }
            int x1 = std::min(m_state->dragStart.x, m_state->dragEnd.x);
            int y1 = std::min(m_state->dragStart.y, m_state->dragEnd.y);
            int x2 = std::max(m_state->dragStart.x, m_state->dragEnd.x);
            int y2 = std::max(m_state->dragStart.y, m_state->dragEnd.y);
            int w = x2 - x1;
            int h = y2 - y1;
            if (w <= 0 || h <= 0) break;
            
            cv::Mat cropped;
            if (m_state->markup.elementCount() > 0) cropped = m_state->markup.getCompositeImage();
            else {
                cv::Rect roi(x1, y1, w, h);
                roi &= cv::Rect(0, 0, m_state->frozenScreen.cols, m_state->frozenScreen.rows);
                if (roi.area() > 0) m_state->frozenScreen(roi).copyTo(cropped);
            }
            int origX = x1 + GetSystemMetrics(SM_XVIRTUALSCREEN);
            int origY = y1 + GetSystemMetrics(SM_YVIRTUALSCREEN);
            CaptureRegion reg{origX, origY, w, h, m_state->cornerRadius};

            if (m_state->beautyShell.enabled && !cropped.empty()) {
                cropped = applyBeautyShell(cropped, m_state->beautyShell);
            } else if (m_state->cornerRadius > 0.5f && !cropped.empty()) {
                cropped = applyRoundedCorners(cropped, m_state->cornerRadius);
            }

            int padX = (cropped.cols > reg.width && reg.width > 0) ? (cropped.cols - reg.width) / 2 : 0;
            int padY = (cropped.rows > reg.height && reg.height > 0) ? (cropped.rows - reg.height) / 2 : 0;

            // 写入剪贴板与历史记录，赋予贴图后可随时粘贴并追溯历史的能力
            CaptureHistory::instance().push(cropped, reg);
            ClipboardUtils::copyImageToClipboard(cropped, L"", nullptr, &reg, padX, padY);

            // 计算智能贴图坐标（原位对齐并避让重叠）
            POINT spawnPos = PinWindow::calculateSmartSpawnPosition(cropped.cols, cropped.rows, nullptr, &reg, padX, padY);

            // 先关闭截图覆盖层
            if (m_cancelCb) m_cancelCb();

            // 精确原位贴图
            PinWindow::create(cropped, spawnPos.x, spawnPos.y);
            break;
        }

        case ToolbarCommand::ScrollCapture: {
            int x1 = std::min(m_state->dragStart.x, m_state->dragEnd.x);
            int y1 = std::min(m_state->dragStart.y, m_state->dragEnd.y);
            int w = std::abs(m_state->dragEnd.x - m_state->dragStart.x);
            int h = std::abs(m_state->dragEnd.y - m_state->dragStart.y);

            int offsetX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int offsetY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            
            RECT capRect = {x1 + offsetX, y1 + offsetY, x1 + offsetX + w, y1 + offsetY + h};
            
            // 关闭覆盖层
            if(m_cancelCb) m_cancelCb();

            // 启动长截图；完成结果由 ScreenCapture 统一保存、复制并写入历史。
            ScrollCaptureOptions opts;
            opts.captureRect = capRect;
            opts.mode = ScrollMode::Auto;
            ScrollCaptureOverlay::instance().show(capRect);
            ScrollCapture::instance().start(opts);
            if (ScrollCapture::instance().isRunning()) {
                ShortcutHintOverlay::instance().show(
                    ShortcutHintContext::ScrollCapture,
                    {(capRect.left + capRect.right) / 2, (capRect.top + capRect.bottom) / 2});
                tools3000::core::EventBus::instance().publish(
                    tools3000::core::ShowToastEvent{L"正在长截图，按 Esc 停止"});
            }
            break;
        }

        case ToolbarCommand::StartRecord: {
            if (m_state->mode == OverlayMode::RecordRegion) {
                if (m_confirmCb) m_confirmCb({});
            } else {
                transitionToRecordMode(*m_state);
                if (m_renderer) {
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                }
            }
            break;
        }

        case ToolbarCommand::Confirm:
            if(m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
            break;

        case ToolbarCommand::Cancel:
            if(m_cancelCb) m_cancelCb();
            break;
    }
}

void CaptureInput::setCurrentTool(MarkupTool tool) {
    if (m_state->activeElement) {
        if (m_state->activeElement->tool == tool) {
            // 如果切回/选中的就是当前已激活元素对应的工具，保持激活状态；对于文本元素立即进入编辑
            m_state->activeElement->isActive = true;
            if (m_state->activeElement->tool == MarkupTool::Text) {
                m_state->activeElement->isEditing = true;
                if (m_hwnd) SetTimer(m_hwnd, RENDER_TIMER_ID, 16, nullptr);
            }
        } else {
            m_state->activeElement->isActive = false;
            m_state->activeElement->isEditing = false;
            if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->text.empty()) {
                m_state->markup.removeElement(m_state->activeElement->id);
            }
            m_state->activeElement = nullptr;
        }
    }
    m_state->currentTool = tool;
    m_state->isMarking = false;
    m_state->openSubmenu = SubmenuType::None;
    m_state->sliderPopup.type = SliderPopupType::None;
    m_state->toolbarLayoutValid = false;
    if ((int)m_state->state.load() == (int)OverlayState::Marking) m_state->state = OverlayState::Selected;
    m_renderer->markMarkupDirty();
    m_renderer->invalidate();
    updateHoverCursor(m_state->currentCursor);
}

bool CaptureInput::isPointInSelection(POINT point) const {
    auto rect = currentSelectionRect();
    return point.x >= rect.left && point.x <= rect.right &&
           point.y >= rect.top && point.y <= rect.bottom;
}

cv::Point CaptureInput::toMarkupPoint(POINT point) const {
    auto rect = currentSelectionRect();
    int width = std::max(1, static_cast<int>(rect.right - rect.left));
    int height = std::max(1, static_cast<int>(rect.bottom - rect.top));
    int x = std::clamp(static_cast<int>(point.x - rect.left), 0, width - 1);
    int y = std::clamp(static_cast<int>(point.y - rect.top), 0, height - 1);
    return {x, y};
}

void CaptureInput::beginMarkup(POINT point) {
    if (!isPointInSelection(point)) return;
    prepareMarkupBase();
    if (!m_state->markupBaseReady) return;

    cv::Point local = toMarkupPoint(point);
    const float currentDpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
    if (m_state->currentTool == MarkupTool::Magnifier) {
        m_state->markup.addMagnifier(local, 2.0f, static_cast<int>(std::round(60.0f * currentDpiScale)));
        return;
    }
    if (m_state->currentTool == MarkupTool::Text) {
        if (m_state->activeElement) {
            m_state->activeElement->isActive = false;
            m_state->activeElement->isEditing = false;
            if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->text.empty()) {
                m_state->markup.removeElement(m_state->activeElement->id);
            }
            m_state->activeElement = nullptr;
        }
        float defaultFontSize = static_cast<float>(tools3000::core::ConfigManager::instance().get<double>("/capture/markup/text/fontSize", 18.0));
        if (defaultFontSize < 10.0f || defaultFontSize > 144.0f) defaultFontSize = 18.0f;
        auto* elem = m_state->markup.addText(local, "", m_state->currentColor, defaultFontSize * currentDpiScale,
                                             m_state->currentTextOutline, m_state->currentTextOutlineColor);
        m_state->activeElement = elem;
        if (m_state->activeElement) {
            m_state->activeElement->isActive = true;
            m_state->activeElement->isEditing = true;
        }
        m_state->state = OverlayState::Selected;
        m_renderer->markMarkupDirty();
        m_renderer->invalidate();
        updateHoverCursor(m_state->currentCursor);
        return;
    }

    m_state->markupStart = point;
    m_state->markupEnd = point;
    m_state->penPoints.clear();
    if (m_state->currentTool == MarkupTool::Pen) {
        m_state->penPoints.push_back(local);
    }
    m_state->isMarking = true;
    m_state->state = OverlayState::Marking;
}

void CaptureInput::updateMarkup(POINT point) {
    if (!m_state->isMarking) return;

    m_state->markupEnd = point;
    if (m_state->currentTool == MarkupTool::Pen) {
        cv::Point local = toMarkupPoint(point);
        if (m_state->penPoints.empty() || m_state->penPoints.back() != local) {
            m_state->penPoints.push_back(local);
        }
    }
}

void CaptureInput::finishMarkup(POINT point) {
    if (!m_state->isMarking) return;

    m_state->markupEnd = point;
    cv::Point start = toMarkupPoint(m_state->markupStart);
    cv::Point end = toMarkupPoint(m_state->markupEnd);

    // Shift 约束：矩形/椭圆保持 1:1 正方/正圆，箭头吸附 45° 正交角度
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        if (m_state->currentTool == MarkupTool::Rectangle || m_state->currentTool == MarkupTool::Ellipse ||
            m_state->currentTool == MarkupTool::Mosaic || m_state->currentTool == MarkupTool::Highlight) {
            int w = std::abs(end.x - start.x);
            int h = std::abs(end.y - start.y);
            int side = std::max(w, h);
            end.x = start.x + (end.x >= start.x ? side : -side);
            end.y = start.y + (end.y >= start.y ? side : -side);
        } else if (m_state->currentTool == MarkupTool::Arrow) {
            double dx = end.x - start.x;
            double dy = end.y - start.y;
            double dist = std::hypot(dx, dy);
            if (dist >= 1.0) {
                double angle = std::atan2(dy, dx);
                constexpr double step = 3.14159265358979323846 / 4.0;
                double snappedAngle = std::round(angle / step) * step;
                end.x = static_cast<int>(std::round(start.x + dist * std::cos(snappedAngle)));
                end.y = static_cast<int>(std::round(start.y + dist * std::sin(snappedAngle)));
            }
        }
    }

    int dx = std::abs(end.x - start.x);
    int dy = std::abs(end.y - start.y);

    if (m_state->currentTool != MarkupTool::Pen && m_state->currentTool != MarkupTool::Number && dx < 3 && dy < 3) {
        m_state->isMarking = false;
        m_state->state = OverlayState::Selected;
        return;
    }

    const float currentDpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;

    switch (m_state->currentTool) {
        case MarkupTool::Rectangle: {
            auto* elem = m_state->markup.drawRectangle(start, end, m_state->currentColor, static_cast<float>(m_state->currentStrokeWidth));
            if (elem) {
                elem->lineStyle = m_state->currentLineStyle;
                elem->fill = m_state->currentFillMode;
                elem->cornerRadius = m_state->currentElementCornerRadius;
                elem->dpiScale = currentDpiScale;
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Line: {
            auto* elem = m_state->markup.drawLine(start, end, m_state->currentColor, static_cast<float>(m_state->currentStrokeWidth));
            if (elem) {
                elem->lineStyle = m_state->currentLineStyle;
                elem->dpiScale = currentDpiScale;
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Arrow: {
            auto* elem = m_state->markup.drawArrow(start, end, m_state->currentColor, static_cast<float>(m_state->currentStrokeWidth));
            if (elem) {
                elem->lineStyle = m_state->currentLineStyle;
                elem->arrowStyle = m_state->currentArrowStyle;
                elem->dpiScale = currentDpiScale;
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Ellipse: {
            auto* elem = m_state->markup.drawEllipse(start, end, m_state->currentColor, static_cast<float>(m_state->currentStrokeWidth));
            if (elem) {
                elem->fill = m_state->currentFillMode;
                elem->dpiScale = currentDpiScale;
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Number: {
            auto* elem = m_state->markup.drawNumberMark(start, m_state->currentColor, currentDpiScale, m_state->currentNumberShape);
            if (elem) {
                float dist = static_cast<float>(std::hypot(end.x - start.x, end.y - start.y));
                if (dist >= 10.0f * currentDpiScale) {
                    elem->hasLeaderArrow = true;
                    elem->endPt = end;
                }
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Pen: {
            auto* elem = m_state->markup.drawPenStroke(m_state->penPoints, m_state->currentColor, static_cast<float>(m_state->currentStrokeWidth));
            if (elem) {
                elem->lineStyle = m_state->currentLineStyle;
                elem->dpiScale = currentDpiScale;
            }
            break;
        }

        case MarkupTool::Highlight:
            m_state->markup.drawHighlight(start, end, m_state->currentColor);
            break;

        case MarkupTool::Mosaic: {
            if (m_state->currentMosaicType == 1) {
                int ksize = (std::max)(5, m_state->currentStrokeWidth * 3);
                m_state->markup.applyBlur(start, end, ksize);
            } else {
                int bs = (std::max)(4, m_state->currentStrokeWidth * 2);
                m_state->markup.applyMosaic(start, end, bs);
            }
            break;
        }

        case MarkupTool::Blur: {
            int ksize = (std::max)(5, m_state->currentStrokeWidth * 3);
            m_state->markup.applyBlur(start, end, ksize);
            break;
        }

        default:
            break;
    }

    m_state->isMarking = false;
    m_state->state = OverlayState::Selected;
    m_renderer->markMarkupDirty();
    m_renderer->invalidate();
    m_state->toolbarLayoutValid = false;
}

void CaptureInput::prepareMarkupBase() {
    if (m_state->markupBaseReady || m_state->frozenScreen.empty()) return;

    auto rect = currentSelectionRect();
    int x = static_cast<int>(rect.left);
    int y = static_cast<int>(rect.top);
    int w = static_cast<int>(rect.right - rect.left);
    int h = static_cast<int>(rect.bottom - rect.top);
    if (w <= 0 || h <= 0) return;

    cv::Rect roiRect(x, y, w, h);
    roiRect &= cv::Rect(0, 0, m_state->frozenScreen.cols, m_state->frozenScreen.rows);
    if (roiRect.area() <= 0) return;

    cv::Mat cropped;
    m_state->frozenScreen(roiRect).copyTo(cropped);
    if (cropped.channels() == 4) {
        cv::cvtColor(cropped, cropped, cv::COLOR_BGRA2BGR);
    }

    m_state->markup.setBaseImage(cropped);
    m_state->markupBaseReady = true;
}

void CaptureInput::rebuildMarkupBase() {
    if (m_state->frozenScreen.empty()) return;
    auto rect = currentSelectionRect();
    int x = static_cast<int>(rect.left);
    int y = static_cast<int>(rect.top);
    int w = static_cast<int>(rect.right - rect.left);
    int h = static_cast<int>(rect.bottom - rect.top);
    if (w <= 0 || h <= 0) return;

    cv::Rect roi(x, y, w, h);
    roi &= cv::Rect(0, 0, m_state->frozenScreen.cols, m_state->frozenScreen.rows);
    if (roi.area() <= 0) return;

    cv::Mat cropped;
    m_state->frozenScreen(roi).copyTo(cropped);
    if (cropped.channels() == 4) {
        cv::cvtColor(cropped, cropped, cv::COLOR_BGRA2BGR);
    }
    m_state->markup.updateBaseImage(cropped);  // 保留标注，仅替换底图
    m_state->markupBaseReady = true;
}

std::vector<RECT> CaptureInput::detectWindowHierarchy(POINT cursorPos) {
    std::vector<RECT> hierarchy;
    std::vector<std::wstring> hierarchyDescriptions;
    int offsetX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int offsetY = GetSystemMetrics(SM_YVIRTUALSCREEN);

    // 1. 穿透覆盖层查找真实窗口
    HWND hTop = nullptr;
    struct SearchParam {
        POINT pt;
        HWND exclude;
        HWND found;
    } param{cursorPos, m_hwnd, nullptr};

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* p = reinterpret_cast<SearchParam*>(lParam);
        if (hwnd == p->exclude || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
        RECT r;
        GetWindowRect(hwnd, &r);
        if (PtInRect(&r, p->pt)) {
            p->found = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&param));
    hTop = param.found;

    if (hTop && hTop != m_hwnd) {
        // 2. 深入获取真实子控件
        POINT clientPt = cursorPos;
        ScreenToClient(hTop, &clientPt);
        HWND hChild = RealChildWindowFromPoint(hTop, clientPt);
        if (!hChild) hChild = hTop;

        // 3. 从内到外依次收集子控件与父窗口
        HWND curr = hChild;
        while (curr && curr != GetDesktopWindow()) {
            RECT rc{};
            bool gotRect = false;
            // 优先使用 DWM 扩展客户边界，剔除 Win10/Win11 隐形阴影边
            RECT frameBounds{};
            if (SUCCEEDED(DwmGetWindowAttribute(curr, DWMWA_EXTENDED_FRAME_BOUNDS, &frameBounds, sizeof(frameBounds)))) {
                rc = frameBounds;
                gotRect = true;
            } else if (GetWindowRect(curr, &rc)) {
                gotRect = true;
            }

            if (gotRect && (rc.right - rc.left > 8) && (rc.bottom - rc.top > 8)) {
                rc.left -= offsetX;
                rc.top -= offsetY;
                rc.right -= offsetX;
                rc.bottom -= offsetY;

                bool exists = false;
                for (const auto& existing : hierarchy) {
                    if (std::abs(existing.left - rc.left) <= 2 &&
                        std::abs(existing.top - rc.top) <= 2 &&
                        std::abs(existing.right - rc.right) <= 2 &&
                        std::abs(existing.bottom - rc.bottom) <= 2) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    hierarchy.push_back(rc);

                    // 生成层级微晶徽章友好描述
                    wchar_t className[64] = {};
                    GetClassNameW(curr, className, 64);
                    wchar_t title[64] = {};
                    GetWindowTextW(curr, title, 64);
                    std::wstring typeName = L"控件";
                    std::wstring cls = className;
                    for (auto& ch : cls) ch = towlower(ch);
                    if (cls.find(L"button") != std::wstring::npos) typeName = L"按钮";
                    else if (cls.find(L"edit") != std::wstring::npos) typeName = L"输入框";
                    else if (cls.find(L"static") != std::wstring::npos) typeName = L"文本";
                    else if (cls.find(L"combo") != std::wstring::npos) typeName = L"下拉框";
                    else if (cls.find(L"list") != std::wstring::npos) typeName = L"列表";
                    else if (cls.find(L"tree") != std::wstring::npos) typeName = L"树形控件";
                    else if (cls.find(L"tab") != std::wstring::npos) typeName = L"选项卡";
                    else if (cls.find(L"toolbar") != std::wstring::npos) typeName = L"工具栏";
                    else if (GetParent(curr) == nullptr || curr == hTop) typeName = L"窗口";

                    int w = rc.right - rc.left;
                    int h = rc.bottom - rc.top;
                    std::wstring desc = typeName;
                    if (title[0] != L'\0') {
                        std::wstring t = title;
                        if (t.size() > 10) t = t.substr(0, 8) + L"...";
                        desc += L": " + t;
                    }
                    desc += L" (" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L")";
                    hierarchyDescriptions.push_back(desc);
                }
            }
            curr = GetParent(curr);
        }
    }

    // 4. 追加当前光标所在的单屏全屏区域
    HMONITOR hMon = MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONEAREST);
    if (hMon) {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(hMon, &mi)) {
            RECT monRc = mi.rcMonitor;
            monRc.left -= offsetX;
            monRc.top -= offsetY;
            monRc.right -= offsetX;
            monRc.bottom -= offsetY;

            bool exists = false;
            for (const auto& existing : hierarchy) {
                if (existing.left == monRc.left && existing.top == monRc.top &&
                    existing.right == monRc.right && existing.bottom == monRc.bottom) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                hierarchy.push_back(monRc);
                hierarchyDescriptions.push_back(L"屏幕 (" + std::to_wstring(monRc.right - monRc.left) + L"x" + std::to_wstring(monRc.bottom - monRc.top) + L")");
            }
        }
    }

    // 5. 兜底全屏
    if (hierarchy.empty()) {
        RECT fullRc{0, 0, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        hierarchy.push_back(fullRc);
        hierarchyDescriptions.push_back(L"全屏 (" + std::to_wstring(fullRc.right - fullRc.left) + L"x" + std::to_wstring(fullRc.bottom - fullRc.top) + L")");
    }

    if (m_state) {
        m_state->detectedWindowHierarchyDescriptions = hierarchyDescriptions;
    }
    return hierarchy;
}

RECT CaptureInput::detectWindowUnderCursor(POINT cursorPos) {
    auto list = detectWindowHierarchy(cursorPos);
    if (!list.empty()) return list.front();
    return {};
}

D2D1_RECT_F CaptureInput::currentSelectionRect() const {
    float x1 = static_cast<float>(std::min(m_state->dragStart.x, m_state->dragEnd.x));
    float y1 = static_cast<float>(std::min(m_state->dragStart.y, m_state->dragEnd.y));
    float x2 = static_cast<float>(std::max(m_state->dragStart.x, m_state->dragEnd.x));
    float y2 = static_cast<float>(std::max(m_state->dragStart.y, m_state->dragEnd.y));
    return D2D1::RectF(x1, y1, x2, y2);
}

LRESULT CaptureInput::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    

    // 窗口过程兜底：渲染/标注路径含 OpenCV 分配、std::format 等可抛操作，异常绝不能逃逸到
    // Win32 派发层（否则 std::terminate 崩溃）。
    try {
    switch (msg) {
        case WM_SETCURSOR: {
            if (!this || !m_state) break;
            ensureCursorVisible();
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            updateHoverCursor(pt);
            return TRUE;
        }
        case WM_LBUTTONDOWN: {
            if (!this) break;
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
            m_renderer->markMarkupDirty();
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if ((int)m_state->state.load() == (int)OverlayState::Selected || (int)m_state->state.load() == (int)OverlayState::Marking) {
                // 0. 如果滑块微浮岛弹窗处于展开状态，优先响应滑块拖拽与预设点击
                if (m_state->sliderPopup.type != SliderPopupType::None) {
                    float px = static_cast<float>(point.x);
                    float py = static_cast<float>(point.y);
                    auto pr = m_state->sliderPopup.popupRect;
                    if (px >= pr.left && px <= pr.right && py >= pr.top && py <= pr.bottom) {
                        // 检测是否点击预设胶囊
                        for (const auto& [val, btnRect] : m_state->sliderPopup.presetButtons) {
                            if (px >= btnRect.left && px <= btnRect.right && py >= btnRect.top && py <= btnRect.bottom) {
                                if (m_state->sliderPopup.type == SliderPopupType::SelectionCornerRadius) {
                                    if (m_state->beautyShell.enabled) {
                                        m_state->beautyShell.cornerRadius = static_cast<float>(val);
                                        tools3000::core::ConfigManager::instance().set<int>("/capture/beautyShellRadius", val);
                                    } else {
                                        m_state->cornerRadius = static_cast<float>(val);
                                        tools3000::core::ConfigManager::instance().set<double>("/screenshot/cornerRadius", static_cast<double>(val));
                                    }
                                    prepareMarkupBase();
                                } else if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                                    m_state->currentElementCornerRadius = static_cast<float>(val);
                                } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                                        m_state->activeElement->fontSize = static_cast<float>(val);
                                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                                    }
                                    const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                                    tools3000::core::ConfigManager::instance().set<double>("/capture/markup/text/fontSize", static_cast<double>(val) / curDpi);
                                } else {
                                    m_state->currentStrokeWidth = val;
                                }
                                if (m_state->activeElement) {
                                    if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                                        m_state->activeElement->cornerRadius = static_cast<float>(val);
                                    } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                                        m_state->activeElement->fontSize = static_cast<float>(val);
                                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                                    } else if (m_state->sliderPopup.type != SliderPopupType::SelectionCornerRadius) {
                                        m_state->activeElement->thickness = static_cast<float>(val);
                                    }
                                    m_renderer->markMarkupDirty();
                                }
                                m_state->toolbarLayoutValid = false;
                                m_renderer->invalidate();
                                return 0;
                            }
                        }

                        // 检测是否点击或拖拽滑动轨道
                        auto tr = m_state->sliderPopup.trackRect;
                        if (px >= tr.left - 8.0f && px <= tr.right + 8.0f && py >= tr.top - 12.0f && py <= tr.bottom + 12.0f) {
                            m_state->sliderPopup.isDragging = true;
                            float pct = std::clamp((px - tr.left) / (tr.right - tr.left), 0.0f, 1.0f);
                            int val = m_state->sliderPopup.minValue + static_cast<int>(std::round(pct * (m_state->sliderPopup.maxValue - m_state->sliderPopup.minValue)));
                            if (m_state->sliderPopup.type == SliderPopupType::SelectionCornerRadius) {
                                if (m_state->beautyShell.enabled) {
                                    m_state->beautyShell.cornerRadius = static_cast<float>(val);
                                    tools3000::core::ConfigManager::instance().set<int>("/capture/beautyShellRadius", val);
                                } else {
                                    m_state->cornerRadius = static_cast<float>(val);
                                    tools3000::core::ConfigManager::instance().set<double>("/screenshot/cornerRadius", static_cast<double>(val));
                                }
                                prepareMarkupBase();
                            } else if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                                m_state->currentElementCornerRadius = static_cast<float>(val);
                            } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                                    m_state->activeElement->fontSize = static_cast<float>(val);
                                    m_state->activeElement->textRenderSize = cv::Size(0, 0);
                                }
                                const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                                tools3000::core::ConfigManager::instance().set<double>("/capture/markup/text/fontSize", static_cast<double>(val) / curDpi);
                            } else {
                                m_state->currentStrokeWidth = val;
                            }
                            if (m_state->activeElement) {
                                if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                                    m_state->activeElement->cornerRadius = static_cast<float>(val);
                                } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                                    m_state->activeElement->fontSize = static_cast<float>(val);
                                    m_state->activeElement->textRenderSize = cv::Size(0, 0);
                                } else if (m_state->sliderPopup.type != SliderPopupType::SelectionCornerRadius) {
                                    m_state->activeElement->thickness = static_cast<float>(val);
                                }
                                m_renderer->markMarkupDirty();
                            }
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                            return 0;
                        }
                        return 0;
                    } else {
                        // 点击滑块外部区域，优雅收起
                        m_state->sliderPopup.type = SliderPopupType::None;
                        m_state->sliderPopup.isDragging = false;
                        m_renderer->invalidate();
                    }
                }

                // 1. 如果尺寸设置菜单处于展开状态，优先检测点击
                if (m_state->isSizeMenuOpen) {
                    float mx = static_cast<float>(point.x);
                    float my = static_cast<float>(point.y);
                    auto mr = m_state->sizeMenuRect;
                    if (mx >= mr.left && mx <= mr.right && my >= mr.top && my <= mr.bottom) {
                        const float scale = std::clamp(m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
                        float row1Y = mr.top + 16.0f * scale;
                        float row2Y = row1Y + 22.0f * scale;
                        float row3Y = row2Y + 26.0f * scale;
                        float row4Y = row3Y + 28.0f * scale;

                        if (my >= mr.top && my < row2Y) {
                            if (mx < mr.left + (mr.right - mr.left) * 0.5f) {
                                m_state->sizeUnit = CaptureState::SizeUnit::Pixel;
                            } else {
                                m_state->sizeUnit = CaptureState::SizeUnit::DeviceIndependentPixel;
                            }
                        } else if (my >= row2Y && my < row4Y) {
                            m_state->showPositionInHud = !m_state->showPositionInHud;
                        } else if (my >= row4Y) {
                            m_state->showUnitInHud = !m_state->showUnitInHud;
                        }
                        m_renderer->invalidate();
                        return 0;
                    } else {
                        m_state->isSizeMenuOpen = false;
                        m_renderer->invalidate();
                    }
                }

                // 2. 检测是否点击尺寸胶囊（HUD）以展开/收起菜单
                float px = static_cast<float>(point.x);
                float py = static_cast<float>(point.y);
                auto hr = m_state->sizeHudRect;
                if (px >= hr.left && px <= hr.right && py >= hr.top && py <= hr.bottom) {
                    m_state->isSizeMenuOpen = !m_state->isSizeMenuOpen;
                    m_renderer->invalidate();
                    return 0;
                }

                // 2.5. 检测是否点击二维码胶囊 Chip (一键复制到剪贴板)
                if (!m_state->detectedQrText.empty() && m_state->qrChipRect.right > m_state->qrChipRect.left) {
                    if (px >= m_state->qrChipRect.left && px <= m_state->qrChipRect.right &&
                        py >= m_state->qrChipRect.top && py <= m_state->qrChipRect.bottom) {
                        tools3000::core::WinUtils::copyToClipboard(tools3000::core::WinUtils::utf8ToWstring(m_state->detectedQrText), hwnd);
                        tools3000::core::EventBus::instance().publish(
                            tools3000::core::ShowToastEvent{L"已复制二维码内容到剪贴板"});
                        return 0;
                    }
                }

                // 2.6. 检测是否点击微晶下拉菜单项 (美化外壳渐变/边距/录屏设置)
                if (m_state->dropdownMenu.type != DropdownType::None) {
                    int hitItem = -1;
                    for (int i = 0; i < static_cast<int>(m_state->dropdownMenu.items.size()); ++i) {
                        const auto& item = m_state->dropdownMenu.items[i];
                        if (point.x >= item.rect.left && point.x <= item.rect.right &&
                            point.y >= item.rect.top && point.y <= item.rect.bottom) {
                            hitItem = i;
                            break;
                        }
                    }
                    if (hitItem >= 0) {
                        commitDropdownSelection(m_state->dropdownMenu.type, m_state->dropdownMenu.items[hitItem].id);
                        return 0;
                    } else {
                        tools3000::capture::closeDropdownMenu(*m_state, true);
                        m_renderer->invalidate();
                    }
                }

                HitArea selHit = HitArea::None;
                if (auto* button = hitTestToolbar(point)) {
                    // 如果正在编辑文字且点击的不是选颜色按钮，退出编辑
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                        if (button->command != ToolbarCommand::SelectColor &&
                            button->command != ToolbarCommand::ToggleTextOutline &&
                            button->command != ToolbarCommand::SelectTextOutlineColor) {
                            m_state->activeElement->isEditing = false;
                            if (m_state->activeElement->text.empty()) {
                                m_state->markup.removeElement(m_state->activeElement->id);
                                m_state->activeElement = nullptr;
                            }
                        }
                    }
                    executeToolbarCommand(*button);
                    return 0;
                }

                const bool inSelection = isPointInSelection(point);
                cv::Point local = inSelection ? toMarkupPoint(point) : cv::Point{0, 0};

                // 3. 选区控制点/外框拉伸手柄检测 (优先级高于画布标注元素，彻底解决正在编辑文本时外框手柄被文本框抢占的严重缺陷)
                int cornerIdx = -1;
                selHit = hitTestSelectionBox(point, &cornerIdx);

                if (selHit == HitArea::CornerRadius || (selHit != HitArea::None && selHit != HitArea::Body)) {
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                        m_state->activeElement->isEditing = false;
                        if (m_state->activeElement->text.empty()) {
                            m_state->markup.removeElement(m_state->activeElement->id);
                            m_state->activeElement = nullptr;
                        }
                    }
                    if (selHit == HitArea::CornerRadius) {
                        m_state->isAdjustingCornerRadius = true;
                        m_state->cornerDragStartRadius = m_state->effectiveCornerRadius();
                        m_state->cornerDragStartPos = point;
                        m_state->cornerDragIndex = (cornerIdx >= 0) ? cornerIdx : 0;
                        updateHoverCursor(point);
                        return 0;
                    }
                    m_state->isAdjustingSelection = true;
                    m_state->selAdjustHandle = selHit;
                    m_state->selAdjustLast = point;
                    updateHoverCursor(point);
                    return 0;
                }

                // 录屏选区模式下：选区边框带 (HitArea::Body) 用于整体移动录屏选区
                if (m_state->mode == OverlayMode::RecordRegion && selHit == HitArea::Body) {
                    m_state->isAdjustingSelection = true;
                    m_state->selAdjustHandle = HitArea::Body;
                    m_state->selAdjustLast = point;
                    updateHoverCursor(point);
                    return 0;
                }

                if (!inSelection && selHit == HitArea::Body) {
                    m_state->isAdjustingSelection = true;
                    m_state->selAdjustHandle = HitArea::Body;
                    m_state->selAdjustLast = point;
                    updateHoverCursor(point);
                    return 0;
                }

                // 4. 检测当前处于激活态的标注元素（包括矩形四角内向微晶圆角把手与 8 方向拉伸手柄或文本框）
                if (inSelection && m_state->activeElement && m_state->activeElement->isActive) {
                    HitArea activeElemHit = m_state->activeElement->hitTestEx(local);
                    if (activeElemHit != HitArea::None) {
                        if (m_state->activeElement->tool == MarkupTool::Text) {
                            if (activeElemHit == HitArea::CornerRadius) {
                                m_state->activeElement->isEditing = false;
                            } else {
                                m_state->activeElement->isEditing = true;
                                SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                            }
                        }

                        if (activeElemHit == HitArea::CornerRadius) {
                            m_state->cornerDragStartPos = point;
                            m_state->cornerDragStartRadius = m_state->activeElement->cornerRadius;
                            int x1 = (std::min)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                            int y1 = (std::min)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                            int x2 = (std::max)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                            int y2 = (std::max)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                            float w = static_cast<float>(x2 - x1);
                            float h = static_cast<float>(y2 - y1);
                            bool isSmall = (w < 80.0f || h < 80.0f);
                            int hitIdx = CornerRadiusHelper::hitTestHandles(
                                static_cast<float>(x1), static_cast<float>(y1),
                                static_cast<float>(x2), static_cast<float>(y2),
                                m_state->activeElement->cornerRadius,
                                static_cast<float>(local.x), static_cast<float>(local.y),
                                1.0f, 9.0f, isSmall);
                            m_state->cornerDragIndex = (hitIdx >= 0) ? hitIdx : 0;
                        }

                        m_state->dragHandle = (activeElemHit == HitArea::Body) ? HitArea::None : activeElemHit;
                        m_state->isManipulating = true;
                        m_state->lastMousePos = point;
                        m_renderer->invalidate();
                        updateHoverCursor(point);
                        return 0;
                    }
                }

                // 5. 选区内部点击流转：未激活元素选中 vs 空白区域开始绘制
                if (inSelection) {
                    // 测试是否命中画布上的其他标注元素（空心未填充矩形只命中描边轮廓，不遮挡内部绘制）
                    HitResult hit = m_state->markup.getElementAtEx(local);
                    if (hit.element) {
                        if (m_state->activeElement && m_state->activeElement != hit.element) {
                            m_state->activeElement->isActive = false;
                            m_state->activeElement->isEditing = false;
                            if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->text.empty()) {
                                m_state->markup.removeElement(m_state->activeElement->id);
                            }
                        }

                        m_state->activeElement = hit.element;
                        m_state->activeElement->isActive = true;

                        // 全标注工具二次编辑框架：同步选中元素的工具类型与全部属性至全局状态
                        m_state->currentTool = hit.element->tool;
                        m_state->currentColor = hit.element->color;
                        m_state->currentStrokeWidth = static_cast<int>(std::round(hit.element->thickness));
                        m_state->currentFillMode = hit.element->fill;
                        m_state->currentLineStyle = hit.element->lineStyle;
                        m_state->currentArrowStyle = hit.element->arrowStyle;
                        m_state->currentNumberShape = hit.element->numberShape;
                        m_state->currentElementCornerRadius = hit.element->cornerRadius;
                        if (hit.element->tool == MarkupTool::Text) {
                            m_state->currentTextOutline = hit.element->textOutline;
                            m_state->currentTextOutlineColor = hit.element->textOutlineColor;
                        }
                        m_state->toolbarLayoutValid = false;

                        if (hit.element->tool == MarkupTool::Text) {
                            m_state->activeElement->isEditing = true;
                            SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                        } else {
                            m_state->activeElement->isEditing = false;
                        }

                        if (hit.area == HitArea::CornerRadius) {
                            m_state->cornerDragStartPos = point;
                            m_state->cornerDragStartRadius = m_state->activeElement->cornerRadius;
                            if (m_state->activeElement->tool == MarkupTool::Rectangle) {
                                int x1 = (std::min)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                                int y1 = (std::min)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                                int x2 = (std::max)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                                int y2 = (std::max)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                                float w = static_cast<float>(x2 - x1);
                                float h = static_cast<float>(y2 - y1);
                                bool isSmall = (w < 80.0f || h < 80.0f);
                                int hitIdx = CornerRadiusHelper::hitTestHandles(
                                    static_cast<float>(x1), static_cast<float>(y1),
                                    static_cast<float>(x2), static_cast<float>(y2),
                                    m_state->activeElement->cornerRadius,
                                    static_cast<float>(local.x), static_cast<float>(local.y),
                                    1.0f, 9.0f, isSmall);
                                m_state->cornerDragIndex = (hitIdx >= 0) ? hitIdx : 0;
                            }
                        }

                        m_state->dragHandle = (hit.area == HitArea::Body) ? HitArea::None : hit.area;
                        m_state->isManipulating = true;
                        m_state->lastMousePos = point;
                        m_renderer->invalidate();
                        updateHoverCursor(point);
                        return 0;
                    }

                    // 点击在空白区域：
                    // 若此前正在编辑文本，点击空白区域应提交并退出文本编辑
                    bool wasEditingText = false;
                    if (m_state->activeElement) {
                        if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                            wasEditingText = true;
                        }
                        m_state->activeElement->isActive = false;
                        m_state->activeElement->isEditing = false;
                        if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->text.empty()) {
                            m_state->markup.removeElement(m_state->activeElement->id);
                        }
                        m_state->activeElement = nullptr;
                    }

                    // 若刚退出文本编辑态，仅提交并恢复就绪态，不立即连续落点新建第二个文本框
                    if (wasEditingText && m_state->currentTool == MarkupTool::Text) {
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        updateHoverCursor(point);
                        return 0;
                    }

                    // 在选区内部空白区域立即启动新标注绘制流程
                    beginMarkup(point);
                    return 0;
                }
                return 0;
            }

            m_state->dragStart = point;
            m_state->dragEnd = m_state->dragStart;
            m_state->lastMousePos = point;
            m_state->dragging = true;
            m_state->state = OverlayState::Selecting;
            if (m_state->options.showShortcutHints) {
                ShortcutHintOverlay::instance().show(
                    m_state->mode == OverlayMode::RecordRegion
                        ? ShortcutHintContext::RecordSelecting
                        : ShortcutHintContext::CaptureSelecting);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!this) break;
            m_state->currentCursor = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            // Mouse coordinates are client-relative while monitor APIs require
            // virtual-desktop screen coordinates (which may also be negative).
            POINT screenPoint = m_state->currentCursor;
            ClientToScreen(hwnd, &screenPoint);
            HMONITOR hMon = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
            const UINT dpiX = tools3000::core::dpi::effectiveDpiForMonitor(hMon);
            const float newScale = tools3000::core::dpi::scaleForDpi(dpiX);
            if (std::abs(newScale - m_state->dpiScale) >= 0.01f) {
                m_state->dpiScale = newScale;
                if (!m_renderer->updateDpiScale(newScale)) {
                    LOG_WARN("截图覆盖层 DPI 文本资源更新失败: dpi={}", dpiX);
                }
                m_renderer->invalidate();
            }

            if (m_state->dropdownMenu.type != DropdownType::None) {
                int hoverItem = -1;
                for (int i = 0; i < static_cast<int>(m_state->dropdownMenu.items.size()); ++i) {
                    const auto& item = m_state->dropdownMenu.items[i];
                    if (m_state->currentCursor.x >= item.rect.left && m_state->currentCursor.x <= item.rect.right &&
                        m_state->currentCursor.y >= item.rect.top && m_state->currentCursor.y <= item.rect.bottom) {
                        hoverItem = i;
                        break;
                    }
                }
                tools3000::capture::updateDropdownHover(*m_state, hoverItem);
                m_renderer->invalidate();
            }

            if (m_state->sliderPopup.isDragging) {
                float px = static_cast<float>(m_state->currentCursor.x);
                auto tr = m_state->sliderPopup.trackRect;
                float pct = std::clamp((px - tr.left) / (tr.right - tr.left), 0.0f, 1.0f);
                int val = m_state->sliderPopup.minValue + static_cast<int>(std::round(pct * (m_state->sliderPopup.maxValue - m_state->sliderPopup.minValue)));
                if (m_state->sliderPopup.type == SliderPopupType::SelectionCornerRadius) {
                    if (m_state->beautyShell.enabled) {
                        m_state->beautyShell.cornerRadius = static_cast<float>(val);
                        tools3000::core::ConfigManager::instance().set<int>("/capture/beautyShellRadius", val);
                    } else {
                        m_state->cornerRadius = static_cast<float>(val);
                        tools3000::core::ConfigManager::instance().set<double>("/screenshot/cornerRadius", static_cast<double>(val));
                    }
                    prepareMarkupBase();
                } else if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                    m_state->currentElementCornerRadius = static_cast<float>(val);
                } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                        m_state->activeElement->fontSize = static_cast<float>(val);
                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                    }
                    const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                    tools3000::core::ConfigManager::instance().set<double>("/capture/markup/text/fontSize", static_cast<double>(val) / curDpi);
                } else {
                    m_state->currentStrokeWidth = val;
                }
                if (m_state->activeElement) {
                    if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                        m_state->activeElement->cornerRadius = static_cast<float>(val);
                    } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                        m_state->activeElement->fontSize = static_cast<float>(val);
                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                    } else if (m_state->sliderPopup.type != SliderPopupType::SelectionCornerRadius) {
                        m_state->activeElement->thickness = static_cast<float>(val);
                    }
                    m_renderer->markMarkupDirty();
                }
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                return 0;
            }

            if (m_state->isAdjustingCornerRadius) {
                // 以选区中心为基准：往内拉增加圆角，往外推减小圆角
                auto r = currentSelectionRect();
                float cfgMaxRadius = m_state->beautyShell.enabled ? 32.0f : static_cast<float>(tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0));
                const float currentDpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                float newR = CornerRadiusHelper::calculateDraggedRadius(
                    r.left, r.top, r.right, r.bottom,
                    static_cast<float>(m_state->cornerDragStartPos.x),
                    static_cast<float>(m_state->cornerDragStartPos.y),
                    m_state->cornerDragStartRadius,
                    static_cast<float>(m_state->currentCursor.x),
                    static_cast<float>(m_state->currentCursor.y),
                    cfgMaxRadius,
                    m_state->cornerDragIndex,
                    currentDpiScale);

                const bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
                if (m_state->beautyShell.enabled) {
                    m_state->beautyShell.cornerRadius = std::round(newR);
                    m_state->loupeToastMessage = CornerRadiusHelper::getToastMessage(
                        CornerRadiusTarget::BeautifyShell, m_state->beautyShell.cornerRadius, isZh);
                } else {
                    m_state->cornerRadius = std::round(newR);
                    m_state->loupeToastMessage = CornerRadiusHelper::getToastMessage(
                        CornerRadiusTarget::Selection, m_state->cornerRadius, isZh);
                }
                m_state->loupeToastUntil = GetTickCount() + 1200;
                m_renderer->invalidate();
                return 0;
            } else if (m_state->isAdjustingSelection) {
                int dx = m_state->currentCursor.x - m_state->selAdjustLast.x;
                int dy = m_state->currentCursor.y - m_state->selAdjustLast.y;
                adjustSelection(m_state->selAdjustHandle, dx, dy);
                m_state->selAdjustLast = m_state->currentCursor;
                m_renderer->invalidate();
            } else if (m_state->isManipulating && m_state->activeElement) {
                if (m_state->dragHandle == HitArea::CornerRadius && m_state->activeElement->tool == MarkupTool::Rectangle) {
                    auto r = currentSelectionRect();
                    int x1 = (std::min)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                    int y1 = (std::min)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                    int x2 = (std::max)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                    int y2 = (std::max)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                    float l = r.left + static_cast<float>(x1);
                    float t = r.top + static_cast<float>(y1);
                    float right = r.left + static_cast<float>(x2);
                    float bottom = r.top + static_cast<float>(y2);
                    const float currentDpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                    float newR = CornerRadiusHelper::calculateDraggedRadius(
                        l, t, right, bottom,
                        static_cast<float>(m_state->cornerDragStartPos.x),
                        static_cast<float>(m_state->cornerDragStartPos.y),
                        m_state->cornerDragStartRadius,
                        static_cast<float>(m_state->currentCursor.x),
                        static_cast<float>(m_state->currentCursor.y),
                        0.0f,
                        m_state->cornerDragIndex,
                        currentDpiScale);
                    m_state->activeElement->cornerRadius = std::round(newR);
                    m_state->currentElementCornerRadius = m_state->activeElement->cornerRadius;
                    const bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
                    m_state->loupeToastMessage = CornerRadiusHelper::getToastMessage(
                        CornerRadiusTarget::RectangleMarkup, m_state->activeElement->cornerRadius, isZh);
                    m_state->loupeToastUntil = GetTickCount() + 1200;
                    m_state->lastMousePos = m_state->currentCursor;
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                } else {
                    int dx = m_state->currentCursor.x - m_state->lastMousePos.x;
                    int dy = m_state->currentCursor.y - m_state->lastMousePos.y;
                    if (m_state->dragHandle == HitArea::None) {
                        m_state->activeElement->moveBy(dx, dy);
                    } else {
                        m_state->activeElement->resize(dx, dy, m_state->dragHandle);
                    }
                    m_state->lastMousePos = m_state->currentCursor;
                    m_renderer->markMarkupDirty();   // 元素几何变了，合成缓存失效
                    m_renderer->invalidate();        // 立即触发 Direct2D 实时重绘
                }
            } else if (m_state->isMarking) {
                updateMarkup(m_state->currentCursor);
                m_renderer->invalidate();        // 拖拽预览走 D2D，合成图不变
            } else if (m_state->dragging) {
                // 拖拽框选过程中立即隐藏快捷键提示层，把全屏视野还给用户
                ShortcutHintOverlay::instance().hide();
                const bool spaceDown = (GetKeyState(VK_SPACE) & 0x8000) != 0;
                if (spaceDown) {
                    // Space 键按住时：平移当前选区（Snipaste 核心交互机制）
                    int dx = m_state->currentCursor.x - m_state->lastMousePos.x;
                    int dy = m_state->currentCursor.y - m_state->lastMousePos.y;
                    m_state->dragStart.x += dx;
                    m_state->dragStart.y += dy;
                    m_state->dragEnd.x += dx;
                    m_state->dragEnd.y += dy;
                } else {
                    m_state->dragEnd = m_state->currentCursor;
                    float targetRatio = getTargetAspectRatio(m_state->aspectRatio);
                    if (targetRatio > 0.0f) {
                        float dx = static_cast<float>(m_state->dragEnd.x - m_state->dragStart.x);
                        float dy = static_cast<float>(m_state->dragEnd.y - m_state->dragStart.y);
                        float signX = (dx >= 0.0f) ? 1.0f : -1.0f;
                        float signY = (dy >= 0.0f) ? 1.0f : -1.0f;
                        float curW = std::abs(dx);
                        float curH = std::abs(dy);
                        if (curW / targetRatio >= curH) {
                            float newH = curW / targetRatio;
                            m_state->dragEnd.y = static_cast<LONG>(m_state->dragStart.y + signY * newH);
                        } else {
                            float newW = curH * targetRatio;
                            m_state->dragEnd.x = static_cast<LONG>(m_state->dragStart.x + signX * newW);
                        }
                    }
                }
                m_state->lastMousePos = m_state->currentCursor;
                m_renderer->invalidate();
            } else {
                if (((int)m_state->state.load() == (int)OverlayState::Idle ||
                     (int)m_state->state.load() == (int)OverlayState::Selecting) && !m_state->dragging) {
                    if (m_state->options.autoDetectWindow) {
                        POINT screenPt = m_state->currentCursor;
                        int offX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                        int offY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                        screenPt.x += offX;
                        screenPt.y += offY;

                        // 光标防抖锁定：若光标物理位移为 0 或已处于高层级且仍在选区包围盒内（含 6px 容差），则保留当前层级，杜绝鼠标微动导致层级强制重置跳变
                        bool preserveHierarchy = false;
                        if (m_state->currentCursor.x == m_state->lastMousePos.x &&
                            m_state->currentCursor.y == m_state->lastMousePos.y &&
                            !m_state->detectedWindowHierarchy.empty()) {
                            preserveHierarchy = true;
                        } else if (m_state->detectedWindowHierarchyIndex > 0 &&
                            m_state->detectedWindow.right > m_state->detectedWindow.left &&
                            m_state->detectedWindow.bottom > m_state->detectedWindow.top) {
                            RECT guardRect = m_state->detectedWindow;
                            guardRect.left -= 6;
                            guardRect.top -= 6;
                            guardRect.right += 6;
                            guardRect.bottom += 6;
                            POINT clientPt = m_state->currentCursor;
                            if (PtInRect(&guardRect, clientPt)) {
                                preserveHierarchy = true;
                            }
                        }

                        if (!preserveHierarchy) {
                            m_state->detectedWindowHierarchy = detectWindowHierarchy(screenPt);
                            m_state->detectedWindowHierarchyIndex = 0;
                            if (!m_state->detectedWindowHierarchy.empty()) {
                                m_state->detectedWindow = m_state->detectedWindowHierarchy[0];
                                m_state->animDetectedWindowRect = D2D1::RectF(
                                    static_cast<float>(m_state->detectedWindow.left),
                                    static_cast<float>(m_state->detectedWindow.top),
                                    static_cast<float>(m_state->detectedWindow.right),
                                    static_cast<float>(m_state->detectedWindow.bottom));
                                m_state->targetDetectedWindowRect = m_state->animDetectedWindowRect;
                                m_state->isHierarchyAnimating = false;
                            } else {
                                m_state->detectedWindow = {};
                                m_state->animDetectedWindowRect = {};
                                m_state->targetDetectedWindowRect = {};
                                m_state->isHierarchyAnimating = false;
                            }
                        }
                    } else {
                        m_state->detectedWindow = {};
                        m_state->animDetectedWindowRect = {};
                        m_state->targetDetectedWindowRect = {};
                        m_state->isHierarchyAnimating = false;
                        m_state->detectedWindowHierarchy.clear();
                        m_state->detectedWindowHierarchyDescriptions.clear();
                    }
                }

                // 探测二维码悬停
                if (!m_state->detectedQrText.empty() && m_state->qrChipRect.right > m_state->qrChipRect.left) {
                    bool hover = (m_state->currentCursor.x >= m_state->qrChipRect.left &&
                                  m_state->currentCursor.x <= m_state->qrChipRect.right &&
                                  m_state->currentCursor.y >= m_state->qrChipRect.top &&
                                  m_state->currentCursor.y <= m_state->qrChipRect.bottom);
                    if (hover != m_state->isQrChipHovered) {
                        m_state->isQrChipHovered = hover;
                        m_renderer->invalidate();
                    }
                }
                m_renderer->invalidate();        // 十字准星/动态放大镜跟随光标
            }

            m_state->lastMousePos = m_state->currentCursor;
            updateHoverCursor(m_state->currentCursor);
            return 0;
        }

        case WM_LBUTTONUP: {
            if (!this) break;
            m_renderer->markMarkupDirty();

            if (m_state->sliderPopup.isDragging) {
                m_state->sliderPopup.isDragging = false;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isAdjustingCornerRadius) {
                m_state->isAdjustingCornerRadius = false;
                if (m_state->beautyShell.enabled) {
                    tools3000::core::ConfigManager::instance().set<int>(
                        "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
                } else {
                    tools3000::core::ConfigManager::instance().set<double>(
                        "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
                }
                prepareMarkupBase();
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isAdjustingSelection) {
                m_state->isAdjustingSelection = false;
                m_state->selAdjustHandle = HitArea::None;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isManipulating) {
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                    const float dpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                    float unscaled = m_state->activeElement->fontSize / dpiScale;
                    tools3000::core::ConfigManager::instance().set<double>(
                        "/capture/markup/text/fontSize", static_cast<double>(unscaled));
                    m_state->activeElement->isEditing = true;
                    SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                }
                m_state->isManipulating = false;
                m_state->dragHandle = HitArea::None;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isMarking) {
                finishMarkup({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (!m_state->dragging) break;
            m_state->dragEnd = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            m_state->dragging = false;

            int w = std::abs(m_state->dragEnd.x - m_state->dragStart.x);
            int h = std::abs(m_state->dragEnd.y - m_state->dragStart.y);

            if (w > 3 && h > 3) {
                m_state->state = OverlayState::Selected;
                prepareMarkupBase();
                if (m_state->options.showShortcutHints) {
                    ShortcutHintOverlay::instance().show(
                        m_state->mode == OverlayMode::RecordRegion
                            ? ShortcutHintContext::RecordSelecting
                            : ShortcutHintContext::CaptureSelected);
                }
            } else {
                // 拖拽太小，视为点击——吸附到检测的窗口
                if (m_state->detectedWindow.right > m_state->detectedWindow.left &&
                    m_state->detectedWindow.bottom > m_state->detectedWindow.top) {
                    m_state->isHierarchyAnimating = false;
                    m_state->animDetectedWindowRect = {};
                    m_state->targetDetectedWindowRect = {};
                    KillTimer(hwnd, RENDER_TIMER_ID);
                    m_state->dragStart = {static_cast<LONG>(m_state->detectedWindow.left),
                                         static_cast<LONG>(m_state->detectedWindow.top)};
                    m_state->dragEnd = {static_cast<LONG>(m_state->detectedWindow.right),
                                       static_cast<LONG>(m_state->detectedWindow.bottom)};
                    m_state->cornerRadius = m_state->detectedWindowCornerRadius; // 自动继承现代 Win11 窗口圆角
                    m_state->state = OverlayState::Selected;
                    prepareMarkupBase();
                    if (m_state->options.showShortcutHints) {
                        ShortcutHintOverlay::instance().show(
                            m_state->mode == OverlayMode::RecordRegion
                                ? ShortcutHintContext::RecordSelecting
                                : ShortcutHintContext::CaptureSelected);
                    }
                } else {
                    if (m_cancelCb) m_cancelCb();
                }
            }
            updateHoverCursor(m_state->currentCursor);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            if (this && (int)m_state->state.load() == (int)OverlayState::Selected) {
                m_renderer->markMarkupDirty();
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (hitTestSelectionBox(pt) == HitArea::CornerRadius) {
                    if (m_state->beautyShell.enabled) {
                        m_state->beautyShell.cornerRadius = (m_state->beautyShell.cornerRadius > 0.0f) ? 0.0f : 16.0f;
                        tools3000::core::ConfigManager::instance().set<int>(
                            "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
                    } else {
                        // 设计师黄金习惯：双击圆角把手在纯直角 (0px) 与推荐圆角 (12px) 之间快速切换
                        m_state->cornerRadius = (m_state->cornerRadius > 0.0f) ? 0.0f : 12.0f;
                        tools3000::core::ConfigManager::instance().set<double>(
                            "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
                    }
                    prepareMarkupBase();
                    m_renderer->invalidate();
                    return 0;
                }
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                    m_state->activeElement->isEditing = true;
                    m_state->currentTool = MarkupTool::Text;
                    m_state->currentColor = m_state->activeElement->color;
                    m_state->currentTextOutline = m_state->activeElement->textOutline;
                    m_state->currentTextOutlineColor = m_state->activeElement->textOutlineColor;
                    m_state->toolbarLayoutValid = false;
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                    SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    return 0;
                }
                int relX = pt.x - std::min(m_state->dragStart.x, m_state->dragEnd.x);
                int relY = pt.y - std::min(m_state->dragStart.y, m_state->dragEnd.y);
                HitResult hit = m_state->markup.getElementAtEx(cv::Point(relX, relY));
                if (hit.element) {
                    if (m_state->activeElement && m_state->activeElement != hit.element) {
                        m_state->activeElement->isActive = false;
                        m_state->activeElement->isEditing = false;
                    }
                    m_state->activeElement = hit.element;
                    m_state->activeElement->isActive = true;
                    m_state->currentTool = hit.element->tool;
                    m_state->currentColor = hit.element->color;
                    m_state->currentStrokeWidth = static_cast<int>(std::round(hit.element->thickness));
                    m_state->currentFillMode = hit.element->fill;
                    m_state->currentLineStyle = hit.element->lineStyle;
                    m_state->currentArrowStyle = hit.element->arrowStyle;
                    m_state->currentNumberShape = hit.element->numberShape;
                    m_state->currentElementCornerRadius = hit.element->cornerRadius;
                    if (hit.element->tool == MarkupTool::Text) {
                        m_state->currentTextOutline = hit.element->textOutline;
                        m_state->currentTextOutlineColor = hit.element->textOutlineColor;
                    }
                    m_state->toolbarLayoutValid = false;

                    if (hit.element->tool == MarkupTool::Text) {
                        m_state->activeElement->isEditing = true;
                        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    } else {
                        m_state->activeElement->isEditing = false;
                    }
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                    return 0;
                }
                if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            if (!this) break;
            
            // 1. 正在拖拽选区中：右键取消本次拖拽
            if (m_state->dragging) {
                m_state->dragging = false;
                m_state->state = OverlayState::Selecting;
                m_renderer->invalidate();
                return 0;
            }

            // 2. 正在编辑文字：右键退出文字编辑
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                m_state->activeElement->isEditing = false;
                if (m_state->activeElement->text.empty()) {
                    m_state->markup.removeElement(m_state->activeElement->id);
                    m_state->activeElement = nullptr;
                }
                m_renderer->invalidate();
                return 0;
            }

            // 3. 有激活选中的标注元素：取消选中
            if (m_state->activeElement) {
                m_state->activeElement->isActive = false;
                m_state->activeElement = nullptr;
                m_renderer->invalidate();
                return 0;
            }

            // 4. 处于选区态（Selected/Marking）：右键重置选区回到初始未选区态
            if ((int)m_state->state.load() == (int)OverlayState::Selected || (int)m_state->state.load() == (int)OverlayState::Marking) {
                m_state->state = OverlayState::Selecting;
                m_state->dragStart = {0, 0};
                m_state->dragEnd = {0, 0};
                m_state->isHierarchyAnimating = false;
                m_state->animDetectedWindowRect = {};
                m_state->targetDetectedWindowRect = {};
                m_state->markup.clearAll();
                m_renderer->invalidate();
                if (m_state->options.showShortcutHints) {
                    ShortcutHintOverlay::instance().show(
                        m_state->mode == OverlayMode::RecordRegion
                            ? ShortcutHintContext::RecordSelecting
                            : ShortcutHintContext::CaptureSelecting);
                }
                return 0;
            }

            // 5. 无选区状态下：鼠标右键直接退出截图
            ShortcutHintOverlay::instance().hide();
            if (m_cancelCb) {
                m_cancelCb();
            }
            return 0;
        }

        case WM_IME_STARTCOMPOSITION: {
            if (this && m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                HIMC hImc = ImmGetContext(hwnd);
                if (hImc) {
                    COMPOSITIONFORM cf{};
                    cf.dwStyle = CFS_POINT;
                    auto selRect = currentSelectionRect();
                    cf.ptCurrentPos.x = static_cast<LONG>(selRect.left + m_state->activeElement->startPt.x);
                    cf.ptCurrentPos.y = static_cast<LONG>(selRect.top + m_state->activeElement->startPt.y + m_state->activeElement->fontSize + 6);
                    ImmSetCompositionWindow(hImc, &cf);
                    ImmReleaseContext(hwnd, hImc);
                }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_IME_COMPOSITION: {
            if (this && m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                if (lParam & GCS_RESULTSTR) {
                    HIMC hImc = ImmGetContext(hwnd);
                    if (hImc) {
                        LONG bytes = ImmGetCompositionStringW(hImc, GCS_RESULTSTR, nullptr, 0);
                        if (bytes > 0) {
                            std::wstring resultStr(bytes / sizeof(wchar_t), L'\0');
                            ImmGetCompositionStringW(hImc, GCS_RESULTSTR, &resultStr[0], bytes);

                            std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                            wstr.append(resultStr);
                            m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                            m_state->activeElement->textRenderSize = cv::Size(0, 0);
                            m_renderer->markMarkupDirty();
                            m_renderer->invalidate();
                        }
                        ImmReleaseContext(hwnd, hImc);
                        return 0; // 消费输入法上屏文字，防止 DefWindowProcW 再次派发导致重复
                    }
                }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_IME_CHAR: {
            return 0;
        }

        case WM_CHAR: {
            if (this && m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                wchar_t ch = static_cast<wchar_t>(wParam);
                if (ch == 0x08) { // Backspace
                    std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                    if (!wstr.empty()) {
                        wstr.pop_back();
                        m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                    }
                } else if (ch == 0x0D || ch == 0x0A) { // Enter 提交
                    m_state->activeElement->isEditing = false;
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                } else if (ch >= 0x20 || ch == 0x09) {
                    std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                    wstr.push_back(ch);
                    m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                    m_state->activeElement->textRenderSize = cv::Size(0, 0);
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                }
                return 0;
            }
            break;
        }

        case WM_MOUSEWHEEL: {
            if (!this) break;
            m_renderer->markMarkupDirty();
            short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);

            // 1. 选区前/未拖拽状态：滚轮切换探测窗口层级 (子控件 <-> 父容器 <-> 顶级窗口 <-> 全屏)
            if (((int)m_state->state.load() == (int)OverlayState::Idle ||
                 (int)m_state->state.load() == (int)OverlayState::Selecting) && !m_state->dragging) {
                if (!m_state->detectedWindowHierarchy.empty()) {
                    int count = static_cast<int>(m_state->detectedWindowHierarchy.size());
                    int prevIdx = m_state->detectedWindowHierarchyIndex;
                    if (zDelta > 0) {
                        m_state->detectedWindowHierarchyIndex = std::min(m_state->detectedWindowHierarchyIndex + 1, count - 1);
                    } else if (zDelta < 0) {
                        m_state->detectedWindowHierarchyIndex = std::max(m_state->detectedWindowHierarchyIndex - 1, 0);
                    }
                    if (m_state->detectedWindowHierarchyIndex != prevIdx) {
                        RECT targetRc = m_state->detectedWindowHierarchy[m_state->detectedWindowHierarchyIndex];
                        m_state->detectedWindow = targetRc;

                        // 启动阻尼弹簧微动效 (Spring Damping Lerp)
                        if (m_state->animDetectedWindowRect.right <= m_state->animDetectedWindowRect.left) {
                            RECT prevRc = m_state->detectedWindowHierarchy[prevIdx];
                            m_state->animDetectedWindowRect = D2D1::RectF(
                                static_cast<float>(prevRc.left), static_cast<float>(prevRc.top),
                                static_cast<float>(prevRc.right), static_cast<float>(prevRc.bottom));
                        }
                        m_state->targetDetectedWindowRect = D2D1::RectF(
                            static_cast<float>(targetRc.left), static_cast<float>(targetRc.top),
                            static_cast<float>(targetRc.right), static_cast<float>(targetRc.bottom));
                        m_state->animVelocity = D2D1::RectF(0, 0, 0, 0);
                        m_state->isHierarchyAnimating = true;
                        m_state->hierarchyBadgeFadeOutUntil = GetTickCount() + 1800;

                        int idx = m_state->detectedWindowHierarchyIndex;
                        std::wstring desc = (idx >= 0 && idx < (int)m_state->detectedWindowHierarchyDescriptions.size())
                            ? m_state->detectedWindowHierarchyDescriptions[idx]
                            : L"选区";
                        m_state->hierarchyBadgeText = L"[" + std::to_wstring(idx + 1) + L"/" + std::to_wstring(count) + L"] " + desc;

                        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                        m_renderer->invalidate();
                        return 0;
                    }
                    return 0;
                }
            }

            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                m_state->activeElement->magnifierScale += (zDelta > 0) ? 0.2f : -0.2f;
                m_state->activeElement->magnifierScale = std::clamp(m_state->activeElement->magnifierScale, 1.0f, 8.0f);
            } else if (m_state->currentTool == MarkupTool::Magnifier && !m_state->isMarking && !m_state->isManipulating) {
                m_state->dynamicMagnifierScale += (zDelta > 0) ? 0.2f : -0.2f;
                m_state->dynamicMagnifierScale = std::clamp(m_state->dynamicMagnifierScale, 1.0f, 8.0f);
            } else if (m_state->sliderPopup.type != SliderPopupType::None ||
                       ((int)m_state->state.load() == (int)OverlayState::Selected &&
                        (m_state->currentTool == MarkupTool::Rectangle || m_state->currentTool == MarkupTool::Ellipse ||
                         m_state->currentTool == MarkupTool::Pen || m_state->currentTool == MarkupTool::Arrow ||
                         m_state->currentTool == MarkupTool::Text || m_state->currentTool == MarkupTool::Number ||
                         m_state->currentTool == MarkupTool::Mosaic))) {
                int delta = (zDelta > 0) ? 1 : -1;
                if (m_state->sliderPopup.type == SliderPopupType::SelectionCornerRadius) {
                    if (m_state->beautyShell.enabled) {
                        float newRad = std::clamp(m_state->beautyShell.cornerRadius + static_cast<float>(delta * 2), 0.0f, 32.0f);
                        m_state->beautyShell.cornerRadius = newRad;
                        tools3000::core::ConfigManager::instance().set<int>(
                            "/capture/beautyShellRadius", static_cast<int>(std::round(newRad)));
                    } else {
                        float newRad = std::clamp(m_state->cornerRadius + static_cast<float>(delta * 2), 0.0f, 80.0f);
                        m_state->cornerRadius = newRad;
                        tools3000::core::ConfigManager::instance().set<double>(
                            "/screenshot/cornerRadius", static_cast<double>(newRad));
                    }
                    prepareMarkupBase();
                } else if (m_state->sliderPopup.type == SliderPopupType::CornerRadius || (GetKeyState(VK_SHIFT) & 0x8000)) {
                    float newRad = std::clamp(m_state->currentElementCornerRadius + static_cast<float>(delta * 2), 0.0f, 40.0f);
                    m_state->currentElementCornerRadius = newRad;
                    if (m_state->activeElement) {
                        m_state->activeElement->cornerRadius = newRad;
                    }
                } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize || m_state->currentTool == MarkupTool::Text) {
                    float curSize = (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text)
                        ? m_state->activeElement->fontSize
                        : static_cast<float>(tools3000::core::ConfigManager::instance().get<double>("/capture/markup/text/fontSize", 18.0));
                    float newSize = std::clamp(curSize + static_cast<float>(delta * 2), 10.0f, 144.0f);
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                        m_state->activeElement->fontSize = newSize;
                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                    }
                    const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                    tools3000::core::ConfigManager::instance().set<double>(
                        "/capture/markup/text/fontSize", static_cast<double>(newSize) / curDpi);
                } else {
                    int newW = std::clamp(m_state->currentStrokeWidth + delta, 1, 28);
                    m_state->currentStrokeWidth = newW;
                    if (m_state->activeElement) {
                        m_state->activeElement->thickness = static_cast<float>(newW);
                    }
                }
                m_renderer->markMarkupDirty();
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
            }
            return 0;
        }


        case WM_KEYDOWN: {
            if (!this) break;
            m_renderer->markMarkupDirty();

            bool editingText = m_state->activeElement &&
                               m_state->activeElement->tool == MarkupTool::Text &&
                               m_state->activeElement->isEditing;
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool hasSelection = ((int)m_state->state.load() == (int)OverlayState::Selected ||
                                 (int)m_state->state.load() == (int)OverlayState::Marking);

            // 历史回放快捷键（Snipaste 经典模式：按 ',' / '.' 直接穿梭历史）
            if (wParam == VK_OEM_2) { // '/'
                m_state->historyMode = !m_state->historyMode;
                if (m_state->historyMode) {
                    m_state->historyIndex = 0;
                    m_renderer->updateHistoryBitmap(*m_state);
                }
                m_renderer->invalidate();
                return 0;
            }
            if ((wParam == VK_OEM_COMMA || wParam == VK_OEM_PERIOD) && !ctrl && !editingText) {
                int total = CaptureHistory::instance().count();
                if (total > 0) {
                    if (!m_state->historyMode) {
                        m_state->historyMode = true;
                        m_state->historyIndex = 0;
                    } else if (wParam == VK_OEM_COMMA) {
                        if (m_state->historyIndex < total - 1) m_state->historyIndex++;
                    } else if (wParam == VK_OEM_PERIOD) {
                        if (m_state->historyIndex > 0) m_state->historyIndex--;
                    }
                    m_renderer->updateHistoryBitmap(*m_state);
                    m_renderer->invalidate();
                    return 0;
                }
            }
            if (m_state->historyMode) {
                // 在历史模式下，按ESC退出
                if (wParam == VK_ESCAPE) {
                    m_state->historyMode = false;
                    m_renderer->invalidate();
                    return 0;
                }
                return 0;
            }

            // 方向键 / WASD 像素级微调与光标移动
            if (!editingText) {
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                bool isDirectionKey = (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_LEFT || wParam == VK_RIGHT ||
                                       (!ctrl && (wParam == 'W' || wParam == 'S' || wParam == 'A' || wParam == 'D')));
                
                // 存在选区时：方向键 / WASD 移动选区，Shift+方向键 / Shift+WASD 调整选区尺寸
                if (hasSelection && isDirectionKey) {
                    int step = ctrl ? 10 : 1;
                    int dx = (wParam == VK_LEFT || wParam == 'A' ? -step : (wParam == VK_RIGHT || wParam == 'D' ? step : 0));
                    int dy = (wParam == VK_UP   || wParam == 'W' ? -step : (wParam == VK_DOWN  || wParam == 'S' ? step : 0));
                    
                    if (m_state->activeElement) {
                        m_state->activeElement->moveBy(dx, dy);
                    } else if (shift) {
                        adjustSelection(HitArea::RB, dx, dy);
                    } else {
                        adjustSelection(HitArea::Body, dx, dy);
                    }
                    prepareMarkupBase();
                    m_renderer->invalidate();
                    return 0;
                }
                
                // 无选区时：若存在探测层级且按下 W / S，切换探测层级；否则 WASD/方向键微调光标
                if (!hasSelection) {
                    if (!m_state->dragging && !m_state->detectedWindowHierarchy.empty() && !ctrl &&
                        (wParam == 'W' || wParam == 'S')) {
                        int count = static_cast<int>(m_state->detectedWindowHierarchy.size());
                        int prevIdx = m_state->detectedWindowHierarchyIndex;
                        if (wParam == 'W') {
                            m_state->detectedWindowHierarchyIndex = std::min(m_state->detectedWindowHierarchyIndex + 1, count - 1);
                        } else {
                            m_state->detectedWindowHierarchyIndex = std::max(m_state->detectedWindowHierarchyIndex - 1, 0);
                        }
                        if (m_state->detectedWindowHierarchyIndex != prevIdx) {
                            RECT targetRc = m_state->detectedWindowHierarchy[m_state->detectedWindowHierarchyIndex];
                            m_state->detectedWindow = targetRc;

                            // 启动阻尼弹簧微动效 (Spring Damping Lerp)
                            if (m_state->animDetectedWindowRect.right <= m_state->animDetectedWindowRect.left) {
                                RECT prevRc = m_state->detectedWindowHierarchy[prevIdx];
                                m_state->animDetectedWindowRect = D2D1::RectF(
                                    static_cast<float>(prevRc.left), static_cast<float>(prevRc.top),
                                    static_cast<float>(prevRc.right), static_cast<float>(prevRc.bottom));
                            }
                            m_state->targetDetectedWindowRect = D2D1::RectF(
                                static_cast<float>(targetRc.left), static_cast<float>(targetRc.top),
                                static_cast<float>(targetRc.right), static_cast<float>(targetRc.bottom));
                            m_state->animVelocity = D2D1::RectF(0, 0, 0, 0);
                            m_state->isHierarchyAnimating = true;
                            m_state->hierarchyBadgeFadeOutUntil = GetTickCount() + 1800;

                            int idx = m_state->detectedWindowHierarchyIndex;
                            std::wstring desc = (idx >= 0 && idx < (int)m_state->detectedWindowHierarchyDescriptions.size())
                                ? m_state->detectedWindowHierarchyDescriptions[idx]
                                : L"选区";
                            m_state->hierarchyBadgeText = L"[" + std::to_wstring(idx + 1) + L"/" + std::to_wstring(count) + L"] " + desc;

                            SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                            m_renderer->invalidate();
                            return 0;
                        }
                        return 0;
                    }

                    POINT pt;
                    GetCursorPos(&pt);
                    bool moved = false;
                    if (wParam == 'W' || wParam == VK_UP) { pt.y -= 1; moved = true; }
                    if (wParam == 'S' || wParam == VK_DOWN) { pt.y += 1; moved = true; }
                    if (wParam == 'A' || wParam == VK_LEFT) { pt.x -= 1; moved = true; }
                    if (wParam == 'D' || wParam == VK_RIGHT) { pt.x += 1; moved = true; }
                    if (moved) {
                        SetCursorPos(pt.x, pt.y);
                        m_renderer->invalidate();
                        return 0;
                    }
                }
            }

            // Shift: 选区前/拖拽中依次循环切换 8 种常用颜色格式
            if (wParam == VK_SHIFT && !ctrl &&
                ((int)m_state->state.load() == (int)OverlayState::Idle ||
                 (int)m_state->state.load() == (int)OverlayState::Selecting)) {
                int nextIdx = (static_cast<int>(m_state->colorFormat) + 1) % static_cast<int>(ColorFormatType::COUNT);
                m_state->colorFormat = static_cast<ColorFormatType>(nextIdx);
                m_state->colorFormatHex = (m_state->colorFormat == ColorFormatType::HEX);
                m_renderer->invalidate();
                return 0;
            }

            // 选区锁定比例快捷键：Shift+1(自由), Shift+2(16:9), Shift+3(4:3), Shift+4(1:1), Shift+5(黄金比例)
            if (!ctrl && !editingText && (GetKeyState(VK_SHIFT) & 0x8000) != 0 && wParam >= '1' && wParam <= '5') {
                switch (wParam) {
                    case '1': m_state->aspectRatio = AspectRatioPreset::Free; break;
                    case '2': m_state->aspectRatio = AspectRatioPreset::Ratio_16_9; break;
                    case '3': m_state->aspectRatio = AspectRatioPreset::Ratio_4_3; break;
                    case '4': m_state->aspectRatio = AspectRatioPreset::Ratio_1_1; break;
                    case '5': m_state->aspectRatio = AspectRatioPreset::Ratio_Golden; break;
                }
                enforceAspectRatioOnCurrentSelection();
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                return 0;
            }

            // 线宽无级快捷键：'+' / '-' 实时调节笔触粗细
            if (!editingText && (wParam == VK_OEM_PLUS || wParam == VK_ADD || wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT)) {
                int delta = (wParam == VK_OEM_PLUS || wParam == VK_ADD) ? 1 : -1;
                m_state->currentStrokeWidth = std::clamp(m_state->currentStrokeWidth + delta, 1, 60);
                if (m_state->activeElement) {
                    m_state->activeElement->thickness = static_cast<float>(m_state->currentStrokeWidth);
                    m_renderer->markMarkupDirty();
                }
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                return 0;
            }

            // 文本重编辑：选中状态下按 Enter 或 F2 直接重新编辑文本
            if (!editingText && (wParam == VK_RETURN || wParam == VK_F2) && m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                m_state->activeElement->isEditing = true;
                m_renderer->markMarkupDirty();
                m_renderer->invalidate();
                SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                return 0;
            }

            // 取色：按 C 复制 HEX，按 Shift+C 复制 RGB (Snipaste / PixPin / CleanShot X 黄金交互)
            if ((wParam == 'C' || wParam == 'I') && !ctrl && !editingText) {
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                int cr = 0, cg = 0, cb = 0;
                if (m_renderer->sampleScreenColor(m_state->currentCursor.x, m_state->currentCursor.y, cr, cg, cb, *m_state)) {
                    ColorFormatType fmt = shift ? ColorFormatType::RGB : ColorFormatType::HEX;
                    std::string colorText = formatColorText(fmt, cr, cg, cb);
                    tools3000::core::WinUtils::copyToClipboard(colorText);
                    m_state->loupeToastUntil = GetTickCount() + 1400;
                    std::wstring wColorText(colorText.begin(), colorText.end());
                    m_state->loupeToastMessage = (shift ? L"✓ 已复制 RGB: " : L"✓ 已复制 HEX: ") + wColorText;
                    m_state->currentColor = MarkupColor(static_cast<uint8_t>(cr), static_cast<uint8_t>(cg), static_cast<uint8_t>(cb));
                    if (m_state->activeElement) {
                        m_state->activeElement->color = m_state->currentColor;
                        m_renderer->markMarkupDirty();
                    }
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                }
                return 0;
            }

            // 美化外壳分享模式切换：按 B 切换开/关，按 Shift+B 循环切换 5 种预设背景
            if ((wParam == 'B' || wParam == 'b') && !ctrl && !editingText) {
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (shift) {
                    int cur = static_cast<int>(m_state->beautyShell.bgType);
                    int next = (cur + 1) % static_cast<int>(BeautyBackgroundType::COUNT);
                    m_state->beautyShell.bgType = static_cast<BeautyBackgroundType>(next);
                    m_state->beautyShell.enabled = true;
                    static const wchar_t* bgNames[] = { L"极光紫蓝", L"晨曦珊瑚", L"翡翠深海", L"曜石黑晶", L"极简浅灰" };
                    std::wstring name = (next >= 0 && next < 5) ? bgNames[next] : L"自定义";
                    m_state->loupeToastUntil = GetTickCount() + 1500;
                    m_state->loupeToastMessage = L"美化外壳背景: " + name;
                } else {
                    m_state->beautyShell.enabled = !m_state->beautyShell.enabled;
                    m_state->loupeToastUntil = GetTickCount() + 1500;
                    m_state->loupeToastMessage = m_state->beautyShell.enabled ? L"✓ 已开启美化外壳模式" : L"✕ 已关闭美化外壳模式";
                }
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                return 0;
            }

            // ESC 分级退出：编辑文字→退出编辑；选中元素→取消选中；有选区→取消选区；否则→关闭截图
            if (wParam == VK_ESCAPE) {
                if (editingText) {
                    m_state->activeElement->isEditing = false;
                    if (m_state->activeElement->text.empty()) {
                        m_state->markup.removeElement(m_state->activeElement->id);
                        m_state->activeElement = nullptr;
                    }
                    m_renderer->invalidate();
                } else if (m_state->activeElement) {
                    m_state->activeElement->isActive = false;
                    m_state->activeElement = nullptr;
                    m_renderer->invalidate();
                } else if ((int)m_state->state.load() == (int)OverlayState::Selected || (int)m_state->state.load() == (int)OverlayState::Marking) {
                    m_state->state = OverlayState::Selecting;
                    m_state->dragStart = {0, 0};
                    m_state->dragEnd = {0, 0};
                    m_state->markup.clearAll();
                    m_renderer->invalidate();
                    if (m_state->options.showShortcutHints) {
                        ShortcutHintOverlay::instance().show(
                            m_state->mode == OverlayMode::RecordRegion
                                ? ShortcutHintContext::RecordSelecting
                                : ShortcutHintContext::CaptureSelecting);
                    }
                } else {
                    ShortcutHintOverlay::instance().hide();
                    if (m_cancelCb) m_cancelCb();
                }
                return 0;
            }

            // Delete / Backspace: 在非文本编辑状态下删除当前选中的标注元素
            if ((wParam == VK_DELETE || wParam == VK_BACK) && !editingText && m_state->activeElement) {
                m_state->markup.removeElement(m_state->activeElement->id);
                m_state->activeElement = nullptr;
                m_renderer->markMarkupDirty();
                m_renderer->invalidate();
                return 0;
            }

            // 正在输入文字：支持 Ctrl+V 粘贴与其余按键交给 WM_CHAR；支持 Ctrl+C 提交并完成截图
            if (editingText) {
                if (ctrl && (wParam == 'V' || wParam == 'v')) {
                    std::string clipText = tools3000::core::WinUtils::captureSelectedText();
                    if (!clipText.empty()) {
                        std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                        wstr.append(tools3000::core::WinUtils::utf8ToWstring(clipText));
                        m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                    }
                    return 0;
                }
                if (ctrl && (wParam == 'C' || wParam == 'c')) {
                    // 文本编辑中按 Ctrl+C：提交文本图元，丝滑完成截图并复制到剪贴板
                    m_state->activeElement->isEditing = false;
                    m_state->activeElement->isActive = false;
                    m_state->activeElement = nullptr;
                    prepareMarkupBase();
                    if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
                    return 0;
                }
                return 0;
            }

            // Ctrl+A: 一键全选当前屏幕
            if (ctrl && wParam == 'A') {
                m_state->dragStart = {0, 0};
                m_state->dragEnd = {m_state->frozenScreen.cols, m_state->frozenScreen.rows};
                m_state->state = OverlayState::Selected;
                prepareMarkupBase();
                m_renderer->invalidate();
                if (m_state->options.showShortcutHints) {
                    ShortcutHintOverlay::instance().show(
                        m_state->mode == OverlayMode::RecordRegion
                            ? ShortcutHintContext::RecordSelecting
                            : ShortcutHintContext::CaptureSelected);
                }
                return 0;
            }

            // 全局命令
            switch (wParam) {
                case VK_OEM_4: // '[' 键：减小选区/外壳圆角
                    if (hasSelection) {
                        if (m_state->beautyShell.enabled) {
                            static const std::array<float, 5> radiuses = {0.0f, 10.0f, 16.0f, 24.0f, 32.0f};
                            for (int i = (int)radiuses.size() - 1; i >= 0; --i) {
                                if (radiuses[i] < m_state->beautyShell.cornerRadius - 0.5f) {
                                    m_state->beautyShell.cornerRadius = radiuses[i];
                                    break;
                                }
                                if (i == 0) m_state->beautyShell.cornerRadius = 0.0f;
                            }
                            tools3000::core::ConfigManager::instance().set<int>(
                                "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
                        } else {
                            float cfgMaxRadius = static_cast<float>(tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0));
                            static const std::array<float, 6> radiuses = {0.0f, 8.0f, 14.0f, 24.0f, 40.0f, 60.0f};
                            for (int i = (int)radiuses.size() - 1; i >= 0; --i) {
                                if (radiuses[i] <= cfgMaxRadius && radiuses[i] < m_state->cornerRadius - 0.5f) {
                                    m_state->cornerRadius = radiuses[i];
                                    break;
                                }
                                if (i == 0) m_state->cornerRadius = 0.0f;
                            }
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
                        }
                        prepareMarkupBase();
                        m_renderer->invalidate();
                        return 0;
                    }
                    break;
                case VK_OEM_6: // ']' 键：增加选区/外壳圆角
                    if (hasSelection) {
                        if (m_state->beautyShell.enabled) {
                            static const std::array<float, 5> radiuses = {0.0f, 10.0f, 16.0f, 24.0f, 32.0f};
                            for (size_t i = 0; i < radiuses.size(); ++i) {
                                if (radiuses[i] > m_state->beautyShell.cornerRadius + 0.5f) {
                                    m_state->beautyShell.cornerRadius = radiuses[i];
                                    break;
                                }
                                if (i == radiuses.size() - 1) m_state->beautyShell.cornerRadius = 32.0f;
                            }
                            tools3000::core::ConfigManager::instance().set<int>(
                                "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
                        } else {
                            float cfgMaxRadius = static_cast<float>(tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0));
                            static const std::array<float, 6> radiuses = {0.0f, 8.0f, 14.0f, 24.0f, 40.0f, 60.0f};
                            for (size_t i = 0; i < radiuses.size(); ++i) {
                                if (radiuses[i] <= cfgMaxRadius && radiuses[i] > m_state->cornerRadius + 0.5f) {
                                    m_state->cornerRadius = radiuses[i];
                                    break;
                                }
                                if (i == radiuses.size() - 1) m_state->cornerRadius = std::min(60.0f, cfgMaxRadius);
                            }
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
                        }
                        prepareMarkupBase();
                        m_renderer->invalidate();
                        return 0;
                    }
                    break;
                case VK_RETURN:
                    if (m_state->isMarking) {
                        finishMarkup(m_state->markupEnd);
                    }
                    if (hasSelection) {
                        if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
                        return 0;
                    } else if (m_state->detectedWindow.right > m_state->detectedWindow.left &&
                               m_state->detectedWindow.bottom > m_state->detectedWindow.top) {
                        m_state->dragStart = {static_cast<LONG>(m_state->detectedWindow.left),
                                             static_cast<LONG>(m_state->detectedWindow.top)};
                        m_state->dragEnd = {static_cast<LONG>(m_state->detectedWindow.right),
                                           static_cast<LONG>(m_state->detectedWindow.bottom)};
                        m_state->cornerRadius = m_state->detectedWindowCornerRadius;
                        m_state->state = OverlayState::Selected;
                        prepareMarkupBase();
                        if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
                        return 0;
                    }
                    return 0;
                case 'C':
                    if (ctrl) {
                        if (m_state->isMarking) {
                            finishMarkup(m_state->markupEnd);
                        }
                        if (hasSelection) {
                            if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
                            return 0;
                        } else if (m_state->dragging) {
                            m_state->dragging = false;
                            m_state->state = OverlayState::Selected;
                            prepareMarkupBase();
                            if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
                            return 0;
                        } else if (m_state->detectedWindow.right > m_state->detectedWindow.left &&
                                   m_state->detectedWindow.bottom > m_state->detectedWindow.top) {
                            m_state->dragStart = {static_cast<LONG>(m_state->detectedWindow.left),
                                                 static_cast<LONG>(m_state->detectedWindow.top)};
                            m_state->dragEnd = {static_cast<LONG>(m_state->detectedWindow.right),
                                               static_cast<LONG>(m_state->detectedWindow.bottom)};
                            m_state->cornerRadius = m_state->detectedWindowCornerRadius;
                            m_state->state = OverlayState::Selected;
                            prepareMarkupBase();
                            if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
                            return 0;
                        }
                    }
                    break;
                case 'T':
                    if (ctrl && hasSelection) {
                        ToolbarButton pinBtn;
                        pinBtn.command = ToolbarCommand::PinWindow;
                        executeToolbarCommand(pinBtn);
                        return 0;
                    } else if (!ctrl) {
                        setCurrentTool(MarkupTool::Text);
                        return 0;
                    }
                    break;
                case 'S':
                    if (ctrl && hasSelection) {
                        std::array<wchar_t, 32768> fileBuffer{};
                        const auto defaultExtension = extensionForFormat(m_state->options.format);
                        const auto defaultName = std::wstring(L"Tools3000_Screenshot.") + defaultExtension;
                        wcsncpy_s(fileBuffer.data(), fileBuffer.size(), defaultName.c_str(), _TRUNCATE);
                        OPENFILENAMEW ofn{};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = hwnd;
                        ofn.lpstrFile = fileBuffer.data();
                        ofn.nMaxFile = static_cast<DWORD>(fileBuffer.size());
                        ofn.lpstrFilter = L"PNG Image (*.png)\0*.png\0JPEG Image (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0Bitmap (*.bmp)\0*.bmp\0WebP Image (*.webp)\0*.webp\0";
                        ofn.nFilterIndex = filterIndexForFormat(m_state->options.format);
                        ofn.lpstrDefExt = defaultExtension;
                        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                                    OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
                        if (GetSaveFileNameW(&ofn)) {
                            std::filesystem::path selected(fileBuffer.data());
                            if (!selected.has_extension()) {
                                selected += L".";
                                selected += extensionForFormat(formatForSavePath(selected, ofn.nFilterIndex));
                            }
                            if (m_confirmCb) {
                                m_confirmCb({
                                    CaptureCompletionAction::SaveAs,
                                    tools3000::core::WinUtils::wstringToUtf8(selected.wstring()),
                                    formatForSavePath(selected, ofn.nFilterIndex)});
                            }
                        }
                    } else if (!ctrl) {
                        setCurrentTool(MarkupTool::Spotlight);
                        return 0;
                    }
                    break;
                case 'Z':
                    if (ctrl) {
                        m_state->activeElement = nullptr;
                        m_state->markup.undo();
                        m_renderer->markMarkupDirty();
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                    }
                    return 0;
                case 'Y':
                    if (ctrl) {
                        m_state->activeElement = nullptr;
                        m_state->markup.redo();
                        m_renderer->markMarkupDirty();
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                    }
                    return 0;
                case VK_DELETE:
                    if (m_state->activeElement) {
                        m_state->markup.removeElement(m_state->activeElement->id);
                        m_state->activeElement = nullptr;
                        m_renderer->markMarkupDirty();
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                    }
                    return 0;
                case '1':
                    if (!ctrl) {
                        m_state->currentColor = MarkupColor::Red();
                        if (m_state->activeElement) { m_state->activeElement->color = m_state->currentColor; m_renderer->markMarkupDirty(); }
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                        return 0;
                    }
                    break;
                case '2':
                    if (!ctrl) {
                        m_state->currentColor = MarkupColor::Yellow();
                        if (m_state->activeElement) { m_state->activeElement->color = m_state->currentColor; m_renderer->markMarkupDirty(); }
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                        return 0;
                    }
                    break;
                case '3':
                    if (!ctrl) {
                        m_state->currentColor = MarkupColor::Green();
                        if (m_state->activeElement) { m_state->activeElement->color = m_state->currentColor; m_renderer->markMarkupDirty(); }
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                        return 0;
                    }
                    break;
                case '4':
                    if (!ctrl) {
                        m_state->currentColor = MarkupColor::Blue();
                        if (m_state->activeElement) { m_state->activeElement->color = m_state->currentColor; m_renderer->markMarkupDirty(); }
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                        return 0;
                    }
                    break;
                case '5':
                    if (!ctrl) {
                        m_state->currentColor = MarkupColor::White();
                        if (m_state->activeElement) { m_state->activeElement->color = m_state->currentColor; m_renderer->markMarkupDirty(); }
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                        return 0;
                    }
                    break;
                case 'R': if (!ctrl) { setCurrentTool(MarkupTool::Rectangle); return 0; } break;
                case 'A': if (!ctrl) { setCurrentTool(MarkupTool::Arrow);     return 0; } break;
                case 'O': case 'E': if (!ctrl) { setCurrentTool(MarkupTool::Ellipse); return 0; } break;
                case 'P': if (!ctrl) { setCurrentTool(MarkupTool::Pen);       return 0; } break;
                case 'H': if (!ctrl) { setCurrentTool(MarkupTool::Highlight); return 0; } break;
                case 'M': if (!ctrl) { setCurrentTool(MarkupTool::Mosaic);    return 0; } break;
                case 'N': if (!ctrl) { setCurrentTool(MarkupTool::Number);    return 0; } break;
                case 'G': if (!ctrl) { setCurrentTool(MarkupTool::Magnifier); return 0; } break;
                case 'W': if (!ctrl) { setCurrentTool(MarkupTool::Watermark); return 0; } break;
                case 'I': if (!ctrl) { setCurrentTool(MarkupTool::Inpaint);   return 0; } break;
                case VK_UP:
                case VK_DOWN:
                case VK_LEFT:
                case VK_RIGHT: {
                    const bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    const int step = isShift ? 10 : 1;
                    int dx = 0, dy = 0;
                    if (wParam == VK_LEFT)  dx = -step;
                    if (wParam == VK_RIGHT) dx = step;
                    if (wParam == VK_UP)    dy = -step;
                    if (wParam == VK_DOWN)  dy = step;

                    POINT pt;
                    GetCursorPos(&pt);
                    pt.x += dx;
                    pt.y += dy;
                    SetCursorPos(pt.x, pt.y);

                    m_state->currentCursor = {pt.x, pt.y};
                    if (m_state->dragging) {
                        m_state->dragEnd = m_state->currentCursor;
                    }
                    m_renderer->invalidate();
                    return 0;
                }
                default: break;
            }
            return 0;
        }

        case WM_TIMER: {
            if (this && wParam == RENDER_TIMER_ID) {
                if (m_state->isFadingOut) {
                    DWORD elapsed = GetTickCount() - m_state->fadeOutStart;
                    if (elapsed >= 150) {
                        if (m_cancelCb) m_cancelCb();
                    } else {
                        float alpha = 1.0f - (elapsed / 150.0f);
                        SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(alpha * 255), LWA_ALPHA);
                    }
                    return 0;
                }

                // 智能选区阻尼弹簧微动效 (Spring Damping Lerp)
                if (m_state->isHierarchyAnimating) {
                    constexpr float k = 280.0f;
                    constexpr float d = 30.0f;
                    constexpr float dt = 0.016f;

                    auto stepSpring = [&](float& pos, float target, float& vel) -> bool {
                        float force = -k * (pos - target) - d * vel;
                        vel += force * dt;
                        pos += vel * dt;
                        if (std::abs(pos - target) < 0.5f && std::abs(vel) < 1.0f) {
                            pos = target;
                            vel = 0.0f;
                            return true;
                        }
                        return false;
                    };

                    bool lOk = stepSpring(m_state->animDetectedWindowRect.left, m_state->targetDetectedWindowRect.left, m_state->animVelocity.left);
                    bool tOk = stepSpring(m_state->animDetectedWindowRect.top, m_state->targetDetectedWindowRect.top, m_state->animVelocity.top);
                    bool rOk = stepSpring(m_state->animDetectedWindowRect.right, m_state->targetDetectedWindowRect.right, m_state->animVelocity.right);
                    bool bOk = stepSpring(m_state->animDetectedWindowRect.bottom, m_state->targetDetectedWindowRect.bottom, m_state->animVelocity.bottom);

                    if (lOk && tOk && rOk && bOk) {
                        m_state->isHierarchyAnimating = false;
                        m_state->animDetectedWindowRect = m_state->targetDetectedWindowRect;
                    }
                    m_renderer->invalidate();
                }
                
                // 取色放大镜：复制成功提示的存续期间持续重绘，到期后再渲染一帧将其清除
                if (m_state->loupeToastUntil != 0) {
                    m_renderer->invalidate();
                    if (GetTickCount() >= m_state->loupeToastUntil) m_state->loupeToastUntil = 0;
                }
                // 文字编辑期间需持续重算以保留闪烁光标；其余情况按脏标记重绘。
                bool editingText = m_state->activeElement &&
                                   m_state->activeElement->tool == MarkupTool::Text &&
                                   m_state->activeElement->isEditing;
                if (editingText) {
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                }
                if (m_renderer->needsRender()) {
                    m_renderer->clearNeedsRender();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }

                // 动效已结束且微晶徽章已过淡出时间、无取色提示、无文字编辑，销毁计时器实现零空闲开销
                if (!m_state->isHierarchyAnimating &&
                    GetTickCount() >= m_state->hierarchyBadgeFadeOutUntil &&
                    m_state->loupeToastUntil == 0 && !editingText) {
                    KillTimer(hwnd, RENDER_TIMER_ID);
                }
            }
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    } catch (const std::exception& e) {
        LOG_ERROR("CaptureOverlay 窗口过程异常: {}", e.what());
    } catch (...) {
        LOG_ERROR("CaptureOverlay 窗口过程未知异常");
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CaptureInput::openDropdownMenu(DropdownType type, const D2D1_RECT_F& anchorRect) {
    if (!m_state) return;
    tools3000::capture::openDropdownMenu(*m_state, type, anchorRect);
    if (m_renderer) m_renderer->invalidate();
}

void CaptureInput::commitDropdownSelection(DropdownType type, int id) {
    if (!m_state) return;
    tools3000::capture::commitDropdownSelection(*m_state, type, id);
    if (m_renderer) m_renderer->invalidate();
}

} // namespace tools3000::capture
