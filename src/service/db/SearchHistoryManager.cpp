#include "SearchHistoryManager.h"
#include "RunHistoryManager.h"
#include "CsvUtils.h"
#include "../../common/AtomicFile.h"
#include <shlobj.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cwctype>

namespace tools3000::service::db {

namespace {

std::wstring getDefaultAppDataFolder() {
    wchar_t appData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData))) {
        std::wstring dir = std::wstring(appData) + L"\\Tools3000";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir;
    }
    return L".";
}

std::wstring getUserHistoryPath(const std::wstring& sid, const std::wstring& fileName) {
    if (sid.empty()) {
        return getDefaultAppDataFolder() + L"\\" + fileName;
    }
    wchar_t commonAppData[MAX_PATH]{};
    std::wstring baseDir;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, commonAppData))) {
        baseDir = std::wstring(commonAppData) + L"\\Tools3000\\users\\" + sid;
    } else {
        baseDir = getDefaultAppDataFolder() + L"\\users\\" + sid;
    }

    std::wstring sddl = L"D:P(A;OICI;GA;;;" + sid + L")(A;OICI;GA;;;BA)(A;OICI;GA;;;SY)";
    PSECURITY_DESCRIPTOR pSD = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &pSD, nullptr)) {
        sa.lpSecurityDescriptor = pSD;
    }

    std::filesystem::path p(baseDir);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    CreateDirectoryW(baseDir.c_str(), pSD ? &sa : nullptr);
    if (pSD) LocalFree(pSD);

    return baseDir + L"\\" + fileName;
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

SearchHistoryManager::SearchHistoryManager() {
    init();
}

SearchHistoryManager::~SearchHistoryManager() {
    if (m_isDirty) {
        save();
    }
}

std::wstring SearchHistoryManager::getNormalizedKey(const std::wstring& search) const {
    std::wstring key = trim(search);
    for (auto& ch : key) {
        ch = std::towlower(ch);
    }
    return key;
}

void SearchHistoryManager::init(const std::wstring& customCsvPath) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!customCsvPath.empty()) {
        m_csvPath = customCsvPath;
    } else if (m_csvPath.empty()) {
        m_csvPath = getDefaultAppDataFolder() + L"\\Search History.csv";
    }
    load();
}

void SearchHistoryManager::initForSid(const std::wstring& sid) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_isDirty) {
        save();
    }
    m_records.clear();
    m_isDirty = false;
    m_csvPath = getUserHistoryPath(sid, L"Search History.csv");
    load();
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
        it->second.search = trimmed; // 更新保留最新大小写
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

    HANDLE hFile = CreateFileW(
        m_csvPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
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
