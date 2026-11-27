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
#include "capture/PinMemoryGovernor.h"
#include "capture/PinOcrTypes.h"
#include "capture/MarkupEngine.h"
#include "capture/InPlaceTranslateEngine.h"
#include "core/utils/WindowImeGuard.h"
#include "ocr/OcrEngine.h"
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
#include <string>
#include <string_view>

namespace tools3000::capture {

struct CaptureRegion;
class PinZoomStage;
class ScopedHighResTimerPeriod;

class PinWindow : public std::enable_shared_from_this<PinWindow> {
public:
    /// 创建一个新的贴图窗口（可选携带微晶卡片圆角、原始纯文本元数据、1:1 无损底图与独立矢量图元列表）
    static std::shared_ptr<PinWindow> create(const cv::Mat& image, int x, int y,
                                             float cardCornerRadius = 0.0f,
                                             std::wstring sourceText = {},
                                             const cv::Mat& cleanBaseImage = {},
                                             std::vector<MarkupElement> markupElements = {},
                                             bool autoStartLiveOcr = true);

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

    /// 贴图空白处双击动作偏好（默认 Edit 保持现有行为，可选 Fold/Close 适配 Snipaste 肌肉记忆）
    enum class DoubleClickAction : uint8_t {
        Edit = 0,
        Fold = 1,
        Close = 2,
    };

    /// 贴图鼠标中键单击动作偏好（默认 ResetScale 保持 1:1 复原，可选 Close/Fold）
    enum class MiddleClickAction : uint8_t {
        ResetScale = 0,
        Close = 1,
        Fold = 2,
    };

    static constexpr DoubleClickAction parseDoubleClickAction(std::string_view value) noexcept {
        if (value == "fold") return DoubleClickAction::Fold;
        if (value == "close") return DoubleClickAction::Close;
        return DoubleClickAction::Edit;
    }

    static constexpr const char* doubleClickActionToString(DoubleClickAction action) noexcept {
        switch (action) {
            case DoubleClickAction::Fold: return "fold";
            case DoubleClickAction::Close: return "close";
            case DoubleClickAction::Edit:
            default: return "edit";
        }
    }

    static constexpr MiddleClickAction parseMiddleClickAction(std::string_view value) noexcept {
        if (value == "close") return MiddleClickAction::Close;
        if (value == "fold") return MiddleClickAction::Fold;
        return MiddleClickAction::ResetScale;
    }

    static constexpr const char* middleClickActionToString(MiddleClickAction action) noexcept {
        switch (action) {
            case MiddleClickAction::Close: return "close";
            case MiddleClickAction::Fold: return "fold";
            case MiddleClickAction::ResetScale:
            default: return "reset_scale";
        }
    }

    /// 决策双击空白处后的实际行为（处于折叠态时优先展开，原位改字编辑中绝不误关或折叠）
    static constexpr DoubleClickAction resolveEffectiveDoubleClickAction(
        DoubleClickAction configured,
        bool isFolded,
        bool isEditingTextReplace) noexcept {
        if (isFolded) return DoubleClickAction::Fold;
        if (isEditingTextReplace && configured != DoubleClickAction::Edit) {
            return DoubleClickAction::Edit;
        }
        return configured;
    }

    /// 决策鼠标中键单击后的实际行为（原位改字编辑中绝不误关闭，退回 ResetScale 保护未保存编辑）
    static constexpr MiddleClickAction resolveEffectiveMiddleClickAction(
        MiddleClickAction configured,
        bool isEditingTextReplace) noexcept {
        if (isEditingTextReplace && configured == MiddleClickAction::Close) {
            return MiddleClickAction::ResetScale;
        }
        return configured;
    }

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

    /// 单张贴图识字语种覆写（"" 表示跟随全局设置，"auto"/"cjk"/"japan"/"korean"/"cyrillic"/"latin"）
    std::string effectiveOcrScriptMode() const;
    bool hasOcrScriptModeOverride() const noexcept { return !m_ocrScriptModeOverride.empty(); }
    void setOcrScriptModeOverride(const std::string& scriptMode);

