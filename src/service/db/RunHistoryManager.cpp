#include "RunHistoryManager.h"
#include "CsvUtils.h"
#include "../../common/AtomicFile.h"
#include <shlobj.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cwctype>
#include <cmath>

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

RunHistoryManager& RunHistoryManager::instance() {
    static RunHistoryManager s_mgr;
    return s_mgr;
}

RunHistoryManager::RunHistoryManager() {
    init();
}

RunHistoryManager::~RunHistoryManager() {
    if (m_isDirty) {
        save();
    }
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
    std::wstring key = path;
    for (auto& ch : key) {
        if (ch == L'/') ch = L'\\';
        ch = std::towlower(ch);
    }
    return key;
}

void RunHistoryManager::init(const std::wstring& customCsvPath) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!customCsvPath.empty()) {
        m_csvPath = customCsvPath;
    } else if (m_csvPath.empty()) {
        m_csvPath = getDefaultAppDataFolder() + L"\\Run History.csv";
    }
    load();
}

void RunHistoryManager::initForSid(const std::wstring& sid) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_isDirty) {
        save();
    }
    m_records.clear();
    m_isDirty = false;
    m_csvPath = getUserHistoryPath(sid, L"Run History.csv");
    load();
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

    // 1 秒 = 10,000,000 个 100ns 间隔；1 小时 = 36,000,000,000
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

    // Frecency 综合得分公式
    return static_cast<double>(rec.runCount) * 100.0 + recencyBonus;
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

    std::string csvData = "Filename,Run Count,Last Run Date\r\n";

    // 排序后导出：运行频次高、最近运行的排在前列
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
