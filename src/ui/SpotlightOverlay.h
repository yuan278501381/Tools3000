#ifndef TOOLS3000_UI_SPOTLIGHTOVERLAY_H
#define TOOLS3000_UI_SPOTLIGHTOVERLAY_H

#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

#include "core/mouse/MouseFeedbackOverlayBase.h"
#include "core/mouse/MouseShakeDetector.h"
#include "core/mouse/MouseStreamCore.h"
#include "core/mouse/MouseViewportManager.h"
#include "core/utils/SpscRingBuffer.h"

namespace tools3000::ui {

struct SpotlightSettings {
    bool enabled = true;
    bool triggerDoubleCtrl = true;
    bool triggerShakeMouse = false;
    bool autoBypassFullscreen = true;
    std::string spotlightColor = "auto";
    int spotlightSize = 300;           // 直径 (像素，默认 300)
    int animationDurationMs = 1000;    // 渐变动画速度 (ms)
    int holdDurationMs = 0;            // 停留保持时间 (ms，默认 0 表示持续常亮不自动退出)
    int shakeThreshold = tools3000::core::MouseShakeDetector::kDefaultReversalThreshold;  // 摇晃折返次数 (默认 4, [3, 20])
    int shakeMinDistance = tools3000::core::MouseShakeDetector::kDefaultMinDistanceDip;   // 摇晃最小累计移动距离 (DIP, 默认 400, [100, 2000])
    std::string spotlightAnimStyle = "inward_gravity"; // inward_gravity (默认向心引力超聚焦), tactical_sonar (科技声纳), aurora_ripple (极简涟漪)

    // 双击 Ctrl 演示模式与电影级平滑交互放大 (Issue #34)
    std::string doubleCtrlMode = "spotlight"; // spotlight (经典聚光灯) | cinema_zoom (电影级平滑放大) | cinema_zoom_ring (电影放大+光标光环)
    float zoomLevel = 2.0f;                   // 电影放大默认倍率 (1.25 ~ 4.0)
    bool zoomFollowCursor = true;             // 放大期间镜头柔和死区跟随光标
    bool zoomWheelAdjust = true;              // 放大期间允许 Ctrl+滚轮 无级调节倍率

    // 鼠标点击与轨迹特效 (演示辅助)
    bool clickRippleEnabled = false;
    bool mouseTrailEnabled = false;
    std::string clickRippleStyle = "sparkle_burst";    // sparkle_burst (默认), ripple_ring, target_pulse, soft_glow, mac_zoom
    std::string mouseTrailStyle = "sonar_pulses";      // sonar_pulses (默认), stardust_orbs, aurora_ribbon, classic_comet
    std::string mouseTrailColorMode = "rainbow";       // rainbow, accent
    std::string leftClickColor = "auto";
    std::string rightClickColor = "#fb7185";
    std::string middleClickColor = "#fbbf24";
};

class SpotlightOverlay : public tools3000::core::MouseFeedbackOverlayBase {
public:
    static constexpr UINT_PTR TIMER_ANIM_ID = 201;
    static constexpr UINT WM_SPOTLIGHT_TICK = WM_APP + 201;

    static SpotlightOverlay& instance();

    bool initialize(HINSTANCE hInstance);
    void shutdown();

    /// 触发聚光灯或电影级交互放大，若 pt={0,0} 且 autoFetch=true 则自动采集当前光标坐标
    void trigger(POINT pt = {0, 0}, bool autoFetch = true, bool allowCinemaZoom = true);

    /// 立即取消或提前平滑淡出聚光灯/电影放大（例如按键按下、鼠标点击或锁屏）
    void dismiss();

    /// 电影级平滑放大完全归位至 1.0x 后的联动收尾回调（自动收起伴随光环）
    void onCinemaZoomEnded();

    /// 设置与持久化
    void updateSettings(const SpotlightSettings& settings);
    SpotlightSettings getSettings() const;
    void resetDefaults();

    /// 是否处于活跃/显示状态
    bool isActive() const;

