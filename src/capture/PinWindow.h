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

#include "capture/PinGroupManager.h"
#include "capture/MarkupEngine.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <opencv2/core.hpp>
#include <array>
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
    /// 创建一个新的贴图窗口（可选携带微晶卡片圆角、原始纯文本元数据、1:1 无损底图与独立矢量图元列表）
    static std::shared_ptr<PinWindow> create(const cv::Mat& image, int x, int y,
                                             float cardCornerRadius = 0.0f,
                                             std::wstring sourceText = {},
                                             const cv::Mat& cleanBaseImage = {},
                                             std::vector<MarkupElement> markupElements = {});

    /// 将剪贴板内容（图像 / 文本 / #颜色）智能贴成浮空贴图
    static std::shared_ptr<PinWindow> createFromClipboard();

    /// 连续历史贴图会话调度入口：按时间先后倒序依次贴出历史截图（或当前剪贴板内容）
    /// 支持连续触发快捷键依次贴出倒数第 1 张、倒数第 2 张、倒数第 3 张...直至第 N 张
    static std::shared_ptr<PinWindow> pasteNextHistoryOrClipboard();

    /// 颜色与文本卡片生成工具（供贴图渲染及连续回溯会话调度器复用）
    static bool parseHexColor(const std::wstring& s, cv::Scalar& bgr, std::wstring& label);
    static cv::Mat renderColorSwatch(const cv::Scalar& bgr, const std::wstring& label, float dpiScaleOverride = 1.0f);
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

    /// 贴图分组归属 (0 = 未分组, 1..32 = 第 1~32 组)
    int groupId() const noexcept { return m_groupId; }
    void setGroupId(int groupId);
    void toggleAssignGroup(int groupId);
    void showGroupPopupMenu(POINT screenPt);

    /// 重新进入标注编辑模式
    void editMarkup();

    /// 提取贴图文本 (OCR)
    void extractText();

    /// 全选贴图识别出的全部文字块并高亮 (Ctrl+A)
    void selectAllText();

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

    /// 当前已选中的文字块数量（供状态查询与测试验证）
    size_t selectedTextBlockCount() const;
    bool isOcrReady() const noexcept { return m_ocrReady.load(); }
    bool isOcrInFlight() const noexcept {
        return m_ocrInFlightVersion.load() != 0 || (m_ocrActiveWorkers && m_ocrActiveWorkers->load() > 0);
    }
    uint64_t ocrRunCount() const noexcept { return m_ocrRunCount.load(); }

    /// 注入/预置 OCR 文字块（供离线单元测试确定性验证 Ctrl+A / 旋转保护）
    void setOcrBlocksForTest(std::vector<PinOcrBlock> blocks);

    /// 更新贴图图像内容
    void updateImage(const cv::Mat& newImage, bool clearCardMetadata = true);

    /// 在 1:1 原始物理像素空间无损应用二次标注底图与独立矢量列表，保持 m_origWidth/Height 与 m_scale 缩放倍率不变
    void applyEditedMarkup(const cv::Mat& cleanBaseImage,
                           const std::vector<MarkupElement>& elements,
                           const cv::Mat& precomposited = {});

    const cv::Mat& cleanBaseImage() const noexcept { return m_cleanBaseImage; }
    const std::vector<MarkupElement>& markupElements() const noexcept { return m_markupElements; }

    /// 是否为生成式微晶字签/色卡签
    bool isGeneratedCard() const { return m_cardCornerRadius > 0.5f || !m_pinnedSourceText.empty(); }
    float cardCornerRadius() const { return m_cardCornerRadius; }
    const std::wstring& pinnedSourceText() const { return m_pinnedSourceText; }

    /// 获取当前展示图像
    const cv::Mat& sourceImage() const { return m_sourceImage; }
    /// 获取当前缩放比与不透明度
    float scale() const { return m_scale; }
    float opacity() const { return m_opacity; }
    int origWidth() const { return m_origWidth; }
    int origHeight() const { return m_origHeight; }
    HWND hwnd() const { return m_hwnd; }
    bool isFocused() const { return m_focused; }
    bool isDragging() const noexcept { return m_isDragging; }
    int pressedToolbarBtn() const noexcept { return m_pressedToolbarBtn; }
    bool isHoverToolbarVisible() const noexcept {
        return m_hoverAlpha > 0.01f && m_toolbarRect.right > m_toolbarRect.left && m_toolbarRect.bottom > m_toolbarRect.top;
    }
    D2D1_RECT_F hoverToolbarRect() const noexcept { return m_toolbarRect; }
    D2D1_RECT_F textButtonRect() const noexcept { return m_btnTextRect; }
    D2D1_RECT_F invertButtonRect() const noexcept { return m_btnInvertRect; }
    void forceHoverStateForTest(bool hovering);

    /// 激活并获取当前贴图窗口的键盘焦点（确保快捷键如旋转 R / 1 / 2 等即时生效）
    void focusWindow();

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

    /// ── 贴图分组与按组查看全局编排 ───────────────────────────────────────
    static bool isAllHidden() noexcept { return s_allHidden; }
    static int activeGroupFilter() noexcept { return s_activeGroupFilter; }
    static void setActiveGroupFilter(int groupFilter);
    static bool isExplicitGroupCreated(int groupId) noexcept;
    static int nextAvailableGroupId() noexcept;
    static int createAndActivateNewGroup(PinWindow* assignPin = nullptr);
    static int groupAllUngrouped();
    static void arrangeGroup(int groupId);
    static size_t dissolveGroup(int groupId);
    static size_t closeGroup(int groupId);
    static size_t countInGroup(int groupId) noexcept;
    static void ensureExplicitGroupCreated(int groupId) noexcept;
    static void refreshGroupBar();
    static void refreshAllVisualsAndGroupBar();
    static void broadcastGroupStateChanged();

    /// 当前贴图数量
    static size_t count() { return s_instances.size(); }

    /// 获取最近创建的贴图窗口实例（供桥接自省与集成测试调用）
    static std::shared_ptr<PinWindow> latestInstance() {
        return s_instances.empty() ? nullptr : s_instances.back();
    }

    /// 模块级后台 OCR 线程停机门禁与安全排空（插件卸载前等待在途识别线程退出，杜绝 FreeLibrary 崩溃）
    static void resetModuleShutdown() noexcept;
    static void beginModuleShutdown() noexcept;
    static bool waitForPendingOcrWorkers(uint32_t timeoutMs = 5000) noexcept;
    static uint32_t globalActiveOcrWorkerCount() noexcept;

