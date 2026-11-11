#include "capture/CaptureBackend.h"

#include "core/logger/Logger.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace tools3000::capture {
namespace {

using Microsoft::WRL::ComPtr;

std::string hresultText(HRESULT result) {
    char buffer[16]{};
    sprintf_s(buffer, "0x%08X", static_cast<unsigned>(result));
    return buffer;
}

bool containsRegion(const RECT& output, const CaptureRegion& region) {
    const auto right = static_cast<long long>(region.x) + region.width;
    const auto bottom = static_cast<long long>(region.y) + region.height;
    return region.x >= output.left && region.y >= output.top &&
           right <= output.right && bottom <= output.bottom;
}

HRESULT createDevice(IDXGIAdapter* adapter, ComPtr<ID3D11Device>& device,
                     ComPtr<ID3D11DeviceContext>& context) {
    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL selected{};
    HRESULT result = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels.data(), static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
        &device, &selected, &context);
    if (result == E_INVALIDARG) {
        result = D3D11CreateDevice(
            adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data() + 1, static_cast<UINT>(featureLevels.size() - 1),
            D3D11_SDK_VERSION, &device, &selected, &context);
    }
    return result;
}

class GdiCaptureBackend final : public ICaptureBackend {
public:
    ~GdiCaptureBackend() override { shutdown(); }

    const CaptureBackendInfo& info() const noexcept override { return m_info; }