    /// 原位无痕翻译贴图（selectedOnly=true 仅翻译当前划选文字；forceRetranslate=true 强制忽略缓存重译）
    void translateInPlace(bool selectedOnly = false, bool forceRetranslate = false);

    /// 0ms 瞬切原图 / 译图对照（非破坏原图）
    void toggleTranslationOverlay();

    /// 复制本次原位翻译的全部译文到剪贴板
    void copyTranslatedText();

    /// 清除过期译图并取消在途翻译请求
    void clearTranslationState();

    bool isShowingTranslation() const noexcept {
        return m_showingTranslation && (!m_translatedImage.empty() || !m_translatedVault.empty());
    }
    bool isTranslationInFlight() const noexcept { return m_translationInFlight.load(std::memory_order_acquire); }
    bool hasTranslationResult() const noexcept { return !m_translatedImage.empty() || !m_translatedVault.empty(); }
    const cv::Mat& activeDisplayImage() const noexcept {
        ensureImagesMaterialized();
        return (m_showingTranslation && !m_translatedImage.empty()) ? m_translatedImage : m_sourceImage;
    }

    /// 全选贴图识别出的全部文字块并高亮 (Ctrl+A)
    void selectAllText();

    /// 复制所选文字到剪贴板 (Ctrl+C / 浮动复制微徽章)
    void copySelectedText();

    /// 复制全部文字到剪贴板 (Ctrl+Shift+C)
    void copyAllText();

    /// 将选中或全图表格结构以 CF_HTML + TSV 双格式复制到剪贴板（Excel/WPS/飞书直粘分列）
    void copySelectedOrAllAsTable();

    /// 打开右侧/独立 OCR 对照面板窗口精修编辑文本
    void openOcrComparisonPanel();

    /// 复制指定二维码解码文本到剪贴板
    void copyQrCodeText(size_t qrIndex);

    /// 将当前带全部最新标注的高清贴图写入剪贴板并给出诚实 Toast 反馈
    bool copyPinImageWithFeedback();

    /// 异步预加载/执行贴图 OCR 文本与二维码识别 (silentPreload=true 时静默识别不弹等待遮挡胶囊)
    void ensureOcrExecutedAsync(bool silentPreload = false);

    /// 直接注入已完成识别的页面文字行与二维码热区（供截图点击 OCR 原地生贴图零二次识别开销复用）
    void injectRecognizedPage(std::vector<tools3000::ocr::OcrResult> lines,
                              std::vector<tools3000::ocr::OcrQrResult> qrCodes = {});

    /// 清空当前文字选择
    void clearTextSelection();

    /// 切换文字选择模式
    void toggleTextSelectionMode();

    /// 是否处于文字选择模式
    bool isTextSelectionMode() const { return m_textSelectionMode; }

    /// 当前原图实景选字 (Live Text) 是否已启用
    bool isLiveTextEnabled() const noexcept;

    /// 当前已选中的文字块数量（供状态查询与测试验证）
    size_t selectedTextBlockCount() const;
    std::string selectedTextUtf8() const;
    bool hasDetectedTable() const;
    size_t qrCodeCount() const;
    int hoveredBlockIndex() const noexcept { return m_hoveredBlockIndex; }
    D2D1_RECT_F copyFloatingButtonRect() const noexcept { return m_btnCopyFloatingRect; }
    D2D1_RECT_F copyTableFloatingButtonRect() const noexcept { return m_btnCopyTableFloatingRect; }
    D2D1_RECT_F openPanelFloatingButtonRect() const noexcept { return m_btnOpenPanelFloatingRect; }
    D2D1_RECT_F highlightFloatingButtonRect() const noexcept { return m_btnHighlightFloatingRect; }
    D2D1_RECT_F wavyFloatingButtonRect() const noexcept { return m_btnWavyFloatingRect; }
    D2D1_RECT_F underlineFloatingButtonRect() const noexcept { return m_btnUnderlineFloatingRect; }
    D2D1_RECT_F textReplaceFloatingButtonRect() const noexcept { return m_btnTextReplaceFloatingRect; }
    D2D1_RECT_F textReplaceDragHandleRect() const noexcept { return m_trDragHandleRect; }
    D2D1_RECT_F undoFloatingButtonRect() const noexcept { return m_btnUndoFloatingRect; }
    D2D1_RECT_F colorDotFloatingRect(size_t idx) const noexcept {
        return idx < m_btnColorDotFloatingRects.size() ? m_btnColorDotFloatingRects[idx] : D2D1_RECT_F{};
    }

