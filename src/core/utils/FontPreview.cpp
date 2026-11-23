#include "core/utils/FontPreview.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace tools3000::core {

namespace {

constexpr size_t kMaxFontReadBytes = 32u * 1024u * 1024u;

uint16_t readBe16(const uint8_t* p) noexcept {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

uint32_t readBe32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

int32_t readBeS32(const uint8_t* p) noexcept {
    return static_cast<int32_t>(readBe32(p));
}

void writeBe16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[1] = static_cast<uint8_t>(v & 0xFFu);
}

void writeBe32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(v & 0xFFu);
}

bool readWholeFileBounded(const std::wstring& filePath, size_t maxBytes, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE h = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    struct Guard {
        HANDLE handle;
        ~Guard() { CloseHandle(handle); }
    } guard{h};

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || static_cast<uint64_t>(sz.QuadPart) > maxBytes) {
        return false;
    }
    const size_t total = static_cast<size_t>(sz.QuadPart);
    out.resize(total);
    size_t readTotal = 0;
    while (readTotal < total) {
        const DWORD chunk = static_cast<DWORD>((std::min)(total - readTotal, static_cast<size_t>(1u << 20)));
        DWORD got = 0;
        if (!ReadFile(h, out.data() + readTotal, chunk, &got, nullptr) || got == 0) {
            out.clear();
            return false;
        }
        readTotal += got;
    }
    return true;
}

std::string decodeUtf16BeToUtf8(const uint8_t* data, size_t byteLen) {
    if (!data || byteLen < 2) return {};
    const size_t count = byteLen / 2;
    std::wstring w(count, L'\0');
    for (size_t i = 0; i < count; ++i) {
        w[i] = static_cast<wchar_t>(readBe16(data + i * 2));
    }
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    return WinUtils::wstringToUtf8(w);
}

struct TableSlice {
    const uint8_t* ptr = nullptr;
    size_t len = 0;
    std::vector<uint8_t> owned;
};

void parseNameTable(const uint8_t* tbl, size_t len, FontPreviewInfo& out) {
    if (!tbl || len < 6) return;
    const uint16_t count = readBe16(tbl + 2);
    const uint16_t stringOffset = readBe16(tbl + 4);
    if (stringOffset >= len) return;

    auto priorityOf = [](uint16_t platformId, uint16_t langId) -> int {
        if (platformId == 3 && (langId == 0x0804 || langId == 0x0409)) {
            return (langId == 0x0804) ? 100 : 90;
        }
        if (platformId == 3) return 70;
        if (platformId == 0) return 60;
        if (platformId == 1 && langId == 0) return 40;
        return 10;
    };

    std::unordered_map<uint16_t, std::pair<int, std::string>> bestById;
    for (uint16_t i = 0; i < count; ++i) {
        const size_t recOff = 6u + static_cast<size_t>(i) * 12u;
        if (recOff + 12 > len) break;
        const uint16_t platformId = readBe16(tbl + recOff);
        const uint16_t encodingId = readBe16(tbl + recOff + 2);
        const uint16_t langId = readBe16(tbl + recOff + 4);
        const uint16_t nameId = readBe16(tbl + recOff + 6);
        const uint16_t strLen = readBe16(tbl + recOff + 8);
        const uint16_t strOff = readBe16(tbl + recOff + 10);

        if (nameId != 0 && nameId != 1 && nameId != 2 && nameId != 4 &&
            nameId != 5 && nameId != 9 && nameId != 16 && nameId != 17) {
            continue;
        }
        const size_t absOff = static_cast<size_t>(stringOffset) + strOff;
        if (absOff + strLen > len || strLen == 0) continue;

        std::string decoded;
        if (platformId == 3 || platformId == 0) {
            decoded = decodeUtf16BeToUtf8(tbl + absOff, strLen);
        } else if (platformId == 1 && encodingId == 0) {
            decoded.assign(reinterpret_cast<const char*>(tbl + absOff), strLen);
        }
        if (decoded.empty()) continue;

        const int prio = priorityOf(platformId, langId);
        auto it = bestById.find(nameId);
        if (it == bestById.end() || prio > it->second.first) {
            bestById[nameId] = {prio, std::move(decoded)};
        }
    }

    auto pick = [&](uint16_t primary, uint16_t fallback = 0xFFFFu) -> std::string {
        if (auto it = bestById.find(primary); it != bestById.end()) return it->second.second;
        if (fallback != 0xFFFFu) {
            if (auto it = bestById.find(fallback); it != bestById.end()) return it->second.second;
        }
        return {};
    };

    if (out.familyName.empty()) out.familyName = pick(16, 1);
    if (out.subfamilyName.empty()) out.subfamilyName = pick(17, 2);
    if (out.fullName.empty()) out.fullName = pick(4, 1);
    if (out.version.empty()) out.version = pick(5);
    if (out.copyright.empty()) out.copyright = pick(0);
    if (out.designer.empty()) out.designer = pick(9);
}

