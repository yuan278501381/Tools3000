#include "core/utils/ArchivePreview.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string_view>

namespace tools3000::core {

namespace {

constexpr uint32_t kZipEocdSig = 0x06054b50u;
constexpr uint32_t kZip64LocatorSig = 0x07064b50u;
constexpr uint32_t kZip64EocdSig = 0x06064b50u;
constexpr uint32_t kZipCentralHeaderSig = 0x02014b50u;
constexpr size_t kMaxCentralDirBytes = 16u * 1024u * 1024u;
constexpr uint64_t kMaxGzipScanBytes = 64ull * 1024ull * 1024ull;

struct ScopedWinHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~ScopedWinHandle() {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) {
            CloseHandle(h);
        }
    }
};

uint16_t readLe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readLe64(const uint8_t* p) noexcept {
    return static_cast<uint64_t>(readLe32(p)) |
           (static_cast<uint64_t>(readLe32(p + 4)) << 32);
}

bool readFileAt(HANDLE h, uint64_t offset, void* dst, size_t bytes, size_t& outRead) noexcept {
    outRead = 0;
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;

    uint8_t* ptr = static_cast<uint8_t*>(dst);
    size_t remaining = bytes;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<size_t>(1u << 20)));
        DWORD got = 0;
        if (!ReadFile(h, ptr, chunk, &got, nullptr)) return false;
        if (got == 0) break;
        ptr += got;
        outRead += got;
        remaining -= got;
    }
    return true;
}

std::string normalizeArchiveEntryPath(std::string raw) {
    for (char& ch : raw) {
        if (ch == '\\') ch = '/';
    }
    while (raw.size() >= 2 && raw[0] == '.' && raw[1] == '/') {
        raw.erase(0, 2);
    }
    while (!raw.empty() && raw.front() == '/') {
        raw.erase(0, 1);
    }
    return raw;
}

std::string decodeZipFilename(const char* raw, size_t len, bool isUtf8Flag) {
    if (!raw || len == 0) return {};
    if (isUtf8Flag) {
        return normalizeArchiveEntryPath(std::string(raw, len));
    }
    // 先校验是否为合法 UTF-8（部分现代压缩工具未设 bit11 但仍写 UTF-8）
    const int utf8Chars = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, raw, static_cast<int>(len), nullptr, 0);
    if (utf8Chars > 0) {
        return normalizeArchiveEntryPath(std::string(raw, len));
    }
    // 否则按系统 ANSI 代码页（中文 Windows 下 CP936/GBK）无损转 UTF-8
    const int acpChars = MultiByteToWideChar(CP_ACP, 0, raw, static_cast<int>(len), nullptr, 0);
    if (acpChars <= 0) {
        return normalizeArchiveEntryPath(std::string(raw, len));
    }
    std::wstring wide(static_cast<size_t>(acpChars), L'\0');
    MultiByteToWideChar(CP_ACP, 0, raw, static_cast<int>(len), wide.data(), acpChars);
    return normalizeArchiveEntryPath(WinUtils::wstringToUtf8(wide));
}

std::string formatDosDateTime(uint16_t dosTime, uint16_t dosDate) {
    if (dosDate == 0) return {};
    const unsigned year = 1980u + ((dosDate >> 9) & 0x7Fu);
    const unsigned month = (dosDate >> 5) & 0x0Fu;
    const unsigned day = dosDate & 0x1Fu;
    const unsigned hour = (dosTime >> 11) & 0x1Fu;
    const unsigned minute = (dosTime >> 5) & 0x3Fu;
    if (month < 1 || month > 12 || day < 1 || day > 31) return {};
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u", year, month, day, hour, minute);
    return buf;
}

std::string formatUnixEpoch(uint64_t epochSec) {
    if (epochSec == 0 || epochSec > 4102444800ull) return {};
    const time_t t = static_cast<time_t>(epochSec);
    tm localTm{};
    if (localtime_s(&localTm, &t) != 0) return {};
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                  localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday,
                  localTm.tm_hour, localTm.tm_min);
    return buf;
}