    bool initialize(const CaptureRegion& region, std::string& error) override {
        shutdown();
        if (!region.isValid() || region.width > 32768 || region.height > 32768 ||
            region.width > (std::numeric_limits<int>::max() - 3) / 3) {
            error = "invalid GDI capture region";
            return false;
        }
        m_region = region;
        m_screenDc = GetDC(nullptr);
        if (!m_screenDc) {
            error = "GetDC failed (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        m_memoryDc = CreateCompatibleDC(m_screenDc);
        if (!m_memoryDc) {
            error = "CreateCompatibleDC failed (" + std::to_string(GetLastError()) + ")";
            shutdown();
            return false;
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = region.width;
        bitmapInfo.bmiHeader.biHeight = -region.height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 24;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        m_bitmap = CreateDIBSection(m_screenDc, &bitmapInfo, DIB_RGB_COLORS,
                                    &m_bits, nullptr, 0);
        if (!m_bitmap || !m_bits) {
            error = "CreateDIBSection failed (" + std::to_string(GetLastError()) + ")";
            shutdown();
            return false;
        }
        m_previousBitmap = SelectObject(m_memoryDc, m_bitmap);
        if (!m_previousBitmap || m_previousBitmap == HGDI_ERROR) {
            error = "SelectObject failed (" + std::to_string(GetLastError()) + ")";
            m_previousBitmap = nullptr;
            shutdown();
            return false;
        }
        m_stride = (region.width * 3 + 3) & ~3;
        return true;
    }

    bool capture(CaptureFrameView& frame, std::string& error) override {
        if (!m_screenDc || !m_memoryDc || !m_bits) {
            error = "GDI capture backend is not initialized";
            return false;
        }
        if (!BitBlt(m_memoryDc, 0, 0, m_region.width, m_region.height,
                    m_screenDc, m_region.x, m_region.y, SRCCOPY | CAPTUREBLT)) {
            error = "BitBlt failed (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        frame = {
            static_cast<std::uint8_t*>(m_bits), m_stride,
            m_region.width, m_region.height, CapturePixelFormat::Bgr24
        };
        return true;
    }

    void releaseFrame() noexcept override {}

    void shutdown() noexcept override {
        if (m_memoryDc && m_previousBitmap) {
            SelectObject(m_memoryDc, m_previousBitmap);
            m_previousBitmap = nullptr;
        }
        if (m_bitmap) {
            DeleteObject(m_bitmap);
            m_bitmap = nullptr;
        }
        m_bits = nullptr;
        if (m_memoryDc) {
            DeleteDC(m_memoryDc);
            m_memoryDc = nullptr;
        }
        if (m_screenDc) {
            ReleaseDC(nullptr, m_screenDc);
            m_screenDc = nullptr;
        }
        m_stride = 0;
        m_region = {};
    }

private:
    CaptureBackendInfo m_info{"gdi-bitblt", "GDI BitBlt", true, false, {}};
    CaptureRegion m_region;
    HDC m_screenDc = nullptr;
    HDC m_memoryDc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_previousBitmap = nullptr;
    void* m_bits = nullptr;
    int m_stride = 0;
};

class DesktopDuplicationBackend final : public ICaptureBackend {
public:
    ~DesktopDuplicationBackend() override { shutdown(); }

    const CaptureBackendInfo& info() const noexcept override { return m_info; }

    bool initialize(const CaptureRegion& region, std::string& error) override {
        shutdown();
        if (!region.isValid() || region.width > 32768 || region.height > 32768) {
            error = "invalid Desktop Duplication capture region";
            return false;
        }

        ComPtr<IDXGIFactory1> factory;
        HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(result)) {
            error = "CreateDXGIFactory1 failed (" + hresultText(result) + ")";
            return false;
        }

        RECT regRect{ region.x, region.y, region.x + region.width, region.y + region.height };

        struct MatchedOutput {
            ComPtr<IDXGIAdapter1> adapter;
            ComPtr<IDXGIOutput> output;
            DXGI_OUTPUT_DESC desc{};
            RECT intersection{};
        };
        std::vector<MatchedOutput> matchedOutputs;

        for (UINT adapterIndex = 0; ; ++adapterIndex) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT adapterResult = factory->EnumAdapters1(adapterIndex, &adapter);
            if (adapterResult == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(adapterResult) || !adapter) continue;
            for (UINT outputIndex = 0; ; ++outputIndex) {
                ComPtr<IDXGIOutput> output;
                const HRESULT outputResult = adapter->EnumOutputs(outputIndex, &output);
                if (outputResult == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(outputResult) || !output) continue;
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                    RECT sect{};
                    if (IntersectRect(&sect, &desc.DesktopCoordinates, &regRect) &&
                        sect.right > sect.left && sect.bottom > sect.top) {
                        if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
                            desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
                            error = "rotated display requires the compatibility backend";
                            return false;
                        }
                        matchedOutputs.push_back({ adapter, output, desc, sect });
                    }
                }
            }
        }

        if (matchedOutputs.empty()) {
            error = "no attached DXGI outputs intersect capture region";
            return false;
        }

        // A Desktop Duplication object must be created with a D3D device from
        // the output's own adapter. One device therefore cannot safely compose
        // a region spanning adapters. Reject it before allocating any DXGI
        // resources; AutomaticCaptureBackend will select the existing GDI path.
        const IDXGIAdapter1* captureAdapter = matchedOutputs.front().adapter.Get();
        const bool spansAdapters = std::any_of(
            matchedOutputs.cbegin() + 1, matchedOutputs.cend(),
            [captureAdapter](const MatchedOutput& item) {
                return item.adapter.Get() != captureAdapter;
            });
        if (spansAdapters) {
            error = "capture region spans multiple DXGI adapters";
            return false;
        }

        // 创建 Direct3D11 渲染与捕获设备
        result = createDevice(matchedOutputs.front().adapter.Get(), m_device, m_context);
        if (FAILED(result)) {
            error = "D3D11CreateDevice failed (" + hresultText(result) + ")";
            shutdown();
            return false;
        }

        for (auto& item : matchedOutputs) {
            ComPtr<IDXGIOutput1> output1;
            result = item.output.As(&output1);
            if (FAILED(result)) {
                error = "IDXGIOutput1 is unavailable (" + hresultText(result) + ")";
                shutdown();
                return false;
            }
            ComPtr<IDXGIOutputDuplication> dupl;
            result = output1->DuplicateOutput(m_device.Get(), &dupl);
            if (FAILED(result)) {
                error = "DuplicateOutput failed (" + hresultText(result) + ")";
                shutdown();
                return false;
            }
            m_outputs.push_back({ output1, dupl, item.desc, item.intersection, false });
        }

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = static_cast<UINT>(region.width);
        textureDesc.Height = static_cast<UINT>(region.height);
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_STAGING;
        textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        result = m_device->CreateTexture2D(&textureDesc, nullptr, &m_stagingTexture);
        if (FAILED(result)) {
            error = "CreateTexture2D staging surface failed (" + hresultText(result) + ")";
            shutdown();
            return false;
        }

        m_region = region;
        return true;
    }

    bool capture(CaptureFrameView& frame, std::string& error) override {
        releaseFrame();
        if (m_outputs.empty() || !m_context || !m_stagingTexture) {
            error = "Desktop Duplication backend is not initialized";
            return false;
        }

        bool allOutputsReady = true;
        for (auto& out : m_outputs) {
            const int maxAttempts = out.hasFrame ? 1 : 2;
            for (int attempt = 0; attempt < maxAttempts; ++attempt) {
                DXGI_OUTDUPL_FRAME_INFO frameInfo{};
                ComPtr<IDXGIResource> desktopResource;
                // 首帧允许最多等待 2x20ms 以跳过纯鼠标更新帧（LastPresentTime == 0），后续帧 0ms 轮询
                const HRESULT acquireResult = out.duplication->AcquireNextFrame(
                    out.hasFrame ? 0u : 20u, &frameInfo, &desktopResource);
                if (acquireResult == DXGI_ERROR_ACCESS_LOST) {
                    error = "desktop duplication access was lost";
                    return false;
                }
                if (acquireResult == DXGI_ERROR_WAIT_TIMEOUT) {
                    break;
                }
                if (FAILED(acquireResult)) {
                    error = "AcquireNextFrame failed (" + hresultText(acquireResult) + ")";
                    return false;
                }

                // 若 LastPresentTime == 0，说明此次仅更新了鼠标指针形状/位置，desktopResource 未包含有效桌面图像（全黑）
                if (frameInfo.LastPresentTime.QuadPart == 0) {
                    out.duplication->ReleaseFrame();
                    if (out.hasFrame) {
                        break;
                    }
                    continue;
                }

                ComPtr<ID3D11Texture2D> desktopTexture;
                const HRESULT textureResult = desktopResource.As(&desktopTexture);
                if (FAILED(textureResult)) {
                    out.duplication->ReleaseFrame();
                    error = "desktop resource is not a D3D11 texture (" + hresultText(textureResult) + ")";
                    return false;
                }
                const UINT srcLeft = static_cast<UINT>(out.intersection.left - out.desc.DesktopCoordinates.left);
                const UINT srcTop = static_cast<UINT>(out.intersection.top - out.desc.DesktopCoordinates.top);
                const UINT width = static_cast<UINT>(out.intersection.right - out.intersection.left);
                const UINT height = static_cast<UINT>(out.intersection.bottom - out.intersection.top);
                const D3D11_BOX sourceBox{
                    srcLeft, srcTop, 0, srcLeft + width, srcTop + height, 1
                };
                const UINT dstX = static_cast<UINT>(out.intersection.left - m_region.x);
                const UINT dstY = static_cast<UINT>(out.intersection.top - m_region.y);

                m_context->CopySubresourceRegion(
                    m_stagingTexture.Get(), 0, dstX, dstY, 0,
                    desktopTexture.Get(), 0, &sourceBox);
                out.duplication->ReleaseFrame();
                out.hasFrame = true;
                break;
            }

            // 静态桌面首帧未触发 DWM 新合成时，使用单次 GDI BitBlt 预热 staging 纹理，
            // 避免因 DXGI_ERROR_WAIT_TIMEOUT 误拆毁硬件加速会话并整场退化为 GDI。
            if (!out.hasFrame && !seedOutputFromGdi(out)) {
                allOutputsReady = false;
            }
        }

        if (!allOutputsReady) {
            error = "timed out waiting for complete initial frame across all outputs";
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapResult = m_context->Map(
            m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(mapResult)) {
            error = "Map staging surface failed (" + hresultText(mapResult) + ")";
            return false;
        }
        m_mapped = true;

        // 防御性校验：检查映射表面是否为全 0 未初始化黑帧（例如静态桌面未触发 DWM 合成）
        const auto* rawBytes = static_cast<const std::uint8_t*>(mapped.pData);
        const int pitch = static_cast<int>(mapped.RowPitch);
        bool hasNonZeroPixel = false;
        if (rawBytes && m_region.width > 0 && m_region.height > 0) {
            const int stepY = std::max(1, m_region.height / 8);
            const int stepX = std::max(1, m_region.width / 8);
            for (int y = 0; y < m_region.height && !hasNonZeroPixel; y += stepY) {
                const std::uint8_t* row = rawBytes + static_cast<std::size_t>(y) * pitch;
                for (int x = 0; x < m_region.width; x += stepX) {
                    const std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
                    if (px[0] != 0 || px[1] != 0 || px[2] != 0 || px[3] != 0) {
                        hasNonZeroPixel = true;
                        break;
                    }
                }
            }
        }
        if (!hasNonZeroPixel) {
            releaseFrame();
            for (auto& out : m_outputs) out.hasFrame = false;
            error = "Desktop Duplication returned uninitialized zero surface";
            return false;
        }
        frame = {
            static_cast<std::uint8_t*>(mapped.pData),
            static_cast<int>(mapped.RowPitch), m_region.width, m_region.height,
            CapturePixelFormat::Bgra32
        };
        return true;
    }

    bool readLastValidFrame(CaptureFrameView& frame) noexcept override {
        if (!m_context || !m_stagingTexture || m_region.width <= 0 || m_region.height <= 0) {
            return false;
        }
        bool anyHasFrame = false;
        for (const auto& out : m_outputs) {
            if (out.hasFrame) {
                anyHasFrame = true;
                break;
            }
        }
        if (!anyHasFrame) {
            return false;
        }
        releaseFrame();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
            return false;
        }
        m_mapped = true;
        frame = {
            static_cast<std::uint8_t*>(mapped.pData),
            static_cast<int>(mapped.RowPitch), m_region.width, m_region.height,
            CapturePixelFormat::Bgra32
        };
        return true;
    }

    void releaseFrame() noexcept override {
        if (m_mapped && m_context && m_stagingTexture) {
            m_context->Unmap(m_stagingTexture.Get(), 0);
            m_mapped = false;
        }
    }

    void shutdown() noexcept override {
        releaseFrame();
        m_stagingTexture.Reset();
        m_outputs.clear();
        m_context.Reset();
        m_device.Reset();
        m_region = {};
    }

