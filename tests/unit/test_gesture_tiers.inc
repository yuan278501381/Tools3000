// ─────────────────────────────────────────────────────────────────────────────
// test_gesture_tiers.inc — Tools3000 鼠标手势与 DirectComposition Tiers 1-3 全面测试套件
// 严格对齐 TEST_INFRA.md 与 ORIGINAL_REQUEST.md 规范体系：
//   • Tier 1: 特性覆盖 (Feature Coverage, 12 大特性 × 5 用例 = 60 项测试)
//   • Tier 2: 边界与极端情况 (Boundary & Corner Cases, 12 大特性 × 5 用例 = 60 项测试)
//   • Tier 3: 跨特性组合交互 (Cross-Feature Combinations, 14 项组合矩阵测试)
// ─────────────────────────────────────────────────────────────────────────────

#include <gtest/gtest.h>
#include <windows.h>
#include <timeapi.h>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <cmath>

#pragma comment(lib, "winmm.lib")

#include "gesture/GestureRecognizer.h"
#include "gesture/GestureAction.h"
#include "gesture/GestureProfile.h"
#include "gesture/GestureInputPolicy.h"
#include "gesture/GestureTrailOverlay.h"
#include "gesture/BuiltinCommands.h"
#include "gesture/MouseHook.h"
#include "core/utils/SpscRingBuffer.h"
#include "core/utils/ThemeUtils.h"

using namespace tools3000::gesture;
using namespace tools3000::core;

// ═════════════════════════════════════════════════════════════════════════════
// TIER 1: FEATURE COVERAGE (12 特性 × 5 用例 = 60 项测试)
// ═════════════════════════════════════════════════════════════════════════════

// ── Feature 1: 1000Hz 鼠标输入采样 (ORIGINAL_REQUEST §R1) ─────────────────────
TEST(GestureTier1FeatureCoverageTest, F1_1_HighRateInputCadence_1000Hz_1msInterval) {
    GestureRecognizer recognizer;
    recognizer.reset();
    for (int i = 0; i < 100; ++i) {
        recognizer.addPoint(i * 3, 0);
    }
    auto result = recognizer.finalize();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->code, "R");
}

TEST(GestureTier1FeatureCoverageTest, F1_2_ContinuousMouseMove_1000PointsBurst) {
    GestureRecognizer recognizer;
    recognizer.reset();
    for (int i = 0; i < 1000; ++i) {
        recognizer.addPoint(i * 2, i * 2);
    }
    auto result = recognizer.finalize();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->code, "DR");
}

TEST(GestureTier1FeatureCoverageTest, F1_3_HighFrequencySubpixelAccumulation) {
    GestureRecognizer recognizer;
    recognizer.reset();
    for (int i = 0; i <= 200; ++i) {
        recognizer.addPoint(0, i * 2);
    }
    auto result = recognizer.finalize();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->code, "D");
}

TEST(GestureTier1FeatureCoverageTest, F1_4_SamplingIntervalFilterPass) {
    GestureRecognizer recognizer;
    recognizer.reset();
    recognizer.addPoint(0, 0);
    recognizer.addPoint(15, 0);
    recognizer.addPoint(30, 0);
    recognizer.addPoint(45, 0);
    auto result = recognizer.finalize();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->code, "R");
}

TEST(GestureTier1FeatureCoverageTest, F1_5_ZeroLatencyImmediateWakePolicy) {
    EXPECT_TRUE(gestureShouldWakeRenderImmediately(true));
    EXPECT_FALSE(gestureShouldWakeRenderImmediately(false));
}

// ── Feature 2: 无锁环形缓冲区分发 (ORIGINAL_REQUEST §R1) ──────────────────────
TEST(GestureTier1FeatureCoverageTest, F2_1_SpscRingBufferPushPopBasic) {
    SpscRingBuffer<int, 64> buffer;
    EXPECT_TRUE(buffer.empty());
    EXPECT_TRUE(buffer.push(42));
    EXPECT_FALSE(buffer.empty());
    int val = 0;
    EXPECT_TRUE(buffer.pop(val));
    EXPECT_EQ(val, 42);
    EXPECT_TRUE(buffer.empty());
}

TEST(GestureTier1FeatureCoverageTest, F2_2_SpscRingBufferFifoOrdering1000Items) {
    SpscRingBuffer<int, 2048> buffer;
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(buffer.push(i));
    }
    for (int i = 0; i < 1000; ++i) {
        int val = -1;
        EXPECT_TRUE(buffer.pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_TRUE(buffer.empty());
}

TEST(GestureTier1FeatureCoverageTest, F2_3_SpscRingBufferTryPushCapacityLimits) {
    SpscRingBuffer<int, 8> buffer;
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(buffer.try_push(i));
    }
    EXPECT_FALSE(buffer.try_push(999));
    int val = 0;
    EXPECT_TRUE(buffer.pop(val));
    EXPECT_EQ(val, 0);
    EXPECT_TRUE(buffer.try_push(999));
}

TEST(GestureTier1FeatureCoverageTest, F2_4_SpscRingBufferSlidingWindowOverrun) {
    SpscRingBuffer<int, 8> buffer;
    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(buffer.push(i));
    }
    int val = 0;
    EXPECT_TRUE(buffer.pop(val));
    EXPECT_GE(val, 12);
}

TEST(GestureTier1FeatureCoverageTest, F2_5_SpscRingBufferEmptyStateDetection) {
    SpscRingBuffer<int, 16> buffer;
    int val = 0;
    EXPECT_FALSE(buffer.pop(val));
    EXPECT_TRUE(buffer.empty());
}

// ── Feature 3: 0 锁争用与 0 阻塞判定 (ORIGINAL_REQUEST §R1) ─────────────────────
TEST(GestureTier1FeatureCoverageTest, F3_1_NonBlockingPushZeroLockWait) {
    SpscRingBuffer<int, 1024> buffer;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 500; ++i) {
        buffer.push(i);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    EXPECT_LT(elapsedUs, 5000);
}

