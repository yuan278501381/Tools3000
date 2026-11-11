#include "RunHistoryManager.h"
#include "CsvUtils.h"
#include "../../common/AtomicFile.h"
#include <shlobj.h>
#include <objbase.h>
#include <sddl.h>
#include <filesystem>
#include <algorithm>
#include <cwctype>
#include <cmath>

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

} // namespace

RunHistoryManager& RunHistoryManager::instance() {
    static RunHistoryManager s_mgr;
    return s_mgr;
}

RunHistoryManager::RunHistoryManager() = default;

RunHistoryManager::~RunHistoryManager() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_isDirty) {
        save();
    }
    closeImpersonationTokenUnlocked();
}

void RunHistoryManager::closeImpersonationTokenUnlocked() noexcept {
    if (m_impersonationToken && m_impersonationToken != INVALID_HANDLE_VALUE) {
        CloseHandle(m_impersonationToken);
    }
    m_impersonationToken = nullptr;
    m_requiresImpersonation = false;
}

uint64_t RunHistoryManager::getCurrentFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

std::wstring RunHistoryManager::getNormalizedKey(const std::wstring& path) const {
    std::wstring key;
    key.reserve(path.size());
    for (wchar_t ch : path) {
        key.push_back(RunHistorySnapshot::foldPathChar(ch));
    }
    return key;
}

void RunHistoryManager::init(const std::wstring& customCsvPath) {
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
        m_csvPath = getDefaultAppDataFolder() + L"\\Run History.csv";
    }
    load();
}

void RunHistoryManager::initForSid(const std::wstring& sid) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_isDirty && !m_csvPath.empty()) {
        save();
    }
    // Never write as SYSTEM into a user-writable ProgramData directory.
    // Defer actual user file binding until bindClientUserContext supplies the client token;
    // when running in single-user non-SYSTEM mode without a pipe token, resolve current user's LocalAppData.
    closeImpersonationTokenUnlocked();
    m_boundClientSid = sid;
    m_records.clear();
    m_isDirty = false;
    m_csvPath.clear();
}

bool RunHistoryManager::bindClientUserContext(
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
    const std::wstring targetCsvPath = targetUserDir + L"\\Run History.csv";

    // Fast path: already bound to the exact same user profile path; refresh token handle and return.
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

void RunHistoryManager::recordRun(const std::wstring& filename) {
    if (filename.empty()) return;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::wstring key = getNormalizedKey(filename);
    uint64_t nowFt = getCurrentFileTime();

    auto it = m_records.find(key);
    if (it != m_records.end()) {
        it->second.runCount++;
        it->second.lastRunDate = nowFt;
        it->second.filename = filename; // 更新保留最新大小写
    } else {
        m_records[key] = RunRecord{
            .filename = filename,
            .runCount = 1,
            .lastRunDate = nowFt
        };
    }
    m_isDirty = true;
    save();
}

uint32_t RunHistoryManager::getRunCount(const std::wstring& filename) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::wstring key = getNormalizedKey(filename);
    auto it = m_records.find(key);
    if (it != m_records.end()) {
        return it->second.runCount;
    }
    return 0;
}

uint64_t RunHistoryManager::getLastRunDate(const std::wstring& filename) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::wstring key = getNormalizedKey(filename);
    auto it = m_records.find(key);
    if (it != m_records.end()) {
        return it->second.lastRunDate;
    }
    return 0;
}

double RunHistoryManager::calculateFrecencyScore(const std::wstring& filename, uint64_t currentFileTime) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_records.empty()) {
        return 0.0;
    }
    std::wstring key = getNormalizedKey(filename);
    auto it = m_records.find(key);
    if (it == m_records.end() || it->second.runCount == 0) {
        return 0.0;
    }

    const auto& rec = it->second;
    uint64_t nowFt = (currentFileTime != 0) ? currentFileTime : getCurrentFileTime();

    constexpr uint64_t FT_ONE_HOUR = 3600ULL * 10000000ULL;
    constexpr uint64_t FT_ONE_DAY = 24ULL * FT_ONE_HOUR;

    uint64_t diff = (nowFt > rec.lastRunDate) ? (nowFt - rec.lastRunDate) : 0;

    double recencyBonus = 0.0;
    if (diff <= FT_ONE_HOUR * 4) {
        recencyBonus = 1000.0; // 4 小时内访问
    } else if (diff <= FT_ONE_DAY) {
        recencyBonus = 700.0;  // 24 小时内访问
    } else if (diff <= FT_ONE_DAY * 7) {
        recencyBonus = 400.0;  // 7 天内访问
    } else if (diff <= FT_ONE_DAY * 30) {
        recencyBonus = 150.0;  // 30 天内访问
    } else {
        recencyBonus = 20.0;   // 历史访问
    }

    return static_cast<double>(rec.runCount) * 100.0 + recencyBonus;
}

