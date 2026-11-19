#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// TextAnnotationHelper.h — 实景文字智能批注（荧光笔 / 波浪线 / 直线下划线）几何与流式选字中枢 (SSOT)
//
// 职责:
//   1. 为 PinWindow (贴图实景选字) 与 CaptureOverlay (截图框选智能标注) 提供单一事实源的流式逐字选区计算；
//   2. 实现排版感知同行跨度无缝合并 (Intra-Row Span Coalescing)，将同一行内相邻词块/代码胶囊平滑桥接为
//      高度整齐、不断裂的行级跨度矩形，同时严格隔离跨列空白走廊与异构侧边栏；
//   3. 统一定义文字批注精选高颜值色板常量与跨度几何等价判定。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_TEXTANNOTATIONHELPER_H
#define TOOLS3000_CAPTURE_TEXTANNOTATIONHELPER_H

#include "capture/MarkupEngine.h"
#include <opencv2/core.hpp>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <vector>

namespace tools3000::capture {

/// 文字批注精选 5 色调色板（兼顾浅底正片叠底清透度与深底高可读性）
namespace TextAnnotationConstants {
    inline constexpr size_t kPresetColorCount = 5;

    inline constexpr std::array<MarkupColor, kPresetColorCount> kPresetColors = {{
        {244,  63,  94, 255}, // 0: 珊瑚粉红 (#F43F5E) - 默认主力重点色
        {250, 204,  21, 255}, // 1: 琥珀亮黄 (#FACC15) - 经典荧光笔黄
        { 16, 185, 129, 255}, // 2: 翡翠薄荷 (#10B981)
        { 59, 130, 246, 255}, // 3: 晴空宝蓝 (#3B82F6)
        {139,  92, 246, 255}, // 4: 星云紫罗 (#8B5CF6)
    }};

    inline constexpr int kHorizontalSpanPadPx = 2;
    inline constexpr float kIntraRowMaxGapHeightRatio = 1.85f;
    inline constexpr float kIntraRowMinVerticalOverlap = 0.52f;
} // namespace TextAnnotationConstants

/// 通用字符级选中区间描述（供贴图与截图框选层共享）
struct SelectedBlockCharSpan {
    size_t blockIndex = 0;
    size_t startChar = 0;
    size_t endExclusive = 0;
};

class TextAnnotationHelper {
public:
    /// 在单行 charBoxes 中定位距离水平坐标 mx 最近的字符索引
    static size_t locateNearestCharInBoxes(const std::vector<cv::Rect>& charBoxes, float mx) noexcept {
        if (charBoxes.empty()) return 0;
        for (size_t c = 0; c < charBoxes.size(); ++c) {
            const auto& cb = charBoxes[c];
            if (mx >= static_cast<float>(cb.x) && mx <= static_cast<float>(cb.x + cb.width)) {
                return c;
            }
        }
        size_t bestIdx = 0;
        float bestDist = FLT_MAX;
        for (size_t c = 0; c < charBoxes.size(); ++c) {
            const auto& cb = charBoxes[c];
            const float cx = static_cast<float>(cb.x) + static_cast<float>(cb.width) * 0.5f;
            const float d = std::abs(mx - cx);
            if (d < bestDist) {
                bestDist = d;
                bestIdx = c;
            }
        }
        return bestIdx;
    }

    /// 兼容 PinOcrBlock (.origBox) 与 OcrResult (.box) 的统一包围盒读取器
    template <typename BlockT>
    static const cv::Rect& blockRect(const BlockT& b) noexcept {
        if constexpr (requires { b.origBox; }) {
            return b.origBox;
        } else {
            return b.box;
        }
    }

    /// 命中检测点 (mx, my) 所在的文字块与字符索引（带温和微容差 padPx，提升选字跟手率）
    template <typename BlockT>
    static int hitTestBlockAndChar(const std::vector<BlockT>& blocks,
                                   float mx,
                                   float my,
                                   size_t* outCharIdx = nullptr,
                                   float padPx = 3.0f) noexcept {
        const cv::Point pt(static_cast<int>(std::lround(mx)), static_cast<int>(std::lround(my)));
        for (size_t i = 0; i < blocks.size(); ++i) {
            const cv::Rect& rc = blockRect(blocks[i]);
            if (rc.contains(pt)) {
                if (outCharIdx) {
                    *outCharIdx = locateNearestCharInBoxes(blocks[i].charBoxes, mx);
                }
                return static_cast<int>(i);
            }
        }
        int bestIdx = -1;
        float bestDistSq = padPx * padPx;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const cv::Rect& rc = blockRect(blocks[i]);
            const float dx = (mx < rc.x) ? (rc.x - mx)
                           : ((mx > rc.x + rc.width) ? (mx - (rc.x + rc.width)) : 0.0f);
            const float dy = (my < rc.y) ? (rc.y - my)
                           : ((my > rc.y + rc.height) ? (my - (rc.y + rc.height)) : 0.0f);
            const float dSq = dx * dx + dy * dy;
            if (dSq <= bestDistSq) {
                bestDistSq = dSq;
                bestIdx = static_cast<int>(i);
            }
        }
        if (bestIdx >= 0 && outCharIdx) {
            *outCharIdx = locateNearestCharInBoxes(blocks[static_cast<size_t>(bestIdx)].charBoxes, mx);
        }
        return bestIdx;
    }

