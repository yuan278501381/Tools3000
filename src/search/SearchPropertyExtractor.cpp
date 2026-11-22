#include "search/SearchPropertyExtractor.h"
#include "core/utils/WinUtils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <objbase.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <list>
#include <mutex>
#include <numeric>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace tools3000::search {
namespace {

using Microsoft::WRL::ComPtr;

// 策略桶位掩码：每次提取仅激活所请求列真正需要的底层引擎，杜绝多余磁盘或 COM 开销。
enum ExtractorBucket : uint32_t {
    kBucketNone          = 0u,
    kBucketStat          = 1u << 0,
    kBucketShellType     = 1u << 1,
    kBucketWicImage      = 1u << 2,
    kBucketVersion       = 1u << 3,
    kBucketPropertyStore = 1u << 4,
    kBucketFolderStats   = 1u << 5,
    kBucketHash          = 1u << 6,
    kBucketOwner         = 1u << 7,
    kBucketPureString    = 1u << 8,
};

struct ScopedComInit {
    bool shouldUninit = false;
    ScopedComInit() noexcept {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (SUCCEEDED(hr)) {
            shouldUninit = true;
        }
    }
    ~ScopedComInit() noexcept {
        if (shouldUninit) {
            CoUninitialize();
        }
    }
    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit& operator=(const ScopedComInit&) = delete;
};

struct CachedFileEntry {
    uint64_t fileSize = 0;
    uint64_t lastWriteTime = 0;
    std::unordered_map<std::string, ExtractedPropertyValue> props;
};

class PropertyLruCache {
public:
    explicit PropertyLruCache(size_t capacity) : m_capacity(capacity) {}

    bool lookup(
        const std::string& key,
        uint64_t fileSize,
        uint64_t lastWriteTime,
        const std::vector<std::string>& requestedProps,
        std::unordered_map<std::string, ExtractedPropertyValue>& outFound,
        std::vector<std::string>& outMissing
    ) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_index.find(key);
        if (it == m_index.end()) {
            outMissing = requestedProps;
            return false;
        }
        auto& node = *(it->second);
        if (node.second.fileSize != fileSize || node.second.lastWriteTime != lastWriteTime) {
            m_list.erase(it->second);
            m_index.erase(it);
            outMissing = requestedProps;
            return false;
        }
        // 提升至 LRU 头部
        m_list.splice(m_list.begin(), m_list, it->second);
        bool allHit = true;
        outMissing.clear();
        for (const auto& propId : requestedProps) {
            auto pit = node.second.props.find(propId);
            if (pit != node.second.props.end()) {
                outFound.emplace(propId, pit->second);
            } else {
                outMissing.push_back(propId);
                allHit = false;
            }
        }
        return allHit;
    }

    void upsert(
        const std::string& key,
        uint64_t fileSize,
        uint64_t lastWriteTime,
        const std::unordered_map<std::string, ExtractedPropertyValue>& newProps
    ) {
        if (newProps.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_index.find(key);
        if (it != m_index.end()) {
            auto& entry = it->second->second;
            if (entry.fileSize != fileSize || entry.lastWriteTime != lastWriteTime) {
                entry.fileSize = fileSize;
                entry.lastWriteTime = lastWriteTime;
                entry.props.clear();
            }
            for (const auto& [k, v] : newProps) {
                entry.props[k] = v;
            }
            m_list.splice(m_list.begin(), m_list, it->second);
            return;
        }
        while (m_list.size() >= m_capacity) {
            const auto& victim = m_list.back();
            m_index.erase(victim.first);
            m_list.pop_back();
        }
        CachedFileEntry entry;
        entry.fileSize = fileSize;
        entry.lastWriteTime = lastWriteTime;
        entry.props = newProps;
        m_list.emplace_front(key, std::move(entry));
        m_index.emplace(key, m_list.begin());
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_index.clear();
        m_list.clear();
    }

private:
    size_t m_capacity;
    std::mutex m_mutex;
    std::list<std::pair<std::string, CachedFileEntry>> m_list;
    std::unordered_map<std::string, decltype(m_list)::iterator> m_index;
};

PropertyLruCache& sharedPropertyCache() {
    static PropertyLruCache cache(SearchPropertyExtractor::kPropertyCacheCapacity);
    return cache;
}

std::mutex& sharedShellTypeMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::wstring, std::string>& sharedShellTypeMap() {
    static std::unordered_map<std::wstring, std::string> cache;
    return cache;
}

std::mutex& sharedSidCacheMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::wstring, std::string>& sharedSidNameCache() {
    static std::unordered_map<std::wstring, std::string> cache;
    return cache;
}

std::string normalizePathCacheKey(std::string_view utf8Path) {
    std::string out;
    out.reserve(utf8Path.size());
    for (unsigned char ch : utf8Path) {
        if (ch == '/') {
            out.push_back('\\');
        } else if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

bool isSpecialVirtualPath(std::wstring_view path) noexcept {
    if (path.size() >= 6 && _wcsnicmp(path.data(), L"shell:", 6) == 0) {
        return true;
    }
    return false;
}

std::wstring extractExtensionLower(const std::wstring& path, bool isDirectory) {
    if (isDirectory || path.empty()) return {};
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot <= slash) || dot + 1 >= path.size()) {
        return {};
    }
    std::wstring ext = path.substr(dot); // 包含 '.'
    for (auto& ch : ext) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        } else if (ch >= 0x80) {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
    }
    return ext;
}

std::wstring extractFileNameFromPath(const std::wstring& path) {
    if (path.empty()) return {};
    size_t end = path.size();
    while (end > 3 && (path[end - 1] == L'\\' || path[end - 1] == L'/')) {
        --end;
    }
    const size_t slash = path.find_last_of(L"\\/", end - 1);
    if (slash == std::wstring::npos) return path.substr(0, end);
    return path.substr(slash + 1, end - slash - 1);
}

std::wstring extractParentNameFromPath(const std::wstring& path) {
    if (path.empty()) return {};
    size_t end = path.size();
    while (end > 3 && (path[end - 1] == L'\\' || path[end - 1] == L'/')) {
        --end;
    }
    const size_t slash = path.find_last_of(L"\\/", end - 1);
    if (slash == std::wstring::npos || slash == 0) return {};
    const std::wstring parentDir = path.substr(0, slash);
    if (parentDir.size() == 2 && parentDir[1] == L':') {
        return parentDir + L"\\";
    }
    const size_t prevSlash = parentDir.find_last_of(L"\\/");
    if (prevSlash == std::wstring::npos) return parentDir;
    return parentDir.substr(prevSlash + 1);
}

bool isWicCandidateExtension(const std::wstring& extLower) {
    static const std::unordered_set<std::wstring> kImageExts = {
        L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".bmp", L".dib",
        L".gif", L".tif", L".tiff", L".ico", L".cur", L".webp", L".wdp",
        L".hdp", L".jxr", L".heic", L".heif", L".avif", L".dds", L".dng",
        L".cr2", L".nef", L".arw", L".orf", L".rw2"
    };
    return kImageExts.find(extLower) != kImageExts.end();
}

bool isVersionInfoCandidateExtension(const std::wstring& extLower) {
    static const std::unordered_set<std::wstring> kPeExts = {
        L".exe", L".dll", L".sys", L".ocx", L".scr", L".cpl",
        L".drv", L".mui", L".ax", L".acm", L".tsp", L".efi", L".com"
    };
    return kPeExts.find(extLower) != kPeExts.end();
}

