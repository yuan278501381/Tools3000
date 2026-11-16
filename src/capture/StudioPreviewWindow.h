#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// StudioPreviewWindow.h — 录屏结束即时弹出的 Screen Studio 级原生演播室预览与导出窗口
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_STUDIOPREVIEWWINDOW_H
#define TOOLS3000_CAPTURE_STUDIOPREVIEWWINDOW_H

#include "capture/StudioTimeline.h"
#include "capture/StudioVideoCompositor.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tools3000::capture {

class StudioPreviewWindow {
public:
    using CompletionHandler = std::function<void(const std::string& savedPath, double durationSec, bool studioExported)>;

    static StudioPreviewWindow& instance();

    /// 在 UI 主线程打开演播室实时预览窗口
    bool showSession(
        const std::string& rawVideoPathUtf8,
        StudioTimelineData timeline,
        const StudioRenderConfig& initialConfig,
        int bitrateMbps,
        CompletionHandler onFinished = nullptr);

    /// 关闭窗口并安全回收后台导出线程与预览帧工作集
    void shutdown();

    bool isVisible() const noexcept;

private:
    StudioPreviewWindow() = default;
    ~StudioPreviewWindow() { shutdown(); }
    StudioPreviewWindow(const StudioPreviewWindow&) = delete;
    StudioPreviewWindow& operator=(const StudioPreviewWindow&) = delete;

    struct HitZone {
        int id = 0;
        D2D1_RECT_F rect{};
        int payload = -1;
    };

    bool ensureWindowCreated();
    bool ensureRenderResources();
    void discardDeviceResources();
    void rebakeTimelinePreview();
    void updateCurrentComposedBitmap();
    void render();
    void onPlaybackTick();
    void startAsyncExport();
    void keepOriginalAndClose();
    void closeSessionAndCleanup();
    void joinExportWorkerIfDone(bool wait);
    void handleMouseDown(float lx, float ly);
    void handleMouseMove(float lx, float ly, bool leftButtonDown);
    void handleMouseUp();

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    float m_dpiScale = 1.0f;
    int m_logicalWidth = 980;
    int m_logicalHeight = 620;

    std::string m_rawVideoPath;
    StudioTimelineData m_timeline;
    StudioRenderConfig m_config;
    int m_bitrateMbps = 10;
    double m_durationSec = 0.0;
    CompletionHandler m_onFinished;

    // 轻量预览帧牌组与预烘焙时间轴状态
    std::vector<cv::Mat> m_previewDeckBgr;
    int m_origWidth = 0;
    int m_origHeight = 0;
    int m_previewEvalFps = 30;
    std::vector<StudioEvaluatedFrame> m_evaluatedFrames;
    StudioVideoCompositor m_previewCompositor;
    cv::Mat m_currentComposedBgra;
    bool m_composedDirty = true;

    // 播放与时间轴交互状态
    bool m_playing = true;
    double m_playheadSec = 0.0;
    DWORD m_lastTickMs = 0;
    bool m_scrubbingTimeline = false;
    bool m_draggingWindow = false;
    POINT m_dragStartScreen{};
    POINT m_windowStartPos{};
    int m_hoveredHitId = 0;
    int m_hoveredZoomIdx = -1;
    D2D1_RECT_F m_timelineTrackRect{};
    std::vector<HitZone> m_hitZones;

    // 后台硬件加速转码导出状态
    std::atomic<bool> m_exporting{false};
    std::atomic<bool> m_cancelExport{false};
    std::atomic<double> m_exportProgress{0.0};
    std::thread m_exportWorker;
    std::mutex m_exportMutex;

    // Direct2D / DirectWrite 渲染资源
    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_frameBitmap;
    UINT32 m_bitmapW = 0;
    UINT32 m_bitmapH = 0;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_sectionFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_bodyFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_captionFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_monoFmt;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_STUDIOPREVIEWWINDOW_H
