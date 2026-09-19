#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinWindow — 贴图窗口
//
// 职责:
//   1. 将截图固定在屏幕最前面（Always-on-top）
//   2. 支持拖拽移动、鼠标滚轮缩放、旋转/翻转
//   3. 双击关闭
//   4. 右键菜单：复制/保存/关闭/透明度调节/旋转/翻转
//   5. 多个贴图窗口共存
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_PINWINDOW_H
#define TOOLS3000_CAPTURE_PINWINDOW_H

#include <windows.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <opencv2/core.hpp>
#include <vector>
#include <memory>
#include <cstdint>
#include <mutex>
#include <atomic>

namespace tools3000::capture {

struct PinOcrBlock {
    std::string text;
    cv::Rect origBox;
};

struct CaptureRegion;

class PinWindow : public std::enable_shared_from_this<PinWindow> {
public:
    /// 创建一个新的贴图窗口
    static std::shared_ptr<PinWindow> create(const cv::Mat& image, int x, int y);

    /// 将剪贴板内容（图像 / 文本 / #颜色）智能贴成浮空贴图
    static std::shared_ptr<PinWindow> createFromClipboard();

    /// 连续历史贴图会话调度入口：按时间先后倒序依次贴出历史截图（或当前剪贴板内容）
    /// 支持连续触发快捷键依次贴出倒数第 1 张、倒数第 2 张、倒数第 3 张...直至第 N 张
    static std::shared_ptr<PinWindow> pasteNextHistoryOrClipboard();

    /// 颜色与文本卡片生成工具（供贴图渲染及连续回溯会话调度器复用）
    static bool parseHexColor(const std::wstring& s, cv::Scalar& bgr, std::wstring& label);
    static cv::Mat renderColorSwatch(const cv::Scalar& bgr, const std::wstring& label);
    static cv::Mat renderTextToImage(const std::wstring& text);

    /// 根据图像大小与来源上下文，智能计算世界级最佳贴图生成坐标
    /// 若剪贴板图像与最近一次截图匹配，则精确贴在原截图坐标；
    /// 若为外部内容或无法匹配，则以光标为中心智能吸附居中，并严格限制在显示器工作区内部（杜绝超出屏幕边缘）
    static POINT calculateSmartSpawnPosition(int imageWidth, int imageHeight, const POINT* fallbackCursor = nullptr, const CaptureRegion* preferredRegion = nullptr, int padX = 0, int padY = 0);

    /// 关闭此贴图窗口
    void close();

    /// 设置透明度 (0.0~1.0)
    void setOpacity(float opacity);

    /// 设置缩放比例
    void setScale(float scale);

    /// 旋转贴图（90, 180, 270 度）
    void rotate(int angleDeg);

    /// 翻转贴图（horizontal: true 水平翻转, false 垂直翻转）
    void flip(bool horizontal);

    /// 色彩反转 / 负片模式 (快捷键 I)
    void invertColors();
    bool isInverted() const { return m_inverted; }

    /// 切换灰度黑白显示 (快捷键 G)
    void toggleGrayscale();
    bool isGrayscale() const { return m_grayscale; }

    /// 恢复 1:1 原始物理缩放比例 (快捷键 0 / 鼠标中键)
    void resetScale();

    /// 折叠 / 展开贴图 (微晶迷你胶囊, 快捷键 M)
    void toggleFold();
    bool isFolded() const { return m_folded; }

    /// 重新进入标注编辑模式
    void editMarkup();

    /// 提取贴图文本 (OCR)
    void extractText();

    /// 复制所选文字到剪贴板 (Ctrl+C / 浮动复制微徽章)
    void copySelectedText();

    /// 复制全部文字到剪贴板 (Ctrl+Shift+C)
    void copyAllText();

    /// 异步预加载/执行贴图 OCR 文本识别
    void ensureOcrExecutedAsync();

    /// 清空当前文字选择
    void clearTextSelection();

    /// 切换文字选择模式
    void toggleTextSelectionMode();

    /// 是否处于文字选择模式
    bool isTextSelectionMode() const { return m_textSelectionMode; }

    /// 更新贴图图像内容
    void updateImage(const cv::Mat& newImage);

    /// 获取当前展示图像
    const cv::Mat& sourceImage() const { return m_sourceImage; }
    /// 获取当前缩放比
    float scale() const { return m_scale; }
    int origWidth() const { return m_origWidth; }
    int origHeight() const { return m_origHeight; }

