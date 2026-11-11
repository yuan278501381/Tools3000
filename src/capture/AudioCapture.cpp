#include "capture/AudioCapture.h"

#include "core/logger/Logger.h"

#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace tools3000::capture {
namespace {

using Microsoft::WRL::ComPtr;
constexpr int MixBlockFrames = AudioCapture::SampleRate / 100; // 10 ms
constexpr std::size_t MaxQueuedSamples = AudioCapture::SampleRate *
    AudioCapture::Channels * 2; // 2 seconds

std::string hrText(HRESULT value) {
    char buffer[16]{};
    sprintf_s(buffer, "0x%08X", static_cast<unsigned>(value));
    return buffer;
}

std::wstring utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          value.data(), static_cast<int>(value.size()),
                                          nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string wideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          value.data(), static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count,
                        nullptr, nullptr);
    return result;
}

AVSampleFormat sampleFormat(const WAVEFORMATEX& format) {
    WORD tag = format.wFormatTag;
    GUID subtype{};
    if (tag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        subtype = extended.SubFormat;
        if (subtype == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (subtype == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32) return AV_SAMPLE_FMT_FLT;
    if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16) return AV_SAMPLE_FMT_S16;
    if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 32) return AV_SAMPLE_FMT_S32;
    if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 8) return AV_SAMPLE_FMT_U8;
    return AV_SAMPLE_FMT_NONE;
}

struct Endpoint {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* format = nullptr;
    SwrContext* resampler = nullptr;
    HANDLE event = nullptr;
    std::vector<float> fifo;
    std::vector<float> converted;
    std::vector<std::uint8_t> silence;
    std::size_t readOffset = 0;
    std::array<float, AudioCapture::Channels> lastSample{0.0f, 0.0f};
    bool primed = false;
    bool inUnderrun = true;
    bool active = false;

    void resetBufferState() noexcept {
        fifo.clear();
        readOffset = 0;
        lastSample = {0.0f, 0.0f};
        primed = false;
        inUnderrun = true;
    }

    void close() noexcept {
        if (client) client->Stop();
        capture.Reset();
        client.Reset();
        if (resampler) swr_free(&resampler);
        if (format) CoTaskMemFree(format);
        if (event) CloseHandle(event);
        format = nullptr;
        event = nullptr;
        fifo.clear();
        converted.clear();
        silence.clear();
        readOffset = 0;
        lastSample = {0.0f, 0.0f};
        primed = false;
        inUnderrun = true;
        active = false;
    }
};

}  // namespace

struct AudioCapture::Impl {
    using Clock = std::chrono::steady_clock;

    struct ReconnectState {
        bool previouslyActive = false;
        bool scheduled = false;
        unsigned failedAttempts = 0;
        Clock::time_point nextAttempt{};
    };

    AudioCaptureOptions options;
    Endpoint system;
    Endpoint microphone;
    std::jthread worker;
    HANDLE wakeEvent = nullptr;
    std::atomic<bool> paused{false};
    std::atomic<std::uint64_t> syncResetEpoch{0};
    std::atomic<std::int64_t> requestedSyncQpc{0};
    std::atomic<float> systemVolume{1.0f};
    std::atomic<float> microphoneVolume{1.0f};
    std::atomic<bool> systemMuted{false};
    std::atomic<bool> microphoneMuted{false};
    mutable std::mutex outputMutex;
    std::vector<float> output;
    std::size_t outputOffset = 0;
    mutable std::mutex statusMutex;
    AudioCaptureStatus captureStatus;
    std::mutex startupMutex;
    std::condition_variable startupCv;
    bool startupComplete = false;

    static std::int64_t queryQpc() noexcept {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return counter.QuadPart;
    }

    static std::int64_t queryQpcFreq() noexcept {
        static const std::int64_t freq = []() {
            LARGE_INTEGER f{};
            return (QueryPerformanceFrequency(&f) && f.QuadPart > 0) ? f.QuadPart : 10'000'000LL;
        }();
        return freq;
    }

    ~Impl() { stop(); }

