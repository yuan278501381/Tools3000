#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// WindowElementDetector.h — 世界级截图自动捕捉窗口与控件引擎 (MECE 四维架构)
//
// 核心架构 (Mutually Exclusive, Collectively Exhaustive):
//   维度 1 [零卡顿拓扑快照] : 冻屏瞬间 1 次预采集全桌面可见窗口 Z-Order、精确无阴影
//                             外框 (DWMWA_EXTENDED_FRAME_BOUNDS)、内容客户区、深层
//                             递归子 HWND 与真实圆角偏好；鼠标移动 0ms 纯内存命中。
//   维度 2 [懒缓存空间剪枝 UIA] : 独立后台 MTA 线程 + UIA2 60ms 硬超时防卡死；基于
//                             维度 1 命中的底层真实 HWND，调用 ElementFromHandle +
//                             CacheRequest 自顶向下按点裁剪展开并会话级缓存，彻底免
//                             疫全屏遮罩遮挡，二次移动 0.05ms 纯内存微秒级命中。
//   维度 3 [视觉卡片补全]   : 当鼠标落入纯图像/Canvas/游戏/远控等无障碍盲区时，在冻结
//                             画布局部 ROI 内检测高置信度闭合矩形卡片作为补充层级。
//   维度 4 [归一化与动效]   : 面积严格升序、IoU/边差智能去重、语义优先级仲裁、自适应
//                             控件/窗口/全屏圆角与版本号防撕裂异步回调。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_WINDOWELEMENTDETECTOR_H
#define TOOLS3000_CAPTURE_WINDOWELEMENTDETECTOR_H

#include <windows.h>
#include <opencv2/core.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tools3000::capture {

/// 异步控件细化完成通知消息（由后台线程 PostMessage 至截图遮罩窗口）
constexpr UINT WM_CAPTURE_ELEMENT_REFINED = WM_APP + 76;

/// 探测候选层级来源类型
enum class DetectedCandidateKind {
    VisionRegion = 0, // 冻结画布局部视觉闭合区块（无障碍盲区补全）
    UiaElement,       // UI Automation 现代控件/网页/Electron/WinUI3/WPF/Qt 元素
    Win32Child,       // Win32 深层递归子 HWND 控件
    WindowClient,     // 顶层窗口纯内容客户区（剔除标题栏与边框）
    WindowFrame,      // 顶层可见窗口 DWM 无阴影精确外框
    MonitorDisplay,   // 光标所在单块显示器
    VirtualDesktop,   // 虚拟桌面全屏兜底
};

/// 归一化后的单个层级候选框（rect 为相对虚拟桌面左上角的局部像素坐标）
struct DetectedElementCandidate {
    RECT rect{};
    std::wstring description;
    float cornerRadius = 0.0f; // 逻辑圆角半径 (未乘 DPI 缩放因子)
    DetectedCandidateKind kind = DetectedCandidateKind::WindowFrame;
    int zPriority = 0;         // 近似重合合并时的语义仲裁权重，越大越优先
};

/// 异步细化结果载荷（由 UI 线程在收到 WM_CAPTURE_ELEMENT_REFINED 时消费）
struct AsyncRefinedHierarchyResult {
    uint64_t generation = 0;
    POINT queryScreenPt{};
    std::vector<DetectedElementCandidate> candidates;
};

/// 纯函数：确定性层级归一化、裁剪、严格面积升序与近似重合智能去重
/// 可直接由单元测试独立调用验证所有边界条件
std::vector<DetectedElementCandidate> normalizeDetectedHierarchy(
    std::vector<DetectedElementCandidate> rawCandidates,
    int screenWidth,
    int screenHeight,
    std::size_t maxLayers = 10);

/// 纯函数：在冻结画布局部 ROI 内执行高置信度视觉闭合矩形卡片探测
/// 仅在 UIA/Win32 最内层候选面积过大（如纯图片/远控/Canvas）时触发；置信度不足返回 false
bool detectVisualCardRegionAtPoint(
    const cv::Mat& frozenScreen,
    POINT localCursor,
    RECT boundingLimit,
    RECT& outCardRect);

/// 纯函数：计算两个矩形的交并比 (Intersection over Union, 0.0 ~ 1.0)
double computeRectIoU(const RECT& a, const RECT& b) noexcept;

/// 会话级窗口与现代控件自动捕捉引擎
class WindowElementDetector {
public:
    WindowElementDetector();
    ~WindowElementDetector();

    WindowElementDetector(const WindowElementDetector&) = delete;
    WindowElementDetector& operator=(const WindowElementDetector&) = delete;