private:
    PinWindow() = default;
    PinWindow(const PinWindow&) = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    bool initWindow(HINSTANCE hInstance, int x, int y, int w, int h);
    bool createRenderResources(const cv::Mat& image);
    bool ensureTextResources(float dpiScale);
    float measureUiTextWidth(const std::wstring& text, float dpiScale);
    SIZE computeFoldCapsuleSize(float dpiScale);
    void applySharpWindowCorners();
    void updateWindowCornerRegion();
    void render();
    void applyLayeredOpacity();  // 按 opacity × 穿透提示系数 应用分层透明度

    static LRESULT CALLBACK pinWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    float m_dpiScale = 1.0f;
    float m_opacity = 1.0f;
    float m_scale = 1.0f;
    int m_origWidth = 0;
    int m_origHeight = 0;
    bool m_isDragging = false;
    POINT m_dragOffset{};
    int m_pressedToolbarBtn = -1;
    POINT m_pressStartClientPt{};
    bool m_dragThresholdExceeded = false;
    bool m_clickThrough = false;
    bool m_focused = false;  // 选中态（键盘焦点）：显示高亮边框，可按 Esc 隐藏
    int m_historyBrowseIndex = 0;  // 获焦状态下时光机翻页游标 (0=最新, 1=上一张...)
    bool m_inverted = false;
    bool m_grayscale = false;
    bool m_folded = false;
    int m_groupId = 0;
    RECT m_unfoldedRect{};
    cv::Mat m_colorBackup;
    
    // Hover Toolbar & Group Badge
    bool m_isHovering = false;
    float m_hoverAlpha = 0.0f;
    uint64_t m_hoverTime = 0;
    D2D1_RECT_F m_btnGroupBadgeRect = {};
    bool m_hoverGroupBadge = false;
    D2D1_RECT_F m_toolbarRect = {};
    D2D1_RECT_F m_btnFoldRect = {};
    D2D1_RECT_F m_btnRotateRect = {};
    D2D1_RECT_F m_btnInvertRect = {};
    D2D1_RECT_F m_btnEditRect = {};
    D2D1_RECT_F m_btnTextRect = {};
    D2D1_RECT_F m_btnCopyRect = {};
    D2D1_RECT_F m_btnSaveRect = {};
    D2D1_RECT_F m_btnCloseRect = {};
    bool m_hoverFold = false;
    bool m_hoverRotate = false;
    bool m_hoverInvert = false;
    bool m_hoverEdit = false;
    bool m_hoverText = false;
    bool m_hoverCopy = false;
    bool m_hoverSave = false;
    bool m_hoverClose = false;

    // 贴图直接划选与文字提取 (Live Text Selection & Copying)
    bool m_textSelectionMode = false;
    std::atomic<uint64_t> m_ocrVersion{1};
    std::atomic<uint64_t> m_ocrInFlightVersion{0};
    std::atomic<uint64_t> m_ocrRunCount{0};
    std::atomic<bool> m_extractTextInFlight{false};
    std::shared_ptr<std::atomic<uint32_t>> m_ocrActiveWorkers = std::make_shared<std::atomic<uint32_t>>(0);
    void invalidateOcrStateAndRefreshIfSelecting();
    void releaseGraphicsResources() noexcept;
    std::vector<PinOcrBlock> m_ocrBlocks;
    mutable std::mutex m_ocrMutex;
    std::atomic<bool> m_ocrReady{false};
    std::atomic<bool> m_pendingSelectAllOcr{false};
    std::vector<size_t> m_selectedBlockIndices;
    int m_hoveredBlockIndex = -1;
    bool m_isSelectingText = false;
    POINT m_selectStart{};
    POINT m_selectEnd{};
    D2D1_RECT_F m_btnCopyFloatingRect = {};
    bool m_hoverCopyFloating = false;
    
    void updateHoverAnimation();
    void drawHoverToolbar();
    void clearHoverToolbarRects() noexcept;
    int hitTestInteractiveButton(int x, int y) const noexcept;
    void executeInteractiveButton(int btnId, int clientX, int clientY, bool shiftHeld);

    cv::Mat m_sourceImage;  // 原图副本，供"复制到剪贴板"
    cv::Mat m_cleanBaseImage;  // 1:1 原始物理分辨率无损纯净底图（与矢量标注分离存储）
    std::vector<MarkupElement> m_markupElements;  // 1:1 原始像素空间下的独立可二次编辑矢量列表
    float m_cardCornerRadius = 0.0f;       // 微晶字签/色卡签基准像素圆角 (0 = 普通直角截图)
    std::wstring m_pinnedSourceText;       // 微晶字签/色卡签原始纯文本，支持零误差一键复制

    // D2D & DirectWrite 缓存
    struct PinBitmapTile {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    };
    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_bitmap;
    std::vector<PinBitmapTile> m_bitmapTiles;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_uiLeadTextFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_uiCenterTextFormat;
    float m_cachedTextScale = 0.0f;
    bool m_cachedTextChinese = true;

    // 全局实例管理
    static std::vector<std::shared_ptr<PinWindow>> s_instances;
    static bool s_classRegistered;
    static bool s_allHidden;  // toggleHideAll 的隐藏态
    static int s_activeGroupFilter; // -1=全部, 0=仅未分组, 1..32=仅对应组
    static std::array<bool, PinGroupConstants::kMaxGroupId + 1> s_createdGroups;
};

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_PINWINDOW_H