void parseSfntTables(const std::unordered_map<uint32_t, TableSlice>& tables, FontPreviewInfo& out) {
    auto findTbl = [&](const char tag[4]) -> const TableSlice* {
        const uint32_t key = (static_cast<uint32_t>(static_cast<uint8_t>(tag[0])) << 24) |
                             (static_cast<uint32_t>(static_cast<uint8_t>(tag[1])) << 16) |
                             (static_cast<uint32_t>(static_cast<uint8_t>(tag[2])) << 8) |
                             static_cast<uint32_t>(static_cast<uint8_t>(tag[3]));
        auto it = tables.find(key);
        return it == tables.end() ? nullptr : &it->second;
    };

    if (const auto* nameTbl = findTbl("name")) {
        parseNameTable(nameTbl->ptr, nameTbl->len, out);
    }
    if (const auto* maxpTbl = findTbl("maxp"); maxpTbl && maxpTbl->len >= 6) {
        out.glyphCount = readBe16(maxpTbl->ptr + 4);
    }
    if (const auto* os2Tbl = findTbl("OS/2"); os2Tbl && os2Tbl->len >= 64) {
        out.weightClass = readBe16(os2Tbl->ptr + 4);
        const uint16_t fsSelection = readBe16(os2Tbl->ptr + 62);
        out.isItalic = (fsSelection & 0x0001u) != 0;
        if (os2Tbl->len >= 58) {
            // Unicode Range 2 (bits 48..63) 包含 CJK 表意文字 (bit 59 -> ulUnicodeRange2 bit 27)
            const uint32_t range2 = readBe32(os2Tbl->ptr + 46);
            if ((range2 & (1u << 27)) != 0 || (range2 & (1u << 17)) != 0 || (range2 & (1u << 24)) != 0) {
                out.hasCjkSupport = true;
            }
        }
    }
    if (const auto* fvarTbl = findTbl("fvar"); fvarTbl && fvarTbl->len >= 16) {
        const uint16_t axisOffset = readBe16(fvarTbl->ptr + 4);
        const uint16_t axisCount = readBe16(fvarTbl->ptr + 8);
        const uint16_t axisSize = readBe16(fvarTbl->ptr + 10);
        if (axisCount > 0 && axisSize >= 20) {
            out.isVariable = true;
            for (uint16_t i = 0; i < axisCount && i < 16; ++i) {
                const size_t off = static_cast<size_t>(axisOffset) + static_cast<size_t>(i) * axisSize;
                if (off + 20 > fvarTbl->len) break;
                FontAxisInfo axis;
                axis.tag.assign(reinterpret_cast<const char*>(fvarTbl->ptr + off), 4);
                axis.minValue = static_cast<float>(readBeS32(fvarTbl->ptr + off + 4)) / 65536.0f;
                axis.defaultValue = static_cast<float>(readBeS32(fvarTbl->ptr + off + 8)) / 65536.0f;
                axis.maxValue = static_cast<float>(readBeS32(fvarTbl->ptr + off + 12)) / 65536.0f;
                out.axes.push_back(std::move(axis));
            }
        }
    }
}

bool parseSfntAtOffset(const std::vector<uint8_t>& bytes, size_t offset, FontPreviewInfo& out) {
    if (offset + 12 > bytes.size()) return false;
    const uint32_t sfntVer = readBe32(bytes.data() + offset);
    if (sfntVer != 0x00010000u && sfntVer != 0x4F54544Fu && sfntVer != 0x74727565u) {
        return false;
    }
    const uint16_t numTables = readBe16(bytes.data() + offset + 4);
    if (offset + 12u + static_cast<size_t>(numTables) * 16u > bytes.size()) {
        return false;
    }

    std::unordered_map<uint32_t, TableSlice> tables;
    for (uint16_t i = 0; i < numTables; ++i) {
        const size_t rec = offset + 12u + static_cast<size_t>(i) * 16u;
        const uint32_t tag = readBe32(bytes.data() + rec);
        const uint32_t tblOff = readBe32(bytes.data() + rec + 8);
        const uint32_t tblLen = readBe32(bytes.data() + rec + 12);
        if (static_cast<size_t>(tblOff) + tblLen <= bytes.size()) {
            TableSlice s;
            s.ptr = bytes.data() + tblOff;
            s.len = tblLen;
            tables.emplace(tag, std::move(s));
        }
    }
    parseSfntTables(tables, out);
    return true;
}