bool inspectZipHandle(HANDLE hFile, uint64_t fileSize, size_t maxDisplayEntries, ArchivePreviewResult& out) {
    if (fileSize < 22) return false;

    const size_t tailLen = static_cast<size_t>((std::min)(fileSize, static_cast<uint64_t>(65557u + 64u)));
    const uint64_t tailOffset = fileSize - tailLen;
    std::vector<uint8_t> tail(tailLen);
    size_t got = 0;
    if (!readFileAt(hFile, tailOffset, tail.data(), tailLen, got) || got < 22) {
        return false;
    }

    size_t eocdPos = std::string::npos;
    for (size_t i = got - 22 + 1; i > 0; --i) {
        const size_t idx = i - 1;
        if (readLe32(tail.data() + idx) == kZipEocdSig) {
            const uint16_t commentLen = readLe16(tail.data() + idx + 20);
            if (idx + 22 + commentLen <= got) {
                eocdPos = idx;
                break;
            }
        }
    }
    if (eocdPos == std::string::npos) return false;

    uint64_t totalEntries = readLe16(tail.data() + eocdPos + 10);
    uint64_t cdSize = readLe32(tail.data() + eocdPos + 12);
    uint64_t cdOffset = readLe32(tail.data() + eocdPos + 16);

    // 支持 ZIP64 End of Central Directory Locator
    if (eocdPos >= 20 && readLe32(tail.data() + eocdPos - 20) == kZip64LocatorSig) {
        const uint64_t zip64EocdOffset = readLe64(tail.data() + eocdPos - 12);
        if (zip64EocdOffset + 56 <= fileSize) {
            uint8_t z64[56]{};
            size_t z64Read = 0;
            if (readFileAt(hFile, zip64EocdOffset, z64, sizeof(z64), z64Read) &&
                z64Read == sizeof(z64) && readLe32(z64) == kZip64EocdSig) {
                totalEntries = readLe64(z64 + 32);
                cdSize = readLe64(z64 + 40);
                cdOffset = readLe64(z64 + 48);
            }
        }
    }

    if (cdOffset >= fileSize || cdSize == 0) {
        out.ok = true;
        out.totalEntries = 0;
        return true;
    }

    const size_t bytesToRead = static_cast<size_t>((std::min)(
        (std::min)(cdSize, fileSize - cdOffset),
        static_cast<uint64_t>(kMaxCentralDirBytes)));
    std::vector<uint8_t> cd(bytesToRead);
    size_t cdRead = 0;
    if (!readFileAt(hFile, cdOffset, cd.data(), bytesToRead, cdRead) || cdRead < 46) {
        return false;
    }

    out.entries.reserve((std::min)(maxDisplayEntries, static_cast<size_t>(totalEntries)));
    size_t pos = 0;
    uint64_t parsedCount = 0;
    while (pos + 46 <= cdRead) {
        if (readLe32(cd.data() + pos) != kZipCentralHeaderSig) break;
        const uint16_t flags = readLe16(cd.data() + pos + 8);
        const uint16_t dosTime = readLe16(cd.data() + pos + 12);
        const uint16_t dosDate = readLe16(cd.data() + pos + 14);
        uint64_t compSize = readLe32(cd.data() + pos + 20);
        uint64_t uncompSize = readLe32(cd.data() + pos + 24);
        const uint16_t nameLen = readLe16(cd.data() + pos + 28);
        const uint16_t extraLen = readLe16(cd.data() + pos + 30);
        const uint16_t commentLen = readLe16(cd.data() + pos + 32);
        const uint32_t extAttr = readLe32(cd.data() + pos + 38);

        if (pos + 46u + nameLen + extraLen + commentLen > cdRead) {
            out.truncated = true;
            break;
        }

        // 解析 ZIP64 扩展字段 (0x0001)
        if ((uncompSize == 0xFFFFFFFFu || compSize == 0xFFFFFFFFu) && extraLen >= 4) {
            size_t epos = pos + 46u + nameLen;
            const size_t eend = epos + extraLen;
            while (epos + 4 <= eend) {
                const uint16_t hid = readLe16(cd.data() + epos);
                const uint16_t hsz = readLe16(cd.data() + epos + 2);
                epos += 4;
                if (epos + hsz > eend) break;
                if (hid == 0x0001u) {
                    size_t fpos = epos;
                    if (uncompSize == 0xFFFFFFFFu && fpos + 8 <= epos + hsz) {
                        uncompSize = readLe64(cd.data() + fpos);
                        fpos += 8;
                    }
                    if (compSize == 0xFFFFFFFFu && fpos + 8 <= epos + hsz) {
                        compSize = readLe64(cd.data() + fpos);
                    }
                    break;
                }
                epos += hsz;
            }
        }

        std::string entryPath = decodeZipFilename(
            reinterpret_cast<const char*>(cd.data() + pos + 46),
            nameLen,
            (flags & 0x0800u) != 0);
        const bool isDir = (!entryPath.empty() && entryPath.back() == '/') || ((extAttr & 0x10u) != 0);
        while (!entryPath.empty() && entryPath.back() == '/') {
            entryPath.pop_back();
        }
        const bool entryEncrypted = (flags & 0x0001u) != 0;
        if (entryEncrypted) out.encrypted = true;

        if (!entryPath.empty()) {
            ++parsedCount;
            if (isDir) {
                ++out.totalFolders;
            } else {
                ++out.totalFiles;
                out.totalUncompressedBytes += uncompSize;
            }
            if (out.entries.size() < maxDisplayEntries) {
                ArchiveEntryInfo info;
                info.path = std::move(entryPath);
                info.isDirectory = isDir;
                info.uncompressedSize = isDir ? 0 : uncompSize;
                info.compressedSize = isDir ? 0 : compSize;
                info.encrypted = entryEncrypted;
                info.modified = formatDosDateTime(dosTime, dosDate);
                out.entries.push_back(std::move(info));
            } else {
                out.truncated = true;
            }
        }

        pos += 46u + nameLen + extraLen + commentLen;
    }

    if (cdSize > bytesToRead) out.truncated = true;
    out.totalEntries = (std::max)(parsedCount, totalEntries);
    out.ok = true;
    return true;
}