    /// 版面感知流式逐字选区计算（隔离异构侧边栏与错位孤立标签，支持单行双向与跨多行流式划词）
    template <typename BlockT, typename SpanOutT>
    static void computeStreamSelection(const std::vector<BlockT>& blocks,
                                       int anchorBlockIndex,
                                       size_t anchorCharIndex,
                                       float startMx,
                                       float curMx,
                                       float curMy,
                                       std::vector<size_t>& outBlockIndices,
                                       std::vector<SpanOutT>& outCharSpans) {
        outBlockIndices.clear();
        outCharSpans.clear();
        if (anchorBlockIndex < 0 || static_cast<size_t>(anchorBlockIndex) >= blocks.size()) {
            return;
        }

        const auto& anchorBlk = blocks[static_cast<size_t>(anchorBlockIndex)];
        const cv::Rect& anchorRc = blockRect(anchorBlk);
        const int anchorRegId = anchorBlk.layoutRegionId;

        float regMinX = static_cast<float>(anchorRc.x);
        float regMaxX = static_cast<float>(anchorRc.x + anchorRc.width);
        for (const auto& rb : blocks) {
            if (rb.layoutRegionId == anchorRegId) {
                const cv::Rect& rrc = blockRect(rb);
                regMinX = (std::min)(regMinX, static_cast<float>(rrc.x));
                regMaxX = (std::max)(regMaxX, static_cast<float>(rrc.x + rrc.width));
            }
        }
        const float anchorH = static_cast<float>((std::max)(10, anchorRc.height));
        const bool cursorInAnchorRegionX =
            (curMx >= regMinX - anchorH * 2.5f && curMx <= regMaxX + anchorH * 2.5f);

        int targetBlock = -1;
        float bestScore = FLT_MAX;
        for (int pass = 0; pass < 2 && targetBlock < 0; ++pass) {
            const bool sameRegOnly = (pass == 0 && cursorInAnchorRegionX);
            if (pass == 0 && !cursorInAnchorRegionX) continue;
            for (size_t b = 0; b < blocks.size(); ++b) {
                const auto& cand = blocks[b];
                if (sameRegOnly && cand.layoutRegionId != anchorRegId) continue;
                const cv::Rect& ob = blockRect(cand);
                constexpr float padY = 4.0f;
                if (curMy >= static_cast<float>(ob.y) - padY &&
                    curMy <= static_cast<float>(ob.y + ob.height) + padY) {
                    float dx = 0.0f;
                    if (curMx < static_cast<float>(ob.x)) dx = static_cast<float>(ob.x) - curMx;
                    else if (curMx > static_cast<float>(ob.x + ob.width)) dx = curMx - static_cast<float>(ob.x + ob.width);
                    if (dx < bestScore) {
                        bestScore = dx;
                        targetBlock = static_cast<int>(b);
                    }
                }
            }
        }
        if (targetBlock < 0) {
            for (size_t b = 0; b < blocks.size(); ++b) {
                const auto& cand = blocks[b];
                if (cursorInAnchorRegionX && cand.layoutRegionId != anchorRegId) continue;
                const cv::Rect& ob = blockRect(cand);
                const float cy = static_cast<float>(ob.y) + static_cast<float>(ob.height) * 0.5f;
                const float dy = std::abs(curMy - cy);
                if (dy < bestScore) {
                    bestScore = dy;
                    targetBlock = static_cast<int>(b);
                }
            }
        }
        if (targetBlock < 0) {
            for (size_t b = 0; b < blocks.size(); ++b) {
                const cv::Rect& ob = blockRect(blocks[b]);
                const float cy = static_cast<float>(ob.y) + static_cast<float>(ob.height) * 0.5f;
                const float dy = std::abs(curMy - cy);
                if (dy < bestScore) {
                    bestScore = dy;
                    targetBlock = static_cast<int>(b);
                }
            }
        }
        if (targetBlock < 0) return;

        if (targetBlock == anchorBlockIndex) {
            const auto& blk = blocks[static_cast<size_t>(targetBlock)];
            const float minMx = (std::min)(startMx, curMx);
            const float maxMx = (std::max)(startMx, curMx);
            size_t firstHit = blk.charBoxes.size();
            size_t lastHit = 0;
            for (size_t c = 0; c < blk.charBoxes.size(); ++c) {
                const auto& cb = blk.charBoxes[c];
                const float entryThresh = static_cast<float>(cb.x) + static_cast<float>(cb.width) * 0.28f;
                const float exitThresh = static_cast<float>(cb.x) + static_cast<float>(cb.width) * 0.72f;
                if (entryThresh <= maxMx && exitThresh >= minMx) {
                    firstHit = (std::min)(firstHit, c);
                    lastHit = (std::max)(lastHit, c + 1);
                }
            }
            if (firstHit >= lastHit && !blk.charBoxes.empty()) {
                const size_t fallbackC = (std::min)(anchorCharIndex, blk.charBoxes.size() - 1);
                firstHit = fallbackC;
                lastHit = fallbackC + 1;
            }
            if (firstHit < lastHit) {
                outBlockIndices.push_back(static_cast<size_t>(targetBlock));
                outCharSpans.push_back(SpanOutT{static_cast<size_t>(targetBlock), firstHit, lastHit});
            } else if (blk.charBoxes.empty()) {
                outBlockIndices.push_back(static_cast<size_t>(targetBlock));
            }
            return;
        }

        if (blocks[static_cast<size_t>(targetBlock)].layoutRegionId == anchorRegId) {
            const size_t caretChar = locateNearestCharInBoxes(
                blocks[static_cast<size_t>(targetBlock)].charBoxes, curMx);
            int b1 = anchorBlockIndex;
            size_t c1 = anchorCharIndex;
            int b2 = targetBlock;
            size_t c2 = caretChar;
            if (b1 > b2) {
                std::swap(b1, b2);
                std::swap(c1, c2);
            }

            const cv::Rect& rc1 = blockRect(blocks[static_cast<size_t>(b1)]);
            const cv::Rect& rc2 = blockRect(blocks[static_cast<size_t>(b2)]);
            const float padX = static_cast<float>((std::max)(rc1.height, rc2.height)) * 1.5f;
            const float activeMinX = (std::min)({
                static_cast<float>(rc1.x), static_cast<float>(rc2.x), startMx, curMx
            }) - padX;
            const float activeMaxX = (std::max)({
                static_cast<float>(rc1.x + rc1.width), static_cast<float>(rc2.x + rc2.width), startMx, curMx
            }) + padX;

            const int row1 = blocks[static_cast<size_t>(b1)].alignedRowId;
            const int row2 = blocks[static_cast<size_t>(b2)].alignedRowId;

            for (int b = b1; b <= b2; ++b) {
                const auto& blk = blocks[static_cast<size_t>(b)];
                if (blk.layoutRegionId != anchorRegId) continue;
                const cv::Rect& ob = blockRect(blk);

                if (b != b1 && b != b2 && blk.alignedRowId != row1 && blk.alignedRowId != row2) {
                    const float bx0 = static_cast<float>(ob.x);
                    const float bx1 = static_cast<float>(ob.x + ob.width);
                    if (bx1 < activeMinX || bx0 > activeMaxX) {
                        size_t rowPeers = 0;
                        for (const auto& p : blocks) {
                            if (p.layoutRegionId == anchorRegId && p.alignedRowId == blk.alignedRowId) {
                                ++rowPeers;
                            }
                        }
                        if (rowPeers <= 1) continue;
                    }
                }

                const size_t totalC = blk.charBoxes.size();
                if (totalC == 0) {
                    outBlockIndices.push_back(static_cast<size_t>(b));
                    continue;
                }
                size_t sc = (b == b1) ? (std::min)(c1, totalC - 1) : 0;
                size_t ec = (b == b2) ? (std::min)(totalC, c2 + 1) : totalC;
                if (sc < ec) {
                    outBlockIndices.push_back(static_cast<size_t>(b));
                    outCharSpans.push_back(SpanOutT{static_cast<size_t>(b), sc, ec});
                }
            }
            return;
        }

        // 跨版面分区拖拽：退化为几何矩形覆盖选块
        const float rx1 = (std::min)(startMx, curMx);
        const float rx2 = (std::max)(startMx, curMx);
        const float anchorCy = static_cast<float>(anchorRc.y) + static_cast<float>(anchorRc.height) * 0.5f;
        const float ry1 = (std::min)(anchorCy, curMy);
        const float ry2 = (std::max)(anchorCy, curMy);
        const cv::Rect2f dragBox(rx1, ry1, (std::max)(1.0f, rx2 - rx1), (std::max)(1.0f, ry2 - ry1));
        for (size_t b = 0; b < blocks.size(); ++b) {
            const cv::Rect& ob = blockRect(blocks[b]);
            const cv::Rect2f bf(static_cast<float>(ob.x), static_cast<float>(ob.y),
                                static_cast<float>(ob.width), static_cast<float>(ob.height));
            if ((bf & dragBox).area() > 0.0f) {
                outBlockIndices.push_back(b);
                if (!blocks[b].charBoxes.empty()) {
                    outCharSpans.push_back(SpanOutT{b, 0, blocks[b].charBoxes.size()});
                }
            }
        }
    }

