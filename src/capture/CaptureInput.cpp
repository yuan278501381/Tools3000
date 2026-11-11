#define RENDER_TIMER_ID 1
#include <algorithm>
#include <windowsx.h>
#include "capture/CaptureInput.h"
#include "capture/CaptureOverlay.h"
#include "capture/CaptureHistory.h"
#include "capture/ClipboardUtils.h"
#include "capture/PinWindow.h"
#include "capture/ScrollCapture.h"
#include "capture/ScrollCaptureOverlay.h"
#include "capture/ScrollCaptureEditorOverlay.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/CornerRadiusHelper.h"
#include "capture/CaptureToolbarLayout.h"
#include "capture/CaptureToolbarAccessibility.h"
#include "capture/CaptureInteractionPolicy.h"
#include "capture/CaptureShortcutPolicy.h"
#include "capture/CaptureTextEditor.h"
#include "capture/CaptureDropdownMenu.h"
#include "capture/CaptureHudPalette.h"
#include "capture/MarkupBaseHelper.h"
#include "core/config/ConfigManager.h"
#include "core/accessibility/OverlayAnnouncement.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
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

HCURSOR CaptureInput::getCornerRadiusLiveCursor(int cornerIndex, float dpiScale, bool isDragging) {
    return CornerRadiusHelper::getLiveCursor(cornerIndex, dpiScale, isDragging);
}

