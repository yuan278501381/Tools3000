// ─────────────────────────────────────────────────────────────────────────────
// MonitorBrightnessController.cpp — 多屏硬件亮度双引擎控制器实现 (DDC/CI + WMI)
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/MonitorBrightnessController.h"
#include "core/logger/Logger.h"

#include <comdef.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace tools3000::gesture {

namespace {

class ScopedComInit {
public:
    ScopedComInit() noexcept {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            m_shouldUninit = true;
            m_ok = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            // 线程已以 STA 初始化，可直接复用当前套间调用 WMI
            m_ok = true;
        }
    }
    ~ScopedComInit() noexcept {
        if (m_shouldUninit) {
            CoUninitialize();
        }
    }
    bool ok() const noexcept { return m_ok; }

private:
    bool m_ok = false;
    bool m_shouldUninit = false;
};

struct Dxva2Api {
    using FnGetNum = BOOL(WINAPI*)(HMONITOR, LPDWORD);
    using FnGetPhys = BOOL(WINAPI*)(HMONITOR, DWORD, LPPHYSICAL_MONITOR);
    using FnDestroyPhys = BOOL(WINAPI*)(DWORD, LPPHYSICAL_MONITOR);
    using FnGetBri = BOOL(WINAPI*)(HANDLE, LPDWORD, LPDWORD, LPDWORD);
    using FnSetBri = BOOL(WINAPI*)(HANDLE, DWORD);
    using FnGetVcp = BOOL(WINAPI*)(HANDLE, BYTE, LPMC_VCP_CODE_TYPE, LPDWORD, LPDWORD);
    using FnSetVcp = BOOL(WINAPI*)(HANDLE, BYTE, DWORD);

    HMODULE module = nullptr;
    FnGetNum getNum = nullptr;
    FnGetPhys getPhys = nullptr;
    FnDestroyPhys destroyPhys = nullptr;
    FnGetBri getBri = nullptr;
    FnSetBri setBri = nullptr;
    FnGetVcp getVcp = nullptr;
    FnSetVcp setVcp = nullptr;

    static const Dxva2Api& get() noexcept {
        static Dxva2Api api = []() {
            Dxva2Api a{};
            a.module = LoadLibraryW(L"dxva2.dll");
            if (a.module) {
                a.getNum = reinterpret_cast<FnGetNum>(GetProcAddress(a.module, "GetNumberOfPhysicalMonitorsFromHMONITOR"));
                a.getPhys = reinterpret_cast<FnGetPhys>(GetProcAddress(a.module, "GetPhysicalMonitorsFromHMONITOR"));
                a.destroyPhys = reinterpret_cast<FnDestroyPhys>(GetProcAddress(a.module, "DestroyPhysicalMonitors"));
                a.getBri = reinterpret_cast<FnGetBri>(GetProcAddress(a.module, "GetMonitorBrightness"));
                a.setBri = reinterpret_cast<FnSetBri>(GetProcAddress(a.module, "SetMonitorBrightness"));
                a.getVcp = reinterpret_cast<FnGetVcp>(GetProcAddress(a.module, "GetVCPFeatureAndVCPFeatureReply"));
                a.setVcp = reinterpret_cast<FnSetVcp>(GetProcAddress(a.module, "SetVCPFeature"));
            }
            return a;
        }();
        return api;
    }

    bool valid() const noexcept {
        return module != nullptr && getNum != nullptr && getPhys != nullptr && destroyPhys != nullptr &&
               ((getBri != nullptr && setBri != nullptr) || (getVcp != nullptr && setVcp != nullptr));
    }
};

struct PhysicalMonitorGuard {
    const Dxva2Api& api;
    std::vector<PHYSICAL_MONITOR> monitors;

    ~PhysicalMonitorGuard() noexcept {
        if (!monitors.empty() && api.destroyPhys) {
            api.destroyPhys(static_cast<DWORD>(monitors.size()), monitors.data());
        }
    }
};

bool connectWmiNamespace(ComPtr<IWbemServices>& outServices) {
    ComPtr<IWbemLocator> locator;
    HRESULT hr = CoCreateInstance(
        CLSID_WbemLocator,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,
        reinterpret_cast<void**>(locator.GetAddressOf()));
    if (FAILED(hr) || !locator) {
        return false;
    }

    _bstr_t ns(L"ROOT\\WMI");
    hr = locator->ConnectServer(
        ns,
        nullptr,
        nullptr,
        nullptr,
        0,
        nullptr,
        nullptr,
        outServices.GetAddressOf());
    if (FAILED(hr) || !outServices) {
        return false;
    }

    (void)CoSetProxyBlanket(
        outServices.Get(),
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        nullptr,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        EOAC_NONE);
    return true;
}

bool isInternalTechnology(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY tech) noexcept {
    switch (tech) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
            return true;
        default:
            return false;
    }
}

}  // namespace

