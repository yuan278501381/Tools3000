// ─────────────────────────────────────────────────────────────────────────────
// OcrTableReconstructor.cpp — OCR 表格结构网格检测与 CF_HTML + TSV 双格式重建实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/OcrTableReconstructor.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace tools3000::ocr {

namespace {

struct CellToken {
    std::string text;
    cv::Rect box;
    float left() const noexcept { return static_cast<float>(box.x); }
    float right() const noexcept { return static_cast<float>(box.x + (std::max)(1, box.width)); }
    float cx() const noexcept { return static_cast<float>(box.x) + static_cast<float>((std::max)(1, box.width)) * 0.5f; }
    float cy() const noexcept { return static_cast<float>(box.y) + static_cast<float>((std::max)(1, box.height)) * 0.5f; }
    float h() const noexcept { return static_cast<float>((std::max)(1, box.height)); }
};

std::string trimAsciiWhitespace(std::string_view sv) {
    size_t a = 0;
    while (a < sv.size() && (sv[a] == ' ' || sv[a] == '\t' || sv[a] == '\r' || sv[a] == '\n')) {
        ++a;
    }
    size_t b = sv.size();
    while (b > a && (sv[b - 1] == ' ' || sv[b - 1] == '\t' || sv[b - 1] == '\r' || sv[b - 1] == '\n')) {
        --b;
    }
    return std::string(sv.substr(a, b - a));
}

std::string sanitizeTsvCell(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char ch : sv) {
        if (ch == '\t' || ch == '\r' || ch == '\n') {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return trimAsciiWhitespace(out);
}

std::vector<CellToken> expandBlocksToCellTokens(const std::vector<OcrResult>& blocks) {
    std::vector<CellToken> tokens;
    tokens.reserve(blocks.size() * 2);

    for (const auto& blk : blocks) {
        if (blk.text.empty()) continue;

        if (blk.text.find('\t') == std::string::npos) {
            std::string cleaned = trimAsciiWhitespace(blk.text);
            if (!cleaned.empty()) {
                cv::Rect safeBox = blk.box;
                if (safeBox.width <= 0) safeBox.width = 1;
                if (safeBox.height <= 0) safeBox.height = 16;
                tokens.push_back(CellToken{std::move(cleaned), safeBox});
            }
            continue;
        }

        const auto cps = OcrEngine::splitUtf8Codepoints(blk.text);
        std::vector<cv::Rect> cboxes = blk.charBoxes;
        if (cboxes.size() != cps.size()) {
            cboxes = OcrEngine::buildProportionalCharBoxes(blk.text, blk.box);
        }

        std::string curSeg;
        int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
        bool hasRect = false;

        auto flushSegment = [&]() {
            std::string cleaned = trimAsciiWhitespace(curSeg);
            if (!cleaned.empty()) {
                cv::Rect segBox = hasRect
                    ? cv::Rect(minX, minY, (std::max)(1, maxX - minX), (std::max)(1, maxY - minY))
                    : blk.box;
                tokens.push_back(CellToken{std::move(cleaned), segBox});
            }
            curSeg.clear();
            minX = INT_MAX;
            minY = INT_MAX;
            maxX = INT_MIN;
            maxY = INT_MIN;
            hasRect = false;
        };

        for (size_t i = 0; i < cps.size(); ++i) {
            if (cps[i] == "\t") {
                flushSegment();
                continue;
            }
            curSeg += cps[i];
            if (i < cboxes.size() && cps[i] != " ") {
                const cv::Rect& cb = cboxes[i];
                minX = (std::min)(minX, cb.x);
                minY = (std::min)(minY, cb.y);
                maxX = (std::max)(maxX, cb.x + (std::max)(1, cb.width));
                maxY = (std::max)(maxY, cb.y + (std::max)(1, cb.height));
                hasRect = true;
            }
        }
        flushSegment();
    }

    return tokens;
}

struct VisualRow {
    std::vector<CellToken> cells;
    float sumCy = 0.0f;
    float sumH = 0.0f;
    float avgCy() const noexcept {
        return cells.empty() ? 0.0f : sumCy / static_cast<float>(cells.size());
    }
    float avgH() const noexcept {
        return cells.empty() ? 16.0f : sumH / static_cast<float>(cells.size());
    }
};

struct ColumnCluster {
    float minLeft = 0.0f;
    float maxRight = 0.0f;
    float sumLeft = 0.0f;
    float sumCenter = 0.0f;
    size_t count = 0;

    float avgLeft() const noexcept { return count > 0 ? sumLeft / static_cast<float>(count) : minLeft; }
    float avgCenter() const noexcept { return count > 0 ? sumCenter / static_cast<float>(count) : (minLeft + maxRight) * 0.5f; }

    void absorb(const CellToken& tok) noexcept {
        if (count == 0) {
            minLeft = tok.left();
            maxRight = tok.right();
        } else {
            minLeft = (std::min)(minLeft, tok.left());
            maxRight = (std::max)(maxRight, tok.right());
        }
        sumLeft += tok.left();
        sumCenter += tok.cx();
        ++count;
    }
};

float computeHorizontalMatchCost(const CellToken& tok, const ColumnCluster& col, float medH) noexcept {
    const float overlapLeft = (std::max)(tok.left(), col.minLeft);
    const float overlapRight = (std::min)(tok.right(), col.maxRight);
    const float overlap = overlapRight - overlapLeft;
    const float minSpan = (std::max)(1.0f, (std::min)(tok.right() - tok.left(), col.maxRight - col.minLeft));

    const float leftDiff = std::abs(tok.left() - col.avgLeft());
    const float centerDiff = std::abs(tok.cx() - col.avgCenter());

    if (overlap > minSpan * 0.20f) {
        return (std::min)(leftDiff, centerDiff) * 0.5f;
    }
    const float minAlignDist = (std::min)(leftDiff, centerDiff);
    if (minAlignDist <= (std::max)(18.0f, medH * 1.85f)) {
        return minAlignDist;
    }
    return FLT_MAX;
}

} // namespace

std::string OcrTableGrid::toTsv(const char* lineEnding) const {
    if (!isTable || cells.empty() || colCount == 0) return {};
    const char* eol = (lineEnding != nullptr) ? lineEnding : "\r\n";

    std::string out;
    for (size_t r = 0; r < cells.size(); ++r) {
        if (r > 0) out += eol;
        const auto& row = cells[r];
        for (size_t c = 0; c < colCount; ++c) {
            if (c > 0) out.push_back('\t');
            if (c < row.size()) {
                out += sanitizeTsvCell(row[c]);
            }
        }
    }
    return out;
}

std::string OcrTableGrid::toHtmlTable() const {
    if (!isTable || cells.empty() || colCount == 0) return {};

    std::string html = "<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" style=\"border-collapse:collapse;\"><tbody>";
    for (size_t r = 0; r < cells.size(); ++r) {
        html += "<tr>";
        const auto& row = cells[r];
        for (size_t c = 0; c < colCount; ++c) {
            html += "<td>";
            if (c < row.size()) {
                html += OcrTableReconstructor::escapeHtml(sanitizeTsvCell(row[c]));
            }
            html += "</td>";
        }
        html += "</tr>";
    }
    html += "</tbody></table>";
    return html;
}

std::string OcrTableReconstructor::escapeHtml(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for (char ch : text) {
        switch (ch) {
            case '&':  escaped += "&amp;";  break;
            case '<':  escaped += "&lt;";   break;
            case '>':  escaped += "&gt;";   break;
            case '"':  escaped += "&quot;"; break;
            case '\'': escaped += "&#39;";  break;
            default:   escaped.push_back(ch); break;
        }
    }
    return escaped;
}

std::string OcrTableReconstructor::buildCfHtmlClipboardPayload(std::string_view htmlFragment) {
    // Windows CF_HTML 标准固定宽度 10 位十进制字节偏移头
    static constexpr const char* kHeaderTemplate =
        "Version:0.9\r\n"
        "StartHTML:%010zu\r\n"
        "EndHTML:%010zu\r\n"
        "StartFragment:%010zu\r\n"
        "EndFragment:%010zu\r\n";

    char probeHeader[256]{};
    const int headerLenInt = std::snprintf(probeHeader, sizeof(probeHeader), kHeaderTemplate,
                                           size_t{0}, size_t{0}, size_t{0}, size_t{0});
    const size_t headerLen = headerLenInt > 0 ? static_cast<size_t>(headerLenInt) : 0;

    static constexpr std::string_view kDocPrefix = "<html>\r\n<body>\r\n<!--StartFragment-->";
    static constexpr std::string_view kDocSuffix = "<!--EndFragment-->\r\n</body>\r\n</html>";

    const size_t startHtml = headerLen;
    const size_t startFragment = startHtml + kDocPrefix.size();
    const size_t endFragment = startFragment + htmlFragment.size();
    const size_t endHtml = endFragment + kDocSuffix.size();

    char finalHeader[256]{};
    std::snprintf(finalHeader, sizeof(finalHeader), kHeaderTemplate,
                  startHtml, endHtml, startFragment, endFragment);

    std::string payload;
    payload.reserve(endHtml + 4);
    payload.append(finalHeader, headerLen);
    payload.append(kDocPrefix);
    payload.append(htmlFragment);
    payload.append(kDocSuffix);
    return payload;
}

OcrTableGrid OcrTableReconstructor::reconstruct(const std::vector<OcrResult>& blocks) {
    OcrTableGrid result{};
    std::vector<CellToken> tokens = expandBlocksToCellTokens(blocks);
    if (tokens.size() < OcrTableConstants::kMinTableRows * OcrTableConstants::kMinTableCols) {
        return result;
    }

    // 计算中位高度作为自适应几何尺度基准
    std::vector<float> heights;
    heights.reserve(tokens.size());
    for (const auto& t : tokens) heights.push_back(t.h());
    std::nth_element(heights.begin(), heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2), heights.end());
    const float medH = (std::max)(8.0f, heights[heights.size() / 2]);