    /// 启动新一轮截图会话：在冻屏时刻单次预采集全桌面可见窗口 Z-Order 拓扑快照
    void beginSession(
        HWND excludeOverlayHwnd,
        int virtualOriginX,
        int virtualOriginY,
        int screenWidth,
        int screenHeight,
        const cv::Mat* frozenScreen);

    /// 结束当前会话：立即递增代际作废在途异步任务，并在后台线程释放全部 UIA COM 缓存
    void endSession();

    /// 零卡顿同步查询（O(N) 纯内存命中 Z-Order 快照 + 懒缓存子 HWND + 客户区 + 外框 + 显示器）
    /// 可安全在 1000Hz WM_MOUSEMOVE 热路径直接调用，并输出命中的底层真实顶层 HWND
    std::vector<DetectedElementCandidate> querySync(POINT screenPt, HWND* outTopHwnd = nullptr);

    /// 发起后台异步精细探测（懒缓存空间剪枝 UIA + 无障碍盲区视觉卡片补全）
    /// 返回本次请求的 generation；完成后通过 PostMessage(notifyHwnd, WM_CAPTURE_ELEMENT_REFINED, ...) 投递
    uint64_t requestAsyncRefine(
        HWND notifyHwnd,
        POINT screenPt,
        HWND topHwnd,
        const std::vector<DetectedElementCandidate>& baseSyncCandidates);

    /// 在 UI 线程中尝试取回最新的异步精细结果（仅当结果存在且 generation 仍有效时返回 true）
    bool consumeRefinedResult(AsyncRefinedHierarchyResult& outResult);

    /// 获取当前最新代际号
    uint64_t currentGeneration() const noexcept {
        return m_generation.load(std::memory_order_acquire);
    }

private:
    struct CachedChildHwnd {
        HWND hwnd = nullptr;
        RECT localRect{};
        std::wstring description;
        float cornerRadius = 4.0f;
    };

    struct WindowSnapshot {
        HWND hwnd = nullptr;
        RECT screenFrameRect{};     // 屏幕物理坐标外框
        RECT localFrameRect{};      // 相对虚拟桌面左上角外框
        RECT localClientRect{};     // 相对虚拟桌面左上角客户区
        bool hasDistinctClient = false;
        std::wstring title;
        std::wstring className;
        std::wstring frameDescription;
        std::wstring clientDescription;
        float frameCornerRadius = 0.0f;
        bool isTaskbar = false;
        bool childrenScanned = false;
        std::vector<CachedChildHwnd> children;
    };

    struct AsyncRequest {
        uint64_t generation = 0;
        HWND notifyHwnd = nullptr;
        HWND topHwnd = nullptr;
        POINT screenPt{};
        RECT topLocalBounds{};
        std::vector<DetectedElementCandidate> baseSyncCandidates;
    };

    struct UiaSessionCache; // PIMPL，隐藏 UIAutomation COM 头依赖

    void ensureWorkerStarted();
    void workerLoop();
    void scanChildHwndsForWindow(WindowSnapshot& win);
    static float detectWindowCornerRadius(HWND hwnd, const RECT& screenFrame, bool isTaskbar);
    static std::wstring classifyWin32ClassName(const std::wstring& className, bool isTopLevel, bool isZh);
    static std::wstring formatCandidateDesc(const std::wstring& typeLabel, const std::wstring& title, const RECT& rc);

    // 会话几何快照（仅在 UI 线程读写，零锁）
    HWND m_excludeHwnd = nullptr;
    int m_originX = 0;
    int m_originY = 0;
    int m_screenW = 0;
    int m_screenH = 0;
    std::vector<WindowSnapshot> m_windows;

    // 冻结画布浅拷贝引用（供后台视觉卡片兜底只读访问，随会话重置释放）
    std::mutex m_frozenMutex;
    cv::Mat m_frozenCopy;

    // 异步后台探测管线（版本防撕裂 + 线程安全队列）
    std::atomic<uint64_t> m_generation{0};
    std::mutex m_workerMutex;
    std::condition_variable m_workerCv;
    bool m_workerRunning = false;
    bool m_shutdown = false;
    bool m_clearCacheRequested = false;
    bool m_hasPendingRequest = false;
    AsyncRequest m_pendingRequest{};
    std::thread m_workerThread;

    // 已就绪的异步结果槽（单槽最新覆盖，彻底消除积压）
    std::mutex m_resultMutex;
    bool m_hasReadyResult = false;
    AsyncRefinedHierarchyResult m_readyResult{};
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_WINDOWELEMENTDETECTOR_H
