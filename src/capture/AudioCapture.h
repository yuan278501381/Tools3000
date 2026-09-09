#pragma once

#ifndef TOOLS3000_CAPTURE_AUDIOCAPTURE_H
#define TOOLS3000_CAPTURE_AUDIOCAPTURE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tools3000::capture {

struct AudioCaptureOptions {
    bool systemAudio = false;
    bool microphone = false;
    std::string systemDeviceId;
    std::string microphoneDeviceId;
    float systemVolume = 1.0f;
    float microphoneVolume = 1.0f;
};

struct AudioDeviceInfo {
    std::string id;
    std::string name;
    bool systemAudio = false;
    bool defaultDevice = false;
};

struct AudioCaptureStatus {
    bool systemAudioActive = false;
    bool microphoneActive = false;
    std::uint64_t droppedFrames = 0;
    std::uint64_t discontinuities = 0;
    float systemPeak = 0.0f;
    float microphonePeak = 0.0f;
    float mixedPeak = 0.0f;
    bool systemMuted = false;
    bool microphoneMuted = false;
    std::uint64_t reconnectAttempts = 0;
    std::uint64_t reconnectSuccesses = 0;
    std::string error;
};

class AudioCapture {
public:
    static constexpr int SampleRate = 48000;
    static constexpr int Channels = 2;

    AudioCapture();
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    /// Starts requested WASAPI endpoints. Partial success is allowed.
    bool start(const AudioCaptureOptions& options);
    void stop() noexcept;
    void setPaused(bool paused);
    void setVolumes(float systemVolume, float microphoneVolume);
    void setSystemMuted(bool muted);
    void setMicrophoneMuted(bool muted);

    /// Reads exactly frameCount interleaved stereo float frames when available.
    bool readFrames(int frameCount, std::vector<float>& output);
    AudioCaptureStatus status() const;

    /// Enumerates active render and capture endpoints without starting a stream.
    static std::vector<AudioDeviceInfo> devices();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_AUDIOCAPTURE_H
