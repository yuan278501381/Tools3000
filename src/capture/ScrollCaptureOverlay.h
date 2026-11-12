#pragma once
#include "capture/ScrollCapture.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/utils/DWriteFontUtils.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <opencv2/core.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>

namespace tools3000::capture {

namespace ScrollCaptureHotkeyIds {
    inline constexpr int kTogglePause = 0x7A01;
    inline constexpr int kConfirm     = 0x7A02;
    inline constexpr int kPin         = 0x7A03;
    inline constexpr int kSaveAs      = 0x7A04;
    inline constexpr int kCancel      = 0x7A05;
    inline constexpr int kCopy        = 0x7A06;
} // namespace ScrollCaptureHotkeyIds

struct ScrollHotkeySpec {
    int id = 0;
    std::uint32_t modifiers = 0;
    UINT vk = 0;
    const char* name = "";
    ScrollFinishAction finishAction = ScrollFinishAction::Confirm;
    bool isTogglePause = false;
};

struct ScrollControlBarLayoutSnapshot {
    int barWidth = 0;
    int barHeight = 0;
    float dividerX = 0.0f;
    std::array<D2D1_RECT_F, 5> buttonRects{};
    std::array<std::wstring, 5> keycaps{};
    std::array<std::wstring, 5> labels{};
    std::array<float, 5> labelMeasuredWidths{};
    std::array<float, 5> keyPillWidths{};
    int compactLevel = 0;
};

class ScrollCaptureOverlay {
public:
    static ScrollCaptureOverlay& instance() {
        static ScrollCaptureOverlay inst;
        return inst;
    }

    bool initialize();
    void show(RECT captureRect);
    void hide();
    void reloadTheme();

    /// 更新最近滚动预览缩略图并触发边框脉冲反馈
    void updatePreview(const cv::Mat& stitched, int frameCount);

    /// 判断屏幕坐标是否位于悬浮控制台热区（供自动滚动循环避让光标与滚轮）
    bool isPointInControlZone(POINT screenPt) const;

    /// 获取长截图瞬态热键规格（SSOT：移除裸单键 P/S，升级为 Ctrl+T / Ctrl+S / Ctrl+C，暂停态释放 Space）
    static inline std::vector<ScrollHotkeySpec> transientHotkeySpecs(bool autoScrollingActive) {
        using tools3000::core::ModKey;
        std::vector<ScrollHotkeySpec> specs;
        specs.reserve(6);
        if (autoScrollingActive) {
            specs.push_back({ScrollCaptureHotkeyIds::kTogglePause, static_cast<std::uint32_t>(ModKey::None), VK_SPACE,
                             "ScrollCapture.TogglePause", ScrollFinishAction::Confirm, true});
        }
        specs.push_back({ScrollCaptureHotkeyIds::kConfirm, static_cast<std::uint32_t>(ModKey::None), VK_RETURN,
                         "ScrollCapture.Confirm", ScrollFinishAction::Confirm, false});
        specs.push_back({ScrollCaptureHotkeyIds::kPin, static_cast<std::uint32_t>(ModKey::Ctrl), 'T',
                         "ScrollCapture.Pin", ScrollFinishAction::Pin, false});
        specs.push_back({ScrollCaptureHotkeyIds::kSaveAs, static_cast<std::uint32_t>(ModKey::Ctrl), 'S',
                         "ScrollCapture.SaveAs", ScrollFinishAction::SaveAs, false});
        specs.push_back({ScrollCaptureHotkeyIds::kCopy, static_cast<std::uint32_t>(ModKey::Ctrl), 'C',
                         "ScrollCapture.Copy", ScrollFinishAction::Copy, false});
        specs.push_back({ScrollCaptureHotkeyIds::kCancel, static_cast<std::uint32_t>(ModKey::None), VK_ESCAPE,
                         "ScrollCapture.Cancel", ScrollFinishAction::Cancelled, false});
        return specs;
    }

    /// 计算悬浮控制台按钮动态布局快照（供测试直接验证任意 DPI、中英文与窄屏紧凑降级下按钮不重叠、不越界）
    ScrollControlBarLayoutSnapshot computeControlBarLayoutForTest(
        float dpiScale, bool isChinese, bool isPaused, int workWidth = 0);