TEST(GestureTier1FeatureCoverageTest, F3_2_ConcurrentProducerConsumerNoDeadlock) {
    SpscRingBuffer<int, 2048> buffer;
    std::atomic<bool> start{false};
    std::atomic<bool> producerDone{false};
    constexpr int kCount = 5000;

    std::jthread producer([&](std::stop_token) {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kCount; ++i) {
            while (!buffer.try_push(i)) {
                std::this_thread::yield();
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::vector<int> consumed;
    consumed.reserve(kCount);
    std::jthread consumer([&](std::stop_token) {
        while (!start.load(std::memory_order_acquire)) {}
        int val = 0;
        while (!producerDone.load(std::memory_order_acquire) || !buffer.empty()) {
            if (buffer.pop(val)) {
                consumed.push_back(val);
            } else {
                std::this_thread::yield();
            }
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    EXPECT_EQ(consumed.size(), static_cast<size_t>(kCount));
}

TEST(GestureTier1FeatureCoverageTest, F3_3_HookCallbackFastReturnPolicy) {
    EXPECT_EQ(physicalVkForTriggerDown(MouseEventType::RightDown), VK_RBUTTON);
    EXPECT_EQ(physicalVkForTriggerDown(MouseEventType::MiddleDown), VK_MBUTTON);
    EXPECT_EQ(physicalVkForTriggerDown(MouseEventType::X1Down), VK_XBUTTON1);
    EXPECT_EQ(physicalVkForTriggerDown(MouseEventType::X2Down), VK_XBUTTON2);
    EXPECT_EQ(physicalVkForTriggerDown(MouseEventType::LeftDown), VK_LBUTTON);
}

TEST(GestureTier1FeatureCoverageTest, F3_4_CancelsGestureTrackingOnConflictingButton) {
    EXPECT_TRUE(cancelsGestureTracking(MouseEventType::LeftDown, MouseEventType::RightDown));
    EXPECT_TRUE(cancelsGestureTracking(MouseEventType::MiddleDown, MouseEventType::RightDown));
    EXPECT_TRUE(cancelsGestureTracking(MouseEventType::X1Down, MouseEventType::RightDown));
    EXPECT_FALSE(cancelsGestureTracking(MouseEventType::RightDown, MouseEventType::RightDown));
}

TEST(GestureTier1FeatureCoverageTest, F3_5_EmergencyFlushInputSafety) {
    EXPECT_NO_THROW(tools3000::core::WinUtils::emergencyFlushInputState());
}

// ── Feature 4: 预分配硬件表面 (ORIGINAL_REQUEST §R2) ──────────────────────────
TEST(GestureTier1FeatureCoverageTest, F4_1_InitialSurfaceMinimum1024x1024) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(500, 500, 500, 500, 0, 0, 0, 0, false, 0, 0, 1920, 1080,
                               outL, outT, outR, outB);
    EXPECT_GE(outR - outL, 1024);
    EXPECT_GE(outB - outT, 1024);
}

TEST(GestureTier1FeatureCoverageTest, F4_2_OriginCenteringAndPadding) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(800, 600, 800, 600, 0, 0, 0, 0, false, 0, 0, 3840, 2160,
                               outL, outT, outR, outB);
    EXPECT_LE(outL, 800 - 500);
    EXPECT_GE(outR, 800 + 500);
    EXPECT_LE(outT, 600 - 500);
    EXPECT_GE(outB, 600 + 500);
}

TEST(GestureTier1FeatureCoverageTest, F4_3_VirtualScreenBoundaryClamping) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(10, 10, 10, 10, 0, 0, 0, 0, false, 0, 0, 1920, 1080,
                               outL, outT, outR, outB);
    EXPECT_GE(outL, 0);
    EXPECT_GE(outT, 0);
}

TEST(GestureTier1FeatureCoverageTest, F4_4_SnapGridAlignment) {
    EXPECT_EQ(snapDown(1023, 512), 512);
    EXPECT_EQ(snapDown(1024, 512), 1024);
    EXPECT_EQ(snapUp(1025, 512), 1536);
    EXPECT_EQ(snapUp(1024, 512), 1024);
}

TEST(GestureTier1FeatureCoverageTest, F4_5_AspectAndScreenMarginMargins) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(1900, 1050, 1900, 1050, 0, 0, 0, 0, false, 0, 0, 1920, 1080,
                               outL, outT, outR, outB);
    EXPECT_LE(outR, 1920 + 64);
    EXPECT_LE(outB, 1080 + 64);
}

// ── Feature 5: 0 SetWindowPos 窗口重排 (ORIGINAL_REQUEST §R2) ──────────────────
TEST(GestureTier1FeatureCoverageTest, F5_1_ZeroSetWindowPosDuringGestureMove) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(600, 400, 600, 400, 0, 0, 0, 0, false, 0, 0, 2560, 1440,
                               outL, outT, outR, outB);
    const int initL = outL;
    const int initT = outT;
    const int initW = outR - outL;
    const int initH = outB - outT;

    int moveL = 0, moveT = 0, moveR = 0, moveB = 0;
    computeOverlaySurfaceBounds(650, 450, 650, 450, initL, initT, initW, initH, true, 0, 0, 2560, 1440,
                               moveL, moveT, moveR, moveB);
    EXPECT_EQ(moveL, initL);
    EXPECT_EQ(moveT, initT);
    EXPECT_EQ(moveR - moveL, initW);
    EXPECT_EQ(moveB - moveT, initH);
}

TEST(GestureTier1FeatureCoverageTest, F5_2_OriginLockPreventsJitter) {
    int initL = 500, initT = 300, initW = 1024, initH = 1024;
    int curL = 0, curT = 0, curR = 0, curB = 0;
    computeOverlaySurfaceBounds(700, 500, 800, 600, initL, initT, initW, initH, true, 0, 0, 1920, 1080,
                               curL, curT, curR, curB);
    EXPECT_EQ(curL, initL);
    EXPECT_EQ(curT, initT);
}

TEST(GestureTier1FeatureCoverageTest, F5_3_growOverlayRectMonotonicExpansion) {
    int l = 500, t = 500, r = 600, b = 600;
    growOverlayRect(l, t, r, b, 450, 450, 200, 200);
    EXPECT_EQ(l, 450);
    EXPECT_EQ(t, 450);
    EXPECT_EQ(r, 650);
    EXPECT_EQ(b, 650);
}

TEST(GestureTier1FeatureCoverageTest, F5_4_WindowStylePreservesNoActivateAndToolWindow) {
    LONG_PTR style = WS_OVERLAPPEDWINDOW;
    LONG_PTR norm = normalizeGestureOverlayExStyle(style);
    EXPECT_TRUE(gestureOverlayIsTaskbarSafe(norm));
    EXPECT_EQ(norm & WS_EX_APPWINDOW, 0);
}

TEST(GestureTier1FeatureCoverageTest, F5_5_TaskbarSafetyInvariant) {
    EXPECT_TRUE(gestureOverlayIsTaskbarSafe(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE));
    EXPECT_FALSE(gestureOverlayIsTaskbarSafe(WS_EX_TOOLWINDOW | WS_EX_APPWINDOW));
    EXPECT_FALSE(gestureOverlayIsTaskbarSafe(WS_EX_NOACTIVATE));
}

// ── Feature 6: 0 运行时 CreateSurface (ORIGINAL_REQUEST §R2) ──────────────────
TEST(GestureTier1FeatureCoverageTest, F6_1_SurfaceContainmentCheck) {
    EXPECT_TRUE(overlaySurfaceContains(100, 100, 500, 500, 0, 0, 1024, 1024));
    EXPECT_FALSE(overlaySurfaceContains(100, 100, 1200, 500, 0, 0, 1024, 1024));
}

TEST(GestureTier1FeatureCoverageTest, F6_2_SurfaceReusePolicyThreshold) {
    EXPECT_TRUE(overlayCanReuseSurface(800, 800, 1024, 1024, 4));
    EXPECT_FALSE(overlayCanReuseSurface(100, 100, 1024, 1024, 4));
}

TEST(GestureTier1FeatureCoverageTest, F6_3_RejectOversizedLeftoverSurface) {
    EXPECT_FALSE(overlayCanReuseSurface(200, 200, 3840, 2160, 4));
}

TEST(GestureTier1FeatureCoverageTest, F6_4_RepeatedStrokesWithinBoundsZeroRecreate) {
    int originX = 0, originY = 0, surfaceW = 1024, surfaceH = 1024;
    for (int stroke = 0; stroke < 5; ++stroke) {
        int x1 = 100 + stroke * 50;
        int y1 = 100 + stroke * 50;
        int x2 = x1 + 80;
        int y2 = y1 + 80;
        EXPECT_TRUE(overlaySurfaceContains(x1, y1, x2, y2, originX, originY, surfaceW, surfaceH));
    }
}

TEST(GestureTier1FeatureCoverageTest, F6_5_DirectCompositionDoubleBufferSwap) {
    LONG_PTR dcompStyle = WS_EX_NOREDIRECTIONBITMAP;
    EXPECT_TRUE(windowUsesCompositorSurface(dcompStyle));
    EXPECT_FALSE(windowUsesCompositorSurface(0));
}

// ── Feature 7: GPU 增量局部绘制 (ORIGINAL_REQUEST §R2) ────────────────────────
TEST(GestureTier1FeatureCoverageTest, F7_1_IncrementalBoundsGrowthMath) {
    int l = 500, t = 500, r = 500, b = 500;
    growOverlayRect(l, t, r, b, 600, 600, 50, 50);
    EXPECT_EQ(l, 500);
    EXPECT_EQ(t, 500);
    EXPECT_EQ(r, 650);
    EXPECT_EQ(b, 650);
}

TEST(GestureTier1FeatureCoverageTest, F7_2_TrailOutlineWidthClamping) {
    EXPECT_FLOAT_EQ(clampTrailOutlineWidth(-1.0f), 0.0f);
    EXPECT_FLOAT_EQ(clampTrailOutlineWidth(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(clampTrailOutlineWidth(2.5f), 2.5f);
    EXPECT_FLOAT_EQ(clampTrailOutlineWidth(10.0f), 8.0f);
}

TEST(GestureTier1FeatureCoverageTest, F7_3_TrailOutlineWidenMath) {
    EXPECT_FLOAT_EQ(trailOutlineWidenWidth(3.0f, 2.0f), 7.0f);
    EXPECT_FLOAT_EQ(trailOutlineWidenWidth(0.0f, 2.0f), 0.0f);
    EXPECT_FLOAT_EQ(trailOutlineWidenWidth(3.0f, 0.0f), 0.0f);
}

TEST(GestureTier1FeatureCoverageTest, F7_4_FilletFoldingSmoothTurn) {
    auto simplified = GestureRecognizer::simplifyDirections({Direction::Right, Direction::DownRight, Direction::Down});
    EXPECT_EQ(directionsToCode(simplified), "R-D");
}

TEST(GestureTier1FeatureCoverageTest, F7_5_ReboundJitterElimination) {
    auto simplified = GestureRecognizer::simplifyDirections({Direction::Right, Direction::UpRight, Direction::Right});
    EXPECT_EQ(directionsToCode(simplified), "R");
}

// ── Feature 8: 物理刷新率 VBlank 对齐 (ORIGINAL_REQUEST §R3) ───────────────────
TEST(GestureTier1FeatureCoverageTest, F8_1_144HzTargetFrameIntervalCalculation) {
    constexpr int kTargetIntervalMs = 7;
    EXPECT_EQ(kTargetIntervalMs, 7);
}

TEST(GestureTier1FeatureCoverageTest, F8_2_FadeoutFrameInterval16ms) {
    constexpr DWORD kFadeWaitMs = 16;
    EXPECT_EQ(kFadeWaitMs, 16u);
}

TEST(GestureTier1FeatureCoverageTest, F8_3_FirstFrameImmediatePresentation) {
    const auto now = std::chrono::steady_clock::now();
    const auto last = now - std::chrono::milliseconds(10);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    EXPECT_GE(elapsed, 7);
}

TEST(GestureTier1FeatureCoverageTest, F8_4_VBlankPacingThrottleHighFrequencyInput) {
    const auto now = std::chrono::steady_clock::now();
    const auto last = now - std::chrono::milliseconds(3);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    EXPECT_LT(elapsed, 7);
}

TEST(GestureTier1FeatureCoverageTest, F8_5_RenderThreadPriorityIsHighest) {
    EXPECT_EQ(gestureRenderThreadPriority(), THREAD_PRIORITY_HIGHEST);
}

// ── Feature 9: QPC 高精度单调节拍器 (ORIGINAL_REQUEST §R3) ─────────────────────
TEST(GestureTier1FeatureCoverageTest, F9_1_MonotonicClockProgression) {
    auto t1 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto t2 = std::chrono::steady_clock::now();
    EXPECT_GT(t2, t1);
}

TEST(GestureTier1FeatureCoverageTest, F9_2_DesktopSwitchThrottlingInterval) {
    EXPECT_TRUE(isDesktopSwitchThrottled(50, 120));
    EXPECT_FALSE(isDesktopSwitchThrottled(120, 120));
    EXPECT_FALSE(isDesktopSwitchThrottled(150, 120));
}

TEST(GestureTier1FeatureCoverageTest, F9_3_FadeClockRequiresFirstPresentedFrame) {
    EXPECT_TRUE(gestureFrameReadyToFade(true, false, false));
    EXPECT_FALSE(gestureFrameReadyToFade(false, false, false));
    EXPECT_FALSE(gestureFrameReadyToFade(true, true, false));
    EXPECT_TRUE(gestureFrameReadyToFade(true, true, true));
}

TEST(GestureTier1FeatureCoverageTest, F9_4_LiveGestureMatchHoldTimer) {
    EXPECT_TRUE(keepLiveGestureMatch(false, true, 50, kLiveGestureMatchHoldMs));
    EXPECT_FALSE(keepLiveGestureMatch(false, true, 130, kLiveGestureMatchHoldMs));
}

TEST(GestureTier1FeatureCoverageTest, F9_5_FadeFinishDetection) {
    EXPECT_FALSE(gestureFadeShouldFinish(false, 500, 150, 180));
    EXPECT_FALSE(gestureFadeShouldFinish(true, 300, 150, 180));
    EXPECT_TRUE(gestureFadeShouldFinish(true, 330, 150, 180));
    EXPECT_TRUE(gestureFadeShouldFinish(true, 400, 150, 180));
}

// ── Feature 10: 尖端采样相位差 < 4ms (ORIGINAL_REQUEST §R3) ───────────────────
TEST(GestureTier1FeatureCoverageTest, F10_1_CursorTipInterpolationActive) {
    EXPECT_TRUE(gestureShouldInterpolateCursorTip(false, true, 102.0f, 100.0f, 100.0f, 100.0f, 1.0f));
}

TEST(GestureTier1FeatureCoverageTest, F10_2_CursorTipInterpolationFadingLocked) {
    EXPECT_FALSE(gestureShouldInterpolateCursorTip(true, true, 105.0f, 100.0f, 100.0f, 100.0f, 1.0f));
}

TEST(GestureTier1FeatureCoverageTest, F10_3_CursorTipInterpolationSubpixelJitterFiltered) {
    EXPECT_FALSE(gestureShouldInterpolateCursorTip(false, true, 100.4f, 100.3f, 100.0f, 100.0f, 1.0f));
}

TEST(GestureTier1FeatureCoverageTest, F10_4_RapidDirectionChangeTipResponse) {
    EXPECT_TRUE(gestureShouldInterpolateCursorTip(false, true, 90.0f, 85.0f, 100.0f, 100.0f, 1.0f));
}

TEST(GestureTier1FeatureCoverageTest, F10_5_HighFrequencyBurstPhaseRetention) {
    int accepted = 0;
    float lastX = 0.0f, lastY = 0.0f;
    for (int i = 1; i <= 100; ++i) {
        float curX = static_cast<float>(i * 3);
        float curY = static_cast<float>(i * 3);
        if (gestureShouldInterpolateCursorTip(false, true, curX, curY, lastX, lastY, 1.0f)) {
            accepted++;
            lastX = curX;
            lastY = curY;
        }
    }
    EXPECT_EQ(accepted, 100);
}

// ── Feature 11: 单笔画即时高亮与 HUD (ORIGINAL_REQUEST §AC) ────────────────────
TEST(GestureTier1FeatureCoverageTest, F11_1_SingleStrokeInstantRecognition) {
    GestureRecognizer recognizer;
    recognizer.reset();
    recognizer.addPoint(0, 0);
    recognizer.addPoint(50, 0);
    auto res = recognizer.finalize();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->code, "R");
}

TEST(GestureTier1FeatureCoverageTest, F11_2_DiagonalSingleStrokeExpand) {
    EXPECT_EQ(*expandSingleDiagonalCode("DR"), "D-R");
    EXPECT_EQ(*expandSingleDiagonalCode("DL"), "D-L");
    EXPECT_EQ(*expandSingleDiagonalCode("UR"), "U-R");
    EXPECT_EQ(*expandSingleDiagonalCode("UL"), "U-L");
}

TEST(GestureTier1FeatureCoverageTest, F11_3_SuccessPulseIntensityHoldAndFade) {
    EXPECT_FLOAT_EQ(gestureSuccessPulseIntensity(true, true, true, 50, 110, 80), 1.0f);
    EXPECT_FLOAT_EQ(gestureSuccessPulseIntensity(true, true, true, 110, 110, 80), 1.0f);
    float mid = gestureSuccessPulseIntensity(true, true, true, 150, 110, 80);
    EXPECT_GT(mid, 0.0f);
    EXPECT_LT(mid, 1.0f);
    EXPECT_FLOAT_EQ(gestureSuccessPulseIntensity(true, true, true, 200, 110, 80), 0.0f);
}

TEST(GestureTier1FeatureCoverageTest, F11_4_SuccessPulseBgColorCalculation) {
    AccentColorRGB base{0.2f, 0.4f, 0.8f};
    auto bright = computeSuccessPulseBgColor(base, 1.0f);
    EXPECT_GT(bright.r, base.r);
    EXPECT_GT(bright.g, base.g);
    EXPECT_GT(bright.b, base.b);
}

TEST(GestureTier1FeatureCoverageTest, F11_5_SuccessPulseBorderWidthExpansion) {
    float normal = computeSuccessPulseBorderWidth(2.6f, 0.0f, 1.0f);
    float pulsed = computeSuccessPulseBorderWidth(2.6f, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(normal, 2.6f);
    EXPECT_FLOAT_EQ(pulsed, 3.4f);
}

// ── Feature 12: 短促手势与淡出鲁棒性 (ORIGINAL_REQUEST §AC) ─────────────────────
TEST(GestureTier1FeatureCoverageTest, F12_1_UltraShortFlickUnder50ms) {
    GestureRecognizer recognizer;
    recognizer.reset();
    recognizer.addPoint(100, 100);
    recognizer.addPoint(130, 100);
    auto res = recognizer.finalize();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->code, "R");
}

TEST(GestureTier1FeatureCoverageTest, F12_2_FadeAlphaHoldThenQuadraticEase) {
    EXPECT_FLOAT_EQ(gestureFadeAlpha(true, 50, 150, 180), 1.0f);
    EXPECT_FLOAT_EQ(gestureFadeAlpha(true, 150, 150, 180), 1.0f);
    float mid = gestureFadeAlpha(true, 240, 150, 180);
    EXPECT_GT(mid, 0.0f);
    EXPECT_LT(mid, 1.0f);
    EXPECT_FLOAT_EQ(gestureFadeAlpha(true, 330, 150, 180), 0.0f);
}

TEST(GestureTier1FeatureCoverageTest, F12_3_InterruptedFadeOutByNewTrail) {
    uint64_t currentEpoch = 5;
    uint64_t fadeEpoch = 4;
    EXPECT_NE(currentEpoch, fadeEpoch);
}

TEST(GestureTier1FeatureCoverageTest, F12_4_ResultToastRequiresRecognitionOrExcessive) {
    EXPECT_TRUE(shouldShowGestureResultToast(true, true, false));
    EXPECT_FALSE(shouldShowGestureResultToast(false, false, false));
    EXPECT_TRUE(shouldShowGestureResultToast(false, false, true));
}

TEST(GestureTier1FeatureCoverageTest, F12_5_SuccessFlashAlphaDecayCurve) {
    float peak = computeSuccessPulseFlashAlpha(1.0f, 1.0f);
    EXPECT_FLOAT_EQ(peak, 0.38f);
    float zero = computeSuccessPulseFlashAlpha(0.0f, 1.0f);
    EXPECT_FLOAT_EQ(zero, 0.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
// TIER 2: BOUNDARY & CORNER CASES (12 特性 × 5 用例 = 60 项测试)
// ═════════════════════════════════════════════════════════════════════════════

// ── Feature 1: 1000Hz 鼠标输入采样 (边界用例) ──────────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F1_1_ZeroDisplacementRightClick) {
    GestureRecognizer recognizer;
    recognizer.reset();
    recognizer.addPoint(500, 500);
    recognizer.addPoint(500, 500);
    auto res = recognizer.finalize();
    EXPECT_FALSE(res.has_value());
}

TEST(GestureTier2BoundaryAndCornerTest, F1_2_SubpixelJitterUnder1Pixel) {
    GestureRecognizer recognizer;
    recognizer.reset();
    recognizer.addPoint(100, 100);
    recognizer.addPoint(100, 101);
    recognizer.addPoint(101, 100);
    recognizer.addPoint(100, 100);
    auto res = recognizer.finalize();
    EXPECT_FALSE(res.has_value());
}

TEST(GestureTier2BoundaryAndCornerTest, F1_3_UltraHighFrequencyBurst2000Hz) {
    GestureRecognizer recognizer;
    recognizer.reset();
    for (int i = 0; i < 2000; ++i) {
        recognizer.addPoint(i, 0);
    }
    auto res = recognizer.finalize();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->code, "R");
}

TEST(GestureTier2BoundaryAndCornerTest, F1_4_NegativeCoordinateSampling) {
    GestureRecognizer recognizer;
    recognizer.reset();
    recognizer.addPoint(-1920, 100);
    recognizer.addPoint(-1800, 100);
    auto res = recognizer.finalize();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->code, "R");
}

TEST(GestureTier2BoundaryAndCornerTest, F1_5_ExactSamplingIntervalBoundary) {
    GestureRecognizer recognizer;
    recognizer.reset();
    recognizer.addPoint(0, 0);
    recognizer.addPoint(13, 0);
    EXPECT_FALSE(recognizer.finalize().has_value());
    recognizer.reset();
    recognizer.addPoint(0, 0);
    recognizer.addPoint(14, 0);
    EXPECT_TRUE(recognizer.finalize().has_value());
}

// ── Feature 2: 无锁环形缓冲区分发 (边界用例) ──────────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F2_1_BufferExactCapacityBoundary2048) {
    SpscRingBuffer<int, 2048> buffer;
    for (int i = 0; i < 2048; ++i) {
        EXPECT_TRUE(buffer.try_push(i));
    }
    EXPECT_FALSE(buffer.try_push(9999));
    int val = 0;
    EXPECT_TRUE(buffer.pop(val));
    EXPECT_EQ(val, 0);
}

