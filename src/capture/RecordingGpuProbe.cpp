#include "capture/RecordingGpuProbe.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <format>

namespace tools3000::capture {
namespace {

std::string hresultText(HRESULT value) {
    return std::format("HRESULT 0x{:08X}", static_cast<unsigned long>(value));
}

class MediaFoundationLifetime final {
public:
    bool start(std::string& error) noexcept {
        const HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(result)) {
            error = hresultText(result);
            return false;
        }
        m_started = true;
        return true;
    }
    ~MediaFoundationLifetime() {
        if (m_started) MFShutdown();
    }
private:
    bool m_started = false;
};

}  // namespace

RecordingGpuProbe probeRecordingGpuPath() noexcept {
    RecordingGpuProbe probe;
    try {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(result)) {
            probe.d3d11Error = hresultText(result);
            return probe;
        }
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(0, &adapter);
        if (FAILED(result)) {
            probe.d3d11Error = hresultText(result);
            return probe;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter->GetDesc1(&description))) {
            probe.adapterLuidLow = description.AdapterLuid.LowPart;
            probe.adapterLuidHigh = description.AdapterLuid.HighPart;
        }
        static constexpr D3D_FEATURE_LEVEL requested[] = {
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL selected{};
        result = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested, static_cast<UINT>(std::size(requested)),
            D3D11_SDK_VERSION, &device, &selected, &context);
        if (FAILED(result)) {
            probe.d3d11Error = hresultText(result);
            return probe;
        }
        probe.d3d11Available = true;
        probe.d3d11FeatureLevel = static_cast<std::uint32_t>(selected);

        // This probe is also called by tests and diagnostic tools outside an
        // active recorder. Start MF locally instead of relying on unrelated
        // process-global initialization, then balance it before return.
        MediaFoundationLifetime mediaFoundation;
        if (mediaFoundation.start(probe.mediaFoundationError)) {
            IMFActivate** activations = nullptr;
            UINT count = 0;
            result = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE,
                               nullptr, nullptr, &activations, &count);
            if (SUCCEEDED(result)) {
                probe.mediaFoundationHardwareMftAvailable = count > 0;
                for (UINT index = 0; index < count; ++index) activations[index]->Release();
                CoTaskMemFree(activations);
            } else {
                probe.mediaFoundationError = hresultText(result);
            }
        }

        probe.ffmpegD3d11vaCompiled =
            av_hwdevice_find_type_by_name("d3d11va") == AV_HWDEVICE_TYPE_D3D11VA;
        if (!probe.ffmpegD3d11vaCompiled) {
            probe.ffmpegError = "FFmpeg was built without d3d11va hardware frames";
        }
    } catch (...) {
        probe.d3d11Error = "GPU capability probe raised an unexpected exception";
    }
    return probe;
}

}  // namespace tools3000::capture