static HCURSOR cursorForArea(HitArea area, int cornerIndex = 0, float dpiScale = 1.0f, bool isDragging = false) {
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
        case HitArea::LensOffset:
            return LoadCursor(nullptr, IDC_SIZEALL);
        case HitArea::FocusSource:
            return LoadCursor(nullptr, IDC_HAND);
        case HitArea::CornerRadius:
            return CaptureInput::getCornerRadiusLiveCursor(cornerIndex, dpiScale, isDragging);
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


static void pushCoalescedElementUndo(CaptureState& state, DWORD coalesceWindowMs = 450) {
    if (!state.activeElement) return;
    const DWORD now = GetTickCount();
    const uint32_t id = state.activeElement->id;
    if (id != 0 &&
        state.lastCoalescedUndoElemId == id &&
        (now - state.lastCoalescedUndoTick) <= coalesceWindowMs) {
        state.lastCoalescedUndoTick = now;
        return;
    }
    state.markup.pushUndoSnapshot();
    state.lastCoalescedUndoElemId = id;
    state.lastCoalescedUndoTick = now;
}

static void resetCoalescedUndo(CaptureState& state) noexcept {
    state.lastCoalescedUndoElemId = 0;
    state.lastCoalescedUndoTick = 0;
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
    const float curDpi = (m_state && m_state->dpiScale > 0.0f) ? m_state->dpiScale : 1.0f;
    auto st = m_state->state.load();
    if ((int)st == (int)OverlayState::Selected || (int)st == (int)OverlayState::Marking) {
        // ========== Level 1: 进行中的交互拖拽操作 (最高优先级) ==========
        if (m_state->isDraggingToolbar) {
            cur = LoadCursor(nullptr, IDC_SIZEALL);
        } else if (m_state->isSelectingText) {
            cur = LoadCursor(nullptr, IDC_IBEAM);
        } else if (m_state->isAdjustingCornerRadius) {
            cur = getCornerRadiusLiveCursor(m_state->cornerDragIndex, curDpi, true);
        } else if (m_state->isAdjustingSelection) {
            cur = cursorForArea(m_state->selAdjustHandle, 0, curDpi, true);
        } else if (m_state->isManipulating) {
            if (m_state->dragHandle == HitArea::CornerRadius) {
                cur = getCornerRadiusLiveCursor(m_state->cornerDragIndex, curDpi, true);
            } else if (m_state->dragHandle == HitArea::TextMoveBorder) {
                cur = LoadCursor(nullptr, IDC_SIZEALL);
            } else if (m_state->dragHandle != HitArea::None) {
                cur = cursorForArea(m_state->dragHandle, m_state->cornerDragIndex, curDpi, true);
            } else {
                cur = LoadCursor(nullptr, IDC_SIZEALL);
            }
        } else if (m_state->sliderPopup.isDragging) {
            cur = LoadCursor(nullptr, IDC_ARROW);
        } else if (m_state->isLongImageMode() &&
                   (m_state->longCanvas.middleButtonPanning ||
                    m_state->longCanvas.dragMode != CaptureState::LongCanvasDragMode::None)) {
            if (m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::MinimapCropTop ||
                m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::MinimapCropBottom) {
                cur = LoadCursor(nullptr, IDC_SIZENS);
            } else {
                cur = LoadCursor(nullptr, IDC_SIZEALL);
            }
        }

        // ========== Level 2: UI 浮层与控制面板 (手柄/工具栏/滑块/下拉菜单/尺寸菜单/二维码) ==========
        if (!cur && m_state->isLongImageMode()) {
            const float fpx = static_cast<float>(point.x);
            const float fpy = static_cast<float>(point.y);
            const auto inRect = [&](const D2D1_RECT_F& rc) {
                return rc.right > rc.left && fpx >= rc.left && fpx <= rc.right && fpy >= rc.top && fpy <= rc.bottom;
            };
            if (inRect(m_state->longCanvas.topHeaderRect)) {
                if (inRect(m_state->longCanvas.btnJumpTopRect) ||
                    inRect(m_state->longCanvas.btnJumpBottomRect) ||
                    inRect(m_state->longCanvas.btnResetCropRect) ||
                    inRect(m_state->longCanvas.btnZoomToggleRect)) {
                    cur = LoadCursor(nullptr, IDC_HAND);
                } else {
                    cur = LoadCursor(nullptr, IDC_ARROW);
                }
            } else if (inRect(m_state->longCanvas.minimapPanelRect)) {
                const auto& mmImg = m_state->longCanvas.minimapImageRect;
                const int topRow = std::min(m_state->dragStart.y, m_state->dragEnd.y);
                const int botRow = std::max(m_state->dragStart.y, m_state->dragEnd.y);
                const float mmTopY = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(
                    topRow, mmImg, m_state->longCanvas.docHeight);
                const float mmBotY = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(
                    botRow, mmImg, m_state->longCanvas.docHeight);
                const float hitBand = 9.0f * curDpi;
                if (fpx >= mmImg.left - 8.0f * curDpi && fpx <= mmImg.right + 8.0f * curDpi &&
                    (std::abs(fpy - mmTopY) <= hitBand || std::abs(fpy - mmBotY) <= hitBand)) {
                    cur = LoadCursor(nullptr, IDC_SIZENS);
                } else if (inRect(mmImg)) {
                    cur = LoadCursor(nullptr, IDC_HAND);
                } else {
                    cur = LoadCursor(nullptr, IDC_ARROW);
                }
            }
        }
        if (!cur) {
            rebuildToolbarButtons(currentSelectionRect());
            const auto& gr = m_state->toolbarGripRect;
            if (gr.right > gr.left && point.x >= gr.left && point.x <= gr.right &&
                point.y >= gr.top && point.y <= gr.bottom) {
                cur = LoadCursor(nullptr, IDC_SIZEALL);
            } else if (hitTestToolbar(point)) {
                cur = LoadCursor(nullptr, IDC_ARROW);
            } else if (m_state->sliderPopup.type != SliderPopupType::None) {
                auto pr = m_state->sliderPopup.popupRect;
                if (point.x >= pr.left && point.x <= pr.right && point.y >= pr.top && point.y <= pr.bottom) {
                    cur = LoadCursor(nullptr, IDC_ARROW);
                }
            } else if (m_state->dropdownMenu.type != DropdownType::None) {
                const auto dmRect = m_state->dropdownMenu.menuRect;
                if (point.x >= dmRect.left && point.x <= dmRect.right &&
                    point.y >= dmRect.top && point.y <= dmRect.bottom) {
                    cur = LoadCursor(nullptr, IDC_ARROW);
                }
            } else if (m_state->openSubmenu != SubmenuType::None) {
                const auto smRect = m_state->openSubmenuRect;
                if (point.x >= smRect.left && point.x <= smRect.right &&
                    point.y >= smRect.top && point.y <= smRect.bottom) {
                    cur = LoadCursor(nullptr, IDC_ARROW);
                }
            } else if (m_state->isSizeMenuOpen) {
                auto mr = m_state->sizeMenuRect;
                if (point.x >= mr.left && point.x <= mr.right && point.y >= mr.top && point.y <= mr.bottom) {
                    const float layScale = std::clamp(m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
                    const auto lay = computeSizeMenuLayoutFromRect(mr, layScale);
                    const float px = static_cast<float>(point.x);
                    const float py = static_cast<float>(point.y);
                    const auto inR = [&](const D2D1_RECT_F& r) {
                        return px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
                    };
                    const bool overInteractive = inR(lay.segmentTrackRect) || inR(lay.positionRowRect) || inR(lay.unitRowRect);
                    cur = LoadCursor(nullptr, overInteractive ? IDC_HAND : IDC_ARROW);
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
                cur = getCornerRadiusLiveCursor(cornerIdx, curDpi, false);
            } else if (sel != HitArea::None && sel != HitArea::Body) {
                cur = cursorForArea(sel, cornerIdx, curDpi, false);
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
                            cur = getCornerRadiusLiveCursor(cornerIdx, curDpi, false);
                        } else if (elemHit == HitArea::TextMoveBorder) {
                            cur = LoadCursor(nullptr, IDC_SIZEALL);
                        } else if (elemHit == HitArea::Body) {
                            if (m_state->activeElement->tool == MarkupTool::Text) {
                                cur = m_state->activeElement->isEditing
                                    ? LoadCursor(nullptr, IDC_IBEAM)
                                    : LoadCursor(nullptr, IDC_SIZEALL);
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
                        if (hit.element->tool == MarkupTool::Text && hit.area == HitArea::Body) {
                            cur = LoadCursor(nullptr, IDC_IBEAM);
                        } else {
                            cur = cursorForArea(hit.area);
                        }
                    }
                }
            }
        }

        // ========== Level 4.5: 长图工作台按住 Space 临时抓手浏览 ==========
        if (!cur && m_state->isLongImageMode() && ((GetKeyState(VK_SPACE) & 0x8000) != 0)) {
            const auto& stg = m_state->longCanvas.stageRect;
            if (point.x >= stg.left && point.x <= stg.right && point.y >= stg.top && point.y <= stg.bottom) {
                cur = LoadCursor(nullptr, IDC_SIZEALL);
            }
        }

        // ========== Level 5: 选区内部画布 (当前标注工具分发) ==========
        if (!cur) {
            const float effDpi = (m_state && m_state->dpiScale > 0.0f) ? m_state->dpiScale : 1.0f;
            if (isPointInSelection(point)) {
                if (m_state->mode == OverlayMode::RecordRegion || !m_state->isMarkupToolActive) {
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
                // ========== Level 6: 选区外部区域 (长图舞台空白支持抓手滚屏，其余杜绝十字准星泄漏) ==========
                if (m_state->isLongImageMode()) {
                    const auto& stg = m_state->longCanvas.stageRect;
                    if (point.x >= stg.left && point.x <= stg.right &&
                        point.y >= stg.top && point.y <= stg.bottom) {
                        cur = LoadCursor(nullptr, IDC_SIZEALL);
                    } else {
                        cur = LoadCursor(nullptr, IDC_ARROW);
                    }
                } else {
                    cur = LoadCursor(nullptr, IDC_ARROW);
                }
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
    auto docRect = currentSelectionRect();
    if (docRect.right - docRect.left < 1.0f || docRect.bottom - docRect.top < 1.0f) return HitArea::None;

    const float dpiScale = std::clamp(
        m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
    const float hw = 9.0f * dpiScale;   // 控制点命中半径
    float px = static_cast<float>(point.x);
    float py = static_cast<float>(point.y);

    if (m_state->isLongImageMode()) {
        const auto& stg = m_state->longCanvas.stageRect;
        if (stg.right <= stg.left ||
            px < stg.left - 14.0f * dpiScale || px > stg.right + 14.0f * dpiScale ||
            py < stg.top - 14.0f * dpiScale || py > stg.bottom + 14.0f * dpiScale) {
            return HitArea::None;
        }
        const auto r = m_state->docRectToScreen(docRect);
        const float selW = r.right - r.left;
        const float selH = r.bottom - r.top;
        const float zoom = (std::max)(0.05f, m_state->longCanvas.zoom);

        if (CornerRadiusHelper::canShowHandles(selW, selH, dpiScale, 64.0f)) {
            const float effectiveScreenRadius = m_state->effectiveCornerRadius() * zoom;
            const int hitIdx = CornerRadiusHelper::hitTestHandles(
                r.left, r.top, r.right, r.bottom,
                effectiveScreenRadius, px, py, dpiScale, 11.0f * dpiScale, false);
            if (hitIdx >= 0) {
                if (outCornerIndex) *outCornerIndex = hitIdx;
                return HitArea::CornerRadius;
            }
        }

        // 四角微调手柄优先于整根顶底推拉横梁
        struct CornerH { float x, y; HitArea area; };
        const CornerH corners[4] = {
            {r.left,  r.top,    HitArea::LT},
            {r.right, r.top,    HitArea::RT},
            {r.right, r.bottom, HitArea::RB},
            {r.left,  r.bottom, HitArea::LB},
        };
        for (const auto& c : corners) {
            if (std::abs(px - c.x) <= hw && std::abs(py - c.y) <= hw) return c.area;
        }

        // Smartisan 式全宽首尾无损裁切横梁热区（整条顶边/底边与中央胶囊均可直接上下推拉）
        const float beamBandY = 10.5f * dpiScale;
        if (px >= r.left - hw && px <= r.right + hw) {
            if (std::abs(py - r.top) <= beamBandY) return HitArea::T;
            if (std::abs(py - r.bottom) <= beamBandY) return HitArea::B;
        }

        // 左右两侧微调裁切边线热区
        const float sideBandX = 7.5f * dpiScale;
        if (py >= r.top - hw && py <= r.bottom + hw) {
            if (std::abs(px - r.left) <= sideBandX) return HitArea::L;
            if (std::abs(px - r.right) <= sideBandX) return HitArea::R;
        }
        return HitArea::None;
    }

    auto r = docRect;
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;

    struct H { float x, y; HitArea area; };
    const H handles[8] = {
        {r.left,  r.top,    HitArea::LT}, {cx,      r.top,    HitArea::T},
        {r.right, r.top,    HitArea::RT}, {r.right, cy,       HitArea::R},
        {r.right, r.bottom, HitArea::RB}, {cx,      r.bottom, HitArea::B},
        {r.left,  r.bottom, HitArea::LB}, {r.left,  cy,       HitArea::L},
    };

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

    if (m_state->isLongImageMode() && handle == HitArea::Body) {
        return;
    }

    float maxW = 1.0e6f, maxH = 1.0e6f;
    if (m_state->isLongImageMode()) {
        maxW = static_cast<float>(m_state->longCanvas.docWidth);
        maxH = static_cast<float>(m_state->longCanvas.docHeight);
    } else if (m_renderer->getRenderTarget()) {
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
        const float minSize = m_state->isLongImageMode()
            ? static_cast<float>(ScrollCaptureEditorOverlay::kMinCropDimensionPx)
            : 8.0f;
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



    m_state->toolbarLayoutValid = false;

    if (m_state->markup.hasAnyMarkup()) {
        // 已有标注（含撤销栈）：按新选区重裁底图，并由 prepareMarkupBase 统筹平移已有图元
        rebuildMarkupBase();
        m_renderer->markMarkupDirty();
    } else {
        // 无任何标注：底图在下次标注或选区确定时按新选区再裁（避免拖拽选区期间每帧重裁开销）
        m_state->markupBaseReady = false;
        m_state->markupBaseRect = {0, 0, 0, 0};
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
    if (m_state->isLongImageMode()) {
        maxW = static_cast<float>(m_state->longCanvas.docWidth);
        maxH = static_cast<float>(m_state->longCanvas.docHeight);
    } else if (m_renderer->getRenderTarget()) {
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
    rebuildCaptureToolbar(*m_state, m_state->toolbarAnchorRect(selectionRect),
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
        button.command != ToolbarCommand::ChooseCustomColor &&
        button.command != ToolbarCommand::ToggleFill &&
        button.command != ToolbarCommand::ToggleTextOutline &&
        button.command != ToolbarCommand::SelectTextOutlineColor &&
        button.command != ToolbarCommand::CycleStrokeWidth &&
        button.command != ToolbarCommand::CycleElementCornerRadius &&
        button.command != ToolbarCommand::ToggleLineStyleDropdown &&
        button.command != ToolbarCommand::SelectLineStyle &&
        button.command != ToolbarCommand::ToggleArrowStyleDropdown &&
        button.command != ToolbarCommand::SelectArrowStyle &&
        button.command != ToolbarCommand::ToggleCurveMode &&
        button.command != ToolbarCommand::ToggleLineArrow &&
        button.command != ToolbarCommand::ToggleStartCapDropdown &&
        button.command != ToolbarCommand::SelectStartCap &&
        button.command != ToolbarCommand::ToggleEndCapDropdown &&
        button.command != ToolbarCommand::SelectEndCap &&
        button.command != ToolbarCommand::SelectMosaicType &&
        button.command != ToolbarCommand::ToggleNumberShape &&
        button.command != ToolbarCommand::ToggleNumberFormatDropdown &&
        button.command != ToolbarCommand::ToggleSpotlightShape &&
        button.command != ToolbarCommand::CycleSpotlightDim &&
        button.command != ToolbarCommand::CycleMagnifierScale &&
        button.command != ToolbarCommand::ToggleMagnifierCallout &&
        button.command != ToolbarCommand::ToggleMagnifierCalloutStyleDropdown &&
        button.command != ToolbarCommand::ResetNumberCounter &&
        button.command != ToolbarCommand::ToggleBeautyShell &&
        button.command != ToolbarCommand::CycleBeautyBg &&
        button.command != ToolbarCommand::CycleBeautyPadding &&
        button.command != ToolbarCommand::CycleBeautyRadius &&
        button.command != ToolbarCommand::SideCycleAspectRatio &&
        button.command != ToolbarCommand::SideInvertSelection &&
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
            resetCoalescedUndo(*m_state);
            m_renderer->markMarkupDirty();
        }
    }

    switch (button.command) {
        case ToolbarCommand::SelectTool:
            if (!button.isSecondary) {
                toggleOrSetTool(button.tool);
            } else {
                setCurrentTool(button.tool);
            }
            return;

        case ToolbarCommand::SelectColor: {
            const bool altHeld = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool editBorder = (m_state->isEditingOutlineColor || altHeld) &&
                                    button.tool != MarkupTool::Magnifier &&
                                    m_state->currentTool != MarkupTool::Magnifier;
            if (editBorder) {
                m_state->currentTextOutlineColor = button.color;
                if (m_state->currentOutlineWidth <= 0) {
                    m_state->currentOutlineWidth = 2;
                }
                if (m_state->activeElement) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->textOutlineColor = button.color;
                    m_state->activeElement->hasCustomOutline = true;
                    m_state->activeElement->textOutline = true;
                    if (m_state->activeElement->outlineThickness <= 0.05f) {
                        m_state->activeElement->outlineThickness = static_cast<float>(m_state->currentOutlineWidth);
                    }
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                }
            } else {
                if (button.tool == MarkupTool::Magnifier || m_state->currentTool == MarkupTool::Magnifier) {
                    m_state->currentMagnifierColor = button.color;
                } else {
                    m_state->currentColor = button.color;
                }
                if (m_state->activeElement && m_state->activeElement->color != button.color) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->color = button.color;
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                }
            }
            m_state->toolbarLayoutValid = false;
            break;
        }

        case ToolbarCommand::ChooseCustomColor: {
            static COLORREF customCustColors[16] = {
                RGB(239, 68, 68),   RGB(249, 115, 22),  RGB(234, 179, 8),   RGB(34, 197, 94),
                RGB(59, 130, 246),  RGB(139, 92, 246),  RGB(236, 72, 153),  RGB(20, 184, 166),
                RGB(15, 23, 42),    RGB(51, 65, 85),    RGB(100, 116, 139), RGB(255, 255, 255),
                RGB(124, 58, 237),  RGB(225, 29, 72),   RGB(2, 132, 199),   RGB(16, 185, 129)
            };
            struct ColorDialogAnchor {
                POINT btnCenterScreen{};
                int toolbarTopScreen = 0;
                int toolbarBottomScreen = 0;

                static UINT_PTR CALLBACK hook(HWND hdlg, UINT uiMsg, WPARAM /*wParam*/, LPARAM lParam) {
                    if (uiMsg == WM_INITDIALOG && lParam != 0) {
                        const auto* pccData = reinterpret_cast<const CHOOSECOLORW*>(lParam);
                        const auto* pAnchor = reinterpret_cast<const ColorDialogAnchor*>(pccData->lCustData);
                        if (pAnchor) {
                            RECT dlgRc{};
                            if (GetWindowRect(hdlg, &dlgRc)) {
                                const int dlgW = dlgRc.right - dlgRc.left;
                                const int dlgH = dlgRc.bottom - dlgRc.top;
                                HMONITOR hMon = MonitorFromPoint(pAnchor->btnCenterScreen, MONITOR_DEFAULTTONEAREST);
                                MONITORINFO mi{};
                                mi.cbSize = sizeof(mi);
                                RECT workArea{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
                                if (hMon && GetMonitorInfoW(hMon, &mi)) {
                                    workArea = mi.rcWork;
                                }
                                const int margin = 10;
                                int targetX = pAnchor->btnCenterScreen.x - dlgW / 2;
                                targetX = std::clamp(targetX, static_cast<int>(workArea.left) + margin,
                                                     std::max(static_cast<int>(workArea.left) + margin,
                                                              static_cast<int>(workArea.right) - dlgW - margin));

                                // 优先紧贴工具栏下方弹出；若下方空间不足，则紧贴主工具栏上方就近弹出
                                int targetY = pAnchor->toolbarBottomScreen + margin;
                                if (targetY + dlgH > workArea.bottom - margin) {
                                    targetY = pAnchor->toolbarTopScreen - dlgH - margin;
                                }
                                targetY = std::clamp(targetY, static_cast<int>(workArea.top) + margin,
                                                     std::max(static_cast<int>(workArea.top) + margin,
                                                              static_cast<int>(workArea.bottom) - dlgH - margin));

                                SetWindowPos(hdlg, HWND_TOPMOST, targetX, targetY, 0, 0,
                                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                            }
                        }
                    }
                    return 0;
                }
            };

            int virX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int virY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            RECT winRc{};
            if (m_hwnd && IsWindow(m_hwnd) && GetWindowRect(m_hwnd, &winRc)) {
                virX = winRc.left;
                virY = winRc.top;
            }
            ColorDialogAnchor anchor{};
            if (button.rect.right > button.rect.left) {
                anchor.btnCenterScreen.x = virX + static_cast<int>((button.rect.left + button.rect.right) * 0.5f);
                anchor.btnCenterScreen.y = virY + static_cast<int>((button.rect.top + button.rect.bottom) * 0.5f);
            } else {
                GetCursorPos(&anchor.btnCenterScreen);
            }
            const float topLocal = (m_state->primaryToolbarRect.bottom > m_state->primaryToolbarRect.top)
                ? std::min(m_state->primaryToolbarRect.top,
                           (m_state->secondaryToolbarRect.bottom > m_state->secondaryToolbarRect.top)
                               ? m_state->secondaryToolbarRect.top
                               : m_state->primaryToolbarRect.top)
                : static_cast<float>(anchor.btnCenterScreen.y - virY - 24);
            const float bottomLocal = (m_state->secondaryToolbarRect.bottom > m_state->secondaryToolbarRect.top)
                ? std::max(m_state->primaryToolbarRect.bottom, m_state->secondaryToolbarRect.bottom)
                : ((m_state->primaryToolbarRect.bottom > m_state->primaryToolbarRect.top)
                       ? m_state->primaryToolbarRect.bottom
                       : static_cast<float>(anchor.btnCenterScreen.y - virY + 24));
            anchor.toolbarTopScreen = virY + static_cast<int>(topLocal);
            anchor.toolbarBottomScreen = virY + static_cast<int>(bottomLocal);

            const bool forMagnifier = (button.tool == MarkupTool::Magnifier || m_state->currentTool == MarkupTool::Magnifier);
            const bool altHeld = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool editBorder = (m_state->isEditingOutlineColor || altHeld) && !forMagnifier;
            MarkupColor initCol = forMagnifier ? m_state->currentMagnifierColor : m_state->currentColor;
            if (editBorder) {
                if (m_state->activeElement && !m_state->activeElement->textOutlineColor.isAuto()) {
                    initCol = m_state->activeElement->textOutlineColor;
                } else if (!m_state->currentTextOutlineColor.isAuto()) {
                    initCol = m_state->currentTextOutlineColor;
                } else {
                    initCol = MarkupColor::White();
                }
            } else if (m_state->activeElement != nullptr) {
                initCol = m_state->activeElement->color;
            }
            CHOOSECOLORW cc{};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = m_hwnd;
            cc.lpCustColors = customCustColors;
            cc.rgbResult = RGB(initCol.r, initCol.g, initCol.b);
            cc.lCustData = reinterpret_cast<LPARAM>(&anchor);
            cc.lpfnHook = &ColorDialogAnchor::hook;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR | CC_ENABLEHOOK;

            ReleaseCapture();
            const BOOL chosen = ChooseColorW(&cc);
            if (m_hwnd && IsWindow(m_hwnd)) {
                SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                SetForegroundWindow(m_hwnd);
                SetFocus(m_hwnd);
            }
            if (chosen) {
                const uint8_t r = GetRValue(cc.rgbResult);
                const uint8_t g = GetGValue(cc.rgbResult);
                const uint8_t b = GetBValue(cc.rgbResult);
                const uint8_t keepAlpha = (initCol.a > 0) ? initCol.a : 255;
                m_state->customColor = MarkupColor(r, g, b, keepAlpha);
                m_state->hasCustomColor = true;
                if (editBorder) {
                    m_state->currentTextOutlineColor = m_state->customColor;
                    if (m_state->currentOutlineWidth <= 0) {
                        m_state->currentOutlineWidth = 2;
                    }
                    if (m_state->activeElement) {
                        m_state->markup.pushUndoSnapshot();
                        resetCoalescedUndo(*m_state);
                        m_state->activeElement->textOutlineColor = m_state->customColor;
                        m_state->activeElement->hasCustomOutline = true;
                        m_state->activeElement->textOutline = true;
                        if (m_state->activeElement->outlineThickness <= 0.05f) {
                            m_state->activeElement->outlineThickness = static_cast<float>(m_state->currentOutlineWidth);
                        }
                        m_renderer->markMarkupDirty();
                    }
                } else {
                    if (forMagnifier) {
                        m_state->currentMagnifierColor = m_state->customColor;
                    } else {
                        m_state->currentColor = m_state->customColor;
                    }
                    if (m_state->activeElement && m_state->activeElement->color != m_state->customColor) {
                        m_state->markup.pushUndoSnapshot();
                        resetCoalescedUndo(*m_state);
                        m_state->activeElement->color = m_state->customColor;
                        m_renderer->markMarkupDirty();
                    }
                }
                char hexBuf[16]{};
                snprintf(hexBuf, sizeof(hexBuf), "#%02x%02x%02x", r, g, b);
                tools3000::core::ConfigManager::instance().set<std::string>(
                    "/capture/customColor", hexBuf);
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleFill:
            if (button.tool == MarkupTool::Magnifier || m_state->currentTool == MarkupTool::Magnifier) {
                m_state->magnifierShowBadge = !m_state->magnifierShowBadge;
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->fill = m_state->magnifierShowBadge;
                    m_renderer->markMarkupDirty();
                }
            } else if (button.tool == MarkupTool::Spotlight || m_state->currentTool == MarkupTool::Spotlight) {
                m_state->currentSpotlightBorder = !m_state->currentSpotlightBorder;
                tools3000::core::ConfigManager::instance().set<bool>(
                    "/capture/markup/spotlightBorder", m_state->currentSpotlightBorder);
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Spotlight) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->spotlightShowBorder = m_state->currentSpotlightBorder;
                    m_renderer->markMarkupDirty();
                }
            } else {
                m_state->currentFillMode = !m_state->currentFillMode;
                if (!m_state->currentFillMode) {
                    m_state->isEditingOutlineColor = false;
                    if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                        m_state->sliderPopup.type = SliderPopupType::None;
                    }
                }
                if (m_state->activeElement) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->fill = m_state->currentFillMode;
                    m_renderer->markMarkupDirty();
                }
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleTextOutline: {
            const bool isShapeOrArrowBorder = (button.tool == MarkupTool::Arrow) ||
                ((button.tool == MarkupTool::Rectangle || button.tool == MarkupTool::Ellipse) && m_state->currentFillMode);
            const bool isTextTool = !isShapeOrArrowBorder;
            const bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool zh = tools3000::core::isUiLanguageChinese();
            if (isTextTool && !shiftHeld) {
                m_state->currentTextOutline = !m_state->currentTextOutline;
                if (!m_state->currentTextOutline) {
                    m_state->isEditingOutlineColor = false;
                } else {
                    m_state->currentTextOutlineColor = MarkupColor::Auto();
                }
                tools3000::core::ConfigManager::instance().set<bool>(
                    "/capture/markup/text/outline", m_state->currentTextOutline);
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->textOutline = m_state->currentTextOutline;
                    if (m_state->currentTextOutline) {
                        m_state->activeElement->textOutlineColor = MarkupColor::Auto();
                    }
                    m_renderer->markMarkupDirty();
                }
                m_state->loupeToastUntil = GetTickCount() + 1500;
                m_state->loupeToastMessage = m_state->currentTextOutline
                    ? (zh ? L"文字描边: 已开启" : L"Text Outline: ON")
                    : (zh ? L"文字描边: 已关闭" : L"Text Outline: OFF");
            } else {
                m_state->isEditingOutlineColor = !m_state->isEditingOutlineColor;
                if (m_state->isEditingOutlineColor) {
                    if (isTextTool) {
                        m_state->currentTextOutline = true;
                        if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text &&
                            !m_state->activeElement->textOutline) {
                            m_state->markup.pushUndoSnapshot();
                            resetCoalescedUndo(*m_state);
                            m_state->activeElement->textOutline = true;
                            m_renderer->markMarkupDirty();
                        }
                    }
                    closeAllCapturePopups(*m_state, false);
                    m_state->sliderPopup.type = SliderPopupType::OutlineWidth;
                } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                    m_state->sliderPopup.type = SliderPopupType::None;
                }
                m_state->loupeToastUntil = GetTickCount() + 1800;
                m_state->loupeToastMessage = m_state->isEditingOutlineColor
                    ? (zh ? L"边框模式已开启：右侧色块改边框色，拖动滑块或滚轮调边框粗细"
                          : L"Border Mode ON: Pick border color on right, scroll/drag for border width")
                    : (zh ? L"已切回主填充色模式" : L"Switched back to Fill Color mode");
            }
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
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text &&
                m_state->activeElement->textOutlineColor != button.color) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
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
                closeAllCapturePopups(*m_state, false);
                m_state->sliderPopup.type = (m_state->currentTool == MarkupTool::Mosaic || m_state->currentTool == MarkupTool::Blur)
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
                closeAllCapturePopups(*m_state, false);
                m_state->sliderPopup.type = SliderPopupType::SelectionCornerRadius;
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SideResetSelection: {
            m_state->cornerRadius = 0.0f;
            if (m_state->beautyShell.enabled) {
                m_state->beautyShell.cornerRadius = 0.0f;
            }
            m_state->aspectRatio = AspectRatioPreset::Free;
            if (m_state->isLongImageMode()) {
                m_state->dragStart = {0, 0};
                m_state->dragEnd = {m_state->longCanvas.docWidth, m_state->longCanvas.docHeight};
                const bool zh = tools3000::core::isUiLanguageChinese();
                m_state->loupeToastMessage = zh ? L"已恢复完整长图与直角输出" : L"Restored full long image";
                m_state->loupeToastUntil = GetTickCount() + 1400;
            }
            closeAllCapturePopups(*m_state, false);
            m_state->toolbarLayoutValid = false;
            prepareMarkupBase();
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SideInvertSelection: {
            // 全屏/全幅长图扩展与原选区双向切换
            const int virX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int virY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int curLeft = std::min(m_state->dragStart.x, m_state->dragEnd.x);
            const int curTop = std::min(m_state->dragStart.y, m_state->dragEnd.y);
            const int curRight = std::max(m_state->dragStart.x, m_state->dragEnd.x);
            const int curBottom = std::max(m_state->dragStart.y, m_state->dragEnd.y);

            RECT fullLocal{0, 0,
                           m_state->frozenScreen.empty() ? GetSystemMetrics(SM_CXVIRTUALSCREEN) : m_state->frozenScreen.cols,
                           m_state->frozenScreen.empty() ? GetSystemMetrics(SM_CYVIRTUALSCREEN) : m_state->frozenScreen.rows};
            if (m_state->isLongImageMode()) {
                fullLocal = {0, 0, m_state->longCanvas.docWidth, m_state->longCanvas.docHeight};
            } else {
                POINT centerScreen{virX + (curLeft + curRight) / 2, virY + (curTop + curBottom) / 2};
                HMONITOR hMon = MonitorFromPoint(centerScreen, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (hMon && GetMonitorInfoW(hMon, &mi)) {
                    fullLocal.left = mi.rcMonitor.left - virX;
                    fullLocal.top = mi.rcMonitor.top - virY;
                    fullLocal.right = mi.rcMonitor.right - virX;
                    fullLocal.bottom = mi.rcMonitor.bottom - virY;
                }
            }

            const bool isAlreadyFullMonitor =
                std::abs(curLeft - fullLocal.left) <= 2 &&
                std::abs(curTop - fullLocal.top) <= 2 &&
                std::abs(curRight - fullLocal.right) <= 2 &&
                std::abs(curBottom - fullLocal.bottom) <= 2;

            const bool zh = tools3000::core::isUiLanguageChinese();
            if (isAlreadyFullMonitor && m_state->hasSavedSelectionBeforeInvert &&
                m_state->savedSelectionBeforeInvert.right > m_state->savedSelectionBeforeInvert.left &&
                m_state->savedSelectionBeforeInvert.bottom > m_state->savedSelectionBeforeInvert.top) {
                m_state->dragStart = {m_state->savedSelectionBeforeInvert.left, m_state->savedSelectionBeforeInvert.top};
                m_state->dragEnd = {m_state->savedSelectionBeforeInvert.right, m_state->savedSelectionBeforeInvert.bottom};
                m_state->hasSavedSelectionBeforeInvert = false;
                m_state->loupeToastMessage = zh ? L"已还原上次选区" : L"Restored previous selection";
            } else {
                m_state->savedSelectionBeforeInvert = {curLeft, curTop, curRight, curBottom};
                m_state->hasSavedSelectionBeforeInvert = true;
                m_state->dragStart = {fullLocal.left, fullLocal.top};
                m_state->dragEnd = {fullLocal.right, fullLocal.bottom};
                m_state->loupeToastMessage = zh ? L"已选中当前屏幕" : L"Selected current screen";
            }
            m_state->loupeToastUntil = GetTickCount() + 1400;
            m_state->toolbarLayoutValid = false;
            prepareMarkupBase();
            m_renderer->markMarkupDirty();
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::CycleElementCornerRadius: {
            if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                m_state->sliderPopup.type = SliderPopupType::None;
            } else {
                closeAllCapturePopups(*m_state, false);
                m_state->sliderPopup.type = SliderPopupType::CornerRadius;
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleLineStyleDropdown: {
            const bool reopeningSame = (m_state->openSubmenu == SubmenuType::LineStyle);
            closeAllCapturePopups(*m_state, false);
            m_state->openSubmenu = reopeningSame ? SubmenuType::None : SubmenuType::LineStyle;
            break;
        }

        case ToolbarCommand::SelectLineStyle:
            m_state->currentLineStyle = button.lineStyleParam;
            m_state->openSubmenu = SubmenuType::None;
            if (m_state->activeElement && m_state->activeElement->lineStyle != m_state->currentLineStyle) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->lineStyle = m_state->currentLineStyle;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            break;

        case ToolbarCommand::ToggleArrowStyleDropdown: {
            const bool reopeningSame = (m_state->openSubmenu == SubmenuType::ArrowStyle);
            closeAllCapturePopups(*m_state, false);
            m_state->openSubmenu = reopeningSame ? SubmenuType::None : SubmenuType::ArrowStyle;
            break;
        }

        case ToolbarCommand::SelectArrowStyle:
            m_state->currentArrowStyle = button.arrowStyleParam;
            switch (m_state->currentArrowStyle) {
                case ArrowStyle::Thin:
                    m_state->currentStartCap = LineCapStyle::None;
                    m_state->currentEndCap = LineCapStyle::OpenArrow;
                    break;
                case ArrowStyle::Tech:
                case ArrowStyle::CampaignMarch:
                case ArrowStyle::MarchDoubleTrack:
                    m_state->currentStartCap = LineCapStyle::None;
                    m_state->currentEndCap = LineCapStyle::StealthArrow;
                    break;
                case ArrowStyle::DoubleEnded:
                    m_state->currentStartCap = LineCapStyle::SolidTriangle;
                    m_state->currentEndCap = LineCapStyle::SolidTriangle;
                    break;
                case ArrowStyle::Standard:
                default:
                    m_state->currentStartCap = LineCapStyle::None;
                    m_state->currentEndCap = LineCapStyle::SolidTriangle;
                    break;
            }
            m_state->openSubmenu = SubmenuType::None;
            if (m_state->activeElement) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->arrowStyle = m_state->currentArrowStyle;
                if (m_state->activeElement->hasCustomCaps) {
                    m_state->activeElement->startCap = m_state->currentStartCap;
                    m_state->activeElement->endCap = m_state->currentEndCap;
                }
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleCurveMode:
            m_state->currentSmoothCurve = !m_state->currentSmoothCurve;
            if (m_state->activeElement &&
                (m_state->activeElement->tool == MarkupTool::Line ||
                 m_state->activeElement->tool == MarkupTool::Arrow ||
                 m_state->activeElement->tool == MarkupTool::Pen)) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->smoothCurve = m_state->currentSmoothCurve;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleLineArrow:
            m_state->currentLineArrowEnabled = !m_state->currentLineArrowEnabled;
            if (m_state->currentLineArrowEnabled &&
                m_state->currentStartCap == LineCapStyle::None &&
                m_state->currentEndCap == LineCapStyle::None) {
                m_state->currentEndCap = LineCapStyle::OpenArrow;
            }
            if (!m_state->currentLineArrowEnabled &&
                (m_state->openSubmenu == SubmenuType::StartCapStyle ||
                 m_state->openSubmenu == SubmenuType::EndCapStyle)) {
                m_state->openSubmenu = SubmenuType::None;
            }
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Line) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->hasCustomCaps = m_state->currentLineArrowEnabled;
                m_state->activeElement->startCap = m_state->currentStartCap;
                m_state->activeElement->endCap = m_state->currentEndCap;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleStartCapDropdown: {
            const bool reopeningSame = (m_state->openSubmenu == SubmenuType::StartCapStyle);
            closeAllCapturePopups(*m_state, false);
            m_state->openSubmenu = reopeningSame ? SubmenuType::None : SubmenuType::StartCapStyle;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SelectStartCap:
        case ToolbarCommand::SelectEndCap: {
            const LineCapStyle chosenCap = static_cast<LineCapStyle>(std::clamp(button.intParam, 0, 9));
            if (button.command == ToolbarCommand::SelectStartCap) {
                m_state->currentStartCap = chosenCap;
            } else {
                m_state->currentEndCap = chosenCap;
            }
            m_state->currentLineArrowEnabled = true;
            m_state->openSubmenu = SubmenuType::None;
            if (m_state->currentStartCap == LineCapStyle::None && m_state->currentEndCap == LineCapStyle::OpenArrow) {
                m_state->currentArrowStyle = ArrowStyle::Thin;
            } else if (m_state->currentStartCap == LineCapStyle::None && m_state->currentEndCap == LineCapStyle::StealthArrow) {
                m_state->currentArrowStyle = ArrowStyle::Tech;
            } else if (m_state->currentStartCap != LineCapStyle::None && m_state->currentEndCap != LineCapStyle::None) {
                m_state->currentArrowStyle = ArrowStyle::DoubleEnded;
            } else {
                m_state->currentArrowStyle = ArrowStyle::Standard;
            }
            if (m_state->activeElement &&
                (m_state->activeElement->tool == MarkupTool::Line ||
                 m_state->activeElement->tool == MarkupTool::Arrow)) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->hasCustomCaps = true;
                m_state->activeElement->startCap = m_state->currentStartCap;
                m_state->activeElement->endCap = m_state->currentEndCap;
                m_state->activeElement->arrowStyle = m_state->currentArrowStyle;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleEndCapDropdown: {
            const bool reopeningSame = (m_state->openSubmenu == SubmenuType::EndCapStyle);
            closeAllCapturePopups(*m_state, false);
            m_state->openSubmenu = reopeningSame ? SubmenuType::None : SubmenuType::EndCapStyle;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::SelectMosaicType:
            m_state->currentMosaicType = button.intParam;
            m_state->openSubmenu = SubmenuType::None;
            if (m_state->currentTool == MarkupTool::Mosaic || m_state->currentTool == MarkupTool::Blur) {
                m_state->currentTool = (button.intParam == 1) ? MarkupTool::Blur : MarkupTool::Mosaic;
            }
            if (m_state->activeElement &&
                (m_state->activeElement->tool == MarkupTool::Mosaic || m_state->activeElement->tool == MarkupTool::Blur)) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->tool = (button.intParam == 1) ? MarkupTool::Blur : MarkupTool::Mosaic;
                m_state->activeElement->mosaicBlockSize = (button.intParam == 1)
                    ? (std::max)(5, m_state->currentStrokeWidth * 3)
                    : (std::max)(4, m_state->currentStrokeWidth * 2);
                m_state->currentTool = m_state->activeElement->tool;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleBeautyShell: {
            m_state->beautyShell.enabled = !m_state->beautyShell.enabled;
            tools3000::core::ConfigManager::instance().set<bool>(
                "/capture/beautyShellEnabled", m_state->beautyShell.enabled);
            if (!m_state->beautyShell.enabled &&
                (m_state->dropdownMenu.type == DropdownType::BeautyShellBg ||
                 m_state->dropdownMenu.type == DropdownType::BeautyShellPadding ||
                 m_state->dropdownMenu.type == DropdownType::BeautyShellRadius)) {
                closeDropdownMenu(*m_state, false);
            }
            const bool zh = tools3000::core::isUiLanguageChinese();
            m_state->loupeToastUntil = GetTickCount() + 1500;
            m_state->loupeToastMessage = m_state->beautyShell.enabled
                ? (zh ? L"已开启美化外壳" : L"Beauty Shell enabled")
                : (zh ? L"已关闭美化外壳" : L"Beauty Shell disabled");
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
            const bool zh = tools3000::core::isUiLanguageChinese();
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
                const bool zh = tools3000::core::isUiLanguageChinese();
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
                const bool zh = tools3000::core::isUiLanguageChinese();
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
            if (button.tool == MarkupTool::Magnifier || m_state->currentTool == MarkupTool::Magnifier) {
                m_state->currentMagnifierShape = (m_state->currentMagnifierShape == NumberBadgeShape::Circle)
                    ? NumberBadgeShape::RoundedSquare
                    : NumberBadgeShape::Circle;
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->numberShape = m_state->currentMagnifierShape;
                    m_renderer->markMarkupDirty();
                }
            } else {
                if (m_state->currentNumberShape == NumberBadgeShape::Circle) {
                    m_state->currentNumberShape = NumberBadgeShape::RoundedSquare;
                } else {
                    m_state->currentNumberShape = NumberBadgeShape::Circle;
                }
                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number) {
                    m_state->markup.pushUndoSnapshot();
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement->numberShape = m_state->currentNumberShape;
                    m_renderer->markMarkupDirty();
                }
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleNumberFormatDropdown: {
            m_state->sliderPopup.type = SliderPopupType::None;
            openDropdownMenu(DropdownType::NumberFormat, button.rect);
            break;
        }

        case ToolbarCommand::ToggleMagnifierCalloutStyleDropdown: {
            m_state->sliderPopup.type = SliderPopupType::None;
            openDropdownMenu(DropdownType::MagnifierCalloutStyle, button.rect);
            break;
        }

        case ToolbarCommand::ToggleSpotlightShape: {
            m_state->currentSpotlightEllipse = !m_state->currentSpotlightEllipse;
            tools3000::core::ConfigManager::instance().set<bool>(
                "/capture/markup/spotlightEllipse", m_state->currentSpotlightEllipse);
            if (m_state->currentSpotlightEllipse && m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                m_state->sliderPopup.type = SliderPopupType::None;
            }
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Spotlight) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->fill = m_state->currentSpotlightEllipse;
                m_state->activeElement->spotlightEllipse = m_state->currentSpotlightEllipse;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::CycleSpotlightDim: {
            if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim) {
                m_state->sliderPopup.type = SliderPopupType::None;
            } else {
                closeAllCapturePopups(*m_state, false);
                m_state->sliderPopup.type = SliderPopupType::SpotlightDim;
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::CycleMagnifierScale: {
            const float curScale = (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier)
                ? m_state->activeElement->magnifierScale
                : m_state->dynamicMagnifierScale;
            float nextScale = 2.0f;
            if (curScale < 1.75f) nextScale = 2.0f;
            else if (curScale < 2.5f) nextScale = 3.0f;
            else if (curScale < 3.5f) nextScale = 4.0f;
            else nextScale = 1.5f;

            m_state->dynamicMagnifierScale = nextScale;
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                m_state->activeElement->magnifierScale = nextScale;
                m_renderer->markMarkupDirty();
            }
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ToggleMagnifierCallout: {
            const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
            const bool zh = tools3000::core::isUiLanguageChinese();
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                m_state->markup.pushUndoSnapshot();
                resetCoalescedUndo(*m_state);
                auto* elem = m_state->activeElement;
                const bool unsetEnd = !elem->hasLeaderArrow && elem->endPt.x == 0 && elem->endPt.y == 0 &&
                                      (elem->startPt.x != 0 || elem->startPt.y != 0);
                const cv::Point lc = unsetEnd ? elem->startPt : elem->endPt;
                const bool currentlyOffset = std::hypot(static_cast<double>(lc.x - elem->startPt.x),
                                                        static_cast<double>(lc.y - elem->startPt.y)) > 8.0;
                if (currentlyOffset) {
                    elem->endPt = elem->startPt;
                    elem->hasLeaderArrow = false;
                    m_state->magnifierCalloutMode = false;
                } else {
                    const auto sel = currentSelectionRect();
                    const int selW = std::max(1, static_cast<int>(std::round(sel.right - sel.left)));
                    const int selH = std::max(1, static_cast<int>(std::round(sel.bottom - sel.top)));
                    elem->endPt = computeSmartMagnifierCalloutCenter(
                        elem->startPt, elem->magnifierRadius, elem->magnifierScale, selW, selH, curDpi);
                    elem->hasLeaderArrow = std::hypot(static_cast<double>(elem->endPt.x - elem->startPt.x),
                                                      static_cast<double>(elem->endPt.y - elem->startPt.y)) > 8.0;
                    m_state->magnifierCalloutMode = true;
                }
                m_renderer->markMarkupDirty();
            } else {
                m_state->magnifierCalloutMode = !m_state->magnifierCalloutMode;
            }
            tools3000::core::ConfigManager::instance().set<bool>(
                "/capture/markup/magnifier/calloutMode", m_state->magnifierCalloutMode);
            m_state->loupeToastUntil = GetTickCount() + 1500;
            m_state->loupeToastMessage = m_state->magnifierCalloutMode
                ? (zh ? L"已切换为引出放大镜" : L"Callout magnifier")
                : (zh ? L"已切换为原位放大镜" : L"In-place magnifier");
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::ResetNumberCounter: {
            const NumberFormat targetFmt = (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number)
                ? m_state->activeElement->numberFormat
                : m_state->currentNumberFormat;
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number) {
                m_state->activeElement->isActive = false;
                m_state->activeElement = nullptr;
                m_renderer->markMarkupDirty();
            }
            m_state->markup.resetNumber(1, targetFmt);
            const bool zh = tools3000::core::isUiLanguageChinese();
            const std::wstring firstLabel = formatNumberBadgeLabel(1, targetFmt);
            m_state->loupeToastUntil = GetTickCount() + 1400;
            m_state->loupeToastMessage = zh
                ? (L"当前序列起始已重置为 " + firstLabel)
                : (L"Sequence reset to " + firstLabel);
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;
        }

        case ToolbarCommand::Undo:
            m_state->activeElement = nullptr;
            m_state->isManipulating = false;
            m_state->hasManipulateSnapshot = false;
            m_state->dragHandle = HitArea::None;
            resetCoalescedUndo(*m_state);
            m_state->markup.undo();
            for (const auto& el : m_state->markup.elements()) {
                if (el) {
                    el->isActive = false;
                    el->isEditing = false;
                }
            }
            m_renderer->markMarkupDirty();
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::Redo:
            m_state->activeElement = nullptr;
            m_state->isManipulating = false;
            m_state->hasManipulateSnapshot = false;
            m_state->dragHandle = HitArea::None;
            resetCoalescedUndo(*m_state);
            m_state->markup.redo();
            for (const auto& el : m_state->markup.elements()) {
                if (el) {
                    el->isActive = false;
                    el->isEditing = false;
                }
            }
            m_renderer->markMarkupDirty();
            m_state->toolbarLayoutValid = false;
            m_renderer->invalidate();
            break;

        case ToolbarCommand::Clear:
            m_state->activeElement = nullptr;
            m_state->isManipulating = false;
            m_state->hasManipulateSnapshot = false;
            m_state->dragHandle = HitArea::None;
            resetCoalescedUndo(*m_state);
            m_state->markup.clearElementsUndoable();
            m_renderer->markMarkupDirty();
            m_state->toolbarLayoutValid = false;
            prepareMarkupBase();
            m_renderer->invalidate();
            break;

        case ToolbarCommand::ToggleCornerRadius: {
            if (m_state->beautyShell.enabled) {
                static const std::array<float, 7> radiuses = {0.0f, 8.0f, 10.0f, 12.0f, 16.0f, 24.0f, 32.0f};
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
                if (w <= 0 || h <= 0) break;
                prepareMarkupBase();

                cv::Mat cropped;
                if (m_state->markup.elementCount() > 0) cropped = m_state->markup.getCompositeImage();
                else cropped = cropMarkupBase(m_state->frozenScreen, cv::Rect(x1, y1, w, h));

                int offsetX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                int offsetY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                RECT winRc{};
                if (m_hwnd && IsWindow(m_hwnd) && GetWindowRect(m_hwnd, &winRc)) {
                    offsetX = winRc.left;
                    offsetY = winRc.top;
                }
                CaptureRegion region{x1 + offsetX, y1 + offsetY, w, h};
                if (m_state->isLongImageMode()) {
                    const D2D1_RECT_F anchor = m_state->toolbarAnchorRect();
                    region = CaptureRegion{
                        offsetX + static_cast<int>(std::lround(anchor.left)),
                        offsetY + static_cast<int>(std::lround(anchor.top)),
                        std::max(120, static_cast<int>(std::lround(anchor.right - anchor.left))),
                        std::max(120, static_cast<int>(std::lround(anchor.bottom - anchor.top)))
                    };
                }
                auto ocrCb = m_state->ocrCallback;
                if(m_cancelCb) m_cancelCb(); 
                ocrCb(region, cropped);
            }
            break;

        case ToolbarCommand::PinWindow: {
            if (m_state->isMarking) {
                finishMarkup(m_state->currentCursor);
            }
            if (m_state->isLongImageMode() || CaptureOverlay::instance().isEditingPinned()) {
                if (m_confirmCb) {
                    m_confirmCb({CaptureCompletionAction::Pin});
                }
                break;
            }
            int x1 = std::min(m_state->dragStart.x, m_state->dragEnd.x);
            int y1 = std::min(m_state->dragStart.y, m_state->dragEnd.y);
            int x2 = std::max(m_state->dragStart.x, m_state->dragEnd.x);
            int y2 = std::max(m_state->dragStart.y, m_state->dragEnd.y);
            int w = x2 - x1;
            int h = y2 - y1;
            if (w <= 0 || h <= 0) break;
            prepareMarkupBase();

            cv::Mat cleanBase = cropMarkupBase(m_state->frozenScreen, cv::Rect(x1, y1, w, h));
            std::vector<MarkupElement> initialElements = m_state->markup.exportElements();
            cv::Mat cropped;
            if (m_state->markup.elementCount() > 0) cropped = m_state->markup.getCompositeImage();
            else cropped = cleanBase;
            int origX = x1 + GetSystemMetrics(SM_XVIRTUALSCREEN);
            int origY = y1 + GetSystemMetrics(SM_YVIRTUALSCREEN);
            CaptureRegion reg{origX, origY, w, h, m_state->cornerRadius};

            const bool bakedDecorations =
                (m_state->beautyShell.enabled || m_state->cornerRadius > 0.5f) && !cropped.empty();
            if (m_state->beautyShell.enabled && !cropped.empty()) {
                cropped = applyBeautyShell(cropped, m_state->beautyShell);
            } else if (m_state->cornerRadius > 0.5f && !cropped.empty()) {
                cropped = applyRoundedCorners(cropped, m_state->cornerRadius);
            }
            if (bakedDecorations) {
                cleanBase = cropped;
                initialElements.clear();
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

            // 精确原位贴图（携带 1:1 无损底图与独立矢量图元，支持后续任意缩放态无损增删改）
            PinWindow::create(cropped, spawnPos.x, spawnPos.y, 0.0f, {}, cleanBase, std::move(initialElements));
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

            // 先关闭常规覆盖层与提示浮窗，长截图统一使用带硬件防截隔离的专属交互式控制台
            ShortcutHintOverlay::instance().hide();
            if (m_cancelCb) m_cancelCb();

            ScrollCaptureOptions opts;
            opts.captureRect = capRect;
            opts.mode = ScrollMode::Auto;
            opts.interactiveHoldAtBottom = true;
            opts.isUiHotZone = [](POINT pt) {
                return ScrollCaptureOverlay::instance().isPointInControlZone(pt);
            };
            ScrollCaptureOverlay::instance().show(capRect);
            ScrollCapture::instance().start(opts);
            if (!ScrollCapture::instance().isRunning()) {
                ScrollCaptureOverlay::instance().hide();
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

        case ToolbarCommand::Copy:
            if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
            break;

        case ToolbarCommand::Confirm:
            if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Default});
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
    if (tool == MarkupTool::Blur) {
        m_state->currentMosaicType = 1;
    }
    if (tool != m_state->currentTool) {
        m_state->isEditingOutlineColor = false;
    }
    m_state->currentTool = tool;
    m_state->isMarkupToolActive = true;
    m_state->isMarking = false;
    closeAllCapturePopups(*m_state, false);
    m_state->toolbarLayoutValid = false;
    if ((int)m_state->state.load() == (int)OverlayState::Marking) m_state->state = OverlayState::Selected;
    m_renderer->markMarkupDirty();
    m_renderer->invalidate();
    updateHoverCursor(m_state->currentCursor);
    syncShortcutHintContext();
}

void CaptureInput::toggleOrSetTool(MarkupTool tool) {
    const bool samePrivacyGroup =
        (tool == MarkupTool::Mosaic || tool == MarkupTool::Blur) &&
        (m_state->currentTool == MarkupTool::Mosaic || m_state->currentTool == MarkupTool::Blur);
    if (m_state->isMarkupToolActive && (m_state->currentTool == tool || samePrivacyGroup)) {
        m_state->isMarkupToolActive = false;
        closeAllCapturePopups(*m_state, false);
        m_state->toolbarLayoutValid = false;
        m_renderer->invalidate();
        updateHoverCursor(m_state->currentCursor);
        syncShortcutHintContext();
    } else {
        m_state->isMarkupToolActive = true;
        const MarkupTool resolvedTool =
            (tool == MarkupTool::Mosaic && m_state->currentMosaicType == 1)
                ? MarkupTool::Blur
                : tool;
        setCurrentTool(resolvedTool);
    }
}

void CaptureInput::syncShortcutHintContext() {
    if (!m_state || !m_state->options.showShortcutHints) return;
    const OverlayState ovState = m_state->state.load();
    if (ovState == OverlayState::Idle) return;

    if (m_state->dragging) {
        ShortcutHintOverlay::instance().hide();
        return;
    }

    ShortcutHintContext ctx = ShortcutHintContext::CaptureSelecting;
    if (m_state->mode == OverlayMode::RecordRegion) {
        ctx = ShortcutHintContext::RecordSelecting;
    } else if (m_state->activeElement &&
               m_state->activeElement->tool == MarkupTool::Text &&
               m_state->activeElement->isEditing) {
        ctx = ShortcutHintContext::CaptureEditingText;
    } else if (ovState == OverlayState::Selected || ovState == OverlayState::Marking) {
        if (m_state->isMarkupToolActive && m_state->currentTool == MarkupTool::Number) {
            ctx = ShortcutHintContext::CaptureMarkingNumber;
        } else if (m_state->isMarkupToolActive) {
            ctx = ShortcutHintContext::CaptureMarking;
        } else {
            ctx = ShortcutHintContext::CaptureSelected;
        }
    }
    ShortcutHintOverlay::instance().show(ctx);
}

void CaptureInput::stepDetectedWindowHierarchy(HWND hwnd, int delta) {
    if (!m_state || m_state->detectedWindowHierarchy.empty() || delta == 0) return;
    const int count = static_cast<int>(m_state->detectedWindowHierarchy.size());
    const int prevIdx = m_state->detectedWindowHierarchyIndex;
    if (delta > 0) {
        m_state->detectedWindowHierarchyIndex = std::min(prevIdx + 1, count - 1);
    } else {
        m_state->detectedWindowHierarchyIndex = std::max(prevIdx - 1, 0);
    }
    if (m_state->detectedWindowHierarchyIndex != prevIdx) {
        const RECT targetRc = m_state->detectedWindowHierarchy[m_state->detectedWindowHierarchyIndex];
        m_state->detectedWindow = targetRc;

        if (m_state->animDetectedWindowRect.right <= m_state->animDetectedWindowRect.left) {
            const RECT prevRc = m_state->detectedWindowHierarchy[prevIdx];
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

        const int idx = m_state->detectedWindowHierarchyIndex;
        const bool zh = tools3000::core::isUiLanguageChinese();
        const std::wstring desc = (idx >= 0 && idx < static_cast<int>(m_state->detectedWindowHierarchyDescriptions.size()))
            ? m_state->detectedWindowHierarchyDescriptions[idx]
            : (zh ? L"选区" : L"Region");
        m_state->hierarchyBadgeText = L"[" + std::to_wstring(idx + 1) + L"/" + std::to_wstring(count) + L"] " + desc;

        if (hwnd) SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
        if (m_renderer) m_renderer->invalidate();
    }
}

bool CaptureInput::isPointInSelection(POINT point) const {
    auto rect = currentSelectionRect();
    if (m_state->isLongImageMode()) {
        const auto& stg = m_state->longCanvas.stageRect;
        if (stg.right > stg.left &&
            (point.x < stg.left || point.x > stg.right || point.y < stg.top || point.y > stg.bottom)) {
            return false;
        }
        const auto doc = m_state->screenToDoc(static_cast<float>(point.x), static_cast<float>(point.y));
        return doc.x >= rect.left && doc.x <= rect.right &&
               doc.y >= rect.top && doc.y <= rect.bottom;
    }
    return point.x >= rect.left && point.x <= rect.right &&
           point.y >= rect.top && point.y <= rect.bottom;
}

cv::Point CaptureInput::toMarkupPointFromDoc(POINT docPoint) const {
    auto rect = currentSelectionRect();
    int x0 = static_cast<int>(std::round(rect.left));
    int y0 = static_cast<int>(std::round(rect.top));
    int width = std::max(1, static_cast<int>(std::round(rect.right - rect.left)));
    int height = std::max(1, static_cast<int>(std::round(rect.bottom - rect.top)));
    return calculateMarkupPoint(docPoint, cv::Rect(x0, y0, width, height));
}

cv::Point CaptureInput::toMarkupPoint(POINT point) const {
    return toMarkupPointFromDoc(m_state->screenToDocPoint(point));
}

void CaptureInput::beginMarkup(POINT point) {
    if (!isPointInSelection(point)) return;
    prepareMarkupBase();
    if (!m_state->markupBaseReady) return;

    cv::Point local = toMarkupPoint(point);
    const float currentDpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
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
            m_state->activeElement->fill = m_state->currentFillMode;
            m_state->activeElement->outlineThickness = static_cast<float>(m_state->currentOutlineWidth);
            m_state->activeElement->hasCustomOutline = (m_state->currentOutlineWidth != 2 || !m_state->currentTextOutlineColor.isAuto());
            m_state->activeElement->isActive = true;
            m_state->activeElement->isEditing = true;
            m_state->activeElement->textCaret = 0;
            m_state->activeElement->textSelAnchor = 0;
            m_state->activeElement->textLastCaretTick = GetTickCount();
            m_state->lastCoalescedUndoElemId = m_state->activeElement->id;
            m_state->lastCoalescedUndoTick = m_state->activeElement->textLastCaretTick;
            m_state->textPreferredCaretX = -1.0f;
            if (m_hwnd) SetTimer(m_hwnd, RENDER_TIMER_ID, 16, nullptr);
        }
        m_state->state = OverlayState::Selected;
        m_renderer->markMarkupDirty();
        m_renderer->invalidate();
        updateHoverCursor(m_state->currentCursor);
        syncShortcutHintContext();
        return;
    }

    const POINT docPt = m_state->screenToDocPoint(point);
    m_state->markupStart = docPt;
    m_state->markupEnd = docPt;
    m_state->penPoints.clear();
    if (m_state->currentTool == MarkupTool::Pen) {
        m_state->penPoints.push_back(local);
    }
    m_state->isMarking = true;
    m_state->state = OverlayState::Marking;
}

void CaptureInput::updateMarkup(POINT point) {
    if (!m_state->isMarking) return;

    m_state->markupEnd = m_state->screenToDocPoint(point);
    if (m_state->currentTool == MarkupTool::Pen) {
        cv::Point local = toMarkupPoint(point);
        if (m_state->penPoints.empty() || m_state->penPoints.back() != local) {
            m_state->penPoints.push_back(local);
        }
    }
}

void CaptureInput::finishMarkup(POINT point) {
    if (!m_state->isMarking) return;

    m_state->markupEnd = m_state->screenToDocPoint(point);
    cv::Point start = toMarkupPointFromDoc(m_state->markupStart);
    cv::Point end = toMarkupPointFromDoc(m_state->markupEnd);

    // Shift 约束：矩形/椭圆保持 1:1 正方/正圆，箭头/直线吸附 45° 正交角度
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        if (m_state->currentTool == MarkupTool::Rectangle || m_state->currentTool == MarkupTool::Ellipse ||
            m_state->currentTool == MarkupTool::Mosaic || m_state->currentTool == MarkupTool::Blur ||
            m_state->currentTool == MarkupTool::Highlight || m_state->currentTool == MarkupTool::Inpaint ||
            m_state->currentTool == MarkupTool::Spotlight || m_state->currentTool == MarkupTool::Watermark) {
            int w = std::abs(end.x - start.x);
            int h = std::abs(end.y - start.y);
            int side = std::max(w, h);
            end.x = start.x + (end.x >= start.x ? side : -side);
            end.y = start.y + (end.y >= start.y ? side : -side);
        } else if (m_state->currentTool == MarkupTool::Arrow || m_state->currentTool == MarkupTool::Line) {
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

    if (m_state->currentTool != MarkupTool::Pen &&
        m_state->currentTool != MarkupTool::Number &&
        m_state->currentTool != MarkupTool::Magnifier &&
        dx < 3 && dy < 3) {
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
                elem->outlineThickness = static_cast<float>(m_state->currentOutlineWidth);
                elem->textOutlineColor = m_state->currentTextOutlineColor;
                elem->hasCustomOutline = m_state->currentFillMode && (m_state->currentOutlineWidth != 2 || !m_state->currentTextOutlineColor.isAuto());
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
                elem->smoothCurve = m_state->currentSmoothCurve;
                elem->hasCustomCaps = m_state->currentLineArrowEnabled;
                elem->startCap = m_state->currentStartCap;
                elem->endCap = m_state->currentEndCap;
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
                elem->smoothCurve = m_state->currentSmoothCurve;
                elem->hasCustomCaps = true;
                elem->startCap = m_state->currentStartCap;
                elem->endCap = m_state->currentEndCap;
                elem->outlineThickness = static_cast<float>(m_state->currentOutlineWidth);
                elem->textOutlineColor = m_state->currentTextOutlineColor;
                elem->hasCustomOutline = (m_state->currentOutlineWidth != 2 || !m_state->currentTextOutlineColor.isAuto());
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
                elem->lineStyle = m_state->currentLineStyle;
                elem->outlineThickness = static_cast<float>(m_state->currentOutlineWidth);
                elem->textOutlineColor = m_state->currentTextOutlineColor;
                elem->hasCustomOutline = m_state->currentFillMode && (m_state->currentOutlineWidth != 2 || !m_state->currentTextOutlineColor.isAuto());
                elem->dpiScale = currentDpiScale;
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Number: {
            auto* elem = m_state->markup.drawNumberMark(
                start, m_state->currentColor, currentDpiScale, m_state->currentNumberShape, m_state->currentNumberFormat);
            if (elem) {
                elem->fill = m_state->currentFillMode;
                elem->thickness = static_cast<float>(m_state->currentStrokeWidth);
                elem->lineStyle = m_state->currentLineStyle;
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

        case MarkupTool::Magnifier: {
            const float dragDist = static_cast<float>(std::hypot(end.x - start.x, end.y - start.y));
            const bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            // 默认直接拖拽即创建引出式旁路放大镜；按住 Shift 拖拽则支持原位同心自定义半径
            const bool isDraggedOffset = !shiftHeld && (dragDist >= 10.0f * currentDpiScale);
            const int defaultRadius = std::clamp(
                static_cast<int>(std::round(static_cast<float>(m_state->dynamicMagnifierRadius) * currentDpiScale)),
                28, 360);
            const int magRadius = (!isDraggedOffset && shiftHeld && dragDist > 6.0f * currentDpiScale)
                ? std::clamp(static_cast<int>(std::round(dragDist)),
                             static_cast<int>(std::round(24.0f * currentDpiScale)), 420)
                : defaultRadius;

            auto* elem = m_state->markup.addMagnifier(start, m_state->dynamicMagnifierScale, magRadius);
            if (elem) {
                elem->color = m_state->currentMagnifierColor;
                elem->fill = m_state->magnifierShowBadge;
                elem->numberShape = m_state->currentMagnifierShape;
                elem->magnifierCalloutStyle = m_state->currentMagnifierCalloutStyle;
                elem->thickness = static_cast<float>(std::max(2, m_state->currentStrokeWidth));
                elem->dpiScale = currentDpiScale;

                const auto sel = currentSelectionRect();
                const int selW = std::max(1, static_cast<int>(std::round(sel.right - sel.left)));
                const int selH = std::max(1, static_cast<int>(std::round(sel.bottom - sel.top)));
                const int margin = static_cast<int>(std::round(4.0f * currentDpiScale));
                auto clampLensToSel = [&](cv::Point p) {
                    const int loX = magRadius + margin;
                    const int hiX = selW - magRadius - margin;
                    const int loY = magRadius + margin;
                    const int hiY = selH - magRadius - margin;
                    return cv::Point(
                        (loX <= hiX) ? std::clamp(p.x, loX, hiX) : (selW / 2),
                        (loY <= hiY) ? std::clamp(p.y, loY, hiY) : (selH / 2));
                };

                if (isDraggedOffset) {
                    elem->endPt = clampLensToSel(end);
                    elem->hasLeaderArrow = std::hypot(static_cast<double>(elem->endPt.x - start.x),
                                                      static_cast<double>(elem->endPt.y - start.y)) > 8.0;
                } else if (!shiftHeld && m_state->magnifierCalloutMode) {
                    // 引出详图模式下原地单击：自动朝选区内最开阔空白方向智能引出，100% 不遮挡原位文字上下文
                    elem->endPt = computeSmartMagnifierCalloutCenter(
                        start, magRadius, m_state->dynamicMagnifierScale, selW, selH, currentDpiScale);
                    elem->hasLeaderArrow = std::hypot(static_cast<double>(elem->endPt.x - start.x),
                                                      static_cast<double>(elem->endPt.y - start.y)) > 8.0;
                } else {
                    elem->endPt = start;
                    elem->hasLeaderArrow = false;
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
                elem->smoothCurve = m_state->currentSmoothCurve;
                elem->dpiScale = currentDpiScale;
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Highlight: {
            auto* elem = m_state->markup.drawHighlight(start, end, m_state->currentColor);
            if (elem) {
                elem->dpiScale = currentDpiScale;
                elem->thickness = static_cast<float>(m_state->currentStrokeWidth);
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Mosaic: {
            MarkupElement* elem = nullptr;
            if (m_state->currentMosaicType == 1) {
                int ksize = (std::max)(5, m_state->currentStrokeWidth * 3);
                elem = m_state->markup.applyBlur(start, end, ksize);
            } else {
                int bs = (std::max)(4, m_state->currentStrokeWidth * 2);
                elem = m_state->markup.applyMosaic(start, end, bs);
            }
            if (elem) {
                elem->dpiScale = currentDpiScale;
                elem->thickness = static_cast<float>(m_state->currentStrokeWidth);
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Blur: {
            int ksize = (std::max)(5, m_state->currentStrokeWidth * 3);
            auto* elem = m_state->markup.applyBlur(start, end, ksize);
            if (elem) {
                elem->dpiScale = currentDpiScale;
                elem->thickness = static_cast<float>(m_state->currentStrokeWidth);
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Inpaint: {
            int radius = std::clamp(m_state->currentStrokeWidth, 2, 16);
            auto* elem = m_state->markup.applyInpaint(start, end, radius);
            if (elem) {
                elem->dpiScale = currentDpiScale;
                elem->thickness = static_cast<float>(m_state->currentStrokeWidth);
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Spotlight: {
            auto* elem = m_state->markup.addSpotlight(
                start, end, m_state->currentColor,
                m_state->currentSpotlightDimAlpha, m_state->currentSpotlightEllipse);
            if (elem) {
                elem->dpiScale = currentDpiScale;
                elem->thickness = static_cast<float>(m_state->currentStrokeWidth);
                elem->cornerRadius = m_state->currentElementCornerRadius;
                elem->spotlightShowBorder = m_state->currentSpotlightBorder;
                m_state->activeElement = elem;
                elem->isActive = true;
            }
            break;
        }

        case MarkupTool::Watermark: {
            std::string wmText = tools3000::core::ConfigManager::instance().get<std::string>(
                "/capture/markup/watermark/text", "CONFIDENTIAL");
            if (wmText.empty()) wmText = "CONFIDENTIAL";
            float wmFontSize = std::clamp(12.0f + static_cast<float>(m_state->currentStrokeWidth) * 1.5f, 12.0f, 64.0f) * currentDpiScale;
            auto* elem = m_state->markup.addWatermark(start, end, wmText, 0.22f, -30.0f);
            if (elem) {
                elem->color = m_state->currentColor;
                elem->fontSize = wmFontSize;
                elem->dpiScale = currentDpiScale;
                elem->thickness = static_cast<float>(m_state->currentStrokeWidth);
                m_state->activeElement = elem;
                elem->isActive = true;
            }
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
    if (!m_state || m_state->frozenScreen.empty()) return;

    auto rect = currentSelectionRect();
    int x = static_cast<int>(std::round(rect.left));
    int y = static_cast<int>(std::round(rect.top));
    int w = static_cast<int>(std::round(rect.right - rect.left));
    int h = static_cast<int>(std::round(rect.bottom - rect.top));
    if (w <= 0 || h <= 0) return;

    cv::Rect targetRect(x, y, w, h);
    syncMarkupBase(m_state->frozenScreen, targetRect, m_state->markupBaseReady, m_state->markupBaseRect, m_state->markup);
    if (m_renderer) {
        m_renderer->markMarkupDirty();
    }
}

void CaptureInput::rebuildMarkupBase() {
    prepareMarkupBase();
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
        wchar_t cls[64] = {};
        if (GetClassNameW(hwnd, cls, 64) > 0 && wcscmp(cls, L"Tools3000_ShortcutHintOverlay") == 0) {
            return TRUE;
        }
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

                if (!m_state->frozenScreen.empty()) {
                    cv::Rect clamped = clampToScreenBounds(rc, m_state->frozenScreen.cols, m_state->frozenScreen.rows);
                    rc.left = clamped.x;
                    rc.top = clamped.y;
                    rc.right = clamped.x + clamped.width;
                    rc.bottom = clamped.y + clamped.height;
                    if (clamped.width <= 8 || clamped.height <= 8) {
                        curr = GetParent(curr);
                        continue;
                    }
                }

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
    if (!this || !m_state || !m_renderer) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // 当截图会话已结束处于 Idle 态时，严格拦截所有残留排队输入消息，
    // 防止窗口隐藏/销毁过渡期排队的鼠标移动或点击重新进入选区态并残留在屏幕上弹出快捷键 HUD。
    if (m_state->state.load() == OverlayState::Idle) {
        if (msg == WM_TIMER && wParam == RENDER_TIMER_ID) {
            KillTimer(hwnd, RENDER_TIMER_ID);
            return 0;
        }
        switch (msg) {
            case WM_SETCURSOR:
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MOUSEWHEEL:
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_CHAR:
            case WM_IME_STARTCOMPOSITION:
            case WM_IME_COMPOSITION:
            case WM_IME_ENDCOMPOSITION:
                return 0;
            default:
                break;
        }
    }

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
        case WM_MBUTTONDOWN: {
            if (this && m_state && m_state->isLongImageMode()) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                m_state->longCanvas.middleButtonPanning = true;
                m_state->longCanvas.panLastCursor = pt;
                updateHoverCursor(pt);
                return 0;
            }
            break;
        }
        case WM_MBUTTONUP: {
            if (this && m_state && m_state->longCanvas.middleButtonPanning) {
                m_state->longCanvas.middleButtonPanning = false;
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                updateHoverCursor(pt);
                return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            if (!this) break;
            CaptureShortcutPolicy::cancelShiftTapCandidate(m_state->shiftTapCandidate);
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

            if ((int)m_state->state.load() == (int)OverlayState::Selected || (int)m_state->state.load() == (int)OverlayState::Marking) {
                // 0. 如果滑块微浮岛弹窗处于展开状态，优先响应无级滑块轨道点击与拖拽
                if (m_state->sliderPopup.type != SliderPopupType::None) {
                    float px = static_cast<float>(point.x);
                    float py = static_cast<float>(point.y);
                    auto pr = m_state->sliderPopup.popupRect;
                    if (px >= pr.left && px <= pr.right && py >= pr.top && py <= pr.bottom) {
                        // 检测是否点击或拖拽滑动轨道
                        auto tr = m_state->sliderPopup.trackRect;
                        if (px >= tr.left - 8.0f && px <= tr.right + 8.0f && py >= tr.top - 12.0f && py <= tr.bottom + 12.0f) {
                            if (m_state->activeElement &&
                                m_state->sliderPopup.type != SliderPopupType::SelectionCornerRadius) {
                                m_state->markup.pushUndoSnapshot();
                                resetCoalescedUndo(*m_state);
                            }
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
                            } else if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim) {
                                m_state->currentSpotlightDimAlpha = static_cast<float>(std::clamp(val, 15, 85)) / 100.0f;
                                tools3000::core::ConfigManager::instance().set<int>("/capture/markup/spotlightDim", std::clamp(val, 15, 85));
                            } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                                if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                                    m_state->activeElement->fontSize = static_cast<float>(val);
                                    m_state->activeElement->textRenderSize = cv::Size(0, 0);
                                }
                                const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                                tools3000::core::ConfigManager::instance().set<double>("/capture/markup/text/fontSize", static_cast<double>(val) / curDpi);
                            } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                                m_state->currentOutlineWidth = std::clamp(val, 0, 12);
                                if (m_state->currentTool == MarkupTool::Text) {
                                    m_state->currentTextOutline = (m_state->currentOutlineWidth > 0);
                                }
                            } else {
                                m_state->currentStrokeWidth = val;
                            }
                            if (m_state->activeElement) {
                                if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                                    m_state->activeElement->cornerRadius = static_cast<float>(val);
                                } else if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim) {
                                    if (m_state->activeElement->tool == MarkupTool::Spotlight) {
                                        m_state->activeElement->spotlightDimAlpha = m_state->currentSpotlightDimAlpha;
                                    }
                                } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                                    m_state->activeElement->fontSize = static_cast<float>(val);
                                    m_state->activeElement->textRenderSize = cv::Size(0, 0);
                                } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                                    m_state->activeElement->outlineThickness = static_cast<float>(std::clamp(val, 0, 12));
                                    m_state->activeElement->hasCustomOutline = true;
                                    m_state->activeElement->textOutline = (val > 0);
                                    if (m_state->activeElement->tool == MarkupTool::Text) {
                                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                                    }
                                } else if (m_state->sliderPopup.type != SliderPopupType::SelectionCornerRadius) {
                                    m_state->activeElement->thickness = static_cast<float>(val);
                                    if (m_state->activeElement->tool == MarkupTool::Mosaic) {
                                        m_state->activeElement->mosaicBlockSize = (std::max)(4, val * 2);
                                    } else if (m_state->activeElement->tool == MarkupTool::Blur) {
                                        m_state->activeElement->mosaicBlockSize = (std::max)(5, val * 3);
                                    } else if (m_state->activeElement->tool == MarkupTool::Inpaint) {
                                        m_state->activeElement->inpaintRadius = std::clamp(val, 2, 16);
                                    } else if (m_state->activeElement->tool == MarkupTool::Watermark) {
                                        const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                                        m_state->activeElement->fontSize = std::clamp(12.0f + static_cast<float>(val) * 1.5f, 12.0f, 64.0f) * curDpi;
                                    }
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

                float px = static_cast<float>(point.x);
                float py = static_cast<float>(point.y);

                // 1. 严格按渲染 Z 序自顶向下检测已展开弹出层（尺寸菜单 / 下拉菜单 / 样式子菜单），优先级高于底层 HUD 与二维码胶囊
                if (m_state->isSizeMenuOpen) {
                    auto mr = m_state->sizeMenuRect;
                    if (px >= mr.left && px <= mr.right && py >= mr.top && py <= mr.bottom) {
                        const float scale = std::clamp(m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
                        const auto lay = computeSizeMenuLayoutFromRect(mr, scale);
                        const auto inR = [&](const D2D1_RECT_F& r) {
                            return px >= r.left && px <= r.right && py >= r.top && py <= r.bottom;
                        };

                        if (inR(lay.segmentTrackRect)) {
                            const float segMidX = (lay.segmentTrackRect.left + lay.segmentTrackRect.right) * 0.5f;
                            m_state->sizeUnit = (px <= segMidX)
                                ? CaptureState::SizeUnit::Pixel
                                : CaptureState::SizeUnit::DeviceIndependentPixel;
                            tools3000::core::ConfigManager::instance().set<std::string>(
                                "/capture/sizeUnit",
                                m_state->sizeUnit == CaptureState::SizeUnit::DeviceIndependentPixel ? "dp" : "px");
                        } else if (inR(lay.positionRowRect)) {
                            m_state->showPositionInHud = !m_state->showPositionInHud;
                            tools3000::core::ConfigManager::instance().set<bool>(
                                "/capture/showSizePosition", m_state->showPositionInHud);
                        } else if (inR(lay.unitRowRect)) {
                            m_state->showUnitInHud = !m_state->showUnitInHud;
                            tools3000::core::ConfigManager::instance().set<bool>(
                                "/capture/showSizeUnit", m_state->showUnitInHud);
                        }
                        m_renderer->invalidate();
                        return 0;
                    } else {
                        auto hr = m_state->sizeHudRect;
                        m_state->isSizeMenuOpen = false;
                        m_renderer->invalidate();
                        // 若点击的就是尺寸胶囊自身，则已完成 Toggle 收起，直接返回
                        if (px >= hr.left && px <= hr.right && py >= hr.top && py <= hr.bottom) {
                            return 0;
                        }
                    }
                }

                // 1.2 微晶下拉菜单（聚光灯/几何图形、序号样式、美化外壳、录屏设置）点击与面板内边距防穿透
                if (m_state->dropdownMenu.type != DropdownType::None) {
                    int hitItem = -1;
                    for (int i = 0; i < static_cast<int>(m_state->dropdownMenu.items.size()); ++i) {
                        const auto& item = m_state->dropdownMenu.items[i];
                        if (px >= item.rect.left && px <= item.rect.right &&
                            py >= item.rect.top && py <= item.rect.bottom) {
                            hitItem = i;
                            break;
                        }
                    }
                    if (hitItem >= 0) {
                        commitDropdownSelection(m_state->dropdownMenu.type, m_state->dropdownMenu.items[hitItem].id);
                        return 0;
                    }
                    const auto dmRect = m_state->dropdownMenu.menuRect;
                    if (dmRect.right > dmRect.left && px >= dmRect.left && px <= dmRect.right &&
                        py >= dmRect.top && py <= dmRect.bottom) {
                        return 0; // 点击在下拉面板内边距中，消费事件防止穿透底层控件
                    }
                    const auto anchorRc = m_state->dropdownMenu.anchorButtonRect;
                    tools3000::capture::closeDropdownMenu(*m_state, true);
                    m_renderer->invalidate();
                    if (anchorRc.right > anchorRc.left && px >= anchorRc.left && px <= anchorRc.right &&
                        py >= anchorRc.top && py <= anchorRc.bottom) {
                        return 0; // 点击原触发按钮完成 Toggle 收起，避免立刻重开或误切换工具
                    }
                }

                // 1.3 线型 / 箭头样式子菜单点击与外部自动收起
                SubmenuType dismissedSubmenu = SubmenuType::None;
                if (m_state->openSubmenu != SubmenuType::None) {
                    for (const auto& sbtn : m_state->submenuButtons) {
                        if (px >= sbtn.rect.left && px <= sbtn.rect.right &&
                            py >= sbtn.rect.top && py <= sbtn.rect.bottom) {
                            const ToolbarButton copied = sbtn;
                            executeToolbarCommand(copied);
                            m_renderer->invalidate();
                            return 0;
                        }
                    }
                    const auto smRect = m_state->openSubmenuRect;
                    if (smRect.right > smRect.left && px >= smRect.left && px <= smRect.right &&
                        py >= smRect.top && py <= smRect.bottom) {
                        return 0;
                    }
                    dismissedSubmenu = m_state->openSubmenu;
                    m_state->openSubmenu = SubmenuType::None;
                    m_renderer->invalidate();
                }

                // 2. 检测工具栏手柄与工具栏按钮（高于尺寸胶囊与二维码胶囊，防止拖拽后的工具栏被底层胶囊抢走点击）
                HitArea selHit = HitArea::None;
                rebuildToolbarButtons(currentSelectionRect());
                if (m_state->toolbarGripRect.right > m_state->toolbarGripRect.left &&
                    px >= m_state->toolbarGripRect.left && px <= m_state->toolbarGripRect.right &&
                    py >= m_state->toolbarGripRect.top && py <= m_state->toolbarGripRect.bottom) {
                    closeAllCapturePopups(*m_state, true);
                    m_state->isDraggingToolbar = true;
                    m_state->toolbarDragOffset = D2D1::Point2F(
                        px - m_state->primaryToolbarRect.left,
                        py - m_state->primaryToolbarRect.top);
                    updateHoverCursor(point);
                    m_renderer->invalidate();
                    return 0;
                }
                if (auto* button = hitTestToolbar(point)) {
                    if (dismissedSubmenu != SubmenuType::None) {
                        const bool sameSubmenuTrigger =
                            (dismissedSubmenu == SubmenuType::LineStyle && button->command == ToolbarCommand::ToggleLineStyleDropdown) ||
                            (dismissedSubmenu == SubmenuType::ArrowStyle && button->command == ToolbarCommand::ToggleArrowStyleDropdown) ||
                            (dismissedSubmenu == SubmenuType::StartCapStyle && button->command == ToolbarCommand::ToggleStartCapDropdown) ||
                            (dismissedSubmenu == SubmenuType::EndCapStyle && button->command == ToolbarCommand::ToggleEndCapDropdown);
                        if (sameSubmenuTrigger) {
                            m_renderer->invalidate();
                            return 0;
                        }
                    }
                    // 主工具栏形状组按钮（第 1 个）：已激活形状/聚光灯时点击右下角小箭头区，直接弹出「几何图形 / 聚光灯」切换菜单
                    if (!button->isSecondary && !m_state->toolbarButtons.empty() &&
                        button == &m_state->toolbarButtons[0] && button->hasDropdown &&
                        m_state->mode == OverlayMode::Screenshot) {
                        const float curScale = (m_state->dpiScale > 0.0f) ? m_state->dpiScale : 1.0f;
                        const bool isShapeOrSpotActive = m_state->isMarkupToolActive &&
                            (m_state->currentTool == MarkupTool::Rectangle ||
                             m_state->currentTool == MarkupTool::Ellipse ||
                             m_state->currentTool == MarkupTool::Line ||
                             m_state->currentTool == MarkupTool::Spotlight);
                        if (isShapeOrSpotActive && px >= button->rect.right - 11.0f * curScale) {
                            openDropdownMenu(DropdownType::ShapeMode, button->rect);
                            m_renderer->invalidate();
                            return 0;
                        }
                    }
                    // 如果正在编辑文字且点击的不是文本属性设置按钮，退出编辑
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                        if (button->command != ToolbarCommand::SelectColor &&
                            button->command != ToolbarCommand::ChooseCustomColor &&
                            button->command != ToolbarCommand::ToggleFill &&
                            button->command != ToolbarCommand::CycleStrokeWidth &&
                            button->command != ToolbarCommand::ToggleTextOutline &&
                            button->command != ToolbarCommand::SelectTextOutlineColor) {
                            m_state->activeElement->isEditing = false;
                            m_state->isSelectingText = false;
                            if (m_state->activeElement->text.empty()) {
                                m_state->markup.removeElement(m_state->activeElement->id);
                                m_state->activeElement = nullptr;
                            }
                            m_renderer->markMarkupDirty();
                            syncShortcutHintContext();
                        }
                    }
                    executeToolbarCommand(*button);
                    return 0;
                }

                // 若点击落在悬浮工具栏玻璃底板的按钮间隙内，直接消费事件，防止穿透到底层画布误画标注或挪动选区
                auto inToolbarPanel = [&](const D2D1_RECT_F& rc) {
                    return rc.right > rc.left && px >= rc.left && px <= rc.right && py >= rc.top && py <= rc.bottom;
                };
                if (inToolbarPanel(m_state->primaryToolbarRect) ||
                    inToolbarPanel(m_state->secondaryToolbarRect) ||
                    inToolbarPanel(m_state->selectionSideRect)) {
                    return 0;
                }

                // 2.3. 长图精修工作台专属导航舱（顶部首尾速跳/重置/缩放胶囊 + 右侧全景 Minimap + Space 临时抓手）
                if (m_state->isLongImageMode()) {
                    const float curScale = std::clamp(m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
                    if (inToolbarPanel(m_state->longCanvas.topHeaderRect)) {
                        closeAllCapturePopups(*m_state, true);
                        if (inToolbarPanel(m_state->longCanvas.btnJumpTopRect)) {
                            const float topDocRow = static_cast<float>(std::min(m_state->dragStart.y, m_state->dragEnd.y));
                            m_state->scrollLongCanvasToDocRow(topDocRow, 0.12f);
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                        } else if (inToolbarPanel(m_state->longCanvas.btnJumpBottomRect)) {
                            const float botDocRow = static_cast<float>(std::max(m_state->dragStart.y, m_state->dragEnd.y));
                            m_state->scrollLongCanvasToDocRow(botDocRow, 0.82f);
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                        } else if (inToolbarPanel(m_state->longCanvas.btnResetCropRect)) {
                            m_state->dragStart = {0, 0};
                            m_state->dragEnd = {m_state->longCanvas.docWidth, m_state->longCanvas.docHeight};
                            prepareMarkupBase();
                            const bool zh = tools3000::core::isUiLanguageChinese();
                            m_state->loupeToastMessage = zh ? L"已无损恢复完整长图" : L"Restored full long image";
                            m_state->loupeToastUntil = GetTickCount() + 1400;
                            SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                        } else if (inToolbarPanel(m_state->longCanvas.btnZoomToggleRect)) {
                            const auto& stg = m_state->longCanvas.stageRect;
                            const auto centerDoc = m_state->screenToDoc(
                                (stg.left + stg.right) * 0.5f,
                                (stg.top + stg.bottom) * 0.5f);
                            const float fitZ = std::clamp(m_state->longCanvas.fitZoom, 0.15f, 4.0f);
                            if (std::abs(m_state->longCanvas.zoom - 1.0f) > 0.03f) {
                                m_state->longCanvas.zoom = 1.0f;
                            } else {
                                m_state->longCanvas.zoom = (std::abs(fitZ - 1.0f) > 0.03f) ? fitZ : 0.75f;
                            }
                            m_state->longCanvas.scrollX = 0.0f;
                            m_state->scrollLongCanvasToDocRow(centerDoc.y, 0.5f);
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                        }
                        updateHoverCursor(point);
                        return 0;
                    }

                    if (inToolbarPanel(m_state->longCanvas.minimapPanelRect)) {
                        closeAllCapturePopups(*m_state, true);
                        const auto& mmImg = m_state->longCanvas.minimapImageRect;
                        const int curTop = std::min(m_state->dragStart.y, m_state->dragEnd.y);
                        const int curBot = std::max(m_state->dragStart.y, m_state->dragEnd.y);
                        const float mmTopY = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(
                            curTop, mmImg, m_state->longCanvas.docHeight);
                        const float mmBotY = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(
                            curBot, mmImg, m_state->longCanvas.docHeight);
                        const float hitBand = 9.5f * curScale;

                        if (px >= mmImg.left - 10.0f * curScale && px <= mmImg.right + 10.0f * curScale &&
                            std::abs(py - mmTopY) <= hitBand) {
                            m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::MinimapCropTop;
                        } else if (px >= mmImg.left - 10.0f * curScale && px <= mmImg.right + 10.0f * curScale &&
                                   std::abs(py - mmBotY) <= hitBand) {
                            m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::MinimapCropBottom;
                        } else {
                            m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::MinimapViewport;
                            const int targetRow = ScrollCaptureEditorOverlay::mapMinimapYToImageRow(
                                py, mmImg, m_state->longCanvas.docHeight);
                            m_state->scrollLongCanvasToDocRow(static_cast<float>(targetRow), 0.5f);
                            m_state->toolbarLayoutValid = false;
                            m_renderer->invalidate();
                        }
                        updateHoverCursor(point);
                        return 0;
                    }

                    if (((GetKeyState(VK_SPACE) & 0x8000) != 0) && inToolbarPanel(m_state->longCanvas.stageRect)) {
                        m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::PanCanvas;
                        m_state->longCanvas.panLastCursor = point;
                        updateHoverCursor(point);
                        return 0;
                    }
                }

                // 2.4. 检测是否点击尺寸胶囊（HUD）以展开菜单（互斥收起其他弹出层）
                auto hr = m_state->sizeHudRect;
                if (px >= hr.left && px <= hr.right && py >= hr.top && py <= hr.bottom) {
                    const bool willOpen = !m_state->isSizeMenuOpen;
                    closeAllCapturePopups(*m_state, true);
                    m_state->isSizeMenuOpen = willOpen;
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
                        m_renderer->markMarkupDirty();
                        syncShortcutHintContext();
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
                    m_state->longCanvas.lastSelAdjustDocPt = m_state->screenToDoc(px, py);
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
                            const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
                            if (m_state->activeElement->isEditing && activeElemHit == HitArea::Body && !altDown) {
                                // 编辑态下点击文字内容区：定位字符级光标，开启按住左键拖拽划词选中（绝不拖走整个文本框）
                                const std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                                const auto layout = CaptureTextEditor::buildLayout(
                                    wstr, static_cast<int>(m_state->activeElement->fontSize));
                                const float relX = static_cast<float>(local.x - m_state->activeElement->startPt.x);
                                const float relY = static_cast<float>(local.y - m_state->activeElement->startPt.y);
                                const size_t hitIdx = CaptureTextEditor::hitTestCaretIndex(layout, wstr, relX, relY);
                                const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                                m_state->activeElement->textCaret = hitIdx;
                                if (!shiftDown) {
                                    m_state->activeElement->textSelAnchor = hitIdx;
                                }
                                m_state->activeElement->textLastCaretTick = GetTickCount();
                                m_state->textPreferredCaretX = -1.0f;
                                m_state->isSelectingText = true;
                                m_state->isManipulating = false;
                                m_state->lastMousePos = point;
                                SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                                m_renderer->markMarkupDirty();
                                m_renderer->invalidate();
                                updateHoverCursor(point);
                                return 0;
                            }
                            if (activeElemHit == HitArea::TextMoveBorder ||
                                (m_state->activeElement->isEditing && altDown && activeElemHit == HitArea::Body)) {
                                // 点击外围聚焦虚线框环带（或按住 Alt 拖内部）：保持编辑态并平移整个文本框
                                m_state->isSelectingText = false;
                                m_state->manipulateStartSnapshot = m_state->markup.captureSnapshot();
                                m_state->hasManipulateSnapshot = true;
                                resetCoalescedUndo(*m_state);
                                m_state->dragHandle = HitArea::TextMoveBorder;
                                m_state->isManipulating = true;
                                m_state->lastMousePos = point;
                                m_state->longCanvas.lastManipulateDocPt = m_state->screenToDoc(px, py);
                                m_renderer->invalidate();
                                updateHoverCursor(point);
                                return 0;
                            }
                            if (!m_state->activeElement->isEditing && activeElemHit == HitArea::Body) {
                                m_state->isSelectingText = false;
                                m_state->manipulateStartSnapshot = m_state->markup.captureSnapshot();
                                m_state->hasManipulateSnapshot = true;
                                resetCoalescedUndo(*m_state);
                                m_state->textManipulateStart = point;
                                m_state->dragHandle = HitArea::None;
                                m_state->isManipulating = true;
                                m_state->lastMousePos = point;
                                m_state->longCanvas.lastManipulateDocPt = m_state->screenToDoc(px, py);
                                m_renderer->invalidate();
                                updateHoverCursor(point);
                                return 0;
                            }
                            m_state->isSelectingText = false;
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

                        LOG_DEBUG("CaptureInput: active element handle selected hitArea={}, tool={}",
                                  static_cast<int>(activeElemHit), static_cast<int>(m_state->activeElement->tool));
                        m_state->manipulateStartSnapshot = m_state->markup.captureSnapshot();
                        m_state->hasManipulateSnapshot = true;
                        resetCoalescedUndo(*m_state);
                        m_state->dragHandle = (activeElemHit == HitArea::Body) ? HitArea::None : activeElemHit;
                        m_state->isManipulating = true;
                        m_state->lastMousePos = point;
                        m_state->longCanvas.lastManipulateDocPt = m_state->screenToDoc(px, py);
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
                        m_state->isSelectingText = false;

                        // 全标注工具二次编辑框架：同步选中元素的工具类型与全部属性至全局状态
                        m_state->isMarkupToolActive = true;
                        m_state->currentTool = hit.element->tool;
                        m_state->currentStrokeWidth = static_cast<int>(std::round(hit.element->thickness));
                        m_state->currentLineStyle = hit.element->lineStyle;
                        m_state->currentArrowStyle = hit.element->arrowStyle;
                        m_state->currentSmoothCurve = hit.element->smoothCurve;
                        if (hit.element->tool == MarkupTool::Line) {
                            m_state->currentLineArrowEnabled = hit.element->hasCustomCaps;
                        }
                        if (hit.element->hasCustomCaps) {
                            m_state->currentStartCap = hit.element->startCap;
                            m_state->currentEndCap = hit.element->endCap;
                        } else if (hit.element->tool == MarkupTool::Arrow) {
                            switch (hit.element->arrowStyle) {
                                case ArrowStyle::Thin:
                                    m_state->currentStartCap = LineCapStyle::None;
                                    m_state->currentEndCap = LineCapStyle::OpenArrow;
                                    break;
                                case ArrowStyle::Tech:
                                    m_state->currentStartCap = LineCapStyle::None;
                                    m_state->currentEndCap = LineCapStyle::StealthArrow;
                                    break;
                                case ArrowStyle::DoubleEnded:
                                    m_state->currentStartCap = LineCapStyle::SolidTriangle;
                                    m_state->currentEndCap = LineCapStyle::SolidTriangle;
                                    break;
                                case ArrowStyle::Standard:
                                default:
                                    m_state->currentStartCap = LineCapStyle::None;
                                    m_state->currentEndCap = LineCapStyle::SolidTriangle;
                                    break;
                            }
                        }
                        m_state->currentElementCornerRadius = hit.element->cornerRadius;
                        if (hit.element->tool == MarkupTool::Magnifier) {
                            m_state->currentMagnifierColor = hit.element->color;
                            m_state->magnifierShowBadge = hit.element->fill;
                            m_state->currentMagnifierShape = hit.element->numberShape;
                            m_state->dynamicMagnifierScale = hit.element->magnifierScale;
                            const bool unsetEnd = !hit.element->hasLeaderArrow &&
                                                  hit.element->endPt.x == 0 && hit.element->endPt.y == 0 &&
                                                  (hit.element->startPt.x != 0 || hit.element->startPt.y != 0);
                            const cv::Point lc = unsetEnd ? hit.element->startPt : hit.element->endPt;
                            m_state->magnifierCalloutMode =
                                std::hypot(static_cast<double>(lc.x - hit.element->startPt.x),
                                           static_cast<double>(lc.y - hit.element->startPt.y)) > 8.0;
                        } else if (hit.element->tool == MarkupTool::Spotlight) {
                            m_state->currentColor = hit.element->color;
                            m_state->currentSpotlightEllipse = hit.element->fill;
                            m_state->currentSpotlightDimAlpha = hit.element->spotlightDimAlpha;
                            m_state->currentSpotlightBorder = hit.element->spotlightShowBorder;
                        } else {
                            m_state->currentColor = hit.element->color;
                            m_state->currentFillMode = hit.element->fill;
                            m_state->currentNumberShape = hit.element->numberShape;
                            if (hit.element->tool == MarkupTool::Number) {
                                m_state->currentNumberFormat = hit.element->numberFormat;
                            }
                        }
                        if (hit.element->tool == MarkupTool::Blur) {
                            m_state->currentMosaicType = 1;
                        } else if (hit.element->tool == MarkupTool::Mosaic) {
                            m_state->currentMosaicType = 0;
                        }
                        if (hit.element->tool == MarkupTool::Text) {
                            m_state->currentTextOutline = hit.element->textOutline;
                            m_state->currentTextOutlineColor = hit.element->textOutlineColor;
                        } else if (hit.element->hasCustomOutline) {
                            m_state->currentOutlineWidth = static_cast<int>(std::round(hit.element->outlineThickness));
                            m_state->currentTextOutlineColor = hit.element->textOutlineColor;
                        }
                        m_state->toolbarLayoutValid = false;

                        m_state->activeElement->isEditing = false;
                        if (hit.element->tool == MarkupTool::Text) {
                            m_state->textManipulateStart = point;
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

                        m_state->manipulateStartSnapshot = m_state->markup.captureSnapshot();
                        m_state->hasManipulateSnapshot = true;
                        resetCoalescedUndo(*m_state);
                        m_state->dragHandle = (hit.area == HitArea::Body || hit.area == HitArea::TextMoveBorder)
                            ? HitArea::None
                            : hit.area;
                        m_state->isManipulating = true;
                        m_state->lastMousePos = point;
                        m_state->longCanvas.lastManipulateDocPt = m_state->screenToDoc(px, py);
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        updateHoverCursor(point);
                        return 0;
                    }

                    // 点击在空白区域：
                    // 若此前正在编辑文本，点击空白区域应提交并退出文本编辑
                    bool wasEditingText = false;
                    m_state->isSelectingText = false;
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
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                    }

                    // 若刚退出文本编辑态，仅提交并恢复就绪态，不立即连续落点新建第二个文本框
                    if (wasEditingText && m_state->currentTool == MarkupTool::Text) {
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        updateHoverCursor(point);
                        syncShortcutHintContext();
                        return 0;
                    }
                    if (wasEditingText) {
                        syncShortcutHintContext();
                    }

                    // 若未激活标注工具：长图模式下抓手上下平移画布，普通截图下整体拖拽调整选区位置
                    if (!m_state->isMarkupToolActive) {
                        if (m_state->isLongImageMode()) {
                            m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::PanCanvas;
                            m_state->longCanvas.panLastCursor = point;
                        } else {
                            m_state->isAdjustingSelection = true;
                            m_state->selAdjustHandle = HitArea::Body;
                            m_state->selAdjustLast = point;
                        }
                        updateHoverCursor(point);
                        return 0;
                    }

                    // 在选区内部空白区域立即启动新标注绘制流程
                    beginMarkup(point);
                    return 0;
                }
                // 长图模式下点击舞台内已裁除暗罩或两侧留白区域：取消编辑/选中并开启抓手浏览画布
                if (m_state->isLongImageMode() && inToolbarPanel(m_state->longCanvas.stageRect)) {
                    if (m_state->activeElement) {
                        m_state->activeElement->isActive = false;
                        m_state->activeElement->isEditing = false;
                        if (m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->text.empty()) {
                            m_state->markup.removeElement(m_state->activeElement->id);
                        }
                        m_state->activeElement = nullptr;
                        m_renderer->markMarkupDirty();
                        syncShortcutHintContext();
                    }
                    m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::PanCanvas;
                    m_state->longCanvas.panLastCursor = point;
                    m_renderer->invalidate();
                    updateHoverCursor(point);
                }
                return 0;
            }

            m_state->dragStart = point;
            m_state->dragEnd = m_state->dragStart;
            m_state->lastMousePos = point;
            m_state->dragging = true;
            m_state->state = OverlayState::Selecting;
            m_state->isMarkupToolActive = false;
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

            if ((int)m_state->state.load() == (int)OverlayState::Selected ||
                (int)m_state->state.load() == (int)OverlayState::Marking) {
                const auto& gr = m_state->toolbarGripRect;
                const bool gripHover = (gr.right > gr.left &&
                                        m_state->currentCursor.x >= gr.left &&
                                        m_state->currentCursor.x <= gr.right &&
                                        m_state->currentCursor.y >= gr.top &&
                                        m_state->currentCursor.y <= gr.bottom);
                if (gripHover != m_state->isToolbarGripHovered) {
                    m_state->isToolbarGripHovered = gripHover;
                    m_renderer->invalidate();
                }

                if (m_state->isLongImageMode()) {
                    const float cpx = static_cast<float>(m_state->currentCursor.x);
                    const float cpy = static_cast<float>(m_state->currentCursor.y);
                    const auto inBtn = [&](const D2D1_RECT_F& rc) {
                        return rc.right > rc.left && cpx >= rc.left && cpx <= rc.right && cpy >= rc.top && cpy <= rc.bottom;
                    };
                    int hovHdr = -1;
                    if (inBtn(m_state->longCanvas.btnJumpTopRect)) hovHdr = 0;
                    else if (inBtn(m_state->longCanvas.btnJumpBottomRect)) hovHdr = 1;
                    else if (inBtn(m_state->longCanvas.btnResetCropRect)) hovHdr = 2;
                    else if (inBtn(m_state->longCanvas.btnZoomToggleRect)) hovHdr = 3;
                    if (hovHdr != m_state->longCanvas.hoveredHeaderBtn) {
                        m_state->longCanvas.hoveredHeaderBtn = hovHdr;
                        m_renderer->invalidate();
                    }
                }
            }

            if (m_state->isLongImageMode() &&
                (m_state->longCanvas.middleButtonPanning ||
                 m_state->longCanvas.dragMode != CaptureState::LongCanvasDragMode::None)) {
                const bool mbHeld = (wParam & MK_MBUTTON) != 0;
                const bool lbHeld = (wParam & MK_LBUTTON) != 0;
                if (m_state->longCanvas.middleButtonPanning && !mbHeld) {
                    m_state->longCanvas.middleButtonPanning = false;
                }
                if (m_state->longCanvas.dragMode != CaptureState::LongCanvasDragMode::None && !lbHeld) {
                    m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::None;
                }

                if (m_state->longCanvas.middleButtonPanning ||
                    m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::PanCanvas) {
                    const float dx = static_cast<float>(m_state->currentCursor.x - m_state->longCanvas.panLastCursor.x);
                    const float dy = static_cast<float>(m_state->currentCursor.y - m_state->longCanvas.panLastCursor.y);
                    m_state->longCanvas.scrollX -= dx;
                    m_state->longCanvas.scrollY -= dy;
                    m_state->clampLongCanvasScroll();
                    m_state->longCanvas.panLastCursor = m_state->currentCursor;
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }

                if (m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::MinimapViewport) {
                    const int targetRow = ScrollCaptureEditorOverlay::mapMinimapYToImageRow(
                        static_cast<float>(m_state->currentCursor.y),
                        m_state->longCanvas.minimapImageRect,
                        m_state->longCanvas.docHeight);
                    m_state->scrollLongCanvasToDocRow(static_cast<float>(targetRow), 0.5f);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }

                if (m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::MinimapCropTop ||
                    m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::MinimapCropBottom) {
                    const bool isTop = (m_state->longCanvas.dragMode == CaptureState::LongCanvasDragMode::MinimapCropTop);
                    const int mappedRow = ScrollCaptureEditorOverlay::mapMinimapYToImageRow(
                        static_cast<float>(m_state->currentCursor.y),
                        m_state->longCanvas.minimapImageRect,
                        m_state->longCanvas.docHeight);
                    ScrollCropRange cand{
                        std::min(m_state->dragStart.y, m_state->dragEnd.y),
                        std::max(m_state->dragStart.y, m_state->dragEnd.y),
                        std::min(m_state->dragStart.x, m_state->dragEnd.x),
                        std::max(m_state->dragStart.x, m_state->dragEnd.x)
                    };
                    if (isTop) cand.topRow = mappedRow;
                    else cand.bottomRow = mappedRow;
                    const auto clamped = ScrollCaptureEditorOverlay::clampScrollCropRange(
                        cand, m_state->longCanvas.docWidth, m_state->longCanvas.docHeight,
                        ScrollCaptureEditorOverlay::kMinCropDimensionPx, isTop ? 1 : 2);
                    m_state->dragStart = {clamped.leftCol, clamped.topRow};
                    m_state->dragEnd = {clamped.rightCol, clamped.bottomRow};
                    m_state->scrollLongCanvasToDocRow(
                        static_cast<float>(isTop ? clamped.topRow : clamped.bottomRow),
                        isTop ? 0.24f : 0.76f);
                    if (m_state->markup.hasAnyMarkup()) {
                        rebuildMarkupBase();
                        m_renderer->markMarkupDirty();
                    } else {
                        m_state->markupBaseReady = false;
                        m_state->markupBaseRect = {0, 0, 0, 0};
                    }
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
            }

            if (m_state->isDraggingToolbar) {
                if ((wParam & MK_LBUTTON) == 0) {
                    m_state->isDraggingToolbar = false;
                } else {
                    m_state->hasCustomToolbarPos = true;
                    m_state->customToolbarOrigin = D2D1::Point2F(
                        static_cast<float>(m_state->currentCursor.x) - m_state->toolbarDragOffset.x,
                        static_cast<float>(m_state->currentCursor.y) - m_state->toolbarDragOffset.y);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    m_state->lastMousePos = m_state->currentCursor;
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
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
                } else if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim) {
                    m_state->currentSpotlightDimAlpha = static_cast<float>(std::clamp(val, 15, 85)) / 100.0f;
                    tools3000::core::ConfigManager::instance().set<int>("/capture/markup/spotlightDim", std::clamp(val, 15, 85));
                } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                        m_state->activeElement->fontSize = static_cast<float>(val);
                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                    }
                    const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                    tools3000::core::ConfigManager::instance().set<double>("/capture/markup/text/fontSize", static_cast<double>(val) / curDpi);
                } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                    m_state->currentOutlineWidth = std::clamp(val, 0, 12);
                    if (m_state->currentTool == MarkupTool::Text) {
                        m_state->currentTextOutline = (m_state->currentOutlineWidth > 0);
                    }
                } else {
                    m_state->currentStrokeWidth = val;
                }
                if (m_state->activeElement) {
                    if (m_state->sliderPopup.type == SliderPopupType::CornerRadius) {
                        m_state->activeElement->cornerRadius = static_cast<float>(val);
                    } else if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim) {
                        if (m_state->activeElement->tool == MarkupTool::Spotlight) {
                            m_state->activeElement->spotlightDimAlpha = m_state->currentSpotlightDimAlpha;
                        }
                    } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize) {
                        m_state->activeElement->fontSize = static_cast<float>(val);
                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                    } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth) {
                        m_state->activeElement->outlineThickness = static_cast<float>(std::clamp(val, 0, 12));
                        m_state->activeElement->hasCustomOutline = true;
                        m_state->activeElement->textOutline = (val > 0);
                        if (m_state->activeElement->tool == MarkupTool::Text) {
                            m_state->activeElement->textRenderSize = cv::Size(0, 0);
                        }
                    } else if (m_state->sliderPopup.type != SliderPopupType::SelectionCornerRadius) {
                        m_state->activeElement->thickness = static_cast<float>(val);
                        if (m_state->activeElement->tool == MarkupTool::Mosaic) {
                            m_state->activeElement->mosaicBlockSize = (std::max)(4, val * 2);
                        } else if (m_state->activeElement->tool == MarkupTool::Blur) {
                            m_state->activeElement->mosaicBlockSize = (std::max)(5, val * 3);
                        } else if (m_state->activeElement->tool == MarkupTool::Inpaint) {
                            m_state->activeElement->inpaintRadius = std::clamp(val, 2, 16);
                        } else if (m_state->activeElement->tool == MarkupTool::Watermark) {
                            const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                            m_state->activeElement->fontSize = std::clamp(12.0f + static_cast<float>(val) * 1.5f, 12.0f, 64.0f) * curDpi;
                        }
                    }
                    m_renderer->markMarkupDirty();
                }
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                return 0;
            }

            if (m_state->isSelectingText) {
                if ((wParam & MK_LBUTTON) == 0 || !m_state->activeElement ||
                    m_state->activeElement->tool != MarkupTool::Text || !m_state->activeElement->isEditing) {
                    m_state->isSelectingText = false;
                } else {
                    const cv::Point localPt = toMarkupPoint(m_state->currentCursor);
                    const float relX = static_cast<float>(localPt.x - m_state->activeElement->startPt.x);
                    const float relY = static_cast<float>(localPt.y - m_state->activeElement->startPt.y);
                    const std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                    const auto layout = CaptureTextEditor::buildLayout(
                        wstr, static_cast<int>(m_state->activeElement->fontSize));
                    const size_t newCaret = CaptureTextEditor::hitTestCaretIndex(layout, wstr, relX, relY);
                    if (newCaret != m_state->activeElement->textCaret) {
                        m_state->activeElement->textCaret = newCaret;
                        m_state->activeElement->textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                    }
                    m_state->lastMousePos = m_state->currentCursor;
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
            }

            if (m_state->isAdjustingCornerRadius) {
                // 以选区中心为基准：往内拉增加圆角，往外推减小圆角（兼容长图缩放矩阵）
                const float zoom = m_state->isLongImageMode() ? (std::max)(0.05f, m_state->longCanvas.zoom) : 1.0f;
                auto r = m_state->docRectToScreen(currentSelectionRect());
                float cfgMaxRadius = m_state->beautyShell.enabled ? 32.0f : static_cast<float>(tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0));
                const float currentDpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                float newScreenR = CornerRadiusHelper::calculateDraggedRadius(
                    r.left, r.top, r.right, r.bottom,
                    static_cast<float>(m_state->cornerDragStartPos.x),
                    static_cast<float>(m_state->cornerDragStartPos.y),
                    m_state->cornerDragStartRadius * zoom,
                    static_cast<float>(m_state->currentCursor.x),
                    static_cast<float>(m_state->currentCursor.y),
                    cfgMaxRadius * zoom,
                    m_state->cornerDragIndex,
                    currentDpiScale);
                float newR = std::clamp(newScreenR / zoom, 0.0f, cfgMaxRadius);

                const bool isZh = tools3000::core::isUiLanguageChinese();
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
                if (m_state->isLongImageMode()) {
                    const auto curDoc = m_state->screenToDoc(
                        static_cast<float>(m_state->currentCursor.x),
                        static_cast<float>(m_state->currentCursor.y));
                    const int dx = static_cast<int>(std::trunc(curDoc.x - m_state->longCanvas.lastSelAdjustDocPt.x));
                    const int dy = static_cast<int>(std::trunc(curDoc.y - m_state->longCanvas.lastSelAdjustDocPt.y));
                    if (dx != 0 || dy != 0) {
                        adjustSelection(m_state->selAdjustHandle, dx, dy);
                        m_state->longCanvas.lastSelAdjustDocPt.x += static_cast<float>(dx);
                        m_state->longCanvas.lastSelAdjustDocPt.y += static_cast<float>(dy);
                    }
                    // 当推拉顶/底裁切横梁靠近舞台视口上下边缘时，启动 60fps 平滑自动滚屏推进
                    const float curScale = std::clamp(m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
                    const float edgeZone = 44.0f * curScale;
                    const auto& stg = m_state->longCanvas.stageRect;
                    if (m_state->currentCursor.y < stg.top + edgeZone ||
                        m_state->currentCursor.y > stg.bottom - edgeZone) {
                        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    }
                } else {
                    int dx = m_state->currentCursor.x - m_state->selAdjustLast.x;
                    int dy = m_state->currentCursor.y - m_state->selAdjustLast.y;
                    adjustSelection(m_state->selAdjustHandle, dx, dy);
                }
                m_state->selAdjustLast = m_state->currentCursor;
                m_renderer->invalidate();
            } else if (m_state->isManipulating && m_state->activeElement) {
                if (m_state->dragHandle == HitArea::CornerRadius && m_state->activeElement->tool == MarkupTool::Rectangle) {
                    const float zoom = m_state->isLongImageMode() ? (std::max)(0.05f, m_state->longCanvas.zoom) : 1.0f;
                    auto r = currentSelectionRect();
                    int x1 = (std::min)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                    int y1 = (std::min)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                    int x2 = (std::max)(m_state->activeElement->startPt.x, m_state->activeElement->endPt.x);
                    int y2 = (std::max)(m_state->activeElement->startPt.y, m_state->activeElement->endPt.y);
                    const auto scrBox = m_state->docRectToScreen(D2D1::RectF(
                        r.left + static_cast<float>(x1),
                        r.top + static_cast<float>(y1),
                        r.left + static_cast<float>(x2),
                        r.top + static_cast<float>(y2)));
                    const float currentDpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                    float newScreenR = CornerRadiusHelper::calculateDraggedRadius(
                        scrBox.left, scrBox.top, scrBox.right, scrBox.bottom,
                        static_cast<float>(m_state->cornerDragStartPos.x),
                        static_cast<float>(m_state->cornerDragStartPos.y),
                        m_state->cornerDragStartRadius * zoom,
                        static_cast<float>(m_state->currentCursor.x),
                        static_cast<float>(m_state->currentCursor.y),
                        0.0f,
                        m_state->cornerDragIndex,
                        currentDpiScale);
                    float newR = (std::max)(0.0f, newScreenR / zoom);
                    m_state->activeElement->cornerRadius = std::round(newR);
                    m_state->currentElementCornerRadius = m_state->activeElement->cornerRadius;
                    m_state->toolbarLayoutValid = false;
                    const bool isZh = tools3000::core::isUiLanguageChinese();
                    m_state->loupeToastMessage = CornerRadiusHelper::getToastMessage(
                        CornerRadiusTarget::RectangleMarkup, m_state->activeElement->cornerRadius, isZh);
                    m_state->loupeToastUntil = GetTickCount() + 1200;
                    m_state->lastMousePos = m_state->currentCursor;
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                } else {
                    int dx = m_state->currentCursor.x - m_state->lastMousePos.x;
                    int dy = m_state->currentCursor.y - m_state->lastMousePos.y;
                    if (m_state->isLongImageMode()) {
                        const auto curDoc = m_state->screenToDoc(
                            static_cast<float>(m_state->currentCursor.x),
                            static_cast<float>(m_state->currentCursor.y));
                        dx = static_cast<int>(std::trunc(curDoc.x - m_state->longCanvas.lastManipulateDocPt.x));
                        dy = static_cast<int>(std::trunc(curDoc.y - m_state->longCanvas.lastManipulateDocPt.y));
                        m_state->longCanvas.lastManipulateDocPt.x += static_cast<float>(dx);
                        m_state->longCanvas.lastManipulateDocPt.y += static_cast<float>(dy);
                    }
                    LOG_TRACE("CaptureInput: manipulating element dragHandle={}, dx={}, dy={}",
                              static_cast<int>(m_state->dragHandle), dx, dy);
                    if (m_state->dragHandle == HitArea::None) {
                        m_state->activeElement->moveBy(dx, dy);
                    } else {
                        m_state->activeElement->resize(dx, dy, m_state->dragHandle);
                        if (m_state->activeElement->tool == MarkupTool::Number && m_state->dragHandle == HitArea::RT) {
                            m_state->currentStrokeWidth = static_cast<int>(std::round(m_state->activeElement->thickness));
                            m_state->toolbarLayoutValid = false;
                        } else if (m_state->activeElement->tool == MarkupTool::Magnifier &&
                                   (m_state->dragHandle == HitArea::LensOffset || m_state->dragHandle == HitArea::FocusSource)) {
                            const auto sel = currentSelectionRect();
                            const int selW = std::max(1, static_cast<int>(std::round(sel.right - sel.left)));
                            const int selH = std::max(1, static_cast<int>(std::round(sel.bottom - sel.top)));
                            if (m_state->dragHandle == HitArea::LensOffset) {
                                const int rx = m_state->activeElement->effectiveMagnifierRx();
                                const int ry = m_state->activeElement->effectiveMagnifierRy();
                                const int loX = std::min(rx, selW / 2);
                                const int hiX = std::max(loX, selW - rx);
                                const int loY = std::min(ry, selH / 2);
                                const int hiY = std::max(loY, selH - ry);
                                m_state->activeElement->endPt.x = std::clamp(m_state->activeElement->endPt.x, loX, hiX);
                                m_state->activeElement->endPt.y = std::clamp(m_state->activeElement->endPt.y, loY, hiY);
                            } else {
                                m_state->activeElement->startPt.x = std::clamp(m_state->activeElement->startPt.x, 1, std::max(1, selW - 1));
                                m_state->activeElement->startPt.y = std::clamp(m_state->activeElement->startPt.y, 1, std::max(1, selH - 1));
                            }
                            m_state->toolbarLayoutValid = false;
                        }
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
                if ((int)m_state->state.load() == (int)OverlayState::Selecting && !m_state->dragging) {
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

            if (m_state->isLongImageMode() &&
                m_state->longCanvas.dragMode != CaptureState::LongCanvasDragMode::None) {
                const auto prevDrag = m_state->longCanvas.dragMode;
                m_state->longCanvas.dragMode = CaptureState::LongCanvasDragMode::None;
                if (prevDrag == CaptureState::LongCanvasDragMode::MinimapCropTop ||
                    prevDrag == CaptureState::LongCanvasDragMode::MinimapCropBottom) {
                    prepareMarkupBase();
                }
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isDraggingToolbar) {
                m_state->isDraggingToolbar = false;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

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
                prepareMarkupBase();
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                return 0;
            }

            if (m_state->isSelectingText) {
                m_state->isSelectingText = false;
                m_renderer->markMarkupDirty();
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
                    const POINT releasePt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    const double moveDist = std::hypot(
                        static_cast<double>(releasePt.x - m_state->textManipulateStart.x),
                        static_cast<double>(releasePt.y - m_state->textManipulateStart.y));
                    if (!m_state->activeElement->isEditing && m_state->dragHandle == HitArea::None && moveDist <= 4.0) {
                        // 原地点击非编辑态文本框：进入编辑态并将光标精准定位到点击字缝
                        m_state->activeElement->isEditing = true;
                        const cv::Point local = toMarkupPoint(releasePt);
                        const std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                        const auto layout = CaptureTextEditor::buildLayout(
                            wstr, static_cast<int>(m_state->activeElement->fontSize));
                        const float relX = static_cast<float>(local.x - m_state->activeElement->startPt.x);
                        const float relY = static_cast<float>(local.y - m_state->activeElement->startPt.y);
                        const size_t hitIdx = CaptureTextEditor::hitTestCaretIndex(layout, wstr, relX, relY);
                        m_state->activeElement->textCaret = hitIdx;
                        m_state->activeElement->textSelAnchor = hitIdx;
                        m_state->activeElement->textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                        m_renderer->markMarkupDirty();
                    }
                }
                if (m_state->hasManipulateSnapshot) {
                    m_state->hasManipulateSnapshot = false;
                    if (!MarkupEngine::elementsEquivalent(
                            m_state->manipulateStartSnapshot.elements,
                            m_state->markup.exportElements())) {
                        m_state->markup.commitUndoSnapshot(std::move(m_state->manipulateStartSnapshot));
                        m_renderer->markMarkupDirty();
                    }
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

            auto showSelectedShortcutHints = [&]() {
                if (!m_state->options.showShortcutHints || m_state->state.load() == OverlayState::Idle) return;
                const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                RECT sel = {
                    std::min(m_state->dragStart.x, m_state->dragEnd.x) + vx,
                    std::min(m_state->dragStart.y, m_state->dragEnd.y) + vy,
                    std::max(m_state->dragStart.x, m_state->dragEnd.x) + vx,
                    std::max(m_state->dragStart.y, m_state->dragEnd.y) + vy
                };
                MONITORINFO mi{sizeof(mi)};
                const HMONITOR hMon = MonitorFromRect(&sel, MONITOR_DEFAULTTONEAREST);
                RECT workArea{
                    vx, vy,
                    vx + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                    vy + GetSystemMetrics(SM_CYVIRTUALSCREEN)
                };
                if (hMon && GetMonitorInfoW(hMon, &mi)) {
                    workArea = mi.rcWork;
                }
                const LONG monW = std::max<LONG>(1, workArea.right - workArea.left);
                const LONG monH = std::max<LONG>(1, workArea.bottom - workArea.top);
                const bool isLargeWindowOrScreenSelection =
                    (sel.right - sel.left) >= (monW * 55) / 100 ||
                    (sel.bottom - sel.top) >= (monH * 60) / 100;
                std::vector<RECT> initialAvoid;
                if (!isLargeWindowOrScreenSelection) {
                    initialAvoid.push_back(sel);
                }
                ShortcutHintOverlay::instance().show(
                    m_state->mode == OverlayMode::RecordRegion
                        ? ShortcutHintContext::RecordSelecting
                        : ShortcutHintContext::CaptureSelected,
                    {LONG_MIN, LONG_MIN},
                    initialAvoid);
            };

            if (w > 3 && h > 3) {
                m_state->state = OverlayState::Selected;
                m_state->isMarkupToolActive = false;
                prepareMarkupBase();
                showSelectedShortcutHints();
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
                    m_state->isMarkupToolActive = false;
                    prepareMarkupBase();
                    showSelectedShortcutHints();
                } else {
                    if (m_cancelCb) m_cancelCb();
                }
            }
            updateHoverCursor(m_state->currentCursor);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            if (this && ((int)m_state->state.load() == (int)OverlayState::Selected ||
                         (int)m_state->state.load() == (int)OverlayState::Marking)) {
                m_renderer->markMarkupDirty();
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (m_state->isLongImageMode()) {
                    const auto inPanel = [&](const D2D1_RECT_F& rc) {
                        return rc.right > rc.left && pt.x >= rc.left && pt.x <= rc.right &&
                               pt.y >= rc.top && pt.y <= rc.bottom;
                    };
                    if (inPanel(m_state->longCanvas.topHeaderRect) ||
                        inPanel(m_state->longCanvas.minimapPanelRect)) {
                        return handleMessage(hwnd, WM_LBUTTONDOWN, wParam, lParam);
                    }
                }
                rebuildToolbarButtons(currentSelectionRect());
                if (m_state->toolbarGripRect.right > m_state->toolbarGripRect.left &&
                    pt.x >= m_state->toolbarGripRect.left && pt.x <= m_state->toolbarGripRect.right &&
                    pt.y >= m_state->toolbarGripRect.top && pt.y <= m_state->toolbarGripRect.bottom) {
                    m_state->hasCustomToolbarPos = false;
                    m_state->isDraggingToolbar = false;
                    m_state->toolbarLayoutValid = false;
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    m_state->loupeToastUntil = GetTickCount() + 1400;
                    m_state->loupeToastMessage = zh ? L"已恢复工具栏自动跟随" : L"Toolbar auto-follow restored";
                    m_renderer->invalidate();
                    updateHoverCursor(pt);
                    return 0;
                }
                auto* hitBtn = hitTestToolbar(pt);
                const bool inSel = isPointInSelection(pt);
                const HitArea selBoxHit = hitTestSelectionBox(pt);
                const cv::Point local = inSel ? toMarkupPoint(pt) : cv::Point{0, 0};

                const bool activeElemHit = inSel && m_state->activeElement && m_state->activeElement->isActive &&
                                           (m_state->activeElement->hitTestEx(local) != HitArea::None);
                HitResult hit = (inSel && !activeElemHit) ? m_state->markup.getElementAtEx(local) : HitResult{};

                const DoubleClickIntent intent = resolveDoubleClickIntent(
                    *m_state, pt, inSel, selBoxHit, activeElemHit || (hit.element != nullptr), hitBtn);

                switch (intent) {
                    case DoubleClickIntent::ForwardPointerDown:
                        return handleMessage(hwnd, WM_LBUTTONDOWN, wParam, lParam);

                    case DoubleClickIntent::RepeatToolbarAction:
                        if (hitBtn) executeToolbarCommand(*hitBtn);
                        return 0;

                    case DoubleClickIntent::ConsumeNoOp:
                        return 0;

                    case DoubleClickIntent::ToggleSelectionCornerRadius:
                        if (m_state->beautyShell.enabled) {
                            m_state->beautyShell.cornerRadius = (m_state->beautyShell.cornerRadius > 0.0f) ? 0.0f : 16.0f;
                            tools3000::core::ConfigManager::instance().set<int>(
                                "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
                        } else {
                            m_state->cornerRadius = (m_state->cornerRadius > 0.0f) ? 0.0f : 12.0f;
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
                        }
                        prepareMarkupBase();
                        m_renderer->invalidate();
                        return 0;

                    case DoubleClickIntent::EditOrSelectMarkupElement: {
                        auto enterTextWordSelection = [&](MarkupElement& elem) {
                            elem.isEditing = true;
                            m_state->isSelectingText = false;
                            m_state->isManipulating = false;
                            const std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(elem.text);
                            const auto layout = CaptureTextEditor::buildLayout(wstr, static_cast<int>(elem.fontSize));
                            const float relX = static_cast<float>(local.x - elem.startPt.x);
                            const float relY = static_cast<float>(local.y - elem.startPt.y);
                            const size_t hitIdx = CaptureTextEditor::hitTestCaretIndex(layout, wstr, relX, relY);
                            const auto [curS, curE] = CaptureTextEditor::normalizedRange(wstr, elem.textCaret, elem.textSelAnchor);
                            if (curE > curS && !(curS == 0 && curE == wstr.size())) {
                                // 已选中单个词块时再次双击：平滑升级为全选当前文本框全文
                                CaptureTextEditor::selectAll(wstr, elem.textCaret, elem.textSelAnchor);
                            } else {
                                CaptureTextEditor::selectWordAt(wstr, hitIdx, elem.textCaret, elem.textSelAnchor);
                            }
                            elem.textLastCaretTick = GetTickCount();
                            m_state->textPreferredCaretX = -1.0f;
                            SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                            syncShortcutHintContext();
                        };

                        if (activeElemHit) {
                            if (m_state->activeElement->tool == MarkupTool::Text) {
                                enterTextWordSelection(*m_state->activeElement);
                                m_state->currentTool = MarkupTool::Text;
                                m_state->currentColor = m_state->activeElement->color;
                                m_state->currentTextOutline = m_state->activeElement->textOutline;
                                m_state->currentTextOutlineColor = m_state->activeElement->textOutlineColor;
                                m_state->toolbarLayoutValid = false;
                                m_renderer->markMarkupDirty();
                                m_renderer->invalidate();
                            }
                            return 0;
                        }
                        if (hit.element) {
                            if (m_state->activeElement && m_state->activeElement != hit.element) {
                                m_state->activeElement->isActive = false;
                                m_state->activeElement->isEditing = false;
                            }
                            m_state->activeElement = hit.element;
                            m_state->activeElement->isActive = true;
                            m_state->isMarkupToolActive = true;
                            m_state->currentTool = hit.element->tool;
                            m_state->currentStrokeWidth = static_cast<int>(std::round(hit.element->thickness));
                            m_state->currentLineStyle = hit.element->lineStyle;
                            m_state->currentArrowStyle = hit.element->arrowStyle;
                            m_state->currentSmoothCurve = hit.element->smoothCurve;
                            if (hit.element->tool == MarkupTool::Line) {
                                m_state->currentLineArrowEnabled = hit.element->hasCustomCaps;
                            }
                            if (hit.element->hasCustomCaps) {
                                m_state->currentStartCap = hit.element->startCap;
                                m_state->currentEndCap = hit.element->endCap;
                            } else if (hit.element->tool == MarkupTool::Arrow) {
                                switch (hit.element->arrowStyle) {
                                    case ArrowStyle::Thin:
                                        m_state->currentStartCap = LineCapStyle::None;
                                        m_state->currentEndCap = LineCapStyle::OpenArrow;
                                        break;
                                    case ArrowStyle::Tech:
                                        m_state->currentStartCap = LineCapStyle::None;
                                        m_state->currentEndCap = LineCapStyle::StealthArrow;
                                        break;
                                    case ArrowStyle::DoubleEnded:
                                        m_state->currentStartCap = LineCapStyle::SolidTriangle;
                                        m_state->currentEndCap = LineCapStyle::SolidTriangle;
                                        break;
                                    case ArrowStyle::Standard:
                                    default:
                                        m_state->currentStartCap = LineCapStyle::None;
                                        m_state->currentEndCap = LineCapStyle::SolidTriangle;
                                        break;
                                }
                            }
                            m_state->currentElementCornerRadius = hit.element->cornerRadius;
                            if (hit.element->tool == MarkupTool::Magnifier) {
                                m_state->currentMagnifierColor = hit.element->color;
                                m_state->magnifierShowBadge = hit.element->fill;
                                m_state->currentMagnifierShape = hit.element->numberShape;
                                m_state->dynamicMagnifierScale = hit.element->magnifierScale;
                                const bool unsetEnd = !hit.element->hasLeaderArrow &&
                                                      hit.element->endPt.x == 0 && hit.element->endPt.y == 0 &&
                                                      (hit.element->startPt.x != 0 || hit.element->startPt.y != 0);
                                const cv::Point lc = unsetEnd ? hit.element->startPt : hit.element->endPt;
                                m_state->magnifierCalloutMode =
                                    std::hypot(static_cast<double>(lc.x - hit.element->startPt.x),
                                               static_cast<double>(lc.y - hit.element->startPt.y)) > 8.0;
                            } else if (hit.element->tool == MarkupTool::Spotlight) {
                                m_state->currentColor = hit.element->color;
                                m_state->currentSpotlightEllipse = hit.element->fill;
                                m_state->currentSpotlightDimAlpha = hit.element->spotlightDimAlpha;
                                m_state->currentSpotlightBorder = hit.element->spotlightShowBorder;
                            } else {
                                m_state->currentColor = hit.element->color;
                                m_state->currentFillMode = hit.element->fill;
                                m_state->currentNumberShape = hit.element->numberShape;
                                if (hit.element->tool == MarkupTool::Number) {
                                    m_state->currentNumberFormat = hit.element->numberFormat;
                                }
                            }
                            if (hit.element->tool == MarkupTool::Blur) {
                                m_state->currentMosaicType = 1;
                            } else if (hit.element->tool == MarkupTool::Mosaic) {
                                m_state->currentMosaicType = 0;
                            }
                            if (hit.element->tool == MarkupTool::Text) {
                                m_state->currentTextOutline = hit.element->textOutline;
                                m_state->currentTextOutlineColor = hit.element->textOutlineColor;
                                enterTextWordSelection(*m_state->activeElement);
                            } else {
                                if (hit.element->hasCustomOutline) {
                                    m_state->currentOutlineWidth = static_cast<int>(std::round(hit.element->outlineThickness));
                                    m_state->currentTextOutlineColor = hit.element->textOutlineColor;
                                }
                                m_state->activeElement->isEditing = false;
                            }
                            m_state->toolbarLayoutValid = false;
                            m_renderer->markMarkupDirty();
                            m_renderer->invalidate();
                        }
                        return 0;
                    }

                    case DoubleClickIntent::BeginAnnotationAtPoint:
                        beginMarkup(pt);
                        m_renderer->invalidate();
                        updateHoverCursor(pt);
                        return 0;

                    case DoubleClickIntent::ConfirmSelectionAndCopy:
                        if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Default});
                        return 0;
                }
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            if (!this) break;
            CaptureShortcutPolicy::cancelShiftTapCandidate(m_state->shiftTapCandidate);

            // 0. 已选区截图态下右键点击主工具栏第 1 个形状组按钮：弹出「几何图形 / 聚光灯」模式切换菜单
            if (m_state->mode == OverlayMode::Screenshot &&
                ((int)m_state->state.load() == (int)OverlayState::Selected ||
                 (int)m_state->state.load() == (int)OverlayState::Marking)) {
                POINT rpt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                rebuildToolbarButtons(currentSelectionRect());
                if (!m_state->toolbarButtons.empty()) {
                    const auto& shapeBtn = m_state->toolbarButtons[0];
                    if (rpt.x >= shapeBtn.rect.left && rpt.x <= shapeBtn.rect.right &&
                        rpt.y >= shapeBtn.rect.top && rpt.y <= shapeBtn.rect.bottom) {
                        openDropdownMenu(DropdownType::ShapeMode, shapeBtn.rect);
                        m_renderer->invalidate();
                        return 0;
                    }
                }
            }
            
            // 0.5 若有任何悬浮弹出菜单处于展开态（下拉菜单 / 滑块 / 子菜单 / 尺寸菜单），右键优先收起菜单，绝不误退标注或清空选区
            if (hasAnyCapturePopupOpen(*m_state)) {
                closeAllCapturePopups(*m_state, true);
                m_renderer->invalidate();
                return 0;
            }

            // 1. 正在拖拽选区中：右键取消本次拖拽
            if (m_state->dragging) {
                m_state->dragging = false;
                m_state->state = OverlayState::Selecting;
                m_renderer->invalidate();
                syncShortcutHintContext();
                return 0;
            }

            // 2. 正在编辑文字：右键退出文字编辑
            if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text && m_state->activeElement->isEditing) {
                m_state->activeElement->isEditing = false;
                m_state->isSelectingText = false;
                if (m_state->activeElement->text.empty()) {
                    m_state->markup.removeElement(m_state->activeElement->id);
                    m_state->activeElement = nullptr;
                }
                m_renderer->markMarkupDirty();
                m_renderer->invalidate();
                syncShortcutHintContext();
                return 0;
            }

            // 3. 有激活选中的标注元素：取消选中
            if (m_state->activeElement) {
                m_state->activeElement->isActive = false;
                m_state->activeElement = nullptr;
                m_renderer->markMarkupDirty();
                m_renderer->invalidate();
                syncShortcutHintContext();
                return 0;
            }

            // 3.5 若激活了标注工具：右键取消当前工具，返回选区调整模式
            if (m_state->isMarkupToolActive) {
                m_state->isMarkupToolActive = false;
                m_state->openSubmenu = SubmenuType::None;
                m_state->sliderPopup.type = SliderPopupType::None;
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
                updateHoverCursor(m_state->currentCursor);
                syncShortcutHintContext();
                return 0;
            }

            // 4. 处于选区态（Selected/Marking）：长图模式下右键保持在浏览裁切态（绝不清空长图或标注），普通截图下重置选区
            if ((int)m_state->state.load() == (int)OverlayState::Selected || (int)m_state->state.load() == (int)OverlayState::Marking) {
                if (m_state->isLongImageMode()) {
                    return 0;
                }
                m_state->state = OverlayState::Selecting;
                m_state->dragStart = {0, 0};
                m_state->dragEnd = {0, 0};
                m_state->isHierarchyAnimating = false;
                m_state->animDetectedWindowRect = {};
                m_state->targetDetectedWindowRect = {};
                m_state->primaryToolbarRect = {};
                m_state->secondaryToolbarRect = {};
                m_state->selectionSideRect = {};
                m_state->toolbarLayoutValid = false;
                m_state->markup.clearAll();
                m_renderer->markMarkupDirty();
                m_renderer->invalidate();
                syncShortcutHintContext();
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
                    const std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                    const auto layout = CaptureTextEditor::buildLayout(
                        wstr, static_cast<int>(m_state->activeElement->fontSize));
                    const auto vis = CaptureTextEditor::computeCaretVisual(
                        layout, wstr, m_state->activeElement->textCaret);
                    COMPOSITIONFORM cf{};
                    cf.dwStyle = CFS_POINT;
                    auto selRect = currentSelectionRect();
                    const auto scrCaret = m_state->docToScreen(
                        selRect.left + static_cast<float>(m_state->activeElement->startPt.x) + vis.x,
                        selRect.top + static_cast<float>(m_state->activeElement->startPt.y) + vis.y + vis.height + 2.0f);
                    cf.ptCurrentPos.x = static_cast<LONG>(std::round(scrCaret.x));
                    cf.ptCurrentPos.y = static_cast<LONG>(std::round(scrCaret.y));
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

                            pushCoalescedElementUndo(*m_state, 650);
                            std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                            CaptureTextEditor::insertText(
                                wstr,
                                m_state->activeElement->textCaret,
                                m_state->activeElement->textSelAnchor,
                                resultStr);
                            m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                            m_state->activeElement->textLastCaretTick = GetTickCount();
                            m_state->textPreferredCaretX = -1.0f;
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
                if (ch == 0x08 || ch == 0x7F) { // Backspace / Ctrl+Backspace
                    std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                    if (!wstr.empty() || CaptureTextEditor::hasSelection(m_state->activeElement->textCaret, m_state->activeElement->textSelAnchor)) {
                        pushCoalescedElementUndo(*m_state, 650);
                    }
                    if (CaptureTextEditor::backspace(
                            wstr,
                            m_state->activeElement->textCaret,
                            m_state->activeElement->textSelAnchor,
                            ch == 0x7F)) {
                        m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                    }
                    m_state->activeElement->textLastCaretTick = GetTickCount();
                    m_state->textPreferredCaretX = -1.0f;
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                } else if (ch == 0x0D) { // Enter: 在当前光标处插入换行符（或替换选区为换行）
                    pushCoalescedElementUndo(*m_state, 650);
                    std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                    CaptureTextEditor::insertText(
                        wstr,
                        m_state->activeElement->textCaret,
                        m_state->activeElement->textSelAnchor,
                        L"\n");
                    m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                    m_state->activeElement->textLastCaretTick = GetTickCount();
                    m_state->textPreferredCaretX = -1.0f;
                    m_state->activeElement->textRenderSize = cv::Size(0, 0);
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                } else if (ch == 0x0A) { // Ctrl+Enter: 结束文本编辑
                    m_state->activeElement->isEditing = false;
                    m_state->isSelectingText = false;
                    m_renderer->markMarkupDirty();
                    m_renderer->invalidate();
                    syncShortcutHintContext();
                } else if (ch >= 0x20 || ch == 0x09) {
                    pushCoalescedElementUndo(*m_state, 650);
                    std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                    CaptureTextEditor::insertText(
                        wstr,
                        m_state->activeElement->textCaret,
                        m_state->activeElement->textSelAnchor,
                        std::wstring(1, ch));
                    m_state->activeElement->text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                    m_state->activeElement->textLastCaretTick = GetTickCount();
                    m_state->textPreferredCaretX = -1.0f;
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
            CaptureShortcutPolicy::cancelShiftTapCandidate(m_state->shiftTapCandidate);
            short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);

            // 1. 选区前/未拖拽状态：滚轮切换探测窗口层级 (子控件 <-> 父容器 <-> 顶级窗口 <-> 全屏)
            if (((int)m_state->state.load() == (int)OverlayState::Idle ||
                 (int)m_state->state.load() == (int)OverlayState::Selecting) && !m_state->dragging) {
                if (!m_state->detectedWindowHierarchy.empty()) {
                    if (zDelta != 0) {
                        stepDetectedWindowHierarchy(hwnd, zDelta > 0 ? 1 : -1);
                    }
                    return 0;
                }
            }

            POINT wheelPt = m_state->currentCursor;
            if (lParam != 0) {
                POINT screenWheelPt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (hwnd && IsWindow(hwnd)) {
                    ScreenToClient(hwnd, &screenWheelPt);
                    wheelPt = screenWheelPt;
                } else {
                    wheelPt = screenWheelPt;
                }
            }

            const ToolbarButton* hoveredColorBtn = hitTestToolbar(wheelPt);
            if (zDelta != 0 && hoveredColorBtn != nullptr &&
                (hoveredColorBtn->command == ToolbarCommand::ChooseCustomColor ||
                 hoveredColorBtn->command == ToolbarCommand::SelectColor)) {
                const int delta = (zDelta > 0) ? 1 : -1;
                const bool ctrlDown = ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0) ||
                                      ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
                pushCoalescedElementUndo(*m_state, 450);
                adjustPaletteColorByWheel(*m_state, hoveredColorBtn->tool, delta, ctrlDown);
                m_renderer->markMarkupDirty();
                m_renderer->invalidate();
                return 0;
            }

            MarkupElement* hoveredMag = isPointInSelection(wheelPt)
                ? findHoveredMagnifierElement(*m_state, toMarkupPoint(wheelPt))
                : nullptr;
            const bool ctrlOrShiftWheel = ((GetKeyState(VK_CONTROL) & 0x8000) != 0) ||
                                          ((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            const float wheelDpi = (m_state->dpiScale > 0.0f) ? m_state->dpiScale : 1.0f;

            if (hoveredMag != nullptr ||
                (!m_state->isMarkupToolActive && m_state->activeElement &&
                 m_state->activeElement->tool == MarkupTool::Magnifier)) {
                MarkupElement* targetMag = hoveredMag ? hoveredMag : m_state->activeElement;
                if (m_state->activeElement && m_state->activeElement != targetMag) {
                    m_state->activeElement->isActive = false;
                    m_state->activeElement->isEditing = false;
                }
                m_state->activeElement = targetMag;
                targetMag->isActive = true;
                pushCoalescedElementUndo(*m_state, 450);
                if (ctrlOrShiftWheel) {
                    const int stepPx = (std::max)(4, static_cast<int>(std::lround(6.0f * wheelDpi)));
                    const int minR = static_cast<int>(std::lround(24.0f * wheelDpi));
                    const int oldRx = targetMag->effectiveMagnifierRx();
                    const int oldRy = targetMag->effectiveMagnifierRy();
                    const int deltaR = (zDelta > 0) ? stepPx : -stepPx;
                    targetMag->magnifierRadius = std::clamp(oldRx + deltaR, minR, 420);
                    targetMag->magnifierRadiusY = std::clamp(oldRy + deltaR, minR, 420);
                } else {
                    targetMag->magnifierScale = std::clamp(
                        targetMag->magnifierScale + ((zDelta > 0) ? 0.25f : -0.25f), 1.5f, 6.0f);
                    m_state->dynamicMagnifierScale = targetMag->magnifierScale;
                }
                m_renderer->markMarkupDirty();
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
            } else if (m_state->isMarkupToolActive && m_state->currentTool == MarkupTool::Magnifier && !m_state->isMarking && !m_state->isManipulating) {
                if (ctrlOrShiftWheel) {
                    m_state->dynamicMagnifierRadius = std::clamp(
                        m_state->dynamicMagnifierRadius + ((zDelta > 0) ? 6 : -6), 28, 240);
                } else {
                    m_state->dynamicMagnifierScale = std::clamp(
                        m_state->dynamicMagnifierScale + ((zDelta > 0) ? 0.25f : -0.25f), 1.5f, 6.0f);
                }
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
            } else if (m_state->isLongImageMode() &&
                       m_state->sliderPopup.type == SliderPopupType::None &&
                       hitTestToolbar(wheelPt) == nullptr) {
                // 长图工作台滚轮契约：普通滚轮丝滑上下翻阅长图（绝不误改线宽/字号），Ctrl+滚轮以光标为锚点无级缩放
                const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const float wx = static_cast<float>(wheelPt.x);
                const float wy = static_cast<float>(wheelPt.y);
                if (ctrlDown) {
                    const auto anchorDoc = m_state->screenToDoc(wx, wy);
                    const float factor = (zDelta > 0) ? 1.15f : (1.0f / 1.15f);
                    const float newZoom = std::clamp(m_state->longCanvas.zoom * factor, 0.15f, 4.0f);
                    m_state->longCanvas.zoom = newZoom;
                    const auto& stg = m_state->longCanvas.stageRect;
                    const float pad = 20.0f * wheelDpi;
                    m_state->longCanvas.scrollX = stg.left + pad + anchorDoc.x * newZoom - wx;
                    m_state->longCanvas.scrollY = stg.top + pad + anchorDoc.y * newZoom - wy;
                    m_state->clampLongCanvasScroll();
                } else if (shiftDown) {
                    m_state->longCanvas.scrollX -= (static_cast<float>(zDelta) / 120.0f) * 148.0f * wheelDpi;
                    m_state->clampLongCanvasScroll();
                } else {
                    m_state->longCanvas.scrollY -= (static_cast<float>(zDelta) / 120.0f) * 148.0f * wheelDpi;
                    m_state->clampLongCanvasScroll();
                }
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
            } else if (m_state->sliderPopup.type != SliderPopupType::None ||
                       hitTestToolbar(wheelPt) != nullptr ||
                       ((int)m_state->state.load() == (int)OverlayState::Selected &&
                        (m_state->currentTool == MarkupTool::Rectangle || m_state->currentTool == MarkupTool::Ellipse ||
                         m_state->currentTool == MarkupTool::Line || m_state->currentTool == MarkupTool::Pen ||
                         m_state->currentTool == MarkupTool::Arrow || m_state->currentTool == MarkupTool::Text ||
                         m_state->currentTool == MarkupTool::Number || m_state->currentTool == MarkupTool::Mosaic ||
                         m_state->currentTool == MarkupTool::Blur || m_state->currentTool == MarkupTool::Inpaint ||
                         m_state->currentTool == MarkupTool::Spotlight || m_state->currentTool == MarkupTool::Watermark))) {
                int delta = (zDelta > 0) ? 1 : -1;
                const ToolbarButton* wheelBtn = hitTestToolbar(wheelPt);
                const ToolbarCommand wheelCmd = wheelBtn ? wheelBtn->command : ToolbarCommand::SelectTool;
                if (wheelCmd == ToolbarCommand::CycleBeautyRadius) {
                    float newRad = std::clamp(m_state->beautyShell.cornerRadius + static_cast<float>(delta * 2), 0.0f, 32.0f);
                    m_state->beautyShell.cornerRadius = newRad;
                    tools3000::core::ConfigManager::instance().set<int>(
                        "/capture/beautyShellRadius", static_cast<int>(std::round(newRad)));
                    prepareMarkupBase();
                } else if (m_state->sliderPopup.type == SliderPopupType::SelectionCornerRadius ||
                           wheelCmd == ToolbarCommand::SideToggleCornerRadius ||
                           wheelCmd == ToolbarCommand::ToggleCornerRadius) {
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
                } else if (m_state->sliderPopup.type == SliderPopupType::CornerRadius ||
                           wheelCmd == ToolbarCommand::CycleElementCornerRadius ||
                           (GetKeyState(VK_SHIFT) & 0x8000)) {
                    float newRad = std::clamp(m_state->currentElementCornerRadius + static_cast<float>(delta * 2), 0.0f, 40.0f);
                    m_state->currentElementCornerRadius = newRad;
                    if (m_state->activeElement) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->cornerRadius = newRad;
                    }
                } else if (m_state->sliderPopup.type == SliderPopupType::SpotlightDim ||
                           wheelCmd == ToolbarCommand::CycleSpotlightDim ||
                           (wheelCmd != ToolbarCommand::CycleStrokeWidth && m_state->currentTool == MarkupTool::Spotlight)) {
                    const int curPct = static_cast<int>(std::round(m_state->currentSpotlightDimAlpha * 100.0f));
                    const int newPct = std::clamp(curPct + delta * 3, 15, 85);
                    m_state->currentSpotlightDimAlpha = static_cast<float>(newPct) / 100.0f;
                    tools3000::core::ConfigManager::instance().set<int>("/capture/markup/spotlightDim", newPct);
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Spotlight) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->spotlightDimAlpha = m_state->currentSpotlightDimAlpha;
                    }
                } else if (m_state->sliderPopup.type == SliderPopupType::OutlineWidth ||
                           wheelCmd == ToolbarCommand::ToggleTextOutline) {
                    const int curOutW = (m_state->activeElement && m_state->activeElement->hasCustomOutline)
                        ? static_cast<int>(std::round(m_state->activeElement->outlineThickness))
                        : m_state->currentOutlineWidth;
                    const int newOutW = std::clamp(curOutW + delta, 0, 12);
                    m_state->currentOutlineWidth = newOutW;
                    if (m_state->currentTool == MarkupTool::Text) {
                        m_state->currentTextOutline = (newOutW > 0);
                    }
                    if (m_state->activeElement) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->outlineThickness = static_cast<float>(newOutW);
                        m_state->activeElement->hasCustomOutline = true;
                        m_state->activeElement->textOutline = (newOutW > 0);
                        if (m_state->activeElement->tool == MarkupTool::Text) {
                            m_state->activeElement->textRenderSize = cv::Size(0, 0);
                        }
                    }
                } else if (m_state->sliderPopup.type == SliderPopupType::TextFontSize || m_state->currentTool == MarkupTool::Text) {
                    const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                        pushCoalescedElementUndo(*m_state, 450);
                        const float newPhysSize = std::clamp(
                            m_state->activeElement->fontSize + static_cast<float>(delta * 2),
                            10.0f, 144.0f * curDpi);
                        m_state->activeElement->fontSize = newPhysSize;
                        m_state->activeElement->textRenderSize = cv::Size(0, 0);
                        tools3000::core::ConfigManager::instance().set<double>(
                            "/capture/markup/text/fontSize",
                            std::clamp(static_cast<double>(newPhysSize / curDpi), 10.0, 144.0));
                    } else {
                        const float curLogical = static_cast<float>(
                            tools3000::core::ConfigManager::instance().get<double>("/capture/markup/text/fontSize", 18.0));
                        const float newLogical = std::clamp(curLogical + static_cast<float>(delta * 2), 10.0f, 144.0f);
                        tools3000::core::ConfigManager::instance().set<double>(
                            "/capture/markup/text/fontSize", static_cast<double>(newLogical));
                    }
                } else {
                    const int maxW = (m_state->sliderPopup.type == SliderPopupType::MosaicBlockSize ||
                                      m_state->currentTool == MarkupTool::Mosaic ||
                                      m_state->currentTool == MarkupTool::Blur) ? 32 : 28;
                    int newW = std::clamp(m_state->currentStrokeWidth + delta, 1, maxW);
                    m_state->currentStrokeWidth = newW;
                    if (m_state->activeElement) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->thickness = static_cast<float>(newW);
                        if (m_state->activeElement->tool == MarkupTool::Mosaic) {
                            m_state->activeElement->mosaicBlockSize = (std::max)(4, newW * 2);
                        } else if (m_state->activeElement->tool == MarkupTool::Blur) {
                            m_state->activeElement->mosaicBlockSize = (std::max)(5, newW * 3);
                        } else if (m_state->activeElement->tool == MarkupTool::Inpaint) {
                            m_state->activeElement->inpaintRadius = std::clamp(newW, 2, 16);
                        } else if (m_state->activeElement->tool == MarkupTool::Watermark) {
                            const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                            m_state->activeElement->fontSize = std::clamp(12.0f + static_cast<float>(newW) * 1.5f, 12.0f, 64.0f) * curDpi;
                        }
                    }
                }
                m_renderer->markMarkupDirty();
                m_state->toolbarLayoutValid = false;
                m_renderer->invalidate();
            }
            return 0;
        }


        case WM_SYSKEYUP:
        case WM_KEYUP: {
            if (!this) break;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (msg == WM_SYSKEYUP) || ((GetKeyState(VK_MENU) & 0x8000) != 0);
            const auto keyCtx = CaptureShortcutPolicy::snapshotFromState(*m_state);
            const auto decision = CaptureShortcutPolicy::resolveKeyUp(
                m_state->shiftTapCandidate, keyCtx, wParam, ctrl, alt);
            if (decision.action == CaptureShortcutAction::CycleColorFormat) {
                const int nextIdx = (static_cast<int>(m_state->colorFormat) + 1) %
                                    static_cast<int>(ColorFormatType::COUNT);
                m_state->colorFormat = static_cast<ColorFormatType>(nextIdx);
                m_state->colorFormatHex = (m_state->colorFormat == ColorFormatType::HEX);
                const bool zh = tools3000::core::isUiLanguageChinese();
                const wchar_t* fmtName = CaptureShortcutPolicy::colorFormatShortName(m_state->colorFormat);
                m_state->loupeToastUntil = GetTickCount() + 1200;
                m_state->loupeToastMessage = (zh ? L"取色格式: " : L"Color Format: ") +
                                             std::wstring(fmtName);
                SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                m_renderer->invalidate();
            }
            return 0;
        }

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            if (!this) break;

            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool alt = (msg == WM_SYSKEYDOWN) || ((GetKeyState(VK_MENU) & 0x8000) != 0);
            const bool isAutoRepeat = ((lParam & (static_cast<LPARAM>(1) << 30)) != 0);

            CaptureShortcutPolicy::updateShiftTapOnKeyDown(
                m_state->shiftTapCandidate, wParam, isAutoRepeat, ctrl, alt);

            const auto keyCtx = CaptureShortcutPolicy::snapshotFromState(*m_state);
            if (m_state->isLongImageMode() && !keyCtx.editingText && !ctrl && !alt) {
                const auto& stg = m_state->longCanvas.stageRect;
                const float pageStep = (std::max)(120.0f, (stg.bottom - stg.top) * 0.82f);
                if (wParam == VK_HOME) {
                    m_state->scrollLongCanvasToDocRow(static_cast<float>(std::min(m_state->dragStart.y, m_state->dragEnd.y)), 0.12f);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }
                if (wParam == VK_END) {
                    m_state->scrollLongCanvasToDocRow(static_cast<float>(std::max(m_state->dragStart.y, m_state->dragEnd.y)), 0.82f);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }
                if (wParam == VK_PRIOR || wParam == VK_NEXT) {
                    m_state->longCanvas.scrollY += (wParam == VK_PRIOR) ? -pageStep : pageStep;
                    m_state->clampLongCanvasScroll();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }
                if (wParam == VK_SPACE) {
                    updateHoverCursor(m_state->currentCursor);
                    return 0;
                }
            }
            const bool hasSelection = (keyCtx.overlayState == OverlayState::Selected ||
                                       keyCtx.overlayState == OverlayState::Marking);
            const auto decision = CaptureShortcutPolicy::resolveKeyDown(keyCtx, wParam, ctrl, shift, alt);

            switch (decision.action) {
                case CaptureShortcutAction::None:
                    if (msg == WM_SYSKEYDOWN && (wParam == VK_MENU || wParam == VK_F10)) {
                        return 0; // 抑制 Win32 系统菜单焦点抢占
                    }
                    return (msg == WM_SYSKEYDOWN) ? DefWindowProcW(hwnd, msg, wParam, lParam) : 0;

                case CaptureShortcutAction::ConsumeInHistoryMode:
                case CaptureShortcutAction::CycleColorFormat:
                    return 0;

                case CaptureShortcutAction::ToggleHistoryMode:
                    m_state->historyMode = !m_state->historyMode;
                    if (m_state->historyMode) {
                        m_state->historyIndex = 0;
                        m_renderer->updateHistoryBitmap(*m_state);
                    }
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::StepHistoryPrev:
                case CaptureShortcutAction::StepHistoryNext: {
                    const int total = CaptureHistory::instance().count();
                    if (total > 0) {
                        if (!m_state->historyMode) {
                            m_state->historyMode = true;
                            m_state->historyIndex = 0;
                        } else if (decision.action == CaptureShortcutAction::StepHistoryPrev) {
                            if (m_state->historyIndex < total - 1) m_state->historyIndex++;
                        } else {
                            if (m_state->historyIndex > 0) m_state->historyIndex--;
                        }
                        m_renderer->updateHistoryBitmap(*m_state);
                        m_renderer->invalidate();
                    }
                    return 0;
                }

                case CaptureShortcutAction::ExitHistoryMode:
                    m_state->historyMode = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::MoveCursorPixels: {
                    POINT screenPt{};
                    GetCursorPos(&screenPt);
                    screenPt.x += decision.dx;
                    screenPt.y += decision.dy;
                    SetCursorPos(screenPt.x, screenPt.y);

                    POINT clientPt = screenPt;
                    ScreenToClient(hwnd, &clientPt);
                    const POINT prevCursor = m_state->currentCursor;
                    m_state->currentCursor = clientPt;

                    if (m_state->dragging) {
                        if ((GetKeyState(VK_SPACE) & 0x8000) != 0) {
                            const int pdx = clientPt.x - prevCursor.x;
                            const int pdy = clientPt.y - prevCursor.y;
                            m_state->dragStart.x += pdx;
                            m_state->dragStart.y += pdy;
                            m_state->dragEnd.x += pdx;
                            m_state->dragEnd.y += pdy;
                        } else {
                            m_state->dragEnd = clientPt;
                            const float targetRatio = getTargetAspectRatio(m_state->aspectRatio);
                            if (targetRatio > 0.0f) {
                                const float rdx = static_cast<float>(m_state->dragEnd.x - m_state->dragStart.x);
                                const float rdy = static_cast<float>(m_state->dragEnd.y - m_state->dragStart.y);
                                const float signX = (rdx >= 0.0f) ? 1.0f : -1.0f;
                                const float signY = (rdy >= 0.0f) ? 1.0f : -1.0f;
                                const float curW = std::abs(rdx);
                                const float curH = std::abs(rdy);
                                if (curW / targetRatio >= curH) {
                                    m_state->dragEnd.y = static_cast<LONG>(m_state->dragStart.y + signY * (curW / targetRatio));
                                } else {
                                    m_state->dragEnd.x = static_cast<LONG>(m_state->dragStart.x + signX * (curH * targetRatio));
                                }
                            }
                        }
                        m_state->lastMousePos = clientPt;
                    } else if (m_state->options.autoDetectWindow &&
                               m_state->state.load() == OverlayState::Selecting) {
                        bool preserveHierarchy = false;
                        if (m_state->detectedWindowHierarchyIndex > 0 &&
                            m_state->detectedWindow.right > m_state->detectedWindow.left &&
                            m_state->detectedWindow.bottom > m_state->detectedWindow.top) {
                            RECT guardRect = m_state->detectedWindow;
                            guardRect.left -= 6;
                            guardRect.top -= 6;
                            guardRect.right += 6;
                            guardRect.bottom += 6;
                            if (PtInRect(&guardRect, clientPt)) {
                                preserveHierarchy = true;
                            }
                        }
                        if (!preserveHierarchy) {
                            m_state->detectedWindowHierarchy = detectWindowHierarchy(screenPt);
                            m_state->detectedWindowHierarchyIndex = 0;
                            if (!m_state->detectedWindowHierarchy.empty()) {
                                m_state->detectedWindow = m_state->detectedWindowHierarchy.front();
                            }
                            m_state->isHierarchyAnimating = false;
                            m_state->animDetectedWindowRect = {};
                            m_state->targetDetectedWindowRect = {};
                        }
                        m_state->lastMousePos = clientPt;
                    }
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::NudgeSelectionOrElement:
                    if (m_state->activeElement) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->moveBy(decision.dx, decision.dy);
                        m_renderer->markMarkupDirty();
                    } else {
                        adjustSelection(HitArea::Body, decision.dx, decision.dy);
                        prepareMarkupBase();
                    }
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::ResizeSelectionByKey:
                    adjustSelection(HitArea::RB, decision.dx, decision.dy);
                    prepareMarkupBase();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::StepWindowHierarchy:
                    stepDetectedWindowHierarchy(hwnd, decision.intDelta);
                    return 0;

                case CaptureShortcutAction::CopyCurrentPixelColor:
                case CaptureShortcutAction::CopyAlternatePixelColor: {
                    int cr = 0, cg = 0, cb = 0;
                    const POINT samplePt = m_state->screenToDocPoint(m_state->currentCursor);
                    if (m_renderer->sampleScreenColor(
                            samplePt.x, samplePt.y, cr, cg, cb, *m_state)) {
                        const ColorFormatType fmt =
                            (decision.action == CaptureShortcutAction::CopyAlternatePixelColor)
                                ? CaptureShortcutPolicy::alternateColorFormat(m_state->colorFormat)
                                : m_state->colorFormat;
                        const std::string colorText = formatColorText(fmt, cr, cg, cb);
                        tools3000::core::WinUtils::copyToClipboard(colorText);
                        m_state->loupeToastUntil = GetTickCount() + 1500;
                        const bool zh = tools3000::core::isUiLanguageChinese();
                        const std::wstring wColorText = tools3000::core::WinUtils::utf8ToWstring(colorText);
                        const wchar_t* fmtName = CaptureShortcutPolicy::colorFormatShortName(fmt);
                        m_state->loupeToastMessage = (zh ? L"已复制 " : L"Copied ") +
                                                     std::wstring(fmtName) + L": " + wColorText;
                        if (hasSelection) {
                            m_state->currentColor = MarkupColor(
                                static_cast<uint8_t>(cr), static_cast<uint8_t>(cg), static_cast<uint8_t>(cb));
                            if (m_state->activeElement) {
                                m_state->markup.pushUndoSnapshot();
                                resetCoalescedUndo(*m_state);
                                m_state->activeElement->color = m_state->currentColor;
                                m_renderer->markMarkupDirty();
                            }
                            m_state->toolbarLayoutValid = false;
                        }
                        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                        m_renderer->invalidate();
                    }
                    return 0;
                }

                case CaptureShortcutAction::SelectFullScreen:
                    m_state->dragging = false;
                    m_state->dragStart = {0, 0};
                    m_state->dragEnd = {m_state->frozenScreen.cols, m_state->frozenScreen.rows};
                    m_state->state = OverlayState::Selected;
                    m_state->isMarkupToolActive = false;
                    m_state->toolbarLayoutValid = false;
                    prepareMarkupBase();
                    m_renderer->invalidate();
                    syncShortcutHintContext();
                    return 0;

                case CaptureShortcutAction::LockAspectRatio:
                    m_state->aspectRatio = decision.aspectPreset;
                    enforceAspectRatioOnCurrentSelection();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::AdjustNumberCounter: {
                    const NumberFormat targetFmt = (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number)
                        ? m_state->activeElement->numberFormat
                        : m_state->currentNumberFormat;
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number) {
                        pushCoalescedElementUndo(*m_state, 450);
                        m_state->activeElement->numberValue =
                            (std::max)(1, m_state->activeElement->numberValue + decision.intDelta);
                        m_renderer->markMarkupDirty();
                    } else {
                        const int nextNum = (std::max)(1, m_state->markup.currentNumber(targetFmt) + decision.intDelta);
                        m_state->markup.resetNumber(nextNum, targetFmt);
                    }
                    const int shownNum = (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Number)
                        ? m_state->activeElement->numberValue
                        : m_state->markup.currentNumber(targetFmt);
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    const std::wstring shownLabel = formatNumberBadgeLabel(shownNum, targetFmt);
                    m_state->loupeToastUntil = GetTickCount() + 1200;
                    m_state->loupeToastMessage = (zh ? L"当前序号: " : L"Step ") + shownLabel;
                    SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::AdjustCornerRadius: {
                    if (decision.intDelta < 0) {
                        if (m_state->beautyShell.enabled) {
                            static const std::array<float, 7> radiuses = {0.0f, 8.0f, 10.0f, 12.0f, 16.0f, 24.0f, 32.0f};
                            for (int i = static_cast<int>(radiuses.size()) - 1; i >= 0; --i) {
                                if (radiuses[i] < m_state->beautyShell.cornerRadius - 0.5f) {
                                    m_state->beautyShell.cornerRadius = radiuses[i];
                                    break;
                                }
                                if (i == 0) m_state->beautyShell.cornerRadius = 0.0f;
                            }
                            tools3000::core::ConfigManager::instance().set<int>(
                                "/capture/beautyShellRadius", static_cast<int>(std::round(m_state->beautyShell.cornerRadius)));
                        } else {
                            const float cfgMaxRadius = static_cast<float>(
                                tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0));
                            static const std::array<float, 6> radiuses = {0.0f, 8.0f, 14.0f, 24.0f, 40.0f, 60.0f};
                            for (int i = static_cast<int>(radiuses.size()) - 1; i >= 0; --i) {
                                if (radiuses[i] <= cfgMaxRadius && radiuses[i] < m_state->cornerRadius - 0.5f) {
                                    m_state->cornerRadius = radiuses[i];
                                    break;
                                }
                                if (i == 0) m_state->cornerRadius = 0.0f;
                            }
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
                        }
                    } else {
                        if (m_state->beautyShell.enabled) {
                            static const std::array<float, 7> radiuses = {0.0f, 8.0f, 10.0f, 12.0f, 16.0f, 24.0f, 32.0f};
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
                            const float cfgMaxRadius = static_cast<float>(
                                tools3000::core::ConfigManager::instance().get<double>("/screenshot/maxCornerRadius", 60.0));
                            static const std::array<float, 6> radiuses = {0.0f, 8.0f, 14.0f, 24.0f, 40.0f, 60.0f};
                            for (size_t i = 0; i < radiuses.size(); ++i) {
                                if (radiuses[i] <= cfgMaxRadius && radiuses[i] > m_state->cornerRadius + 0.5f) {
                                    m_state->cornerRadius = radiuses[i];
                                    break;
                                }
                                if (i == radiuses.size() - 1) m_state->cornerRadius = (std::min)(60.0f, cfgMaxRadius);
                            }
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/screenshot/cornerRadius", static_cast<double>(m_state->cornerRadius));
                        }
                    }
                    prepareMarkupBase();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::AdjustStrokeWidth: {
                    if (m_state->currentTool == MarkupTool::Text ||
                        (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text)) {
                        const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                        if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                            pushCoalescedElementUndo(*m_state, 450);
                            const float newPhysSize = std::clamp(
                                m_state->activeElement->fontSize + static_cast<float>(decision.intDelta * 2),
                                10.0f, 144.0f * curDpi);
                            m_state->activeElement->fontSize = newPhysSize;
                            m_state->activeElement->textRenderSize = cv::Size(0, 0);
                            m_renderer->markMarkupDirty();
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/capture/markup/text/fontSize",
                                std::clamp(static_cast<double>(newPhysSize / curDpi), 10.0, 144.0));
                        } else {
                            const float curLogical = static_cast<float>(
                                tools3000::core::ConfigManager::instance().get<double>(
                                    "/capture/markup/text/fontSize", 18.0));
                            const float newLogical = std::clamp(
                                curLogical + static_cast<float>(decision.intDelta * 2), 10.0f, 144.0f);
                            tools3000::core::ConfigManager::instance().set<double>(
                                "/capture/markup/text/fontSize", static_cast<double>(newLogical));
                        }
                    } else {
                        m_state->currentStrokeWidth = std::clamp(m_state->currentStrokeWidth + decision.intDelta, 1, 60);
                        if (m_state->activeElement) {
                            pushCoalescedElementUndo(*m_state, 450);
                            const int val = m_state->currentStrokeWidth;
                            m_state->activeElement->thickness = static_cast<float>(val);
                            if (m_state->activeElement->tool == MarkupTool::Mosaic) {
                                m_state->activeElement->mosaicBlockSize = (std::max)(4, val * 2);
                            } else if (m_state->activeElement->tool == MarkupTool::Blur) {
                                m_state->activeElement->mosaicBlockSize = (std::max)(5, val * 3);
                            } else if (m_state->activeElement->tool == MarkupTool::Inpaint) {
                                m_state->activeElement->inpaintRadius = std::clamp(val, 2, 16);
                            } else if (m_state->activeElement->tool == MarkupTool::Watermark) {
                                const float curDpi = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
                                m_state->activeElement->fontSize =
                                    std::clamp(12.0f + static_cast<float>(val) * 1.5f, 12.0f, 64.0f) * curDpi;
                            }
                            m_renderer->markMarkupDirty();
                        }
                    }
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::ToggleMarkupTool:
                    toggleOrSetTool(decision.tool);
                    return 0;

                case CaptureShortcutAction::SelectPresetColor:
                    if (m_state->currentTool == MarkupTool::Magnifier ||
                        (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier)) {
                        m_state->currentMagnifierColor = decision.color;
                        if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Magnifier) {
                            m_state->markup.pushUndoSnapshot();
                            resetCoalescedUndo(*m_state);
                            m_state->activeElement->color = decision.color;
                            m_renderer->markMarkupDirty();
                        }
                    } else {
                        m_state->currentColor = decision.color;
                        if (m_state->activeElement) {
                            m_state->markup.pushUndoSnapshot();
                            resetCoalescedUndo(*m_state);
                            m_state->activeElement->color = decision.color;
                            m_renderer->markMarkupDirty();
                        }
                    }
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::ToggleBeautyShell: {
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    m_state->beautyShell.enabled = !m_state->beautyShell.enabled;
                    tools3000::core::ConfigManager::instance().set<bool>(
                        "/capture/beautyShellEnabled", m_state->beautyShell.enabled);
                    if (!m_state->beautyShell.enabled &&
                        (m_state->dropdownMenu.type == DropdownType::BeautyShellBg ||
                         m_state->dropdownMenu.type == DropdownType::BeautyShellPadding ||
                         m_state->dropdownMenu.type == DropdownType::BeautyShellRadius)) {
                        closeDropdownMenu(*m_state, false);
                    }
                    m_state->loupeToastUntil = GetTickCount() + 1500;
                    m_state->loupeToastMessage = m_state->beautyShell.enabled
                        ? (zh ? L"已开启美化外壳" : L"Beauty Shell enabled")
                        : (zh ? L"已关闭美化外壳" : L"Beauty Shell disabled");
                    SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::CycleBeautyShellTheme: {
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    const int cur = static_cast<int>(m_state->beautyShell.bgType);
                    const int next = (cur + 1) % static_cast<int>(BeautyBackgroundType::COUNT);
                    m_state->beautyShell.bgType = static_cast<BeautyBackgroundType>(next);
                    m_state->beautyShell.enabled = true;
                    static const char* themeKeys[] = {
                        "aurora_purple", "sunrise_coral", "emerald_sea", "obsidian_dark", "minimal_light"
                    };
                    tools3000::core::ConfigManager::instance().set<std::string>(
                        "/capture/beautyShellTheme", (next >= 0 && next < 5) ? themeKeys[next] : "aurora_purple");
                    tools3000::core::ConfigManager::instance().set<bool>("/capture/beautyShellEnabled", true);
                    static const wchar_t* bgNamesZh[] = { L"极光紫", L"日落珊瑚", L"翡翠海", L"曜石黑", L"极简浅灰" };
                    static const wchar_t* bgNamesEn[] = { L"Aurora Purple", L"Sunrise Coral", L"Emerald Sea", L"Obsidian Dark", L"Minimal Light" };
                    const std::wstring name = (next >= 0 && next < 5)
                        ? (zh ? bgNamesZh[next] : bgNamesEn[next])
                        : (zh ? L"自定义" : L"Custom");
                    m_state->loupeToastUntil = GetTickCount() + 1500;
                    m_state->loupeToastMessage = (zh ? L"外壳质感: " : L"Shell Theme: ") + name;
                    SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::CycleBeautyShellPadding: {
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    static constexpr std::array<int, 6> kPaddings = {16, 24, 32, 48, 64, 72};
                    int nextPad = kPaddings[2];
                    for (size_t i = 0; i < kPaddings.size(); ++i) {
                        if (m_state->beautyShell.padding <= kPaddings[i]) {
                            nextPad = kPaddings[(i + 1) % kPaddings.size()];
                            break;
                        }
                        if (i + 1 == kPaddings.size()) {
                            nextPad = kPaddings[0];
                        }
                    }
                    m_state->beautyShell.padding = nextPad;
                    m_state->beautyShell.enabled = true;
                    tools3000::core::ConfigManager::instance().set<int>("/capture/beautyShellPadding", nextPad);
                    tools3000::core::ConfigManager::instance().set<bool>("/capture/beautyShellEnabled", true);
                    m_state->loupeToastUntil = GetTickCount() + 1400;
                    m_state->loupeToastMessage = (zh ? L"外壳留白: " : L"Shell Padding: ") +
                                                 std::to_wstring(nextPad) + L"px";
                    SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;
                }

                case CaptureShortcutAction::ReEditSelectedText: {
                    if (m_state->activeElement && m_state->activeElement->tool == MarkupTool::Text) {
                        const std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(m_state->activeElement->text);
                        m_state->activeElement->isEditing = true;
                        m_state->isSelectingText = false;
                        if (decision.selectAllOnTextEdit && !wstr.empty()) {
                            CaptureTextEditor::selectAll(
                                wstr, m_state->activeElement->textCaret, m_state->activeElement->textSelAnchor);
                        } else {
                            m_state->activeElement->textCaret = wstr.size();
                            m_state->activeElement->textSelAnchor = wstr.size();
                        }
                        m_state->activeElement->textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                        syncShortcutHintContext();
                    }
                    return 0;
                }

                case CaptureShortcutAction::DeleteSelectedElement:
                    if (m_state->activeElement) {
                        resetCoalescedUndo(*m_state);
                        m_state->markup.removeElement(m_state->activeElement->id);
                        m_state->activeElement = nullptr;
                        m_renderer->markMarkupDirty();
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                        syncShortcutHintContext();
                    }
                    return 0;

                case CaptureShortcutAction::UndoMarkup:
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement = nullptr;
                    m_state->isManipulating = false;
                    m_state->dragHandle = HitArea::None;
                    m_state->markup.undo();
                    for (const auto& el : m_state->markup.elements()) {
                        if (el) {
                            el->isActive = false;
                            el->isEditing = false;
                        }
                    }
                    m_renderer->markMarkupDirty();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::RedoMarkup:
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement = nullptr;
                    m_state->isManipulating = false;
                    m_state->dragHandle = HitArea::None;
                    m_state->markup.redo();
                    for (const auto& el : m_state->markup.elements()) {
                        if (el) {
                            el->isActive = false;
                            el->isEditing = false;
                        }
                    }
                    m_renderer->markMarkupDirty();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::ClearAllMarkup:
                    resetCoalescedUndo(*m_state);
                    m_state->activeElement = nullptr;
                    m_state->isManipulating = false;
                    m_state->dragHandle = HitArea::None;
                    m_state->markup.clearElementsUndoable();
                    m_renderer->markMarkupDirty();
                    m_state->toolbarLayoutValid = false;
                    m_renderer->invalidate();
                    return 0;

                case CaptureShortcutAction::PinSelection: {
                    ToolbarButton pinBtn;
                    pinBtn.command = ToolbarCommand::PinWindow;
                    executeToolbarCommand(pinBtn);
                    return 0;
                }

                case CaptureShortcutAction::SaveSelectionAs: {
                    if (m_state->isLongImageMode()) {
                        if (m_state->isMarking) {
                            finishMarkup(m_state->currentCursor);
                        }
                        if (m_confirmCb) {
                            m_confirmCb({CaptureCompletionAction::SaveAs});
                        }
                        return 0;
                    }
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
                    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
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
                    return 0;
                }

                case CaptureShortcutAction::ConfirmOrCommitUi:
                    if (m_state->dropdownMenu.type != DropdownType::None) {
                        if (m_state->dropdownMenu.hoveredIndex >= 0 &&
                            m_state->dropdownMenu.hoveredIndex < static_cast<int>(m_state->dropdownMenu.items.size())) {
                            commitDropdownSelection(
                                m_state->dropdownMenu.type,
                                m_state->dropdownMenu.items[m_state->dropdownMenu.hoveredIndex].id);
                        } else {
                            closeDropdownMenu(*m_state, false);
                        }
                        m_renderer->invalidate();
                        return 0;
                    }
                    if (hasAnyCapturePopupOpen(*m_state)) {
                        closeAllCapturePopups(*m_state, false);
                        m_renderer->invalidate();
                        return 0;
                    }
                    if (m_state->isMarking) {
                        finishMarkup(m_state->currentCursor);
                    }
                    if (hasSelection) {
                        if (m_state->mode == OverlayMode::RecordRegion) {
                            if (m_confirmCb) m_confirmCb({});
                        } else {
                            if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Default});
                        }
                        return 0;
                    }
                    if (m_state->detectedWindow.right > m_state->detectedWindow.left &&
                        m_state->detectedWindow.bottom > m_state->detectedWindow.top) {
                        m_state->dragStart = {static_cast<LONG>(m_state->detectedWindow.left),
                                             static_cast<LONG>(m_state->detectedWindow.top)};
                        m_state->dragEnd = {static_cast<LONG>(m_state->detectedWindow.right),
                                           static_cast<LONG>(m_state->detectedWindow.bottom)};
                        m_state->cornerRadius = m_state->detectedWindowCornerRadius;
                        m_state->state = OverlayState::Selected;
                        prepareMarkupBase();
                        if (m_state->mode == OverlayMode::RecordRegion) {
                            if (m_confirmCb) m_confirmCb({});
                        } else {
                            if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Default});
                        }
                    }
                    return 0;

                case CaptureShortcutAction::CopySelectionAndFinish:
                    if (m_state->isMarking) {
                        finishMarkup(m_state->currentCursor);
                    }
                    if (hasSelection) {
                        if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
                    } else if (m_state->dragging) {
                        m_state->dragging = false;
                        m_state->state = OverlayState::Selected;
                        prepareMarkupBase();
                        if (m_confirmCb) m_confirmCb({CaptureCompletionAction::Copy});
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
                    }
                    return 0;

                case CaptureShortcutAction::HierarchicalEscape:
                    if (hasAnyCapturePopupOpen(*m_state)) {
                        closeAllCapturePopups(*m_state, true);
                        m_renderer->invalidate();
                    } else if (keyCtx.editingText && m_state->activeElement) {
                        m_state->activeElement->isEditing = false;
                        m_state->isSelectingText = false;
                        if (m_state->activeElement->text.empty()) {
                            m_state->markup.removeElement(m_state->activeElement->id);
                            m_state->activeElement = nullptr;
                        }
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        syncShortcutHintContext();
                    } else if (m_state->activeElement) {
                        m_state->activeElement->isActive = false;
                        m_state->activeElement = nullptr;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        syncShortcutHintContext();
                    } else if (m_state->isMarkupToolActive) {
                        m_state->isMarkupToolActive = false;
                        m_state->openSubmenu = SubmenuType::None;
                        m_state->sliderPopup.type = SliderPopupType::None;
                        m_state->toolbarLayoutValid = false;
                        m_renderer->invalidate();
                        updateHoverCursor(m_state->currentCursor);
                        syncShortcutHintContext();
                    } else if (m_state->dragging) {
                        m_state->dragging = false;
                        m_state->state = OverlayState::Selecting;
                        m_renderer->invalidate();
                        syncShortcutHintContext();
                    } else if (hasSelection && !m_state->isLongImageMode()) {
                        m_state->state = OverlayState::Selecting;
                        m_state->dragStart = {0, 0};
                        m_state->dragEnd = {0, 0};
                        m_state->isHierarchyAnimating = false;
                        m_state->animDetectedWindowRect = {};
                        m_state->targetDetectedWindowRect = {};
                        m_state->primaryToolbarRect = {};
                        m_state->secondaryToolbarRect = {};
                        m_state->selectionSideRect = {};
                        m_state->toolbarLayoutValid = false;
                        m_state->markup.clearAll();
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        syncShortcutHintContext();
                    } else {
                        ShortcutHintOverlay::instance().hide();
                        if (m_cancelCb) m_cancelCb();
                    }
                    return 0;

                case CaptureShortcutAction::DelegateToTextEditor: {
                    if (!m_state->activeElement) return 0;
                    const bool zh = tools3000::core::isUiLanguageChinese();
                    auto& elem = *m_state->activeElement;
                    std::wstring wstr = tools3000::core::WinUtils::utf8ToWstring(elem.text);

                    if (ctrl && (wParam == 'A' || wParam == 'a')) {
                        CaptureTextEditor::selectAll(wstr, elem.textCaret, elem.textSelAnchor);
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (ctrl && (wParam == 'C' || wParam == 'c')) {
                        if (CaptureTextEditor::hasSelection(elem.textCaret, elem.textSelAnchor)) {
                            const std::wstring sub = CaptureTextEditor::selectedText(wstr, elem.textCaret, elem.textSelAnchor);
                            if (!sub.empty()) {
                                tools3000::core::WinUtils::copyToClipboard(sub, hwnd);
                                m_state->loupeToastUntil = GetTickCount() + 1400;
                                m_state->loupeToastMessage = zh ? L"已复制选中文字" : L"Selected text copied";
                                SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                                m_renderer->invalidate();
                            }
                        } else if (!wstr.empty()) {
                            tools3000::core::WinUtils::copyToClipboard(wstr, hwnd);
                            m_state->loupeToastUntil = GetTickCount() + 1400;
                            m_state->loupeToastMessage = zh ? L"已复制标注文字" : L"Annotation text copied";
                            SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                            m_renderer->invalidate();
                        }
                        return 0;
                    }

                    if (ctrl && (wParam == 'X' || wParam == 'x')) {
                        if (CaptureTextEditor::hasSelection(elem.textCaret, elem.textSelAnchor)) {
                            const std::wstring sub = CaptureTextEditor::selectedText(wstr, elem.textCaret, elem.textSelAnchor);
                            if (!sub.empty()) {
                                m_state->markup.pushUndoSnapshot();
                                resetCoalescedUndo(*m_state);
                                tools3000::core::WinUtils::copyToClipboard(sub, hwnd);
                                CaptureTextEditor::insertText(wstr, elem.textCaret, elem.textSelAnchor, L"");
                                elem.text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                                elem.textRenderSize = cv::Size(0, 0);
                                elem.textLastCaretTick = GetTickCount();
                                m_state->textPreferredCaretX = -1.0f;
                                m_state->loupeToastUntil = GetTickCount() + 1400;
                                m_state->loupeToastMessage = zh ? L"已剪切选中文字" : L"Selected text cut";
                                SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
                                m_renderer->markMarkupDirty();
                                m_renderer->invalidate();
                            }
                        }
                        return 0;
                    }

                    if (ctrl && (wParam == 'V' || wParam == 'v')) {
                        std::wstring clipWstr;
                        if (OpenClipboard(nullptr)) {
                            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                                if (HANDLE hData = GetClipboardData(CF_UNICODETEXT)) {
                                    if (const wchar_t* pText = static_cast<const wchar_t*>(GlobalLock(hData))) {
                                        clipWstr = pText;
                                        GlobalUnlock(hData);
                                    }
                                }
                            }
                            CloseClipboard();
                        }
                        if (!clipWstr.empty()) {
                            m_state->markup.pushUndoSnapshot();
                            resetCoalescedUndo(*m_state);
                            std::wstring normalized;
                            normalized.reserve(clipWstr.size());
                            for (size_t i = 0; i < clipWstr.size(); ++i) {
                                if (clipWstr[i] == L'\r') {
                                    if (i + 1 < clipWstr.size() && clipWstr[i + 1] == L'\n') {
                                        continue;
                                    }
                                    normalized.push_back(L'\n');
                                } else {
                                    normalized.push_back(clipWstr[i]);
                                }
                            }
                            CaptureTextEditor::insertText(wstr, elem.textCaret, elem.textSelAnchor, normalized);
                            elem.text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                            elem.textRenderSize = cv::Size(0, 0);
                            elem.textLastCaretTick = GetTickCount();
                            m_state->textPreferredCaretX = -1.0f;
                            m_renderer->markMarkupDirty();
                            m_renderer->invalidate();
                        }
                        return 0;
                    }

                    if (wParam == VK_LEFT) {
                        CaptureTextEditor::moveLeft(wstr, elem.textCaret, elem.textSelAnchor, shift, ctrl);
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (wParam == VK_RIGHT) {
                        CaptureTextEditor::moveRight(wstr, elem.textCaret, elem.textSelAnchor, shift, ctrl);
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (wParam == VK_HOME) {
                        CaptureTextEditor::moveHome(wstr, elem.textCaret, elem.textSelAnchor, shift, ctrl);
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (wParam == VK_END) {
                        CaptureTextEditor::moveEnd(wstr, elem.textCaret, elem.textSelAnchor, shift, ctrl);
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (wParam == VK_UP || wParam == VK_DOWN) {
                        const auto layout = CaptureTextEditor::buildLayout(wstr, static_cast<int>(elem.fontSize));
                        CaptureTextEditor::moveVertical(
                            layout, wstr, elem.textCaret, elem.textSelAnchor,
                            (wParam == VK_UP) ? -1 : 1, shift, m_state->textPreferredCaretX);
                        elem.textLastCaretTick = GetTickCount();
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }

                    if (wParam == VK_DELETE) {
                        if (!wstr.empty() || CaptureTextEditor::hasSelection(elem.textCaret, elem.textSelAnchor)) {
                            pushCoalescedElementUndo(*m_state, 650);
                        }
                        if (CaptureTextEditor::deleteForward(wstr, elem.textCaret, elem.textSelAnchor, ctrl)) {
                            elem.text = tools3000::core::WinUtils::wstringToUtf8(wstr);
                            elem.textRenderSize = cv::Size(0, 0);
                        }
                        elem.textLastCaretTick = GetTickCount();
                        m_state->textPreferredCaretX = -1.0f;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                        return 0;
                    }
                    return 0;
                }
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
                
                // 长图 Smartisan 首尾裁切标尺拖至上下视口边缘时 60fps 平滑自动滚屏与切口同步推进
                bool edgeAutoScrolling = false;
                if (m_state->isLongImageMode() && m_state->isAdjustingSelection &&
                    (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
                    const float curScale = std::clamp(m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f, 1.0f, 5.0f);
                    const float edgeZone = 44.0f * curScale;
                    const auto& stg = m_state->longCanvas.stageRect;
                    const float cy = static_cast<float>(m_state->currentCursor.y);
                    float scrollDelta = 0.0f;
                    if (cy < stg.top + edgeZone) {
                        const float ratio = std::clamp((stg.top + edgeZone - cy) / edgeZone, 0.0f, 1.0f);
                        scrollDelta = -(3.5f + ratio * 24.0f) * curScale;
                    } else if (cy > stg.bottom - edgeZone) {
                        const float ratio = std::clamp((cy - (stg.bottom - edgeZone)) / edgeZone, 0.0f, 1.0f);
                        scrollDelta = (3.5f + ratio * 24.0f) * curScale;
                    }
                    if (std::abs(scrollDelta) > 0.1f) {
                        edgeAutoScrolling = true;
                        const float prevScrollY = m_state->longCanvas.scrollY;
                        m_state->longCanvas.scrollY += scrollDelta;
                        m_state->clampLongCanvasScroll();
                        if (std::abs(m_state->longCanvas.scrollY - prevScrollY) > 0.1f) {
                            const auto curDoc = m_state->screenToDoc(
                                static_cast<float>(m_state->currentCursor.x), cy);
                            const int dx = static_cast<int>(std::trunc(curDoc.x - m_state->longCanvas.lastSelAdjustDocPt.x));
                            const int dy = static_cast<int>(std::trunc(curDoc.y - m_state->longCanvas.lastSelAdjustDocPt.y));
                            if (dx != 0 || dy != 0) {
                                adjustSelection(m_state->selAdjustHandle, dx, dy);
                                m_state->longCanvas.lastSelAdjustDocPt.x += static_cast<float>(dx);
                                m_state->longCanvas.lastSelAdjustDocPt.y += static_cast<float>(dy);
                            }
                            m_renderer->invalidate();
                        }
                    }
                }

                // 取色放大镜：复制成功提示的存续期间持续重绘，到期后再渲染一帧将其清除
                if (m_state->loupeToastUntil != 0) {
                    m_renderer->invalidate();
                    if (GetTickCount() >= m_state->loupeToastUntil) m_state->loupeToastUntil = 0;
                }
                // 文字编辑期间仅当 480ms 光标显隐状态发生翻转时重算标注图层，避免每 16ms 全画布克隆重绘。
                bool editingText = m_state->activeElement &&
                                   m_state->activeElement->tool == MarkupTool::Text &&
                                   m_state->activeElement->isEditing;
                if (editingText) {
                    const auto* elem = m_state->activeElement;
                    const uint32_t nowTick = GetTickCount();
                    const bool recentlyMoved = (elem->textLastCaretTick != 0) &&
                                               ((nowTick - elem->textLastCaretTick) < 520u);
                    const bool showCursor = recentlyMoved || ((nowTick / 480u) % 2u == 0u) || elem->text.empty();
                    if (m_renderer->needsRender()) {
                        m_lastEditedElementId = elem->id;
                        m_lastTextCaretVisible = showCursor;
                    } else if (elem->id != m_lastEditedElementId || showCursor != m_lastTextCaretVisible) {
                        m_lastEditedElementId = elem->id;
                        m_lastTextCaretVisible = showCursor;
                        m_renderer->markMarkupDirty();
                        m_renderer->invalidate();
                    }
                } else {
                    m_lastEditedElementId = 0;
                }
                if (m_renderer->needsRender()) {
                    m_renderer->clearNeedsRender();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }

                // 动效已结束且微晶徽章已过淡出时间、无取色提示、无文字编辑，销毁计时器实现零空闲开销
                if (!m_state->isHierarchyAnimating &&
                    !edgeAutoScrolling &&
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
    if (m_renderer) {
        if (type == DropdownType::MagnifierCalloutStyle && m_state->activeElement) {
            m_renderer->markMarkupDirty();
        }
        m_renderer->invalidate();
    }
}

} // namespace tools3000::capture