bool isRichPropertyStoreExtension(const std::wstring& extLower) {
    static const std::unordered_set<std::wstring> kRichExts = {
        // 图片
        L".jpg", L".jpeg", L".tif", L".tiff", L".png", L".gif", L".bmp", L".webp", L".heic", L".heif", L".dng", L".cr2", L".nef",
        // 音频
        L".mp3", L".flac", L".wav", L".m4a", L".aac", L".ogg", L".wma", L".opus", L".alac", L".aiff",
        // 视频
        L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".webm", L".m4v", L".mpg", L".mpeg", L".ts", L".flv", L".3gp",
        // 文档
        L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".pdf", L".odt", L".ods", L".odp", L".rtf", L".vsdx",
        // 可执行/快捷方式
        L".exe", L".dll", L".msi", L".lnk"
    };
    return kRichExts.find(extLower) != kRichExts.end();
}

enum class PropStoreFormatKind : uint8_t {
    DisplayString = 0,
    Duration100Ns,
    FrameRateMilliFps,
    BitRateBps,
    SampleRateHz,
    IntegerCount,
    FileTimeDate,
    FocalLengthMm,
    IsoSpeed,
    RatingPercent,
};

struct BuiltinPropertyStoreSpec {
    const char* propId;
    const wchar_t* canonicalName;
    PropStoreFormatKind formatKind;
};

const BuiltinPropertyStoreSpec* findBuiltinPropStoreSpec(const std::string& propId) {
    static const std::unordered_map<std::string, BuiltinPropertyStoreSpec> kSpecs = {
        {"duration",        {"duration",        L"System.Media.Duration",           PropStoreFormatKind::Duration100Ns}},
        {"frameRate",       {"frameRate",       L"System.Video.FrameRate",          PropStoreFormatKind::FrameRateMilliFps}},
        {"audioBitRate",    {"audioBitRate",    L"System.Audio.EncodingBitrate",    PropStoreFormatKind::BitRateBps}},
        {"audioSampleRate", {"audioSampleRate", L"System.Audio.SampleRate",         PropStoreFormatKind::SampleRateHz}},
        {"audioChannels",   {"audioChannels",   L"System.Audio.ChannelCount",       PropStoreFormatKind::IntegerCount}},
        {"artist",          {"artist",          L"System.Music.Artist",             PropStoreFormatKind::DisplayString}},
        {"album",           {"album",           L"System.Music.AlbumTitle",         PropStoreFormatKind::DisplayString}},
        {"albumArtist",     {"albumArtist",     L"System.Music.AlbumArtist",        PropStoreFormatKind::DisplayString}},
        {"genre",           {"genre",           L"System.Music.Genre",              PropStoreFormatKind::DisplayString}},
        {"title",           {"title",           L"System.Title",                    PropStoreFormatKind::DisplayString}},
        {"authors",         {"authors",         L"System.Author",                   PropStoreFormatKind::DisplayString}},
        {"subject",         {"subject",         L"System.Subject",                  PropStoreFormatKind::DisplayString}},
        {"pageCount",       {"pageCount",       L"System.Document.PageCount",       PropStoreFormatKind::IntegerCount}},
        {"wordCount",       {"wordCount",       L"System.Document.WordCount",       PropStoreFormatKind::IntegerCount}},
        {"comment",         {"comment",         L"System.Comment",                  PropStoreFormatKind::DisplayString}},
        {"dateTaken",       {"dateTaken",       L"System.Photo.DateTaken",          PropStoreFormatKind::FileTimeDate}},
        {"cameraMaker",     {"cameraMaker",     L"System.Photo.CameraManufacturer", PropStoreFormatKind::DisplayString}},
        {"cameraModel",     {"cameraModel",     L"System.Photo.CameraModel",        PropStoreFormatKind::DisplayString}},
        {"focalLength",     {"focalLength",     L"System.Photo.FocalLength",        PropStoreFormatKind::FocalLengthMm}},
        {"isoSpeed",        {"isoSpeed",        L"System.Photo.ISOSpeed",           PropStoreFormatKind::IsoSpeed}},
        {"rating",          {"rating",          L"System.Rating",                   PropStoreFormatKind::RatingPercent}},
    };
    auto it = kSpecs.find(propId);
    return it != kSpecs.end() ? &it->second : nullptr;
}

uint32_t classifyPropertyBucket(const std::string& propId) {
    if (propId == "type") return kBucketShellType;
    if (propId == "accessed" || propId == "attributes") return kBucketStat;
    if (propId == "dimensions" || propId == "width" || propId == "height" ||
        propId == "aspectRatio" || propId == "bitDepth") {
        return kBucketWicImage;
    }
    if (propId == "fileVersion" || propId == "productVersion" || propId == "productName" ||
        propId == "company" || propId == "fileDescription" || propId == "copyright") {
        return kBucketVersion;
    }
    if (propId == "childCount") return kBucketFolderStats;
    if (propId == "crc32" || propId == "md5" || propId == "sha256") return kBucketHash;
    if (propId == "owner") return kBucketOwner;
    if (propId == "ext" || propId == "stem" || propId == "parentName" ||
        propId == "drive" || propId == "fullPathLength" || propId == "nameLength") {
        return kBucketPureString;
    }
    if (propId == "videoDimensions" || findBuiltinPropStoreSpec(propId) != nullptr ||
        propId.rfind("System.", 0) == 0) {
        return kBucketPropertyStore;
    }
    return kBucketNone;
}

uint64_t fileTimeToUint64(const FILETIME& ft) noexcept {
    ULARGE_INTEGER raw{};
    raw.LowPart = ft.dwLowDateTime;
    raw.HighPart = ft.dwHighDateTime;
    return raw.QuadPart;
}

std::string formatFileTimeReadable(uint64_t ft64) {
    if (ft64 == 0) return {};
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(ft64 & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(ft64 >> 32);
    FILETIME localFt{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ft, &localFt) || !FileTimeToSystemTime(&localFt, &st)) {
        return {};
    }
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return std::string(buf);
}

std::string formatDurationSeconds(double totalSeconds) {
    if (totalSeconds <= 0.0 || !std::isfinite(totalSeconds)) return {};
    const uint64_t sec = static_cast<uint64_t>(std::llround(totalSeconds));
    const uint64_t h = sec / 3600ULL;
    const uint64_t m = (sec % 3600ULL) / 60ULL;
    const uint64_t s = sec % 60ULL;
    char buf[32]{};
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
                      static_cast<unsigned long long>(h),
                      static_cast<unsigned long long>(m),
                      static_cast<unsigned long long>(s));
    } else {
        std::snprintf(buf, sizeof(buf), "%02llu:%02llu",
                      static_cast<unsigned long long>(m),
                      static_cast<unsigned long long>(s));
    }
    return std::string(buf);
}

std::string formatAspectRatio(UINT width, UINT height) {
    if (width == 0 || height == 0) return {};
    const UINT g = std::gcd(width, height);
    const UINT rw = width / g;
    const UINT rh = height / g;
    if (rw <= 64 && rh <= 64) {
        return std::to_string(rw) + ":" + std::to_string(rh);
    }
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%.2f:1", static_cast<double>(width) / static_cast<double>(height));
    return std::string(buf);
}