uint64_t parseTarOctal(const uint8_t* p, size_t len) noexcept {
    if (len > 0 && (p[0] & 0x80u) != 0) {
        // GNU base-256 扩展大尺寸
        uint64_t val = 0;
        for (size_t i = 1; i < len; ++i) {
            val = (val << 8) | p[i];
        }
        return val;
    }
    uint64_t val = 0;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = p[i];
        if (c == 0 || c == ' ') {
            if (val != 0) break;
            continue;
        }
        if (c >= '0' && c <= '7') {
            val = (val << 3) | static_cast<uint64_t>(c - '0');
        }
    }
    return val;
}

bool isAllZeroBlock(const uint8_t block[512]) noexcept {
    for (size_t i = 0; i < 512; ++i) {
        if (block[i] != 0) return false;
    }
    return true;
}

bool inspectTarStream(const std::function<size_t(uint8_t*, size_t)>& readExactOrLess,
                      const std::function<bool(uint64_t)>& skipBytes,
                      size_t maxDisplayEntries,
                      ArchivePreviewResult& out) {
    uint8_t header[512]{};
    std::string pendingLongName;
    uint64_t scannedUncompressed = 0;
    bool sawAnyHeader = false;

    while (true) {
        const size_t got = readExactOrLess(header, 512);
        if (got < 512) break;
        if (isAllZeroBlock(header)) break;

        sawAnyHeader = true;
        const uint64_t entrySize = parseTarOctal(header + 124, 12);
        const uint64_t mtime = parseTarOctal(header + 136, 12);
        const char typeFlag = static_cast<char>(header[156]);

        const auto readFixedStr = [](const uint8_t* ptr, size_t cap) -> std::string {
            size_t len = 0;
            while (len < cap && ptr[len] != 0) ++len;
            return std::string(reinterpret_cast<const char*>(ptr), len);
        };

        std::string name = readFixedStr(header, 100);
        const std::string prefix = readFixedStr(header + 345, 155);
        if (!prefix.empty()) {
            name = prefix + "/" + name;
        }

        const uint64_t paddedSize = ((entrySize + 511ull) / 512ull) * 512ull;

        // GNU LongLink ('L')
        if (typeFlag == 'L' && entrySize > 0 && entrySize <= 8192) {
            std::vector<uint8_t> buf(static_cast<size_t>(paddedSize));
            if (readExactOrLess(buf.data(), buf.size()) < entrySize) break;
            size_t slen = static_cast<size_t>(entrySize);
            while (slen > 0 && buf[slen - 1] == 0) --slen;
            pendingLongName.assign(reinterpret_cast<const char*>(buf.data()), slen);
            continue;
        }

        // PAX extended header ('x' / 'g')
        if ((typeFlag == 'x' || typeFlag == 'g') && entrySize > 0 && entrySize <= 32768) {
            std::vector<uint8_t> buf(static_cast<size_t>(paddedSize));
            if (readExactOrLess(buf.data(), buf.size()) < entrySize) break;
            if (typeFlag == 'x') {
                std::string_view pax(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(entrySize));
                const size_t pathKey = pax.find(" path=");
                if (pathKey != std::string_view::npos) {
                    const size_t valStart = pathKey + 6;
                    const size_t valEnd = pax.find('\n', valStart);
                    if (valEnd != std::string_view::npos && valEnd > valStart) {
                        pendingLongName.assign(pax.substr(valStart, valEnd - valStart));
                    }
                }
            }
            continue;
        }

        if (!pendingLongName.empty()) {
            name = std::move(pendingLongName);
            pendingLongName.clear();
        }

        std::string cleanPath = normalizeArchiveEntryPath(std::move(name));
        const bool isDir = (typeFlag == '5') || (!cleanPath.empty() && cleanPath.back() == '/');
        while (!cleanPath.empty() && cleanPath.back() == '/') {
            cleanPath.pop_back();
        }

        if (!cleanPath.empty()) {
            ++out.totalEntries;
            if (isDir) {
                ++out.totalFolders;
            } else {
                ++out.totalFiles;
                out.totalUncompressedBytes += entrySize;
            }
            if (out.entries.size() < maxDisplayEntries) {
                ArchiveEntryInfo info;
                info.path = std::move(cleanPath);
                info.isDirectory = isDir;
                info.uncompressedSize = isDir ? 0 : entrySize;
                info.compressedSize = 0;
                info.modified = formatUnixEpoch(mtime);
                out.entries.push_back(std::move(info));
            } else {
                out.truncated = true;
            }
        }

        scannedUncompressed += 512ull + paddedSize;
        if (scannedUncompressed > kMaxGzipScanBytes && !skipBytes(paddedSize)) {
            out.truncated = true;
            break;
        }
        if (paddedSize > 0 && !skipBytes(paddedSize)) {
            out.truncated = true;
            break;
        }
    }

    out.ok = sawAnyHeader;
    return sawAnyHeader;
}

