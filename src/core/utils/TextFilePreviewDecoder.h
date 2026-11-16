#pragma once
#include "core/utils/WinUtils.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace tools3000::core {

struct DecodedTextPreview {
    bool fileOpened = false;
    bool isReadableText = false;
    bool truncated = false;
    std::string encoding;
    std::string utf8Text;
    std::string hexDump;
};

class TextFilePreviewDecoder {
public:
    static constexpr size_t kMaxTextPreviewBytes = 256 * 1024; // 256 KB 极速预览上限

    /// 识别明确的非纯文本二进制魔数（PDF / ZIP-Office / OLE2-Office / 图像 / 压缩包 / SQLite）
    static bool hasKnownBinarySignature(const uint8_t* ptr, size_t len) noexcept {
        if (!ptr || len < 4) return false;
        if (len >= 5 && std::memcmp(ptr, "%PDF-", 5) == 0) return true;
        if (ptr[0] == 0x50 && ptr[1] == 0x4B &&
            ((ptr[2] == 0x03 && ptr[3] == 0x04) ||
             (ptr[2] == 0x05 && ptr[3] == 0x06) ||
             (ptr[2] == 0x07 && ptr[3] == 0x08))) {
            return true;
        }
        static constexpr uint8_t kOle2Sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
        if (len >= 8 && std::memcmp(ptr, kOle2Sig, 8) == 0) return true;
        static constexpr uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        if (len >= 8 && std::memcmp(ptr, kPngSig, 8) == 0) return true;
        if (ptr[0] == 0xFF && ptr[1] == 0xD8 && ptr[2] == 0xFF) return true;
        if (len >= 6 && (std::memcmp(ptr, "GIF87a", 6) == 0 || std::memcmp(ptr, "GIF89a", 6) == 0)) return true;
        static constexpr uint8_t kRarSig[6] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07};
        if (len >= 6 && std::memcmp(ptr, kRarSig, 6) == 0) return true;
        static constexpr uint8_t k7zSig[6] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
        if (len >= 6 && std::memcmp(ptr, k7zSig, 6) == 0) return true;
        if (ptr[0] == 0x1F && ptr[1] == 0x8B && ptr[2] == 0x08) return true;
        if (len >= 16 && std::memcmp(ptr, "SQLite format 3", 15) == 0) return true;
        return false;
    }

    static bool isValidUtf8Buffer(const uint8_t* data, size_t len) noexcept {
        size_t i = 0;
        size_t badCtrl = 0;
        while (i < len) {
            const uint8_t c = data[i];
            if (c == 0x00) return false;
            if (c <= 0x7F) {
                if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D && c != 0x0C && c != 0x08 && c != 0x1B) {
                    ++badCtrl;
                }
                ++i;
            } else if (c >= 0xC2 && c <= 0xDF) {
                if (i + 1 >= len) break; // 允许截断尾部不完整多字节序列
                if ((data[i + 1] & 0xC0) != 0x80) return false;
                i += 2;
            } else if (c >= 0xE0 && c <= 0xEF) {
                if (i + 2 >= len) {
                    if (i + 1 < len && (data[i + 1] & 0xC0) != 0x80) return false;
                    break;
                }
                const uint8_t c1 = data[i + 1];
                const uint8_t c2 = data[i + 2];
                if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
                if (c == 0xE0 && c1 < 0xA0) return false;
                if (c == 0xED && c1 >= 0xA0) return false;
                i += 3;
            } else if (c >= 0xF0 && c <= 0xF4) {
                if (i + 3 >= len) {
                    for (size_t k = i + 1; k < len; ++k) {
                        if ((data[k] & 0xC0) != 0x80) return false;
                    }
                    break;
                }
                const uint8_t c1 = data[i + 1];
                const uint8_t c2 = data[i + 2];
                const uint8_t c3 = data[i + 3];
                if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
                if (c == 0xF0 && c1 < 0x90) return false;
                if (c == 0xF4 && c1 > 0x8F) return false;
                i += 4;
            } else {
                return false;
            }
        }
        return (badCtrl * 64 <= len);
    }

    static size_t trimIncompleteUtf8Tail(const uint8_t* ptr, size_t len) noexcept {
        if (len == 0 || (ptr[len - 1] & 0x80) == 0) {
            return len;
        }
        size_t seqStart = len - 1;
        while (seqStart > 0 && (len - seqStart) < 4 && (ptr[seqStart] & 0xC0) == 0x80) {
            --seqStart;
        }
        const uint8_t lead = ptr[seqStart];
        size_t expectedLen = 1;
        if ((lead & 0xE0) == 0xC0) expectedLen = 2;
        else if ((lead & 0xF0) == 0xE0) expectedLen = 3;
        else if ((lead & 0xF8) == 0xF0) expectedLen = 4;
        else return seqStart;
        if (len - seqStart < expectedLen) {
            return seqStart;
        }
        return len;
    }

    /// 校验 GBK 双字节序列合法性并在截断处剥离尾部半个双字节汉字（杜绝随机二进制流误判为 GBK 乱码）
    static bool validateAndTrimGbkBuffer(const uint8_t* data, size_t len, size_t& outValidLen) noexcept {
        outValidLen = 0;
        size_t i = 0;
        size_t badCtrl = 0;
        size_t invalidPairs = 0;
        while (i < len) {
            const uint8_t c = data[i];
            if (c == 0x00) return false;
            if (c <= 0x7F) {
                if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D && c != 0x0C && c != 0x08 && c != 0x1B) {
                    ++badCtrl;
                }
                ++i;
                outValidLen = i;
            } else if (c >= 0x81 && c <= 0xFE) {
                if (i + 1 >= len) {
                    // 截断边界刚好切在双字节汉字的前半字节，安全回退到 i
                    break;
                }
                const uint8_t c2 = data[i + 1];
                if (c2 >= 0x40 && c2 <= 0xFE && c2 != 0x7F) {
                    i += 2;
                    outValidLen = i;
                } else if (c2 >= 0x30 && c2 <= 0x39 && i + 3 < len) {
                    // GB18030 四字节扩展字符
                    const uint8_t c3 = data[i + 2];
                    const uint8_t c4 = data[i + 3];
                    if (c3 >= 0x81 && c3 <= 0xFE && c4 >= 0x30 && c4 <= 0x39) {
                        i += 4;
                        outValidLen = i;
                    } else {
                        ++invalidPairs;
                        ++i;
                    }
                } else {
                    ++invalidPairs;
                    ++i;
                }
            } else {
                // 0x80 或 0xFF 非标准 GBK 独立字节
                ++invalidPairs;
                ++i;
            }
        }
        if (badCtrl * 64 > len) return false;
        if (invalidPairs * 128 > len) return false;
        return outValidLen > 0;
    }

    /// 仅当缓冲区确实包含 UTF-16 特征的 0x00 高位字节且无 0x0000 空字符时，才判定为无 BOM 的 UTF-16
    static bool isBomlessUtf16Text(const uint8_t* data, size_t len, bool bigEndian) noexcept {
        if (len < 4) return false;
        const size_t pairCount = len / 2;
        size_t asciiZeroHighCount = 0;
        size_t badCtrl = 0;
        for (size_t i = 0; i < pairCount; ++i) {
            const uint8_t hi = bigEndian ? data[i * 2] : data[i * 2 + 1];
            const uint8_t lo = bigEndian ? data[i * 2 + 1] : data[i * 2];
            const uint16_t ch = static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
            if (ch == 0x0000) return false;
            if (hi == 0x00) {
                if (lo == 0x09 || lo == 0x0A || lo == 0x0D || (lo >= 0x20 && lo <= 0x7E)) {
                    ++asciiZeroHighCount;
                } else {
                    ++badCtrl;
                }
            }
        }
        return asciiZeroHighCount * 5 >= pairCount && (badCtrl * 32 <= pairCount);
    }

    static std::string buildHexDump(const uint8_t* data, size_t len) {
        std::ostringstream hexStream;
        const size_t limit = (std::min)(len, static_cast<size_t>(512));
        for (size_t i = 0; i < limit; ++i) {
            hexStream << std::hex << std::setw(2) << std::setfill('0') << (static_cast<int>(data[i]) & 0xFF) << " ";
            if ((i + 1) % 16 == 0) hexStream << "\n";
        }
        return hexStream.str();
    }

    static DecodedTextPreview decodeBuffer(const uint8_t* rawData, size_t rawLen, uintmax_t totalFileSize = 0) {
        DecodedTextPreview res;
        res.fileOpened = true;
        res.truncated = (totalFileSize > rawLen && rawLen >= kMaxTextPreviewBytes);

        if (rawLen == 0 || !rawData) {
            res.isReadableText = true;
            res.encoding = "UTF-8";
            return res;
        }

        const uint8_t* ptr = rawData;
        size_t len = rawLen;

        // 0. 已知二进制签名硬拦截（防止 PDF / ZIP / OLE2 等二进制文件被误判为文本产生乱码）
        if (hasKnownBinarySignature(ptr, len)) {
            res.hexDump = buildHexDump(rawData, rawLen);
            return res;
        }

        // 1. 显式 BOM 优先匹配（100% 确定性）
        if (len >= 3 && ptr[0] == 0xEF && ptr[1] == 0xBB && ptr[2] == 0xBF) {
            ptr += 3;
            len -= 3;
            len = trimIncompleteUtf8Tail(ptr, len);
            res.utf8Text.assign(reinterpret_cast<const char*>(ptr), len);
            res.isReadableText = true;
            res.encoding = "UTF-8 BOM";
        } else if (len >= 2 && ptr[0] == 0xFF && ptr[1] == 0xFE) {
            ptr += 2;
            len -= 2;
            const size_t wlen = len / 2;
            std::wstring wide(reinterpret_cast<const wchar_t*>(ptr), wlen);
            res.utf8Text = WinUtils::wstringToUtf8(wide);
            res.isReadableText = true;
            res.encoding = "UTF-16 LE";
        } else if (len >= 2 && ptr[0] == 0xFE && ptr[1] == 0xFF) {
            ptr += 2;
            len -= 2;
            const size_t wlen = len / 2;
            std::wstring wide(wlen, L'\0');
            for (size_t i = 0; i < wlen; ++i) {
                wide[i] = static_cast<wchar_t>((static_cast<uint16_t>(ptr[i * 2]) << 8) | ptr[i * 2 + 1]);
            }
            res.utf8Text = WinUtils::wstringToUtf8(wide);
            res.isReadableText = true;
            res.encoding = "UTF-16 BE";
        }
        // 2. 标准 UTF-8 / ASCII
        else if (isValidUtf8Buffer(ptr, len)) {
            len = trimIncompleteUtf8Tail(ptr, len);
            res.utf8Text.assign(reinterpret_cast<const char*>(ptr), len);
            res.isReadableText = true;
            res.encoding = "UTF-8";
        }
        // 3. 无 BOM 的 UTF-16 LE / BE
        else if (isBomlessUtf16Text(ptr, len, false)) {
            const size_t wlen = len / 2;
            std::wstring wide(reinterpret_cast<const wchar_t*>(ptr), wlen);
            res.utf8Text = WinUtils::wstringToUtf8(wide);
            res.isReadableText = true;
            res.encoding = "UTF-16 LE";
        } else if (isBomlessUtf16Text(ptr, len, true)) {
            const size_t wlen = len / 2;
            std::wstring wide(wlen, L'\0');
            for (size_t i = 0; i < wlen; ++i) {
                wide[i] = static_cast<wchar_t>((static_cast<uint16_t>(ptr[i * 2]) << 8) | ptr[i * 2 + 1]);
            }
            res.utf8Text = WinUtils::wstringToUtf8(wide);
            res.isReadableText = true;
            res.encoding = "UTF-16 BE";
        }
        // 4. GBK / GB18030 (CP936) 编码校验与尾部双字节对齐解码
        else {
            size_t validGbkLen = 0;
            if (validateAndTrimGbkBuffer(ptr, len, validGbkLen)) {
                const UINT cp = IsValidCodePage(54936) ? 54936 : (IsValidCodePage(936) ? 936 : CP_ACP);
                int wideLen = MultiByteToWideChar(
                    cp, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(ptr), static_cast<int>(validGbkLen), nullptr, 0);
                if (wideLen <= 0) {
                    wideLen = MultiByteToWideChar(
                        cp, 0, reinterpret_cast<const char*>(ptr), static_cast<int>(validGbkLen), nullptr, 0);
                }
                if (wideLen > 0) {
                    std::wstring wide(wideLen, L'\0');
                    MultiByteToWideChar(cp, 0, reinterpret_cast<const char*>(ptr), static_cast<int>(validGbkLen), wide.data(), wideLen);
                    res.utf8Text = WinUtils::wstringToUtf8(wide);
                    res.isReadableText = true;
                    res.encoding = "GBK";
                }
            }
        }

        if (!res.isReadableText) {
            res.hexDump = buildHexDump(rawData, rawLen);
            return res;
        }

        if (res.truncated) {
            res.utf8Text += "\n\n// ... [Preview truncated at 256 KB, press Enter to open full file] ...";
        }
        return res;
    }

    static DecodedTextPreview readAndDecodeFile(const std::wstring& filePath, uintmax_t totalFileSize) {
        DecodedTextPreview res;
        HANDLE hFile = CreateFileW(
            filePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            return res;
        }

        const size_t bytesToRead = static_cast<size_t>((std::min)(totalFileSize, static_cast<uintmax_t>(kMaxTextPreviewBytes)));
        if (bytesToRead == 0) {
            CloseHandle(hFile);
            res.fileOpened = true;
            res.isReadableText = true;
            res.encoding = "UTF-8";
            return res;
        }

        std::vector<uint8_t> raw(bytesToRead);
        DWORD bytesRead = 0;
        const BOOL readOk = ReadFile(hFile, raw.data(), static_cast<DWORD>(bytesToRead), &bytesRead, nullptr);
        CloseHandle(hFile);
        if (!readOk || bytesRead == 0) {
            return res;
        }
        raw.resize(bytesRead);
        return decodeBuffer(raw.data(), raw.size(), totalFileSize);
    }
};

} // namespace tools3000::core