uint32_t resolveWicBitsPerPixel(const GUID& pixelFormat) {
    if (pixelFormat == GUID_WICPixelFormat32bppBGRA || pixelFormat == GUID_WICPixelFormat32bppRGBA ||
        pixelFormat == GUID_WICPixelFormat32bppBGR || pixelFormat == GUID_WICPixelFormat32bppPBGRA ||
        pixelFormat == GUID_WICPixelFormat32bppPRGBA || pixelFormat == GUID_WICPixelFormat32bppCMYK) {
        return 32;
    }
    if (pixelFormat == GUID_WICPixelFormat24bppBGR || pixelFormat == GUID_WICPixelFormat24bppRGB) {
        return 24;
    }
    if (pixelFormat == GUID_WICPixelFormat48bppRGB || pixelFormat == GUID_WICPixelFormat48bppBGR) {
        return 48;
    }
    if (pixelFormat == GUID_WICPixelFormat64bppRGBA || pixelFormat == GUID_WICPixelFormat64bppBGRA) {
        return 64;
    }
    if (pixelFormat == GUID_WICPixelFormat16bppBGR555 || pixelFormat == GUID_WICPixelFormat16bppBGR565 ||
        pixelFormat == GUID_WICPixelFormat16bppGray) {
        return 16;
    }
    if (pixelFormat == GUID_WICPixelFormat8bppIndexed || pixelFormat == GUID_WICPixelFormat8bppGray) {
        return 8;
    }
    if (pixelFormat == GUID_WICPixelFormat4bppIndexed || pixelFormat == GUID_WICPixelFormat4bppGray) {
        return 4;
    }
    if (pixelFormat == GUID_WICPixelFormat1bppIndexed || pixelFormat == GUID_WICPixelFormatBlackWhite) {
        return 1;
    }
    return 0;
}

void extractWicImageProperties(
    ComPtr<IWICImagingFactory>& sharedFactory,
    const std::wstring& fullPath,
    const std::wstring& extLower,
    const std::vector<std::string>& missingProps,
    std::unordered_map<std::string, ExtractedPropertyValue>& out
) {
    if (!isWicCandidateExtension(extLower)) {
        return;
    }
    if (!sharedFactory) {
        const HRESULT hrCreate = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&sharedFactory)
        );
        if (FAILED(hrCreate) || !sharedFactory) return;
    }
    IWICImagingFactory* factory = sharedFactory.Get();

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(
        fullPath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder
    );
    if (FAILED(hr) || !decoder) return;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) return;

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
        return;
    }

    uint32_t bpp = 0;
    WICPixelFormatGUID pf{};
    if (SUCCEEDED(frame->GetPixelFormat(&pf))) {
        bpp = resolveWicBitsPerPixel(pf);
        if (bpp == 0) {
            ComPtr<IWICComponentInfo> compInfo;
            if (SUCCEEDED(factory->CreateComponentInfo(pf, &compInfo)) && compInfo) {
                ComPtr<IWICPixelFormatInfo> pfInfo;
                if (SUCCEEDED(compInfo.As(&pfInfo)) && pfInfo) {
                    UINT bits = 0;
                    if (SUCCEEDED(pfInfo->GetBitsPerPixel(&bits))) {
                        bpp = bits;
                    }
                }
            }
        }
    }

    for (const auto& prop : missingProps) {
        if (prop == "dimensions") {
            out[prop] = ExtractedPropertyValue{
                std::to_string(width) + " x " + std::to_string(height),
                static_cast<double>(width) * static_cast<double>(height)
            };
        } else if (prop == "width") {
            out[prop] = ExtractedPropertyValue{
                std::to_string(width) + " px",
                static_cast<double>(width)
            };
        } else if (prop == "height") {
            out[prop] = ExtractedPropertyValue{
                std::to_string(height) + " px",
                static_cast<double>(height)
            };
        } else if (prop == "aspectRatio") {
            out[prop] = ExtractedPropertyValue{
                formatAspectRatio(width, height),
                static_cast<double>(width) / static_cast<double>(height)
            };
        } else if (prop == "bitDepth" && bpp > 0) {
            out[prop] = ExtractedPropertyValue{
                std::to_string(bpp),
                static_cast<double>(bpp)
            };
        }
    }
}

void extractPeVersionProperties(
    const std::wstring& fullPath,
    const std::wstring& extLower,
    const std::vector<std::string>& missingProps,
    std::unordered_map<std::string, ExtractedPropertyValue>& out
) {
    if (!isVersionInfoCandidateExtension(extLower)) {
        return;
    }
    DWORD dummyHandle = 0;
    const DWORD infoSize = GetFileVersionInfoSizeW(fullPath.c_str(), &dummyHandle);
    if (infoSize == 0 || infoSize > 4 * 1024 * 1024) {
        return;
    }
    std::vector<BYTE> buffer(infoSize);
    if (!GetFileVersionInfoW(fullPath.c_str(), 0, infoSize, buffer.data())) {
        return;
    }

    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedLen = 0;
    std::string fixedFileVer;
    std::string fixedProdVer;
    if (VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&fixedInfo), &fixedLen) &&
        fixedInfo != nullptr && fixedLen >= sizeof(VS_FIXEDFILEINFO)) {
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                      HIWORD(fixedInfo->dwFileVersionMS), LOWORD(fixedInfo->dwFileVersionMS),
                      HIWORD(fixedInfo->dwFileVersionLS), LOWORD(fixedInfo->dwFileVersionLS));
        fixedFileVer = buf;
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                      HIWORD(fixedInfo->dwProductVersionMS), LOWORD(fixedInfo->dwProductVersionMS),
                      HIWORD(fixedInfo->dwProductVersionLS), LOWORD(fixedInfo->dwProductVersionLS));
        fixedProdVer = buf;
    }

    struct LangCodePage {
        WORD language;
        WORD codePage;
    };
    LangCodePage* translations = nullptr;
    UINT transBytes = 0;
    std::vector<std::wstring> subBlockPrefixes;
    if (VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<LPVOID*>(&translations), &transBytes) &&
        translations != nullptr && transBytes >= sizeof(LangCodePage)) {
        const size_t count = (std::min<size_t>)(transBytes / sizeof(LangCodePage), 4);
        for (size_t i = 0; i < count; ++i) {
            wchar_t prefix[64]{};
            std::swprintf(prefix, 64, L"\\StringFileInfo\\%04x%04x\\",
                          translations[i].language, translations[i].codePage);
            subBlockPrefixes.emplace_back(prefix);
        }
    }
    subBlockPrefixes.emplace_back(L"\\StringFileInfo\\040904b0\\");
    subBlockPrefixes.emplace_back(L"\\StringFileInfo\\080404b0\\");

    auto queryStringValue = [&](const wchar_t* fieldName) -> std::string {
        for (const auto& prefix : subBlockPrefixes) {
            const std::wstring query = prefix + fieldName;
            wchar_t* strPtr = nullptr;
            UINT strLen = 0;
            if (VerQueryValueW(buffer.data(), query.c_str(),
                               reinterpret_cast<LPVOID*>(&strPtr), &strLen) &&
                strPtr != nullptr && strLen > 0) {
                std::wstring val(strPtr);
                while (!val.empty() && std::iswspace(val.back())) val.pop_back();
                if (!val.empty()) {
                    return tools3000::core::WinUtils::wstringToUtf8(val);
                }
            }
        }
        return {};
    };

    for (const auto& prop : missingProps) {
        if (prop == "fileVersion") {
            std::string val = queryStringValue(L"FileVersion");
            if (val.empty()) val = fixedFileVer;
            if (!val.empty()) out[prop] = ExtractedPropertyValue{std::move(val), std::nullopt};
        } else if (prop == "productVersion") {
            std::string val = queryStringValue(L"ProductVersion");
            if (val.empty()) val = fixedProdVer;
            if (!val.empty()) out[prop] = ExtractedPropertyValue{std::move(val), std::nullopt};
        } else if (prop == "productName") {
            std::string val = queryStringValue(L"ProductName");
            if (!val.empty()) out[prop] = ExtractedPropertyValue{std::move(val), std::nullopt};
        } else if (prop == "company") {
            std::string val = queryStringValue(L"CompanyName");
            if (!val.empty()) out[prop] = ExtractedPropertyValue{std::move(val), std::nullopt};
        } else if (prop == "fileDescription") {
            std::string val = queryStringValue(L"FileDescription");
            if (!val.empty()) out[prop] = ExtractedPropertyValue{std::move(val), std::nullopt};
        } else if (prop == "copyright") {
            std::string val = queryStringValue(L"LegalCopyright");
            if (!val.empty()) out[prop] = ExtractedPropertyValue{std::move(val), std::nullopt};
        }
    }
}