private:
    struct OutputDuplicationEntry {
        ComPtr<IDXGIOutput1> output;
        ComPtr<IDXGIOutputDuplication> duplication;
        DXGI_OUTPUT_DESC desc{};
        RECT intersection{};
        bool hasFrame = false;
    };

    bool seedOutputFromGdi(OutputDuplicationEntry& out) noexcept {
        const int width = out.intersection.right - out.intersection.left;
        const int height = out.intersection.bottom - out.intersection.top;
        if (width <= 0 || height <= 0 || !m_context || !m_stagingTexture) {
            return false;
        }

        HDC screenDc = GetDC(nullptr);
        if (!screenDc) return false;
        HDC memDc = CreateCompatibleDC(screenDc);
        if (!memDc) {
            ReleaseDC(nullptr, screenDc);
            return false;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* dibBits = nullptr;
        HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        if (!dib || !dibBits) {
            if (dib) DeleteObject(dib);
            DeleteDC(memDc);
            ReleaseDC(nullptr, screenDc);
            return false;
        }

        HGDIOBJ oldBmp = SelectObject(memDc, dib);
        const BOOL bltOk = BitBlt(
            memDc, 0, 0, width, height,
            screenDc, out.intersection.left, out.intersection.top, SRCCOPY | CAPTUREBLT);

        bool seeded = false;
        if (bltOk) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mapped)) &&
                mapped.pData) {
                const int dstX = out.intersection.left - m_region.x;
                const int dstY = out.intersection.top - m_region.y;
                const auto* srcBase = static_cast<const std::uint8_t*>(dibBits);
                auto* dstBase = static_cast<std::uint8_t*>(mapped.pData);
                const std::size_t srcStride = static_cast<std::size_t>(width) * 4;
                for (int y = 0; y < height; ++y) {
                    const std::uint8_t* srcRow = srcBase + static_cast<std::size_t>(y) * srcStride;
                    std::uint8_t* dstRow = dstBase +
                        static_cast<std::size_t>(dstY + y) * mapped.RowPitch +
                        static_cast<std::size_t>(dstX) * 4;
                    for (int x = 0; x < width; ++x) {
                        const std::size_t off = static_cast<std::size_t>(x) * 4;
                        dstRow[off + 0] = srcRow[off + 0];
                        dstRow[off + 1] = srcRow[off + 1];
                        dstRow[off + 2] = srcRow[off + 2];
                        dstRow[off + 3] = 0xFF;
                    }
                }
                m_context->Unmap(m_stagingTexture.Get(), 0);
                out.hasFrame = true;
                seeded = true;
            }
        }

        if (oldBmp && oldBmp != HGDI_ERROR) SelectObject(memDc, oldBmp);
        DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return seeded;
    }

    CaptureBackendInfo m_info{
        "dxgi-desktop-duplication", "DXGI Desktop Duplication", true, true, {}
    };
    CaptureRegion m_region;
    std::vector<OutputDuplicationEntry> m_outputs;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11Texture2D> m_stagingTexture;
    bool m_mapped = false;
};