bool parseWoff1(const std::vector<uint8_t>& bytes, FontPreviewInfo& out) {
    if (bytes.size() < 44 || readBe32(bytes.data()) != 0x774F4646u) return false;
    const uint16_t numTables = readBe16(bytes.data() + 12);
    if (44u + static_cast<size_t>(numTables) * 20u > bytes.size()) return false;

    std::unordered_map<uint32_t, TableSlice> tables;
    for (uint16_t i = 0; i < numTables; ++i) {
        const size_t rec = 44u + static_cast<size_t>(i) * 20u;
        const uint32_t tag = readBe32(bytes.data() + rec);
        if (tag != 0x6E616D65u && tag != 0x6D617870u && tag != 0x4F532F32u && tag != 0x66766172u) {
            continue;
        }
        const uint32_t off = readBe32(bytes.data() + rec + 4);
        const uint32_t compLen = readBe32(bytes.data() + rec + 8);
        const uint32_t origLen = readBe32(bytes.data() + rec + 12);
        if (static_cast<size_t>(off) + compLen > bytes.size() || origLen > 8u * 1024u * 1024u) {
            continue;
        }
        TableSlice s;
        if (compLen == origLen) {
            s.ptr = bytes.data() + off;
            s.len = origLen;
        } else {
            s.owned.resize(origLen);
            uLongf dstLen = origLen;
            if (uncompress(s.owned.data(), &dstLen, bytes.data() + off, compLen) != Z_OK) {
                continue;
            }
            s.ptr = s.owned.data();
            s.len = dstLen;
        }
        tables.emplace(tag, std::move(s));
    }
    parseSfntTables(tables, out);
    return true;
}

std::string readDWriteLocalizedString(IDWriteLocalizedStrings* strings) {
    if (!strings) return {};
    UINT32 count = strings->GetCount();
    if (count == 0) return {};

    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (FAILED(strings->FindLocaleName(L"zh-CN", &idx, &exists)) || !exists) {
        if (FAILED(strings->FindLocaleName(L"en-US", &idx, &exists)) || !exists) {
            idx = 0;
        }
    }
    UINT32 len = 0;
    if (FAILED(strings->GetStringLength(idx, &len)) || len == 0) return {};
    std::wstring buf(len + 1, L'\0');
    if (FAILED(strings->GetString(idx, buf.data(), len + 1))) return {};
    buf.resize(len);
    return WinUtils::wstringToUtf8(buf);
}