    bool initializeEndpoint(IMMDeviceEnumerator* enumerator, Endpoint& endpoint,
                            EDataFlow flow, ERole role, bool loopback,
                            std::string_view requestedDeviceId,
                            std::string& error) {
        error.clear();
        endpoint.close();
        ComPtr<IMMDevice> device;
        HRESULT result = E_FAIL;
        if (!requestedDeviceId.empty()) {
            const auto wideId = utf8ToWide(requestedDeviceId);
            if (!wideId.empty()) result = enumerator->GetDevice(wideId.c_str(), &device);
            if (FAILED(result)) {
                error = "selected audio device unavailable; using the default device";
                device.Reset();
            }
        }
        if (!device) result = enumerator->GetDefaultAudioEndpoint(flow, role, &device);
        if (FAILED(result)) {
            error = "default audio endpoint unavailable (" + hrText(result) + ")";
            return false;
        }
        result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(endpoint.client.GetAddressOf()));
        if (FAILED(result)) {
            error = "IAudioClient activation failed (" + hrText(result) + ")";
            return false;
        }
        result = endpoint.client->GetMixFormat(&endpoint.format);
        if (FAILED(result) || !endpoint.format) {
            error = "GetMixFormat failed (" + hrText(result) + ")";
            endpoint.close();
            return false;
        }
        const AVSampleFormat inputFormat = sampleFormat(*endpoint.format);
        if (inputFormat == AV_SAMPLE_FMT_NONE || endpoint.format->nChannels == 0 ||
            endpoint.format->nSamplesPerSec == 0) {
            error = "unsupported WASAPI mix format";
            endpoint.close();
            return false;
        }

        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        result = endpoint.client->Initialize(
            AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, endpoint.format, nullptr);
        if (FAILED(result)) {
            error = "WASAPI shared stream initialization failed (" + hrText(result) + ")";
            endpoint.close();
            return false;
        }
        endpoint.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!endpoint.event || FAILED(endpoint.client->SetEventHandle(endpoint.event))) {
            error = "WASAPI event setup failed";
            endpoint.close();
            return false;
        }
        result = endpoint.client->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(endpoint.capture.GetAddressOf()));
        if (FAILED(result)) {
            error = "IAudioCaptureClient unavailable (" + hrText(result) + ")";
            endpoint.close();
            return false;
        }

        AVChannelLayout inputLayout{};
        AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
        if (endpoint.format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            endpoint.format->cbSize >= 22) {
            const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(*endpoint.format);
            if (extended.dwChannelMask != 0) {
                av_channel_layout_from_mask(&inputLayout, extended.dwChannelMask);
            }
        }
        if (inputLayout.nb_channels != endpoint.format->nChannels) {
            av_channel_layout_uninit(&inputLayout);
            av_channel_layout_default(&inputLayout, endpoint.format->nChannels);
        }
        const int swrResult = swr_alloc_set_opts2(
            &endpoint.resampler, &outputLayout, AV_SAMPLE_FMT_FLT, SampleRate,
            &inputLayout, inputFormat, endpoint.format->nSamplesPerSec, 0, nullptr);
        av_channel_layout_uninit(&inputLayout);
        if (swrResult < 0 || !endpoint.resampler || swr_init(endpoint.resampler) < 0) {
            error = "audio resampler initialization failed";
            endpoint.close();
            return false;
        }
        result = endpoint.client->Start();
        if (FAILED(result)) {
            error = "WASAPI stream start failed (" + hrText(result) + ")";
            endpoint.close();
            return false;
        }
        endpoint.active = true;
        return true;
    }

    bool drainEndpoint(Endpoint& endpoint, std::string& error) {
        if (!endpoint.active) return false;
        while (true) {
            UINT32 packetFrames = 0;
            const HRESULT nextResult = endpoint.capture->GetNextPacketSize(&packetFrames);
            if (nextResult == AUDCLNT_E_DEVICE_INVALIDATED ||
                nextResult == AUDCLNT_E_RESOURCES_INVALIDATED) {
                endpoint.close();
                error = "audio device was disconnected";
                return false;
            }
            if (FAILED(nextResult) || packetFrames == 0) return SUCCEEDED(nextResult);
            BYTE* data = nullptr;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            const HRESULT result = endpoint.capture->GetBuffer(
                &data, &packetFrames, &flags, &devicePosition, &qpcPosition);
            if (result == AUDCLNT_E_DEVICE_INVALIDATED || result == AUDCLNT_E_RESOURCES_INVALIDATED) {
                endpoint.close();
                error = "audio device was disconnected";
                return false;
            }
            if (FAILED(result)) return false;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                std::lock_guard lock(statusMutex);
                ++captureStatus.discontinuities;
            }

            const std::uint8_t* source = data;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !source) {
                endpoint.silence.resize(
                    static_cast<std::size_t>(packetFrames) * endpoint.format->nBlockAlign);
                std::fill(endpoint.silence.begin(), endpoint.silence.end(), 0);
                if (sampleFormat(*endpoint.format) == AV_SAMPLE_FMT_U8) {
                    std::fill(endpoint.silence.begin(), endpoint.silence.end(), 128);
                }
                source = endpoint.silence.data();
            }
            const int maxFrames = swr_get_out_samples(endpoint.resampler, packetFrames);
            endpoint.converted.resize(static_cast<std::size_t>(maxFrames) * Channels);
            std::uint8_t* outputPlanes[]{reinterpret_cast<std::uint8_t*>(endpoint.converted.data())};
            const std::uint8_t* inputPlanes[]{source};
            const int convertedFrames = swr_convert(
                endpoint.resampler, outputPlanes, maxFrames, inputPlanes, packetFrames);
            endpoint.capture->ReleaseBuffer(packetFrames);
            if (convertedFrames > 0) {
                endpoint.fifo.insert(
                    endpoint.fifo.end(), endpoint.converted.begin(),
                    endpoint.converted.begin() +
                        static_cast<std::ptrdiff_t>(convertedFrames * Channels));
            }
        }
        return true;
    }

    bool initializeMicrophone(IMMDeviceEnumerator* enumerator, std::string& error) {
        if (initializeEndpoint(enumerator, microphone, eCapture, eCommunications,
                               false, options.microphoneDeviceId, error)) {
            return true;
        }
        return initializeEndpoint(enumerator, microphone, eCapture, eConsole,
                                  false, options.microphoneDeviceId, error);
    }

    void updateStatus(std::string_view systemError, std::string_view microphoneError) {
        std::lock_guard statusLock(statusMutex);
        captureStatus.systemAudioActive = system.active;
        captureStatus.microphoneActive = microphone.active;
        captureStatus.error.clear();
        if (!systemError.empty() && !microphoneError.empty()) {
            captureStatus.error = std::string(systemError) + "; " +
                std::string(microphoneError);
        } else if (!systemError.empty()) {
            captureStatus.error = systemError;
        } else if (!microphoneError.empty()) {
            captureStatus.error = microphoneError;
        }
    }

    void scheduleReconnectIfNeeded(const Endpoint& endpoint, ReconnectState& state,
                                   Clock::time_point now) {
        if (endpoint.active) {
            state.previouslyActive = true;
            state.scheduled = false;
            state.failedAttempts = 0;
        } else if (state.previouslyActive && !state.scheduled) {
            state.scheduled = true;
            state.nextAttempt = now;
        }
    }

    template <typename Initializer>
    void tryReconnect(Endpoint& endpoint, ReconnectState& state,
                      std::string& error, std::string_view label,
                      Clock::time_point now, Initializer&& initialize) {
        if (!state.scheduled || endpoint.active || now < state.nextAttempt) return;
        {
            std::lock_guard statusLock(statusMutex);
            ++captureStatus.reconnectAttempts;
        }
        if (initialize()) {
            state.scheduled = false;
            state.failedAttempts = 0;
            state.previouslyActive = true;
            {
                std::lock_guard statusLock(statusMutex);
                ++captureStatus.reconnectSuccesses;
            }
            LOG_INFO("WASAPI {} endpoint reconnected", label);
            return;
        }

        // 1, 2, 4, 8, 16, 30... seconds. The cap prevents both hot-looping and
        // excessively slow recovery after a dock/headset is plugged back in.
        const unsigned exponent = std::min(state.failedAttempts, 5U);
        const unsigned delaySeconds = std::min(1U << exponent, 30U);
        ++state.failedAttempts;
        state.nextAttempt = now + std::chrono::seconds(delaySeconds);
        LOG_WARN("WASAPI {} endpoint reconnect failed; retrying in {}s: {}",
                 label, delaySeconds, error);
    }

    static constexpr int FadeFrames = 48; // 1.0 ms @ 48 kHz anti-click micro-ramp
    static constexpr std::size_t PrimingSamples =
        static_cast<std::size_t>(MixBlockFrames) * Channels * 2; // 20 ms jitter absorption cushion

    void extractEndpointBlock(Endpoint& endpoint, float gain,
                              std::array<float, MixBlockFrames * Channels>& outSamples,
                              float& outPeak) {
        outSamples.fill(0.0f);
        outPeak = 0.0f;
        if (!endpoint.active) {
            if (std::abs(endpoint.lastSample[0]) > 1e-6f || std::abs(endpoint.lastSample[1]) > 1e-6f) {
                for (int frameIdx = 0; frameIdx < FadeFrames; ++frameIdx) {
                    const float w = 1.0f - static_cast<float>(frameIdx + 1) / static_cast<float>(FadeFrames);
                    for (int ch = 0; ch < Channels; ++ch) {
                        const float s = endpoint.lastSample[ch] * w * gain;
                        outSamples[static_cast<std::size_t>(frameIdx * Channels + ch)] = s;
                        outPeak = (std::max)(outPeak, std::abs(s));
                    }
                }
                endpoint.lastSample = {0.0f, 0.0f};
            }
            endpoint.inUnderrun = true;
            endpoint.primed = false;
            return;
        }

        const std::size_t available = endpoint.fifo.size() - endpoint.readOffset;
        const std::size_t blockSamples = static_cast<std::size_t>(MixBlockFrames) * Channels;

        if (!endpoint.primed) {
            if (available >= PrimingSamples) {
                endpoint.primed = true;
            } else {
                // Cushioning initial packets against 10ms poll jitter; output silence smoothly.
                endpoint.inUnderrun = true;
                return;
            }
        }

        if (available < blockSamples) {
            // Underrun: consume remaining samples if any, then smoothly fade out from last sample to 0
            // to eliminate hard zero-step clicks/pops.
            const int availFrames = static_cast<int>(available / Channels);
            const bool fadingIn = endpoint.inUnderrun;
            for (int frameIdx = 0; frameIdx < availFrames; ++frameIdx) {
                const float fadeIn = (fadingIn && frameIdx < FadeFrames)
                    ? (static_cast<float>(frameIdx + 1) / static_cast<float>(FadeFrames))
                    : 1.0f;
                for (int ch = 0; ch < Channels; ++ch) {
                    const float raw = endpoint.fifo[endpoint.readOffset + static_cast<std::size_t>(frameIdx * Channels + ch)] * fadeIn;
                    endpoint.lastSample[ch] = raw;
                    const float s = raw * gain;
                    outSamples[static_cast<std::size_t>(frameIdx * Channels + ch)] = s;
                    outPeak = (std::max)(outPeak, std::abs(s));
                }
            }
            const int remainFrames = MixBlockFrames - availFrames;
            const int rampFrames = (std::min)(FadeFrames, remainFrames);
            if (std::abs(endpoint.lastSample[0]) > 1e-6f || std::abs(endpoint.lastSample[1]) > 1e-6f) {
                for (int r = 0; r < rampFrames; ++r) {
                    const float w = 1.0f - static_cast<float>(r + 1) / static_cast<float>(rampFrames);
                    const int dstFrame = availFrames + r;
                    for (int ch = 0; ch < Channels; ++ch) {
                        const float s = endpoint.lastSample[ch] * w * gain;
                        outSamples[static_cast<std::size_t>(dstFrame * Channels + ch)] = s;
                        outPeak = (std::max)(outPeak, std::abs(s));
                    }
                }
            }
            endpoint.lastSample = {0.0f, 0.0f};
            endpoint.readOffset = endpoint.fifo.size();
            endpoint.fifo.clear();
            endpoint.readOffset = 0;
            endpoint.primed = false;
            endpoint.inUnderrun = true;
            return;
        }

        const bool applyFadeIn = endpoint.inUnderrun;
        for (int frameIdx = 0; frameIdx < MixBlockFrames; ++frameIdx) {
            const float fade = (applyFadeIn && frameIdx < FadeFrames)
                ? (static_cast<float>(frameIdx + 1) / static_cast<float>(FadeFrames))
                : 1.0f;
            for (int ch = 0; ch < Channels; ++ch) {
                const std::size_t idx = endpoint.readOffset + static_cast<std::size_t>(frameIdx * Channels + ch);
                const float raw = endpoint.fifo[idx] * fade;
                endpoint.lastSample[ch] = raw;
                const float s = raw * gain;
                outSamples[static_cast<std::size_t>(frameIdx * Channels + ch)] = s;
                outPeak = (std::max)(outPeak, std::abs(s));
            }
        }
        endpoint.inUnderrun = false;

        endpoint.readOffset += blockSamples;
        if (endpoint.readOffset > MaxQueuedSamples / 4) {
            endpoint.fifo.erase(endpoint.fifo.begin(), endpoint.fifo.begin() +
                                static_cast<std::ptrdiff_t>(endpoint.readOffset));
            endpoint.readOffset = 0;
        }
        const std::size_t remaining = endpoint.fifo.size() - endpoint.readOffset;
        if (remaining > MaxQueuedSamples) {
            const std::size_t dropped = remaining - MaxQueuedSamples;
            const std::size_t alignedDrop = dropped - dropped % Channels;
            endpoint.readOffset += alignedDrop;
            endpoint.inUnderrun = true; // trigger micro fade-in across discontinuity
            std::lock_guard lock(statusMutex);
            captureStatus.droppedFrames += alignedDrop / Channels;
        }
    }

    void mixBlock() {
        std::array<float, MixBlockFrames * Channels> sysBlock{};
        std::array<float, MixBlockFrames * Channels> micBlock{};
        std::array<float, MixBlockFrames * Channels> block{};
        float systemPeak = 0.0f;
        float microphonePeak = 0.0f;
        float mixedPeak = 0.0f;
        const float systemGain = systemMuted.load(std::memory_order_relaxed)
            ? 0.0f : systemVolume.load(std::memory_order_relaxed);
        const float microphoneGain = microphoneMuted.load(std::memory_order_relaxed)
            ? 0.0f : microphoneVolume.load(std::memory_order_relaxed);

        extractEndpointBlock(system, systemGain, sysBlock, systemPeak);
        extractEndpointBlock(microphone, microphoneGain, micBlock, microphonePeak);

        for (std::size_t index = 0; index < block.size(); ++index) {
            const float mixed = std::clamp(sysBlock[index] + micBlock[index], -1.0f, 1.0f);
            block[index] = mixed;
            mixedPeak = std::max(mixedPeak, std::abs(mixed));
        }

        std::lock_guard lock(outputMutex);
        output.insert(output.end(), block.begin(), block.end());
        const std::size_t queued = output.size() - outputOffset;
        if (queued > MaxQueuedSamples) {
            const std::size_t dropped = queued - MaxQueuedSamples;
            outputOffset += dropped - dropped % Channels;
            std::lock_guard statusLock(statusMutex);
            captureStatus.droppedFrames += dropped / Channels;
        }
        if (outputOffset > MaxQueuedSamples / 4) {
            output.erase(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(outputOffset));
            outputOffset = 0;
        }
        {
            std::lock_guard statusLock(statusMutex);
            constexpr float decay = 0.82f;
            const bool isSysMuted = systemMuted.load(std::memory_order_relaxed);
            const bool isMicMuted = microphoneMuted.load(std::memory_order_relaxed);
            captureStatus.systemPeak = (system.active && !isSysMuted)
                ? std::max(systemPeak, captureStatus.systemPeak * decay) : 0.0f;
            captureStatus.microphonePeak = (microphone.active && !isMicMuted)
                ? std::max(microphonePeak, captureStatus.microphonePeak * decay) : 0.0f;
            captureStatus.mixedPeak = std::max(mixedPeak, captureStatus.mixedPeak * decay);
        }
    }

    void run(std::stop_token stopToken) {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> enumerator;
        std::string systemError;
        std::string microphoneError;
        ReconnectState systemReconnect;
        ReconnectState microphoneReconnect;
        if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE) {
            const HRESULT createResult = CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&enumerator));
            if (FAILED(createResult)) {
                const auto error = "audio endpoint enumerator unavailable (" +
                    hrText(createResult) + ")";
                if (options.systemAudio) systemError = error;
                if (options.microphone) microphoneError = error;
            }
        } else {
            const auto error = "COM initialization failed (" + hrText(comResult) + ")";
            if (options.systemAudio) systemError = error;
            if (options.microphone) microphoneError = error;
        }
        if (enumerator && options.systemAudio) {
            initializeEndpoint(enumerator.Get(), system, eRender, eConsole, true,
                               options.systemDeviceId, systemError);
        }
        if (enumerator && options.microphone) {
            initializeMicrophone(enumerator.Get(), microphoneError);
        }
        systemReconnect.previouslyActive = system.active;
        microphoneReconnect.previouslyActive = microphone.active;
        updateStatus(systemError, microphoneError);
        {
            std::lock_guard startupLock(startupMutex);
            startupComplete = true;
        }
        startupCv.notify_all();

        const std::int64_t qpcFreq = queryQpcFreq();
        std::uint64_t observedSyncEpoch = syncResetEpoch.load(std::memory_order_acquire);
        std::int64_t syncOriginQpc = queryQpc();
        std::uint64_t totalMixedFrames = 0;

        while (!stopToken.stop_requested()) {
            std::array<HANDLE, 3> events{};
            DWORD count = 0;
            if (wakeEvent) events[count++] = wakeEvent;
            if (system.active && system.event) events[count++] = system.event;
            if (microphone.active && microphone.event) events[count++] = microphone.event;

            const std::int64_t nowBeforeWaitQpc = queryQpc();
            const std::int64_t nextBlockQpc = syncOriginQpc +
                static_cast<std::int64_t>((static_cast<double>(totalMixedFrames + MixBlockFrames) *
                                           static_cast<double>(qpcFreq)) /
                                          static_cast<double>(SampleRate));
            const std::int64_t remainTicks = (std::max)(0LL, nextBlockQpc - nowBeforeWaitQpc);
            const long long remainMs = (remainTicks * 1000LL) / qpcFreq;
            const DWORD timeout = static_cast<DWORD>(std::clamp<long long>(remainMs, 1, 10));

            if (count > 0) WaitForMultipleObjects(count, events.data(), FALSE, timeout);
            else std::this_thread::sleep_for(std::chrono::milliseconds(timeout));

            drainEndpoint(system, systemError);
            drainEndpoint(microphone, microphoneError);
            const auto reconnectNow = Clock::now();
            if (options.systemAudio) {
                scheduleReconnectIfNeeded(system, systemReconnect, reconnectNow);
                tryReconnect(system, systemReconnect, systemError, "system audio", reconnectNow,
                    [&] {
                        return initializeEndpoint(enumerator.Get(), system, eRender, eConsole,
                                                  true, options.systemDeviceId, systemError);
                    });
            }
            if (options.microphone) {
                scheduleReconnectIfNeeded(microphone, microphoneReconnect, reconnectNow);
                tryReconnect(microphone, microphoneReconnect, microphoneError, "microphone",
                             reconnectNow,
                    [&] { return initializeMicrophone(enumerator.Get(), microphoneError); });
            }
            updateStatus(systemError, microphoneError);

            const std::uint64_t latestEpoch = syncResetEpoch.load(std::memory_order_acquire);
            if (latestEpoch != observedSyncEpoch) {
                observedSyncEpoch = latestEpoch;
                system.resetBufferState();
                microphone.resetBufferState();
                {
                    std::lock_guard outLock(outputMutex);
                    output.clear();
                    outputOffset = 0;
                }
                {
                    std::lock_guard statusLock(statusMutex);
                    captureStatus.droppedFrames = 0;
                    captureStatus.systemPeak = 0.0f;
                    captureStatus.microphonePeak = 0.0f;
                    captureStatus.mixedPeak = 0.0f;
                }
                const std::int64_t reqQpc = requestedSyncQpc.load(std::memory_order_acquire);
                syncOriginQpc = (reqQpc > 0) ? reqQpc : queryQpc();
                totalMixedFrames = 0;
            }

            if (paused.load(std::memory_order_acquire)) {
                system.resetBufferState();
                microphone.resetBufferState();
                {
                    std::lock_guard statusLock(statusMutex);
                    captureStatus.systemPeak = 0.0f;
                    captureStatus.microphonePeak = 0.0f;
                    captureStatus.mixedPeak = 0.0f;
                }
                syncOriginQpc = queryQpc();
                totalMixedFrames = 0;
                continue;
            }

            // Compute exact target audio frames elapsed since QPC zero-origin so that
            // endpoint reconnect delays are padded with exact silence instead of drifting A/V sync.
            const std::int64_t afterDrainQpc = queryQpc();
            const std::int64_t elapsedQpc = (std::max)(0LL, afterDrainQpc - syncOriginQpc);
            const std::uint64_t expectedFrames = static_cast<std::uint64_t>(
                (static_cast<double>(elapsedQpc) * static_cast<double>(SampleRate)) /
                static_cast<double>(qpcFreq));

            constexpr std::uint64_t MaxCatchupBlocks = 200; // up to 2.0s exact silence pad per stall
            std::uint64_t blocksMixedNow = 0;
            while (totalMixedFrames + static_cast<std::uint64_t>(MixBlockFrames) <= expectedFrames &&
                   blocksMixedNow < MaxCatchupBlocks) {
                mixBlock();
                totalMixedFrames += static_cast<std::uint64_t>(MixBlockFrames);
                ++blocksMixedNow;
            }
            if (totalMixedFrames + static_cast<std::uint64_t>(MixBlockFrames) <= expectedFrames) {
                const std::uint64_t skippedFrames = expectedFrames - totalMixedFrames;
                const std::uint64_t alignedSkipped =
                    (skippedFrames / static_cast<std::uint64_t>(MixBlockFrames)) *
                    static_cast<std::uint64_t>(MixBlockFrames);
                if (alignedSkipped > 0) {
                    totalMixedFrames += alignedSkipped;
                    std::lock_guard statusLock(statusMutex);
                    captureStatus.droppedFrames += alignedSkipped;
                }
            }
        }
        system.close();
        microphone.close();
        if (SUCCEEDED(comResult)) CoUninitialize();
    }

    void stop() noexcept {
        if (worker.joinable()) {
            worker.request_stop();
            if (wakeEvent) SetEvent(wakeEvent);
            worker.join();
        }
        if (wakeEvent) {
            CloseHandle(wakeEvent);
            wakeEvent = nullptr;
        }
    }
};