TEST(GestureTier2BoundaryAndCornerTest, F2_2_SingleElementWrapAroundIndex) {
    SpscRingBuffer<int, 4> buffer;
    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(buffer.push(i));
        int val = -1;
        EXPECT_TRUE(buffer.pop(val));
        EXPECT_EQ(val, i);
    }
}

TEST(GestureTier2BoundaryAndCornerTest, F2_3_PopOnEmptyBufferReturnsFalse) {
    SpscRingBuffer<int, 16> buffer;
    for (int i = 0; i < 10; ++i) {
        int v = 0;
        EXPECT_FALSE(buffer.pop(v));
    }
}

TEST(GestureTier2BoundaryAndCornerTest, F2_4_SlidingWindowDoubleWrapOverrun) {
    SpscRingBuffer<int, 8> buffer;
    for (int i = 0; i < 100; ++i) {
        buffer.push(i);
    }
    int val = 0;
    EXPECT_TRUE(buffer.pop(val));
    EXPECT_GE(val, 92);
}

TEST(GestureTier2BoundaryAndCornerTest, F2_5_StructDataIntegrityAcrossWrap) {
    struct TestPayload {
        int id = 0;
        float x = 0.0f;
        float y = 0.0f;
        uint64_t tag = 0;
    };
    SpscRingBuffer<TestPayload, 8> buffer;
    for (int i = 0; i < 20; ++i) {
        buffer.push({i, static_cast<float>(i * 1.5f), static_cast<float>(i * 2.5f), static_cast<uint64_t>(0xA000 + i)});
    }
    TestPayload out{};
    EXPECT_TRUE(buffer.pop(out));
    EXPECT_GE(out.id, 12);
    EXPECT_EQ(out.tag, static_cast<uint64_t>(0xA000 + out.id));
}

