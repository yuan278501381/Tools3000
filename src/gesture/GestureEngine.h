#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// GestureEngine — 手势引擎主入口
//
// 职责:
//   1. 协调 MouseHook → GestureRecognizer → ScopeRuleEngine → GestureProfile
//   2. 管理手势的完整生命周期: 开始 → 采集 → 识别 → 匹配 → 执行
//   3. 手势轨迹可视化控制
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_GESTUREENGINE_H
#define TOOLS3000_GESTURE_GESTUREENGINE_H

#include "gesture/MouseHook.h"
#include "gesture/GestureRecognizer.h"
#include "gesture/GestureAction.h"
#include "gesture/GestureProfile.h"
#include "gesture/ScopeRule.h"
#include "gesture/GestureInputPolicy.h"

#include <memory>
#include <unordered_map>
#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <future>

namespace tools3000::gesture {

/// 手势状态
enum class GestureState {
    Idle,       // 空闲，等待触发
    Tracking,   // 正在追踪手势轨迹
    Executing   // 正在执行手势动作
};

/// 手势轨迹可视化回调（由 UI 层实现）
using TrailRenderCallback = std::function<void(const std::vector<TrackPoint>& points,
                                                const std::vector<Direction>& directions)>;
using PauseChangedCallback = std::function<bool(bool paused)>;

class GestureDispatchWorker;

class GestureEngine {
    friend class GestureDispatchWorker;
public:
    static GestureEngine& instance();

    /// 启动手势引擎
    bool start();

    /// 停止手势引擎
    void stop();

    /// 暂停/恢复手势
    /// 返回 false 表示状态持久化失败，运行时状态已自动回滚。
    bool setPaused(bool paused);
    bool isPaused() const { return m_paused.load(); }
    void setPauseChangedCallback(PauseChangedCallback callback);

    /// 手势录制/绘图模式：打开手势录制界面时临时放行所有鼠标手势，绝不触发任何动作
    void setRecordingMode(bool recording);
    bool isRecordingMode() const { return m_recordingMode.load(); }

    /// 触发按钮配置
    void setTriggerButton(const std::string& button);
    std::string triggerButton() const;

    /// 轨迹可视化开关
    void setTrailVisible(bool visible);
    bool trailVisible() const { return m_trailVisible.load(); }

    /// 全屏应用/游戏自动免打扰开关
    void setAutoBypassFullscreen(bool enable);
    bool autoBypassFullscreen() const { return m_autoBypassFullscreen.load(); }

    /// 手势作用窗口：起点下方，或当前前台窗口。
    void setTargetMode(const std::string& mode);
    std::string targetMode() const;

    /// 手势起始超时时间 (毫秒, 默认 500ms)
    void setInitialTimeoutMs(int ms);
    int initialTimeoutMs() const { return m_initialTimeoutMs.load(); }

    /// 手势最小识别距离 (像素, 默认 24px)
    void setMinSegmentDistance(int px);
    int minSegmentDistance() const { return m_minSegmentDistance.load(); }

    /// 当前状态
    GestureState state() const { return m_state.load(); }

    // ── Profile 管理 ─────────────────────────────────────────────────────

    /// 添加/更新 Profile
    void setProfile(const std::string& name, const GestureProfile& profile);

    /// 获取 Profile 快照。禁止向调用方暴露容器内指针，避免并发更新后悬空。
    std::optional<GestureProfile> getProfile(const std::string& name) const;

    /// 获取全部 Profile 的稳定快照，按名称排序。
    std::vector<GestureProfile> getProfiles() const;

    /// 删除 Profile（用于持久化失败后的回滚）。
    bool removeProfile(const std::string& name);

    // ── 作用域规则 ───────────────────────────────────────────────────────

    ScopeRuleEngine& scopeRules() { return m_scopeRules; }

    // ── 识别器配置 ───────────────────────────────────────────────────────

    void setRecognizerConfig(const RecognizerConfig& config);

    // ── 轨迹可视化 ──────────────────────────────────────────────────────

    void setTrailCallback(TrailRenderCallback callback);

    /// 取消当前追踪（左键、Esc、钩子熔断时调用）
    void cancelActiveGesture();

    /// 从配置文件加载全部状态
    void loadFromConfig();

    /// 保存全部状态到配置文件
    bool saveToConfig();

    /// 同步全生命周期触发掩码到底层鼠标钩子
    void syncTriggerMask();

    /// 根据当前 Profile 状态表计算综合触发掩码
    uint32_t computeTriggerMask() const;

private:
    GestureEngine();
    GestureEngine(const GestureEngine&) = delete;
    GestureEngine& operator=(const GestureEngine&) = delete;

    /// 处理从鼠标钩子传来的事件，返回 true 表示拦截该事件
    bool onMouseEvent(const MouseEvent& event);

    /// 开始手势追踪
    void beginTracking(const MouseEvent& event);
    /// 更新手势轨迹
    void updateTracking(const MouseEvent& event);
    /// 结束手势追踪
    void endTracking(const MouseEvent& event);
    /// 取消手势追踪
    void cancelTracking();

    /// 处理按住触发键期间的滚轮手势 (RButton + WheelUp/Down)
    bool handleWheelGesture(const MouseEvent& event);

    /// 执行匹配到的手势动作
    void executeActionInternal(
        const GestureAction& action,
        HWND startWindow,
        HWND previousForeground,
        POINT startPt,
        POINT endPt);

    /// 把被吞掉的触发键点击补发出去 (无有效手势时还原右键/中键的正常点击)
    void reinjectTriggerClick(MouseEventType triggerOverride = MouseEventType::Move);