AudioCapture::AudioCapture() : m_impl(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() = default;

bool AudioCapture::start(const AudioCaptureOptions& options) {
    stop();
    m_impl->options = options;
    m_impl->paused.store(false, std::memory_order_relaxed);
    m_impl->syncResetEpoch.store(0, std::memory_order_relaxed);
    m_impl->requestedSyncQpc.store(0, std::memory_order_relaxed);
    m_impl->wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_impl->systemVolume.store(std::clamp(options.systemVolume, 0.0f, 2.0f),
                               std::memory_order_relaxed);
    m_impl->microphoneVolume.store(std::clamp(options.microphoneVolume, 0.0f, 2.0f),
                                   std::memory_order_relaxed);
    m_impl->systemMuted.store(false, std::memory_order_relaxed);
    m_impl->microphoneMuted.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_impl->outputMutex);
        m_impl->output.clear();
        m_impl->outputOffset = 0;
    }
    {
        std::lock_guard lock(m_impl->statusMutex);
        m_impl->captureStatus = {};
    }
    {
        std::lock_guard lock(m_impl->startupMutex);
        m_impl->startupComplete = false;
    }
    m_impl->worker = std::jthread([this](std::stop_token token) { m_impl->run(token); });
    std::unique_lock lock(m_impl->startupMutex);
    const bool started = m_impl->startupCv.wait_for(
        lock, std::chrono::seconds(3), [this] { return m_impl->startupComplete; });
    lock.unlock();
    if (!started) {
        stop();
        return false;
    }
    const auto state = status();
    const bool active = state.systemAudioActive || state.microphoneActive;
    if (!active) stop();
    return active;
}