    // 1. 按 Y 轴中心聚类为视觉行
    std::sort(tokens.begin(), tokens.end(), [](const CellToken& a, const CellToken& b) {
        return a.cy() < b.cy();
    });

    std::vector<VisualRow> rows;
    for (auto& tok : tokens) {
        int bestRow = -1;
        float bestDy = FLT_MAX;
        for (int i = static_cast<int>(rows.size()) - 1; i >= 0; --i) {
            const float refH = (std::max)(8.0f, (std::min)(rows[i].avgH(), tok.h()));
            const float dy = std::abs(tok.cy() - rows[i].avgCy());
            if (dy <= refH * 0.60f && dy < bestDy) {
                bestDy = dy;
                bestRow = i;
            }
        }
        if (bestRow >= 0) {
            rows[bestRow].sumCy += tok.cy();
            rows[bestRow].sumH += tok.h();
            rows[bestRow].cells.push_back(std::move(tok));
        } else {
            VisualRow vr;
            vr.sumCy = tok.cy();
            vr.sumH = tok.h();
            vr.cells.push_back(std::move(tok));
            rows.push_back(std::move(vr));
        }
    }

    if (rows.size() < OcrTableConstants::kMinTableRows) {
        return result;
    }

    std::sort(rows.begin(), rows.end(), [](const VisualRow& a, const VisualRow& b) {
        return a.avgCy() < b.avgCy();
    });