bool inspectTarFile(HANDLE hFile, uint64_t fileSize, size_t maxDisplayEntries, ArchivePreviewResult& out) {
    uint64_t cursor = 0;
    auto readFn = [&](uint8_t* dst, size_t len) -> size_t {
        if (cursor >= fileSize) return 0;
        size_t got = 0;
        if (!readFileAt(hFile, cursor, dst, len, got)) return 0;
        cursor += got;
        return got;
    };
    auto skipFn = [&](uint64_t bytes) -> bool {
        if (cursor + bytes > fileSize) return false;
        cursor += bytes;
        return true;
    };
    return inspectTarStream(readFn, skipFn, maxDisplayEntries, out);
}

bool inspectTarGzFile(HANDLE hFile, uint64_t fileSize, size_t maxDisplayEntries, ArchivePreviewResult& out) {
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return false;
    struct ZGuard {
        z_stream* s;
        ~ZGuard() { inflateEnd(s); }
    } zGuard{&zs};

    std::vector<uint8_t> inBuf(64 * 1024);
    uint64_t fileCursor = 0;
    uint64_t totalInflated = 0;
    bool eof = false;

    auto pullDecompressed = [&](uint8_t* dst, size_t want) -> size_t {
        size_t produced = 0;
        while (produced < want && !eof && totalInflated < kMaxGzipScanBytes) {
            if (zs.avail_in == 0) {
                if (fileCursor >= fileSize) {
                    eof = true;
                    break;
                }
                size_t got = 0;
                if (!readFileAt(hFile, fileCursor, inBuf.data(), inBuf.size(), got) || got == 0) {
                    eof = true;
                    break;
                }
                fileCursor += got;
                zs.next_in = inBuf.data();
                zs.avail_in = static_cast<uInt>(got);
            }
            const size_t chunk = (std::min)(want - produced, static_cast<size_t>(kMaxGzipScanBytes - totalInflated));
            if (chunk == 0) {
                out.truncated = true;
                eof = true;
                break;
            }
            zs.next_out = dst + produced;
            zs.avail_out = static_cast<uInt>(chunk);
            const int ret = inflate(&zs, Z_NO_FLUSH);
            const size_t delta = chunk - zs.avail_out;
            produced += delta;
            totalInflated += delta;
            if (ret == Z_STREAM_END) {
                eof = true;
                break;
            }
            if (ret != Z_OK && ret != Z_BUF_ERROR) {
                eof = true;
                break;
            }
        }
        return produced;
    };

    std::vector<uint8_t> discardBuf(32 * 1024);
    auto skipDecompressed = [&](uint64_t bytes) -> bool {
        uint64_t left = bytes;
        while (left > 0) {
            const size_t step = static_cast<size_t>((std::min)(left, static_cast<uint64_t>(discardBuf.size())));
            const size_t got = pullDecompressed(discardBuf.data(), step);
            if (got < step) return false;
            left -= got;
        }
        return true;
    };

    return inspectTarStream(pullDecompressed, skipDecompressed, maxDisplayEntries, out);
}