class AutomaticCaptureBackend final : public ICaptureBackend {
public:
    ~AutomaticCaptureBackend() override { shutdown(); }

    const CaptureBackendInfo& info() const noexcept override {
        if (m_active) return m_active->info();
        if (!m_fallbackBuffer.empty()) return m_secureDesktopInfo;
        return m_unavailableInfo;
    }

    bool initialize(const CaptureRegion& region, std::string& error) override {
        shutdown();
        if (!region.isValid() || region.width > 32768 || region.height > 32768) {
            error = "invalid capture region";
            return false;
        }
        m_region = region;
        m_preferAccelerated = true;

        auto accelerated = std::make_unique<DesktopDuplicationBackend>();
        std::string acceleratedError;
        if (accelerated->initialize(region, acceleratedError)) {
            m_active = std::move(accelerated);
            return true;
        }

        LOG_WARN("DXGI 捕获不可用，回退 GDI: {}", acceleratedError);
        m_lastDxgiRetryTickMs = GetTickCount64();
        if (switchToGdi(error)) {
            return true;
        }

        // UAC / Win+L 安全桌面期间启动捕获：初始化纯黑占位帧以保持会话存活，待桌面恢复后自动接入 DXGI/GDI
        if (isDesktopTemporarilyInaccessible() && ensureFallbackBuffer()) {
            error.clear();
            LOG_WARN("AutomaticCaptureBackend: input desktop inaccessible during init, starting with placeholder frame");
            return true;
        }
        return false;
    }

