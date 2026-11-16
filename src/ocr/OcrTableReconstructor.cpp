// ─────────────────────────────────────────────────────────────────────────────
// OcrTableReconstructor.cpp — OCR 表格结构网格检测与 CF_HTML + TSV 双格式重建实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/OcrTableReconstructor.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
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
    out = trimAsciiWhitespace(out);
    // 剥离表格横分割线紧贴产生的尾随孤立噪点（如 "Pdf ," -> "Pdf"）
    if (out.size() >= 3 && out[out.size() - 2] == ' ' &&
        (out.back() == ',' || out.back() == '.' || out.back() == '|' || out.back() == '_')) {
        out.resize(out.size() - 2);
        out = trimAsciiWhitespace(out);
    }
    return out;
}

bool isPureSeparatorNoiseToken(std::string_view sv) noexcept {
    if (sv.empty()) return true;
    bool hasSubstantive = false;
    for (size_t i = 0; i < sv.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(sv[i]);
        if (c == ' ' || c == '\t' || c == ',' || c == '.' || c == '-' ||
            c == '_' || c == '|' || c == ':' || c == ';' || c == '\'' || c == '`') {
            continue;
        }
        // UTF-8 常见分隔圆点/顿号 "·" (\xC2\xB7) 或 "、" (\xE3\x80\x81)
        if (c == 0xC2 && i + 1 < sv.size() && static_cast<unsigned char>(sv[i + 1]) == 0xB7) {
            ++i;
            continue;
        }
        if (c == 0xE3 && i + 2 < sv.size() &&
            static_cast<unsigned char>(sv[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(sv[i + 2]) == 0x81 || static_cast<unsigned char>(sv[i + 2]) == 0x82)) {
            i += 2;
            continue;
        }
        hasSubstantive = true;
        break;
    }
    return !hasSubstantive;
}

std::vector<CellToken> expandBlocksToCellTokens(const std::vector<OcrResult>& blocks) {
    std::vector<CellToken> tokens;
    tokens.reserve(blocks.size() * 2);

    for (const auto& blk : blocks) {
        if (blk.text.empty()) continue;

        if (blk.text.find('\t') == std::string::npos) {
            std::string cleaned = sanitizeTsvCell(blk.text);
            if (!cleaned.empty() && !isPureSeparatorNoiseToken(cleaned)) {
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
            std::string cleaned = sanitizeTsvCell(curSeg);
            if (!cleaned.empty() && !isPureSeparatorNoiseToken(cleaned)) {
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
    float sumRight = 0.0f;
    size_t count = 0;

    float avgLeft() const noexcept { return count > 0 ? sumLeft / static_cast<float>(count) : minLeft; }
    float avgCenter() const noexcept { return count > 0 ? sumCenter / static_cast<float>(count) : (minLeft + maxRight) * 0.5f; }
    float avgRight() const noexcept { return count > 0 ? sumRight / static_cast<float>(count) : maxRight; }

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
        sumRight += tok.right();
        ++count;
    }

    void mergeFrom(const ColumnCluster& other) noexcept {
        if (other.count == 0) return;
        if (count == 0) {
            *this = other;
            return;
        }
        minLeft = (std::min)(minLeft, other.minLeft);
        maxRight = (std::max)(maxRight, other.maxRight);
        sumLeft += other.sumLeft;
        sumCenter += other.sumCenter;
        sumRight += other.sumRight;
        count += other.count;
    }
};

float computeHorizontalMatchCost(const CellToken& tok, const ColumnCluster& col, float medH) noexcept {
    const float overlapLeft = (std::max)(tok.left(), col.minLeft);
    const float overlapRight = (std::min)(tok.right(), col.maxRight);
    const float overlap = overlapRight - overlapLeft;
    const float minSpan = (std::max)(1.0f, (std::min)(tok.right() - tok.left(), col.maxRight - col.minLeft));

    const float leftDiff = std::abs(tok.left() - col.avgLeft());
    const float centerDiff = std::abs(tok.cx() - col.avgCenter());
    const float rightDiff = std::abs(tok.right() - col.avgRight());
    const float minAlignDist = (std::min)({leftDiff, centerDiff, rightDiff});

    if (overlap > minSpan * 0.20f) {
        return minAlignDist * 0.45f;
    }
    if (minAlignDist <= (std::max)(20.0f, medH * 1.90f)) {
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

    // 行内从左到右排序，并合并同一单元格内部紧邻的碎词
    int globalMinX = INT_MAX;
    int globalMaxX = INT_MIN;
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
        for (const auto& cell : rowCells) {
            globalMinX = (std::min)(globalMinX, cell.box.x);
            globalMaxX = (std::max)(globalMaxX, cell.box.x + (std::max)(1, cell.box.width));
        }
    }

    // 1b. 跨行 1D 垂直空白走廊投影：若单行内部因个别单词漏检产生中等缺口 (< 2.8*H)，
    //     但上下多行在该水平区间均有真实文字横跨（非贯穿分列走廊），自动缝合回同一单元格
    if (rows.size() >= 3 && globalMaxX > globalMinX + 16 && (globalMaxX - globalMinX) <= 16384) {
        const int spanW = globalMaxX - globalMinX + 4;
        std::vector<uint16_t> xRowOcc(static_cast<size_t>(spanW), 0);
        for (const auto& r : rows) {
            int lastMarkedX = -1;
            for (const auto& cell : r.cells) {
                const int x0 = std::clamp(cell.box.x - globalMinX, 0, spanW - 1);
                const int x1 = std::clamp(cell.box.x + (std::max)(1, cell.box.width) - globalMinX, x0, spanW - 1);
                const int startX = (std::max)(x0, lastMarkedX + 1);
                for (int x = startX; x <= x1; ++x) {
                    ++xRowOcc[static_cast<size_t>(x)];
                }
                lastMarkedX = (std::max)(lastMarkedX, x1);
            }
        }

        const uint16_t nonGutterRowFloor = static_cast<uint16_t>((std::max)(size_t{2}, rows.size() / 4));
        for (auto& r : rows) {
            if (r.cells.size() < 2) continue;
            std::vector<CellToken> stitched;
            stitched.reserve(r.cells.size());
            for (auto& c : r.cells) {
                if (!stitched.empty()) {
                    auto& prev = stitched.back();
                    const float gap = c.left() - prev.right();
                    const float localH = (std::max)(8.0f, (prev.h() + c.h()) * 0.5f);
                    if (gap >= 0.0f && gap < localH * 2.85f) {
                        const int gx0 = std::clamp(static_cast<int>(std::floor(prev.right())) - globalMinX, 0, spanW - 1);
                        const int gx1 = std::clamp(static_cast<int>(std::ceil(c.left())) - globalMinX, gx0, spanW - 1);
                        uint16_t minOccInGap = UINT16_MAX;
                        for (int x = gx0; x <= gx1; ++x) {
                            minOccInGap = (std::min)(minOccInGap, xRowOcc[static_cast<size_t>(x)]);
                        }
                        if (minOccInGap != UINT16_MAX && minOccInGap >= nonGutterRowFloor) {
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
                }
                stitched.push_back(std::move(c));
            }
            r.cells = std::move(stitched);
        }
    }

    size_t multiColRowCount = 0;
    size_t seedRowIdx = 0;
    size_t maxRowCols = 0;
    std::array<size_t, 33> colCountHist{};

    for (size_t r = 0; r < rows.size(); ++r) {
        const size_t rc = rows[r].cells.size();
        if (rc >= OcrTableConstants::kMinTableCols) {
            ++multiColRowCount;
            if (rc < colCountHist.size()) {
                ++colCountHist[rc];
            }
            if (rc > maxRowCols) {
                maxRowCols = rc;
                seedRowIdx = r;
            }
        }
    }

    if (multiColRowCount < OcrTableConstants::kMinMultiColRows || maxRowCols < OcrTableConstants::kMinTableCols) {
        return result;
    }

    // 优先选取在至少 2 行稳定出现的最大列数行作为初始列种子，避免单行异常碎片主导列数
    size_t stableSeedCols = 0;
    for (size_t k = (std::min)(maxRowCols, colCountHist.size() - 1); k >= OcrTableConstants::kMinTableCols; --k) {
        if (colCountHist[k] >= 2) {
            stableSeedCols = k;
            break;
        }
    }
    if (stableSeedCols >= OcrTableConstants::kMinTableCols) {
        for (size_t r = 0; r < rows.size(); ++r) {
            if (rows[r].cells.size() == stableSeedCols) {
                seedRowIdx = r;
                break;
            }
        }
    }

    // 2. 以稳健多列种子行初始化列簇，再融合所有多列行校准全局列坐标（支持左对齐/居中/右对齐混合列）
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

    // 2b. 同行互斥重叠列簇归并：消除右对齐长短句或单行偏移分裂出的伪空列，同时完整保留真实的空缺单元格列
    auto assignCellToBestCol = [&](const CellToken& cell, const std::vector<ColumnCluster>& cols) -> size_t {
        size_t bestCol = 0;
        float bestDist = FLT_MAX;
        for (size_t c = 0; c < cols.size(); ++c) {
            const float cost = computeHorizontalMatchCost(cell, cols[c], medH);
            const float fallbackDist = (std::min)({
                std::abs(cell.left() - cols[c].avgLeft()),
                std::abs(cell.cx() - cols[c].avgCenter()),
                std::abs(cell.right() - cols[c].avgRight())
            });
            const float score = (cost < FLT_MAX) ? cost : (10000.0f + fallbackDist);
            if (score < bestDist) {
                bestDist = score;
                bestCol = c;
            }
        }
        return bestCol;
    };

    bool mergedAnyCol = true;
    while (mergedAnyCol && columns.size() > OcrTableConstants::kMinTableCols) {
        mergedAnyCol = false;
        for (size_t c = 0; c + 1 < columns.size(); ++c) {
            const auto& leftCol = columns[c];
            const auto& rightCol = columns[c + 1];
            const float overlap = (std::min)(leftCol.maxRight, rightCol.maxRight) -
                                  (std::max)(leftCol.minLeft, rightCol.minLeft);
            const float minAnchorDiff = (std::min)({
                std::abs(leftCol.avgLeft() - rightCol.avgLeft()),
                std::abs(leftCol.avgCenter() - rightCol.avgCenter()),
                std::abs(leftCol.avgRight() - rightCol.avgRight())
            });

            const size_t minColCount = (std::min)(leftCol.count, rightCol.count);
            const size_t maxColCount = (std::max)(leftCol.count, rightCol.count);
            const bool sparseOutlierPair =
                (minColCount <= (std::max)(size_t{1}, rows.size() / 5)) &&
                (maxColCount >= 3);

            const bool geometricallyRelated =
                (overlap > -medH * (sparseOutlierPair ? 2.50f : 0.45f)) ||
                (minAnchorDiff <= (std::max)(sparseOutlierPair ? 52.0f : 24.0f,
                                             medH * (sparseOutlierPair ? 4.60f : 2.15f)));
            if (!geometricallyRelated) continue;

            size_t cooccurRows = 0;
            for (const auto& r : rows) {
                bool hasC = false;
                bool hasNext = false;
                for (const auto& cell : r.cells) {
                    const size_t assigned = assignCellToBestCol(cell, columns);
                    if (assigned == c) hasC = true;
                    else if (assigned == c + 1) hasNext = true;
                }
                if (hasC && hasNext) {
                    ++cooccurRows;
                    break;
                }
            }

            if (cooccurRows == 0) {
                columns[c].mergeFrom(rightCol);
                columns.erase(columns.begin() + static_cast<std::ptrdiff_t>(c + 1));
                mergedAnyCol = true;
                break;
            }
        }
    }

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
            const size_t bestCol = assignCellToBestCol(cell, columns);
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