    /// 对当前选中的实景文字施加或切换（Toggle）荧光笔 / 波浪线 / 直线，保留 OCR 缓存零重跑
    bool annotateSelectedText(TextMarkupMode mode);
    /// 对当前选中的文字开启原位无痕改字实时编辑态（6 维视觉 DNA 自动克隆 + 无损图元）
    bool startInPlaceTextReplace();
    /// 双击已存在的原位改字图元重新进入无损二次编辑
    bool startInPlaceTextReplaceOnElement(size_t elementIdx);
    /// 提交或取消回滚当前原位改字编辑态
    void commitInPlaceTextReplace(bool keepChanges = true);
    bool isEditingTextReplace() const noexcept { return m_editingTextReplaceIdx >= 0; }
    bool handleTextReplaceChar(wchar_t ch);
    bool handleTextReplaceKeyDown(WPARAM vk, bool ctrl, bool shift, bool alt);
    void setTextMarkupColorIndex(int index);
    int textMarkupColorIndex() const noexcept { return m_textMarkupColorIndex; }
    MarkupColor currentTextMarkupColor() const noexcept;
    bool undoPinMarkup();
    bool redoPinMarkup();
    bool canUndoPinMarkup() const noexcept { return !m_pinUndoStack.empty(); }
    bool canRedoPinMarkup() const noexcept { return !m_pinRedoStack.empty(); }
    bool isOcrReady() const noexcept { return m_ocrReady.load(); }
    bool isOcrInFlight() const noexcept {
        return m_ocrInFlightVersion.load() != 0 || (m_ocrActiveWorkers && m_ocrActiveWorkers->load() > 0);
    }
    uint64_t ocrRunCount() const noexcept { return m_ocrRunCount.load(); }

    /// 注入/预置 OCR 文字块与二维码（供离线单元测试确定性验证 Ctrl+A / 逐字划选 / 旋转保护）
    void setOcrBlocksForTest(std::vector<PinOcrBlock> blocks);
    void setQrCodesForTest(std::vector<tools3000::ocr::OcrQrResult> qrCodes);

    /// 更新贴图图像内容
    void updateImage(const cv::Mat& newImage, bool clearCardMetadata = true);

    /// 在 1:1 原始物理像素空间无损应用二次标注底图与独立矢量列表，保持 m_origWidth/Height 与 m_scale 缩放倍率不变
    void applyEditedMarkup(const cv::Mat& cleanBaseImage,
                           const std::vector<MarkupElement>& elements,
                           const cv::Mat& precomposited = {});

    const cv::Mat& cleanBaseImage() const noexcept {
        ensureImagesMaterialized();
        return m_cleanBaseImage;
    }
    const std::vector<MarkupElement>& markupElements() const noexcept { return m_markupElements; }

    /// 是否为生成式微晶字签/色卡签
    bool isGeneratedCard() const { return m_cardCornerRadius > 0.5f || !m_pinnedSourceText.empty(); }
    float cardCornerRadius() const { return m_cardCornerRadius; }
    const std::wstring& pinnedSourceText() const { return m_pinnedSourceText; }

    /// 获取当前展示图像（若处于冷压缩态则自动透明无损还原）
    const cv::Mat& sourceImage() const {
        ensureImagesMaterialized();
        return m_sourceImage;
    }