    /// 将当前选中的字符跨度提取并执行排版感知同行无缝合并（生成整齐划一、不断裂的行级批注矩形序列）
    template <typename BlockT, typename SpanInT>
    static std::vector<cv::Rect> coalesceSelectedTextSpans(
        const std::vector<BlockT>& blocks,
        const std::vector<size_t>& selectedBlockIndices,
        const std::vector<SpanInT>& selectedCharSpans,
        int imageWidth = 0,
        int imageHeight = 0) {
        struct RawSpanItem {
            cv::Rect rect;
            int regionId = 0;
            int rowId = -1;
        };
        std::vector<RawSpanItem> rawItems;

        if (!selectedCharSpans.empty()) {
            rawItems.reserve(selectedCharSpans.size());
            for (const auto& sp : selectedCharSpans) {
                if (sp.blockIndex >= blocks.size() || sp.startChar >= sp.endExclusive) continue;
                const auto& blk = blocks[sp.blockIndex];
                const cv::Rect& ob = blockRect(blk);
                if (blk.charBoxes.empty() || sp.startChar >= blk.charBoxes.size()) {
                    if (ob.width > 0 && ob.height > 0) {
                        rawItems.push_back({ob, blk.layoutRegionId, blk.alignedRowId});
                    }
                    continue;
                }
                const size_t endIdx = (std::min)(sp.endExclusive, blk.charBoxes.size()) - 1;
                int x1 = blk.charBoxes[sp.startChar].x;
                int y1 = blk.charBoxes[sp.startChar].y;
                int x2 = blk.charBoxes[sp.startChar].x + blk.charBoxes[sp.startChar].width;
                int y2 = blk.charBoxes[sp.startChar].y + blk.charBoxes[sp.startChar].height;
                for (size_t k = sp.startChar; k <= endIdx; ++k) {
                    const auto& cb = blk.charBoxes[k];
                    x1 = (std::min)(x1, cb.x);
                    y1 = (std::min)(y1, cb.y);
                    x2 = (std::max)(x2, cb.x + cb.width);
                    y2 = (std::max)(y2, cb.y + cb.height);
                }
                if (x2 > x1 && y2 > y1) {
                    rawItems.push_back({cv::Rect(x1, y1, x2 - x1, y2 - y1), blk.layoutRegionId, blk.alignedRowId});
                }
            }
        } else {
            rawItems.reserve(selectedBlockIndices.size());
            for (size_t idx : selectedBlockIndices) {
                if (idx < blocks.size()) {
                    const auto& blk = blocks[idx];
                    const cv::Rect& ob = blockRect(blk);
                    if (ob.width > 0 && ob.height > 0) {
                        rawItems.push_back({ob, blk.layoutRegionId, blk.alignedRowId});
                    }
                }
            }
        }

        if (rawItems.empty()) return {};

        std::stable_sort(rawItems.begin(), rawItems.end(), [](const RawSpanItem& a, const RawSpanItem& b) {
            if (a.regionId != b.regionId) return a.regionId < b.regionId;
            if (a.rowId >= 0 && b.rowId >= 0 && a.rowId != b.rowId) return a.rowId < b.rowId;
            const int cyA = a.rect.y + a.rect.height / 2;
            const int cyB = b.rect.y + b.rect.height / 2;
            const int minH = (std::max)(1, (std::min)(a.rect.height, b.rect.height));
            if (std::abs(cyA - cyB) > minH / 2) return cyA < cyB;
            return a.rect.x < b.rect.x;
        });

        std::vector<RawSpanItem> merged;
        merged.reserve(rawItems.size());
        for (const auto& item : rawItems) {
            if (merged.empty()) {
                merged.push_back(item);
                continue;
            }
            auto& prev = merged.back();
            bool sameLine = false;
            if (prev.regionId == item.regionId) {
                if (prev.rowId >= 0 && prev.rowId == item.rowId) {
                    sameLine = true;
                } else {
                    const int ovTop = (std::max)(prev.rect.y, item.rect.y);
                    const int ovBot = (std::min)(prev.rect.y + prev.rect.height, item.rect.y + item.rect.height);
                    const int minH = (std::max)(1, (std::min)(prev.rect.height, item.rect.height));
                    if (ovBot > ovTop &&
                        static_cast<float>(ovBot - ovTop) >= static_cast<float>(minH) * TextAnnotationConstants::kIntraRowMinVerticalOverlap) {
                        sameLine = true;
                    }
                }
            }

            if (sameLine) {
                const int maxH = (std::max)(prev.rect.height, item.rect.height);
                const int gap = item.rect.x - (prev.rect.x + prev.rect.width);
                if (gap <= static_cast<int>(std::lround(static_cast<float>(maxH) * TextAnnotationConstants::kIntraRowMaxGapHeightRatio))) {
                    const int nx1 = (std::min)(prev.rect.x, item.rect.x);
                    const int ny1 = (std::min)(prev.rect.y, item.rect.y);
                    const int nx2 = (std::max)(prev.rect.x + prev.rect.width, item.rect.x + item.rect.width);
                    const int ny2 = (std::max)(prev.rect.y + prev.rect.height, item.rect.y + item.rect.height);
                    prev.rect = cv::Rect(nx1, ny1, nx2 - nx1, ny2 - ny1);
                    continue;
                }
            }
            merged.push_back(item);
        }

        std::vector<cv::Rect> outSpans;
        outSpans.reserve(merged.size());
        for (const auto& m : merged) {
            int x1 = m.rect.x;
            int y1 = m.rect.y;
            int x2 = m.rect.x + m.rect.width;
            int y2 = m.rect.y + m.rect.height;
            if (imageWidth > 0 && imageHeight > 0) {
                x1 = std::clamp(x1, 0, imageWidth - 1);
                y1 = std::clamp(y1, 0, imageHeight - 1);
                x2 = std::clamp(x2, x1 + 1, imageWidth);
                y2 = std::clamp(y2, y1 + 1, imageHeight);
            } else {
                x1 = (std::max)(0, x1);
                y1 = (std::max)(0, y1);
                x2 = (std::max)(x1 + 1, x2);
                y2 = (std::max)(y1 + 1, y2);
            }
            outSpans.emplace_back(x1, y1, x2 - x1, y2 - y1);
        }

        std::stable_sort(outSpans.begin(), outSpans.end(), [](const cv::Rect& a, const cv::Rect& b) {
            const int cyA = a.y + a.height / 2;
            const int cyB = b.y + b.height / 2;
            const int minH = (std::max)(1, (std::min)(a.height, b.height));
            if (std::abs(cyA - cyB) > minH / 2) return cyA < cyB;
            return a.x < b.x;
        });
        return outSpans;
    }