void AudioCapture::stop() noexcept { m_impl->stop(); }

void AudioCapture::setPaused(bool paused) {
    m_impl->paused.store(paused, std::memory_order_release);
    if (paused) {
        std::lock_guard lock(m_impl->outputMutex);
        m_impl->output.clear();
        m_impl->outputOffset = 0;
    }
    if (m_impl->wakeEvent) {
        SetEvent(m_impl->wakeEvent);
    }
}

void AudioCapture::resetAndResumeForSyncStart() {
    const std::int64_t nowQpc = Impl::queryQpc();
    {
        std::lock_guard lock(m_impl->outputMutex);
        m_impl->output.clear();
        m_impl->outputOffset = 0;
    }
    {
        std::lock_guard statusLock(m_impl->statusMutex);
        m_impl->captureStatus.droppedFrames = 0;
    }
    m_impl->requestedSyncQpc.store(nowQpc, std::memory_order_release);
    m_impl->syncResetEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_impl->paused.store(false, std::memory_order_release);
    if (m_impl->wakeEvent) {
        SetEvent(m_impl->wakeEvent);
    }
}

void AudioCapture::setVolumes(float systemVolume, float microphoneVolume) {
    m_impl->systemVolume.store(std::clamp(systemVolume, 0.0f, 2.0f),
                               std::memory_order_relaxed);
    m_impl->microphoneVolume.store(std::clamp(microphoneVolume, 0.0f, 2.0f),
                                   std::memory_order_relaxed);
}