    // 行内从左到右排序，并合并同一单元格内部紧邻的碎词 (间距 < medH * 1.15f)
    size_t multiColRowCount = 0;
    size_t seedRowIdx = 0;
    size_t maxRowCols = 0;

    for (size_t r = 0; r < rows.size(); ++r) {
        auto& rowCells = rows[r].cells;
        std::sort(rowCells.begin(), rowCells.end(), [](const CellToken& a, const CellToken& b) {
            return a.left() < b.left();
        });

        std::vector<CellToken> mergedCells;
        mergedCells.reserve(rowCells.size());
        for (auto& c : rowCells) {
            if (!mergedCells.empty()) {
                auto& prev = mergedCells.back();
                const float gap = c.left() - prev.right();
                const float localH = (std::max)(8.0f, (prev.h() + c.h()) * 0.5f);
                if (gap < localH * 0.95f) {
                    prev.text += ' ';
                    prev.text += c.text;
                    const int nx1 = (std::min)(prev.box.x, c.box.x);
                    const int ny1 = (std::min)(prev.box.y, c.box.y);
                    const int nx2 = (std::max)(prev.box.x + prev.box.width, c.box.x + c.box.width);
                    const int ny2 = (std::max)(prev.box.y + prev.box.height, c.box.y + c.box.height);
                    prev.box = cv::Rect(nx1, ny1, nx2 - nx1, ny2 - ny1);
                    continue;
                }
            }
            mergedCells.push_back(std::move(c));
        }
        rowCells = std::move(mergedCells);

        if (rowCells.size() >= OcrTableConstants::kMinTableCols) {
            ++multiColRowCount;
            if (rowCells.size() > maxRowCols) {
                maxRowCols = rowCells.size();
                seedRowIdx = r;
            }
        }
    }

