#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// OcrTableReconstructor — OCR 表格结构网格检测与 CF_HTML + TSV 双格式重建引擎
//
// 职责:
//   1. 从 OCR 文本块或含制表符/字符框的行结果中自动识别 >= 2 行 x 2 列 的表格结构
//   2. 执行跨行列簇对齐与空缺单元格对位补齐，防止粘贴至电子表格时发生错列偏移
//   3. 生成符合微软官方剪贴板规范的 CF_HTML (HTML Format) 字节流与 TSV 制表符纯文本
//   4. 单次事务同时写入 CF_HTML 与 CF_UNICODETEXT 系统剪贴板，Excel / WPS / 飞书 Ctrl+V 直粘分列
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_OCR_OCRTABLERECONSTRUCTOR_H
#define TOOLS3000_OCR_OCRTABLERECONSTRUCTOR_H

#include "ocr/OcrEngine.h"

#include <windows.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::ocr {

namespace OcrTableConstants {
    inline constexpr size_t kMinTableRows = 2;
    inline constexpr size_t kMinTableCols = 2;
    inline constexpr size_t kMinMultiColRows = 2;
} // namespace OcrTableConstants

struct OcrTableGrid {
    bool isTable = false;
    size_t rowCount = 0;
    size_t colCount = 0;
    std::vector<std::vector<std::string>> cells; // [row][col] UTF-8 文本

    /// 序列化为制表符分隔纯文本 (TSV)
    std::string toTsv(const char* lineEnding = "\r\n") const;

    /// 序列化为语义纯净、带 HTML 实体转义的 <table>...</table> 片段
    std::string toHtmlTable() const;
};

class OcrTableReconstructor {
public:
    /// 自动分析 OCR 识别结果列表，若满足 >= 2 行 x 2 列对齐特征则重建二维表格网格
    static OcrTableGrid reconstruct(const std::vector<OcrResult>& blocks);

    /// 快速判定一组 OCR 结果是否包含有效表格结构
    static bool hasTableStructure(const std::vector<OcrResult>& blocks);

    /// 将 HTML 片段封装为标准 Windows CF_HTML ("HTML Format") 剪贴板字节流（含精确 10 位十进制字节偏移头）
    static std::string buildCfHtmlClipboardPayload(std::string_view htmlFragment);

    /// HTML 特殊字符安全转义 (&, <, >, ", ')
    static std::string escapeHtml(std::string_view text);

    /// 将重建好的表格以 CF_HTML + CF_UNICODETEXT (TSV) 双格式原子写入 Windows 系统剪贴板
    static bool copyTableToClipboard(const OcrTableGrid& grid, HWND owner = nullptr);
};

} // namespace tools3000::ocr

#endif // TOOLS3000_OCR_OCRTABLERECONSTRUCTOR_H
