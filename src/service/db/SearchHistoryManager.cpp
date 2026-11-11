#include "SearchHistoryManager.h"
#include "RunHistoryManager.h"
#include "CsvUtils.h"
#include "../../common/AtomicFile.h"
#include <shlobj.h>
#include <objbase.h>
#include <sddl.h>
#include <filesystem>
#include <algorithm>
#include <cwctype>

namespace tools3000::service::db {

namespace {

class ScopedUserTokenContext {
public:
    ScopedUserTokenContext(HANDLE targetToken, bool requireImpersonation) noexcept
        : m_requireImpersonation(requireImpersonation) {
        if (!m_requireImpersonation) {
            m_ok = true;
            return;
        }
        if (!targetToken || targetToken == INVALID_HANDLE_VALUE) {
            m_ok = false;
            return;
        }
        HANDLE prevToken = nullptr;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE, TRUE, &prevToken)) {
            m_prevThreadToken = prevToken;
            m_hadPrevToken = true;
        } else {
            const DWORD err = GetLastError();
            if (err != ERROR_NO_TOKEN) {
                m_ok = false;
                return;
            }
        }
        if (!SetThreadToken(nullptr, targetToken)) {
            if (m_prevThreadToken) {
                CloseHandle(m_prevThreadToken);
                m_prevThreadToken = nullptr;
            }
            m_ok = false;
            return;
        }
        m_switched = true;
        m_ok = true;
    }

    ~ScopedUserTokenContext() {
        if (m_switched) {
            if (m_hadPrevToken) {
                if (!SetThreadToken(nullptr, m_prevThreadToken)) {
                    RevertToSelf();
                }
            } else {
                RevertToSelf();
            }
        }
        if (m_prevThreadToken) {
            CloseHandle(m_prevThreadToken);
            m_prevThreadToken = nullptr;
        }
    }

    ScopedUserTokenContext(const ScopedUserTokenContext&) = delete;
    ScopedUserTokenContext& operator=(const ScopedUserTokenContext&) = delete;

    [[nodiscard]] bool ok() const noexcept { return m_ok; }

private:
    HANDLE m_prevThreadToken = nullptr;
    bool m_hadPrevToken = false;
    bool m_requireImpersonation = false;
    bool m_switched = false;
    bool m_ok = false;
};

std::wstring resolveLocalAppDataUserServiceDir(HANDLE tokenOrNull) {
    PWSTR rawFolder = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(
        FOLDERID_LocalAppData,
        KF_FLAG_DEFAULT,
        tokenOrNull,
        &rawFolder
    );
    std::wstring basePath;
    if (SUCCEEDED(hr) && rawFolder && rawFolder[0] != L'\0') {
        basePath.assign(rawFolder);
    }
    if (rawFolder) {
        CoTaskMemFree(rawFolder);
    }
    if (basePath.empty()) {
        return {};
    }
    while (!basePath.empty() && (basePath.back() == L'\\' || basePath.back() == L'/')) {
        basePath.pop_back();
    }
    return basePath + L"\\Tools3000\\service_user_data";
}

std::wstring getDefaultAppDataFolder() {
    const std::wstring localDir = resolveLocalAppDataUserServiceDir(nullptr);
    if (!localDir.empty()) {
        tools3000::common::ensureDirectorySafeNoReparse(localDir);
        return localDir;
    }
    wchar_t appData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData))) {
        std::wstring dir = std::wstring(appData) + L"\\Tools3000\\service_user_data";
        tools3000::common::ensureDirectorySafeNoReparse(dir);
        return dir;
    }
    return L".";
}

std::wstring trim(const std::wstring& str) {
    size_t first = str.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    size_t last = str.find_last_not_of(L" \t\r\n");
    return str.substr(first, (last - first + 1));
}

} // namespace

SearchHistoryManager& SearchHistoryManager::instance() {
    static SearchHistoryManager s_mgr;
    return s_mgr;
}

SearchHistoryManager::SearchHistoryManager() = default;

SearchHistoryManager::~SearchHistoryManager() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_isDirty) {
        save();
    }
    closeImpersonationTokenUnlocked();
}

void SearchHistoryManager::closeImpersonationTokenUnlocked() noexcept {
    if (m_impersonationToken && m_impersonationToken != INVALID_HANDLE_VALUE) {
        CloseHandle(m_impersonationToken);
    }
    m_impersonationToken = nullptr;
    m_requiresImpersonation = false;
}

std::wstring SearchHistoryManager::getNormalizedKey(const std::wstring& search) const {
    std::wstring key = trim(search);
    if (!key.empty()) {
        CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    }
    return key;
}