    /// 单一事实源控制栏自适应布局计算（支持传入已有 DWrite 格式或按需自建探测格式；workWidth > 0 时启用两级紧凑降级）
    static inline ScrollControlBarLayoutSnapshot buildControlBarLayout(
        float dpiScale,
        bool isChinese,
        bool isPausedOrHold,
        int workWidth = 0,
        IDWriteFactory* externalFactory = nullptr,
        IDWriteTextFormat* externalStatusFmt = nullptr,
        IDWriteTextFormat* externalMetricsFmt = nullptr,
        IDWriteTextFormat* externalBtnLabelFmt = nullptr,
        IDWriteTextFormat* externalKeycapFmt = nullptr,
        const std::wstring& liveStatusText = L"",
        const std::wstring& liveMetricsText = L"") {
        const float s = (dpiScale > 0.0f) ? dpiScale : 1.0f;

        Microsoft::WRL::ComPtr<IDWriteFactory> localFactory;
        IDWriteFactory* dw = externalFactory;
        if (!dw) {
            if (SUCCEEDED(DWriteCreateFactory(
                    DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(localFactory.GetAddressOf())))) {
                dw = localFactory.Get();
            }
        }

        Microsoft::WRL::ComPtr<IDWriteTextFormat> localStatusFmt;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> localMetricsFmt;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> localBtnLabelFmt;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> localKeycapFmt;

        const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(dw);
        const wchar_t* monoFont = tools3000::core::resolveInstalledMonoFontFamily(dw);
        const wchar_t* localeName = isChinese ? L"zh-CN" : L"en-US";

        auto ensureFmt = [&](IDWriteTextFormat* ext, Microsoft::WRL::ComPtr<IDWriteTextFormat>& slot,
                             const wchar_t* family, DWRITE_FONT_WEIGHT weight, float px) -> IDWriteTextFormat* {
            if (ext) return ext;
            if (dw && SUCCEEDED(dw->CreateTextFormat(
                    family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, px, localeName, slot.GetAddressOf())) && slot) {
                slot->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                return slot.Get();
            }
            return nullptr;
        };

        IDWriteTextFormat* statusFmt   = ensureFmt(externalStatusFmt, localStatusFmt, uiFont, DWRITE_FONT_WEIGHT_SEMI_BOLD, (std::max)(12.0f, 12.6f * s));
        IDWriteTextFormat* metricsFmt  = ensureFmt(externalMetricsFmt, localMetricsFmt, uiFont, DWRITE_FONT_WEIGHT_SEMI_BOLD, (std::max)(12.0f, 12.0f * s));
        IDWriteTextFormat* btnLabelFmt = ensureFmt(externalBtnLabelFmt, localBtnLabelFmt, uiFont, DWRITE_FONT_WEIGHT_SEMI_BOLD, (std::max)(12.0f, 12.4f * s));
        IDWriteTextFormat* keycapFmt   = ensureFmt(externalKeycapFmt, localKeycapFmt, monoFont, DWRITE_FONT_WEIGHT_BOLD, (std::max)(12.0f, 12.0f * s));

        auto measureW = [&](const std::wstring& text, IDWriteTextFormat* fmt) -> float {
            if (text.empty()) return 0.0f;
            if (dw && fmt) {
                Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(dw->CreateTextLayout(
                        text.c_str(), static_cast<UINT32>(text.size()), fmt,
                        4096.0f * (std::max)(1.0f, s), 256.0f * (std::max)(1.0f, s), &layout)) && layout) {
                    DWRITE_TEXT_METRICS tm{};
                    if (SUCCEEDED(layout->GetMetrics(&tm))) {
                        return std::ceil(tm.widthIncludingTrailingWhitespace);
                    }
                }
            }
            return static_cast<float>(text.size()) * 13.0f * s;
        };

        const float leftX = 14.0f * s;
        const float dotR = 4.2f * s;
        const float dotGap = 7.0f * s;

        float maxStatusW = (std::max)(
            measureW(isChinese ? L"正在自动滚动采集" : L"Auto scrolling", statusFmt),
            measureW(isChinese ? L"已暂停 · 可手滚补截" : L"Stopped · Wheel ready", statusFmt));
        if (!liveStatusText.empty()) {
            maxStatusW = (std::max)(maxStatusW, measureW(liveStatusText, statusFmt));
        }

        float maxMetricsW = measureW(
            isChinese ? L"99 帧 · 3840 × 9999 px" : L"99 f · 3840 × 9999 px", metricsFmt);
        if (!liveMetricsText.empty()) {
            maxMetricsW = (std::max)(maxMetricsW, measureW(liveMetricsText, metricsFmt));
        }

        const float leftContentW = (std::max)(dotR * 2.0f + dotGap + maxStatusW, maxMetricsW) + 4.0f * s;
        float dividerX = leftX + leftContentW + 12.0f * s;

        const float padL = 9.5f * s;
        const float iconSize = 15.0f * s;
        const float gapIconLabel = 5.5f * s;
        const float gapLabelKey = 7.5f * s;
        const float gapIconKey = 6.5f * s;
        const float padR = 8.5f * s;
        const float btnGap = 5.5f * s;