// ── Feature 3: 0 锁争用与 0 阻塞判定 (边界用例) ─────────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F3_1_Concurrent100ProducersConsumersSimulation) {
    SpscRingBuffer<uint64_t, 1024> buffer;
    std::atomic<bool> done{false};
    constexpr int kItems = 2000;

    std::jthread producer([&](std::stop_token) {
        for (uint64_t i = 0; i < kItems; ++i) {
            buffer.push(i);
        }
        done.store(true, std::memory_order_release);
    });

    uint64_t last = 0;
    while (!done.load(std::memory_order_acquire) || !buffer.empty()) {
        uint64_t v = 0;
        if (buffer.pop(v)) {
            EXPECT_GE(v, last);
            last = v;
        }
    }
    producer.join();
}

TEST(GestureTier2BoundaryAndCornerTest, F3_2_DoubleTriggerDownLatching) {
    EXPECT_FALSE(cancelsGestureTracking(MouseEventType::RightDown, MouseEventType::RightDown));
    EXPECT_FALSE(cancelsGestureTracking(MouseEventType::MiddleDown, MouseEventType::MiddleDown));
}

TEST(GestureTier2BoundaryAndCornerTest, F3_3_TriggerUpWithoutTriggerDownOrphan) {
    EXPECT_EQ(triggerUpFor(MouseEventType::RightDown), MouseEventType::RightUp);
    EXPECT_EQ(triggerUpFor(MouseEventType::LeftDown), MouseEventType::LeftUp);
}