    /// 钩子快速检测辅助
    void onKeyboardEvent(DWORD vkCode, WPARAM wParam);
    void onMouseMove(POINT pt);
    void onHookMouseMove(int x, int y);
    void onMouseDown(int button, POINT pt); // 0: Left, 1: Right, 2: Middle
    void tickAnimation();
    void requestTick();

    void wakeRenderThread();
    void hideNow();
    void refreshFullscreenBypassState(HWND fg = nullptr);

    D2D1_COLOR_F parseColor(const std::string& hexStr, float alpha) const;
    static D2D1_COLOR_F hslToRgb(float h, float s, float l, float alpha);

    struct ViewportBounds {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        bool isFullscreen = false;
    };
    ViewportBounds calculateViewportBoundsLocked() const;

    /// 空间滞回局部视口计算纯算法（供生产与单元测试共用）：内容未越出已有视口时 0 移动、0 表面重建
    static ViewportBounds applySpatialHysteresisViewport(
        const ViewportBounds& currentSticky,
        int contentMinX,
        int contentMinY,
        int contentMaxX,
        int contentMaxY,
        const RECT& safeBounds,
        int lookaheadMarginPx,
        int minDimPx = 256,
        int quantizePx = 64) noexcept;

    /// 全屏聚光灯 4-Rect 裁剪几何分解纯算法：仅圆洞外接局部小矩形走径向渐变，其余 97.5% 纯色矩形填充
    struct FourRectClipLayout {
        D2D1_RECT_F centerBox{};
        D2D1_RECT_F outerStrips[4]{};
        int outerStripCount = 0;
    };
    static FourRectClipLayout computeFourRectClipGeometry(
        float surfaceW,
        float surfaceH,
        float centerX,
        float centerY,
        float outerRadius) noexcept;

    enum class TrailParticleKind {
        OrbMain,      // 主能量光球 (6.5px ~ 8.0px, 带微光晕)
        OrbSub,       // 次级漂浮球 (3.5px ~ 5.0px)
        Sparklet,     // 伴生微星 (1.5px ~ 2.5px, 随机微偏移)
        RibbonNode,   // 极光流丝平滑节点
        SonarRing,    // 扩散声纳微环
        CometDot,     // 经典彗星连线点
        QuantumOrb,   // 量子引力公转微子
        TeslaBolt,    // 特斯拉高能电浆跳跃点
        InkStroke,    // 宣纸水墨动态笔触
        GridRuler,    // CAD 矢量直角刻度
        DewBubble     // 晨露微气泡
    };

    struct ClickSparkle {
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float size = 3.0f;
        float extra = 0.0f;
    };

    // 聚光灯动效状态机
    enum class AnimState {
        Idle,
        FadeIn,
        Holding,
        FadeOut
    };

    // 点击水波纹特效数据
    struct ClickRipple {
        POINT pt;
        std::chrono::steady_clock::time_point startTime;
        std::string color;
        std::string style = "sparkle_burst";
        float maxRadius = 42.0f;
        float durationMs = 450.0f;
        float extraAngle = 0.0f;
        std::vector<ClickSparkle> sparklets;
    };

    // 鼠标流光/星尘轨迹微粒数据
    struct TrailParticle {
        POINT pt;
        std::chrono::steady_clock::time_point time;
        TrailParticleKind kind = TrailParticleKind::SonarRing;
        float size = 7.5f;
        float durationMs = 280.0f;
        std::string color;
        float hue = 0.0f;
        float speed = 0.0f;
        float extra = 0.0f;
    };

    struct RenderSnapshot {
        AnimState animState = AnimState::Idle;
        float currentAlpha = 0.0f;
        float focusProgress = 0.0f;
        float scalePulse = 1.0f;
        float reticleAngle = 0.0f;
        POINT targetPos{0, 0};
        std::chrono::steady_clock::time_point holdStartTime{};
        SpotlightSettings settings;
        std::vector<ClickRipple> ripples;
        std::vector<TrailParticle> trail;
        ViewportBounds bounds;
        bool ringOnlyMode = false;
    };