void SearchHistoryManager::init(const std::wstring& customCsvPath) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_isDirty && !m_csvPath.empty()) {
        save();
    }
    closeImpersonationTokenUnlocked();
    m_boundClientSid.clear();
    m_records.clear();
    m_isDirty = false;

    if (!customCsvPath.empty()) {
        m_csvPath = customCsvPath;
    } else if (m_csvPath.empty()) {
        m_csvPath = getDefaultAppDataFolder() + L"\\Search History.csv";
    }
    load();
}

void SearchHistoryManager::initForSid(const std::wstring& sid) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_isDirty && !m_csvPath.empty()) {
        save();
    }
    closeImpersonationTokenUnlocked();
    m_boundClientSid = sid;
    m_records.clear();
    m_isDirty = false;
    m_csvPath.clear();
}

bool SearchHistoryManager::bindClientUserContext(
    HANDLE clientToken,
    bool isSameUserProcessSid,
    const std::wstring& clientSid
) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    const bool hasKernelToken = (clientToken != nullptr && clientToken != INVALID_HANDLE_VALUE);
    if (!hasKernelToken && !isSameUserProcessSid) {
        return false;
    }

    HANDLE duplicatedToken = nullptr;
    if (hasKernelToken) {
        if (!DuplicateTokenEx(
                clientToken,
                TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE,
                nullptr,
                SecurityImpersonation,
                TokenImpersonation,
                &duplicatedToken)) {
            return false;
        }
    }

    const std::wstring targetUserDir = resolveLocalAppDataUserServiceDir(hasKernelToken ? duplicatedToken : nullptr);
    if (targetUserDir.empty()) {
        if (duplicatedToken) CloseHandle(duplicatedToken);
        return false;
    }
    const std::wstring targetCsvPath = targetUserDir + L"\\Search History.csv";

    if (!clientSid.empty() && m_boundClientSid == clientSid && m_csvPath == targetCsvPath) {
        closeImpersonationTokenUnlocked();
        m_impersonationToken = duplicatedToken;
        m_requiresImpersonation = hasKernelToken;
        return true;
    }
    if (clientSid.empty() && m_csvPath == targetCsvPath && m_requiresImpersonation == hasKernelToken) {
        closeImpersonationTokenUnlocked();
        m_impersonationToken = duplicatedToken;
        m_requiresImpersonation = hasKernelToken;
        return true;
    }

    if (m_isDirty && !m_csvPath.empty()) {
        save();
    }

    closeImpersonationTokenUnlocked();
    m_impersonationToken = duplicatedToken;
    m_requiresImpersonation = hasKernelToken;
    m_boundClientSid = clientSid;
    m_records.clear();
    m_isDirty = false;

    {
        ScopedUserTokenContext imp(m_impersonationToken, m_requiresImpersonation);
        if (!imp.ok()) {
            closeImpersonationTokenUnlocked();
            m_csvPath.clear();
            return false;
        }
        if (!tools3000::common::ensureDirectorySafeNoReparse(targetUserDir)) {
            closeImpersonationTokenUnlocked();
            m_csvPath.clear();
            return false;
        }
    }

    m_csvPath = targetCsvPath;
    load();
    return true;
}

void SearchHistoryManager::recordSearch(const std::wstring& search) {
    std::wstring trimmed = trim(search);
    if (trimmed.empty()) return;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::wstring key = getNormalizedKey(trimmed);
    uint64_t nowFt = RunHistoryManager::getCurrentFileTime();

    auto it = m_records.find(key);
    if (it != m_records.end()) {
        it->second.searchCount++;
        it->second.lastSearchDate = nowFt;
        it->second.search = trimmed;
    } else {
        m_records[key] = SearchHistoryItem{
            .search = trimmed,
            .searchCount = 1,
            .lastSearchDate = nowFt
        };
    }
    m_isDirty = true;
    save();
}

std::vector<SearchHistoryItem> SearchHistoryManager::getRecentSearches(size_t limit) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<SearchHistoryItem> result;
    result.reserve(m_records.size());
    for (const auto& [_, rec] : m_records) {
        result.push_back(rec);
    }

    std::sort(result.begin(), result.end(), [](const SearchHistoryItem& a, const SearchHistoryItem& b) {
        if (a.lastSearchDate != b.lastSearchDate) return a.lastSearchDate > b.lastSearchDate;
        return a.searchCount > b.searchCount;
    });

    if (result.size() > limit) {
        result.resize(limit);
    }
    return result;
}