MonitorBrightnessController& MonitorBrightnessController::instance() {
    static MonitorBrightnessController inst;
    return inst;
}

MonitorBrightnessController::~MonitorBrightnessController() {
    shutdown();
}

void MonitorBrightnessController::shutdown() {
    std::jthread workerToJoin;
    {
        std::lock_guard lock(m_mutex);
        m_hasPending = false;
        m_pendingDelta = 0;
        m_pendingMonitor = nullptr;
        m_pendingCallback = nullptr;
        m_cache.clear();
        if (m_worker.joinable()) {
            m_worker.request_stop();
            workerToJoin = std::move(m_worker);
        }
    }
    m_cv.notify_all();
    if (workerToJoin.joinable()) {
        workerToJoin.join();
    }
}

void MonitorBrightnessController::ensureWorkerStartedLocked() {
    if (!m_worker.joinable()) {
        m_worker = std::jthread([this](std::stop_token st) { workerLoop(st); });
    }
}

void MonitorBrightnessController::adjustAtPointAsync(
    POINT screenPt,
    int deltaPercent,
    FeedbackCallback onApplied) {
    if (deltaPercent == 0) return;

    HMONITOR hMon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
    if (!hMon) {
        hMon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    if (!hMon) return;

    int instantPreviewPercent = -1;
    FeedbackCallback previewCb;
    {
        std::lock_guard lock(m_mutex);
        ensureWorkerStartedLocked();

        if (m_hasPending && m_pendingMonitor == hMon) {
            m_pendingDelta += deltaPercent;
        } else {
            m_pendingMonitor = hMon;
            m_pendingDelta = deltaPercent;
            m_hasPending = true;
        }
        m_pendingCallback = onApplied;

        // 若该显示器 1500ms 内已有热缓存，立即推演预估目标亮度以实现 0ms 零延迟 HUD 回显
        const uint64_t nowMs = GetTickCount64();
        auto it = m_cache.find(hMon);
        if (it != m_cache.end() && (nowMs - it->second.updatedAtMs) <= kBrightnessCacheTtlMs) {
            const int predicted = stepBrightnessPercent(it->second.percent, m_pendingDelta);
            instantPreviewPercent = predicted;
            previewCb = onApplied;
        }
    }

    if (instantPreviewPercent >= 0 && previewCb) {
        previewCb(instantPreviewPercent);
    }
    m_cv.notify_one();
}

void MonitorBrightnessController::workerLoop(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        HMONITOR targetMon = nullptr;
        int delta = 0;
        FeedbackCallback callback;
        {
            std::unique_lock lock(m_mutex);
            if (!m_cv.wait(lock, stopToken, [this]() { return m_hasPending; })) {
                break;
            }
            targetMon = m_pendingMonitor;
            delta = m_pendingDelta;
            callback = m_pendingCallback;
            m_hasPending = false;
            m_pendingDelta = 0;
        }

        if (!targetMon || delta == 0) continue;

        const BrightnessAdjustResult res = adjustMonitorSync(targetMon, delta);
        if (res.success && res.percent >= 0 && callback) {
            // 若期间未堆积新的同屏调节增量，刷新精准硬件百分比至 OSD
            bool hasMorePending = false;
            {
                std::lock_guard lock(m_mutex);
                hasMorePending = m_hasPending && m_pendingMonitor == targetMon;
            }
            if (!hasMorePending) {
                callback(res.percent);
            }
        }
    }
}

bool MonitorBrightnessController::isInternalDisplayMonitor(HMONITOR hMonitor) noexcept {
    if (!hMonitor) return false;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonitor, &mi)) {
        return false;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS ||
        pathCount == 0) {
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr) != ERROR_SUCCESS) {
        return false;
    }

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName{};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = paths[i].sourceInfo.adapterId;
        srcName.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS) {
            continue;
        }
        if (_wcsicmp(srcName.viewGdiDeviceName, mi.szDevice) == 0) {
            return isInternalTechnology(paths[i].targetInfo.outputTechnology);
        }
    }
    return false;
}

bool MonitorBrightnessController::queryWmiBrightness(int& outPercent) {
    ScopedComInit com;
    if (!com.ok()) return false;

    ComPtr<IWbemServices> services;
    if (!connectWmiNamespace(services)) return false;

    ComPtr<IEnumWbemClassObject> enumerator;
    HRESULT hr = services->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        enumerator.GetAddressOf());
    if (FAILED(hr) || !enumerator) return false;

    ComPtr<IWbemClassObject> obj;
    ULONG returned = 0;
    hr = enumerator->Next(1000, 1, obj.GetAddressOf(), &returned);
    if (FAILED(hr) || returned == 0 || !obj) return false;

    VARIANT vt{};
    VariantInit(&vt);
    hr = obj->Get(L"CurrentBrightness", 0, &vt, nullptr, nullptr);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        if (vt.vt == VT_UI1) {
            outPercent = clampBrightnessPercent(static_cast<int>(vt.bVal));
            ok = true;
        } else if (vt.vt == VT_I4 || vt.vt == VT_UI4 || vt.vt == VT_INT) {
            outPercent = clampBrightnessPercent(static_cast<int>(vt.lVal));
            ok = true;
        }
    }
    VariantClear(&vt);
    return ok;
}