    /// 判断两组行跨度序列是否在像素级几何等价（供同选区二次点击 Toggle 取消或实时换色匹配使用）
    static bool spansGeometryEqual(const std::vector<cv::Rect>& a,
                                   const std::vector<cv::Rect>& b,
                                   int tolPx = 2) noexcept {
        if (a.size() != b.size() || a.empty()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::abs(a[i].x - b[i].x) > tolPx ||
                std::abs(a[i].y - b[i].y) > tolPx ||
                std::abs(a[i].width - b[i].width) > tolPx ||
                std::abs(a[i].height - b[i].height) > tolPx) {
                return false;
            }
        }
        return true;
    }
};

/// ─────────────────────────────────────────────────────────────────────────────
/// HandDrawnStrokeGenerator — 人手马克笔/彩笔划重点自然轨迹与压感轮廓单一事实源 (SSOT)
///
/// 供 GDI+ 成品合成（MarkupEngine）与 Direct2D 60FPS 实时预览（CaptureRenderer）共享：
///   - 确定性几何哈希种子：同一条线重绘 100% 稳定零抖动，不同行自带独特手绘韵味；
///   - 宽舒有机手绘波浪（全波长 27~48px + 逐半波微起伏 + 整体手腕微弓），彻底告别 Word 语法报错细锯齿；
///   - 手绘醒目重点直线（轻微手腕弓形弧度 + 起笔顿挫加压 + 中段饱满粗壮 + 收笔轻扬尖细收锋）。
/// ─────────────────────────────────────────────────────────────────────────────
struct HandDrawnSample {
    float x = 0.0f;
    float y = 0.0f;
    float halfWidth = 1.5f;
};