bool SearchHistoryManager::removeSearch(const std::wstring& search) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::wstring key = getNormalizedKey(search);
    if (m_records.erase(key) > 0) {
        m_isDirty = true;
        save();
        return true;
    }
    return false;
}

bool SearchHistoryManager::load() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_csvPath.empty()) return false;

    ScopedUserTokenContext imp(m_impersonationToken, m_requiresImpersonation);
    if (!imp.ok()) {
        return false;
    }
    if (tools3000::common::pathContainsReparsePoint(m_csvPath)) {
        return false;
    }

    HANDLE hFile = CreateFileW(
        m_csvPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION fileInfo{};
    if (GetFileType(hFile) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(hFile, &fileInfo) ||
        (fileInfo.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        CloseHandle(hFile);
        return false;
    }

    LARGE_INTEGER size{};
    constexpr LONGLONG MaxHistoryFileBytes = 16LL * 1024LL * 1024LL;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 ||
        size.QuadPart > MaxHistoryFileBytes) {
        CloseHandle(hFile);
        return false;
    }

    std::string buffer(static_cast<size_t>(size.QuadPart), '\0');
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) ||
        bytesRead != buffer.size()) {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    // UTF-8 BOM 剥离
    size_t offset = 0;
    if (buffer.size() >= 3 && static_cast<uint8_t>(buffer[0]) == 0xEF &&
        static_cast<uint8_t>(buffer[1]) == 0xBB && static_cast<uint8_t>(buffer[2]) == 0xBF) {
        offset = 3;
    }

    std::vector<std::vector<std::string>> rows;
    if (!detail::parseCsvDocument(
            std::string_view(buffer.data() + offset, buffer.size() - offset), rows)) {
        return false;
    }

    m_records.clear();
    for (size_t rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
        const auto& row = rows[rowIndex];
        if (row.size() < 3 || row[0].empty()) continue;

        const std::string& searchUtf8 = row[0];
        uint32_t searchCount = 0;
        uint64_t lastSearchDate = 0;
        try {
            searchCount = static_cast<uint32_t>(std::stoul(row[1]));
            lastSearchDate = std::stoull(row[2]);
        } catch (...) {
            continue;
        }

        if (!searchUtf8.empty()) {
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, searchUtf8.data(), static_cast<int>(searchUtf8.size()), nullptr, 0);
            if (wideLen > 0) {
                std::wstring wSearch(wideLen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, searchUtf8.data(), static_cast<int>(searchUtf8.size()), wSearch.data(), wideLen);

                std::wstring key = getNormalizedKey(wSearch);
                m_records[key] = SearchHistoryItem{
                    .search = std::move(wSearch),
                    .searchCount = searchCount,
                    .lastSearchDate = lastSearchDate
                };
            }
        }
    }

    m_isDirty = false;
    return true;
}

bool SearchHistoryManager::save() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_csvPath.empty()) return false;

    ScopedUserTokenContext imp(m_impersonationToken, m_requiresImpersonation);
    if (!imp.ok()) {
        return false;
    }

    std::string csvData = "Search,Search Count,Last Search Date\r\n";

    std::vector<SearchHistoryItem> list;
    list.reserve(m_records.size());
    for (const auto& [_, rec] : m_records) {
        list.push_back(rec);
    }

    std::sort(list.begin(), list.end(), [](const SearchHistoryItem& a, const SearchHistoryItem& b) {
        if (a.lastSearchDate != b.lastSearchDate) return a.lastSearchDate > b.lastSearchDate;
        return a.searchCount > b.searchCount;
    });

    for (const auto& item : list) {
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, item.search.data(), static_cast<int>(item.search.size()), nullptr, 0, nullptr, nullptr);
        std::string searchUtf8(utf8Len, '\0');
        if (utf8Len > 0) {
            WideCharToMultiByte(CP_UTF8, 0, item.search.data(), static_cast<int>(item.search.size()), searchUtf8.data(), utf8Len, nullptr, nullptr);
        }

        csvData += detail::escapeCsvField(searchUtf8) + "," +
                   std::to_string(item.searchCount) + "," +
                   std::to_string(item.lastSearchDate) + "\r\n";
    }

    if (!tools3000::common::atomicWriteFileWithFlush(m_csvPath, csvData)) return false;
    m_isDirty = false;
    return true;
}

void SearchHistoryManager::clear() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_records.clear();
    m_isDirty = true;
    save();
}

size_t SearchHistoryManager::size() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_records.size();
}

std::wstring SearchHistoryManager::getFilePath() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_csvPath;
}

} // namespace tools3000::service::db
