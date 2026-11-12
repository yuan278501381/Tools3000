#pragma once
#ifndef TOOLS3000_CAPTURE_CAPTUREOVERLAY_H
#define TOOLS3000_CAPTURE_CAPTUREOVERLAY_H

#include "capture/ScreenCapture.h"
#include "capture/ScrollCapture.h"
#include "capture/CaptureState.h"
#include "capture/CaptureRenderer.h"
#include "capture/CaptureInput.h"
#include <windows.h>
#include <memory>

namespace tools3000::capture {

class CaptureOverlay {
public:
    static CaptureOverlay& instance();

    using EditPinnedMarkupCallback = std::function<void(
        bool accepted,
        const cv::Mat& newCleanBaseImage,
        const cv::Mat& compositedImage,
        std::vector<MarkupElement> updatedElements)>;

    bool initialize(HINSTANCE hInstance);
    void shutdown();
    void startSelection(const CaptureOptions& options, OverlayMode mode = OverlayMode::Screenshot);
    /// 启动已有贴图的二次标注编辑
    void startEditPinned(const cv::Mat& image, const CaptureRegion& region,
                         std::function<void(const cv::Mat& newImage)> onFinished);
    /// 启动已有贴图的 1:1 原始像素空间无损矢量二次标注编辑
    void startEditPinnedMarkup(const cv::Mat& cleanBaseImage,
                               const std::vector<MarkupElement>& existingElements,
                               const CaptureRegion& pinScreenRect,
                               EditPinnedMarkupCallback onFinished);
    bool isEditingPinned() const noexcept {
        return static_cast<bool>(m_editPinnedCallback) || static_cast<bool>(m_editPinnedMarkupCallback);
    }
    /// 启动长截图首尾无损裁切与全功能后置精修工作台（单一事实源 SSOT 复用标准截图全量工具链）
    bool startEditLongImage(
        const cv::Mat& stitchedImage,
        int frameCount,
        std::function<void(const cv::Mat& finalImage, ScrollFinishAction action, int frameCount)> onFinished);
    bool isLongImageEditing() const noexcept { return m_state.isLongImageMode(); }
    void cancel();
    void reloadThemeColors() { m_renderer.applyThemeColors(); m_renderer.invalidate(); }

    void setCallback(SelectionCallback callback) { m_state.callback = std::move(callback); }
    void setClosedCallback(std::function<void()> callback) { m_closedCallback = std::move(callback); }
    void setRecordCallback(RecordSelectionCallback callback) { m_state.recordCallback = std::move(callback); }
    void setOcrCallback(std::function<void(const CaptureRegion& region, const cv::Mat& cropped)> callback) { m_state.ocrCallback = std::move(callback); }

    OverlayState state() const { return m_state.state.load(); }
    OverlayMode mode() const { return m_state.mode; }
    bool isOverlayVisible() const { return m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd); }
    void setShortcutHintsEnabled(bool enabled);
    void confirmCurrentSelection(CaptureCompletion completion = {CaptureCompletionAction::Default}) {
        confirmSelection(std::move(completion));
    }

private:
    CaptureOverlay() = default;
    ~CaptureOverlay() = default;
    CaptureOverlay(const CaptureOverlay&) = delete;
    CaptureOverlay& operator=(const CaptureOverlay&) = delete;

    bool createOverlayWindow(HINSTANCE hInstance, const RECT* customBounds = nullptr);
    void resetTransientOverlayState();
    void loadUserMarkupPreferences(bool preserveSavedOutputFrame);
    static LRESULT CALLBACK staticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;

    CaptureState m_state;
    CaptureRenderer m_renderer;
    CaptureInput m_input;
    std::function<void()> m_closedCallback;
    std::function<void(const cv::Mat& newImage)> m_editPinnedCallback;
    EditPinnedMarkupCallback m_editPinnedMarkupCallback;
    cv::Mat m_editPinnedOriginalCleanBase;
    cv::Rect m_editPinnedInitialRect{};
    std::function<void(const cv::Mat& finalImage, ScrollFinishAction action, int frameCount)> m_editLongImageCallback;
    HBITMAP m_frozenBitmap = nullptr;

    void realCancel();
    void cleanupAfterDelivery();
    void confirmSelection(CaptureCompletion completion = {});
    bool freezeScreen();
    void releaseFrozenSurface();
};

} // namespace tools3000::capture
#endif // TOOLS3000_CAPTURE_CAPTUREOVERLAY_H