std::optional<double> propVariantToNumber(const PROPVARIANT& pv) noexcept {
    switch (pv.vt) {
        case VT_UI1: return static_cast<double>(pv.bVal);
        case VT_I2:  return static_cast<double>(pv.iVal);
        case VT_UI2: return static_cast<double>(pv.uiVal);
        case VT_I4:  return static_cast<double>(pv.lVal);
        case VT_UI4: return static_cast<double>(pv.ulVal);
        case VT_INT: return static_cast<double>(pv.intVal);
        case VT_UINT: return static_cast<double>(pv.uintVal);
        case VT_I8:  return static_cast<double>(pv.hVal.QuadPart);
        case VT_UI8: return static_cast<double>(pv.uhVal.QuadPart);
        case VT_R4:  return static_cast<double>(pv.fltVal);
        case VT_R8:  return pv.dblVal;
        case VT_FILETIME: return static_cast<double>(fileTimeToUint64(pv.filetime));
        default: return std::nullopt;
    }
}

std::string formatPropVariantGeneric(const PROPERTYKEY& pkey, const PROPVARIANT& pv) {
    if (pv.vt == VT_EMPTY || pv.vt == VT_NULL) return {};
    PWSTR formatted = nullptr;
    if (SUCCEEDED(PSFormatForDisplayAlloc(pkey, pv, PDFF_DEFAULT, &formatted)) && formatted != nullptr) {
        std::wstring ws(formatted);
        CoTaskMemFree(formatted);
        // 清理 Windows Property System 可能注入的 LRM/RLM 双向控制符，保持等宽渲染干净
        std::wstring cleaned;
        cleaned.reserve(ws.size());
        for (wchar_t ch : ws) {
            if (ch == 0x200E || ch == 0x200F || ch == 0x202A || ch == 0x202B || ch == 0x202C) {
                continue;
            }
            cleaned.push_back(ch);
        }
        return tools3000::core::WinUtils::wstringToUtf8(cleaned);
    }
    return {};
}

bool readPropKeyFromStore(IPropertyStore* store, const PROPERTYKEY& pkey, PROPVARIANT& outPv) {
    PropVariantInit(&outPv);
    if (FAILED(store->GetValue(pkey, &outPv)) || outPv.vt == VT_EMPTY || outPv.vt == VT_NULL) {
        PropVariantClear(&outPv);
        return false;
    }
    return true;
}

void extractPropertyStoreProperties(
    const std::wstring& fullPath,
    const std::wstring& extLower,
    bool isDirectory,
    const std::vector<std::string>& missingProps,
    std::unordered_map<std::string, ExtractedPropertyValue>& out
) {
    bool hasCustomSystemProp = false;
    std::vector<std::string> targetProps;
    for (const auto& prop : missingProps) {
        if (classifyPropertyBucket(prop) == kBucketPropertyStore) {
            targetProps.push_back(prop);
            if (prop.rfind("System.", 0) == 0) {
                hasCustomSystemProp = true;
            }
        }
    }
    if (targetProps.empty()) return;
    if (isDirectory && !hasCustomSystemProp) return;

    // 优先使用 GPS_FASTPROPERTIESONLY（微秒级无慢句柄开销）；
    // 对明确的音视频/图片/Office 文档如果 fast 模式空，才降级到 GPS_DEFAULT。
    ComPtr<IPropertyStore> store;
    HRESULT hr = SHGetPropertyStoreFromParsingName(
        fullPath.c_str(),
        nullptr,
        GPS_FASTPROPERTIESONLY,
        IID_PPV_ARGS(&store)
    );
    const bool allowFallbackDefault = !isDirectory && (isRichPropertyStoreExtension(extLower) || hasCustomSystemProp);
    if ((FAILED(hr) || !store) && allowFallbackDefault) {
        hr = SHGetPropertyStoreFromParsingName(
            fullPath.c_str(),
            nullptr,
            GPS_DEFAULT,
            IID_PPV_ARGS(&store)
        );
    }
    if (FAILED(hr) || !store) return;

    bool extractedAny = false;
    auto tryExtractAllFromStore = [&](IPropertyStore* activeStore) {
        for (const auto& prop : targetProps) {
            if (out.find(prop) != out.end()) continue;

            if (prop == "videoDimensions") {
                PROPVARIANT pvW{};
                PROPVARIANT pvH{};
                const bool okW = readPropKeyFromStore(activeStore, PKEY_Video_FrameWidth, pvW);
                const bool okH = readPropKeyFromStore(activeStore, PKEY_Video_FrameHeight, pvH);
                if (okW && okH) {
                    const auto wNum = propVariantToNumber(pvW);
                    const auto hNum = propVariantToNumber(pvH);
                    if (wNum && hNum && *wNum > 0 && *hNum > 0) {
                        const uint64_t w = static_cast<uint64_t>(*wNum);
                        const uint64_t h = static_cast<uint64_t>(*hNum);
                        out[prop] = ExtractedPropertyValue{
                            std::to_string(w) + " x " + std::to_string(h),
                            static_cast<double>(w) * static_cast<double>(h)
                        };
                        extractedAny = true;
                    }
                }
                PropVariantClear(&pvW);
                PropVariantClear(&pvH);
                continue;
            }

            const BuiltinPropertyStoreSpec* spec = findBuiltinPropStoreSpec(prop);
            std::wstring canonicalName;
            PropStoreFormatKind fmtKind = PropStoreFormatKind::DisplayString;
            if (spec != nullptr) {
                canonicalName = spec->canonicalName;
                fmtKind = spec->formatKind;
            } else if (prop.rfind("System.", 0) == 0) {
                canonicalName = tools3000::core::WinUtils::utf8ToWstring(prop);
            } else {
                continue;
            }

            PROPERTYKEY pkey{};
            if (FAILED(PSGetPropertyKeyFromName(canonicalName.c_str(), &pkey))) {
                continue;
            }
            PROPVARIANT pv{};
            if (!readPropKeyFromStore(activeStore, pkey, pv)) {
                continue;
            }

            ExtractedPropertyValue val;
            switch (fmtKind) {
                case PropStoreFormatKind::Duration100Ns: {
                    if (const auto raw = propVariantToNumber(pv); raw && *raw > 0.0) {
                        const double seconds = *raw / 10000000.0;
                        val.display = formatDurationSeconds(seconds);
                        val.sortNumber = seconds;
                    }
                    break;
                }
                case PropStoreFormatKind::FrameRateMilliFps: {
                    if (const auto raw = propVariantToNumber(pv); raw && *raw > 0.0) {
                        const double fps = *raw / 1000.0;
                        char buf[32]{};
                        std::snprintf(buf, sizeof(buf), "%.2f fps", fps);
                        val.display = buf;
                        val.sortNumber = fps;
                    }
                    break;
                }
                case PropStoreFormatKind::BitRateBps: {
                    if (const auto raw = propVariantToNumber(pv); raw && *raw > 0.0) {
                        const double kbps = *raw / 1000.0;
                        char buf[32]{};
                        std::snprintf(buf, sizeof(buf), "%.0f kbps", kbps);
                        val.display = buf;
                        val.sortNumber = *raw;
                    }
                    break;
                }
                case PropStoreFormatKind::SampleRateHz: {
                    if (const auto raw = propVariantToNumber(pv); raw && *raw > 0.0) {
                        const double khz = *raw / 1000.0;
                        char buf[32]{};
                        std::snprintf(buf, sizeof(buf), "%.1f kHz", khz);
                        val.display = buf;
                        val.sortNumber = *raw;
                    }
                    break;
                }
                case PropStoreFormatKind::IntegerCount: {
                    if (const auto raw = propVariantToNumber(pv)) {
                        val.display = std::to_string(static_cast<int64_t>(std::llround(*raw)));
                        val.sortNumber = *raw;
                    }
                    break;
                }
                case PropStoreFormatKind::FileTimeDate: {
                    if (pv.vt == VT_FILETIME) {
                        const uint64_t ft64 = fileTimeToUint64(pv.filetime);
                        val.display = formatFileTimeReadable(ft64);
                        val.sortNumber = static_cast<double>(ft64);
                    } else {
                        val.display = formatPropVariantGeneric(pkey, pv);
                    }
                    break;
                }
                case PropStoreFormatKind::FocalLengthMm: {
                    if (const auto raw = propVariantToNumber(pv); raw && *raw > 0.0) {
                        char buf[32]{};
                        std::snprintf(buf, sizeof(buf), "%.1f mm", *raw);
                        val.display = buf;
                        val.sortNumber = *raw;
                    }
                    break;
                }
                case PropStoreFormatKind::IsoSpeed: {
                    if (const auto raw = propVariantToNumber(pv); raw && *raw > 0.0) {
                        val.display = "ISO " + std::to_string(static_cast<int64_t>(std::llround(*raw)));
                        val.sortNumber = *raw;
                    }
                    break;
                }
                case PropStoreFormatKind::RatingPercent: {
                    if (const auto raw = propVariantToNumber(pv); raw && *raw > 0.0) {
                        int stars = 0;
                        if (*raw >= 99.0) stars = 5;
                        else if (*raw >= 75.0) stars = 4;
                        else if (*raw >= 50.0) stars = 3;
                        else if (*raw >= 25.0) stars = 2;
                        else if (*raw >= 1.0) stars = 1;
                        if (stars > 0) {
                            val.display = std::to_string(stars) + " / 5";
                            val.sortNumber = static_cast<double>(stars);
                        }
                    }
                    break;
                }
                case PropStoreFormatKind::DisplayString:
                default: {
                    val.display = formatPropVariantGeneric(pkey, pv);
                    val.sortNumber = propVariantToNumber(pv);
                    break;
                }
            }
            PropVariantClear(&pv);

            if (!val.display.empty()) {
                out[prop] = std::move(val);
                extractedAny = true;
            }
        }
    };

    tryExtractAllFromStore(store.Get());

    // 若高速模式未命中且目标文件为支持内嵌元数据的富媒体/文档，回退到标准 PropertyStore 再读一次
    if (!extractedAny && allowFallbackDefault) {
        ComPtr<IPropertyStore> fallbackStore;
        if (SUCCEEDED(SHGetPropertyStoreFromParsingName(
                fullPath.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&fallbackStore))) &&
            fallbackStore) {
            tryExtractAllFromStore(fallbackStore.Get());
        }
    }
}