struct HandDrawnRibbon {
    std::vector<HandDrawnSample> spine;
    std::vector<cv::Point2f> outline; // 闭合变宽压感轮廓多边形（左侧顺行 + 末端圆帽 + 右侧逆行 + 起点圆帽）
};

class HandDrawnStrokeGenerator {
public:
    static uint32_t hashGeometrySeed(float x0, float y0, float x1, float y1) noexcept {
        uint32_t h = 2166136261u;
        auto mix = [&](int v) noexcept {
            h ^= static_cast<uint32_t>(v);
            h *= 16777619u;
        };
        mix(static_cast<int>(std::lround(x0 * 0.25f)));
        mix(static_cast<int>(std::lround(y0 * 0.25f)));
        mix(static_cast<int>(std::lround((x1 - x0) * 0.20f)));
        mix(static_cast<int>(std::lround((y1 - y0) * 0.20f)));
        h ^= (h >> 16);
        h *= 0x7FEB352Du;
        h ^= (h >> 15);
        return h;
    }

    static float unitRand(uint32_t seed, uint32_t index) noexcept {
        uint32_t v = seed ^ (index * 0x9E3779B9u + 0x85EBCA6Bu);
        v ^= (v >> 13);
        v *= 0xC2B2AE35u;
        v ^= (v >> 16);
        return static_cast<float>(v & 0x00FFFFFFu) / 16777215.0f;
    }

