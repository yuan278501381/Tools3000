#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "core/accessibility/OverlayUiaProvider.h"

#include <string>
#include <vector>

namespace tools3000::capture {

enum class ShortcutHintContext {
    CaptureSelecting,    // 1. 未选区刚呼出阶段
    CaptureDragging,     // 2. 拖拽选区阶段
    CaptureSelected,     // 3. 选区已锁定/调整阶段
    CaptureMarking,      // 4. 正在进行几何/箭头标注阶段
    CaptureMarkingNumber,// 5. 正在进行序号标记 (可拖拽引出箭头)
    CaptureEditingText,  // 6. 正在编辑文字/输入阶段
    RecordSelecting,
    ScrollCapture,
    Recording,
    RecordingPaused,
};

struct ShortcutHintItem {
    std::wstring key;
    std::wstring label;
};

/// Small, click-through contextual shortcut guide anchored to the lower-left
/// of the active monitor. It owns no timers and destroys its layered window
/// when hidden, so it has zero idle composition cost.
class ShortcutHintOverlay {
public:
    static ShortcutHintOverlay& instance();

    void show(ShortcutHintContext context, POINT anchor = {LONG_MIN, LONG_MIN},
              const std::vector<RECT>& avoidRects = {});
    void updateAvoidance(const std::vector<RECT>& avoidRects);
    void hide();
    void shutdown() { hide(); }
    bool isVisible() const;
    RECT getBounds() const;
    std::vector<ShortcutHintItem> getItemsForContext(ShortcutHintContext context) const { return itemsFor(context); }

    /// 计算微晶操作提示卡片的最佳布局点位与几何避让 (供渲染与自动化测试调用)
    static POINT computeOptimalPosition(int width, int height, const RECT& workArea, float scale,
                                        const std::vector<RECT>& avoidRects);

private:
    struct PositionedItem {
        ShortcutHintItem item;
        float x = 0.0f;
        float y = 0.0f;
        float keyWidth = 0.0f;
        float labelWidth = 0.0f;
        float labelX = 0.0f;
    };

    ShortcutHintOverlay() = default;
    ~ShortcutHintOverlay() { hide(); }

    bool createWindow();
    bool createResources(float scale);
    std::vector<ShortcutHintItem> itemsFor(ShortcutHintContext context) const;
    float measureText(const std::wstring& text, IDWriteTextFormat* format) const;
    bool layout(const std::vector<ShortcutHintItem>& items, int workWidth, float scale,
                int& width, int& height);
    void render();
    void discardResources();
    std::vector<tools3000::core::accessibility::OverlayUiaAction> accessibilityActions() const;
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    HINSTANCE m_module = nullptr;
    ShortcutHintContext m_context = ShortcutHintContext::CaptureSelecting;
    bool m_hasContext = false;
    RECT m_workArea{};
    float m_scale = 1.0f;
    int m_width = 0;
    int m_height = 0;
    std::vector<RECT> m_avoidRects;
    std::vector<PositionedItem> m_items;
    D2D1_RECT_F m_cardRect = {};

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_keyFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_labelFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_panelBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_keyBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_keyBorderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_keyTextBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_labelBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_sheenBrush;
};

}  // namespace tools3000::capture