        struct BtnSpec {
            std::wstring label;
            std::wstring altLabel;
            std::wstring keycap;
            float labelW = 0.0f;
            float keyW = 0.0f;
            float totalW = 0.0f;
        };

        std::array<BtnSpec, 5> specs = {{
            {isPausedOrHold ? (isChinese ? L"继续" : L"Resume") : (isChinese ? L"暂停" : L"Pause"),
             isPausedOrHold ? (isChinese ? L"暂停" : L"Pause") : (isChinese ? L"继续" : L"Resume"),
             L"Space"},
            {isChinese ? L"贴图" : L"Pin", L"", L"Ctrl+T"},
            {isChinese ? L"保存" : L"Save", L"", L"Ctrl+S"},
            {L"", L"", L"Esc"},
            {isChinese ? L"完成" : L"Done", L"", L"Enter"}
        }};

        auto evaluateSpecs = [&](float curDividerX) -> int {
            float sumBtnsW = 0.0f;
            for (auto& sp : specs) {
                sp.labelW = sp.label.empty()
                    ? 0.0f
                    : (std::max)(measureW(sp.label, btnLabelFmt), measureW(sp.altLabel, btnLabelFmt)) + 2.0f * s;
                sp.keyW = sp.keycap.empty()
                    ? 0.0f
                    : (std::max)(22.0f * s, measureW(sp.keycap, keycapFmt) + 12.0f * s);
                float tw = 0.0f;
                if (!sp.label.empty() && !sp.keycap.empty()) {
                    tw = padL + iconSize + gapIconLabel + sp.labelW + gapLabelKey + sp.keyW + padR;
                } else if (!sp.label.empty()) {
                    tw = padL + iconSize + gapIconLabel + sp.labelW + padR;
                } else if (!sp.keycap.empty()) {
                    tw = padL + iconSize + gapIconKey + sp.keyW + padR;
                } else {
                    tw = padL + iconSize + padL;
                }
                sp.totalW = std::ceil(tw);
                sumBtnsW += sp.totalW;
            }
            return static_cast<int>(std::ceil(
                curDividerX + 10.0f * s + sumBtnsW + static_cast<float>(specs.size() - 1) * btnGap + 12.0f * s));
        };

        int compactLevel = 0;
        int totalBarW = evaluateSpecs(dividerX);

        if (workWidth > 0) {
            const int edge = static_cast<int>(std::lround(10.0f * s));
            const int maxBarWidth = (std::max)(static_cast<int>(std::lround(220.0f * s)), workWidth - edge * 2);

            // 第一级紧凑降级：当全量宽度超出屏幕可用宽度时，隐藏所有按钮右侧内嵌快捷键键帽胶囊
            if (totalBarW > maxBarWidth) {
                compactLevel = 1;
                for (auto& sp : specs) {
                    sp.keycap.clear();
                }
                totalBarW = evaluateSpecs(dividerX);
            }

            // 第二级紧凑降级：若仍超出可用宽度，将贴图 (Pin) 与保存 (SaveAs) 收敛为纯图标紧凑按钮
            if (totalBarW > maxBarWidth) {
                compactLevel = 2;
                specs[1].label.clear();
                specs[1].altLabel.clear();
                specs[2].label.clear();
                specs[2].altLabel.clear();
                totalBarW = evaluateSpecs(dividerX);
            }

            // 超窄竖屏兜底保护：适度收紧左侧状态区宽度（配合省略号裁剪），必要时将暂停按钮也收敛为纯图标，确保右侧取消/完成 100% 留在屏幕内
            if (totalBarW > maxBarWidth) {
                const float overflow = static_cast<float>(totalBarW - maxBarWidth);
                dividerX = (std::max)(leftX + 72.0f * s, dividerX - overflow);
                totalBarW = evaluateSpecs(dividerX);
                if (totalBarW > maxBarWidth) {
                    specs[0].label.clear();
                    specs[0].altLabel.clear();
                    totalBarW = evaluateSpecs(dividerX);
                }
            }
        }

        ScrollControlBarLayoutSnapshot snap{};
        snap.barWidth = totalBarW;
        snap.barHeight = static_cast<int>(std::lround(54.0f * s));
        snap.dividerX = dividerX;
        snap.compactLevel = compactLevel;