enum class ExtractStopReason : uint8_t {
    None = 0,
    BudgetExceeded,
    Cancelled,
};

struct ExtractExecutionBudget {
    std::chrono::steady_clock::time_point deadline;
    const std::atomic<uint64_t>* latestGen = nullptr;
    uint64_t requestGen = 0;

    ExtractStopReason check() const noexcept {
        if (requestGen > 0 && latestGen != nullptr &&
            latestGen->load(std::memory_order_acquire) > requestGen) {
            return ExtractStopReason::Cancelled;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return ExtractStopReason::BudgetExceeded;
        }
        return ExtractStopReason::None;
    }
};

ExtractStopReason extractFolderStats(
    const std::wstring& fullPath,
    bool isDirectory,
    const ExtractExecutionBudget& budget,
    std::unordered_map<std::string, ExtractedPropertyValue>& out
) {
    if (!isDirectory) return ExtractStopReason::None;
    std::wstring pattern = fullPath;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern.push_back(L'\\');
    }
    pattern.push_back(L'*');

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileExW(
        pattern.c_str(),
        FindExInfoBasic,
        &fd,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH
    );
    if (hFind == INVALID_HANDLE_VALUE) return ExtractStopReason::None;

    size_t count = 0;
    bool capped = false;
    ExtractStopReason stopReason = ExtractStopReason::None;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
            continue;
        }
        ++count;
        if ((count & 0xFFu) == 0u) {
            stopReason = budget.check();
            if (stopReason != ExtractStopReason::None) {
                break;
            }
        }
        if (count >= SearchPropertyExtractor::kMaxChildScanLimit) {
            capped = true;
            break;
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if (stopReason != ExtractStopReason::None) {
        return stopReason;
    }

    std::string text = std::to_string(count);
    if (capped) text.push_back('+');
    out["childCount"] = ExtractedPropertyValue{std::move(text), static_cast<double>(count)};
    return ExtractStopReason::None;
}

