#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// FontPreview.h — 零膨胀字体元数据解析与 TTC 子字面按需抽离器 (Tools3000Core)
//
// 支持格式: .ttf / .otf / .woff / .woff2 / .ttc
// 核心能力:
//   1. 零安装、零文件锁提取字体家族名、样式/字重、版本、版权、字形数、可变轴与子字面列表；
//   2. 将 Chromium 无法直接加载的 TrueType Collection (.ttc) 按 ttcf 表无损抽取为独立 .ttf 内存流，
//      供前端原生 FontFace API 实时渲染瀑布流与试打字。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_UTILS_FONTPREVIEW_H
#define TOOLS3000_CORE_UTILS_FONTPREVIEW_H

#include "core/utils/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tools3000::core {

struct FontAxisInfo {
    std::string tag;
    float minValue = 0.0f;
    float defaultValue = 0.0f;
    float maxValue = 0.0f;
};

struct FontPreviewInfo {
    bool ok = false;
    std::string format;           ///< "TTF", "OTF", "WOFF", "WOFF2", "TTC"
    std::string familyName;
    std::string subfamilyName;
    std::string fullName;
    std::string version;
    std::string copyright;
    std::string designer;
    uint32_t glyphCount = 0;
    uint32_t weightClass = 400;
    bool isItalic = false;
    bool isVariable = false;
    bool hasCjkSupport = false;
    uint32_t faceCount = 1;
    std::vector<std::string> faceNames;
    std::vector<FontAxisInfo> axes;
};

class TOOLS3000CORE_API FontPreview {
public:
    /// 零锁读取字体元数据（结合原生 sfnt/wOFF 解析与 DirectWrite InMemoryFontFileLoader）
    static FontPreviewInfo inspectFont(const std::wstring& filePath);

    /// 从 .ttc 集合文件中提取指定索引的独立 .ttf 字节流（零磁盘临时文件，供 WebView2 FontFace 直接加载）
    static bool extractStandaloneTtfFromTtc(const std::wstring& ttcPath,
                                            uint32_t faceIndex,
                                            std::vector<uint8_t>& outTtfBytes);
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_UTILS_FONTPREVIEW_H