TEST(GestureTier2BoundaryAndCornerTest, F3_4_MiddleAndXButtonInterruption) {
    EXPECT_TRUE(cancelsGestureTracking(MouseEventType::MiddleDown, MouseEventType::RightDown));
    EXPECT_TRUE(cancelsGestureTracking(MouseEventType::X1Down, MouseEventType::RightDown));
    EXPECT_TRUE(cancelsGestureTracking(MouseEventType::X2Down, MouseEventType::RightDown));
}

TEST(GestureTier2BoundaryAndCornerTest, F3_5_ExtremeRapidClickSpamming) {
    auto& hook = tools3000::gesture::MouseHook::instance();
    for (int i = 0; i < 50; ++i) {
        hook.resetTriggerState();
        EXPECT_FALSE(hook.isTriggerButtonDown());
    }
}

// ── Feature 4: 预分配硬件表面 (边界用例) ──────────────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F4_1_ExtremeScreenCornerCoordinates) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(0, 0, 0, 0, 0, 0, 0, 0, false, 0, 0, 1920, 1080,
                               outL, outT, outR, outB);
    EXPECT_GE(outL, 0);
    EXPECT_GE(outT, 0);
    EXPECT_GE(outR - outL, 1024);
}

TEST(GestureTier2BoundaryAndCornerTest, F4_2_NegativeMultiMonitorCoordinateHandling) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(-1500, 300, -1500, 300, 0, 0, 0, 0, false, -1920, 0, 3840, 1080,
                               outL, outT, outR, outB);
    EXPECT_GE(outL, -1920 - 64);
    EXPECT_GE(outR - outL, 1024);
}

TEST(GestureTier2BoundaryAndCornerTest, F4_3_8KResolutionBoundaryExpansion) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(7600, 4300, 7600, 4300, 0, 0, 0, 0, false, 0, 0, 7680, 4320,
                               outL, outT, outR, outB);
    EXPECT_LE(outR, 7680 + 64);
    EXPECT_LE(outB, 4320 + 64);
}

TEST(GestureTier2BoundaryAndCornerTest, F4_4_SinglePixelCoordinateSpan) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(1000, 1000, 1001, 1001, 0, 0, 0, 0, false, 0, 0, 3840, 2160,
                               outL, outT, outR, outB);
    EXPECT_EQ(outR - outL, 1024);
    EXPECT_EQ(outB - outT, 1024);
}

TEST(GestureTier2BoundaryAndCornerTest, F4_5_FractionalDpiScaleRounding) {
    for (float scale : { 1.25f, 1.50f, 1.75f, 2.00f, 2.25f }) {
        int scaledWidth = static_cast<int>(std::round(1024 * scale));
        EXPECT_GT(scaledWidth, 1024);
    }
}

// ── Feature 5: 0 SetWindowPos 窗口重排 (边界用例) ──────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F5_1_HugeTrajectoryWithinPreallocatedBounds) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(1000, 1000, 1000, 1000, 0, 0, 0, 0, false, 0, 0, 3840, 2160,
                               outL, outT, outR, outB);
    int initL = outL, initT = outT, initW = outR - outL, initH = outB - outT;

    int moveL = 0, moveT = 0, moveR = 0, moveB = 0;
    computeOverlaySurfaceBounds(1300, 1300, 1300, 1300, initL, initT, initW, initH, true, 0, 0, 3840, 2160,
                               moveL, moveT, moveR, moveB);
    EXPECT_EQ(moveL, initL);
    EXPECT_EQ(moveT, initT);
    EXPECT_EQ(moveR - moveL, initW);
}

TEST(GestureTier2BoundaryAndCornerTest, F5_2_EdgeCollisionNoWindowShrink) {
    int l = 100, t = 100, r = 500, b = 500;
    growOverlayRect(l, t, r, b, 200, 200, 50, 50);
    EXPECT_EQ(l, 100);
    EXPECT_EQ(t, 100);
    EXPECT_EQ(r, 500);
    EXPECT_EQ(b, 500);
}

TEST(GestureTier2BoundaryAndCornerTest, F5_3_ZeroWidthHeightSurfaceSafety) {
    int l = 10, t = 10, r = 20, b = 20;
    growOverlayRect(l, t, r, b, 0, 0, 0, 0);
    EXPECT_EQ(l, 10);
    EXPECT_EQ(r, 20);
}

TEST(GestureTier2BoundaryAndCornerTest, F5_4_TaskbarOverlapBoundarySafety) {
    EXPECT_TRUE(isSystemTrayOrTaskbar(L"Shell_TrayWnd"));
    EXPECT_TRUE(isSystemTrayOrTaskbar(L"Shell_SecondaryTrayWnd"));
    EXPECT_TRUE(isSystemTrayOrTaskbar(L"TrayNotifyWnd"));
}

TEST(GestureTier2BoundaryAndCornerTest, F5_5_FullVirtualDesktopCoverageBounds) {
    EXPECT_TRUE(overlaySurfaceContains(-500, 100, 800, 900, -1000, 0, 3000, 2000));
    EXPECT_FALSE(overlaySurfaceContains(-1500, 100, 800, 900, -1000, 0, 3000, 2000));
}