void enrichWithDirectWrite(const std::vector<uint8_t>& bytes, FontPreviewInfo& out) {
    ComPtr<IDWriteFactory5> factory5;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory5),
            reinterpret_cast<IUnknown**>(factory5.GetAddressOf()))) || !factory5) {
        return;
    }

    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    if (FAILED(factory5->CreateInMemoryFontFileLoader(&loader)) || !loader) return;
    if (FAILED(factory5->RegisterFontFileLoader(loader.Get()))) return;
    struct LoaderGuard {
        IDWriteFactory5* f;
        IDWriteFontFileLoader* l;
        ~LoaderGuard() { f->UnregisterFontFileLoader(l); }
    } loaderGuard{factory5.Get(), loader.Get()};

    ComPtr<IDWriteFontFile> fontFile;
    if (FAILED(loader->CreateInMemoryFontFileReference(
            factory5.Get(),
            bytes.data(),
            static_cast<UINT32>(bytes.size()),
            nullptr,
            &fontFile)) || !fontFile) {
        return;
    }

    BOOL isSupported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE faceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    UINT32 numFaces = 0;
    if (FAILED(fontFile->Analyze(&isSupported, &fileType, &faceType, &numFaces)) || !isSupported || numFaces == 0) {
        return;
    }

    out.ok = true;
    out.faceCount = numFaces;

    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(factory5->CreateFontSetBuilder(&builder)) || !builder) return;
    if (FAILED(builder->AddFontFile(fontFile.Get()))) return;

    ComPtr<IDWriteFontSet> fontSet;
    if (FAILED(builder->CreateFontSet(&fontSet)) || !fontSet) return;
    const UINT32 setCount = fontSet->GetFontCount();
    if (setCount == 0) return;

    auto getPropStr = [&](UINT32 idx, DWRITE_FONT_PROPERTY_ID propId) -> std::string {
        ComPtr<IDWriteLocalizedStrings> str;
        BOOL exists = FALSE;
        if (SUCCEEDED(fontSet->GetPropertyValues(idx, propId, &exists, &str)) && exists && str) {
            return readDWriteLocalizedString(str.Get());
        }
        return {};
    };

    if (out.familyName.empty()) out.familyName = getPropStr(0, DWRITE_FONT_PROPERTY_ID_PREFERRED_FAMILY_NAME);
    if (out.familyName.empty()) out.familyName = getPropStr(0, DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME);
    if (out.fullName.empty()) out.fullName = getPropStr(0, DWRITE_FONT_PROPERTY_ID_FULL_NAME);
    if (out.subfamilyName.empty()) out.subfamilyName = getPropStr(0, DWRITE_FONT_PROPERTY_ID_FACE_NAME);

    if (out.faceNames.empty() && setCount > 1) {
        for (UINT32 i = 0; i < setCount && i < 32; ++i) {
            std::string fn = getPropStr(i, DWRITE_FONT_PROPERTY_ID_FULL_NAME);
            if (fn.empty()) fn = getPropStr(i, DWRITE_FONT_PROPERTY_ID_PREFERRED_FAMILY_NAME);
            if (fn.empty()) fn = "Face #" + std::to_string(i + 1);
            out.faceNames.push_back(std::move(fn));
        }
    }

    ComPtr<IDWriteFontFaceReference> faceRef;
    if (SUCCEEDED(fontSet->GetFontFaceReference(0, &faceRef)) && faceRef) {
        ComPtr<IDWriteFontFace3> face3;
        if (SUCCEEDED(faceRef->CreateFontFace(&face3)) && face3) {
            if (out.designer.empty()) {
                ComPtr<IDWriteLocalizedStrings> designerStr;
                BOOL exists = FALSE;
                if (SUCCEEDED(face3->GetInformationalStrings(
                        DWRITE_INFORMATIONAL_STRING_DESIGNER, &designerStr, &exists)) &&
                    exists && designerStr) {
                    out.designer = readDWriteLocalizedString(designerStr.Get());
                }
            }
            if (out.glyphCount == 0) {
                out.glyphCount = face3->GetGlyphCount();
            }
            if (out.weightClass == 400) {
                out.weightClass = static_cast<uint32_t>(face3->GetWeight());
            }
            out.isItalic = out.isItalic || (face3->GetStyle() != DWRITE_FONT_STYLE_NORMAL);
            // 检测常见汉字“永”“中”以确认是否覆盖 CJK
            if (!out.hasCjkSupport) {
                const UINT32 testCp[2] = {0x6C38u, 0x4E2Du};
                UINT16 indices[2]{};
                if (SUCCEEDED(face3->GetGlyphIndices(testCp, 2, indices)) &&
                    (indices[0] != 0 || indices[1] != 0)) {
                    out.hasCjkSupport = true;
                }
            }
        }
    }
}

}  // namespace

FontPreviewInfo FontPreview::inspectFont(const std::wstring& filePath) {
    FontPreviewInfo info;
    std::vector<uint8_t> bytes;
    if (!readWholeFileBounded(filePath, kMaxFontReadBytes, bytes) || bytes.size() < 12) {
        return info;
    }

    const uint32_t magic = readBe32(bytes.data());
    if (magic == 0x774F4646u) {
        info.format = "WOFF";
        info.ok = parseWoff1(bytes, info);
    } else if (magic == 0x774F4632u) {
        info.format = "WOFF2";
        // WOFF2 由 DirectWrite 5 内存加载器解析元数据，前端 FontFace 原生渲染
        info.ok = true;
    } else if (magic == 0x74746366u) {
        info.format = "TTC";
        if (bytes.size() >= 16) {
            const uint32_t numFaces = readBe32(bytes.data() + 8);
            info.faceCount = numFaces;
            if (numFaces > 0 && 12u + static_cast<size_t>(numFaces) * 4u <= bytes.size()) {
                for (uint32_t i = 0; i < numFaces && i < 32; ++i) {
                    const uint32_t faceOff = readBe32(bytes.data() + 12u + i * 4u);
                    FontPreviewInfo sub;
                    if (parseSfntAtOffset(bytes, faceOff, sub)) {
                        if (i == 0) {
                            info.familyName = sub.familyName;
                            info.subfamilyName = sub.subfamilyName;
                            info.fullName = sub.fullName;
                            info.version = sub.version;
                            info.copyright = sub.copyright;
                            info.designer = sub.designer;
                            info.glyphCount = sub.glyphCount;
                            info.weightClass = sub.weightClass;
                            info.isItalic = sub.isItalic;
                            info.isVariable = sub.isVariable;
                            info.hasCjkSupport = sub.hasCjkSupport;
                            info.axes = sub.axes;
                            info.ok = true;
                        }
                        std::string label = !sub.fullName.empty() ? sub.fullName : sub.familyName;
                        if (label.empty()) label = "Face #" + std::to_string(i + 1);
                        info.faceNames.push_back(std::move(label));
                    }
                }
            }
        }
    } else if (magic == 0x4F54544Fu) {
        info.format = "OTF";
        info.ok = parseSfntAtOffset(bytes, 0, info);
    } else if (magic == 0x00010000u || magic == 0x74727565u) {
        info.format = "TTF";
        info.ok = parseSfntAtOffset(bytes, 0, info);
    }

    enrichWithDirectWrite(bytes, info);

    if (info.familyName.empty()) {
        std::filesystem::path p(filePath);
        info.familyName = WinUtils::wstringToUtf8(p.stem().wstring());
    }
    if (info.fullName.empty()) {
        info.fullName = info.familyName;
    }
    if (info.format.empty()) {
        const std::wstring ext = WinUtils::toLower(std::filesystem::path(filePath).extension().wstring());
        if (ext == L".woff2") info.format = "WOFF2";
        else if (ext == L".woff") info.format = "WOFF";
        else if (ext == L".otf") info.format = "OTF";
        else if (ext == L".ttc") info.format = "TTC";
        else info.format = "TTF";
    }
    return info;
}