void AudioCapture::setSystemMuted(bool muted) {
    m_impl->systemMuted.store(muted, std::memory_order_relaxed);
    std::lock_guard lock(m_impl->statusMutex);
    m_impl->captureStatus.systemMuted = muted;
    if (muted) m_impl->captureStatus.systemPeak = 0.0f;
}

void AudioCapture::setMicrophoneMuted(bool muted) {
    m_impl->microphoneMuted.store(muted, std::memory_order_relaxed);
    std::lock_guard lock(m_impl->statusMutex);
    m_impl->captureStatus.microphoneMuted = muted;
    if (muted) m_impl->captureStatus.microphonePeak = 0.0f;
}

bool AudioCapture::readFrames(int frameCount, std::vector<float>& output) {
    if (frameCount <= 0) return false;
    const std::size_t sampleCount = static_cast<std::size_t>(frameCount) * Channels;
    std::lock_guard lock(m_impl->outputMutex);
    if (m_impl->output.size() - m_impl->outputOffset < sampleCount) return false;
    output.assign(m_impl->output.begin() + static_cast<std::ptrdiff_t>(m_impl->outputOffset),
                  m_impl->output.begin() + static_cast<std::ptrdiff_t>(m_impl->outputOffset + sampleCount));
    m_impl->outputOffset += sampleCount;
    return true;
}