    void enqueueAction(GestureAction action, std::string traceId, HWND targetWindow = nullptr);
    void actionWorkerLoop(std::stop_token stopToken);

    /// 根据当前前台窗口查找适用的 Profile
    std::optional<GestureProfile> resolveProfile(HWND hwnd) const;

    void installForegroundWatch();
    void uninstallForegroundWatch();
    void inspectForegroundIntegrity(HWND hwnd);
    static void CALLBACK foregroundWinEventProc(
        HWINEVENTHOOK hook, DWORD event, HWND hwnd,
        LONG idObject, LONG idChild, DWORD eventThread, DWORD eventTime);

    // 组件
    GestureRecognizer m_recognizer;
    ScopeRuleEngine m_scopeRules;
    std::unordered_map<std::string, GestureProfile> m_profiles;
    mutable std::shared_mutex m_profileMutex;

    // 状态
    std::atomic<GestureState> m_state{GestureState::Idle};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_recordingMode{false};
    std::atomic<bool> m_trailVisible{true};
    std::atomic<bool> m_autoBypassFullscreen{true};
    std::atomic<GestureTargetMode> m_targetMode{GestureTargetMode::UnderPointer};
    std::atomic<int> m_initialTimeoutMs{500};
    std::atomic<int> m_minSegmentDistance{24};
    HWND m_gestureStartWindow = nullptr;  // 手势开始时光标下的顶层窗口（含设置窗）
    HWND m_previousForeground = nullptr;  // 手势开始时的前台窗口（跳过覆盖层）
    HWND m_lastExternalWindow = nullptr;  // 最近一次命中的目标窗口
    POINT m_gestureStartPt{};
    POINT m_gestureEndPt{};
    std::string m_gestureTraceId;         // 当前手势的 TraceId, 贯穿 按下→移动→抬起→执行
    uint8_t m_gestureModifiers = 0;       // 手势开始时的修饰键状态
    ScreenEdgeZone m_gestureEdgeZone = ScreenEdgeZone::None; // 手势开始时的屏幕边缘区域
    std::chrono::steady_clock::time_point m_trackingStartTime; // 手势开始追踪的时间点
    std::optional<GestureProfile> m_activeProfile;       // 本次手势激活的 Profile 缓存
    std::optional<GestureProfile> m_fallbackProfile;     // 本次手势 fallback 的默认 Profile 缓存
    std::vector<Direction> m_lastRecognizedDirections; // 缓存方向序列
    std::string m_lastLiveCode;
    std::string m_liveHeldLabel;
    bool m_liveHadMatch = false;
    DWORD m_liveMatchTick = 0;
    POINT m_lastLiveEvalPt{-1, -1};
    std::chrono::steady_clock::time_point m_lastLiveEvalTime{};
    std::vector<Direction> m_cachedLiveDirs;
    bool m_profileResolvedLocally = false;
    bool m_wheelExecutedDuringTracking = false; // 按住触发键期间是否执行过滚轮手势
    std::chrono::steady_clock::time_point m_lastDesktopSwitchTime{}; // 虚拟桌面切换节流时间戳
    PauseChangedCallback m_pauseChangedCallback;

    // 异步上下文解析 (Lazy Context Resolution)
    struct AsyncContextResult {
        bool disabled = false;
        HWND targetWindow = nullptr;
        std::optional<GestureProfile> profile;
    };
    struct ContextJob {
        uint64_t epoch = 0;
        HWND initialFg = nullptr;
        POINT startPt = {0, 0};
    };
    std::atomic<uint64_t> m_contextEpoch{0};
    std::atomic<bool> m_contextResolved{false};
    AsyncContextResult m_latestContextResult;
    ContextJob m_pendingContextJob;
    std::mutex m_contextMutex;
    std::condition_variable m_contextCv;
    std::mutex m_contextResultMutex;
    std::condition_variable m_contextResultCv;
    std::jthread m_contextWorker;

    void contextWorkerLoop(std::stop_token stopToken);
    AsyncContextResult resolveContextInternal(HWND initialFg, POINT startPt);

    // 轨迹可视化
    TrailRenderCallback m_trailCallback;
    mutable std::mutex m_callbackMutex;

    // 触发按键 (默认右键)
    std::atomic<MouseEventType> m_triggerDown{MouseEventType::RightDown};
    std::atomic<MouseEventType> m_triggerUp{MouseEventType::RightUp};
    // 一次手势从按下到抬起必须使用同一触发键，即使用户此时在设置页切换配置。
    MouseEventType m_activeTriggerDown = MouseEventType::RightDown;
    MouseEventType m_activeTriggerUp = MouseEventType::RightUp;

    std::mutex m_mutex;

    struct ActionJob {
        GestureAction action;
        std::string traceId;
        HWND targetWindow = nullptr;
    };
    std::mutex m_actionMutex;
    std::condition_variable_any m_actionCv;
    std::deque<ActionJob> m_actionQueue;
    std::jthread m_actionWorker;

    HWINEVENTHOOK m_foregroundHook = nullptr;
    std::mutex m_integrityWarnMutex;
    std::unordered_set<DWORD> m_warnedIntegrityPids;

    // 主动式防悬挂自愈看门狗
    std::mutex m_watchdogMutex;
    std::condition_variable m_watchdogCv;
    std::jthread m_watchdogWorker;
    void watchdogWorkerLoop(std::stop_token stopToken);
};

}  // namespace tools3000::gesture

#endif  // TOOLS3000_GESTURE_GESTUREENGINE_H