std::string bytesToLowerHex(const std::vector<BYTE>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (BYTE b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

ExtractStopReason extractHashes(
    const std::wstring& fullPath,
    bool isDirectory,
    uint64_t knownFileSize,
    const std::vector<std::string>& missingProps,
    const ExtractExecutionBudget& budget,
    std::unordered_map<std::string, ExtractedPropertyValue>& out
) {
    if (isDirectory) return ExtractStopReason::None;
    bool needCrc32 = false;
    bool needMd5 = false;
    bool needSha256 = false;
    for (const auto& p : missingProps) {
        if (p == "crc32") needCrc32 = true;
        else if (p == "md5") needMd5 = true;
        else if (p == "sha256") needSha256 = true;
    }
    if (!needCrc32 && !needMd5 && !needSha256) return ExtractStopReason::None;

    if (knownFileSize > SearchPropertyExtractor::kMaxHashFileBytes) {
        const std::string skipLabel = "> 64 MB";
        if (needCrc32) out["crc32"] = ExtractedPropertyValue{skipLabel, std::nullopt};
        if (needMd5) out["md5"] = ExtractedPropertyValue{skipLabel, std::nullopt};
        if (needSha256) out["sha256"] = ExtractedPropertyValue{skipLabel, std::nullopt};
        return ExtractStopReason::None;
    }

    HANDLE hFile = CreateFileW(
        fullPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) return ExtractStopReason::None;

    struct HandleGuard {
        HANDLE h;
        ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    } fileGuard{hFile};

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 0) return ExtractStopReason::None;
    if (static_cast<uint64_t>(fileSize.QuadPart) > SearchPropertyExtractor::kMaxHashFileBytes) {
        const std::string skipLabel = "> 64 MB";
        if (needCrc32) out["crc32"] = ExtractedPropertyValue{skipLabel, std::nullopt};
        if (needMd5) out["md5"] = ExtractedPropertyValue{skipLabel, std::nullopt};
        if (needSha256) out["sha256"] = ExtractedPropertyValue{skipLabel, std::nullopt};
        return ExtractStopReason::None;
    }

    struct CngHasher {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<BYTE> objBuf;
        DWORD hashLen = 0;

        bool init(LPCWSTR algId) {
            if (BCryptOpenAlgorithmProvider(&alg, algId, nullptr, 0) != 0) return false;
            DWORD objLen = 0;
            DWORD cbData = 0;
            if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
                                  sizeof(objLen), &cbData, 0) != 0) {
                return false;
            }
            if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen),
                                  sizeof(hashLen), &cbData, 0) != 0) {
                return false;
            }
            objBuf.resize(objLen);
            return BCryptCreateHash(alg, &hash, objBuf.data(), objLen, nullptr, 0, 0) == 0;
        }
        void update(const BYTE* data, DWORD len) {
            if (hash && len > 0) {
                BCryptHashData(hash, const_cast<PUCHAR>(data), len, 0);
            }
        }
        std::string finalizeHex() {
            if (!hash || hashLen == 0) return {};
            std::vector<BYTE> digest(hashLen);
            if (BCryptFinishHash(hash, digest.data(), hashLen, 0) != 0) return {};
            return bytesToLowerHex(digest);
        }
        ~CngHasher() {
            if (hash) BCryptDestroyHash(hash);
            if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        }
    };

    CngHasher md5Hasher;
    CngHasher sha256Hasher;
    const bool md5Ready = needMd5 && md5Hasher.init(BCRYPT_MD5_ALGORITHM);
    const bool sha256Ready = needSha256 && sha256Hasher.init(BCRYPT_SHA256_ALGORITHM);
    uLong crcVal = crc32(0L, Z_NULL, 0);

    std::array<BYTE, 65536> buf{};
    DWORD bytesRead = 0;
    size_t chunkCount = 0;
    while (ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr) && bytesRead > 0) {
        if (needCrc32) {
            crcVal = crc32(crcVal, buf.data(), bytesRead);
        }
        if (md5Ready) {
            md5Hasher.update(buf.data(), bytesRead);
        }
        if (sha256Ready) {
            sha256Hasher.update(buf.data(), bytesRead);
        }
        ++chunkCount;
        // 每处理 ~1MB 检查一次预算与取消代际，绝不长时间霸占 MessageBridge 工作线程
        if ((chunkCount & 0x0Fu) == 0u) {
            const auto stopReason = budget.check();
            if (stopReason != ExtractStopReason::None) {
                return stopReason;
            }
        }
    }

    if (needCrc32) {
        char hexBuf[16]{};
        std::snprintf(hexBuf, sizeof(hexBuf), "%08x", static_cast<unsigned int>(crcVal));
        out["crc32"] = ExtractedPropertyValue{hexBuf, static_cast<double>(crcVal)};
    }
    if (md5Ready) {
        out["md5"] = ExtractedPropertyValue{md5Hasher.finalizeHex(), std::nullopt};
    }
    if (sha256Ready) {
        out["sha256"] = ExtractedPropertyValue{sha256Hasher.finalizeHex(), std::nullopt};
    }
    return ExtractStopReason::None;
}

void extractFileOwner(
    const std::wstring& fullPath,
    std::unordered_map<std::string, ExtractedPropertyValue>& out
) {
    PSID ownerSid = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD res = GetNamedSecurityInfoW(
        fullPath.c_str(),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION,
        &ownerSid,
        nullptr,
        nullptr,
        nullptr,
        &sd
    );
    if (res != ERROR_SUCCESS || !sd) return;
    struct SdGuard {
        PSECURITY_DESCRIPTOR p;
        ~SdGuard() { if (p) LocalFree(p); }
    } sdGuard{sd};

    if (!ownerSid || !IsValidSid(ownerSid)) return;

    // 按二进制 SID 键做进程级缓存，避免同一用户下 500 个文件重复查 LSA
    const DWORD sidLen = GetLengthSid(ownerSid);
    std::wstring sidKey(reinterpret_cast<const wchar_t*>(ownerSid), sidLen / sizeof(wchar_t));
    {
        std::lock_guard<std::mutex> lock(sharedSidCacheMutex());
        auto it = sharedSidNameCache().find(sidKey);
        if (it != sharedSidNameCache().end()) {
            if (!it->second.empty()) {
                out["owner"] = ExtractedPropertyValue{it->second, std::nullopt};
            }
            return;
        }
    }

    wchar_t nameBuf[256]{};
    wchar_t domainBuf[256]{};
    DWORD nameLen = 256;
    DWORD domainLen = 256;
    SID_NAME_USE use = SidTypeUnknown;
    std::string resolved;
    if (LookupAccountSidW(nullptr, ownerSid, nameBuf, &nameLen, domainBuf, &domainLen, &use)) {
        std::wstring combined;
        if (domainLen > 0 && domainBuf[0] != L'\0') {
            combined.append(domainBuf);
            combined.push_back(L'\\');
        }
        combined.append(nameBuf);
        resolved = tools3000::core::WinUtils::wstringToUtf8(combined);
    }

    if (!resolved.empty()) {
        std::lock_guard<std::mutex> lock(sharedSidCacheMutex());
        if (sharedSidNameCache().size() >= 256) {
            sharedSidNameCache().clear();
        }
        sharedSidNameCache().emplace(std::move(sidKey), resolved);
        out["owner"] = ExtractedPropertyValue{std::move(resolved), std::nullopt};
    }
}

void extractPureStringProperties(
    const std::wstring& fullPath,
    bool isDirectory,
    const std::vector<std::string>& missingProps,
    std::unordered_map<std::string, ExtractedPropertyValue>& out
) {
    const std::wstring fileName = extractFileNameFromPath(fullPath);
    for (const auto& prop : missingProps) {
        if (prop == "ext") {
            if (!isDirectory) {
                const std::wstring extWithDot = extractExtensionLower(fullPath, false);
                if (extWithDot.size() > 1) {
                    out[prop] = ExtractedPropertyValue{
                        tools3000::core::WinUtils::wstringToUtf8(extWithDot.substr(1)),
                        std::nullopt
                    };
                }
            }
        } else if (prop == "stem") {
            if (isDirectory) {
                out[prop] = ExtractedPropertyValue{tools3000::core::WinUtils::wstringToUtf8(fileName), std::nullopt};
            } else {
                const size_t dot = fileName.rfind(L'.');
                const std::wstring stem = (dot != std::wstring::npos && dot > 0) ? fileName.substr(0, dot) : fileName;
                out[prop] = ExtractedPropertyValue{tools3000::core::WinUtils::wstringToUtf8(stem), std::nullopt};
            }
        } else if (prop == "parentName") {
            const std::wstring parent = extractParentNameFromPath(fullPath);
            if (!parent.empty()) {
                out[prop] = ExtractedPropertyValue{tools3000::core::WinUtils::wstringToUtf8(parent), std::nullopt};
            }
        } else if (prop == "drive") {
            if (fullPath.size() >= 2 && fullPath[1] == L':') {
                std::wstring drv = {static_cast<wchar_t>(std::towupper(fullPath[0])), L':'};
                out[prop] = ExtractedPropertyValue{tools3000::core::WinUtils::wstringToUtf8(drv), std::nullopt};
            }
        } else if (prop == "fullPathLength") {
            const size_t len = fullPath.size();
            out[prop] = ExtractedPropertyValue{std::to_string(len), static_cast<double>(len)};
        } else if (prop == "nameLength") {
            const size_t len = fileName.size();
            out[prop] = ExtractedPropertyValue{std::to_string(len), static_cast<double>(len)};
        }
    }
}

} // namespace

