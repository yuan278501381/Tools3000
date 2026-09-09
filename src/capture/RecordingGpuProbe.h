#pragma once

#include <cstdint>
#include <string>

namespace tools3000::capture {

/// Read-only capability report for the opt-in GPU encoder experiment. It does
/// not alter capture or encoding ownership; ScreenRecorder keeps the CPU path
/// as the production backend until a complete hardware backend is enabled.
struct RecordingGpuProbe {
    bool d3d11Available = false;
    std::uint32_t d3d11FeatureLevel = 0;
    std::uint32_t adapterLuidLow = 0;
    std::int32_t adapterLuidHigh = 0;
    bool mediaFoundationHardwareMftAvailable = false;
    bool ffmpegD3d11vaCompiled = false;
    std::string d3d11Error;
    std::string mediaFoundationError;
    std::string ffmpegError;

    bool canExperiment() const noexcept {
        return d3d11Available &&
            (mediaFoundationHardwareMftAvailable || ffmpegD3d11vaCompiled);
    }
};

RecordingGpuProbe probeRecordingGpuPath() noexcept;

}  // namespace tools3000::capture