bool inspectStandaloneGzFile(HANDLE hFile, uint64_t fileSize, const std::wstring& filePath, ArchivePreviewResult& out) {
    if (fileSize < 18) return false;
    uint8_t hdr[256]{};
    size_t got = 0;
    if (!readFileAt(hFile, 0, hdr, sizeof(hdr), got) || got < 10) return false;
    if (hdr[0] != 0x1Fu || hdr[1] != 0x8Bu) return false;

    const uint8_t flg = hdr[3];
    const uint32_t mtime = readLe32(hdr + 4);
    size_t pos = 10;
    if ((flg & 0x04u) != 0 && pos + 2 <= got) {
        pos += 2 + readLe16(hdr + pos);
    }
    std::string innerName;
    if ((flg & 0x08u) != 0 && pos < got) {
        size_t end = pos;
        while (end < got && hdr[end] != 0) ++end;
        innerName.assign(reinterpret_cast<const char*>(hdr + pos), end - pos);
    }
    if (innerName.empty()) {
        std::filesystem::path p(filePath);
        innerName = WinUtils::wstringToUtf8(p.stem().wstring());
    }

    uint8_t trailer[4]{};
    size_t trGot = 0;
    uint32_t isize = 0;
    if (readFileAt(hFile, fileSize - 4, trailer, 4, trGot) && trGot == 4) {
        isize = readLe32(trailer);
    }

    ArchiveEntryInfo info;
    info.path = normalizeArchiveEntryPath(innerName);
    info.isDirectory = false;
    info.uncompressedSize = isize;
    info.compressedSize = fileSize;
    info.modified = formatUnixEpoch(mtime);
    out.entries.push_back(std::move(info));
    out.totalEntries = 1;
    out.totalFiles = 1;
    out.totalUncompressedBytes = isize;
    out.ok = true;
    return true;
}