RunHistorySnapshot RunHistoryManager::captureSnapshot(uint64_t currentFileTime) const {
    RunHistorySnapshot snap;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_records.empty()) {
        return snap;
    }

    const uint64_t nowFt = (currentFileTime != 0) ? currentFileTime : getCurrentFileTime();
    constexpr uint64_t FT_ONE_HOUR = 3600ULL * 10000000ULL;
    constexpr uint64_t FT_ONE_DAY = 24ULL * FT_ONE_HOUR;

    snap.entries.reserve(m_records.size());
    for (const auto& [key, rec] : m_records) {
        if (rec.runCount == 0) continue;
        const uint64_t diff = (nowFt > rec.lastRunDate) ? (nowFt - rec.lastRunDate) : 0;
        double recencyBonus = 20.0;
        if (diff <= FT_ONE_HOUR * 4) {
            recencyBonus = 1000.0;
        } else if (diff <= FT_ONE_DAY) {
            recencyBonus = 700.0;
        } else if (diff <= FT_ONE_DAY * 7) {
            recencyBonus = 400.0;
        } else if (diff <= FT_ONE_DAY * 30) {
            recencyBonus = 150.0;
        }
        const double score = static_cast<double>(rec.runCount) * 100.0 + recencyBonus;
        snap.entries.emplace(key, RunEntryStats{rec.runCount, score});

        uint64_t lenBit = 0;
        uint64_t bloomBits = 0;
        RunHistorySnapshot::computeBloomBits(key, lenBit, bloomBits);
        snap.lenMask |= lenBit;
        snap.bloomMask |= bloomBits;
    }
    return snap;
}

std::vector<RunRecord> RunHistoryManager::getTopRuns(size_t limit) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<RunRecord> result;
    result.reserve(m_records.size());
    for (const auto& [_, rec] : m_records) {
        result.push_back(rec);
    }

    uint64_t nowFt = getCurrentFileTime();
    std::sort(result.begin(), result.end(), [this, nowFt](const RunRecord& a, const RunRecord& b) {
        double scoreA = calculateFrecencyScore(a.filename, nowFt);
        double scoreB = calculateFrecencyScore(b.filename, nowFt);
        if (scoreA != scoreB) return scoreA > scoreB;
        return a.runCount > b.runCount;
    });

    if (result.size() > limit) {
        result.resize(limit);
    }
    return result;
}

bool RunHistoryManager::load() {
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

        const std::string& filenameUtf8 = row[0];
        uint32_t runCount = 0;
        uint64_t lastRunDate = 0;
        try {
            runCount = static_cast<uint32_t>(std::stoul(row[1]));
            lastRunDate = std::stoull(row[2]);
        } catch (...) {
            continue;
        }

        if (!filenameUtf8.empty()) {
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, filenameUtf8.data(), static_cast<int>(filenameUtf8.size()), nullptr, 0);
            if (wideLen > 0) {
                std::wstring wFilename(wideLen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, filenameUtf8.data(), static_cast<int>(filenameUtf8.size()), wFilename.data(), wideLen);

                std::wstring key = getNormalizedKey(wFilename);
                m_records[key] = RunRecord{
                    .filename = std::move(wFilename),
                    .runCount = runCount,
                    .lastRunDate = lastRunDate
                };
            }
        }
    }

    m_isDirty = false;
    return true;
}

bool RunHistoryManager::save() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_csvPath.empty()) return false;

    ScopedUserTokenContext imp(m_impersonationToken, m_requiresImpersonation);
    if (!imp.ok()) {
        return false;
    }

    std::string csvData = "Filename,Run Count,Last Run Date\r\n";

    std::vector<RunRecord> list;
    list.reserve(m_records.size());
    for (const auto& [_, rec] : m_records) {
        list.push_back(rec);
    }

    std::sort(list.begin(), list.end(), [](const RunRecord& a, const RunRecord& b) {
        if (a.runCount != b.runCount) return a.runCount > b.runCount;
        return a.lastRunDate > b.lastRunDate;
    });

    for (const auto& item : list) {
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, item.filename.data(), static_cast<int>(item.filename.size()), nullptr, 0, nullptr, nullptr);
        std::string filenameUtf8(utf8Len, '\0');
        if (utf8Len > 0) {
            WideCharToMultiByte(CP_UTF8, 0, item.filename.data(), static_cast<int>(item.filename.size()), filenameUtf8.data(), utf8Len, nullptr, nullptr);
        }

        csvData += detail::escapeCsvField(filenameUtf8) + "," +
                   std::to_string(item.runCount) + "," +
                   std::to_string(item.lastRunDate) + "\r\n";
    }

    if (!tools3000::common::atomicWriteFileWithFlush(m_csvPath, csvData)) return false;
    m_isDirty = false;
    return true;
}

void RunHistoryManager::clear() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_records.clear();
    m_isDirty = true;
    save();
}

size_t RunHistoryManager::size() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_records.size();
}

std::wstring RunHistoryManager::getFilePath() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_csvPath;
}

} // namespace tools3000::service::db
