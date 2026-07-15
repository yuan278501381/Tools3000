#include "WindowsIFilterExtractor.h"
#include "ContentMatchUtils.h"
#include "ContentTextCache.h"
#include "../SafeMappedRead.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <filter.h>
#include <filterr.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <new>
#include <vector>

namespace tools3000::service::content {

namespace {

constexpr uint64_t MaxIFilterFileBytes = 100ULL * 1024ULL * 1024ULL;
constexpr size_t MaxBinaryFallbackScanBytes = 64ULL * 1024ULL * 1024ULL;
constexpr size_t MaxExtractedTotalChars = 15ULL * 1024ULL * 1024ULL;
constexpr ULONG IFilterChunkReadChars = 16384;
constexpr size_t CancelCheckByteStride = 65536;

using PfnLoadIFilter = HRESULT (STDAPICALLTYPE*)(PCWSTR pwcsPath, IUnknown* pUnkOuter, void** ppIUnk);
using PfnBindIFilterFromStream = HRESULT (STDAPICALLTYPE*)(IStream* pStm, IUnknown* pUnkOuter, void** ppIUnk);

struct QueryDllFunctions {
    PfnLoadIFilter loadIFilter = nullptr;
    PfnBindIFilterFromStream bindIFilterFromStream = nullptr;
};

const QueryDllFunctions& resolveQueryDllFunctions() noexcept {
    static std::once_flag s_once;
    static QueryDllFunctions s_fns{};
    std::call_once(s_once, []() {
        HMODULE hMod = LoadLibraryExW(L"query.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!hMod) {
            hMod = LoadLibraryW(L"query.dll");
        }
        if (hMod) {
            s_fns.loadIFilter = reinterpret_cast<PfnLoadIFilter>(GetProcAddress(hMod, "LoadIFilter"));
            s_fns.bindIFilterFromStream =
                reinterpret_cast<PfnBindIFilterFromStream>(GetProcAddress(hMod, "BindIFilterFromStream"));
        }
    });
    return s_fns;
}

struct ScopedComApartment {
    bool shouldUninit = false;
    ScopedComApartment() noexcept {
        // Legacy Office/WPS/PDF IFilter in-proc servers commonly declare ThreadingModel=Apartment (STA).
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (SUCCEEDED(hr)) {
            shouldUninit = true;
        }
    }
    ~ScopedComApartment() {
        if (shouldUninit) {
            CoUninitialize();
        }
    }
    ScopedComApartment(const ScopedComApartment&) = delete;
    ScopedComApartment& operator=(const ScopedComApartment&) = delete;
};

std::wstring extractLeafFileName(std::wstring_view path) {
    const size_t sep = path.find_last_of(L"\\/");
    if (sep == std::wstring_view::npos) {
        return std::wstring(path);
    }
    return std::wstring(path.substr(sep + 1));
}

class MemoryViewReadStream final : public IStream {
public:
    MemoryViewReadStream(
        const uint8_t* data,
        size_t size,
        std::wstring leafName,
        const CancellationCheck* isCancelled
    ) noexcept
        : m_data(data)
        , m_size(size)
        , m_leafName(std::move(leafName))
        , m_isCancelled(isCancelled) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ISequentialStream) ||
            IsEqualIID(riid, IID_IStream)) {
            *ppvObject = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG next = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (next == 0) {
            delete this;
        }
        return next;
    }

    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
        if (pcbRead) *pcbRead = 0;
        if (cb == 0) return S_OK;
        if (!pv) return STG_E_INVALIDPOINTER;
        if (m_isCancelled && (*m_isCancelled) && (*m_isCancelled)()) {
            return E_ABORT;
        }
        if (!m_data || m_pos >= m_size) {
            return S_FALSE;
        }

        const size_t avail = m_size - m_pos;
        const size_t toRead = (std::min)(static_cast<size_t>(cb), avail);
        if (!safeMappedMemcpy(pv, m_data + m_pos, toRead)) {
            return STG_E_READFAULT;
        }