bool FontPreview::extractStandaloneTtfFromTtc(const std::wstring& ttcPath,
                                              uint32_t faceIndex,
                                              std::vector<uint8_t>& outTtfBytes) {
    outTtfBytes.clear();
    std::vector<uint8_t> bytes;
    if (!readWholeFileBounded(ttcPath, kMaxFontReadBytes, bytes) || bytes.size() < 16) {
        return false;
    }
    if (readBe32(bytes.data()) != 0x74746366u) {
        return false;
    }
    const uint32_t numFaces = readBe32(bytes.data() + 8);
    if (faceIndex >= numFaces || 12u + static_cast<size_t>(numFaces) * 4u > bytes.size()) {
        return false;
    }
    const uint32_t faceOff = readBe32(bytes.data() + 12u + static_cast<size_t>(faceIndex) * 4u);
    if (static_cast<size_t>(faceOff) + 12u > bytes.size()) {
        return false;
    }

    const uint16_t numTables = readBe16(bytes.data() + faceOff + 4);
    if (numTables == 0 || static_cast<size_t>(faceOff) + 12u + static_cast<size_t>(numTables) * 16u > bytes.size()) {
        return false;
    }

    struct RawTable {
        uint32_t tag;
        uint32_t checksum;
        uint32_t srcOffset;
        uint32_t length;
    };
    std::vector<RawTable> tables;
    tables.reserve(numTables);
    size_t totalDataBytes = 0;
    for (uint16_t i = 0; i < numTables; ++i) {
        const size_t rec = static_cast<size_t>(faceOff) + 12u + static_cast<size_t>(i) * 16u;
        RawTable t{
            readBe32(bytes.data() + rec),
            readBe32(bytes.data() + rec + 4),
            readBe32(bytes.data() + rec + 8),
            readBe32(bytes.data() + rec + 12)
        };
        if (static_cast<size_t>(t.srcOffset) + t.length > bytes.size()) {
            return false;
        }
        totalDataBytes += (static_cast<size_t>(t.length) + 3u) & ~static_cast<size_t>(3u);
        tables.push_back(t);
    }

    const size_t headerSize = 12u + static_cast<size_t>(numTables) * 16u;
    outTtfBytes.assign(headerSize + totalDataBytes, 0);
    // 复制原始 12 字节 Offset Table
    std::memcpy(outTtfBytes.data(), bytes.data() + faceOff, 12);

    uint32_t writeCursor = static_cast<uint32_t>(headerSize);
    for (uint16_t i = 0; i < numTables; ++i) {
        const auto& t = tables[i];
        const size_t recOut = 12u + static_cast<size_t>(i) * 16u;
        writeBe32(outTtfBytes.data() + recOut, t.tag);
        writeBe32(outTtfBytes.data() + recOut + 4, t.checksum);
        writeBe32(outTtfBytes.data() + recOut + 8, writeCursor);
        writeBe32(outTtfBytes.data() + recOut + 12, t.length);
        if (t.length > 0) {
            std::memcpy(outTtfBytes.data() + writeCursor, bytes.data() + t.srcOffset, t.length);
        }
        writeCursor += (t.length + 3u) & ~3u;
    }
    return true;
}

}  // namespace tools3000::core
