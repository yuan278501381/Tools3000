#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <string>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include "ui/WebViewSuspend.h"

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;
struct ICoreWebView2WebResourceRequestedEventArgs;

namespace tools3000::ui {

class QuickLookWindow {
public:
    static QuickLookWindow& instance();

    void show(const std::wstring& filePath, HINSTANCE hInstance = nullptr);
    void previewFile(const std::wstring& filePath);
    void notifyFrontendReady();
    void requestFallbackMediaAsync(const std::wstring& filePath, const std::string& mediaKind);
    void stepSiblingAsync(int delta);
    void hide();
    bool isVisible() const;
    void destroy();

    std::wstring currentFilePath() const { return m_currentFilePath; }
    HWND getHwnd() const { return m_hwnd; }
    bool isWebViewReady() const noexcept { return m_webViewReady.load(std::memory_order_acquire); }
    bool isPdfPreviewActive() const noexcept { return m_currentIsPdfPreview.load(std::memory_order_acquire); }

private:
    QuickLookWindow() = default;
    ~QuickLookWindow();

    bool createWindow(HINSTANCE hInstance);
    void initializeWebView2();
    void updatePlacement();
    std::string bindActiveMediaUrl(const std::wstring& filePath, uintmax_t sizeBytes, int faceIndex = -1);
    void clearActiveMediaBinding();
    void handleMediaResourceRequested(ICoreWebView2WebResourceRequestedEventArgs* args);
    void ensureAsyncWorkerStarted();
    void shutdownAsyncWorker();
    void asyncWorkerLoop(std::stop_token st);

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    // 隐藏后 60 秒自动销毁 QuickLook WebView2 与后台异步线程，归还第 4 个渲染进程内存
    static constexpr UINT_PTR IDT_IDLE_DESTROY = 1002;
    static constexpr UINT IDLE_DESTROY_TIMEOUT_MS = 60000;

    HWND m_hwnd = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;
    WebViewSuspendController m_suspendController;

    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_currentIsPdfPreview{false};
    std::atomic<bool> m_webViewReady{false};
    bool m_frontendReady = false;
    uint64_t m_showTimeTick{0};
    uint64_t m_mediaUrlRevision{0};
    std::atomic<uint64_t> m_generation{0};
    std::atomic<uint64_t> m_previewRequestId{0};
    std::wstring m_currentFilePath;
    std::wstring m_pendingFilePath;
    std::wstring m_lastDispatchedPath;
    uint64_t m_lastDispatchedGen = 0;

    std::mutex m_mediaBindingMutex;
    std::wstring m_activeMediaPath;
    std::string m_activeMediaToken;

    std::mutex m_asyncMutex;
    std::condition_variable_any m_asyncCv;
    std::wstring m_asyncPreviewPath;
    uint64_t m_asyncPreviewReqId = 0;
    uint64_t m_asyncPreviewGen = 0;
    bool m_asyncDeepDecode = false;
    bool m_hasAsyncPreviewTask = false;
    std::wstring m_asyncFallbackPath;
    std::string m_asyncFallbackKind;
    uint64_t m_asyncFallbackReqId = 0;
    uint64_t m_asyncFallbackGen = 0;
    bool m_hasAsyncFallbackTask = false;
    std::wstring m_asyncStepBase;
    int m_asyncStepDelta = 0;
    uint64_t m_asyncStepGen = 0;
    bool m_hasAsyncStepTask = false;
    std::jthread m_asyncWorker;
};

} // namespace tools3000::ui