    bool capture(CaptureFrameView& frame, std::string& error) override {
        m_needsActiveRelease = false;
        const std::uint64_t nowTick = GetTickCount64();

        // 在 GDI 降级或安全桌面占位运行期间，每隔 3.5 秒检测输入桌面恢复并自动尝试切回高性能 DXGI 硬件捕获
        const bool runningAccelerated = (m_active && m_active->info().accelerated);
        if (m_preferAccelerated && !runningAccelerated &&
            (nowTick - m_lastDxgiRetryTickMs) >= 3500ULL &&
            !isDesktopTemporarilyInaccessible()) {
            m_lastDxgiRetryTickMs = nowTick;
            auto recovered = std::make_unique<DesktopDuplicationBackend>();
            std::string retryError;
            if (recovered->initialize(m_region, retryError)) {
                CaptureFrameView probeFrame{};
                if (recovered->capture(probeFrame, retryError)) {
                    if (m_active) {
                        m_active->releaseFrame();
                        m_active->shutdown();
                    }
                    m_active = std::move(recovered);
                    m_needsActiveRelease = true;
                    frame = probeFrame;
                    snapshotFallbackFrame(frame, nowTick, true);
                    LOG_INFO("AutomaticCaptureBackend: desktop recovered, switched back to DXGI Desktop Duplication");
                    return true;
                }
                recovered->releaseFrame();
                recovered->shutdown();
            }
        }

        if (!m_active && !isDesktopTemporarilyInaccessible()) {
            std::string gdiErr;
            switchToGdi(gdiErr);
        }

        if (m_active && m_active->capture(frame, error)) {
            m_needsActiveRelease = true;
            m_loggedSecureDesktopHold = false;
            // Healthy DXGI Desktop Duplication keeps its composite staging texture on the backend;
            // avoid copying 33MB every 500ms during healthy recording and only snapshot on demand
            // if GDI is active or right before an accelerated fallback transition.
            if (!m_active->info().accelerated) {
                snapshotFallbackFrame(frame, nowTick, false);
            }
            return true;
        }

        const std::string primaryError = error;
        const bool wasAccelerated = (m_active && m_active->info().accelerated);
        if (wasAccelerated) {
            CaptureFrameView lastValidFrame{};
            if (m_active->readLastValidFrame(lastValidFrame)) {
                snapshotFallbackFrame(lastValidFrame, nowTick, true);
            }
            m_active->releaseFrame();
            m_active->shutdown();
            m_active.reset();
            m_lastDxgiRetryTickMs = nowTick;

            std::string fallbackError;
            if (switchToGdi(fallbackError) && m_active->capture(frame, error)) {
                m_needsActiveRelease = true;
                LOG_WARN("DXGI 捕获运行时失败，已切换 GDI: {}", primaryError);
                snapshotFallbackFrame(frame, nowTick, true);
                return true;
            }
            error = primaryError + (fallbackError.empty() ? "" : ("; GDI fallback failed: " + fallbackError));
        } else if (m_active && !isDesktopTemporarilyInaccessible()) {
            // 刚从安全桌面切回正常桌面时，旧屏幕 HDC 可能失效，自动刷新一次 GDI 上下文重试
            m_active->releaseFrame();
            m_active->shutdown();
            m_active.reset();
            std::string reopenErr;
            if (switchToGdi(reopenErr) && m_active->capture(frame, error)) {
                m_needsActiveRelease = true;
                snapshotFallbackFrame(frame, nowTick, true);
                return true;
            }
        }

        // UAC 弹窗、Win+L 锁屏或显示模式瞬态切换期间：输出最近有效帧（若尚无有效帧则输出纯黑占位帧），绝不断流报错
        if ((isDesktopTemporarilyInaccessible() || wasAccelerated || m_hasValidCachedFrame) &&
            serveFallbackFrame(frame)) {
            if (!m_loggedSecureDesktopHold) {
                m_loggedSecureDesktopHold = true;
                LOG_WARN("AutomaticCaptureBackend: desktop temporarily inaccessible ({}), holding last valid/black frame",
                         primaryError);
            }
            error.clear();
            return true;
        }

        if (error.empty()) {
            error = primaryError.empty() ? "no capture backend is active" : primaryError;
        }
        return false;
    }