    /// ── 贴图冷热分级无损压缩与全局内存预算治理 API ──────────────────────────
    uint64_t pinInstanceId() const noexcept { return m_pinInstanceId; }
    bool isOcrDeferredUntilHover() const noexcept { return m_ocrDeferredUntilHover.load(std::memory_order_acquire); }
    bool isCpuImageColdCompressed() const noexcept;
    uint64_t currentHotCpuImageBytes() const noexcept;
    uint64_t currentCompressedCpuBytes() const noexcept;
    int uploadedBitmapWidth() const noexcept { return m_uploadedBitmapWidth; }
    int uploadedBitmapHeight() const noexcept { return m_uploadedBitmapHeight; }
    bool compressCpuImagesToCold();
    void ensureImagesMaterialized() const noexcept;
    void enterHiddenDormantState();
    static uint64_t totalHotCpuImageBytesAcrossAllPins() noexcept;
    static void enforceGlobalPinMemoryBudget(const PinWindow* protectedPin = nullptr, uint64_t customBudgetBytes = 0);
    static void setHotPixelBudgetForTest(uint64_t budgetBytes) noexcept;
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
    void renderForTest() { render(); }
    bool lastChromeIsDark() const noexcept { return m_lastChromeIsDark; }
    bool lastToolbarIsDark() const noexcept { return m_lastToolbarIsDark; }
    bool lastRenderedPixelGrid() const noexcept { return m_lastRenderedPixelGrid; }
    bool lastRenderedOcrBadge() const noexcept { return m_lastRenderedOcrBadge; }
    std::wstring activeHudBadgeText() const;
    std::wstring hudBadgeTextForTest() const {
        if (m_hudBadgeText.empty() || GetTickCount64() >= m_hudBadgeUntilTick) return {};
        return m_hudBadgeText;
    }
    void setOcrInFlightForTest(bool inFlight) noexcept {
        m_ocrInFlightVersion.store(inFlight ? 1ULL : 0ULL, std::memory_order_release);
        if (inFlight) m_ocrReady.store(false, std::memory_order_release);
    }

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

    /// 关闭除指定贴图外的所有其他贴图窗口
    static void closeAllExcept(const PinWindow* keepPin);

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
    PinWindow();
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
    void handleMouseWheel(WPARAM wParam, LPARAM lParam);
    void handleZoomWheel(int delta, POINT cursorScreenPt, bool hasCursorPt);
    void handleZoomStageMouseInterrupt(UINT msg, WPARAM wParam, POINT screenPt);
    void stopZoomAnimation(bool renderFinalSharpness = false, bool snapToTarget = false);
    void finishZoomAnimationAtScale(double finalScale, bool updateHudBadge);
    void applyScaleAtAnchor(float newScale, POINT anchorScreenPt, float relX, float relY, bool finalFrame);
    void stepZoomAnimation();
    bool startOrUpdateZoomStage(const RECT& currentPinRect, double initialDt);
    void renderCurrentZoomStageFrame();
    void releaseZoomStage() noexcept;
    void invalidateZoomStageTexture() noexcept;

    static constexpr UINT_PTR kTimerIdZoomAnim = 4;
    static constexpr UINT_PTR kTimerIdZoomStageIdleRelease = 5;
    static constexpr UINT_PTR kTimerIdBusySpinner = 6;

    static LRESULT CALLBACK pinWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    float m_dpiScale = 1.0f;
    float m_opacity = 1.0f;
    float m_scale = 1.0f;
    bool m_zoomAnimating = false;
    bool m_zoomStageActive = false;
    double m_zoomCurScale = 1.0;
    double m_zoomTargetScale = 1.0;
    double m_zoomCurLeft = 0.0;
    double m_zoomCurTop = 0.0;
    double m_zoomAnchorX = 0.0;
    double m_zoomAnchorY = 0.0;
    double m_zoomAnchorRelX = 0.5;
    double m_zoomAnchorRelY = 0.5;
    int64_t m_zoomLastQpc = 0;
    uint64_t m_displayImageGeneration = 0;
    std::unique_ptr<PinZoomStage> m_zoomStage;
    std::unique_ptr<ScopedHighResTimerPeriod> m_zoomTimerPeriod;
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
    D2D1_RECT_F m_btnTranslateRect = {};
    D2D1_RECT_F m_btnCopyRect = {};
    D2D1_RECT_F m_btnSaveRect = {};
    D2D1_RECT_F m_btnCloseRect = {};
    bool m_hoverFold = false;
    bool m_hoverRotate = false;
    bool m_hoverInvert = false;
    bool m_hoverEdit = false;
    bool m_hoverText = false;
    bool m_hoverTranslate = false;
    bool m_hoverCopy = false;
    bool m_hoverSave = false;
    bool m_hoverClose = false;