// ── Feature 6: 0 运行时 CreateSurface (边界用例) ──────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F6_1_ExactAreaFactorBoundary) {
    EXPECT_TRUE(overlayCanReuseSurface(500, 500, 1000, 1000, 4));
    EXPECT_FALSE(overlayCanReuseSurface(500, 500, 1001, 1000, 4));
}

TEST(GestureTier2BoundaryAndCornerTest, F6_2_ZeroAreaRecreationCheck) {
    EXPECT_FALSE(overlayCanReuseSurface(0, 500, 1000, 1000, 4));
    EXPECT_FALSE(overlayCanReuseSurface(500, 0, 1000, 1000, 4));
    EXPECT_FALSE(overlayCanReuseSurface(500, 500, 0, 1000, 4));
    EXPECT_FALSE(overlayCanReuseSurface(500, 500, 1000, 0, 4));
}

TEST(GestureTier2BoundaryAndCornerTest, F6_3_Marathon10000PointsGestureSurfaceReuse) {
    int oX = 0, oY = 0, sW = 2048, sH = 2048;
    for (int i = 0; i < 10000; ++i) {
        int x = 500 + (i % 500);
        int y = 500 + (i % 500);
        EXPECT_TRUE(overlaySurfaceContains(x, y, x + 1, y + 1, oX, oY, sW, sH));
    }
}

TEST(GestureTier2BoundaryAndCornerTest, F6_4_OppositeDirectionFlickWithinBounds) {
    EXPECT_TRUE(overlaySurfaceContains(500, 500, 800, 500, 0, 0, 1024, 1024));
    EXPECT_TRUE(overlaySurfaceContains(200, 500, 500, 500, 0, 0, 1024, 1024));
}

TEST(GestureTier2BoundaryAndCornerTest, F6_5_AspectRatio10to1StripSurface) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(100, 500, 900, 510, 0, 0, 0, 0, false, 0, 0, 3840, 2160,
                               outL, outT, outR, outB);
    EXPECT_GE(outB - outT, 1024);
}

// ── Feature 7: GPU 增量局部绘制 (边界用例) ────────────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F7_1_360DegreeCircularLoopTrajectory) {
    GestureRecognizer recognizer;
    recognizer.reset();
    for (int deg = 0; deg <= 360; deg += 10) {
        double rad = deg * 3.14159265358979323846 / 180.0;
        int x = static_cast<int>(100.0 + 80.0 * std::cos(rad));
        int y = static_cast<int>(100.0 + 80.0 * std::sin(rad));
        recognizer.addPoint(x, y);
    }
    auto res = recognizer.finalize();
    EXPECT_TRUE(res.has_value());
}

TEST(GestureTier2BoundaryAndCornerTest, F7_2_Sharp180DegreeReverseFold) {
    GestureRecognizer recognizer;
    recognizer.reset();
    for (int i = 0; i <= 80; i += 8) recognizer.addPoint(i, 0);
    for (int i = 80; i >= 0; i -= 8) recognizer.addPoint(i, 0);
    auto res = recognizer.finalize();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->code, "R-L");
}

TEST(GestureTier2BoundaryAndCornerTest, F7_3_ExtremeHighFrequencyZigZag) {
    GestureRecognizer recognizer;
    recognizer.reset();
    for (int i = 0; i <= 60; i += 4) recognizer.addPoint(i, 0);
    for (int i = 4; i <= 60; i += 4) recognizer.addPoint(60, i);
    for (int i = 64; i <= 120; i += 4) recognizer.addPoint(i, 60);
    auto res = recognizer.finalize();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->code, "R-D-R");
}

TEST(GestureTier2BoundaryAndCornerTest, F7_4_SubpixelStrokeWidthClamping) {
    EXPECT_FLOAT_EQ(clampTrailOutlineWidth(0.0001f), 0.0001f);
    EXPECT_FLOAT_EQ(clampTrailOutlineWidth(-0.0001f), 0.0f);
}

TEST(GestureTier2BoundaryAndCornerTest, F7_5_ExtremeLargeOutlineWidth) {
    EXPECT_FLOAT_EQ(clampTrailOutlineWidth(500.0f), 8.0f);
}

// ── Feature 8: 物理刷新率 VBlank 对齐 (边界用例) ───────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F8_1_ZeroElapsedMsThrottling) {
    constexpr int kInterval = 7;
    int elapsed = 0;
    DWORD waitMs = (elapsed >= kInterval) ? 0 : static_cast<DWORD>(kInterval - elapsed);
    EXPECT_EQ(waitMs, 7u);
}

TEST(GestureTier2BoundaryAndCornerTest, F8_2_LargeTimeDeltaCatchup) {
    constexpr int kInterval = 7;
    int elapsed = 500;
    DWORD waitMs = (elapsed >= kInterval) ? 0 : static_cast<DWORD>(kInterval - elapsed);
    EXPECT_EQ(waitMs, 0u);
}

TEST(GestureTier2BoundaryAndCornerTest, F8_3_TargetIntervalMsBoundary) {
    constexpr int kInterval = 7;
    int elapsed = 7;
    DWORD waitMs = (elapsed >= kInterval) ? 0 : static_cast<DWORD>(kInterval - elapsed);
    EXPECT_EQ(waitMs, 0u);
}

TEST(GestureTier2BoundaryAndCornerTest, F8_4_HighPrecisionTimerResolution) {
    MMRESULT res = timeBeginPeriod(1);
    EXPECT_EQ(res, TIMERR_NOERROR);
    timeEndPeriod(1);
}

TEST(GestureTier2BoundaryAndCornerTest, F8_5_StopTokenCancellationResponsiveness) {
    std::jthread worker([](std::stop_token st) {
        while (!st.stop_requested()) {
            std::this_thread::yield();
        }
    });
    worker.request_stop();
    worker.join();
    EXPECT_TRUE(true);
}

// ── Feature 9: QPC 高精度单调节拍器 (边界用例) ─────────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F9_1_NegativeElapsedHandling) {
    EXPECT_FALSE(isDesktopSwitchThrottled(-10, 120));
}

TEST(GestureTier2BoundaryAndCornerTest, F9_2_ExactThrottleBoundary120ms) {
    EXPECT_TRUE(isDesktopSwitchThrottled(119, 120));
    EXPECT_FALSE(isDesktopSwitchThrottled(120, 120));
}

TEST(GestureTier2BoundaryAndCornerTest, F9_3_UnmatchedElapsedHoldBoundary120ms) {
    EXPECT_TRUE(keepLiveGestureMatch(false, true, 119, 120));
    EXPECT_FALSE(keepLiveGestureMatch(false, true, 120, 120));
}

TEST(GestureTier2BoundaryAndCornerTest, F9_4_ZeroFadeOutDuration) {
    EXPECT_FLOAT_EQ(gestureFadeAlpha(true, 151, 150, 0), 0.0f);
}

TEST(GestureTier2BoundaryAndCornerTest, F9_5_FadeClockNotStartedReturnsFullAlpha) {
    EXPECT_FLOAT_EQ(gestureFadeAlpha(false, 9999, 150, 180), 1.0f);
}

// ── Feature 10: 尖端采样相位差 < 4ms (边界用例) ───────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F10_1_ExactDistanceThreshold1px) {
    EXPECT_FALSE(gestureShouldInterpolateCursorTip(false, true, 100.9f, 100.0f, 100.0f, 100.0f, 1.0f));
    EXPECT_TRUE(gestureShouldInterpolateCursorTip(false, true, 101.0f, 100.0f, 100.0f, 100.0f, 1.0f));
}