        m_pos += toRead;
        if (pcbRead) *pcbRead = static_cast<ULONG>(toRead);
        return (toRead == static_cast<size_t>(cb)) ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG* pcbWritten) override {
        if (pcbWritten) *pcbWritten = 0;
        return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Seek(
        LARGE_INTEGER dlibMove,
        DWORD dwOrigin,
        ULARGE_INTEGER* plibNewPosition
    ) override {
        int64_t base = 0;
        switch (dwOrigin) {
            case STREAM_SEEK_SET:
                base = 0;
                break;
            case STREAM_SEEK_CUR:
                base = static_cast<int64_t>(m_pos);
                break;
            case STREAM_SEEK_END:
                base = static_cast<int64_t>(m_size);
                break;
            default:
                return STG_E_INVALIDFUNCTION;
        }

        const int64_t target = base + dlibMove.QuadPart;
        if (target < 0) {
            return STG_E_INVALIDFUNCTION;
        }
        m_pos = (std::min)(static_cast<size_t>(target), m_size);
        if (plibNewPosition) {
            plibNewPosition->QuadPart = static_cast<ULONGLONG>(m_pos);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD grfStatFlag) override {
        if (!pstatstg) return STG_E_INVALIDPOINTER;
        std::memset(pstatstg, 0, sizeof(*pstatstg));
        pstatstg->type = STGTY_STREAM;
        pstatstg->cbSize.QuadPart = static_cast<ULONGLONG>(m_size);
        pstatstg->grfMode = STGM_READ | STGM_SHARE_DENY_NONE;
        if ((grfStatFlag & STATFLAG_NONAME) == 0 && !m_leafName.empty()) {
            const size_t byteLen = (m_leafName.size() + 1) * sizeof(wchar_t);
            auto* nameCopy = static_cast< OLECHAR* >(CoTaskMemAlloc(byteLen));
            if (!nameCopy) return E_OUTOFMEMORY;
            std::memcpy(nameCopy, m_leafName.c_str(), byteLen);
            pstatstg->pwcsName = nameCopy;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream** ppstm) override {
        if (!ppstm) return STG_E_INVALIDPOINTER;
        auto* copy = new (std::nothrow) MemoryViewReadStream(m_data, m_size, m_leafName, m_isCancelled);
        if (!copy) {
            *ppstm = nullptr;
            return E_OUTOFMEMORY;
        }
        copy->m_pos = m_pos;
        *ppstm = copy;
        return S_OK;
    }

private:
    ~MemoryViewReadStream() = default;

    std::atomic<ULONG> m_refCount{1};
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_pos = 0;
    std::wstring m_leafName;
    const CancellationCheck* m_isCancelled = nullptr;
};

bool hasAnyReadableChar(std::wstring_view text) noexcept {
    for (wchar_t ch : text) {
        if (ch > 0x20 && ch != 0xFFFD) {
            return true;
        }
    }
    return false;
}

bool drainInitializedIFilter(
    IFilter* pFilter,
    std::wstring& outText,
    const CancellationCheck& isCancelled
) {
    ULONG initFlags = 0;
    const HRESULT hrInit = pFilter->Init(
        IFILTER_INIT_CANON_PARAGRAPHS |
        IFILTER_INIT_HARD_LINE_BREAKS |
        IFILTER_INIT_CANON_HYPHENS |
        IFILTER_INIT_CANON_SPACES |
        IFILTER_INIT_APPLY_INDEX_ATTRIBUTES,
        0,
        nullptr,
        &initFlags
    );
    if (FAILED(hrInit)) {
        return false;
    }

    std::vector<wchar_t> chunkBuffer(IFilterChunkReadChars);
    while (outText.size() < MaxExtractedTotalChars) {
        if (isCancelled && isCancelled()) {
            outText.clear();
            return false;
        }

        STAT_CHUNK chunk{};
        const HRESULT hrChunk = pFilter->GetChunk(&chunk);
        if (hrChunk == FILTER_E_END_OF_CHUNKS) {
            break;
        }
        if (hrChunk == FILTER_E_EMBEDDING_UNAVAILABLE || hrChunk == FILTER_E_LINK_UNAVAILABLE) {
            continue;
        }
        if (FAILED(hrChunk)) {
            break;
        }

        if ((chunk.flags & CHUNK_TEXT) == 0) {
            continue;
        }

        if (chunk.breakType != CHUNK_NO_BREAK && !outText.empty() && outText.back() != L'\n') {
            outText.push_back(L'\n');
        }

        while (outText.size() < MaxExtractedTotalChars) {
            if (isCancelled && isCancelled()) {
                outText.clear();
                return false;
            }

            ULONG cwc = static_cast<ULONG>(chunkBuffer.size());
            const HRESULT hrText = pFilter->GetText(&cwc, chunkBuffer.data());
            if (FAILED(hrText) || cwc == 0) {
                break;
            }

            const size_t validCount = (std::min)(static_cast<size_t>(cwc), chunkBuffer.size());
            for (size_t i = 0; i < validCount && outText.size() < MaxExtractedTotalChars; ++i) {
                const wchar_t wc = chunkBuffer[i];
                if (wc == L'\0') continue;
                if (wc == L'\r') {
                    if (outText.empty() || outText.back() != L'\n') {
                        outText.push_back(L'\n');
                    }
                } else {
                    outText.push_back(wc);
                }
            }

            if (hrText == FILTER_S_LAST_TEXT) {
                break;
            }
        }
    }

    return hasAnyReadableChar(outText);
}

bool resolveVerifiedLocalPathFromHandle(HANDLE hFile, std::wstring& outCanonicalLocalPath) {
    outCanonicalLocalPath.clear();
    if (!hFile || hFile == INVALID_HANDLE_VALUE) return false;

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(hFile, &info)) {
        return false;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return false;
    }

    wchar_t stackBuf[MAX_PATH + 8]{};
    DWORD len = GetFinalPathNameByHandleW(hFile, stackBuf, MAX_PATH + 4, VOLUME_NAME_DOS);
    if (len == 0) {
        return false;
    }

    std::wstring finalPath;
    if (len <= MAX_PATH + 4) {
        finalPath.assign(stackBuf, len);
    } else {
        std::wstring dynBuf(static_cast<size_t>(len) + 1, L'\0');
        const DWORD len2 = GetFinalPathNameByHandleW(hFile, dynBuf.data(), len + 1, VOLUME_NAME_DOS);
        if (len2 == 0 || len2 > len) return false;
        dynBuf.resize(len2);
        finalPath = std::move(dynBuf);
    }

    // Reject UNC/network targets so IPersistFile::Load never initiates remote SMB/WebDAV auth.
    if (finalPath.starts_with(L"\\\\?\\UNC\\") ||
        (finalPath.starts_with(L"\\\\") && !finalPath.starts_with(L"\\\\?\\"))) {
        return false;
    }
    if (finalPath.starts_with(L"\\\\?\\") && finalPath.size() >= 6 && finalPath[5] == L':') {
        finalPath.erase(0, 4);
    }
    outCanonicalLocalPath = std::move(finalPath);
    return !outCanonicalLocalPath.empty();
}

bool isCjkOrFullwidthCodeUnit(uint16_t code) noexcept {
    return (code >= 0x4E00u && code <= 0x9FFFu) ||
           (code >= 0x3400u && code <= 0x4DBFu) ||
           (code >= 0x3000u && code <= 0x303Fu) ||
           (code >= 0xFF01u && code <= 0xFF5Eu) ||
           (code >= 0x2014u && code <= 0x2026u);
}

bool isPrintableAsciiOrWhitespaceCodeUnit(uint16_t code) noexcept {
    return (code >= 0x0020u && code <= 0x007Eu) ||
           code == 0x0009u || code == 0x000Au || code == 0x000Du;
}

bool isOle2FallbackExtension(std::wstring_view lowerExt) noexcept {
    return lowerExt == L"doc" || lowerExt == L"dot" ||
           lowerExt == L"xls" || lowerExt == L"xlt" ||
           lowerExt == L"ppt" || lowerExt == L"pot" ||
           lowerExt == L"wps" || lowerExt == L"wpt" ||
           lowerExt == L"et"  || lowerExt == L"ett" ||
           lowerExt == L"dps" || lowerExt == L"dpt" ||
           lowerExt == L"msg";
}

bool hasOle2Signature(const uint8_t* data, size_t size) noexcept {
    if (!data || size < 8) return false;
    uint8_t header[8]{};
    if (!safeMappedMemcpy(header, data, sizeof(header))) return false;
    static constexpr uint8_t kSig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    return std::memcmp(header, kSig, sizeof(kSig)) == 0;
}

UINT resolveGbkCodePage() noexcept {
    static const UINT s_cp = IsValidCodePage(936) ? 936u : (IsValidCodePage(54936) ? 54936u : CP_ACP);
    return s_cp;
}

void scanUtf16LePhase(
    const uint8_t* data,
    size_t size,
    size_t parityOffset,
    std::wstring& outText,
    const CancellationCheck& isCancelled
) {
    if (size < parityOffset + 4) return;

    std::wstring run;
    run.reserve(128);
    size_t cjkCount = 0;
    size_t asciiAlphaNumCount = 0;
    size_t printableCount = 0;

    auto flushRun = [&]() {
        if (cjkCount >= 2 ||
            (cjkCount >= 1 && printableCount >= 3) ||
            (printableCount >= 5 && asciiAlphaNumCount >= 3)) {
            for (wchar_t ch : run) {
                if (outText.size() >= MaxExtractedTotalChars) break;
                if (ch == L'\r') {
                    if (outText.empty() || outText.back() != L'\n') {
                        outText.push_back(L'\n');
                    }
                } else {
                    outText.push_back(ch);
                }
            }
            if (!outText.empty() && outText.back() != L'\n' &&
                outText.size() < MaxExtractedTotalChars) {
                outText.push_back(L'\n');
            }
        }
        run.clear();
        cjkCount = 0;
        asciiAlphaNumCount = 0;
        printableCount = 0;
    };

    for (size_t offset = parityOffset; offset + 1 < size; offset += 2) {
        if (((offset - parityOffset) & (CancelCheckByteStride - 1)) == 0) {
            if (isCancelled && isCancelled()) {
                return;
            }
            if (outText.size() >= MaxExtractedTotalChars) {
                return;
            }
        }

        const uint16_t u16 = static_cast<uint16_t>(data[offset]) |
                             (static_cast<uint16_t>(data[offset + 1]) << 8);

        if (isCjkOrFullwidthCodeUnit(u16)) {
            run.push_back(static_cast<wchar_t>(u16));
            ++cjkCount;
            ++printableCount;
        } else if (isPrintableAsciiOrWhitespaceCodeUnit(u16)) {
            run.push_back(static_cast<wchar_t>(u16));
            if (u16 >= 0x20u) {
                ++printableCount;
                if ((u16 >= '0' && u16 <= '9') ||
                    (u16 >= 'A' && u16 <= 'Z') ||
                    (u16 >= 'a' && u16 <= 'z')) {
                    ++asciiAlphaNumCount;
                }
            }
        } else if (u16 >= 0xD800u && u16 <= 0xDBFFu && offset + 3 < size) {
            const uint16_t low = static_cast<uint16_t>(data[offset + 2]) |
                                 (static_cast<uint16_t>(data[offset + 3]) << 8);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                run.push_back(static_cast<wchar_t>(u16));
                run.push_back(static_cast<wchar_t>(low));
                ++cjkCount;
                ++printableCount;
                offset += 2;
            } else {
                flushRun();
            }
        } else {
            if (!run.empty()) {
                flushRun();
            }
        }
    }

    if (!run.empty()) {
        flushRun();
    }
}