bool tryInspectViaWindowsSystemTar(const std::wstring& filePath,
                                   size_t maxDisplayEntries,
                                   ArchivePreviewResult& out) {
    wchar_t sysDir[MAX_PATH]{};
    const UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (sysLen == 0 || sysLen >= MAX_PATH) return false;

    const std::filesystem::path tarExe = std::filesystem::path(sysDir) / L"tar.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(tarExe, ec)) return false;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring cmdLine = L"\"" + tarExe.wstring() + L"\" -tf \"" + filePath + L"\"";
    const BOOL ok = CreateProcessW(
        tarExe.wstring().c_str(),
        cmdLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
        nullptr,
        nullptr,
        &si,
        &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return false;
    }

    std::string output;
    output.reserve(32 * 1024);
    const ULONGLONG deadline = GetTickCount64() + 1800;
    char buf[4096];
    while (GetTickCount64() < deadline) {
        DWORD avail = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail > 0) {
            DWORD got = 0;
            if (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0) {
                if (output.size() < 2u * 1024u * 1024u) {
                    output.append(buf, got);
                } else {
                    out.truncated = true;
                }
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 15) == WAIT_OBJECT_0) {
            // 排空管道残留
            while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                DWORD got = 0;
                if (!ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) || got == 0) break;
                if (output.size() < 2u * 1024u * 1024u) {
                    output.append(buf, got);
                }
            }
            break;
        }
    }

    DWORD exitCode = 1;
    if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
    } else {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    }
    CloseHandle(readPipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exitCode != 0 || output.empty()) {
        const std::string lowerOut = WinUtils::toLower(output);
        if (lowerOut.find("passphrase") != std::string::npos ||
            lowerOut.find("encrypted") != std::string::npos ||
            lowerOut.find("password") != std::string::npos) {
            out.encrypted = true;
        }
        return false;
    }

    size_t start = 0;
    while (start < output.size()) {
        size_t end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();
        std::string line = output.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = end + 1;

        if (line.empty()) continue;
        if (line.rfind("tar:", 0) == 0 || line.rfind("tar.exe:", 0) == 0) continue;

        const bool isDir = line.back() == '/' || line.back() == '\\';
        std::string clean = normalizeArchiveEntryPath(std::move(line));
        while (!clean.empty() && clean.back() == '/') clean.pop_back();
        if (clean.empty()) continue;

        ++out.totalEntries;
        if (isDir) ++out.totalFolders;
        else ++out.totalFiles;

        if (out.entries.size() < maxDisplayEntries) {
            ArchiveEntryInfo info;
            info.path = std::move(clean);
            info.isDirectory = isDir;
            out.entries.push_back(std::move(info));
        } else {
            out.truncated = true;
        }
    }

    if (out.totalEntries > 0) {
        out.ok = true;
        out.detailNote = "Windows libarchive";
        return true;
    }
    return false;
}

}  // namespace