    if (multiColRowCount < OcrTableConstants::kMinMultiColRows || maxRowCols < OcrTableConstants::kMinTableCols) {
        return result;
    }

    // 2. 以列最完整的多列行初始化列簇，再融合所有多列行校准全局列坐标
    std::vector<ColumnCluster> columns;
    columns.reserve(maxRowCols + 4);
    for (const auto& seedCell : rows[seedRowIdx].cells) {
        ColumnCluster cc;
        cc.absorb(seedCell);
        columns.push_back(cc);
    }

    for (size_t r = 0; r < rows.size(); ++r) {
        if (r == seedRowIdx || rows[r].cells.size() < 2) continue;
        std::vector<bool> colUsed(columns.size(), false);
        for (const auto& cell : rows[r].cells) {
            int bestCol = -1;
            float bestCost = FLT_MAX;
            for (size_t c = 0; c < columns.size(); ++c) {
                if (colUsed[c]) continue;
                const float cost = computeHorizontalMatchCost(cell, columns[c], medH);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestCol = static_cast<int>(c);
                }
            }
            if (bestCol >= 0) {
                columns[static_cast<size_t>(bestCol)].absorb(cell);
                colUsed[static_cast<size_t>(bestCol)] = true;
            } else if (columns.size() < 32) {
                ColumnCluster newCol;
                newCol.absorb(cell);
                auto insertIt = std::lower_bound(
                    columns.begin(), columns.end(), newCol.avgCenter(),
                    [](const ColumnCluster& existing, float cx) {
                        return existing.avgCenter() < cx;
                    });
                const size_t insIdx = static_cast<size_t>(std::distance(columns.begin(), insertIt));
                columns.insert(insertIt, newCol);
                colUsed.insert(colUsed.begin() + static_cast<std::ptrdiff_t>(insIdx), true);
            }
        }
    }

    std::sort(columns.begin(), columns.end(), [](const ColumnCluster& a, const ColumnCluster& b) {
        return a.avgCenter() < b.avgCenter();
    });

    // 验证至少有 2 个列簇在 >= 2 行中稳定出现（排除偶然断句误判）
    size_t stableCols = 0;
    for (const auto& col : columns) {
        if (col.count >= 2) ++stableCols;
    }
    if (columns.size() < OcrTableConstants::kMinTableCols || stableCols < OcrTableConstants::kMinTableCols) {
        return result;
    }

    // 3. 将全部行对位填入二维表格矩阵 (保留空白缺省格)
    const size_t numRows = rows.size();
    const size_t numCols = columns.size();
    std::vector<std::vector<std::string>> grid(numRows, std::vector<std::string>(numCols));

    for (size_t r = 0; r < numRows; ++r) {
        for (const auto& cell : rows[r].cells) {
            size_t bestCol = 0;
            float bestDist = FLT_MAX;
            for (size_t c = 0; c < numCols; ++c) {
                const float cost = computeHorizontalMatchCost(cell, columns[c], medH);
                const float fallbackDist = std::abs(cell.cx() - columns[c].avgCenter());
                const float score = (cost < FLT_MAX) ? cost : (10000.0f + fallbackDist);
                if (score < bestDist) {
                    bestDist = score;
                    bestCol = c;
                }
            }
            if (!grid[r][bestCol].empty()) {
                grid[r][bestCol] += ' ';
            }
            grid[r][bestCol] += cell.text;
        }
    }

    result.isTable = true;
    result.rowCount = numRows;
    result.colCount = numCols;
    result.cells = std::move(grid);
    return result;
}