    void releaseFrame() noexcept override {
        if (m_needsActiveRelease && m_active) {
            m_active->releaseFrame();
        }
        m_needsActiveRelease = false;
    }

    void shutdown() noexcept override {
        if (m_active) {
            if (m_needsActiveRelease) {
                m_active->releaseFrame();
            }
            m_active->shutdown();
            m_active.reset();
        }
        m_needsActiveRelease = false;
        m_fallbackBuffer.clear();
        m_fallbackBuffer.shrink_to_fit();
        m_fallbackStride = 0;
        m_fallbackFormat = CapturePixelFormat::Bgr24;
        m_hasValidCachedFrame = false;
        m_lastSnapshotTickMs = 0;
        m_lastDxgiRetryTickMs = 0;
        m_loggedSecureDesktopHold = false;
        m_region = {};
    }

private:
    bool switchToGdi(std::string& error) {
        auto fallback = std::make_unique<GdiCaptureBackend>();
        if (!fallback->initialize(m_region, error)) return false;
        m_active = std::move(fallback);
        return true;
    }

    bool ensureFallbackBuffer(CapturePixelFormat fmt = CapturePixelFormat::Bgr24) {
        if (!m_region.isValid() || m_region.width <= 0 || m_region.height <= 0 ||
            m_region.width > 32768 || m_region.height > 32768) {
            return false;
        }
        const int bpp = (fmt == CapturePixelFormat::Bgra32) ? 4 : 3;
        const int stride = (m_region.width * bpp + 3) & ~3;
        const std::size_t bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(m_region.height);
        if (bytes == 0 || bytes > (256ULL * 1024ULL * 1024ULL)) {
            return false;
        }
        if (m_fallbackBuffer.size() != bytes || m_fallbackStride != stride || m_fallbackFormat != fmt) {
            m_fallbackBuffer.assign(bytes, 0);
            if (fmt == CapturePixelFormat::Bgra32) {
                for (int y = 0; y < m_region.height; ++y) {
                    std::uint8_t* row = m_fallbackBuffer.data() + static_cast<std::size_t>(y) * stride;
                    for (int x = 0; x < m_region.width; ++x) {
                        row[x * 4 + 3] = 0xFF;
                    }
                }
            }
            m_fallbackStride = stride;
            m_fallbackFormat = fmt;
        }
        return true;
    }