    /// 依据文字行高推算高定斜角书法丝带波浪线参数（醒目主宽 / 克制优雅振幅 / 宽舒半波长）
    static void resolveTextWavyMetrics(float spanHeight, float explicitThickness,
                                       float& outWidth, float& outAmp, float& outHalfWave) noexcept {
        const float h = (std::max)(10.0f, spanHeight);
        outWidth = (explicitThickness > 0.5f)
            ? (std::max)(2.6f, explicitThickness * 1.06f)
            : std::clamp(h * 0.140f, 2.8f, 4.3f);
        outAmp = std::clamp(h * 0.092f, 1.85f, 3.1f);
        outHalfWave = std::clamp(h * 0.78f, 15.5f, 26.0f);
    }

    /// 计算波浪线黄金呼吸基线 Y：确保波峰最高点与文字底边保留 1.8~3.0px 空气留白，100% 绝不切字压笔画
    static float resolveTextWavyBaselineY(float spanBottomY, float spanHeight, float amplitude) noexcept {
        const float h = (std::max)(10.0f, spanHeight);
        return spanBottomY + amplitude + std::clamp(h * 0.085f, 1.8f, 3.0f);
    }

    /// 依据文字行高推算高定微弧手绘重点直线宽度
    static float resolveTextStraightWidth(float spanHeight, float explicitThickness) noexcept {
        const float h = (std::max)(10.0f, spanHeight);
        return (explicitThickness > 0.5f)
            ? (std::max)(2.8f, explicitThickness * 1.10f)
            : std::clamp(h * 0.155f, 3.1f, 4.6f);
    }

    /// 计算手绘重点直线黄金呼吸基线 Y
    static float resolveTextStraightBaselineY(float spanBottomY, float spanHeight, float strokeWidth) noexcept {
        const float h = (std::max)(10.0f, spanHeight);
        return spanBottomY + strokeWidth * 0.55f + std::clamp(h * 0.10f, 2.2f, 3.4f);
    }

