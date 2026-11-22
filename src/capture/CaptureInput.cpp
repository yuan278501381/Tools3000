// ─────────────────────────────────────────────────────────────────────────────
// CaptureInput.cpp — 截图输入核心：光标/选区几何、标注起止、窗口探测与消息分发中枢
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include "core/i18n/UiI18n.h"
#include <windowsx.h>
#include "capture/CaptureInputInternal.h"
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
#include "capture/WatermarkRenderer.h"
#include "capture/TextAnnotationHelper.h"
#include "ocr/OcrEngine.h"
#include "core/config/ConfigManager.h"
#include "core/ipc/MessageBridge.h"
#include "core/events/MainThreadDispatcher.h"
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

using namespace input_detail;

namespace input_detail {

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


HCURSOR cursorForArea(HitArea area, int cornerIndex, float dpiScale, bool isDragging) {
    if (isVertexHitArea(area)) {
        return LoadCursor(nullptr, IDC_HAND);
    }
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

DWORD filterIndexForFormat(ImageFormat format) {
    switch (format) {
        case ImageFormat::JPEG: return 2;
        case ImageFormat::BMP: return 3;
        case ImageFormat::WebP: return 4;
        default: return 1;
    }
}

const wchar_t* extensionForFormat(ImageFormat format) {
    switch (format) {
        case ImageFormat::JPEG: return L"jpg";
        case ImageFormat::BMP: return L"bmp";
        case ImageFormat::WebP: return L"webp";
        default: return L"png";
    }
}

ImageFormat formatForSavePath(const std::filesystem::path& path, DWORD filterIndex) {
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


void pushCoalescedElementUndo(CaptureState& state, DWORD coalesceWindowMs) {
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

void resetCoalescedUndo(CaptureState& state) noexcept {
    state.lastCoalescedUndoElemId = 0;
    state.lastCoalescedUndoTick = 0;
}


} // namespace input_detail

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
        } else if (m_state->isSelectingText || m_state->isTextMarkupStreaming) {
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
                const auto cmRect = m_state->dropdownMenu.childMenuRect;
                if ((point.x >= dmRect.left && point.x <= dmRect.right &&
                     point.y >= dmRect.top && point.y <= dmRect.bottom) ||
                    (cmRect.right > cmRect.left &&
                     point.x >= cmRect.left && point.x <= cmRect.right &&
                     point.y >= cmRect.top && point.y <= cmRect.bottom)) {
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
        if (!cur && !m_state->isDrawingMultiPointLine) {
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
                } else if (m_state->currentTool == MarkupTool::Highlight &&
                           m_state->currentTextMarkupMode != TextMarkupMode::RectRegion &&
                           m_state->selectionOcrReady &&
                           !m_state->selectionOcrBlocks.empty()) {
                    const cv::Point localPt = toMarkupPoint(point);
                    size_t dummyChar = 0;
                    const int hitBlk = TextAnnotationHelper::hitTestBlockAndChar(
                        m_state->selectionOcrBlocks,
                        static_cast<float>(localPt.x),
                        static_cast<float>(localPt.y),
                        &dummyChar, 4.0f);
                    cur = (hitBlk >= 0) ? LoadCursor(nullptr, IDC_IBEAM) : getBeautifulCrosshairCursor(effDpi);
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

void CaptureInput::setCurrentTool(MarkupTool tool) {
    if (m_state->isDrawingMultiPointLine) {
        finalizeMultiPointLine(true);
    }
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
    m_state->isTextMarkupStreaming = false;
    m_state->liveTextMarkupPreviewSpans.clear();
    if (tool == MarkupTool::Highlight && m_state->currentTextMarkupMode != TextMarkupMode::RectRegion) {
        ensureSelectionOcrForTextMarkup();
    }
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
        if (m_state->detectedWindowHierarchyIndex >= 0 &&
            m_state->detectedWindowHierarchyIndex < static_cast<int>(m_state->detectedWindowHierarchyRadii.size())) {
            m_state->detectedWindowCornerRadius = m_state->detectedWindowHierarchyRadii[m_state->detectedWindowHierarchyIndex];
        }

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
            : (tools3000::core::trWCstrFor(zh, L"选区", L"Region"));
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
    m_state->isTextMarkupStreaming = false;
    m_state->liveTextMarkupPreviewSpans.clear();
    if (m_state->currentTool == MarkupTool::Pen) {
        m_state->penPoints.push_back(local);
    } else if (m_state->currentTool == MarkupTool::Highlight &&
               m_state->currentTextMarkupMode != TextMarkupMode::RectRegion) {
        if (!m_state->selectionOcrReady) {
            ensureSelectionOcrForTextMarkup();
        }
        if (m_state->selectionOcrReady && !m_state->selectionOcrBlocks.empty()) {
            size_t anchorChar = 0;
            const int anchorBlk = TextAnnotationHelper::hitTestBlockAndChar(
                m_state->selectionOcrBlocks,
                static_cast<float>(local.x),
                static_cast<float>(local.y),
                &anchorChar, 5.0f);
            if (anchorBlk >= 0) {
                m_state->isTextMarkupStreaming = true;
                m_state->textMarkupAnchorBlock = anchorBlk;
                m_state->textMarkupAnchorChar = anchorChar;
                std::vector<size_t> initIndices;
                std::vector<SelectedBlockCharSpan> initSpans;
                TextAnnotationHelper::computeStreamSelection(
                    m_state->selectionOcrBlocks,
                    anchorBlk, anchorChar,
                    static_cast<float>(local.x),
                    static_cast<float>(local.x), static_cast<float>(local.y),
                    initIndices, initSpans);
                m_state->liveTextMarkupPreviewSpans = TextAnnotationHelper::coalesceSelectedTextSpans(
                    m_state->selectionOcrBlocks, initIndices, initSpans);
                updateHoverCursor(point);
            }
        }
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
    } else if (m_state->isTextMarkupStreaming &&
               m_state->currentTool == MarkupTool::Highlight &&
               m_state->selectionOcrReady &&
               !m_state->selectionOcrBlocks.empty()) {
        const cv::Point startLocal = toMarkupPointFromDoc(m_state->markupStart);
        const cv::Point curLocal = toMarkupPointFromDoc(m_state->markupEnd);
        std::vector<size_t> selIndices;
        std::vector<SelectedBlockCharSpan> selSpans;
        TextAnnotationHelper::computeStreamSelection(
            m_state->selectionOcrBlocks,
            m_state->textMarkupAnchorBlock,
            m_state->textMarkupAnchorChar,
            static_cast<float>(startLocal.x),
            static_cast<float>(curLocal.x), static_cast<float>(curLocal.y),
            selIndices, selSpans);
        m_state->liveTextMarkupPreviewSpans = TextAnnotationHelper::coalesceSelectedTextSpans(
            m_state->selectionOcrBlocks, selIndices, selSpans);
    }
}

void CaptureInput::finalizeMultiPointLine(bool commit) {
    if (!m_state || !m_state->isDrawingMultiPointLine) return;

    std::vector<cv::Point> localPts;
    localPts.reserve(m_state->multiPointDocVertices.size());
    for (size_t i = 0; i < m_state->multiPointDocVertices.size(); ++i) {
        const cv::Point lp = toMarkupPointFromDoc(m_state->multiPointDocVertices[i]);
        const bool isLastClosingFirst =
            (i + 1 == m_state->multiPointDocVertices.size() &&
             localPts.size() >= 3 &&
             std::hypot(lp.x - localPts.front().x, lp.y - localPts.front().y) <= 2.0);
        if (localPts.empty() ||
            isLastClosingFirst ||
            std::hypot(lp.x - localPts.back().x, lp.y - localPts.back().y) >= 2.0) {
            localPts.push_back(isLastClosingFirst ? localPts.front() : lp);
        }
    }

    m_state->isDrawingMultiPointLine = false;
    m_state->multiPointDocVertices.clear();
    m_state->isMarking = false;
    m_state->state = OverlayState::Selected;

    if (commit && localPts.size() >= 2) {
        prepareMarkupBase();
        const float currentDpiScale = m_state->dpiScale > 0.0f ? m_state->dpiScale : 1.0f;
        auto* elem = m_state->markup.drawPolyline(
            localPts,
            m_state->currentColor,
            static_cast<float>(m_state->currentStrokeWidth),
            m_state->currentLineStyle,
            m_state->currentSmoothCurve);
        if (elem) {
            elem->hasCustomCaps = m_state->currentLineArrowEnabled;
            elem->startCap = m_state->currentStartCap;
            elem->endCap = m_state->currentEndCap;
            elem->dpiScale = currentDpiScale;
            m_state->activeElement = elem;
            elem->isActive = true;
        }
        if (m_renderer) {
            m_renderer->markMarkupDirty();
        }
    }

    m_state->toolbarLayoutValid = false;
    if (m_renderer) {
        m_renderer->invalidate();
    }
    syncShortcutHintContext();
}

void CaptureInput::finishMarkup(POINT point) {
    if (m_state->isDrawingMultiPointLine) {
        finalizeMultiPointLine(true);
        return;
    }
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

    // 直线工具下轻点画布（非拖拽）：无缝进入多点连线模式（支持连续点击折线连接或美感平滑曲线连接）
    if (m_state->currentTool == MarkupTool::Line && dx < 5 && dy < 5) {
        MultiPointLinePolicy::startFromFirstClick(*m_state, m_state->markupStart);
        const bool zh = tools3000::core::isUiLanguageChinese();
        m_state->loupeToastUntil = GetTickCount() + 2400;
        m_state->loupeToastMessage = tools3000::core::trWCstrFor(zh, L"多点连线：继续点击添加节点 · 双击 / 右键 / Enter 完成", L"Multi-point: Click to add points · Double/Right-click or Enter to finish");
        if (m_hwnd) {
            SetTimer(m_hwnd, RENDER_TIMER_ID, 16, nullptr);
        }
        return;
    }

    if (m_state->currentTool != MarkupTool::Pen &&
        m_state->currentTool != MarkupTool::Number &&
        m_state->currentTool != MarkupTool::Magnifier &&
        !(m_state->isTextMarkupStreaming && !m_state->liveTextMarkupPreviewSpans.empty()) &&
        dx < 3 && dy < 3) {
        m_state->isMarking = false;
        m_state->isTextMarkupStreaming = false;
        m_state->liveTextMarkupPreviewSpans.clear();
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
            MarkupElement* elem = nullptr;
            if (m_state->isTextMarkupStreaming && !m_state->liveTextMarkupPreviewSpans.empty()) {
                if (m_state->currentTextMarkupMode == TextMarkupMode::Highlight) {
                    elem = m_state->markup.drawTextHighlightSpans(
                        m_state->liveTextMarkupPreviewSpans, m_state->currentColor);
                } else if (m_state->currentTextMarkupMode == TextMarkupMode::WavyUnderline) {
                    elem = m_state->markup.drawTextUnderlineSpans(
                        m_state->liveTextMarkupPreviewSpans, m_state->currentColor, LineStyle::Wavy, 0.0f);
                } else {
                    elem = m_state->markup.drawTextUnderlineSpans(
                        m_state->liveTextMarkupPreviewSpans, m_state->currentColor, LineStyle::Solid, 0.0f);
                }
            } else if (m_state->currentTextMarkupMode == TextMarkupMode::WavyUnderline) {
                elem = m_state->markup.drawLine(
                    start, end, m_state->currentColor, static_cast<float>(m_state->currentStrokeWidth), LineStyle::Wavy);
            } else if (m_state->currentTextMarkupMode == TextMarkupMode::StraightUnderline) {
                elem = m_state->markup.drawLine(
                    start, end, m_state->currentColor, static_cast<float>(m_state->currentStrokeWidth), LineStyle::Solid);
            } else {
                elem = m_state->markup.drawHighlight(start, end, m_state->currentColor);
            }
            m_state->isTextMarkupStreaming = false;
            m_state->liveTextMarkupPreviewSpans.clear();
            if (elem) {
                elem->dpiScale = currentDpiScale;
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
    if (m_state && m_state->isMarkupToolActive &&
        m_state->currentTool == MarkupTool::Highlight &&
        m_state->currentTextMarkupMode != TextMarkupMode::RectRegion) {
        ensureSelectionOcrForTextMarkup();
    }
}

void CaptureInput::ensureSelectionOcrForTextMarkup() {
    if (!m_state || m_state->frozenScreen.empty()) return;
    if (m_state->mode == OverlayMode::RecordRegion) return;

    const auto sel = currentSelectionRect();
    const int rx = static_cast<int>(std::round(sel.left));
    const int ry = static_cast<int>(std::round(sel.top));
    const int rw = static_cast<int>(std::round(sel.right - sel.left));
    const int rh = static_cast<int>(std::round(sel.bottom - sel.top));
    if (rw < 12 || rh < 10) return;

    const cv::Rect screenBounds(0, 0, m_state->frozenScreen.cols, m_state->frozenScreen.rows);
    const cv::Rect clipped = cv::Rect(rx, ry, rw, rh) & screenBounds;
    if (clipped.width < 12 || clipped.height < 10) return;

    if (m_state->selectionOcrCachedRect == clipped &&
        (m_state->selectionOcrReady || m_state->selectionOcrInFlight)) {
        return;
    }

    m_state->selectionOcrCachedRect = clipped;
    m_state->selectionOcrReady = false;
    m_state->selectionOcrInFlight = true;
    m_state->selectionOcrBlocks.clear();
    const uint64_t reqSeq = ++m_state->selectionOcrRequestSeq;
    cv::Mat cropImg = m_state->frozenScreen(clipped).clone();
    CaptureState* statePtr = m_state;
    CaptureRenderer* rendererPtr = m_renderer;
    CaptureInput* selfPtr = this;

    std::thread([cropImg = std::move(cropImg), reqSeq, statePtr, rendererPtr, selfPtr]() mutable {
        auto ocrLines = tools3000::ocr::OcrEngine::instance().extractText(cropImg);
        std::vector<CaptureState::SelectionOcrBlock> parsedBlocks;
        parsedBlocks.reserve(ocrLines.size());
        for (auto& line : ocrLines) {
            if (line.text.empty() || line.box.width <= 0 || line.box.height <= 0) continue;
            CaptureState::SelectionOcrBlock blk;
            blk.text = std::move(line.text);
            blk.origBox = line.box;
            blk.layoutRegionId = line.layoutRegionId;
            blk.alignedRowId = line.alignedRowId;
            blk.charBoxes = !line.charBoxes.empty()
                ? std::move(line.charBoxes)
                : tools3000::ocr::OcrEngine::buildProportionalCharBoxes(blk.text, blk.origBox);
            parsedBlocks.push_back(std::move(blk));
        }

        tools3000::core::MainThreadDispatcher::instance().post(
            [reqSeq, statePtr, rendererPtr, selfPtr, blocks = std::move(parsedBlocks)]() mutable {
                if (!CaptureOverlay::instance().isOverlayVisible() || !statePtr) return;
                if (statePtr->selectionOcrRequestSeq != reqSeq) return;
                statePtr->selectionOcrBlocks = std::move(blocks);
                statePtr->selectionOcrInFlight = false;
                statePtr->selectionOcrReady = true;
                if (selfPtr) {
                    selfPtr->updateHoverCursor(statePtr->currentCursor);
                }
                if (rendererPtr) {
                    rendererPtr->invalidate();
                }
            });
    }).detach();
}

void CaptureInput::beginDetectionSession(
    int originX,
    int originY,
    int width,
    int height,
    const cv::Mat* frozenScreen) {
    m_lastDetectedTopHwnd = nullptr;
    m_lastDetectScreenPt = {INT_MIN, INT_MIN};
    m_hasAsyncRefinedActive = false;
    m_elementDetector.beginSession(m_hwnd, originX, originY, width, height, frozenScreen);
}

void CaptureInput::endDetectionSession() {
    m_lastDetectedTopHwnd = nullptr;
    m_lastDetectScreenPt = {INT_MIN, INT_MIN};
    m_hasAsyncRefinedActive = false;
    m_elementDetector.endSession();
}

void CaptureInput::applyDetectedCandidates(
    const std::vector<DetectedElementCandidate>& candidates,
    bool animate,
    HWND hwnd) {
    if (!m_state) return;

    const RECT prevRect = m_state->detectedWindow;
    const bool hadPrev = (prevRect.right > prevRect.left && prevRect.bottom > prevRect.top);

    m_state->detectedWindowHierarchy.clear();
    m_state->detectedWindowHierarchyDescriptions.clear();
    m_state->detectedWindowHierarchyRadii.clear();
    m_state->detectedWindowHierarchy.reserve(candidates.size());
    m_state->detectedWindowHierarchyDescriptions.reserve(candidates.size());
    m_state->detectedWindowHierarchyRadii.reserve(candidates.size());

    for (const auto& cand : candidates) {
        m_state->detectedWindowHierarchy.push_back(cand.rect);
        m_state->detectedWindowHierarchyDescriptions.push_back(cand.description);
        m_state->detectedWindowHierarchyRadii.push_back(cand.cornerRadius);
    }

    m_state->detectedWindowHierarchyIndex = 0;
    if (m_state->detectedWindowHierarchy.empty()) {
        m_state->detectedWindow = {};
        m_state->detectedWindowCornerRadius = 0.0f;
        m_state->animDetectedWindowRect = {};
        m_state->targetDetectedWindowRect = {};
        m_state->isHierarchyAnimating = false;
        return;
    }

    const RECT nextRect = m_state->detectedWindowHierarchy.front();
    m_state->detectedWindow = nextRect;
    m_state->detectedWindowCornerRadius = m_state->detectedWindowHierarchyRadii.front();

    const D2D1_RECT_F targetF = D2D1::RectF(
        static_cast<float>(nextRect.left),
        static_cast<float>(nextRect.top),
        static_cast<float>(nextRect.right),
        static_cast<float>(nextRect.bottom));

    const bool rectChanged =
        !hadPrev ||
        std::abs(prevRect.left - nextRect.left) > 2 ||
        std::abs(prevRect.top - nextRect.top) > 2 ||
        std::abs(prevRect.right - nextRect.right) > 2 ||
        std::abs(prevRect.bottom - nextRect.bottom) > 2;

    if (animate && hadPrev && rectChanged) {
        if (m_state->animDetectedWindowRect.right <= m_state->animDetectedWindowRect.left) {
            m_state->animDetectedWindowRect = D2D1::RectF(
                static_cast<float>(prevRect.left),
                static_cast<float>(prevRect.top),
                static_cast<float>(prevRect.right),
                static_cast<float>(prevRect.bottom));
            m_state->animVelocity = D2D1::RectF(0, 0, 0, 0);
        } else {
            // 保留柔和连续速度防急刹顿挫，仅在跨越超远距离时适度阻尼收敛
            m_state->animVelocity.left *= 0.45f;
            m_state->animVelocity.top *= 0.45f;
            m_state->animVelocity.right *= 0.45f;
            m_state->animVelocity.bottom *= 0.45f;
        }
        m_state->targetDetectedWindowRect = targetF;
        m_state->isHierarchyAnimating = true;
        if (hwnd) SetTimer(hwnd, RENDER_TIMER_ID, 16, nullptr);
    } else {
        m_state->animDetectedWindowRect = targetF;
        m_state->targetDetectedWindowRect = targetF;
        m_state->animVelocity = D2D1::RectF(0, 0, 0, 0);
        m_state->isHierarchyAnimating = false;
    }
}

void CaptureInput::updateWindowDetectionAtCursor(HWND hwnd, POINT screenPt, bool allowAsyncRefine) {
    if (!m_state) return;
    HWND topHwnd = nullptr;
    auto syncCandidates = m_elementDetector.querySync(screenPt, &topHwnd);

    const bool sameTopWindow = (topHwnd != nullptr && topHwnd == m_lastDetectedTopHwnd);
    const bool hasActiveVisualRect =
        (m_state->detectedWindow.right > m_state->detectedWindow.left &&
         m_state->detectedWindow.bottom > m_state->detectedWindow.top);
    const bool syncOnlyCoarseWindow =
        syncCandidates.empty() ||
        (syncCandidates.front().kind != DetectedCandidateKind::Win32Child &&
         syncCandidates.front().kind != DetectedCandidateKind::UiaElement &&
         syncCandidates.front().kind != DetectedCandidateKind::VisionRegion);

    // 零回弹连续性防抖（Zero-Snapback Continuity）：
    // 在同一现代顶层窗口内微移鼠标时，严禁用粗糙的“整窗同步框”强行覆盖当前已锁定的精细 UIA 控件框
    // （杜绝“小控件 -> 闪弹回大窗口 -> 12ms 后又缩回小控件”的剧烈抖动），平稳等待异步 UIA 返回新框再平滑切换！
    const bool deferCoarseOverwrite =
        allowAsyncRefine && hwnd && sameTopWindow && hasActiveVisualRect &&
        m_hasAsyncRefinedActive && syncOnlyCoarseWindow;

    m_lastDetectedTopHwnd = topHwnd;
    m_lastDetectScreenPt = screenPt;

    if (!deferCoarseOverwrite) {
        m_hasAsyncRefinedActive = false;
        applyDetectedCandidates(syncCandidates, /*animate=*/true, hwnd);
    }
    if (allowAsyncRefine && hwnd && topHwnd) {
        m_elementDetector.requestAsyncRefine(hwnd, screenPt, topHwnd, syncCandidates);
    }
}

void CaptureInput::handleAsyncElementRefined(HWND hwnd) {
    if (!m_state || m_state->state.load() != OverlayState::Selecting ||
        m_state->dragging || !m_state->options.autoDetectWindow) {
        return;
    }
    // 若用户已通过滚轮或 Tab 手动锁定至高层级，则不打断用户当前层级选择
    if (m_state->detectedWindowHierarchyIndex > 0) {
        return;
    }
    AsyncRefinedHierarchyResult result{};
    if (!m_elementDetector.consumeRefinedResult(result)) {
        return;
    }
    m_hasAsyncRefinedActive = !result.candidates.empty() &&
        (result.candidates.front().kind == DetectedCandidateKind::UiaElement ||
         result.candidates.front().kind == DetectedCandidateKind::VisionRegion);
    applyDetectedCandidates(result.candidates, /*animate=*/true, hwnd);
    if (m_renderer) {
        m_renderer->invalidate();
    }
}

std::vector<RECT> CaptureInput::detectWindowHierarchy(POINT cursorPos) {
    auto candidates = m_elementDetector.querySync(cursorPos, nullptr);
    std::vector<RECT> rects;
    std::vector<std::wstring> descriptions;
    std::vector<float> radii;
    rects.reserve(candidates.size());
    descriptions.reserve(candidates.size());
    radii.reserve(candidates.size());
    for (const auto& cand : candidates) {
        rects.push_back(cand.rect);
        descriptions.push_back(cand.description);
        radii.push_back(cand.cornerRadius);
    }
    if (m_state) {
        m_state->detectedWindowHierarchyDescriptions = std::move(descriptions);
        m_state->detectedWindowHierarchyRadii = std::move(radii);
        if (!m_state->detectedWindowHierarchyRadii.empty()) {
            m_state->detectedWindowCornerRadius = m_state->detectedWindowHierarchyRadii.front();
        }
    }
    return rects;
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
            case WM_CAPTURE_ELEMENT_REFINED:
                return 0;
            default:
                break;
        }
    }

    // 窗口过程兜底：渲染/标注路径含 OpenCV 分配、std::format 等可抛操作，异常绝不能逃逸到
    // Win32 派发层（否则 std::terminate 崩溃）。
    try {
    switch (msg) {
        case WM_CAPTURE_ELEMENT_REFINED:
            handleAsyncElementRefined(hwnd);
            return 0;
        case WM_SETCURSOR:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONUP:
            return handleMouseMessage(hwnd, msg, wParam, lParam);
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_CHAR:
        case WM_CHAR:
        case WM_MOUSEWHEEL:
        case WM_SYSKEYUP:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_KEYDOWN:
        case WM_TIMER:
            return handleKeyAndWheelMessage(hwnd, msg, wParam, lParam);
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

} // namespace tools3000::capture