bool MonitorBrightnessController::setWmiBrightness(int targetPercent) {
    ScopedComInit com;
    if (!com.ok()) return false;

    ComPtr<IWbemServices> services;
    if (!connectWmiNamespace(services)) return false;

    ComPtr<IWbemClassObject> cls;
    HRESULT hr = services->GetObject(
        _bstr_t(L"WmiMonitorBrightnessMethods"),
        0,
        nullptr,
        cls.GetAddressOf(),
        nullptr);
    if (FAILED(hr) || !cls) return false;

    ComPtr<IWbemClassObject> inParamsDef;
    hr = cls->GetMethod(L"WmiSetBrightness", 0, inParamsDef.GetAddressOf(), nullptr);
    if (FAILED(hr) || !inParamsDef) return false;

    ComPtr<IEnumWbemClassObject> enumerator;
    hr = services->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT InstanceName FROM WmiMonitorBrightnessMethods WHERE Active=TRUE"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        enumerator.GetAddressOf());
    if (FAILED(hr) || !enumerator) return false;

    ComPtr<IWbemClassObject> instObj;
    ULONG returned = 0;
    hr = enumerator->Next(1000, 1, instObj.GetAddressOf(), &returned);
    if (FAILED(hr) || returned == 0 || !instObj) return false;

    VARIANT vtPath{};
    VariantInit(&vtPath);
    hr = instObj->Get(L"__RELPATH", 0, &vtPath, nullptr, nullptr);
    if (FAILED(hr) || vtPath.vt != VT_BSTR || !vtPath.bstrVal) {
        VariantClear(&vtPath);
        return false;
    }

    ComPtr<IWbemClassObject> inParams;
    hr = inParamsDef->SpawnInstance(0, inParams.GetAddressOf());
    if (FAILED(hr) || !inParams) {
        VariantClear(&vtPath);
        return false;
    }

    VARIANT vtTimeout{};
    VariantInit(&vtTimeout);
    vtTimeout.vt = VT_I4;
    vtTimeout.lVal = 0;
    (void)inParams->Put(L"Timeout", 0, &vtTimeout, 0);
    VariantClear(&vtTimeout);

    VARIANT vtBright{};
    VariantInit(&vtBright);
    vtBright.vt = VT_UI1;
    vtBright.bVal = static_cast<BYTE>(clampBrightnessPercent(targetPercent));
    (void)inParams->Put(L"Brightness", 0, &vtBright, 0);
    VariantClear(&vtBright);

    ComPtr<IWbemClassObject> outParams;
    hr = services->ExecMethod(
        vtPath.bstrVal,
        _bstr_t(L"WmiSetBrightness"),
        0,
        nullptr,
        inParams.Get(),
        outParams.GetAddressOf(),
        nullptr);
    VariantClear(&vtPath);
    return SUCCEEDED(hr);
}

bool MonitorBrightnessController::queryDdcBrightness(
    HMONITOR hMonitor,
    DWORD& outCur,
    DWORD& outMin,
    DWORD& outMax) {
    const auto& api = Dxva2Api::get();
    if (!api.valid() || !hMonitor) return false;

    DWORD count = 0;
    if (!api.getNum(hMonitor, &count) || count == 0) return false;

    PhysicalMonitorGuard guard{api, std::vector<PHYSICAL_MONITOR>(count)};
    if (!api.getPhys(hMonitor, count, guard.monitors.data())) return false;

    for (const auto& pm : guard.monitors) {
        if (api.getBri) {
            DWORD minV = 0, curV = 0, maxV = 0;
            if (api.getBri(pm.hPhysicalMonitor, &minV, &curV, &maxV)) {
                outMin = minV;
                outCur = curV;
                outMax = maxV;
                return true;
            }
        }
        if (api.getVcp) {
            MC_VCP_CODE_TYPE codeType{};
            DWORD curV = 0, maxV = 0;
            if (api.getVcp(pm.hPhysicalMonitor, kVcpCodeLuminance, &codeType, &curV, &maxV)) {
                outMin = 0;
                outCur = curV;
                outMax = maxV > 0 ? maxV : 100;
                return true;
            }
        }
    }
    return false;
}