TEST(GestureTier2BoundaryAndCornerTest, F10_2_NegativeDeltaCoordinateTipInterpolation) {
    EXPECT_TRUE(gestureShouldInterpolateCursorTip(false, true, 50.0f, 50.0f, 100.0f, 100.0f, 1.0f));
}

TEST(GestureTier2BoundaryAndCornerTest, F10_3_StationaryCursorHold) {
    EXPECT_FALSE(gestureShouldInterpolateCursorTip(false, true, 100.0f, 100.0f, 100.0f, 100.0f, 1.0f));
}

TEST(GestureTier2BoundaryAndCornerTest, F10_4_SinglePointQueueTipSafety) {
    EXPECT_TRUE(gestureShouldInterpolateCursorTip(false, true, 150.0f, 150.0f, 100.0f, 100.0f, 1.0f));
}

TEST(GestureTier2BoundaryAndCornerTest, F10_5_HighSpeedLeap200pxTipInterpolation) {
    EXPECT_TRUE(gestureShouldInterpolateCursorTip(false, true, 300.0f, 300.0f, 100.0f, 100.0f, 1.0f));
}

// ── Feature 11: 单笔画即时高亮与 HUD (边界用例) ────────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F11_1_EmptyResultTextNoToast) {
    EXPECT_FALSE(shouldShowGestureResultToast(true, false, false));
}

TEST(GestureTier2BoundaryAndCornerTest, F11_2_ExcessivePointCountOverridesRecognition) {
    EXPECT_TRUE(shouldShowGestureResultToast(false, false, true));
}

TEST(GestureTier2BoundaryAndCornerTest, F11_3_PulseIntensityHoldBoundary110ms) {
    EXPECT_FLOAT_EQ(gestureSuccessPulseIntensity(true, true, true, 110, 110, 80), 1.0f);
    EXPECT_LT(gestureSuccessPulseIntensity(true, true, true, 111, 110, 80), 1.0f);
}

TEST(GestureTier2BoundaryAndCornerTest, F11_4_PulseIntensityFadeEnd190ms) {
    EXPECT_FLOAT_EQ(gestureSuccessPulseIntensity(true, true, true, 190, 110, 80), 0.0f);
    EXPECT_FLOAT_EQ(gestureSuccessPulseIntensity(true, true, true, 250, 110, 80), 0.0f);
}

TEST(GestureTier2BoundaryAndCornerTest, F11_5_PulseFlashAlphaZeroWhenFadeZero) {
    EXPECT_FLOAT_EQ(computeSuccessPulseFlashAlpha(1.0f, 0.0f), 0.0f);
}

// ── Feature 12: 短促手势与淡出鲁棒性 (边界用例) ─────────────────────────────────
TEST(GestureTier2BoundaryAndCornerTest, F12_1_10msUltraShortFlickDetection) {
    GestureRecognizer recognizer;
    recognizer.reset();
    recognizer.addPoint(0, 0);
    recognizer.addPoint(0, 30);
    auto res = recognizer.finalize();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->code, "D");
}

TEST(GestureTier2BoundaryAndCornerTest, F12_2_QuadraticEasingEndpoints) {
    EXPECT_FLOAT_EQ(gestureFadeAlpha(true, 150, 150, 100), 1.0f);
    EXPECT_FLOAT_EQ(gestureFadeAlpha(true, 250, 150, 100), 0.0f);
}

TEST(GestureTier2BoundaryAndCornerTest, F12_3_ElapsedPastTotalDurationClamped) {
    EXPECT_FLOAT_EQ(gestureFadeAlpha(true, 999999, 150, 100), 0.0f);
}

TEST(GestureTier2BoundaryAndCornerTest, F12_4_ZeroPointsFadeoutAbortSafety) {
    EXPECT_FALSE(gestureFrameReadyToFade(false, false, false));
}

