#pragma once

#include "core/utils/DpiUtils.h"

#include <windows.h>

namespace tools3000::ui {

struct SearchWindowStyle {
    static constexpr int BaseWidth = 800;
    static constexpr int BaseHeight = 600;
    static constexpr int BaseScreenMargin = 24;

    static SIZE windowSizeForDpi(unsigned dpi) noexcept {
        const float scale = tools3000::core::dpi::scaleForDpi(dpi);
        return {tools3000::core::dpi::scaleMetric(BaseWidth, scale),
                tools3000::core::dpi::scaleMetric(BaseHeight, scale)};
    }
};

struct TrayWindowStyle {
    static constexpr int BaseWidth = 276;
    static constexpr int BaseHeight = 388;
    static constexpr int BaseScreenMargin = 10;

    static SIZE windowSizeForDpi(unsigned dpi) noexcept {
        const float scale = tools3000::core::dpi::scaleForDpi(dpi);
        return {tools3000::core::dpi::scaleMetric(BaseWidth, scale),
                tools3000::core::dpi::scaleMetric(BaseHeight, scale)};
    }
};

struct SettingsWindowStyle {
    static constexpr int BaseWidth = 1100;
    static constexpr int BaseHeight = 750;
    static constexpr int BaseMinWidth = 680;
    static constexpr int BaseMinHeight = 460;
    static constexpr int BaseScreenMargin = 24;

    static SIZE windowSizeForDpi(unsigned dpi) noexcept {
        const float scale = tools3000::core::dpi::scaleForDpi(dpi);
        return {tools3000::core::dpi::scaleMetric(BaseWidth, scale),
                tools3000::core::dpi::scaleMetric(BaseHeight, scale)};
    }

    static SIZE minWindowSizeForDpi(unsigned dpi) noexcept {
        const float scale = tools3000::core::dpi::scaleForDpi(dpi);
        return {tools3000::core::dpi::scaleMetric(BaseMinWidth, scale),
                tools3000::core::dpi::scaleMetric(BaseMinHeight, scale)};
    }
};

}  // namespace tools3000::ui
