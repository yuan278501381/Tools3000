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

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;

namespace tools3000::ui {

class QuickLookWindow {
public:
    static QuickLookWindow& instance();

    void show(const std::wstring& filePath, HINSTANCE hInstance = nullptr);
    void previewFile(const std::wstring& filePath);
    void stepSiblingAsync(int delta);
    void hide();
    bool isVisible() const;
    void destroy();

    std::wstring currentFilePath() const { return m_currentFilePath; }
    HWND getHwnd() const { return m_hwnd; }
    bool isWebViewReady() const noexcept { return m_webViewReady; }
    bool isPdfPreviewActive() const noexcept { return m_currentIsPdfPreview.load(std::memory_order_acquire); }

private:
    QuickLookWindow() = default;
    ~QuickLookWindow();

    bool createWindow(HINSTANCE hInstance);
    void initializeWebView2();
    void updatePlacement();
    void updateMediaHostMapping(const std::wstring& folderPath);
    void clearMediaHostMapping();
    void ensureAsyncWorkerStarted();
    void shutdownAsyncWorker();
    void asyncWorkerLoop(std::stop_token st);

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;

    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_currentIsPdfPreview{false};
    bool m_webViewReady = false;
    uint64_t m_showTimeTick{0};
    uint64_t m_mediaUrlRevision{0};
    std::atomic<uint64_t> m_generation{0};
    std::atomic<uint64_t> m_previewRequestId{0};
    std::wstring m_currentFilePath;
    std::wstring m_pendingFilePath;
    std::wstring m_mappedMediaFolder;

    std::mutex m_asyncMutex;
    std::condition_variable_any m_asyncCv;
    std::wstring m_asyncPreviewPath;
    uint64_t m_asyncPreviewReqId = 0;
    uint64_t m_asyncPreviewGen = 0;
    bool m_asyncDeepDecode = false;
    bool m_hasAsyncPreviewTask = false;
    std::wstring m_asyncStepBase;
    int m_asyncStepDelta = 0;
    uint64_t m_asyncStepGen = 0;
    bool m_hasAsyncStepTask = false;
    std::jthread m_asyncWorker;
};

} // namespace tools3000::ui
