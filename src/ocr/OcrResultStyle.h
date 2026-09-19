#pragma once

#include "core/utils/DpiUtils.h"

#include <windows.h>

namespace tools3000::ocr {

// Logical (96-DPI) measurements for the native OCR result surface. Keeping
// these values in a pure header makes mixed-DPI behavior testable without
// creating a window or depending on the current machine's display settings.
struct OcrResultStyle {
    static constexpr int BaseWidth = 600;
    static constexpr int BaseHeight = 400;
    static constexpr int BaseScreenMargin = 24;
    static constexpr int BaseMinWidth = 420;
    static constexpr int BaseMinHeight = 280;

    static constexpr float BaseCornerRadius = 14.0f;
    static constexpr float BaseBorderWidth = 1.0f;
    static constexpr float BasePadding = 22.0f;
    static constexpr float BaseHeaderHeight = 48.0f;
    static constexpr float BaseFooterHeight = 62.0f;
    static constexpr float BaseButtonWidth = 106.0f;
    static constexpr float BaseCloseButtonWidth = 82.0f;
    static constexpr float BaseButtonHeight = 36.0f;
    static constexpr float BaseButtonGap = 10.0f;
    static constexpr float BaseTitleFont = 17.0f;
    static constexpr float BaseBodyFont = 16.0f;
    static constexpr float BaseButtonFont = 13.0f;
    static constexpr float BaseHintFont = 12.0f;
    static constexpr float BaseLineScroll = 42.0f;

    static SIZE windowSizeForDpi(unsigned dpi) noexcept {
        const float scale = tools3000::core::dpi::scaleForDpi(dpi);
        return {
            tools3000::core::dpi::scaleMetric(BaseWidth, scale),
            tools3000::core::dpi::scaleMetric(BaseHeight, scale),
        };
    }
};

}  // namespace tools3000::ocr