    /// 生成大师级斜角书法丝带手绘波浪线（C-infinity 光滑相位呼吸 + 38° 斜切笔尖下粗上细韵律 + 起落笔收锋）
    static HandDrawnRibbon buildWavyStroke(float x0, float y0, float x1, float y1,
                                           float baseWidth, float amplitude, float halfWavelength,
                                           bool extendEnds = true) {
        HandDrawnRibbon res;
        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float rawDist = std::hypot(dx, dy);
        if (rawDist < 1.0f) return res;

        const uint32_t seed = hashGeometrySeed(x0, y0, x1, y1);
        const float ux = dx / rawDist;
        const float uy = dy / rawDist;
        const float nx = -uy;
        const float ny = ux;

        // 起止略微飘逸舒展，模拟人手运笔自然出锋
        const float extL = extendEnds ? (std::min)(2.5f, rawDist * 0.045f) : 0.0f;
        const float extR = extendEnds ? (std::min)(4.5f, rawDist * 0.060f) : 0.0f;
        const float sx = x0 - ux * extL;
        const float sy = y0 - uy * extL;
        const float dist = rawDist + extL + extR;

        const float sw = (std::max)(1.6f, baseWidth);
        const float baseR = sw * 0.5f;
        const float amp = (std::max)(1.4f, amplitude);
        const float halfW = (std::max)(10.0f, halfWavelength);

        // 连续光滑低频有机调制参数（C-infinity 无折角，彻底告别铁丝拧巴感与 Word 锯齿）
        const float phWave = unitRand(seed, 1) * 6.2831853f;
        const float phFreqMod = unitRand(seed, 2) * 6.2831853f;
        const float phAmpMod = unitRand(seed, 3) * 6.2831853f;
        const float bowSign = (unitRand(seed, 4) > 0.45f) ? -1.0f : 1.0f;
        const float bowAmp = std::clamp(dist * 0.0028f, 0.25f, 0.72f) * bowSign;
        const float baseOmega = 3.14159265f / halfW;

        const int sampleCount = std::clamp(static_cast<int>(std::ceil(dist / 1.0f)), 10, 2400);
        std::vector<cv::Point2f> centers;
        centers.reserve(static_cast<size_t>(sampleCount) + 1);

        const float startEnvLen = (std::min)(14.0f, dist * 0.14f);
        const float endEnvLen = (std::min)(22.0f, dist * 0.18f);

        for (int i = 0; i <= sampleCount; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(sampleCount);
            const float s = dist * t;

            // 首尾余弦柔化包络，确保入笔轻盈优雅、收笔自然舒展上挑
            float waveEnv = 1.0f;
            if (startEnvLen > 0.5f && s < startEnvLen) {
                const float u = s / startEnvLen;
                waveEnv *= (0.28f + 0.72f * (0.5f - 0.5f * std::cos(3.14159265f * u)));
            }
            if (endEnvLen > 0.5f && (dist - s) < endEnvLen) {
                const float u = (dist - s) / endEnvLen;
                waveEnv *= (0.20f + 0.80f * (0.5f - 0.5f * std::cos(3.14159265f * u)));
            }

            // 平滑相位呼吸（波长随手腕节奏缓步微伸微缩）+ 轻微斜偏二次谐波（赋予流畅书写倾斜美感）
            const float phase = baseOmega * s + phWave + 0.34f * std::sin(s * 0.017f + phFreqMod);
            const float ampBreath = 0.90f + 0.13f * std::sin(s * 0.023f + phAmpMod);
            const float organicWave = std::sin(phase) + 0.11f * std::sin(phase * 2.0f + 0.6f);
            const float archOff = std::sin(3.14159265f * t) * bowAmp;
            const float tailLift = (t > 0.86f)
                ? (-std::pow((t - 0.86f) / 0.14f, 1.6f) * std::clamp(amp * 0.36f, 0.5f, 1.1f))
                : 0.0f;

            const float totalOff = organicWave * amp * ampBreath * waveEnv + archOff + tailLift;
            centers.emplace_back(sx + ux * s + nx * totalOff,
                                 sy + uy * s + ny * totalOff);
        }

        // 斜角平头书法笔尖物理模型（38° 握笔倾角：向右下俯冲饱满宽厚 1.24x，向右上挑纤巧利落 0.62x）
        constexpr float kNibCos = 0.78801075f; // cos(38 deg)
        constexpr float kNibSin = 0.61566148f; // sin(38 deg)
        const float startRamp = (std::min)(12.0f, dist * 0.09f);
        const float endTaper = (std::min)(24.0f, dist * 0.17f);

        res.spine.reserve(centers.size());
        for (size_t i = 0; i < centers.size(); ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(sampleCount);
            const float s = dist * t;
            const auto& pPrev = centers[(i == 0) ? 0 : (i - 1)];
            const auto& pNext = centers[(i + 1 < centers.size()) ? (i + 1) : (centers.size() - 1)];
            float tx = pNext.x - pPrev.x;
            float ty = pNext.y - pPrev.y;
            const float tLen = std::hypot(tx, ty);
            if (tLen > 1e-4f) {
                tx /= tLen;
                ty /= tLen;
            } else {
                tx = ux;
                ty = uy;
            }
            // 将局部切线投影至线段局部坐标系，提取顺坡(downstroke)与上挑(upstroke)笔锋分量
            const float localTx = tx * ux + ty * uy;
            const float localTy = tx * nx + ty * ny;
            // 右手 38° 握笔：向右下走 (localTy > 0) 笔锋垂直展开最宽，向右上挑 (localTy < 0) 侧锋收细
            const float crossNib = std::abs(localTx * kNibSin + localTy * kNibCos);
            const float calligraphicMul = 0.56f + 0.78f * std::clamp(crossNib, 0.0f, 1.0f);

            float taperMul = 1.0f;
            if (startRamp > 0.5f && s < startRamp) {
                const float u = s / startRamp;
                taperMul *= (0.50f + 0.56f * (u * (2.0f - u)));
            }
            if (endTaper > 0.5f && (dist - s) < endTaper) {
                const float u = (dist - s) / endTaper;
                taperMul *= (0.28f + 0.72f * std::pow(std::clamp(u, 0.0f, 1.0f), 0.58f));
            }

            res.spine.push_back(HandDrawnSample{
                centers[i].x,
                centers[i].y,
                (std::max)(0.50f, baseR * calligraphicMul * taperMul)
            });
        }

        buildOutlineFromSpine(res);
        return res;
    }

