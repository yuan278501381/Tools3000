#pragma once
#ifndef TOOLS3000_CAPTURE_CAPTUREINPUT_H
#define TOOLS3000_CAPTURE_CAPTUREINPUT_H

#include "capture/CaptureState.h"
#include "capture/CaptureRenderer.h"
#include <windows.h>
#include <functional>

namespace tools3000::capture {

class CaptureInput {
public:
    void initialize(HWND hwnd, CaptureState& state, CaptureRenderer& renderer,
                    std::function<void()> cancelCb,
                    std::function<void(CaptureCompletion)> confirmCb);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /// Invokes the current toolbar item on the overlay UI thread. UI Automation
    /// providers only post this request; they never touch CaptureState directly.
    bool invokeToolbarButton(std::size_t index);

    void openDropdownMenu(DropdownType type, const D2D1_RECT_F& anchorRect);
    void commitDropdownSelection(DropdownType type, int id);
    void executeToolbarCommand(const ToolbarButton& button);
    void updateHoverCursor(POINT point);
    static void ensureCursorVisible();
    static HCURSOR getBeautifulCrosshairCursor(float dpiScale = 1.0f);
    static HCURSOR getCornerRadiusLiveCursor(int cornerIndex = 0);

private:
    HitArea hitTestSelectionBox(POINT point, int* outCornerIndex = nullptr) const;
    void adjustSelection(HitArea handle, int dx, int dy);
    void enforceAspectRatioOnCurrentSelection();
    
    ToolbarButton* hitTestToolbar(POINT point);
    void rebuildToolbarButtons(const D2D1_RECT_F& selectionRect);
    void pickCustomColor();
    
    void setCurrentTool(MarkupTool tool);
    void toggleOrSetTool(MarkupTool tool);
    bool isPointInSelection(POINT point) const;
    cv::Point toMarkupPoint(POINT point) const;
    
    void beginMarkup(POINT point);
    void updateMarkup(POINT point);
    void finishMarkup(POINT point);
    
    void prepareMarkupBase();
    void rebuildMarkupBase();
    
    RECT detectWindowUnderCursor(POINT cursorPos);
    std::vector<RECT> detectWindowHierarchy(POINT cursorPos);
    D2D1_RECT_F currentSelectionRect() const;

    HWND m_hwnd = nullptr;
    CaptureState* m_state = nullptr;
    CaptureRenderer* m_renderer = nullptr;
    std::function<void()> m_cancelCb;
    std::function<void(CaptureCompletion)> m_confirmCb;
    
    void enableIME(bool enable);
    HIMC m_defaultImc = nullptr;
};

} // namespace tools3000::capture
#endif // TOOLS3000_CAPTURE_CAPTUREINPUT_H
