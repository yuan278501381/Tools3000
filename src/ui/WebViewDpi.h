#pragma once

// WebView2 默认把 Bounds 当成 DIP，再乘 RasterizationScale。
// 本进程是 Per-Monitor V2，GetClientRect 已是物理像素；若不改成 USE_RAW_PIXELS，
// 150%/200% 会二次放大，设置/搜索/托盘看起来又糊又挤。

#include "core/utils/DpiUtils.h"

#include <WebView2.h>
#include <wrl/client.h>

namespace tools3000::ui {

inline bool syncWebViewDpi(ICoreWebView2Controller* controller, HWND hwnd) noexcept {
    if (!controller || !hwnd || !IsWindow(hwnd)) return false;

    Microsoft::WRL::ComPtr<ICoreWebView2Controller3> controller3;
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller3))) && controller3) {
        const double scale = tools3000::core::dpi::rasterizationScaleForDpi(
            tools3000::core::dpi::effectiveDpiForWindow(hwnd));
        controller3->put_ShouldDetectMonitorScaleChanges(TRUE);
        controller3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
        controller3->put_RasterizationScale(scale);
    }

    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    controller->put_Bounds(bounds);
    return true;
}

}  // namespace tools3000::ui