void scanAnsiAndGbkRuns(
    const uint8_t* data,
    size_t size,
    std::wstring& outText,
    const CancellationCheck& isCancelled
) {
    const UINT cp = resolveGbkCodePage();
    std::string byteRun;
    byteRun.reserve(128);
    size_t gbkPairs = 0;
    size_t asciiAlphaNum = 0;

    auto flushByteRun = [&]() {
        if (gbkPairs >= 2 || (byteRun.size() >= 6 && asciiAlphaNum >= 4)) {
            const int wlen = MultiByteToWideChar(
                cp, 0, byteRun.data(), static_cast<int>(byteRun.size()), nullptr, 0);
            if (wlen > 0 && outText.size() + static_cast<size_t>(wlen) + 1 <= MaxExtractedTotalChars) {
                std::wstring decoded(static_cast<size_t>(wlen), L'\0');
                MultiByteToWideChar(
                    cp, 0, byteRun.data(), static_cast<int>(byteRun.size()), decoded.data(), wlen);
                for (wchar_t ch : decoded) {
                    if (ch == L'\r') {
                        if (outText.empty() || outText.back() != L'\n') outText.push_back(L'\n');
                    } else if (ch >= 0x20 || ch == L'\n' || ch == L'\t') {
                        outText.push_back(ch);
                    }
                }
                if (!outText.empty() && outText.back() != L'\n') {
                    outText.push_back(L'\n');
                }
            }
        }
        byteRun.clear();
        gbkPairs = 0;
        asciiAlphaNum = 0;
    };

    size_t i = 0;
    while (i < size) {
        if ((i & (CancelCheckByteStride - 1)) == 0) {
            if (isCancelled && isCancelled()) return;
            if (outText.size() >= MaxExtractedTotalChars) return;
        }

        const uint8_t b = data[i];
        if ((b >= 0x20 && b <= 0x7E) || b == '\t' || b == '\n' || b == '\r') {
            byteRun.push_back(static_cast<char>(b));
            if ((b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z')) {
                ++asciiAlphaNum;
            }
            ++i;
        } else if (b >= 0xB0 && b <= 0xF7 && i + 1 < size) {
            const uint8_t b2 = data[i + 1];
            if (b2 >= 0xA1 && b2 <= 0xFE) {
                byteRun.push_back(static_cast<char>(b));
                byteRun.push_back(static_cast<char>(b2));
                ++gbkPairs;
                i += 2;
            } else {
                if (!byteRun.empty()) flushByteRun();
                ++i;
            }
        } else {
            if (!byteRun.empty()) flushByteRun();
            ++i;
        }
    }

    if (!byteRun.empty()) {
        flushByteRun();
    }
}

} // namespace