    void renderSnapshot(const RenderSnapshot& snapshot);

protected:
    void onRenderTick() override { tickAnimation(); }
    bool isOverlayIdle() const override {
        if (m_visible.load(std::memory_order_relaxed)) return false;
        if (m_hideRequested.load(std::memory_order_relaxed)) return false;
        const HWND hwnd = m_hwnd.load(std::memory_order_acquire);
        if (hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd)) return false;
        if (m_hasDcompSurface.load(std::memory_order_acquire)) return false;
        return !isActive();
    }
    bool needsContinuousAnimation() const override;
    void onRecreateDeviceResources() override;
    void onReleaseDeviceResources() override;

private:
    SpotlightOverlay() = default;
    ~SpotlightOverlay() override;

    void render();
    void drainRawMovesLocked();
    void processMouseMoveLocked(POINT pt, bool bypass = false);
    void updateNeedMouseMoveLocked();
    bool isFullscreenBypassActiveFast() noexcept;
    void refreshCachedAccentColor() const noexcept;
    ID2D1SolidColorBrush* acquireSolidBrush(const D2D1_COLOR_F& color) noexcept;
    void ensureSphereGradientBrushesLocked(const D2D1_COLOR_F& baseColor) noexcept;

    std::atomic<bool> m_needMouseMove{false};
    std::atomic_flag m_rawMovePushSpin = ATOMIC_FLAG_INIT;
    std::atomic<bool> m_fullscreenOverlayBypass{false};
    std::atomic<uintptr_t> m_cachedForegroundHwnd{0};
    std::atomic<bool> m_fgProbePending{false};
    mutable std::atomic<uint32_t> m_cachedAccentRgb{0x3B82F6u};
    mutable std::atomic<uint64_t> m_cachedAccentConfigVersion{0};
    std::atomic<bool> m_pendingAnimStartReanchor{false};

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_reusableSolidBrush;
    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> m_cachedSphereBrush;
    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> m_cachedBounceBrush;
    uint32_t m_cachedGradientRgb = 0xFFFFFFFFu;
    mutable ViewportBounds m_stickyLocalViewport{};

    SpotlightSettings m_settings;
    mutable std::recursive_mutex m_mutex;

    AnimState m_animState = AnimState::Idle;
    std::chrono::steady_clock::time_point m_animStartTime;
    std::chrono::steady_clock::time_point m_holdStartTime;
    POINT m_targetPos{0, 0};
    float m_currentAlpha = 0.0f;
    float m_focusProgress = 0.0f;       // 0.0 ~ 1.0 向心聚焦/扩散进程
    float m_scalePulse = 1.0f;          // 物理弹性回弹缩放因子
    float m_reticleAngle = 0.0f;        // 战术准星旋转角度
    bool m_ringOnlyMode = false;        // 电影放大伴随光环模式（无全屏暗角，仅局部柔光引导环）

    // 双击 Ctrl 工业级状态机 (Down ➔ Up ➔ Down 严格闭环，杜绝 Auto-Repeat 连发与长按误触)
    enum class CtrlDoubleTapState {
        Idle,           // 初始静默态
        FirstPressed,   // 第一次按下，等待快速松开
        WaitingSecond   // 第一次已快速松开，等待第二次按下 (30ms ~ 380ms 时间窗)
    };
    CtrlDoubleTapState m_ctrlState = CtrlDoubleTapState::Idle;
    std::chrono::steady_clock::time_point m_firstCtrlDownTime{};
    std::chrono::steady_clock::time_point m_firstCtrlUpTime{};
    bool m_ctrlIsPhysicallyDown = false;

    // 摇晃鼠标双轴抗误触状态机与末帧渲染锚点
    tools3000::core::MouseShakeDetector m_shakeDetector{};
    POINT m_lastRenderedPos{-9999, -9999};

    std::vector<ClickRipple> m_ripples;
    std::vector<TrailParticle> m_trail;
    float m_trailHue = 195.0f; // 七彩色相累加器
    size_t m_eventSub = 0;
    size_t m_themeSub = 0;
    bool m_streamSubscribed = false;
};

} // namespace tools3000::ui

#endif // TOOLS3000_UI_SPOTLIGHTOVERLAY_H