bool MonitorBrightnessController::setDdcBrightness(
    HMONITOR hMonitor,
    int targetPercent,
    DWORD minRaw,
    DWORD maxRaw,
    bool hasRange) {
    const auto& api = Dxva2Api::get();
    if (!api.valid() || !hMonitor) return false;

    DWORD count = 0;
    if (!api.getNum(hMonitor, &count) || count == 0) return false;

    PhysicalMonitorGuard guard{api, std::vector<PHYSICAL_MONITOR>(count)};
    if (!api.getPhys(hMonitor, count, guard.monitors.data())) return false;

    const DWORD targetRaw = hasRange
        ? mapPercentToRawRange(targetPercent, minRaw, maxRaw)
        : static_cast<DWORD>(clampBrightnessPercent(targetPercent));

    bool anyOk = false;
    for (const auto& pm : guard.monitors) {
        if (api.setBri && api.setBri(pm.hPhysicalMonitor, targetRaw)) {
            anyOk = true;
            continue;
        }
        if (api.setVcp && api.setVcp(pm.hPhysicalMonitor, kVcpCodeLuminance, targetRaw)) {
            anyOk = true;
        }
    }
    return anyOk;
}

BrightnessAdjustResult MonitorBrightnessController::adjustMonitorSync(
    HMONITOR hMonitor,
    int deltaPercent) {
    BrightnessAdjustResult result{};
    if (!hMonitor || deltaPercent == 0) return result;

    const uint64_t nowMs = GetTickCount64();
    bool cacheHit = false;
    CachedMonitorState cached{};
    {
        std::lock_guard lock(m_mutex);
        auto it = m_cache.find(hMonitor);
        if (it != m_cache.end() && (nowMs - it->second.updatedAtMs) <= kBrightnessCacheTtlMs) {
            cached = it->second;
            cacheHit = true;
        }
    }

    if (cacheHit) {
        const int nextPercent = stepBrightnessPercent(cached.percent, deltaPercent);
        bool ok = false;
        if (cached.isWmi) {
            ok = setWmiBrightness(nextPercent);
        } else {
            ok = setDdcBrightness(hMonitor, nextPercent, cached.minRaw, cached.maxRaw, cached.hasRange);
        }
        if (ok) {
            cached.percent = nextPercent;
            cached.updatedAtMs = GetTickCount64();
            {
                std::lock_guard lock(m_mutex);
                m_cache[hMonitor] = cached;
            }
            result.success = true;
            result.percent = nextPercent;
            result.usedWmi = cached.isWmi;
            return result;
        }
    }

    const bool preferWmi = isInternalDisplayMonitor(hMonitor);

    auto tryWmiPath = [&]() -> bool {
        int curPercent = 50;
        if (!queryWmiBrightness(curPercent)) return false;
        const int nextPercent = stepBrightnessPercent(curPercent, deltaPercent);
        if (!setWmiBrightness(nextPercent)) return false;

        CachedMonitorState st{};
        st.percent = nextPercent;
        st.minRaw = 0;
        st.maxRaw = 100;
        st.updatedAtMs = GetTickCount64();
        st.hasRange = true;
        st.isWmi = true;
        {
            std::lock_guard lock(m_mutex);
            m_cache[hMonitor] = st;
        }
        result.success = true;
        result.percent = nextPercent;
        result.usedWmi = true;
        return true;
    };

    auto tryDdcPath = [&]() -> bool {
        DWORD curRaw = 50, minRaw = 0, maxRaw = 100;
        if (!queryDdcBrightness(hMonitor, curRaw, minRaw, maxRaw)) return false;
        const int curPercent = mapRawRangeToPercent(curRaw, minRaw, maxRaw);
        const int nextPercent = stepBrightnessPercent(curPercent, deltaPercent);
        if (!setDdcBrightness(hMonitor, nextPercent, minRaw, maxRaw, maxRaw > minRaw)) return false;

        CachedMonitorState st{};
        st.percent = nextPercent;
        st.minRaw = minRaw;
        st.maxRaw = maxRaw;
        st.updatedAtMs = GetTickCount64();
        st.hasRange = (maxRaw > minRaw);
        st.isWmi = false;
        {
            std::lock_guard lock(m_mutex);
            m_cache[hMonitor] = st;
        }
        result.success = true;
        result.percent = nextPercent;
        result.usedWmi = false;
        return true;
    };

    if (preferWmi) {
        if (tryWmiPath()) return result;
        if (tryDdcPath()) return result;
    } else {
        if (tryDdcPath()) return result;
        // 仅当显示器无法走 DDC/CI 且整机只有单屏时才降级尝试 WMI，避免误调笔记本内屏
        if (GetSystemMetrics(SM_CMONITORS) <= 1 && tryWmiPath()) return result;
    }

    LOG_WARN("MonitorBrightnessController: failed to adjust brightness (preferWmi={}, delta={})",
             preferWmi ? "true" : "false", deltaPercent);
    return result;
}

}  // namespace tools3000::gesture