SearchPropertyExtractor& SearchPropertyExtractor::instance() {
    static SearchPropertyExtractor inst;
    return inst;
}

std::string SearchPropertyExtractor::formatFileAttributes(uint32_t attributes) {
    if (attributes == 0) return {};
    std::string out;
    out.reserve(10);
    if (attributes & FILE_ATTRIBUTE_READONLY)      out.push_back('R');
    if (attributes & FILE_ATTRIBUTE_HIDDEN)        out.push_back('H');
    if (attributes & FILE_ATTRIBUTE_SYSTEM)        out.push_back('S');
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)     out.push_back('D');
    if (attributes & FILE_ATTRIBUTE_ARCHIVE)       out.push_back('A');
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) out.push_back('L');
    if (attributes & FILE_ATTRIBUTE_COMPRESSED)    out.push_back('C');
    if (attributes & FILE_ATTRIBUTE_ENCRYPTED)     out.push_back('E');
    if (attributes & FILE_ATTRIBUTE_OFFLINE)       out.push_back('O');
    if (attributes & FILE_ATTRIBUTE_TEMPORARY)     out.push_back('T');
    return out;
}

std::string SearchPropertyExtractor::getShellTypeName(const std::wstring& extensionLower, bool isDirectory) {
    const std::wstring cacheKey = isDirectory ? L"::dir::" : (extensionLower.empty() ? L"::noext::" : extensionLower);
    {
        std::lock_guard<std::mutex> lock(sharedShellTypeMutex());
        auto& cache = sharedShellTypeMap();
        auto it = cache.find(cacheKey);
        if (it != cache.end()) {
            return it->second;
        }
    }

    SHFILEINFOW sfi{};
    const DWORD attrs = isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    const std::wstring pseudoName = isDirectory ? L"folder" : (extensionLower.empty() ? L"file" : (L"file" + extensionLower));
    std::string typeUtf8;
    if (SHGetFileInfoW(pseudoName.c_str(), attrs, &sfi, sizeof(sfi),
                       SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) != 0 &&
        sfi.szTypeName[0] != L'\0') {
        typeUtf8 = tools3000::core::WinUtils::wstringToUtf8(sfi.szTypeName);
    }

    {
        std::lock_guard<std::mutex> lock(sharedShellTypeMutex());
        auto& cache = sharedShellTypeMap();
        if (cache.size() >= kShellTypeCacheCapacity) {
            cache.clear();
        }
        cache.emplace(cacheKey, typeUtf8);
    }
    return typeUtf8;
}

bool SearchPropertyExtractor::isCloudPlaceholderAttributes(uint32_t attributes) noexcept {
    constexpr uint32_t kRecallOnOpen = 0x00040000U;        // FILE_ATTRIBUTE_RECALL_ON_OPEN
    constexpr uint32_t kRecallOnDataAccess = 0x00400000U;  // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
    constexpr uint32_t kCloudMask = FILE_ATTRIBUTE_OFFLINE | kRecallOnOpen | kRecallOnDataAccess;
    return (attributes & kCloudMask) != 0U;
}