ArchivePreviewResult ArchivePreview::inspectArchive(const std::wstring& filePath,
                                                    size_t maxDisplayEntries) {
    ArchivePreviewResult result;
    if (filePath.empty()) return result;

    ScopedWinHandle file;
    file.h = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (file.h == INVALID_HANDLE_VALUE) {
        return result;
    }

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(file.h, &sz) || sz.QuadPart < 0) {
        return result;
    }
    const uint64_t fileSize = static_cast<uint64_t>(sz.QuadPart);
    result.archiveFileBytes = fileSize;

    const std::wstring lowerPath = WinUtils::toLower(filePath);
    const std::filesystem::path fsPath(filePath);
    const std::wstring extW = WinUtils::toLower(fsPath.extension().wstring());

    // 探测头部魔数以精准识别格式
    uint8_t magic[16]{};
    size_t magicGot = 0;
    readFileAt(file.h, 0, magic, sizeof(magic), magicGot);

    const bool isZipLike =
        extW == L".zip" || extW == L".jar" || extW == L".apk" ||
        extW == L".whl" || extW == L".nupkg" || extW == L".vsix" ||
        extW == L".xpi" || extW == L".epub" ||
        (magicGot >= 4 && magic[0] == 'P' && magic[1] == 'K' && (magic[2] == 3 || magic[2] == 5));

    if (isZipLike) {
        result.format = (extW == L".jar") ? "JAR" :
                        (extW == L".apk") ? "APK" :
                        (extW == L".whl") ? "WHL" :
                        (extW == L".nupkg") ? "NUPKG" : "ZIP";
        if (inspectZipHandle(file.h, fileSize, maxDisplayEntries, result)) {
            return result;
        }
    }

    if (extW == L".tar") {
        result.format = "TAR";
        if (inspectTarFile(file.h, fileSize, maxDisplayEntries, result)) {
            return result;
        }
    }

    const bool isTarGz =
        extW == L".tgz" ||
        (lowerPath.size() > 7 && lowerPath.rfind(L".tar.gz") == lowerPath.size() - 7);
    if (isTarGz) {
        result.format = "TAR.GZ";
        if (inspectTarGzFile(file.h, fileSize, maxDisplayEntries, result)) {
            return result;
        }
    }

    if (extW == L".gz") {
        result.format = "GZ";
        if (inspectTarGzFile(file.h, fileSize, maxDisplayEntries, result)) {
            result.format = "TAR.GZ";
            return result;
        }
        if (inspectStandaloneGzFile(file.h, fileSize, filePath, result)) {
            return result;
        }
    }

    // 7z / RAR / tar.xz / tar.bz2 / tar.zst：魔数头检测 + Win11 系统原生 tar.exe 安全探测
    if (extW == L".7z" ||
        (magicGot >= 6 && magic[0] == 0x37 && magic[1] == 0x7A && magic[2] == 0xBC &&
         magic[3] == 0xAF && magic[4] == 0x27 && magic[5] == 0x1C)) {
        result.format = "7Z";
        if (magicGot >= 8) {
            result.detailNote = "7z v" + std::to_string(magic[6]) + "." + std::to_string(magic[7]);
        }
    } else if (extW == L".rar" ||
               (magicGot >= 7 && magic[0] == 'R' && magic[1] == 'a' && magic[2] == 'r' &&
                magic[3] == '!' && magic[4] == 0x1A && magic[5] == 0x07)) {
        result.format = (magicGot >= 7 && magic[6] == 0x01) ? "RAR5" : "RAR4";
        result.detailNote = result.format;
    } else if (extW == L".xz" || extW == L".txz") {
        result.format = "TAR.XZ";
    } else if (extW == L".bz2" || extW == L".tbz2") {
        result.format = "TAR.BZ2";
    } else if (extW == L".zst" || extW == L".tzst") {
        result.format = "TAR.ZST";
    } else {
        result.format = "ARCHIVE";
    }

    // 释放本进程读句柄前不锁文件，调用系统内置 libarchive tar.exe 安全探测
    if (tryInspectViaWindowsSystemTar(filePath, maxDisplayEntries, result)) {
        return result;
    }

    // 优雅降级为结构化压缩包信息卡（仍标记 ok=false 但带格式/体积/加密提示供前端友好展示）
    return result;
}

}  // namespace tools3000::core