WindowsIFilterExtractor::WindowsIFilterExtractor() {
    const wchar_t* exts[] = {
        L"doc", L"dot", L"xls", L"xlt", L"ppt", L"pot",
        L"wps", L"wpt", L"et", L"ett", L"dps", L"dpt",
        L"eml", L"msg", L"chm", L"one"
    };
    for (const wchar_t* ext : exts) {
        m_supportedExts.insert(ext);
    }
}

bool WindowsIFilterExtractor::canHandle(std::wstring_view extension) const {
    const std::wstring lowerExt = ContentFormatFilter::normalizeExt(extension);
    if (lowerExt.empty()) return false;
    return m_supportedExts.find(lowerExt) != m_supportedExts.end();
}

bool WindowsIFilterExtractor::extractViaSystemIFilterFromHandle(
    void* rawHandle,
    const std::wstring& fileHintPath,
    const uint8_t* mappedData,
    size_t mappedSize,
    std::wstring& outText,
    const CancellationCheck& isCancelled
) {
    outText.clear();
    if (isCancelled && isCancelled()) return false;
    const HANDLE hFile = static_cast<HANDLE>(rawHandle);
    if (!hFile || hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    const QueryDllFunctions& fns = resolveQueryDllFunctions();
    if (!fns.bindIFilterFromStream && !fns.loadIFilter) {
        return false;
    }

    ScopedComApartment comGuard;

    // 1. Prefer stream-bound IFilter over our already-opened read-only mapped view:
    //    never opens any file path or activates path-based COM monikers.
    if (fns.bindIFilterFromStream && mappedData && mappedSize > 0) {
        auto* stream = new (std::nothrow) MemoryViewReadStream(
            mappedData,
            mappedSize,
            extractLeafFileName(fileHintPath),
            &isCancelled
        );
        if (stream) {
            struct StreamReleaser {
                IStream* ptr;
                ~StreamReleaser() {
                    if (ptr) ptr->Release();
                }
            } streamGuard{stream};

            IFilter* pFilter = nullptr;
            const HRESULT hrBind = fns.bindIFilterFromStream(
                stream,
                nullptr,
                reinterpret_cast<void**>(&pFilter)
            );
            if (SUCCEEDED(hrBind) && pFilter) {
                struct FilterReleaser {
                    IFilter* ptr;
                    ~FilterReleaser() {
                        if (ptr) ptr->Release();
                    }
                } filterGuard{pFilter};

                if (drainInitializedIFilter(pFilter, outText, isCancelled)) {
                    return true;
                }
                outText.clear();
            }
        }
    }

    if (isCancelled && isCancelled()) {
        outText.clear();
        return false;
    }

    // 2. Fallback for IPersistFile-only IFilters: hFile has ALREADY passed GENERIC_READ
    //    ACL verification under client impersonation. Resolve canonical local path from hFile.
    if (fns.loadIFilter) {
        std::wstring verifiedLocalPath;
        if (resolveVerifiedLocalPathFromHandle(hFile, verifiedLocalPath)) {
            IFilter* pFilter = nullptr;
            const HRESULT hrLoad = fns.loadIFilter(
                verifiedLocalPath.c_str(),
                nullptr,
                reinterpret_cast<void**>(&pFilter)
            );
            if (SUCCEEDED(hrLoad) && pFilter) {
                struct FilterReleaser {
                    IFilter* ptr;
                    ~FilterReleaser() {
                        if (ptr) ptr->Release();
                    }
                } filterGuard{pFilter};

                if (drainInitializedIFilter(pFilter, outText, isCancelled)) {
                    return true;
                }
                outText.clear();
            }
        }
    }

    return false;
}

bool WindowsIFilterExtractor::extractViaSystemIFilter(
    const std::wstring& filePath,
    std::wstring& outText,
    const CancellationCheck& isCancelled
) {
    outText.clear();
    if (isCancelled && isCancelled()) return false;

    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    struct HandleCloser {
        HANDLE h;
        ~HandleCloser() {
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    } fileCloser{hFile};

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 ||
        static_cast<uint64_t>(fileSize.QuadPart) > MaxIFilterFileBytes) {
        return false;
    }

    const size_t mapSize = static_cast<size_t>(fileSize.QuadPart);
    HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) {
        return extractViaSystemIFilterFromHandle(hFile, filePath, nullptr, 0, outText, isCancelled);
    }
    HandleCloser mapCloser{hMapping};

    const auto* pData = static_cast<const uint8_t*>(MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, mapSize));
    struct ViewUnmapper {
        const void* ptr;
        ~ViewUnmapper() {
            if (ptr) UnmapViewOfFile(ptr);
        }
    } viewGuard{pData};

    return extractViaSystemIFilterFromHandle(hFile, filePath, pData, pData ? mapSize : 0, outText, isCancelled);
}

