#pragma once

#include "core/utils/DpiUtils.h"
#include <cstddef>

namespace tools3000::capture {

/// Centralized visual metrics for the contextual shortcut guide. Values are
/// expressed in 96-DPI logical pixels, then scaled explicitly because the
/// overlay's D2D DC render target is configured in physical-pixel space.
struct ShortcutHintStyle {
    static constexpr float BaseKeyFont = 15.0f;
    static constexpr float BaseLabelFont = 15.5f;
    static constexpr float BaseHeaderFont = 15.0f;
    static constexpr float BaseBadgeFont = 12.0f;
    static constexpr float BaseKeyHeight = 32.0f;
    static constexpr float BaseKeyMinWidth = 50.0f;
    static constexpr float BaseHeaderHeight = 28.0f;
    static constexpr float BaseHeaderBottomGap = 14.0f;
    static constexpr float BaseHorizontalPadding = 20.0f;
    static constexpr float BaseVerticalPadding = 18.0f;
    static constexpr float BaseKeyHorizontalPadding = 16.0f;
    static constexpr float BaseItemGap = 16.0f;
    static constexpr float BaseLabelGap = 16.0f;
    static constexpr float BaseRowGap = 12.0f;
    static constexpr float BaseColumnGap = 30.0f;
    static constexpr float BaseCornerRadius = 16.0f;
    static constexpr float BaseKeyCornerRadius = 8.0f;
    static constexpr float BaseScreenMargin = 24.0f;
    static constexpr float BaseShadowPadding = 12.0f;
    static constexpr std::size_t TwoColumnMinItems = 5;

    static float scaleForDpi(unsigned dpi) noexcept {
        return tools3000::core::dpi::scaleForDpi(dpi);
    }
};

}  // namespace tools3000::capture