    void snapshotFallbackFrame(const CaptureFrameView& src, std::uint64_t nowTick, bool force) {
        m_loggedSecureDesktopHold = false;
        if (!src.data || src.width != m_region.width || src.height != m_region.height || src.stride <= 0) {
            return;
        }
        if (!force && m_hasValidCachedFrame && (nowTick - m_lastSnapshotTickMs) < 500ULL) {
            return;
        }
        if (!ensureFallbackBuffer(src.format)) {
            return;
        }
        const int bpp = (src.format == CapturePixelFormat::Bgra32) ? 4 : 3;
        const std::size_t rowBytes = static_cast<std::size_t>(src.width) * static_cast<std::size_t>(bpp);
        for (int y = 0; y < src.height; ++y) {
            std::memcpy(
                m_fallbackBuffer.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(m_fallbackStride),
                src.data + static_cast<std::size_t>(y) * static_cast<std::size_t>(src.stride),
                rowBytes);
        }
        m_hasValidCachedFrame = true;
        m_lastSnapshotTickMs = nowTick;
    }

    bool serveFallbackFrame(CaptureFrameView& outFrame) {
        const CapturePixelFormat targetFmt = m_hasValidCachedFrame ? m_fallbackFormat : CapturePixelFormat::Bgr24;
        if (!ensureFallbackBuffer(targetFmt) || m_fallbackBuffer.empty()) {
            return false;
        }
        outFrame.data = m_fallbackBuffer.data();
        outFrame.stride = m_fallbackStride;
        outFrame.width = m_region.width;
        outFrame.height = m_region.height;
        outFrame.format = m_fallbackFormat;
        return true;
    }

    CaptureBackendInfo m_unavailableInfo{
        "unavailable", "No capture backend", false, false, "not initialized"
    };
    CaptureBackendInfo m_secureDesktopInfo{
        "secure-desktop-hold", "Secure Desktop Hold", true, false, {}
    };
    CaptureRegion m_region;
    std::unique_ptr<ICaptureBackend> m_active;
    bool m_preferAccelerated = true;
    bool m_needsActiveRelease = false;
    bool m_hasValidCachedFrame = false;
    bool m_loggedSecureDesktopHold = false;
    std::uint64_t m_lastSnapshotTickMs = 0;
    std::uint64_t m_lastDxgiRetryTickMs = 0;
    int m_fallbackStride = 0;
    CapturePixelFormat m_fallbackFormat = CapturePixelFormat::Bgr24;
    std::vector<std::uint8_t> m_fallbackBuffer;
};

class MemoryCaptureBackend final : public ICaptureBackend {
public:
    explicit MemoryCaptureBackend(CapturePixelFormat format) : m_format(format) {
        m_info.id = "memory-synthetic";
        m_info.name = "Memory Synthetic Capture";
        m_info.available = true;
        m_info.accelerated = false;
    }
    ~MemoryCaptureBackend() override { shutdown(); }

    const CaptureBackendInfo& info() const noexcept override { return m_info; }