    // 贴图直接划选与文字提取 (Live Text Selection & Copying)
    bool m_textSelectionMode = false;
    std::atomic<uint64_t> m_ocrVersion{1};
    std::atomic<uint64_t> m_ocrInFlightVersion{0};
    std::atomic<uint64_t> m_ocrRunCount{0};
    std::atomic<bool> m_extractTextInFlight{false};
    std::atomic<bool> m_silentOcrPreload{false};
    std::shared_ptr<std::atomic<uint32_t>> m_ocrActiveWorkers = std::make_shared<std::atomic<uint32_t>>(0);
    void invalidateOcrStateAndRefreshIfSelecting();
    void releaseGraphicsResources() noexcept;
    bool selectWordAtClientPoint(int clientX, int clientY);
    std::vector<PinOcrBlock> m_ocrBlocks;
    std::vector<PinQrCodeItem> m_qrCodes;
    bool m_hasTableStructure = false;
    mutable std::mutex m_ocrMutex;
    std::atomic<bool> m_ocrReady{false};
    std::atomic<bool> m_pendingSelectAllOcr{false};
    std::atomic<bool> m_pendingCopyAllOcr{false};
    std::atomic<bool> m_pendingOpenPanelOcr{false};
    std::vector<size_t> m_selectedBlockIndices;
    std::vector<PinBlockCharSpan> m_selectedCharSpans;
    int m_hoveredBlockIndex = -1;
    bool m_isSelectingText = false;
    bool m_isSelectingReplaceText = false;
    bool m_isCharStreamSelecting = false;
    bool m_showCrossRegionRectGuide = false;
    int m_anchorBlockIndex = -1;
    size_t m_anchorCharIndex = 0;
    POINT m_selectStart{};
    POINT m_selectEnd{};
    D2D1_RECT_F m_btnCopyFloatingRect = {};
    D2D1_RECT_F m_btnCopyTableFloatingRect = {};
    D2D1_RECT_F m_btnOpenPanelFloatingRect = {};
    D2D1_RECT_F m_btnHighlightFloatingRect = {};
    D2D1_RECT_F m_btnWavyFloatingRect = {};
    D2D1_RECT_F m_btnUnderlineFloatingRect = {};
    D2D1_RECT_F m_btnTextReplaceFloatingRect = {};
    D2D1_RECT_F m_btnTranslateFloatingRect = {};
    D2D1_RECT_F m_btnTranslateStatusRect = {};
    D2D1_RECT_F m_btnUndoFloatingRect = {};
    std::array<D2D1_RECT_F, 5> m_btnColorDotFloatingRects = {};
    bool m_hoverCopyFloating = false;
    bool m_hoverCopyTableFloating = false;
    bool m_hoverOpenPanelFloating = false;
    bool m_hoverHighlightFloating = false;
    bool m_hoverWavyFloating = false;
    bool m_hoverUnderlineFloating = false;
    bool m_hoverTextReplaceFloating = false;
    bool m_hoverTranslateFloating = false;
    bool m_hoverTranslateStatus = false;
    bool m_hoverUndoFloating = false;
    int m_hoveredColorDotIndex = -1;
    int m_textMarkupColorIndex = 0;
    bool m_ocrMarkupPreviewClean = false;

