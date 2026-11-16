#pragma once
#include "core/utils/Export.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tools3000::core {

struct SpreadsheetSheetPreview {
    std::string name;
    std::vector<std::vector<std::string>> rows;
    bool truncatedRows = false;
    bool truncatedCols = false;
};

struct SpreadsheetPreviewResult {
    bool ok = false;
    bool encryptedOrUnsupported = false;
    bool truncatedSheets = false;
    size_t totalSheetCount = 0;
    std::vector<SpreadsheetSheetPreview> sheets;
};

struct DocumentPreviewResult {
    bool ok = false;
    bool encryptedOrUnsupported = false;
    bool truncated = false;
    std::string markdownText;
};

struct PdfPreviewResult {
    bool ok = false;
    bool passwordProtected = false;
    uint32_t totalPageCount = 0;
    std::vector<std::string> pageDataUris;
};

class TOOLS3000CORE_API OfficeAndPdfPreview {
public:
    static SpreadsheetPreviewResult extractXlsxPreview(
        const std::wstring& filePath,
        size_t maxSheets = 8,
        size_t maxRowsPerSheet = 200,
        size_t maxColsPerRow = 32);

    static DocumentPreviewResult extractDocxOrPptxPreview(
        const std::wstring& filePath,
        const std::string& lowerExt,
        size_t maxChars = 131072);

    static DocumentPreviewResult extractLegacyOfficePreview(
        const std::wstring& filePath,
        size_t maxChars = 131072);

    static PdfPreviewResult renderPdfPreviewPages(
        const std::wstring& filePath,
        uint32_t maxPages = 6,
        uint32_t targetWidthPx = 1240);

    static PdfPreviewResult renderPdfPreviewPagesFromMemory(
        const uint8_t* pdfBytes,
        size_t pdfSize,
        uint32_t maxPages = 6,
        uint32_t targetWidthPx = 1240);

    static PdfPreviewResult renderAiOrEpsPreviewPages(
        const std::wstring& filePath,
        uint32_t maxPages = 6,
        uint32_t targetWidthPx = 1240);
};

} // namespace tools3000::core