    bool initialize(const CaptureRegion& region, std::string& error) override {
        shutdown();
        if (!region.isValid()) {
            error = "invalid memory capture region";
            return false;
        }
        m_region = region;
        const int pixelBytes = m_format == CapturePixelFormat::Bgra32 ? 4 : 3;
        m_stride = (region.width * pixelBytes + 3) & ~3;
        m_buffer.resize(static_cast<std::size_t>(m_stride) * region.height);
        generatePattern();
        return true;
    }

    bool capture(CaptureFrameView& frame, std::string& error) override {
        if (m_buffer.empty()) {
            error = "memory capture backend not initialized";
            return false;
        }
        generatePattern();
        frame.data = m_buffer.data();
        frame.stride = m_stride;
        frame.width = m_region.width;
        frame.height = m_region.height;
        frame.format = m_format;
        return true;
    }

    void releaseFrame() noexcept override {}

    void shutdown() noexcept override {
        m_buffer.clear();
        m_stride = 0;
        m_frameIndex = 0;
    }

private:
    void generatePattern() {
        if (m_buffer.empty()) return;
        const int pixelBytes = m_format == CapturePixelFormat::Bgra32 ? 4 : 3;
        const std::uint8_t baseR = static_cast<std::uint8_t>((m_frameIndex * 13) % 255);
        const std::uint8_t baseG = static_cast<std::uint8_t>((m_frameIndex * 29) % 255);
        const std::uint8_t baseB = static_cast<std::uint8_t>((m_frameIndex * 47) % 255);

        for (int y = 0; y < m_region.height; ++y) {
            std::uint8_t* row = m_buffer.data() + static_cast<std::size_t>(y) * m_stride;
            for (int x = 0; x < m_region.width; ++x) {
                std::uint8_t* px = row + x * pixelBytes;
                px[0] = static_cast<std::uint8_t>((baseB + x * 2 + y) % 256); // B
                px[1] = static_cast<std::uint8_t>((baseG + y * 3) % 256);     // G
                px[2] = static_cast<std::uint8_t>((baseR + (x + y) * 2) % 256); // R
                if (pixelBytes == 4) px[3] = 255;                              // A
            }
        }
        ++m_frameIndex;
    }

    CaptureBackendInfo m_info;
    CapturePixelFormat m_format{CapturePixelFormat::Bgr24};
    CaptureRegion m_region;
    int m_stride = 0;
    int m_frameIndex = 0;
    std::vector<std::uint8_t> m_buffer;
};

static CaptureBackendFactory s_testBackendFactory = nullptr;

bool hasAttachedDxgiOutput() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    for (UINT adapterIndex = 0; ; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapterResult = factory->EnumAdapters1(adapterIndex, &adapter);
        if (adapterResult == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(adapterResult) || !adapter) continue;
        for (UINT outputIndex = 0; ; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            const HRESULT outputResult = adapter->EnumOutputs(outputIndex, &output);
            if (outputResult == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(outputResult) || !output) continue;
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) return true;
        }
    }
    return false;
}

}  // namespace

std::vector<CaptureBackendInfo> captureBackendCapabilities() {
    const bool dxgiAvailable = hasAttachedDxgiOutput();
    return {
        {"dxgi-desktop-duplication", "DXGI Desktop Duplication", dxgiAvailable, true,
         dxgiAvailable ? "" : "no attached DXGI output"},
        {"gdi-bitblt", "GDI BitBlt", true, false, {}}
    };
}

std::unique_ptr<ICaptureBackend> createMemoryCaptureBackend(CapturePixelFormat format) {
    return std::make_unique<MemoryCaptureBackend>(format);
}

void setCaptureBackendFactoryForTesting(CaptureBackendFactory factory) {
    s_testBackendFactory = std::move(factory);
}

bool isDesktopTemporarilyInaccessible() noexcept {
    HDESK inputDesk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!inputDesk) {
        return true;
    }
    CloseDesktop(inputDesk);
    return false;
}

std::unique_ptr<ICaptureBackend> createCaptureBackend() {
    if (s_testBackendFactory) {
        auto custom = s_testBackendFactory();
        if (custom) return custom;
    }
    return std::make_unique<AutomaticCaptureBackend>();
}

}  // namespace tools3000::capture