bool WindowsIFilterExtractor::extractViaBinaryTextScan(
    const uint8_t* data,
    size_t size,
    std::wstring& outText,
    const CancellationCheck& isCancelled
) {
    if (!data || size == 0) return false;
    const size_t scanLen = (std::min)(size, MaxBinaryFallbackScanBytes);

    const bool ok = runWithMappedViewGuard([&]() -> bool {
        scanUtf16LePhase(data, scanLen, 0, outText, isCancelled);
        if (isCancelled && isCancelled()) {
            outText.clear();
            return false;
        }

        // BIFF8 / variable-prefix records in .xls/.doc may align UTF-16LE strings on odd byte offsets
        scanUtf16LePhase(data, scanLen, 1, outText, isCancelled);
        if (isCancelled && isCancelled()) {
            outText.clear();
            return false;
        }

        scanAnsiAndGbkRuns(data, scanLen, outText, isCancelled);
        if (isCancelled && isCancelled()) {
            outText.clear();
            return false;
        }
        return true;
    });

    if (!ok) {
        outText.clear();
        return false;
    }

    return hasAnyReadableChar(outText);
}

bool WindowsIFilterExtractor::extractDocumentText(
    const std::wstring& filePath,
    std::wstring_view lowerExt,
    std::wstring& outText,
    const CancellationCheck& isCancelled
) {
    outText.clear();
    if (isCancelled && isCancelled()) return false;

    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    struct HandleCloser {
        HANDLE h;
        ~HandleCloser() {
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    } fileCloser{hFile};

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 ||
        static_cast<uint64_t>(fileSize.QuadPart) > MaxIFilterFileBytes) {
        return false;
    }

    const size_t mapSize = static_cast<size_t>(fileSize.QuadPart);
    HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) {
        return extractViaSystemIFilterFromHandle(hFile, filePath, nullptr, 0, outText, isCancelled);
    }
    HandleCloser mapCloser{hMapping};

    const auto* pData = static_cast<const uint8_t*>(MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, mapSize));
    if (!pData) {
        return extractViaSystemIFilterFromHandle(hFile, filePath, nullptr, 0, outText, isCancelled);
    }
    struct ViewUnmapper {
        const void* ptr;
        ~ViewUnmapper() {
            if (ptr) UnmapViewOfFile(ptr);
        }
    } viewGuard{pData};

    if (extractViaSystemIFilterFromHandle(hFile, filePath, pData, mapSize, outText, isCancelled)) {
        return true;
    }
    if (isCancelled && isCancelled()) {
        outText.clear();
        return false;
    }

    if (!isOle2FallbackExtension(lowerExt) && !hasOle2Signature(pData, mapSize)) {
        return false;
    }

    return extractViaBinaryTextScan(pData, mapSize, outText, isCancelled);
}

