#pragma once

#include "core/utils/DpiUtils.h"

namespace tools3000::keycast {

struct KeycastStyle {
    static constexpr int BaseWidth = 800;
    static constexpr int BaseHeight = 160;
    static constexpr int BaseBottomMargin = 64;
    static constexpr float BaseFontSize = 36.0f;

    static SIZE windowSizeForDpi(UINT dpi) noexcept {
        const float scale = tools3000::core::dpi::scaleForDpi(dpi);
        return SIZE{
            tools3000::core::dpi::scaleMetric(BaseWidth, scale),
            tools3000::core::dpi::scaleMetric(BaseHeight, scale)};
    }
};

}  // namespace tools3000::keycast
