#pragma once

#ifndef TOOLS3000_CAPTURE_CURSOROVERLAY_H
#define TOOLS3000_CAPTURE_CURSOROVERLAY_H

#include "capture/CaptureBackend.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace tools3000::capture {

class CursorOverlay {
public:
    class Patch {
    public:
        Patch() = default;
        ~Patch() { restore(); }
        Patch(const Patch&) = delete;
        Patch& operator=(const Patch&) = delete;
        Patch(Patch&& other) noexcept;
        Patch& operator=(Patch&& other) noexcept;

        void restore() noexcept;
        /// 提交修改：放弃复原缓冲，使光标与点击特效永久保留在帧缓冲中（适用于单次截图）
        void commit() noexcept {
            m_frameData = nullptr;
            m_original.clear();
        }
        bool empty() const noexcept { return m_frameData == nullptr; }

    private:
        friend class CursorOverlay;
        std::uint8_t* m_frameData = nullptr;
        int m_stride = 0;
        int m_bytesPerPixel = 0;
        int m_x = 0;
        int m_y = 0;
        int m_width = 0;
        int m_height = 0;
        std::vector<std::uint8_t> m_original;
    };

    CursorOverlay() = default;
    ~CursorOverlay();
    CursorOverlay(const CursorOverlay&) = delete;
    CursorOverlay& operator=(const CursorOverlay&) = delete;

    /// Temporarily composites the system cursor and click feedback into a frame.
    /// The returned RAII patch restores the original pixels before the frame is unmapped.
    Patch apply(CaptureFrameView& frame, const CaptureRegion& screenRegion,
                bool includeCursor, bool showClickEffects, bool showClickZoom = true);

    /// Permanently composites the system cursor into a frame (for screenshots).
    void applyPermanent(CaptureFrameView& frame, const CaptureRegion& screenRegion,
                        bool includeCursor = true);

    /// Fast precomputed Euclidean distance lookup (< 65536)
    static float fastDistance(int d2);

    /// Calculate macOS / Screen Studio style spring-damped micro-zoom scale [0.0..1.0] -> [0.78..1.38..1.00]
    static float calculateClickZoomScale(float progress);

    /// Test hook to simulate a click ripple
    void triggerClickForTesting(POINT pt, bool isRight = false);

private:
    bool updateCursorImage(HCURSOR cursor);
    bool ensureSurfaces(int width, int height);
    void destroySurfaces() noexcept;
    static void blendPixel(CaptureFrameView& frame, int x, int y,
                           std::uint8_t red, std::uint8_t green,
                           std::uint8_t blue, std::uint8_t alpha);

    HCURSOR m_cachedCursor = nullptr;
    int m_cursorWidth = 0;
    int m_cursorHeight = 0;
    int m_hotspotX = 0;
    int m_hotspotY = 0;
    std::vector<std::uint8_t> m_cursorBgra;

    HDC m_blackDc = nullptr;
    HDC m_whiteDc = nullptr;
    HBITMAP m_blackBitmap = nullptr;
    HBITMAP m_whiteBitmap = nullptr;
    HGDIOBJ m_previousBlackBitmap = nullptr;
    HGDIOBJ m_previousWhiteBitmap = nullptr;
    std::uint32_t* m_blackBits = nullptr;
    std::uint32_t* m_whiteBits = nullptr;
    int m_surfaceWidth = 0;
    int m_surfaceHeight = 0;

    struct ClickRipple {
        POINT point{};
        std::chrono::steady_clock::time_point started{};
        bool isRight = false;
    };

    bool m_leftButtonDown = false;
    bool m_rightButtonDown = false;
    std::vector<ClickRipple> m_activeRipples;
};

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_CURSOROVERLAY_H