    // 原位无痕改字 (In-Place Text Replace) 实时编辑态与微调浮岛状态
    int m_editingTextReplaceIdx = -1;
    bool m_editingTextReplaceWasNew = false;
    bool m_isDraggingReplacePos = false;
    bool m_hoverReplaceDragHandle = false;
    POINT m_replaceDragStartClientPt{};
    float m_replaceDragOrigOffsetX = 0.0f;
    float m_replaceDragOrigOffsetY = 0.0f;
    D2D1_RECT_F m_trDragHandleRect = {};
    D2D1_RECT_F m_trEditBoxClientRect = {};
    std::vector<MarkupElement> m_editingTextReplaceBackup;
    D2D1_RECT_F m_btnTrFontDecRect = {};
    D2D1_RECT_F m_btnTrFontIncRect = {};
    D2D1_RECT_F m_btnTrWeightRect = {};
    D2D1_RECT_F m_btnTrFamilyRect = {};
    D2D1_RECT_F m_btnTrBgModeRect = {};
    D2D1_RECT_F m_btnTrReflowRect = {};
    D2D1_RECT_F m_btnTrDoneRect = {};
    int m_hoveredTrBtnId = -1;
    void updateImeCompositionAnchor() noexcept;
    void syncImeState(bool force = false) noexcept;
    bool isTextInputActive() const noexcept { return m_editingTextReplaceIdx >= 0; }
    tools3000::core::WindowImeGuard m_imeGuard;
    void clearTextReplaceToolbarRects() noexcept;
    int m_hoveredQrBadgeIndex = -1;
    bool m_lastChromeIsDark = true;
    bool m_lastToolbarIsDark = true;
    bool m_lastRenderedPixelGrid = false;
    bool m_lastRenderedOcrBadge = false;
    std::wstring m_hudBadgeText;
    uint64_t m_hudBadgeUntilTick = 0;

    std::vector<cv::Rect> collectCoalescedSelectedTextRectsLocked() const;
    void recompositeMarkupPreservingOcr();
    void pushPinMarkupUndoState();
    std::deque<std::vector<MarkupElement>> m_pinUndoStack;
    std::deque<std::vector<MarkupElement>> m_pinRedoStack;

    void showTransientHudBadge(std::wstring text, uint32_t durationMs = 800, bool triggerInvalidate = true);
    void setOcrStageText(std::wstring text);
    void setTranslateStageText(std::wstring text);
    void syncBusySpinnerTimer();
    void updateHoverAnimation();
    void drawHoverToolbar();
    void clearHoverToolbarRects() noexcept;
    int hitTestInteractiveButton(int x, int y) const noexcept;
    bool isInteractiveButtonHitWithSlop(int btnId, int x, int y, float slopPx) const noexcept;
    void executeInteractiveButton(int btnId, int clientX, int clientY, bool shiftHeld);

    uint64_t m_pinInstanceId = 0;
    mutable uint64_t m_lastInteractionTick = 0;
    std::atomic<bool> m_ocrDeferredUntilHover{false};
    mutable std::mutex m_imageVaultMutex;
    mutable cv::Mat m_sourceImage;  // 原图副本，供"复制到剪贴板"（支持冷热无损双向迁移）
    mutable cv::Mat m_cleanBaseImage;  // 1:1 原始物理分辨率无损纯净底图（与矢量标注分离存储）
    mutable cv::Mat m_translatedImage; // 非破坏原位翻译译图副本（与 m_sourceImage 独立存放，0ms 瞬切对照）
    mutable PinLosslessImageSlot m_sourceVault;
    mutable PinLosslessImageSlot m_cleanBaseVault;
    mutable PinLosslessImageSlot m_translatedVault;
    mutable bool m_vaultSharesCleanWithSource = false;
    int m_uploadedBitmapWidth = 0;
    int m_uploadedBitmapHeight = 0;
    bool m_showingTranslation = false;
    std::atomic<bool> m_translationInFlight{false};
    std::atomic<uint64_t> m_translationVersion{0};
    std::shared_ptr<std::atomic<bool>> m_translateCancelFlag;
    std::atomic<bool> m_pendingTranslateAfterOcr{false};
    std::atomic<bool> m_pendingTranslateSelectedOnly{false};
    std::string m_translatedTargetLang;
    std::string m_translatedProviderUsed;
    std::string m_ocrScriptModeOverride; // 贴图级识字语种覆写（空表示跟随全局配置）
    mutable std::mutex m_stageTextMutex;
    std::wstring m_ocrStageText;
    std::wstring m_translateStageText;
    float m_busySpinAngle = 0.0f;
    bool m_busySpinnerTimerActive = false;
    std::vector<TranslationParagraph> m_translatedParagraphs;
    uint64_t m_contentFingerprint = 0; // 当前贴图内容 64-bit 指纹（供 F3 贴图防重复套娃去重）
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