    /// 设置鼠标穿透（点透）。开启后窗口忽略所有鼠标事件，透传到下层窗口。
    void setClickThrough(bool enable);
    bool isClickThrough() const { return m_clickThrough; }

    /// 切换光标所在贴图的鼠标穿透状态。供全局快捷键调用——
    /// 穿透窗口收不到右键，必须靠快捷键切回；且 WS_EX_TRANSPARENT 会被 WindowFromPoint 跳过，
    /// 故此处用窗口矩形包含判断定位光标下的贴图。
    static bool toggleClickThroughUnderCursor();

    /// 退出全部贴图的鼠标穿透状态，确保可靠恢复交互能力
    static void cancelAllClickThrough();

    /// 计算磁性吸附对齐坐标（吸附到屏幕边缘或相邻贴图边缘）
    static POINT calculateMagneticSnap(HWND currentHwnd, POINT targetPos, int curW, int curH);

    /// 是否存活
    bool isAlive() const { return m_hwnd != nullptr; }

    ~PinWindow();

    // ── 全局管理 ─────────────────────────────────────────────────────────

    /// 关闭所有贴图窗口
    static void closeAll();

    /// 隐藏/显示所有贴图（在屏幕贴满时一键看桌面；隐藏后靠快捷键恢复）
    static void toggleHideAll();

    /// 整理所有贴图：归拢为从主屏左上角开始的整齐层叠堆，并恢复可见
    static void arrangeAll();

    /// 当前贴图数量
    static size_t count() { return s_instances.size(); }

private:
    PinWindow() = default;
    PinWindow(const PinWindow&) = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    bool initWindow(HINSTANCE hInstance, int x, int y, int w, int h);
    bool createRenderResources(const cv::Mat& image);
    void render();
    void applyLayeredOpacity();  // 按 opacity × 穿透提示系数 应用分层透明度

    static LRESULT CALLBACK pinWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    float m_opacity = 1.0f;
    float m_scale = 1.0f;
    int m_origWidth = 0;
    int m_origHeight = 0;
    bool m_isDragging = false;
    POINT m_dragOffset{};
    bool m_clickThrough = false;
    bool m_focused = false;  // 选中态（键盘焦点）：显示高亮边框，可按 Esc 隐藏
    int m_historyBrowseIndex = 0;  // 获焦状态下时光机翻页游标 (0=最新, 1=上一张...)
    bool m_inverted = false;
    bool m_grayscale = false;
    bool m_folded = false;
    RECT m_unfoldedRect{};
    cv::Mat m_colorBackup;
    
    // Hover Toolbar
    bool m_isHovering = false;
    float m_hoverAlpha = 0.0f;
    uint64_t m_hoverTime = 0;
    D2D1_RECT_F m_toolbarRect = {};
    D2D1_RECT_F m_btnFoldRect = {};
    D2D1_RECT_F m_btnInvertRect = {};
    D2D1_RECT_F m_btnEditRect = {};
    D2D1_RECT_F m_btnTextRect = {};
    D2D1_RECT_F m_btnCopyRect = {};
    D2D1_RECT_F m_btnSaveRect = {};
    D2D1_RECT_F m_btnCloseRect = {};
    bool m_hoverFold = false;
    bool m_hoverInvert = false;
    bool m_hoverEdit = false;
    bool m_hoverText = false;
    bool m_hoverCopy = false;
    bool m_hoverSave = false;
    bool m_hoverClose = false;

    // 贴图直接划选与文字提取 (Live Text Selection & Copying)
    bool m_textSelectionMode = false;
    std::atomic<uint64_t> m_ocrVersion{0};
    std::vector<PinOcrBlock> m_ocrBlocks;
    std::mutex m_ocrMutex;
    std::atomic<bool> m_ocrReady{false};
    std::vector<size_t> m_selectedBlockIndices;
    int m_hoveredBlockIndex = -1;
    bool m_isSelectingText = false;
    POINT m_selectStart{};
    POINT m_selectEnd{};
    D2D1_RECT_F m_btnCopyFloatingRect = {};
    bool m_hoverCopyFloating = false;
    
    void updateHoverAnimation();
    void drawHoverToolbar();

    cv::Mat m_sourceImage;  // 原图副本，供"复制到剪贴板"

    // D2D
    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_bitmap;

    // 全局实例管理
    static std::vector<std::shared_ptr<PinWindow>> s_instances;
    static bool s_classRegistered;
    static bool s_allHidden;  // toggleHideAll 的隐藏态
};

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_PINWINDOW_H