bool WindowsIFilterExtractor::searchContent(
    const std::wstring& filePath,
    std::wstring_view queryPattern,
    bool caseSensitive,
    std::vector<ContentSnippet>& outSnippets,
    size_t maxSnippetsPerFile
) {
    return searchContentTerms(
        filePath,
        splitQueryPatternTerms(queryPattern),
        caseSensitive,
        outSnippets,
        maxSnippetsPerFile,
        {}
    );
}

bool WindowsIFilterExtractor::searchContentTerms(
    const std::wstring& filePath,
    const std::vector<std::wstring>& queryTerms,
    bool caseSensitive,
    std::vector<ContentSnippet>& outSnippets,
    size_t maxSnippetsPerFile,
    const CancellationCheck& isCancelled
) {
    if (queryTerms.empty() || (isCancelled && isCancelled())) {
        return false;
    }

    // Open GENERIC_READ handle FIRST so NTFS ACLs are enforced under client impersonation
    // BEFORE consulting ContentTextCache.
    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    struct HandleCloser {
        HANDLE h;
        ~HandleCloser() {
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    } fileCloser{hFile};

    const FileCacheStamp stamp = queryFileCacheStampByHandle(hFile);
    if (!stamp.valid || stamp.fileSize == 0 || stamp.fileSize > MaxIFilterFileBytes) {
        return false;
    }

    std::wstring extractedText;
    if (!ContentTextCache::instance().tryGet(filePath, stamp.fileSize, stamp.lastWriteTime, extractedText)) {
        std::wstring lowerExt;
        const size_t dotPos = filePath.rfind(L'.');
        if (dotPos != std::wstring::npos) {
            lowerExt = ContentFormatFilter::normalizeExt(std::wstring_view(filePath).substr(dotPos + 1));
        }

        const size_t mapSize = static_cast<size_t>(stamp.fileSize);
        HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (hMapping) {
            HandleCloser mapCloser{hMapping};
            const auto* pData = static_cast<const uint8_t*>(
                MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, mapSize));
            if (pData) {
                struct ViewUnmapper {
                    const void* ptr;
                    ~ViewUnmapper() {
                        if (ptr) UnmapViewOfFile(ptr);
                    }
                } viewGuard{pData};

                bool extracted = extractViaSystemIFilterFromHandle(
                    hFile, filePath, pData, mapSize, extractedText, isCancelled);
                if (!extracted && !(isCancelled && isCancelled()) &&
                    (isOle2FallbackExtension(lowerExt) || hasOle2Signature(pData, mapSize))) {
                    extractViaBinaryTextScan(pData, mapSize, extractedText, isCancelled);
                }
            } else {
                extractViaSystemIFilterFromHandle(hFile, filePath, nullptr, 0, extractedText, isCancelled);
            }
        } else {
            extractViaSystemIFilterFromHandle(hFile, filePath, nullptr, 0, extractedText, isCancelled);
        }

        if (isCancelled && isCancelled()) {
            return false;
        }

        ContentTextCache::instance().put(filePath, stamp.fileSize, stamp.lastWriteTime, extractedText);
    }

    if (extractedText.empty()) {
        return false;
    }

    return matchDocumentTextAndBuildSnippets(
        extractedText,
        queryTerms,
        caseSensitive,
        outSnippets,
        maxSnippetsPerFile,
        isCancelled
    );
}

} // namespace tools3000::service::content