    /// 生成人手马克笔醒目手绘直线（微弓弧度 + 细微手腕运笔纹理 + 落笔顿挫/收笔尖锋）
    static HandDrawnRibbon buildStraightStroke(float x0, float y0, float x1, float y1,
                                               float baseWidth, bool extendEnds = true) {
        HandDrawnRibbon res;
        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float rawDist = std::hypot(dx, dy);
        if (rawDist < 1.0f) return res;

        const uint32_t seed = hashGeometrySeed(x0, y0, x1, y1);
        const float ux = dx / rawDist;
        const float uy = dy / rawDist;
        const float nx = -uy;
        const float ny = ux;

        const float extL = extendEnds ? (std::min)(2.4f, rawDist * 0.045f) : 0.0f;
        const float extR = extendEnds ? (std::min)(4.4f, rawDist * 0.065f) : 0.0f;
        const float sx = x0 - ux * extL;
        const float sy = y0 - uy * extL;
        const float dist = rawDist + extL + extR;

        const float sw = (std::max)(1.6f, baseWidth);
        const float baseR = sw * 0.5f;

        // 柔和手腕微弓弧度（一眼仍是醒目直线，同时一眼能看出是人手专门划出来的）
        const float bowSign = (unitRand(seed, 5) > 0.42f) ? -1.0f : 1.0f; // 略偏向人手向上轻拱的经典弧度
        const float bowAmp = std::clamp(dist * 0.0085f, 0.70f, 2.05f) * bowSign;
        const float tiltEnd = (unitRand(seed, 6) - 0.62f) * 1.1f; // 收笔自然微挑
        const float ph1 = unitRand(seed, 7) * 6.2831853f;
        const float ph2 = unitRand(seed, 8) * 6.2831853f;
        const float pressPhase = unitRand(seed, 9) * 6.2831853f;

        const int sampleCount = std::clamp(static_cast<int>(std::ceil(dist / 2.0f)), 6, 1024);
        res.spine.reserve(static_cast<size_t>(sampleCount) + 1);

        const float startRamp = (std::min)(10.0f, dist * 0.08f);
        const float endTaper = (std::min)(22.0f, dist * 0.16f);

        for (int i = 0; i <= sampleCount; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(sampleCount);
            const float s = dist * t;
            const float archOff = std::sin(3.14159265f * t) * bowAmp;
            const float tiltOff = (t - 0.5f) * tiltEnd;
            // 低频柔和笔触纹理（严格限制在 +-0.55px 内，保持笔直醒目但告别机械像素直线）
            const float microTex = (0.34f * std::sin(s * 0.048f + ph1) + 0.18f * std::cos(s * 0.095f + ph2))
                                   * std::sin(3.14159265f * t);
            const float totalOff = archOff + tiltOff + microTex;

            float widthFactor = 1.00f + 0.08f * std::sin(t * 5.8f + pressPhase) + 0.04f * std::cos(s * 0.06f);
            if (startRamp > 0.5f && s < startRamp) {
                const float u = s / startRamp;
                const float ease = u * (2.0f - u);
                widthFactor *= (0.70f + 0.42f * ease); // 起笔略顿挫蓄力
            }
            if (endTaper > 0.5f && (dist - s) < endTaper) {
                const float u = (dist - s) / endTaper;
                const float ease = std::sqrt(std::clamp(u, 0.0f, 1.0f));
                widthFactor *= (0.34f + 0.66f * ease); // 收笔利落轻挑尖细
            }

            res.spine.push_back(HandDrawnSample{
                sx + ux * s + nx * totalOff,
                sy + uy * s + ny * totalOff,
                (std::max)(0.65f, baseR * widthFactor)
            });
        }

        buildOutlineFromSpine(res);
        return res;
    }

private:
    static void buildOutlineFromSpine(HandDrawnRibbon& ribbon) {
        const size_t n = ribbon.spine.size();
        if (n < 2) return;

        std::vector<cv::Point2f> leftPts;
        std::vector<cv::Point2f> rightPts;
        leftPts.reserve(n);
        rightPts.reserve(n);

        for (size_t i = 0; i < n; ++i) {
            const auto& prev = ribbon.spine[(i == 0) ? 0 : (i - 1)];
            const auto& next = ribbon.spine[(i + 1 < n) ? (i + 1) : (n - 1)];
            float tx = next.x - prev.x;
            float ty = next.y - prev.y;
            const float len = std::hypot(tx, ty);
            if (len < 1e-4f) {
                tx = 1.0f;
                ty = 0.0f;
            } else {
                tx /= len;
                ty /= len;
            }
            const float nx = -ty;
            const float ny = tx;
            const float r = ribbon.spine[i].halfWidth;
            leftPts.emplace_back(ribbon.spine[i].x + nx * r, ribbon.spine[i].y + ny * r);
            rightPts.emplace_back(ribbon.spine[i].x - nx * r, ribbon.spine[i].y - ny * r);
        }

        ribbon.outline.reserve(n * 2 + 12);
        for (const auto& pt : leftPts) {
            ribbon.outline.push_back(pt);
        }

        // 末端圆润半圆收锋帽
        {
            const auto& tail = ribbon.spine.back();
            const auto& prev = ribbon.spine[n - 2];
            const float ang = std::atan2(tail.y - prev.y, tail.x - prev.x);
            constexpr int kCapSegs = 5;
            for (int k = 1; k < kCapSegs; ++k) {
                const float a = ang + 1.5707963f - 3.14159265f * (static_cast<float>(k) / static_cast<float>(kCapSegs));
                ribbon.outline.emplace_back(tail.x + std::cos(a) * tail.halfWidth,
                                            tail.y + std::sin(a) * tail.halfWidth);
            }
        }

        for (size_t i = n; i > 0; --i) {
            ribbon.outline.push_back(rightPts[i - 1]);
        }

        // 起笔圆润半圆顿挫帽
        {
            const auto& head = ribbon.spine.front();
            const auto& next = ribbon.spine[1];
            const float ang = std::atan2(head.y - next.y, head.x - next.x);
            constexpr int kCapSegs = 5;
            for (int k = 1; k < kCapSegs; ++k) {
                const float a = ang + 1.5707963f - 3.14159265f * (static_cast<float>(k) / static_cast<float>(kCapSegs));
                ribbon.outline.emplace_back(head.x + std::cos(a) * head.halfWidth,
                                            head.y + std::sin(a) * head.halfWidth);
            }
        }
    }
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_TEXTANNOTATIONHELPER_H
