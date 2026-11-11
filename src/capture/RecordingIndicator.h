#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// RecordingIndicator — 录制状态指示器（悬浮工具条）
//
// 职责:
//   1. 录制中在屏幕顶部显示悬浮状态条
//   2. 动态测量并显示录制时间、超宽分辨率 (4K/8K)、多语言帧计数、暂停/停止按钮
//   3. 红色圆点闪烁动画提示录制中
//   4. 可拖拽移动
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_RECORDINGINDICATOR_H
#define TOOLS3000_CAPTURE_RECORDINGINDICATOR_H

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <functional>

namespace tools3000::capture {

/// 指示器按钮事件
using IndicatorAction = std::function<void()>;

/// 录制指示器自适应水平布局快照（逻辑像素，96 DPI 基准）
struct RecordingIndicatorLayout {
    D2D1_RECT_F statusRect{};
    D2D1_RECT_F timeRect{};
    D2D1_RECT_F resolutionRect{};
    D2D1_RECT_F frameRect{};
    RECT systemAudioBtn{};
    RECT microphoneBtn{};
    float separatorX = 0.0f;
    RECT snapshotBtn{};
    RECT pauseBtn{};
    RECT stopBtn{};
    int totalLogicalWidth = 380;
    int totalLogicalHeight = 40;
};

class RecordingIndicator {
public:
    static RecordingIndicator& instance();

    /// 初始化
    bool initialize(HINSTANCE hInstance);

    /// 关闭
    void shutdown();

    /// 显示指示器
    void show();

    /// 隐藏指示器
    void hide();

    /// 更新录制时间和帧数
    void update(double durationSec, int frameCount);

    /// 设置暂停状态
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }

    /// 设置录制选区几何信息（用于控制条智能吸附与常驻镂空发光呼吸线框）
    void setRecordingRegion(int x, int y, int width, int height, float cornerRadius = 0.0f);

    /// 清除录制选区
    void clearRecordingRegion();

    /// 获取录制状态条与录制选区避让矩形集合（供 ShortcutHintOverlay 碰撞避让）
    std::vector<RECT> avoidanceRects() const;

    /// 显示后台异步保存/封装状态（SAVING... / 保存中...），屏蔽录制控制按钮防重入
    void showSaving();
    bool isSaving() const noexcept { return m_saving; }

    /// 显示录制完成快捷响应浮岛卡片（支持一键打开文件夹、一键复制文件）
    void showCompleted(const std::string& filePath, double durationSec);
    bool isCompleted() const { return m_completed; }

    /// 响应全局明暗主题与强调色变更即时重绘
    void reloadTheme();

    /// 判定当前是否应使用中文指示器文案（遵循 /general/language 配置与系统语言回退）
    static bool isChineseLanguagePreferred();

    /// 按语言格式化累计帧计数文案（中文: "{}帧", 英文: "1 frame" / "{} frames"，严格区别于实时帧率 fps）
    static std::wstring formatFrameCounterText(int frames, bool isChinese);

    /// 基于 DirectWrite GetMetrics 动态测算录制条各字段与按钮布局（未启用音频时自动紧凑收缩，4K/8K 超宽零截断）
    RecordingIndicatorLayout computeRecordingBarLayout(const std::wstring& statusStr,
                                                       const std::wstring& timeStr,
                                                       const std::wstring& resStr,
                                                       const std::wstring& frameStr,
                                                       bool showSystemAudio = false,
                                                       bool showMicrophone = false,
                                                       bool countingDown = false) const;

    /// 设置回调
    void onPause(IndicatorAction action) { m_onPause = std::move(action); }
    void onStop(IndicatorAction action) { m_onStop = std::move(action); }
    void onSnapshot(IndicatorAction action) { m_onSnapshot = std::move(action); }
    void onSystemAudioMute(IndicatorAction action) { m_onSystemAudioMute = std::move(action); }
    void onMicrophoneMute(IndicatorAction action) { m_onMicrophoneMute = std::move(action); }

private:
    RecordingIndicator() = default;

    bool createWindow(HINSTANCE hInstance);
    bool createRenderResources();
    bool ensureDWriteReady() const;
    float measureLogicalTextWidth(const std::wstring& text) const;
    void ensureWindowLogicalWidth(int desiredLogicalWidth);
    void releaseRenderResources();
    void render();

    /// 格式化时间为 HH:MM:SS
    std::wstring formatDuration(double seconds) const;

    static LRESULT CALLBACK indicatorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    int m_logicalWidth = 380;
    bool m_paused = false;
    bool m_saving = false;
    bool m_completed = false;
    std::string m_completedFilePath;
    DWORD m_completedShowTick = 0;
    double m_duration = 0.0;
    int m_frames = 0;
    float m_systemAudioPeak = 0.0f;
    float m_microphonePeak = 0.0f;
    bool m_systemAudioActive = false;
    bool m_microphoneActive = false;
    bool m_systemAudioMuted = false;
    bool m_microphoneMuted = false;
    bool m_countingDown = false;
    int m_countdownRemaining = 0;
    bool m_storageWarning = false;
    std::int64_t m_estimatedRemainingSec = -1;
    bool m_performanceLimited = false;
    double m_effectiveFps = 0.0;
    bool m_isDragging = false;
    POINT m_dragOffset{};
    DWORD m_blinkTick = 0;    // 红点闪烁计时
    float m_dpiScale = 1.0f;

    // 录制态按键区域
    RECT m_pauseBtn{};
    RECT m_stopBtn{};
    RECT m_snapshotBtn{};
    RECT m_systemAudioBtn{};
    RECT m_microphoneBtn{};

    // 完成态按键区域
    RECT m_openFolderBtn{};
    RECT m_copyFileBtn{};
    RECT m_closeBtn{};

    // Hover 交互态
    bool m_hoverPause = false;
    bool m_hoverStop = false;
    bool m_hoverSnapshot = false;
    bool m_hoverSysAudio = false;
    bool m_hoverMic = false;
    bool m_hoverOpenFolder = false;
    bool m_hoverCopyFile = false;
    bool m_hoverClose = false;

    IndicatorAction m_onPause;
    IndicatorAction m_onStop;
    IndicatorAction m_onSnapshot;
    IndicatorAction m_onSystemAudioMute;
    IndicatorAction m_onMicrophoneMute;

    // D2D
    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_subTextBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_redDotBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_recordingAuraBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pausedAuraBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pausedDotBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_btnBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_btnHoverBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_audioBrush;
    mutable Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    mutable Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    mutable Microsoft::WRL::ComPtr<IDWriteTextFormat> m_subTextFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_btnTextFormat;

    // 常驻镂空发光呼吸线框
    bool createBorderWindow(HINSTANCE hInstance);
    void destroyBorderWindow();
    void updateBorderPulse();
    static LRESULT CALLBACK borderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_borderHwnd = nullptr;
    RECT m_recordingRegion{};
    float m_recordingCornerRadius = 0.0f;
    bool m_hasRecordingRegion = false;
    float m_borderPulse = 0.0f;

    // 视口居中 3-2-1 倒计时微动效窗口
    bool createCountdownWindow(HINSTANCE hInstance);
    void destroyCountdownWindow();
    void updateCountdownWindow();
    static LRESULT CALLBACK countdownWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_countdownHwnd = nullptr;
};

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_RECORDINGINDICATOR_H