void SearchPropertyExtractor::advanceRequestGeneration(uint64_t latestGen) noexcept {
    if (latestGen == 0) return;
    uint64_t current = m_latestRequestGen.load(std::memory_order_acquire);
    while (latestGen > current &&
           !m_latestRequestGen.compare_exchange_weak(
               current, latestGen, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
}

void SearchPropertyExtractor::clearCache() {
    sharedPropertyCache().clear();
    {
        std::lock_guard<std::mutex> lock(sharedShellTypeMutex());
        sharedShellTypeMap().clear();
    }
    {
        std::lock_guard<std::mutex> lock(sharedSidCacheMutex());
        sharedSidNameCache().clear();
    }
}

BatchPropertyExtractResult SearchPropertyExtractor::extractBatch(
    const std::vector<PropertyExtractItem>& items,
    const std::vector<std::string>& propertyIds,
    uint64_t requestGen
) {
    BatchPropertyExtractResult result;
    if (items.empty() || propertyIds.empty()) {
        return result;
    }

    if (requestGen > 0) {
        if (m_latestRequestGen.load(std::memory_order_acquire) > requestGen) {
            result.cancelled = true;
            return result;
        }
    }

    // 去重并限流属性列表
    std::vector<std::string> cleanProps;
    cleanProps.reserve((std::min)(propertyIds.size(), kMaxBatchProperties));
    std::unordered_set<std::string> seenProps;
    for (const auto& p : propertyIds) {
        if (cleanProps.size() >= kMaxBatchProperties) break;
        if (p.empty() || p.size() > 96) continue;
        if (seenProps.insert(p).second) {
            cleanProps.push_back(p);
        }
    }
    if (cleanProps.empty()) return result;

    ScopedComInit comGuard;
    ComPtr<IWICImagingFactory> batchWicFactory;
    const auto startTick = std::chrono::steady_clock::now();
    const ExtractExecutionBudget budget{
        startTick + std::chrono::milliseconds(kBatchBudgetMs),
        &m_latestRequestGen,
        requestGen
    };
    const size_t itemLimit = (std::min)(items.size(), kMaxBatchItems);

    constexpr uint32_t kContentReadBuckets =
        kBucketWicImage | kBucketVersion | kBucketPropertyStore | kBucketHash;

    for (size_t i = 0; i < itemLimit; ++i) {
        const auto& item = items[i];
        if (item.path.empty() || item.path.size() > 32768) continue;

        if (i > 0) {
            const auto stopReason = budget.check();
            if (stopReason == ExtractStopReason::Cancelled) {
                result.cancelled = true;
                break;
            }
            if (stopReason == ExtractStopReason::BudgetExceeded) {
                result.budgetExceeded = true;
                break;
            }
        }

        const std::wstring widePath = tools3000::core::WinUtils::utf8ToWstring(item.path);
        if (widePath.empty()) continue;

        const bool virtualPath = isSpecialVirtualPath(widePath);
        const std::wstring extLower = extractExtensionLower(widePath, item.isDirectory);

        // 先尝试按 (normalizedPath, fileSize, lastWriteTime) 查进程级 LRU 缓存
        uint64_t cacheSize = item.fileSize;
        uint64_t cacheMtime = item.lastWriteTime;
        bool statFetched = false;
        WIN32_FILE_ATTRIBUTE_DATA statData{};
        if (!virtualPath && cacheMtime == 0 && cacheSize == 0) {
            if (GetFileAttributesExW(widePath.c_str(), GetFileExInfoStandard, &statData)) {
                statFetched = true;
                cacheMtime = fileTimeToUint64(statData.ftLastWriteTime);
                cacheSize = (statData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    ? 0ULL
                    : ((static_cast<uint64_t>(statData.nFileSizeHigh) << 32) | statData.nFileSizeLow);
            }
        }

        const std::string cacheKey = normalizePathCacheKey(item.path);
        std::unordered_map<std::string, ExtractedPropertyValue> rowValues;
        std::vector<std::string> missingProps;
        const bool allHit = sharedPropertyCache().lookup(
            cacheKey, cacheSize, cacheMtime, cleanProps, rowValues, missingProps);

        if (allHit) {
            ++result.cacheHits;
            result.values.emplace(item.path, std::move(rowValues));
            continue;
        }

        ++result.diskExtractions;
        uint32_t activeBuckets = kBucketNone;
        for (const auto& mp : missingProps) {
            activeBuckets |= classifyPropertyBucket(mp);
        }

        std::unordered_map<std::string, ExtractedPropertyValue> freshlyExtracted;

        if (activeBuckets & kBucketShellType) {
            const std::string typeName = getShellTypeName(extLower, item.isDirectory);
            freshlyExtracted["type"] = ExtractedPropertyValue{typeName, std::nullopt};
        }

        if (activeBuckets & kBucketPureString) {
            extractPureStringProperties(widePath, item.isDirectory, missingProps, freshlyExtracted);
        }

        ExtractStopReason innerStop = ExtractStopReason::None;
        if (!virtualPath) {
            const bool needCloudCheck = !item.isDirectory && ((activeBuckets & kContentReadBuckets) != 0U);
            if (((activeBuckets & kBucketStat) != 0U || needCloudCheck) && !statFetched) {
                statFetched = GetFileAttributesExW(widePath.c_str(), GetFileExInfoStandard, &statData) != FALSE;
            }
            const bool isCloudPlaceholder =
                statFetched && !item.isDirectory && isCloudPlaceholderAttributes(statData.dwFileAttributes);

            if ((activeBuckets & kBucketStat) && statFetched) {
                for (const auto& mp : missingProps) {
                    if (mp == "accessed") {
                        const uint64_t at64 = fileTimeToUint64(statData.ftLastAccessTime);
                        freshlyExtracted[mp] = ExtractedPropertyValue{
                            formatFileTimeReadable(at64),
                            static_cast<double>(at64)
                        };
                    } else if (mp == "attributes") {
                        freshlyExtracted[mp] = ExtractedPropertyValue{
                            formatFileAttributes(statData.dwFileAttributes),
                            static_cast<double>(statData.dwFileAttributes)
                        };
                    }
                }
            }

            if (isCloudPlaceholder) {
                // OneDrive / 云盘按需脱机占位文件：严禁触发文件内容读盘（否则拉起漫长云下载阻塞线程池）
                if (activeBuckets & kBucketHash) {
                    for (const auto& mp : missingProps) {
                        if (mp == "crc32" || mp == "md5" || mp == "sha256") {
                            freshlyExtracted[mp] = ExtractedPropertyValue{"Cloud", std::nullopt};
                        }
                    }
                }
            } else {
                if ((activeBuckets & kBucketWicImage) && !item.isDirectory) {
                    extractWicImageProperties(batchWicFactory, widePath, extLower, missingProps, freshlyExtracted);
                }

                if ((activeBuckets & kBucketVersion) && !item.isDirectory) {
                    extractPeVersionProperties(widePath, extLower, missingProps, freshlyExtracted);
                }

                if (activeBuckets & kBucketPropertyStore) {
                    extractPropertyStoreProperties(widePath, extLower, item.isDirectory, missingProps, freshlyExtracted);
                }

                if ((activeBuckets & kBucketHash) && !item.isDirectory) {
                    innerStop = extractHashes(widePath, false, cacheSize, missingProps, budget, freshlyExtracted);
                }
            }

            if (innerStop == ExtractStopReason::None && (activeBuckets & kBucketFolderStats) && item.isDirectory) {
                innerStop = extractFolderStats(widePath, true, budget, freshlyExtracted);
            }

            if (innerStop == ExtractStopReason::None && (activeBuckets & kBucketOwner)) {
                extractFileOwner(widePath, freshlyExtracted);
            }
        }

        if (innerStop != ExtractStopReason::None) {
            if (innerStop == ExtractStopReason::Cancelled) {
                result.cancelled = true;
            } else {
                result.budgetExceeded = true;
            }
            // 中途超时/取消时不向缓存写入不完整的空值占位，等待下批续提
            break;
        }

        // 对未能取到值的缺失属性写入空值缓存，防止滚动回退时对同一无元数据文件反复做无用 I/O
        for (const auto& mp : missingProps) {
            if (freshlyExtracted.find(mp) == freshlyExtracted.end()) {
                freshlyExtracted.emplace(mp, ExtractedPropertyValue{});
            }
        }

        sharedPropertyCache().upsert(cacheKey, cacheSize, cacheMtime, freshlyExtracted);

        for (auto& [k, v] : freshlyExtracted) {
            rowValues[k] = std::move(v);
        }
        result.values.emplace(item.path, std::move(rowValues));
    }

    return result;
}

namespace {

uint64_t parseJsonUint64Field(const nlohmann::json& obj, const char* key) noexcept {
    auto it = obj.find(key);
    if (it == obj.end()) return 0ULL;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) {
        const int64_t v = it->get<int64_t>();
        return v > 0 ? static_cast<uint64_t>(v) : 0ULL;
    }
    if (it->is_string()) {
        const auto& s = it->get_ref<const std::string&>();
        if (s.empty()) return 0ULL;
        uint64_t parsed = 0ULL;
        for (char c : s) {
            if (c < '0' || c > '9') return 0ULL;
            parsed = parsed * 10ULL + static_cast<uint64_t>(c - '0');
        }
        return parsed;
    }
    if (it->is_number_float()) {
        const double d = it->get<double>();
        return (d > 0.0 && std::isfinite(d)) ? static_cast<uint64_t>(d) : 0ULL;
    }
    return 0ULL;
}

} // namespace

nlohmann::json SearchPropertyExtractor::handleBatchRequestJson(const nlohmann::json& params) {
    if (!params.is_object()) {
        return {{"success", false}, {"error", "params must be an object"}};
    }

    const uint64_t requestGen = parseJsonUint64Field(params, "requestGen");
    if (requestGen > 0) {
        advanceRequestGeneration(requestGen);
    }

    std::vector<std::string> propertyIds;
    if (params.contains("properties") && params["properties"].is_array()) {
        for (const auto& p : params["properties"]) {
            if (p.is_string()) {
                propertyIds.push_back(p.get<std::string>());
            }
            if (propertyIds.size() >= kMaxBatchProperties) break;
        }
    }

    std::vector<PropertyExtractItem> items;
    if (params.contains("items") && params["items"].is_array()) {
        items.reserve((std::min)(params["items"].size(), kMaxBatchItems));
        for (const auto& rawItem : params["items"]) {
            if (!rawItem.is_object()) continue;
            PropertyExtractItem item;
            item.path = rawItem.value("path", "");
            if (item.path.empty()) continue;
            item.isDirectory = rawItem.value("isDirectory", false);
            item.fileSize = parseJsonUint64Field(rawItem, "size");
            item.lastWriteTime = parseJsonUint64Field(rawItem, "lastWriteTime");
            items.push_back(std::move(item));
            if (items.size() >= kMaxBatchItems) break;
        }
    }

    const auto outcome = extractBatch(items, propertyIds, requestGen);

    nlohmann::json valuesJson = nlohmann::json::object();
    for (const auto& [pathKey, propsMap] : outcome.values) {
        nlohmann::json rowJson = nlohmann::json::object();
        for (const auto& [propId, val] : propsMap) {
            if (val.sortNumber.has_value()) {
                rowJson[propId] = {
                    {"d", val.display},
                    {"n", *val.sortNumber}
                };
            } else {
                rowJson[propId] = {
                    {"d", val.display}
                };
            }
        }
        valuesJson[pathKey] = std::move(rowJson);
    }

    return {
        {"success", true},
        {"values", std::move(valuesJson)},
        {"cacheHits", outcome.cacheHits},
        {"diskExtractions", outcome.diskExtractions},
        {"budgetExceeded", outcome.budgetExceeded},
        {"cancelled", outcome.cancelled}
    };
}

} // namespace tools3000::search