        float curX = dividerX + 10.0f * s;
        const float btnTop = 8.0f * s;
        const float btnBot = static_cast<float>(snap.barHeight) - 8.0f * s;
        for (std::size_t i = 0; i < specs.size(); ++i) {
            snap.buttonRects[i] = D2D1::RectF(curX, btnTop, curX + specs[i].totalW, btnBot);
            snap.keycaps[i] = specs[i].keycap;
            snap.labels[i] = specs[i].label;
            snap.labelMeasuredWidths[i] = specs[i].labelW;
            snap.keyPillWidths[i] = specs[i].keyW;
            curX += specs[i].totalW + btnGap;
        }
        return snap;
    }

private:
    enum class ControlButtonId : int {
        None = -1,
        TogglePause = 0,
        Pin = 1,
        SaveAs = 2,
        Cancel = 3,
        Confirm = 4,
        Count = 5
    };

    struct ControlButtonRect {
        ControlButtonId id = ControlButtonId::None;
        D2D1_RECT_F rect{};
    };

    struct LayeredDibCache {
        HDC memDc = nullptr;
        HBITMAP dib = nullptr;
        HGDIOBJ oldBmp = nullptr;
        void* bits = nullptr;
        int width = 0;
        int height = 0;
    };

    ScrollCaptureOverlay() = default;
    ~ScrollCaptureOverlay() { hide(); }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK controlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool createResources();
    static bool ensureLayeredDib(HDC screenDc, int width, int height, LayeredDibCache& cache) noexcept;
    static void releaseLayeredDib(LayeredDibCache& cache) noexcept;
    void handleMainTargetDeviceLost() noexcept;
    void handleControlTargetDeviceLost() noexcept;
    float measureTextWidth(const std::wstring& text, IDWriteTextFormat* fmt) const;
    ScrollControlBarLayoutSnapshot rebuildLayout(
        int workWidth, const std::wstring& statusText, const std::wstring& metricsStr, bool isPausedOrHold);
    void render();
    void updateControlPlacement(const RECT* thumbScreenRect);
    void renderControlBar(bool forceRedraw = false);
    ControlButtonId hitTestControlButton(POINT clientPt) const;
    void executeControlAction(ControlButtonId id);
    void registerTransientHotkeys();
    void unregisterTransientHotkeys();
    void syncPauseHotkeyWithRuntimeState();
    void applyTransientOverlayShield(bool active);

    HWND m_hwnd = nullptr;
    HWND m_controlHwnd = nullptr;
    RECT m_captureRect = {};
    mutable std::mutex m_controlRectMutex;
    RECT m_controlScreenRect = {};
    RECT m_lastThumbScreenRect = {};
    bool m_hasThumbRect = false;
    int m_desiredBarWidth = 0;
    int m_desiredBarHeight = 0;

    std::mutex m_mutex;
    cv::Mat m_preview;
    bool m_previewDirty = false;
    int m_frameCount = 0;
    float m_dpiScale = 1.0f;
    bool m_isChinese = true;
    bool m_hotkeysRegistered = false;
    bool m_spaceHotkeyRegistered = false;
    bool m_overlayVisibleFlag = false;

    ControlButtonId m_hoveredBtn = ControlButtonId::None;
    ControlButtonId m_pressedBtn = ControlButtonId::None;
    bool m_trackingMouseLeave = false;
    std::array<ControlButtonRect, 5> m_buttonRects{};

    ScrollControlBarLayoutSnapshot m_cachedLayoutSnap{};
    bool m_layoutCacheValid = false;
    int m_cachedLayoutWorkWidth = -1;
    float m_cachedLayoutScale = 0.0f;
    bool m_cachedLayoutChinese = false;
    bool m_cachedLayoutPausedOrHold = false;
    std::wstring m_cachedLayoutStatus;
    std::wstring m_cachedLayoutMetrics;

    bool m_controlBarDirty = true;
    ScrollRuntimeState m_lastRenderedState = ScrollRuntimeState::Idle;
    int m_lastRenderedWorkWidth = -1;
    int m_lastRenderedPulseStep = -1;
    std::wstring m_lastRenderedStatus;
    std::wstring m_lastRenderedMetrics;
    ControlButtonId m_lastRenderedHoveredBtn = ControlButtonId::None;
    ControlButtonId m_lastRenderedPressedBtn = ControlButtonId::None;

    uint64_t m_lastFlashTime = 0;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_controlRenderTarget;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> m_roundStrokeStyle;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_statusFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_metricsFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_btnLabelFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_keycapFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushFlash;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brushBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlPanelBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlPanelBorder;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlPanelInnerHighlight;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlDividerBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlTextPrimary;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlTextSecondary;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlTextOnPrimary;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlDotBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlBtnHoverBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlBtnPressBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlKeycapBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlKeycapPrimaryBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlAccentBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlAccentHoverBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlDangerHoverBg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_ctrlDangerIconBrush;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_d2dBitmap;
    LayeredDibCache m_overlayDibCache{};
    LayeredDibCache m_controlDibCache{};
};

} // namespace tools3000::capture