bool OcrTableReconstructor::hasTableStructure(const std::vector<OcrResult>& blocks) {
    return reconstruct(blocks).isTable;
}

bool OcrTableReconstructor::copyTableToClipboard(const OcrTableGrid& grid, HWND owner) {
    if (!grid.isTable || grid.cells.empty() || grid.colCount == 0) {
        return false;
    }

    const std::string tsvUtf8 = grid.toTsv("\r\n");
    const std::wstring tsvWide = tools3000::core::WinUtils::utf8ToWstring(tsvUtf8);
    const std::string htmlTable = grid.toHtmlTable();
    const std::string cfHtmlBytes = buildCfHtmlClipboardPayload(htmlTable);

    const UINT cfHtmlFormat = RegisterClipboardFormatW(L"HTML Format");

    bool opened = false;
    for (int retry = 0; retry < 15; ++retry) {
        if (OpenClipboard(owner)) {
            opened = true;
            break;
        }
        Sleep(8);
    }
    if (!opened) {
        LOG_WARN("OcrTableReconstructor: failed to open clipboard for table write");
        return false;
    }

    EmptyClipboard();
    bool wroteAny = false;

    // 1. 写入标准 CF_HTML (UTF-8 含终止符 \0)
    if (cfHtmlFormat != 0 && !cfHtmlBytes.empty()) {
        const size_t htmlBufSize = cfHtmlBytes.size() + 1;
        if (HGLOBAL hHtml = GlobalAlloc(GMEM_MOVEABLE, htmlBufSize)) {
            if (auto* dst = static_cast<char*>(GlobalLock(hHtml))) {
                std::memcpy(dst, cfHtmlBytes.data(), cfHtmlBytes.size());
                dst[cfHtmlBytes.size()] = '\0';
                GlobalUnlock(hHtml);
                if (SetClipboardData(cfHtmlFormat, hHtml) != nullptr) {
                    wroteAny = true;
                } else {
                    GlobalFree(hHtml);
                }
            } else {
                GlobalFree(hHtml);
            }
        }
    }

    // 2. 同步写入 CF_UNICODETEXT (TSV 制表符纯文本兜底)
    if (!tsvWide.empty()) {
        const size_t wideBytes = (tsvWide.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL hWide = GlobalAlloc(GMEM_MOVEABLE, wideBytes)) {
            if (auto* dst = static_cast<wchar_t*>(GlobalLock(hWide))) {
                std::memcpy(dst, tsvWide.c_str(), wideBytes);
                GlobalUnlock(hWide);
                if (SetClipboardData(CF_UNICODETEXT, hWide) != nullptr) {
                    wroteAny = true;
                } else {
                    GlobalFree(hWide);
                }
            } else {
                GlobalFree(hWide);
            }
        }
    }

    CloseClipboard();
    return wroteAny;
}

} // namespace tools3000::ocr
