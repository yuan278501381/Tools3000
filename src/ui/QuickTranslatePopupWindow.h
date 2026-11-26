#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// QuickTranslatePopupWindow — 全局划词与剪贴板轻量原生翻译浮窗
//
// 职责:
//   1. 响应全局「划词与剪贴板翻译」快捷键（默认 Alt+X），异步双源捕获：
//      优先抓取当前选区文本，未命中新选区时无缝回退读取剪贴板文本；
//   2. 100% 复用 core::translate::TranslationService 单一事实源（默认免密钥
//      Bilibili Index-Translate + Edge/Google 双源自动容灾 + BYOK/Ollama/DeepL）；
//   3. 零 WebView2 进程开销、毫秒级光标旁弹出、原生 Direct2D/DirectWrite ClearType
//      渲染、Per-Monitor DPI V2 自适应、深浅主题 + 全局强调色联动、可固定/拖拽/滚轮/
//      一键复制 (Ctrl+C) 与失焦收起后释放 D2D 资源。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_UI_QUICKTRANSLATEPOPUPWINDOW_H
#define TOOLS3000_UI_QUICKTRANSLATEPOPUPWINDOW_H

#include "core/translate/TranslationTypes.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace tools3000::ui {

class QuickTranslatePopupWindow {
public:
    enum class PopupState : uint8_t {
        Hidden = 0,
        Loading = 1,
        Ready = 2,
        Error = 3,
    };

    enum class HitButton : uint8_t {
        None = 0,
        Pin = 1,
        Close = 2,
        Copy = 3,
        Retry = 4,
        LangSwitch = 5,
    };

    static QuickTranslatePopupWindow& instance();

    bool initialize(HINSTANCE hInstance);
    void shutdown();

    /// 触发一次全局划词/剪贴板快速翻译（异步捕获 + 1ms 立即弹出加载卡片 + 异步翻译）
    void triggerQuickTranslate(POINT cursorPt, HWND foregroundHwnd, const std::string& hotkeyDisplay);

    /// 直接对给定文本执行翻译并展示浮窗（便于重试、快捷切换目标语种与测试）
    void translateTextAsync(
        const std::string& textUtf8,
        bool fromClipboard,
        POINT anchorPt,
        bool bypassCache = false,
        const std::string& targetLangOverride = {});

    /// 循环切换快捷目标语种（"" 自动 -> "zh-CN" -> "en" -> "ja" -> ""）
    static std::string nextQuickTargetLanguage(std::string_view currentOverride) noexcept;

    /// 显式切换深浅主题/强调色时同步刷新可见浮窗
    void reloadTheme();

    /// 立即隐藏浮窗并释放 D2D 设备资源
    void hide();

    bool isVisible() const noexcept;
    HWND hwnd() const noexcept { return m_hwnd.load(std::memory_order_acquire); }
    PopupState state() const noexcept { return m_state.load(std::memory_order_acquire); }

    // ── 纯静态策略与布局函数（100% 可离线单元测试） ────────────────────────────

    /// 清理首尾空白并按 UTF-16 安全码点上限截断（不劈裂代理对）
    static std::string sanitizeAndTrimInput(std::string_view rawUtf8, size_t maxCodePoints = 2000);

    /// 计算光标附近多屏工作区安全定位（避让任务栏与屏幕边缘，空间不足自动翻转）
    static RECT computePopupPlacement(POINT anchorPt, int popupW, int popupH, const RECT& workArea) noexcept;

    /// 格式化当前引擎名称标签（用于顶栏胶囊）
    static std::string formatProviderDisplayLabel(
        const tools3000::core::translate::TranslationSettings& settings,
        std::string_view providerUsed = {});

    /// 格式化源/目标语种方向胶囊（如 "EN -> ZH" / "AUTO -> ZH"）
    static std::string formatDirectionBadge(std::string_view detectedLang, std::string_view targetLang);

private:
    QuickTranslatePopupWindow() = default;
    ~QuickTranslatePopupWindow();
    QuickTranslatePopupWindow(const QuickTranslatePopupWindow&) = delete;
    QuickTranslatePopupWindow& operator=(const QuickTranslatePopupWindow&) = delete;

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void presentLoadingOnUiThread(
        uint64_t requestId,
        const std::string& sourceText,
        bool fromClipboard,
        const std::string& providerLabel,
        const std::string& directionLabel,
        POINT anchorPt);
    void presentResultOnUiThread(
        uint64_t requestId,
        const std::string& translatedText,
        const std::string& directionLabel,
        const std::string& providerLabel);
    void presentErrorOnUiThread(
        uint64_t requestId,
        const std::string& errorMessage,
        const std::string& providerLabel);

    void cancelActiveWorkerLocked();
    void ensureWindowCreated();
    bool createDeviceResources();
    void discardDeviceResources();
    void rebuildTextFormats();
    void rebuildLayoutsAndResize(bool repositionAtAnchor);
    void render();
    HitButton hitTestButton(POINT clientPt) const noexcept;
    void copyTranslationToClipboard();
    void triggerRetry();
    void cycleTargetLanguage();

    HINSTANCE m_hInstance = nullptr;
    std::atomic<HWND> m_hwnd{nullptr};
    HWND m_helperOwnerHwnd = nullptr;
    std::atomic<PopupState> m_state{PopupState::Hidden};
    std::atomic<uint64_t> m_requestSeq{0};
    std::atomic<bool> m_shuttingDown{false};

    // 后台异步捕获与翻译任务管理
    std::mutex m_workerMutex;
    std::shared_ptr<std::atomic<bool>> m_activeCancelFlag;
    std::jthread m_workerThread;

    // UI 展示数据（仅在 UI 线程读写）
    uint64_t m_activeUiRequestId = 0;
    std::string m_sourceTextUtf8;
    std::string m_translatedTextUtf8;
    std::string m_errorTextUtf8;
    std::string m_providerLabelUtf8;
    std::string m_directionLabelUtf8;
    std::string m_targetLangOverride;
    bool m_fromClipboard = false;
    bool m_pinned = false;
    bool m_copiedFeedbackActive = false;
    int m_loadingFrame = 0;
    POINT m_anchorPt{0, 0};
    float m_dpiScale = 1.0f;
    int m_clientW = 420;
    int m_clientH = 220;
    float m_scrollY = 0.0f;
    float m_maxScrollY = 0.0f;
    HitButton m_hoveredButton = HitButton::None;
    bool m_trackingMouseLeave = false;

    // 按钮命中区（DIP）
    D2D1_RECT_F m_headerDragRect{};
    D2D1_RECT_F m_dirBadgeRect{};
    D2D1_RECT_F m_pinBtnRect{};
    D2D1_RECT_F m_closeBtnRect{};
    D2D1_RECT_F m_copyBtnRect{};
    D2D1_RECT_F m_retryBtnRect{};
    D2D1_RECT_F m_bodyViewportRect{};
    float m_bodyContentHeight = 0.0f;

    // Direct2D / DirectWrite 资源
    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_badgeFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_sourceFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_bodyFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_btnFormat;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> m_sourceLayout;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> m_bodyLayout;
};

}  // namespace tools3000::ui

#endif  // TOOLS3000_UI_QUICKTRANSLATEPOPUPWINDOW_H
