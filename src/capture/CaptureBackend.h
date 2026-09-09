#pragma once

#ifndef TOOLS3000_CAPTURE_CAPTUREBACKEND_H
#define TOOLS3000_CAPTURE_CAPTUREBACKEND_H

#include "capture/ScreenCapture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tools3000::capture {

enum class CapturePixelFormat {
    Bgr24,
    Bgra32,
};

struct CaptureFrameView {
    std::uint8_t* data = nullptr;
    int stride = 0;
    int width = 0;
    int height = 0;
    CapturePixelFormat format = CapturePixelFormat::Bgr24;
};

struct CaptureBackendInfo {
    std::string id;
    std::string name;
    bool available = false;
    bool accelerated = false;
    std::string unavailableReason;
};

class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;
    virtual const CaptureBackendInfo& info() const noexcept = 0;
    virtual bool initialize(const CaptureRegion& region, std::string& error) = 0;
    virtual bool capture(CaptureFrameView& frame, std::string& error) = 0;
    /// Releases a mapped frame view. Must be called after every successful capture.
    virtual void releaseFrame() noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

#include <functional>

/// Ordered by preference. A backend advertised as available must be creatable.
std::vector<CaptureBackendInfo> captureBackendCapabilities();

/// Creates the best available backend. The factory is the only policy point,
/// allowing WGC/Desktop Duplication to be added without changing the encoder.
std::unique_ptr<ICaptureBackend> createCaptureBackend();

/// Creates a synthetic memory capture backend for headless testing or validation.
std::unique_ptr<ICaptureBackend> createMemoryCaptureBackend(CapturePixelFormat format = CapturePixelFormat::Bgr24);

/// Test hook to inject mock or memory backends into capture pipelines (ScrollCapture, ScreenRecorder).
using CaptureBackendFactory = std::function<std::unique_ptr<ICaptureBackend>()>;
void setCaptureBackendFactoryForTesting(CaptureBackendFactory factory);

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_CAPTUREBACKEND_H