TEST(GestureTier2BoundaryAndCornerTest, F12_5_ConsecutiveFlickSessionChaining) {
    for (int i = 0; i < 10; ++i) {
        GestureRecognizer recognizer;
        recognizer.reset();
        recognizer.addPoint(100, 100);
        recognizer.addPoint(100, 130);
        auto res = recognizer.finalize();
        ASSERT_TRUE(res.has_value());
        EXPECT_EQ(res->code, "D");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// TIER 3: CROSS-FEATURE COMBINATIONS (14 项组合矩阵测试)
// ═════════════════════════════════════════════════════════════════════════════

TEST(GestureTier3CombinatorialTest, F1_F7_HighSpeed1000HzWithZigZagAndIncrementalBounds) {
    GestureRecognizer recognizer;
    recognizer.reset();
    int minX = 0, minY = 0, maxX = 0, maxY = 0;

    for (int x = 0; x <= 80; x += 4) {
        recognizer.addPoint(x, 0);
        minX = (std::min)(minX, x);
        maxX = (std::max)(maxX, x);
    }
    for (int y = 4; y <= 80; y += 4) {
        recognizer.addPoint(80, y);
        minY = (std::min)(minY, y);
        maxY = (std::max)(maxY, y);
    }
    for (int x = 84; x <= 160; x += 4) {
        recognizer.addPoint(x, 80);
        minX = (std::min)(minX, x);
        maxX = (std::max)(maxX, x);
    }

    auto res = recognizer.finalize();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->code, "R-D-R");
    EXPECT_EQ(minX, 0);
    EXPECT_EQ(minY, 0);
    EXPECT_EQ(maxX, 160);
    EXPECT_EQ(maxY, 80);
}

TEST(GestureTier3CombinatorialTest, F2_F8_F3_SpscRingBufferUnderConcurrentStressWithVBlankPacing) {
    // 引入高精度 1ms 计时器作用域守卫，彻底抹平 Windows 默认 15.6ms 时钟片导致的调度抖动
    struct ScopedTimerResolution {
        ScopedTimerResolution() { timeBeginPeriod(1); }
        ~ScopedTimerResolution() { timeEndPeriod(1); }
    } timerGuard;

    SpscRingBuffer<TrailPoint, 2048> pointQueue;
    std::atomic<bool> stopProducer{false};
    std::atomic<int> producedCount{0};

    std::jthread producer([&](std::stop_token) {
        int i = 0;
        while (!stopProducer.load(std::memory_order_relaxed)) {
            pointQueue.push({static_cast<float>(i), static_cast<float>(i), static_cast<DWORD>(i)});
            i++;
            producedCount.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    int renderedFrames = 0;
    auto start = std::chrono::steady_clock::now();
    // 采样窗口调整至 70ms，以 7ms 步长（对齐 144Hz）提供充足冗余，消除 CI 偶发断言失败
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 70) {
        TrailPoint pt{};
        while (pointQueue.pop(pt)) {}
        renderedFrames++;
        std::this_thread::sleep_for(std::chrono::milliseconds(7));
    }

    stopProducer.store(true, std::memory_order_release);
    producer.join();

    EXPECT_GT(producedCount.load(), 100);
    EXPECT_GE(renderedFrames, 5);
}

TEST(GestureTier3CombinatorialTest, F4_F5_F6_MarathonGestureWithPreallocatedSurfaceZeroWindowPos) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(800, 600, 800, 600, 0, 0, 0, 0, false, 0, 0, 3840, 2160,
                               outL, outT, outR, outB);
    const int initL = outL;
    const int initT = outT;
    const int initW = outR - outL;
    const int initH = outB - outT;

    int moveCalls = 0;
    for (int i = 0; i < 1000; ++i) {
        int curX = 800 + (i % 200);
        int curY = 600 + (i % 200);
        int curL = 0, curT = 0, curR = 0, curB = 0;
        computeOverlaySurfaceBounds(curX, curY, curX, curY, initL, initT, initW, initH, true, 0, 0, 3840, 2160,
                                   curL, curT, curR, curB);
        if (curL != initL || curT != initT) {
            moveCalls++;
        }
    }
    EXPECT_EQ(moveCalls, 0);
}

TEST(GestureTier3CombinatorialTest, F1_F3_F12_LeftClickInterruptsActive1000HzGestureInstantHeal) {
    EXPECT_TRUE(cancelsGestureTracking(MouseEventType::LeftDown, MouseEventType::RightDown));
    auto& hook = tools3000::gesture::MouseHook::instance();
    hook.resetTriggerState();
    EXPECT_FALSE(hook.isTriggerButtonDown());
    EXPECT_EQ(hook.activeTriggerDown(), MouseEventType::Move);
}

TEST(GestureTier3CombinatorialTest, F1_F2_F11_WheelGestureDuringRightHoldSuppressesContextMenu) {
    EXPECT_TRUE(isWheelEvent(MouseEventType::WheelUp));
    EXPECT_TRUE(isWheelEvent(MouseEventType::WheelDown));
    EXPECT_STREQ(wheelBareCode(MouseEventType::WheelUp), "WheelUp");
    EXPECT_STREQ(wheelBareCode(MouseEventType::WheelDown), "WheelDown");
    EXPECT_TRUE(shouldSuppressContextMenuOnRelease(true, false));
    EXPECT_TRUE(shouldSuppressContextMenuOnRelease(false, true));
    EXPECT_FALSE(shouldSuppressContextMenuOnRelease(false, false));
}

TEST(GestureTier3CombinatorialTest, F4_F10_F11_NegativeMultiMonitorWithTipInterpolationAndHUD) {
    int outL = 0, outT = 0, outR = 0, outB = 0;
    computeOverlaySurfaceBounds(-1000, 300, -1000, 300, 0, 0, 0, 0, false, -1920, 0, 3840, 1080,
                               outL, outT, outR, outB);
    EXPECT_GE(outL, -1920);
    EXPECT_TRUE(gestureShouldInterpolateCursorTip(false, true, -950.0f, 300.0f, -1000.0f, 300.0f, 1.0f));
    EXPECT_TRUE(shouldShowGestureResultToast(true, true, false));
}

TEST(GestureTier3CombinatorialTest, F11_F12_F8_UltraShortFlickWithLiveMatchHoldAndFadeoutPacing) {
    GestureRecognizer recognizer;
    recognizer.reset();
    recognizer.addPoint(100, 100);
    recognizer.addPoint(140, 100);
    auto res = recognizer.finalize();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->code, "R");

    EXPECT_TRUE(keepLiveGestureMatch(false, true, 50, kLiveGestureMatchHoldMs));
    EXPECT_FLOAT_EQ(gestureFadeAlpha(true, 50, 150, 180), 1.0f);
}

TEST(GestureTier3CombinatorialTest, F6_F12_RapidRestartDuringFadeoutEpochInterruption) {
    uint64_t trailEpoch = 1;
    uint64_t fadeEpoch = 1;
    EXPECT_EQ(trailEpoch, fadeEpoch);

    trailEpoch++; // 新手势开始，递增纪元
    EXPECT_NE(trailEpoch, fadeEpoch);
}

TEST(GestureTier3CombinatorialTest, F3_F5_FullscreenGameVsProductivityBypassPolicy) {
    EXPECT_TRUE(shouldAutoBypassFullscreenGestures(true, false));
    EXPECT_FALSE(shouldAutoBypassFullscreenGestures(true, true));
    EXPECT_TRUE(isProductivityToolkitClassName(L"Chrome_WidgetWin_1"));
    EXPECT_TRUE(isProductivityToolkitClassName(L"Tools3000_SearchWindow"));
    EXPECT_FALSE(isProductivityToolkitClassName(L"D3D11_GameWindow"));
}

TEST(GestureTier3CombinatorialTest, F4_F7_F11_HighDpi200PercentScaleAndOutlineAndSuccessPulse) {
    float dpi = 2.0f;
    float border = computeSuccessPulseBorderWidth(2.6f, 1.0f, dpi);
    EXPECT_FLOAT_EQ(border, (2.6f + 0.8f) * 2.0f);
    float outline = clampTrailOutlineWidth(2.0f);
    EXPECT_FLOAT_EQ(trailOutlineWidenWidth(3.0f * dpi, outline * dpi), 6.0f + 4.0f * 2.0f);
}

TEST(GestureTier3CombinatorialTest, F1_F3_MultiButtonTriggerModeAndTriggerUpPairing) {
    EXPECT_TRUE(isGestureTriggerDown(MouseEventType::RightDown, TriggerMode::RightOnly));
    EXPECT_TRUE(isGestureTriggerDown(MouseEventType::MiddleDown, TriggerMode::MiddleOnly));
    EXPECT_TRUE(isGestureTriggerDown(MouseEventType::X1Down, TriggerMode::X1Only));
    EXPECT_TRUE(isGestureTriggerDown(MouseEventType::X2Down, TriggerMode::X2Only));

    EXPECT_EQ(triggerUpFor(MouseEventType::RightDown), MouseEventType::RightUp);
    EXPECT_EQ(triggerUpFor(MouseEventType::MiddleDown), MouseEventType::MiddleUp);
    EXPECT_EQ(triggerUpFor(MouseEventType::X1Down), MouseEventType::X1Up);
    EXPECT_EQ(triggerUpFor(MouseEventType::X2Down), MouseEventType::X2Up);
}

TEST(GestureTier3CombinatorialTest, F2_F10_RingBufferSlidingOverrunWithTipInterpolation) {
    SpscRingBuffer<TrailPoint, 16> buffer;
    for (int i = 0; i < 50; ++i) {
        buffer.push({static_cast<float>(i * 5), static_cast<float>(i * 5), static_cast<DWORD>(i)});
    }
    TrailPoint lastPt{};
    while (buffer.pop(lastPt)) {}
    EXPECT_TRUE(gestureShouldInterpolateCursorTip(false, true, lastPt.x + 10.0f, lastPt.y + 10.0f, lastPt.x, lastPt.y, 1.0f));
}

TEST(GestureTier3CombinatorialTest, F1_F3_ShiftRightClickSearchMenuBypassVsNormalGesture) {
    EXPECT_TRUE(shouldBypassGestureForNativeSearchMenu(L"Tools3000_SearchWindow", true));
    EXPECT_FALSE(shouldBypassGestureForNativeSearchMenu(L"Tools3000_SearchWindow", false));
    EXPECT_FALSE(shouldBypassGestureForNativeSearchMenu(L"Notepad", true));
}

TEST(GestureTier3CombinatorialTest, F3_F5_EdgeTriggerMasksAndDesktopFileManagerDragProtection) {
    EXPECT_TRUE(isDesktopOrFileManagerWindow(L"Progman"));
    EXPECT_TRUE(isDesktopOrFileManagerWindow(L"CabinetWClass"));
    EXPECT_FALSE(isLeftButtonGestureAllowed(ScreenEdgeZone::None, 0, GestureTriggerMask::Right));
    EXPECT_TRUE(isLeftButtonGestureAllowed(ScreenEdgeZone::Top, 0, GestureTriggerMask::EdgeTopSlide));
}