AudioCaptureStatus AudioCapture::status() const {
    std::lock_guard lock(m_impl->statusMutex);
    return m_impl->captureStatus;
}

std::vector<AudioDeviceInfo> AudioCapture::devices() {
    std::vector<AudioDeviceInfo> result;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) return result;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        if (SUCCEEDED(comResult)) CoUninitialize();
        return result;
    }

    const auto appendFlow = [&](EDataFlow flow, ERole role, bool systemAudio) {
        std::wstring defaultId;
        ComPtr<IMMDevice> defaultDevice;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, role, &defaultDevice))) {
            LPWSTR rawId = nullptr;
            if (SUCCEEDED(defaultDevice->GetId(&rawId)) && rawId) {
                defaultId = rawId;
                CoTaskMemFree(rawId);
            }
        }

        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) return;
        UINT count = 0;
        if (FAILED(collection->GetCount(&count))) return;
        for (UINT index = 0; index < count; ++index) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(index, &device))) continue;
            LPWSTR rawId = nullptr;
            if (FAILED(device->GetId(&rawId)) || !rawId) continue;
            std::wstring id(rawId);
            CoTaskMemFree(rawId);

            std::wstring name = id;
            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal) {
                    name = value.pwszVal;
                }
                PropVariantClear(&value);
            }
            result.push_back({wideToUtf8(id), wideToUtf8(name), systemAudio,
                              id == defaultId});
        }
    };

    appendFlow(eRender, eConsole, true);
    appendFlow(eCapture, eCommunications, false);
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.systemAudio != right.systemAudio) return left.systemAudio > right.systemAudio;
        if (left.defaultDevice != right.defaultDevice) return left.defaultDevice > right.defaultDevice;
        return left.name < right.name;
    });
    if (SUCCEEDED(comResult)) CoUninitialize();
    return result;
}

}  // namespace tools3000::capture
