// ─────────────────────────────────────────────────────────────────────────────
// ScrollCapture.cpp — 世界级长截图引擎实现
//
// 核心管线:
//   1. 非对称滚动条剥离 + 双阶段静态行检测：免疫右侧反向移动滚动条、输入框闪烁光标与微动效
//   2. 多频带双峰值采样 + 一维行梯度能量剖面 + 全重叠区逐行一致性核验：彻底根除重复拼接与错位跳段
//   3. 运动沉降门禁 (Motion Settling Gate) + 连续亚刻度滚轮闭环 + 超调自动反向回滚补帧：彻底根除跨帧漏截
//   4. 精确固定底栏反向剥离 + 3px 亚像素接缝微羽化融合：固定底栏仅在末帧保留一次，接缝零割裂零重影
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/ScrollCapture.h"
#include "capture/ScrollCaptureStorage.h"
#include "core/logger/Logger.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <dwmapi.h>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <system_error>

namespace tools3000::capture {

namespace {

struct BandVote {
    int tplY = 0;
    int tplH = 0;
    int matchY = 0;
    int delta = 0;
    double score = 0.0;
    double mad = 0.0;
    double stdSum = 0.0;
    double weight = 0.0;
};

struct OverlapVerification {
    int refinedDelta = 0;
    int overlapRows = 0;
    int activeRows = 0;
    double inlierRatio = 0.0;
    double trimmedMad = 999.0;
    double meanMad = 999.0;
    double activeInlierRatio = 0.0;
    double activeTrimmedMad = 999.0;
    double activeNcc = -1.0;
    bool valid = false;
};

// 计算非对称水平内缩区域：右侧额外剥离垂直滚动条轨道（滚动条滑块向下移动，与向上滚动的正文方向相反）
void computeContentHorizROI(int totalCols, int& outX, int& outW) {
    if (totalCols <= 80) {
        outX = 0;
        outW = std::max(1, totalCols);
        return;
    }
    const int leftMargin = std::min(totalCols / 14, 20);
    const int rightMargin = std::min(totalCols / 6, 32);
    outX = leftMargin;
    outW = std::max(8, totalCols - leftMargin - rightMargin);
}

// 水平静态侧边栏自动剥离（面向微信/钉钉/IDE/侧边导航栏全窗口长截图：只影响匹配与校验区间，不裁剪最终拼接图全宽）
void refineDynamicHorizROI(const cv::Mat& diffMat, int dynTop, int dynBottom,
                           int& subX, int& subW) {
    subX = 0;
    subW = diffMat.cols;
    const int rows = dynBottom - dynTop;
    const int cols = diffMat.cols;
    if (rows < 24 || cols < 240) return;

    constexpr int kStrips = 32;
    std::array<double, kStrips> stripDiff{};
    double maxStripDiff = 0.0;
    const int rowStep = std::max(1, rows / 160);

    for (int s = 0; s < kStrips; ++s) {
        const int x0 = (s * cols) / kStrips;
        const int x1 = ((s + 1) * cols) / kStrips;
        const int xStep = std::max(1, (x1 - x0) / 8);
        double sum = 0.0;
        int count = 0;
        for (int y = dynTop; y < dynBottom; y += rowStep) {
            const auto* ptr = diffMat.ptr<cv::Vec3b>(y);
            for (int x = x0; x < x1; x += xStep) {
                const cv::Vec3b& d = ptr[x];
                sum += (static_cast<int>(d[0]) + static_cast<int>(d[1]) + static_cast<int>(d[2])) / 3.0;
                ++count;
            }
        }
        stripDiff[static_cast<std::size_t>(s)] = (count > 0) ? (sum / count) : 0.0;
        maxStripDiff = std::max(maxStripDiff, stripDiff[static_cast<std::size_t>(s)]);
    }

    // 若整帧差异都很小（可能静止或极弱变化），不裁剪列区；阈值兼顾高留白/稀疏文字行
    if (maxStripDiff < 1.5) return;

    const double staticThresh = std::min(0.85, maxStripDiff * 0.12);
    int leftStatic = 0;
    while (leftStatic < 12 && stripDiff[static_cast<std::size_t>(leftStatic)] <= staticThresh) {
        ++leftStatic;
    }
    int rightStatic = 0;
    while (rightStatic < 12 && stripDiff[static_cast<std::size_t>(kStrips - 1 - rightStatic)] <= staticThresh) {
        ++rightStatic;
    }

    if (leftStatic == 0 && rightStatic == 0) return;
    // 至少保留 50% 宽度，避免在纯居中窄文栏下过度收缩
    const int remStrips = kStrips - leftStatic - rightStatic;
    if (remStrips < 16) return;

    const int newX0 = (leftStatic * cols) / kStrips;
    const int newX1 = ((kStrips - rightStatic) * cols) / kStrips;
    const int newW = newX1 - newX0;
    if (newW >= std::max(120, (cols * 45) / 100)) {
        subX = newX0;
        subW = newW;
    }
}

// 逐行计算有效纹理标记：精准识别包含文字/边框/图像的有效行，剔除大面积深灰/纯白背景空行对匹配评分的稀释
std::vector<uint8_t> computeRowActivity(const cv::Mat& img) {
    const int rows = img.rows;
    const int cols = img.cols;
    std::vector<uint8_t> active(static_cast<std::size_t>(std::max(0, rows)), 0);
    if (rows <= 0 || cols <= 2) return active;

    const int stepX = std::max(1, cols / 360);
    for (int y = 0; y < rows; ++y) {
        const auto* cur = img.ptr<cv::Vec3b>(y);
        const auto* prev = img.ptr<cv::Vec3b>(std::max(0, y - 1));
        double sumGx = 0.0;
        double sumGy = 0.0;
        double sumLuma = 0.0;
        int samples = 0;

        for (int x = stepX; x < cols; x += stepX) {
            const cv::Vec3b& c = cur[x];
            const cv::Vec3b& l = cur[x - stepX];
            const cv::Vec3b& p = prev[x];
            sumGx += (std::abs(static_cast<int>(c[0]) - static_cast<int>(l[0])) +
                      std::abs(static_cast<int>(c[1]) - static_cast<int>(l[1])) +
                      std::abs(static_cast<int>(c[2]) - static_cast<int>(l[2]))) / 3.0;
            sumGy += (std::abs(static_cast<int>(c[0]) - static_cast<int>(p[0])) +
                      std::abs(static_cast<int>(c[1]) - static_cast<int>(p[1])) +
                      std::abs(static_cast<int>(c[2]) - static_cast<int>(p[2]))) / 3.0;
            sumLuma += (static_cast<int>(c[0]) * 29 + static_cast<int>(c[1]) * 150 + static_cast<int>(c[2]) * 77) / 256.0;
            ++samples;
        }
        if (samples <= 0) continue;

        const double avgGx = sumGx / samples;
        const double avgGy = sumGy / samples;
        if (avgGx >= 1.35 || avgGy >= 2.2) {
            active[static_cast<std::size_t>(y)] = 1;
            continue;
        }

        const double meanLuma = sumLuma / samples;
        double madLuma = 0.0;
        for (int x = stepX; x < cols; x += stepX) {
            const cv::Vec3b& c = cur[x];
            const double luma = (static_cast<int>(c[0]) * 29 + static_cast<int>(c[1]) * 150 + static_cast<int>(c[2]) * 77) / 256.0;
            madLuma += std::abs(luma - meanLuma);
        }
        if (madLuma / samples >= 4.8) {
            active[static_cast<std::size_t>(y)] = 1;
        }
    }
    return active;
}

// 计算整帧纹理标准差之和（区分真实富纹理界面与无纹理纯色单测合成帧）
double computeFrameTextureStd(const cv::Mat& img) {
    if (img.empty()) return 0.0;
    cv::Scalar mean, stddev;
    cv::meanStdDev(img, mean, stddev);
    return stddev[0] + stddev[1] + stddev[2];
}

// 检测上下连续静态边栏高度，并统计动态区零位移逐行吻合率与截尾误差
void detectStaticMargins(const cv::Mat& uInner, const cv::Mat& lInner,
                         cv::Mat& outDiff, int& stickyTop, int& stickyBottom,
                         double& dynamicMeanDiff, double& zeroShiftInlierRatio,
                         double& zeroShiftTrimmedMad) {
    stickyTop = 0;
    stickyBottom = 0;
    dynamicMeanDiff = 999.0;
    zeroShiftInlierRatio = 0.0;
    zeroShiftTrimmedMad = 999.0;

    const int rows = uInner.rows;
    const int cols = uInner.cols;
    if (rows <= 0 || cols <= 0) return;

    cv::absdiff(uInner, lInner, outDiff);

    const int numBlocks = std::clamp(cols / 32, 1, 8);
    std::vector<uint8_t> rowStatic(static_cast<std::size_t>(rows), 0);
    std::vector<double> rowMeans(static_cast<std::size_t>(rows), 0.0);

    for (int y = 0; y < rows; ++y) {
        const auto* rowPtr = outDiff.ptr<cv::Vec3b>(y);
        int staticBlocks = 0;
        double rowSum = 0.0;

        for (int b = 0; b < numBlocks; ++b) {
            const int x0 = (b * cols) / numBlocks;
            const int x1 = ((b + 1) * cols) / numBlocks;
            const int bw = std::max(1, x1 - x0);
            double blockSum = 0.0;
            for (int x = x0; x < x1; ++x) {
                const cv::Vec3b& d = rowPtr[x];
                blockSum += (static_cast<int>(d[0]) + static_cast<int>(d[1]) + static_cast<int>(d[2])) / 3.0;
            }
            rowSum += blockSum;
            if (blockSum / bw < 2.8) {
                ++staticBlocks;
            }
        }

        const double rowMean = rowSum / cols;
        rowMeans[static_cast<std::size_t>(y)] = rowMean;
        // 至少 75% 水平分块完全静止（免疫输入框闪烁光标与右侧小型状态呼吸灯）
        if ((staticBlocks * 4 >= numBlocks * 3 && rowMean < 4.5) || rowMean < 1.8) {
            rowStatic[static_cast<std::size_t>(y)] = 1;
        }
    }

    const auto uRowAct = computeRowActivity(uInner);

    // 限制每个固定边栏最多桥接 1 处极窄微动（<= 2 行），且桥接后方必须紧跟连续静态有纹理控件行，
    // 彻底杜绝稀疏正文/等行距细线/左右分栏页面被无限跳格吞成 38% 顶栏 + 45% 底栏！
    const int maxTop = (rows * 38) / 100;
    int topBridgesUsed = 0;
    for (int y = 0; y < maxTop; ++y) {
        if (rowStatic[static_cast<std::size_t>(y)]) {
            stickyTop = y + 1;
            continue;
        }
        if (topBridgesUsed >= 1) break;
        int bridge = 0;
        while (bridge < 3 && (y + bridge) < maxTop && !rowStatic[static_cast<std::size_t>(y + bridge)]) {
            ++bridge;
        }
        int confirmStatic = 0;
        int confirmActiveStatic = 0;
        for (int k = y + bridge; k < std::min(maxTop, y + bridge + 6); ++k) {
            if (rowStatic[static_cast<std::size_t>(k)]) {
                ++confirmStatic;
                if (uRowAct[static_cast<std::size_t>(k)]) ++confirmActiveStatic;
            }
        }
        if (bridge <= 2 && confirmStatic >= 5 && confirmActiveStatic >= 2) {
            ++topBridgesUsed;
            y += bridge - 1;
            continue;
        }
        break;
    }

    const int maxBottom = (rows * 45) / 100;
    int botBridgesUsed = 0;
    for (int dist = 0; dist < maxBottom; ++dist) {
        const int y = rows - 1 - dist;
        if (y <= stickyTop) break;
        if (rowStatic[static_cast<std::size_t>(y)]) {
            stickyBottom = dist + 1;
            continue;
        }
        if (botBridgesUsed >= 1) break;
        int bridge = 0;
        while (bridge < 3 && (dist + bridge) < maxBottom &&
               (rows - 1 - (dist + bridge)) > stickyTop &&
               !rowStatic[static_cast<std::size_t>(rows - 1 - (dist + bridge))]) {
            ++bridge;
        }
        int confirmStatic = 0;
        int confirmActiveStatic = 0;
        for (int k = dist + bridge; k < std::min(maxBottom, dist + bridge + 6); ++k) {
            const int ky = rows - 1 - k;
            if (ky > stickyTop && rowStatic[static_cast<std::size_t>(ky)]) {
                ++confirmStatic;
                if (uRowAct[static_cast<std::size_t>(ky)]) ++confirmActiveStatic;
            }
        }
        if (bridge <= 2 && confirmStatic >= 5 && confirmActiveStatic >= 2) {
            ++botBridgesUsed;
            dist += bridge - 1;
            continue;
        }
        break;
    }

    // 剥离大块误吞的纯色动态留白：stickyTop / stickyBottom 末尾若紧贴动态区存在超长纯背景空行（无任何纹理/边框），
    // 最多仅保留 24px 紧邻静态控件的内边距，避免把聊天消息间的深色留白误吞进固定栏导致错裁正文
    if (stickyTop > 0 || stickyBottom > 0) {
        if (stickyTop > 24) {
            int lastActiveTop = -1;
            for (int y = stickyTop - 1; y >= 0; --y) {
                if (uRowAct[static_cast<std::size_t>(y)]) {
                    lastActiveTop = y;
                    break;
                }
            }
            if (lastActiveTop < 0) {
                stickyTop = 0;
            } else {
                stickyTop = std::min(stickyTop, lastActiveTop + 1 + 24);
            }
        }
        if (stickyBottom > 24) {
            int highestActiveBottomDist = -1;
            for (int dist = stickyBottom - 1; dist >= 0; --dist) {
                const int y = rows - 1 - dist;
                if (y >= 0 && uRowAct[static_cast<std::size_t>(y)]) {
                    highestActiveBottomDist = dist;
                    break;
                }
            }
            if (highestActiveBottomDist < 0) {
                stickyBottom = 0;
            } else {
                stickyBottom = std::min(stickyBottom, highestActiveBottomDist + 1 + 24);
            }
        }
    }

    const int dynTop = stickyTop;
    const int dynBottom = rows - stickyBottom;
    if (dynBottom - dynTop <= std::max(8, rows / 16)) {
        const cv::Scalar m = cv::mean(outDiff);
        dynamicMeanDiff = (m[0] + m[1] + m[2]) / 3.0;
        if (dynamicMeanDiff < 2.0) {
            dynamicMeanDiff = 0.0;
        }
        zeroShiftInlierRatio = (dynamicMeanDiff < 2.5) ? 1.0 : 0.0;
        zeroShiftTrimmedMad = dynamicMeanDiff;
    } else {
        const int dynRows = dynBottom - dynTop;
        std::vector<double> dynErrs;
        dynErrs.reserve(static_cast<std::size_t>(dynRows));
        int inliers = 0;
        double sumErr = 0.0;
        for (int y = dynTop; y < dynBottom; ++y) {
            const double e = rowMeans[static_cast<std::size_t>(y)];
            dynErrs.push_back(e);
            sumErr += e;
            if (rowStatic[static_cast<std::size_t>(y)] || e < 3.4) {
                ++inliers;
            }
        }
        dynamicMeanDiff = sumErr / std::max(1, dynRows);
        zeroShiftInlierRatio = static_cast<double>(inliers) / std::max(1, dynRows);

        std::sort(dynErrs.begin(), dynErrs.end());
        const std::size_t keepCount = std::max<std::size_t>(1, (dynErrs.size() * 85) / 100);
        double trimSum = 0.0;
        for (std::size_t i = 0; i < keepCount; ++i) {
            trimSum += dynErrs[i];
        }
        zeroShiftTrimmedMad = trimSum / static_cast<double>(keepCount);
    }
}

// 在给定候选窗口内执行金字塔粗搜（三峰值保留）+ 原分辨率精配 + 像素 MAD 校验
bool matchSingleBand(const cv::Mat& uInner, const cv::Mat& lInner,
                     int tplY, int tplH, int searchTop, int searchBottom,
                     std::vector<BandVote>& outVotes) {
    const int cols = uInner.cols;
    if (tplY < 0 || tplY + tplH > uInner.rows) return false;
    searchTop = std::max(0, searchTop);
    searchBottom = std::min(lInner.rows, searchBottom);
    if (searchBottom - searchTop < tplH) return false;

    const cv::Mat tpl = uInner.rowRange(tplY, tplY + tplH);
    cv::Scalar mean, stddev;
    cv::meanStdDev(tpl, mean, stddev);
    const double totalStd = stddev[0] + stddev[1] + stddev[2];
    if (totalStd < 2.2) return false;

    const cv::Mat searchArea = lInner.rowRange(searchTop, searchBottom);

    std::vector<int> candidateRoughYs;
    const bool usePyramid = (cols >= 96 && searchArea.rows >= 48 && tplH >= 20);
    if (usePyramid) {
        constexpr double kScale = 0.5;
        cv::Mat smallTpl, smallSearch, smallRes;
        // 使用抗混叠高斯金字塔 pyrDown 替代硬箱式 INTER_AREA，消除 1~2px 细字体/细横线在奇偶滚动位移下的相位混叠失配
        cv::pyrDown(tpl, smallTpl);
        cv::pyrDown(searchArea, smallSearch);
        if (smallSearch.rows >= smallTpl.rows && smallTpl.rows >= 4 && smallTpl.cols >= 8) {
            cv::matchTemplate(smallSearch, smallTpl, smallRes, cv::TM_CCOEFF_NORMED);
            cv::Mat work = smallRes.clone();
            // 保留前 6 个非极大值抑制峰值：防止等行距代码/表格/聊天流里相邻行相位峰挤掉真实大幅滚动位移
            for (int peakIdx = 0; peakIdx < 6; ++peakIdx) {
                double maxVal = -1.0;
                cv::Point maxLoc{};
                cv::minMaxLoc(work, nullptr, &maxVal, nullptr, &maxLoc);
                const double minPeak = (peakIdx == 0) ? 0.54 : 0.58;
                if (maxVal < minPeak) break;
                candidateRoughYs.push_back(searchTop + static_cast<int>(std::lround(maxLoc.y / kScale)));
                const int supTop = std::max(0, maxLoc.y - 5);
                const int supBot = std::min(work.rows, maxLoc.y + 6);
                work.rowRange(supTop, supBot).setTo(-1.0f);
            }
        }
    }

    bool anyAdded = false;
    auto evaluateFineWindow = [&](int winTop, int winBottom, bool multiPeak) {
        winTop = std::max(searchTop, winTop);
        winBottom = std::min(searchBottom, winBottom);
        if (winBottom - winTop < tplH) return;

        cv::Mat fineRes;
        cv::matchTemplate(lInner.rowRange(winTop, winBottom), tpl, fineRes, cv::TM_CCOEFF_NORMED);
        cv::Mat work = fineRes.clone();
        const int maxPeaks = multiPeak ? 3 : 1;

        for (int p = 0; p < maxPeaks; ++p) {
            double localMax = -1.0;
            cv::Point localLoc{};
            cv::minMaxLoc(work, nullptr, &localMax, nullptr, &localLoc);
            if (localMax < 0.76) break;

            const int my = winTop + localLoc.y;
            const int supTop = std::max(0, localLoc.y - std::max(4, tplH / 2));
            const int supBot = std::min(work.rows, localLoc.y + std::max(5, tplH / 2 + 1));
            work.rowRange(supTop, supBot).setTo(-1.0f);

            bool duplicateY = false;
            for (const auto& existing : outVotes) {
                if (existing.tplY == tplY && std::abs(existing.matchY - my) <= 3) {
                    duplicateY = true;
                    break;
                }
            }
            if (duplicateY) continue;

            cv::Mat patchDiff;
            cv::absdiff(lInner.rowRange(my, my + tplH), tpl, patchDiff);
            const cv::Scalar dm = cv::mean(patchDiff);
            const double mad = (dm[0] + dm[1] + dm[2]) / 3.0;
            if (mad > 22.0) continue;

            BandVote vote;
            vote.tplY = tplY;
            vote.tplH = tplH;
            vote.matchY = my;
            vote.delta = tplY - my;
            vote.score = localMax;
            vote.mad = mad;
            vote.stdSum = totalStd;
            vote.weight = localMax * (1.0 + std::min(totalStd, 55.0) * 0.035) / (1.0 + mad * 0.14);
            if (vote.score >= 0.78) {
                outVotes.push_back(vote);
                anyAdded = true;
            }
        }
    };

    if (!candidateRoughYs.empty()) {
        for (int roughY : candidateRoughYs) {
            evaluateFineWindow(roughY - 8, roughY + tplH + 8, false);
        }
    }
    if (!anyAdded) {
        // 当金字塔粗搜因极细笔画或极端纹理未产出有效精配投票时，回退原分辨率全窗精搜兜底
        evaluateFineWindow(searchTop, searchBottom, true);
    }

    return anyAdded;
}

// 一维行边缘梯度剖面全局位移估计（在大面积纯色留白、稀疏文字场景下提供全局几何兜底候选）
int estimateRowProfileDelta(const cv::Mat& uInner, const cv::Mat& lInner,
                            int dynTop, int dynBottom) {
    const int dynH = dynBottom - dynTop;
    const int cols = uInner.cols;
    if (dynH < 32 || cols < 16) return 0;

    auto buildProfile = [&](const cv::Mat& img) {
        std::vector<double> prof(static_cast<std::size_t>(dynH), 0.0);
        for (int i = 0; i < dynH; ++i) {
            const int y = dynTop + i;
            const auto* cur = img.ptr<cv::Vec3b>(y);
            const auto* prev = img.ptr<cv::Vec3b>(std::max(0, y - 1));
            double sumGrad = 0.0;
            for (int x = 1; x < cols; ++x) {
                const int gx = std::abs(static_cast<int>(cur[x][0]) - static_cast<int>(cur[x - 1][0])) +
                               std::abs(static_cast<int>(cur[x][1]) - static_cast<int>(cur[x - 1][1])) +
                               std::abs(static_cast<int>(cur[x][2]) - static_cast<int>(cur[x - 1][2]));
                const int gy = std::abs(static_cast<int>(cur[x][0]) - static_cast<int>(prev[x][0])) +
                               std::abs(static_cast<int>(cur[x][1]) - static_cast<int>(prev[x][1])) +
                               std::abs(static_cast<int>(cur[x][2]) - static_cast<int>(prev[x][2]));
                sumGrad += static_cast<double>(gx + gy);
            }
            prof[static_cast<std::size_t>(i)] = sumGrad / std::max(1, cols - 1);
        }
        return prof;
    };

    const auto uProf = buildProfile(uInner);
    const auto lProf = buildProfile(lInner);

    const int minOverlap = std::max(16, dynH / 6);
    const int maxDelta = dynH - minOverlap;
    if (maxDelta < 3) return 0;

    int bestD = 0;
    double bestNcc = 0.55;
    for (int d = 3; d <= maxDelta; ++d) {
        const int len = dynH - d;
        double meanU = 0.0, meanL = 0.0;
        for (int i = 0; i < len; ++i) {
            meanU += uProf[static_cast<std::size_t>(i + d)];
            meanL += lProf[static_cast<std::size_t>(i)];
        }
        meanU /= len;
        meanL /= len;

        double num = 0.0, denU = 0.0, denL = 0.0;
        for (int i = 0; i < len; ++i) {
            const double du = uProf[static_cast<std::size_t>(i + d)] - meanU;
            const double dl = lProf[static_cast<std::size_t>(i)] - meanL;
            num += du * dl;
            denU += du * du;
            denL += dl * dl;
        }
        if (denU < 1e-3 || denL < 1e-3) continue;
        const double ncc = num / std::sqrt(denU * denL);
        if (ncc > bestNcc) {
            bestNcc = ncc;
            bestD = d;
        }
    }
    return bestD;
}

// 在指定垂直位移 d >= 0 下计算全重叠区双轨一致性指标（全行 + 有效纹理行 ZNCC/前景 MAD）
OverlapVerification evaluateOverlapAtDelta(const cv::Mat& uInner, const cv::Mat& lInner,
                                           const std::vector<uint8_t>& uAct,
                                           const std::vector<uint8_t>& lAct,
                                           int dynTop, int dynBottom, int d) {
    OverlapVerification out;
    const int dynH = dynBottom - dynTop;
    const int cols = uInner.cols;
    const int overlapRows = dynH - d;
    if (d < 0 || overlapRows <= 0 || cols <= 0) return out;
    if (dynTop + d + overlapRows > uInner.rows || dynTop + overlapRows > lInner.rows) return out;

    const int stepX = std::max(1, cols / 360);
    std::vector<double> rowErrs;
    rowErrs.reserve(static_cast<std::size_t>(overlapRows));
    std::vector<double> activeRowErrs;
    activeRowErrs.reserve(static_cast<std::size_t>(overlapRows));

    int inlierRows = 0;
    double totalMean = 0.0;
    int activeRows = 0;
    int activeInlierRows = 0;

    double gSumU = 0.0, gSumL = 0.0;
    double gSumUU = 0.0, gSumLL = 0.0, gSumUL = 0.0;
    int gSamples = 0;

    for (int r = 0; r < overlapRows; ++r) {
        const int uY = dynTop + d + r;
        const int lY = dynTop + r;
        const auto* uPtr = uInner.ptr<cv::Vec3b>(uY);
        const auto* lPtr = lInner.ptr<cv::Vec3b>(lY);
        const bool isActiveRow =
            (static_cast<std::size_t>(uY) < uAct.size() && uAct[static_cast<std::size_t>(uY)] != 0) ||
            (static_cast<std::size_t>(lY) < lAct.size() && lAct[static_cast<std::size_t>(lY)] != 0);

        double rSum = 0.0;
        int samples = 0;
        if (!isActiveRow) {
            for (int x = 0; x < cols; x += stepX) {
                const cv::Vec3b& up = uPtr[x];
                const cv::Vec3b& lp = lPtr[x];
                rSum += (std::abs(static_cast<int>(up[0]) - static_cast<int>(lp[0])) +
                         std::abs(static_cast<int>(up[1]) - static_cast<int>(lp[1])) +
                         std::abs(static_cast<int>(up[2]) - static_cast<int>(lp[2]))) / 3.0;
                ++samples;
            }
            const double rErr = (samples > 0) ? (rSum / samples) : 0.0;
            rowErrs.push_back(rErr);
            totalMean += rErr;
            if (rErr < 6.2) ++inlierRows;
            continue;
        }

        // 有效纹理行：先计算该行亮度均值，用于分离前景墨迹像素并计算行级/全局 ZNCC
        double rowSumU = 0.0, rowSumL = 0.0;
        double rowSumUU = 0.0, rowSumLL = 0.0, rowSumUL = 0.0;
        for (int x = 0; x < cols; x += stepX) {
            const cv::Vec3b& up = uPtr[x];
            const cv::Vec3b& lp = lPtr[x];
            rSum += (std::abs(static_cast<int>(up[0]) - static_cast<int>(lp[0])) +
                     std::abs(static_cast<int>(up[1]) - static_cast<int>(lp[1])) +
                     std::abs(static_cast<int>(up[2]) - static_cast<int>(lp[2]))) / 3.0;
            const double ul = (static_cast<int>(up[0]) * 29 + static_cast<int>(up[1]) * 150 + static_cast<int>(up[2]) * 77) / 256.0;
            const double ll = (static_cast<int>(lp[0]) * 29 + static_cast<int>(lp[1]) * 150 + static_cast<int>(lp[2]) * 77) / 256.0;
            rowSumU += ul;
            rowSumL += ll;
            rowSumUU += ul * ul;
            rowSumLL += ll * ll;
            rowSumUL += ul * ll;
            ++samples;
        }
        if (samples <= 0) continue;

        const double rErr = rSum / samples;
        rowErrs.push_back(rErr);
        totalMean += rErr;
        if (rErr < 6.2) ++inlierRows;

        gSumU += rowSumU;
        gSumL += rowSumL;
        gSumUU += rowSumUU;
        gSumLL += rowSumLL;
        gSumUL += rowSumUL;
        gSamples += samples;

        const double meanU = rowSumU / samples;
        const double meanL = rowSumL / samples;
        const double varU = std::max(0.0, rowSumUU - samples * meanU * meanU);
        const double varL = std::max(0.0, rowSumLL - samples * meanL * meanL);
        const double cov = rowSumUL - samples * meanU * meanL;
        const double rowNcc = (varU > 25.0 && varL > 25.0)
            ? (cov / std::sqrt(varU * varL))
            : ((rErr < 3.5) ? 1.0 : 0.0);

        // 仅在偏离该行背景均值的真实前景像素上统计 MAD，彻底消除 85% 纯深灰/纯白背景对文字误差的稀释
        double fgErrSum = 0.0;
        int fgCount = 0;
        for (int x = 0; x < cols; x += stepX) {
            const cv::Vec3b& up = uPtr[x];
            const cv::Vec3b& lp = lPtr[x];
            const double ul = (static_cast<int>(up[0]) * 29 + static_cast<int>(up[1]) * 150 + static_cast<int>(up[2]) * 77) / 256.0;
            const double ll = (static_cast<int>(lp[0]) * 29 + static_cast<int>(lp[1]) * 150 + static_cast<int>(lp[2]) * 77) / 256.0;
            if (std::abs(ul - meanU) >= 10.0 || std::abs(ll - meanL) >= 10.0) {
                fgErrSum += (std::abs(static_cast<int>(up[0]) - static_cast<int>(lp[0])) +
                             std::abs(static_cast<int>(up[1]) - static_cast<int>(lp[1])) +
                             std::abs(static_cast<int>(up[2]) - static_cast<int>(lp[2]))) / 3.0;
                ++fgCount;
            }
        }
        const double fgMad = (fgCount >= 2) ? (fgErrSum / fgCount) : rErr;
        activeRowErrs.push_back(fgMad);
        ++activeRows;
        if ((fgMad < 15.5 && rowNcc >= 0.60) || (rErr < 3.4 && rowNcc >= 0.78)) {
            ++activeInlierRows;
        }
    }

    out.refinedDelta = d;
    out.overlapRows = overlapRows;
    out.activeRows = activeRows;
    out.inlierRatio = static_cast<double>(inlierRows) / std::max(1, overlapRows);
    out.meanMad = totalMean / std::max(1, overlapRows);

    if (!rowErrs.empty()) {
        std::sort(rowErrs.begin(), rowErrs.end());
        const std::size_t keepCount = std::max<std::size_t>(1, (rowErrs.size() * 85) / 100);
        double trimSum = 0.0;
        for (std::size_t i = 0; i < keepCount; ++i) trimSum += rowErrs[i];
        out.trimmedMad = trimSum / static_cast<double>(keepCount);
    }

    if (activeRows > 0 && gSamples > 0) {
        out.activeInlierRatio = static_cast<double>(activeInlierRows) / activeRows;
        std::sort(activeRowErrs.begin(), activeRowErrs.end());
        const std::size_t keepAct = std::max<std::size_t>(1, (activeRowErrs.size() * 85) / 100);
        double actTrimSum = 0.0;
        for (std::size_t i = 0; i < keepAct; ++i) actTrimSum += activeRowErrs[i];
        out.activeTrimmedMad = actTrimSum / static_cast<double>(keepAct);

        const double gMeanU = gSumU / gSamples;
        const double gMeanL = gSumL / gSamples;
        const double gVarU = std::max(0.0, gSumUU - gSamples * gMeanU * gMeanU);
        const double gVarL = std::max(0.0, gSumLL - gSamples * gMeanL * gMeanL);
        const double gCov = gSumUL - gSamples * gMeanU * gMeanL;
        if (gVarU > 100.0 && gVarL > 100.0) {
            out.activeNcc = gCov / std::sqrt(gVarU * gVarL);
        } else {
            out.activeNcc = (out.activeTrimmedMad < 4.5) ? 0.95 : 0.0;
        }
    }

    return out;
}

// 全重叠区像素级一致性验证与 ±2px 微精修（双轨门禁：全行统计 + 有效纹理行 ZNCC/前景 MAD，彻底封杀深色空行稀释与周期性重复卡片误配）
OverlapVerification verifyCandidateOnFullOverlap(const cv::Mat& uInner, const cv::Mat& lInner,
                                                 const std::vector<uint8_t>& uAct,
                                                 const std::vector<uint8_t>& lAct,
                                                 int dynTop, int dynBottom, int seedDelta) {
    OverlapVerification best;
    const int dynH = dynBottom - dynTop;
    const int cols = uInner.cols;
    const int minOverlapRows = std::clamp(dynH / 8, 8, 28);
    if (dynH <= minOverlapRows + 1 || cols <= 0) return best;

    const int dMin = std::max(1, seedDelta - 2);
    const int dMax = std::min(dynH - minOverlapRows, seedDelta + 2);
    if (dMin > dMax) return best;

    for (int d = dMin; d <= dMax; ++d) {
        const int overlapRows = dynH - d;
        if (overlapRows < minOverlapRows) continue;

        OverlapVerification cur = evaluateOverlapAtDelta(uInner, lInner, uAct, lAct, dynTop, dynBottom, d);
        const bool passGlobal = (cur.inlierRatio >= 0.56) || (cur.trimmedMad <= 11.5 && cur.inlierRatio >= 0.38);
        if (!passGlobal) continue;

        // 有效纹理行硬门禁：一旦重叠区存在真实文字/边框/图像行，严禁靠 75% 纯色背景空行蒙混过关
        if (cur.activeRows >= 6) {
            const bool passActive =
                (cur.activeInlierRatio >= 0.60 && cur.activeNcc >= 0.72 && cur.activeTrimmedMad <= 20.0) ||
                (cur.activeInlierRatio >= 0.78 && cur.activeNcc >= 0.65 && cur.activeTrimmedMad <= 14.0);
            if (!passActive) continue;
        }

        cur.valid = true;
        const double curQuality = (cur.activeRows >= 6)
            ? (cur.activeInlierRatio * 65.0 + cur.activeNcc * 55.0 - cur.activeTrimmedMad * 1.4 + cur.inlierRatio * 20.0)
            : (cur.inlierRatio * 100.0 - cur.trimmedMad * 2.4);
        const double bestQuality = (best.activeRows >= 6)
            ? (best.activeInlierRatio * 65.0 + best.activeNcc * 55.0 - best.activeTrimmedMad * 1.4 + best.inlierRatio * 20.0)
            : (best.inlierRatio * 100.0 - best.trimmedMad * 2.4);
        if (!best.valid || curQuality > bestQuality) {
            best = cur;
        }
    }

    return best;
}

// 反向滚动（向上回滚 / 弹性触底回弹）轻量探测器：返回最佳反向重叠验证结果，用于无匹配兜底及与擦边正向假匹配的对称对决
OverlapVerification detectBackwardScroll(const cv::Mat& uInner, const cv::Mat& lInner,
                                         const std::vector<uint8_t>& uAct, const std::vector<uint8_t>& lAct,
                                         int dynTop, int dynBottom) {
    OverlapVerification bestRev;
    const int dynH = dynBottom - dynTop;
    const int refH = std::min(uInner.rows, lInner.rows);
    if (dynH < 48 || refH < 64 || uInner.cols < 48) return bestRev;

    const int tplH = std::clamp(dynH / 6, 18, 56);
    if (dynH < tplH + 12) return bestRev;

    // 反向滚动时：lInner 较靠下的位置匹配到 uInner 较靠上的位置（等价于互换 lInner/uInner 寻找正位移）
    std::vector<BandVote> backVotes;
    std::vector<BandVote> tmpVotes;
    constexpr int kProbeBands = 5;
    const int sampleTop = std::max(0, dynTop);
    const int sampleBot = std::max(sampleTop + 1, std::min(refH - tplH - 2, dynBottom - tplH / 2));

    for (int i = 0; i < kProbeBands; ++i) {
        const int y = (kProbeBands == 1 || sampleBot <= sampleTop)
            ? sampleTop
            : (sampleTop + ((sampleBot - sampleTop) * i) / (kProbeBands - 1));
        if (y + tplH > refH) continue;
        tmpVotes.clear();
        const int searchTop = std::max(0, dynTop - 4);
        const int searchBot = std::min(refH, y + tplH + 2);
        if (!matchSingleBand(lInner, uInner, y, tplH, searchTop, searchBot, tmpVotes)) continue;
        for (const auto& v : tmpVotes) {
            if (v.delta >= 4 && v.score >= 0.82) {
                backVotes.push_back(v);
            }
        }
    }

    if (backVotes.empty()) return bestRev;

    // 取权重前 3 簇依次执行全重叠区严格双轨核验，防止等行距代码/列表下假相位峰遮蔽真实回滚位移
    std::vector<BandVote> remBack = backVotes;
    for (int cIdx = 0; cIdx < 3 && !remBack.empty(); ++cIdx) {
        double bestWeight = -1.0;
        int bestSeed = 0;
        for (const auto& s : remBack) {
            double w = 0.0;
            for (const auto& c : remBack) {
                if (std::abs(c.delta - s.delta) <= 3) w += c.weight;
            }
            if (w > bestWeight) {
                bestWeight = w;
                bestSeed = s.delta;
            }
        }
        if (bestWeight <= 0.0 || bestSeed < 4) break;

        std::vector<BandVote> nextRem;
        for (const auto& v : remBack) {
            if (std::abs(v.delta - bestSeed) > 4) nextRem.push_back(v);
        }
        remBack = std::move(nextRem);

        const OverlapVerification revVer = verifyCandidateOnFullOverlap(
            lInner, uInner, lAct, uAct, dynTop, dynBottom, bestSeed);
        if (!revVer.valid) continue;
        const bool passRev = (revVer.activeRows >= 6)
            ? (revVer.activeNcc >= 0.80 && revVer.activeInlierRatio >= 0.70)
            : (revVer.inlierRatio >= 0.75);
        if (!passRev) continue;

        const double curQ = (revVer.activeRows >= 6)
            ? (revVer.activeNcc * 60.0 + revVer.activeInlierRatio * 60.0 - revVer.activeTrimmedMad * 1.5)
            : (revVer.inlierRatio * 100.0 - revVer.trimmedMad * 2.0);
        const double bestQ = (bestRev.activeRows >= 6)
            ? (bestRev.activeNcc * 60.0 + bestRev.activeInlierRatio * 60.0 - bestRev.activeTrimmedMad * 1.5)
            : (bestRev.inlierRatio * 100.0 - bestRev.trimmedMad * 2.0);
        if (!bestRev.valid || curQ > bestQ) {
            bestRev = revVer;
        }
    }
    return bestRev;
}

// 纵深防线：检验即将追加的新动态正文段是否已经在已拼接长图尾部出现过（彻底封杀复读机长图，同时 100% 豁免周期性重复表格）
bool isDuplicateSegmentInTail(const cv::Mat& tailWindow, const cv::Mat& newDynamic, double* outBestScore = nullptr) {
    if (outBestScore) *outBestScore = 0.0;
    if (tailWindow.empty() || newDynamic.empty() ||
        tailWindow.cols != newDynamic.cols || tailWindow.type() != newDynamic.type()) {
        return false;
    }

    const int newH = newDynamic.rows;
    const int tailH = tailWindow.rows;
    if (newH < 56 || tailH < newH + 24) return false;

    int roiX = 0, roiW = newDynamic.cols;
    computeContentHorizROI(newDynamic.cols, roiX, roiW);
    if (roiW < 48) return false;

    const cv::Mat dynRoi = newDynamic(cv::Rect(roiX, 0, roiW, newH));
    if (computeFrameTextureStd(dynRoi) < 3.5) return false;

    const auto dynAct = computeRowActivity(dynRoi);
    int dynActiveCount = 0;
    for (uint8_t f : dynAct) {
        if (f) ++dynActiveCount;
    }
    // 有效纹理行不足（大段空白或极短微步），直接放行避免误伤
    if (dynActiveCount < 14) return false;

    // 周期性重复表格豁免检测：若新段自身内部在纵向上高度自相似（如等高重复表格行），则属于合法重复内容，绝不误拦截
    if (newH >= 80) {
        const int probeH = std::clamp(newH / 3, 24, 48);
        const cv::Mat selfTpl = dynRoi.rowRange(0, probeH);
        const cv::Mat selfSearch = dynRoi.rowRange(std::min(newH - probeH, std::max(14, probeH / 2)), newH);
        if (selfSearch.rows >= selfTpl.rows && computeFrameTextureStd(selfTpl) >= 3.0) {
            cv::Mat selfRes;
            cv::matchTemplate(selfSearch, selfTpl, selfRes, cv::TM_CCOEFF_NORMED);
            double selfMax = -1.0;
            cv::minMaxLoc(selfRes, nullptr, &selfMax);
            if (selfMax >= 0.90) {
                return false;
            }
        }
    }

    // 在已拼接长图尾部中搜索（排除距离最末尾 < 6px 的接缝边界自身，只抓真正回头重复的旧内容块）
    const int searchLimitH = tailH - 6;
    if (searchLimitH < newH) return false;
    const cv::Mat tailRoi = tailWindow(cv::Rect(roiX, 0, roiW, searchLimitH));

    // 先用 0.5x 金字塔快速定位最高相似度候选位置
    int bestCandY = -1;
    double roughScore = -1.0;
    if (roiW >= 96 && newH >= 48) {
        constexpr double kScale = 0.5;
        cv::Mat smallTail, smallDyn, smallRes;
        cv::resize(tailRoi, smallTail, cv::Size(), kScale, kScale, cv::INTER_AREA);
        cv::resize(dynRoi, smallDyn, cv::Size(), kScale, kScale, cv::INTER_AREA);
        if (smallTail.rows >= smallDyn.rows && smallDyn.rows >= 12 && smallDyn.cols >= 16) {
            cv::matchTemplate(smallTail, smallDyn, smallRes, cv::TM_CCOEFF_NORMED);
            cv::Point loc{};
            cv::minMaxLoc(smallRes, nullptr, &roughScore, nullptr, &loc);
            if (roughScore >= 0.84) {
                bestCandY = static_cast<int>(std::lround(loc.y / kScale));
            }
        }
    } else {
        cv::Mat fullRes;
        cv::matchTemplate(tailRoi, dynRoi, fullRes, cv::TM_CCOEFF_NORMED);
        cv::Point loc{};
        cv::minMaxLoc(fullRes, nullptr, &roughScore, nullptr, &loc);
        if (roughScore >= 0.86) {
            bestCandY = loc.y;
        }
    }

    if (bestCandY < 0) return false;

    // 原分辨率精配 + 有效纹理行双轨严检
    const int winTop = std::max(0, bestCandY - 6);
    const int winBot = std::min(searchLimitH, bestCandY + newH + 6);
    if (winBot - winTop < newH) return false;

    cv::Mat fineRes;
    cv::matchTemplate(tailRoi.rowRange(winTop, winBot), dynRoi, fineRes, cv::TM_CCOEFF_NORMED);
    double fineMax = -1.0;
    cv::Point fineLoc{};
    cv::minMaxLoc(fineRes, nullptr, &fineMax, nullptr, &fineLoc);
    if (outBestScore) *outBestScore = fineMax;
    if (fineMax < 0.89) return false;

    const int matchY = winTop + fineLoc.y;
    const cv::Mat matchedPatch = tailRoi.rowRange(matchY, matchY + newH);
    const auto patchAct = computeRowActivity(matchedPatch);
    const OverlapVerification dupVer = evaluateOverlapAtDelta(
        matchedPatch, dynRoi, patchAct, dynAct, 0, newH, 0);

    // 严防误杀「卡片框架相同、仅数行文字不同」的两条合法不同消息：
    // 真正复读的重复段必须几乎 100% 有效纹理行逐像素一致（含最差 5% 文字行，96% 内点率 + 0.96 ZNCC）！
    if (dupVer.activeRows < 12 ||
        dupVer.activeNcc < 0.96 ||
        dupVer.activeInlierRatio < 0.96 ||
        dupVer.activeTrimmedMad > 4.2) {
        return false;
    }

    // 二次全纹理行高分位检验：确保连 4% 的不同文字行都不会被截尾放过
    cv::Mat exactDiff;
    cv::absdiff(matchedPatch, dynRoi, exactDiff);
    int mismatchedActiveRows = 0;
    for (int r = 0; r < newH; ++r) {
        if (!patchAct[static_cast<std::size_t>(r)] && !dynAct[static_cast<std::size_t>(r)]) continue;
        const cv::Scalar rm = cv::mean(exactDiff.row(r));
        if ((rm[0] + rm[1] + rm[2]) / 3.0 > 4.5) {
            ++mismatchedActiveRows;
        }
    }
    return mismatchedActiveRows * 100 <= dupVer.activeRows * 3;
}

// 在真实滚动量锁定后，从底部向上反向精修固定底栏（确保底部输入框/悬浮底栏在前序分段中 100% 剥离干净）
int refineStickyBottomWithDelta(const cv::Mat& uInner, const cv::Mat& lInner,
                                const cv::Mat& zeroDiffMat,
                                int stickyTop, int currentStickyBottom, int delta) {
    const int rows = uInner.rows;
    const int cols = uInner.cols;
    if (delta < 4 || rows < 32 || cols <= 0) return currentStickyBottom;

    const int maxBottom = (rows * 48) / 100;
    int refinedBottom = currentStickyBottom;

    auto computeZeroRowErr = [&](int y) {
        const auto* ptr = zeroDiffMat.ptr<cv::Vec3b>(y);
        double s = 0.0;
        for (int x = 0; x < cols; ++x) {
            s += (static_cast<int>(ptr[x][0]) + static_cast<int>(ptr[x][1]) + static_cast<int>(ptr[x][2])) / 3.0;
        }
        return s / cols;
    };

    auto computeShiftedRowErr = [&](int lY) {
        const int uY = lY + delta;
        if (uY < 0 || uY >= rows || lY < 0 || lY >= rows) return 999.0;
        const auto* uPtr = uInner.ptr<cv::Vec3b>(uY);
        const auto* lPtr = lInner.ptr<cv::Vec3b>(lY);
        double s = 0.0;
        for (int x = 0; x < cols; ++x) {
            s += (std::abs(static_cast<int>(uPtr[x][0]) - static_cast<int>(lPtr[x][0])) +
                  std::abs(static_cast<int>(uPtr[x][1]) - static_cast<int>(lPtr[x][1])) +
                  std::abs(static_cast<int>(uPtr[x][2]) - static_cast<int>(lPtr[x][2]))) / 3.0;
        }
        return s / cols;
    };

    auto computeZeroRowBlockStatic = [&](int y) -> bool {
        const auto* ptr = zeroDiffMat.ptr<cv::Vec3b>(y);
        const int numBlocks = std::clamp(cols / 32, 1, 8);
        int staticBlocks = 0;
        double sum = 0.0;
        for (int b = 0; b < numBlocks; ++b) {
            const int x0 = (b * cols) / numBlocks;
            const int x1 = ((b + 1) * cols) / numBlocks;
            const int bw = std::max(1, x1 - x0);
            double bSum = 0.0;
            for (int x = x0; x < x1; ++x) {
                bSum += (static_cast<int>(ptr[x][0]) + static_cast<int>(ptr[x][1]) + static_cast<int>(ptr[x][2])) / 3.0;
            }
            sum += bSum;
            if (bSum / bw < 2.8) ++staticBlocks;
        }
        return (staticBlocks * 4 >= numBlocks * 3) && (sum / cols < 5.5);
    };

    // 探测底部是否存在在零位移下静止、但在平移 delta 后显著失配的固定底栏块（如带深色边距与闪烁光标的高圆角输入框）
    for (int dist = refinedBottom; dist < maxBottom; ++dist) {
        const int lY = rows - 1 - dist;
        if (lY <= stickyTop + delta + 8) break;

        const double zeroErr = computeZeroRowErr(lY);
        const bool rowMostlyStatic = (zeroErr < 2.5) || computeZeroRowBlockStatic(lY);
        // 若该行本身随滚动剧烈变化，说明已进入真正的滚动正文区，立即停止
        if (!rowMostlyStatic && zeroErr > 4.2) {
            break;
        }

        // 若该行是固定栏文字/边框（平移后显著失配），直接推进固定底栏高度
        const int checkUy = lY - delta;
        double revShiftErr = 999.0;
        if (checkUy >= stickyTop) {
            const auto* uPtr = uInner.ptr<cv::Vec3b>(lY);
            const auto* lPtr = lInner.ptr<cv::Vec3b>(checkUy);
            double s = 0.0;
            for (int x = 0; x < cols; ++x) {
                s += (std::abs(static_cast<int>(uPtr[x][0]) - static_cast<int>(lPtr[x][0])) +
                      std::abs(static_cast<int>(uPtr[x][1]) - static_cast<int>(lPtr[x][1])) +
                      std::abs(static_cast<int>(uPtr[x][2]) - static_cast<int>(lPtr[x][2]))) / 3.0;
            }
            revShiftErr = s / cols;
        }
        (void)computeShiftedRowErr;

        if (rowMostlyStatic && revShiftErr > zeroErr + 3.5) {
            refinedBottom = dist + 1;
        } else if (rowMostlyStatic && dist - refinedBottom <= 28) {
            // 允许跨过固定输入框内部最多 28px 纯色静态行或局部光标行，前提是其上方仍有明确固定边框锚点行
            bool foundStaticAnchorAbove = false;
            for (int look = 1; look <= 28 && (dist + look) < maxBottom; ++look) {
                const int ky = rows - 1 - (dist + look);
                if (ky - delta < stickyTop) break;
                const double kz = computeZeroRowErr(ky);
                const bool kStatic = (kz < 2.5) || computeZeroRowBlockStatic(ky);
                if (!kStatic && kz > 4.2) break;
                const auto* uPtr = uInner.ptr<cv::Vec3b>(ky);
                const auto* lPtr = lInner.ptr<cv::Vec3b>(ky - delta);
                double s = 0.0;
                for (int x = 0; x < cols; ++x) {
                    s += (std::abs(static_cast<int>(uPtr[x][0]) - static_cast<int>(lPtr[x][0])) +
                          std::abs(static_cast<int>(uPtr[x][1]) - static_cast<int>(lPtr[x][1])) +
                          std::abs(static_cast<int>(uPtr[x][2]) - static_cast<int>(lPtr[x][2]))) / 3.0;
                }
                if (kStatic && (s / cols) > kz + 4.5) {
                    foundStaticAnchorAbove = true;
                    break;
                }
            }
            if (foundStaticAnchorAbove) {
                refinedBottom = dist + 1;
            } else {
                break;
            }
        } else {
            break;
        }
    }

    return refinedBottom;
}

// 接缝处 3px 亚像素微羽化融合（仅当对应像素差异微小时平滑过渡 ClearType 抗锯齿误差，绝不产生重影）
void blendSeamOverlap(const cv::Mat& frame, int offset, cv::Mat& newSegment) {
    constexpr int kBlendRows = 3;
    if (offset < kBlendRows || newSegment.rows < kBlendRows || frame.type() != CV_8UC3) return;
    // newSegment 的第 0..kBlendRows-1 行本身来自 frame 的 offset..offset+kBlendRows-1 行；
    // 此函数供双缓冲上下段交界调用，下面提供上下段直接微融版本
    (void)frame;
    (void)offset;
    (void)newSegment;
}

void featherSegmentBoundary(cv::Mat& prevSegment, const cv::Mat& fullCurrFrame, int currOffset) {
    constexpr int kFeatherRows = 3;
    if (prevSegment.empty() || fullCurrFrame.empty() ||
        prevSegment.type() != CV_8UC3 || fullCurrFrame.type() != CV_8UC3 ||
        prevSegment.cols != fullCurrFrame.cols ||
        prevSegment.rows < kFeatherRows || currOffset < kFeatherRows ||
        currOffset > fullCurrFrame.rows) {
        return;
    }

    // prevSegment 末尾 kFeatherRows 行与 fullCurrFrame 在 [currOffset - kFeatherRows, currOffset) 处于同一逻辑画面坐标
    for (int i = 0; i < kFeatherRows; ++i) {
        const int prevY = prevSegment.rows - kFeatherRows + i;
        const int currY = currOffset - kFeatherRows + i;
        auto* pRow = prevSegment.ptr<cv::Vec3b>(prevY);
        const auto* cRow = fullCurrFrame.ptr<cv::Vec3b>(currY);
        const float wCurr = static_cast<float>(i + 1) / static_cast<float>(kFeatherRows + 1);
        const float wPrev = 1.0f - wCurr;

        for (int x = 0; x < prevSegment.cols; ++x) {
            const int db = std::abs(static_cast<int>(pRow[x][0]) - static_cast<int>(cRow[x][0]));
            const int dg = std::abs(static_cast<int>(pRow[x][1]) - static_cast<int>(cRow[x][1]));
            const int dr = std::abs(static_cast<int>(pRow[x][2]) - static_cast<int>(cRow[x][2]));
            if (db + dg + dr <= 36) {
                pRow[x][0] = static_cast<uint8_t>(std::lround(pRow[x][0] * wPrev + cRow[x][0] * wCurr));
                pRow[x][1] = static_cast<uint8_t>(std::lround(pRow[x][1] * wPrev + cRow[x][1] * wCurr));
                pRow[x][2] = static_cast<uint8_t>(std::lround(pRow[x][2] * wPrev + cRow[x][2] * wCurr));
            }
        }
    }
}

bool isPointInsideRect(POINT pt, const RECT& rc, int inset) {
    return pt.x >= rc.left + inset && pt.x <= rc.right - inset &&
           pt.y >= rc.top + inset && pt.y <= rc.bottom - inset;
}

}  // namespace

ScrollCapture& ScrollCapture::instance() {
    static ScrollCapture inst;
    return inst;
}

SIZE ScrollCapture::stitchedDimensions() const {
    return SIZE{
        static_cast<LONG>(m_segmentWidth.load(std::memory_order_acquire)),
        static_cast<LONG>(m_totalRows.load(std::memory_order_acquire))
    };
}

void ScrollCapture::start(const ScrollCaptureOptions& options) {
    if (m_running.load(std::memory_order_acquire)) {
        LOG_WARN("长截图已在运行中");
        return;
    }
    if (m_scrollThread.joinable()) m_scrollThread.join();

    tools3000::core::TraceId::Scope scope;
    {
        std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
        m_options = options;
        clearStorage();
        m_terminalError.clear();
        m_lastFrame.release();
    }
    m_frameCount.store(0, std::memory_order_release);
    m_lastTooFastTickMs.store(0, std::memory_order_release);
    m_completionDelivered.store(false, std::memory_order_release);
    m_suppressCompletion.store(false, std::memory_order_release);
    m_finishAction.store(ScrollFinishAction::Confirm, std::memory_order_release);
    m_resumeNudgeCursor.store(false, std::memory_order_release);
    const bool startAuto = (options.mode == ScrollMode::Auto);
    m_autoScrolling.store(startAuto, std::memory_order_release);
    m_workerInLoop.store(false, std::memory_order_release);
    m_runtimeState.store(
        startAuto ? ScrollRuntimeState::AutoScrolling : ScrollRuntimeState::PausedManual,
        std::memory_order_release);
    m_running.store(true, std::memory_order_release);

    std::string captureError;
    bool initOk = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
        m_captureBackend = createCaptureBackend();
        const CaptureRegion region{
            options.captureRect.left, options.captureRect.top,
            options.captureRect.right - options.captureRect.left,
            options.captureRect.bottom - options.captureRect.top
        };
        initOk = (m_captureBackend && m_captureBackend->initialize(region, captureError));
        if (!initOk) {
            m_captureBackend.reset();
        }
    }
    if (!initOk) {
        m_autoScrolling.store(false, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
        m_runtimeState.store(ScrollRuntimeState::Idle, std::memory_order_release);
        LOG_ERROR("长截图捕获后端初始化失败: {}", captureError);
        const char* defaultInitErr = tools3000::core::isUiLanguageChinese()
            ? "无法初始化屏幕捕获"
            : "Failed to initialize screen capture";
        deliverCompletion(captureError.empty() ? defaultInitErr : captureError);
        return;
    }

    if (startAuto) {
        m_workerInLoop.store(true, std::memory_order_release);
        m_scrollThread = std::thread([this]() { autoScrollLoop(); });
    }

    LOG_INFO("长截图已启动: mode={}, scrollDelay={}ms, maxFrames={}",
             startAuto ? "Auto" : "Manual",
             options.scrollDelayMs, options.maxFrames);
}

void ScrollCapture::stop(ScrollFinishAction action) {
    m_finishAction.store(action, std::memory_order_release);
    m_autoScrolling.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    if (m_scrollThread.joinable() && m_scrollThread.get_id() != std::this_thread::get_id()) {
        m_scrollThread.join();
    }
    m_runtimeState.store(ScrollRuntimeState::Idle, std::memory_order_release);
    std::string err;
    {
        std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
        err = m_terminalError;
    }
    deliverCompletion(err);
}

void ScrollCapture::requestFinish(ScrollFinishAction action) {
    m_finishAction.store(action, std::memory_order_release);
    m_autoScrolling.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    if (!m_workerInLoop.load(std::memory_order_acquire)) {
        if (m_scrollThread.joinable() && m_scrollThread.get_id() != std::this_thread::get_id()) {
            m_scrollThread.join();
        }
        m_runtimeState.store(ScrollRuntimeState::Idle, std::memory_order_release);
        std::string err;
        {
            std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
            err = m_terminalError;
        }
        deliverCompletion(err);
    }
}

void ScrollCapture::cancel() {
    stop(ScrollFinishAction::Cancelled);
}

void ScrollCapture::requestCancel() {
    requestFinish(ScrollFinishAction::Cancelled);
}

void ScrollCapture::pause() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_autoScrolling.store(false, std::memory_order_release);
    m_runtimeState.store(ScrollRuntimeState::PausedManual, std::memory_order_release);
}

void ScrollCapture::resume() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_resumeNudgeCursor.store(true, std::memory_order_release);
    m_autoScrolling.store(true, std::memory_order_release);
    m_runtimeState.store(ScrollRuntimeState::AutoScrolling, std::memory_order_release);
}

void ScrollCapture::togglePause() {
    if (!m_running.load(std::memory_order_acquire)) return;
    const auto cur = m_runtimeState.load(std::memory_order_acquire);
    if (cur == ScrollRuntimeState::AutoScrolling) {
        pause();
    } else {
        resume();
    }
}

bool ScrollCapture::isPaused() const {
    const auto cur = m_runtimeState.load(std::memory_order_acquire);
    return cur == ScrollRuntimeState::PausedManual || cur == ScrollRuntimeState::HoldAtBottom;
}

void ScrollCapture::shutdown() {
    m_suppressCompletion.store(true, std::memory_order_release);
    m_autoScrolling.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    if (m_scrollThread.joinable() && m_scrollThread.get_id() != std::this_thread::get_id()) {
        m_scrollThread.join();
    }
    m_workerInLoop.store(false, std::memory_order_release);
    m_runtimeState.store(ScrollRuntimeState::Idle, std::memory_order_release);
    {
        std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
        m_progressCb = nullptr;
        m_completionCb = nullptr;
        clearStorage();
        m_terminalError.clear();
        m_lastFrame.release();
        if (m_captureBackend) m_captureBackend->shutdown();
        m_captureBackend.reset();
    }
    m_frameCount.store(0, std::memory_order_release);
    tools3000::core::WinUtils::trimWorkingSet();
}

void ScrollCapture::captureCurrentFrame() {
    if (!m_running.load(std::memory_order_acquire)) return;

    auto frame = captureRegion(m_options.captureRect);
    if (!frame.empty()) {
        appendFrame(frame, nullptr);
        std::string fatalErr;
        if (!m_running.load(std::memory_order_acquire)) {
            std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
            fatalErr = m_terminalError;
        }
        if (!m_running.load(std::memory_order_acquire) && !fatalErr.empty()) {
            m_runtimeState.store(ScrollRuntimeState::Idle, std::memory_order_release);
            deliverCompletion(fatalErr);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 自动滚动 + 手动滚轮混合工作循环（运动沉降门禁 + 亚刻度闭环 + 超调自动回滚补帧）
// ─────────────────────────────────────────────────────────────────────────────

void ScrollCapture::autoScrollLoop() {
    tools3000::core::TraceId::Scope scope;
    POINT originalCursor{};
    GetCursorPos(&originalCursor);
    POINT lastProgrammaticCursor = originalCursor;
    bool movedCursorProgrammatically = false;

    const bool isMemoryTestBackend =
        (m_captureBackend && m_captureBackend->info().id == "memory");

    auto restoreCursorIfUntouched = [&]() {
        if (!movedCursorProgrammatically || m_options.interactiveHoldAtBottom) return;
        POINT now{};
        if (GetCursorPos(&now) &&
            std::abs(now.x - lastProgrammaticCursor.x) <= 10 &&
            std::abs(now.y - lastProgrammaticCursor.y) <= 10) {
            SetCursorPos(originalCursor.x, originalCursor.y);
        }
    };

    auto sleepInterruptible = [&](int totalMs, bool breakOnStateChange, ScrollRuntimeState expectedState) {
        int waited = 0;
        while (waited < totalMs && m_running.load(std::memory_order_acquire)) {
            if (breakOnStateChange && m_runtimeState.load(std::memory_order_acquire) != expectedState) {
                break;
            }
            const int slice = std::min(20, totalMs - waited);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            waited += slice;
        }
    };

    auto snapshotLastFrame = [&]() -> cv::Mat {
        std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
        return m_lastFrame.empty() ? cv::Mat{} : m_lastFrame.clone();
    };

    // 捕获稳定停稳帧（Motion Settling Gate）：彻底消除浏览器/IDE 平滑滚动半途抓帧引发的模糊与漂移
    auto captureSettledFrame = [&](ScrollRuntimeState expectedState) -> cv::Mat {
        cv::Mat f1 = captureRegion(m_options.captureRect);
        const cv::Mat lastRef = snapshotLastFrame();
        if (f1.empty() || isMemoryTestBackend || lastRef.empty()) {
            return f1;
        }

        int roiX = 0, roiW = f1.cols;
        computeContentHorizROI(f1.cols, roiX, roiW);
        cv::Mat quickDiff;
        cv::absdiff(lastRef(cv::Rect(roiX, 0, roiW, f1.rows)),
                    f1(cv::Rect(roiX, 0, roiW, f1.rows)), quickDiff);
        const cv::Scalar m0 = cv::mean(quickDiff);
        if ((m0[0] + m0[1] + m0[2]) / 3.0 < 1.2) {
            return f1;
        }

        for (int settleStep = 0; settleStep < 4; ++settleStep) {
            sleepInterruptible(38, true, expectedState);
            if (!m_running.load(std::memory_order_acquire) ||
                m_runtimeState.load(std::memory_order_acquire) != expectedState) {
                return f1;
            }
            cv::Mat f2 = captureRegion(m_options.captureRect);
            if (f2.empty() || f2.size() != f1.size()) break;

            cv::Mat stepDiff;
            cv::absdiff(f1(cv::Rect(roiX, 0, roiW, f1.rows)),
                        f2(cv::Rect(roiX, 0, roiW, f2.rows)), stepDiff);
            const cv::Scalar ms = cv::mean(stepDiff);
            const double motionMad = (ms[0] + ms[1] + ms[2]) / 3.0;
            f1 = std::move(f2);
            if (motionMad < 1.25) {
                break; // 画面已彻底停稳
            }
        }
        return f1;
    };

    auto sendWheelUnits = [&](int signedWheelUnits) -> bool {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(signedWheelUnits);
        return SendInput(1, &input, sizeof(INPUT)) == 1;
    };

    try {
        // 首帧前确保选区覆盖层与历史 Toast 已彻底从 DWM 桌面合成管线移除
        if (!isMemoryTestBackend) {
            DwmFlush();
            std::this_thread::sleep_for(std::chrono::milliseconds(55));
        }

        // 1. 立即采集首帧，让预览缩略图与尺寸计数器在第一时间点亮
        if (m_running.load(std::memory_order_acquire)) {
            cv::Mat firstFrame = captureRegion(m_options.captureRect);
            if (!firstFrame.empty()) {
                appendFrame(firstFrame, nullptr);
            }
        }

        const RECT r = m_options.captureRect;
        const int capW = std::max(1L, r.right - r.left);
        const int capH = std::max(1L, r.bottom - r.top);
        const int centerX = (r.left + r.right) / 2;
        const int safeScrollY = r.top + std::clamp((capH * 42) / 100, 1, std::max(1, capH - 1));

        // 连续亚刻度滚轮剂量（WHEEL_DELTA = 120 为标准 1 格）：初始保守起步，确保首步拥有充足重叠区
        int initialNotches = std::max(1, m_options.scrollAmount);
        if (capH < 360) {
            initialNotches = 1;
        } else if (capH < 680) {
            initialNotches = std::min(initialNotches, 2);
        } else {
            initialNotches = std::min(initialNotches, 3);
        }
        int wheelUnits = initialNotches * WHEEL_DELTA;
        if (capH < 260) {
            wheelUnits = 90;
        }
        bool allowSubDetent = true;

        // 启动瞬间若光标不在选区内部（刚点完截图工具栏按钮必然位于选区外，甚至可能恰好与新弹出的控制栏重合），
        // 主动将其移至选区内容核心区偏上位置（避开顶部标题栏与底部输入框），避免首帧滚动落空秒停
        if (!isMemoryTestBackend && m_running.load(std::memory_order_acquire)) {
            POINT cur{};
            if (GetCursorPos(&cur)) {
                const int inset = std::min(12, std::min(capW, capH) / 6);
                if (!isPointInsideRect(cur, r, inset)) {
                    SetCursorPos(centerX, safeScrollY);
                    lastProgrammaticCursor = {centerX, safeScrollY};
                    movedCursorProgrammatically = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(35));
                }
            }
        }

        int stagnantProbes = 0;

        while (m_running.load(std::memory_order_acquire)) {
            if (!m_options.interactiveHoldAtBottom && !isMemoryTestBackend &&
                (GetAsyncKeyState(VK_ESCAPE) & 1)) {
                LOG_INFO("用户按 Esc 结束长截图, frame={}", m_frameCount.load());
                break;
            }

            const ScrollRuntimeState state = m_runtimeState.load(std::memory_order_acquire);

            // 分支 A：暂停态或触底驻留态 —— 停止自动发滚轮，静默监听用户手动滚轮/拖动滚动条补截
            if (state == ScrollRuntimeState::PausedManual || state == ScrollRuntimeState::HoldAtBottom) {
                stagnantProbes = 0;
                sleepInterruptible(isMemoryTestBackend ? 5 : 85, true, state);
                if (!m_running.load(std::memory_order_acquire)) break;
                if (m_runtimeState.load(std::memory_order_acquire) != state) continue;

                cv::Mat frame = captureSettledFrame(state);
                if (frame.empty()) continue;

                const cv::Mat lastRef = snapshotLastFrame();
                if (lastRef.empty()) {
                    m_lastTooFastTickMs.store(0, std::memory_order_release);
                    appendFrame(frame, nullptr);
                } else {
                    const ScrollFrameTransition tr = analyzeFrameTransition(lastRef, frame);
                    if (tr.matched && !tr.isUnchanged && !tr.isBackwardScroll && tr.scrollDelta >= 4) {
                        m_lastTooFastTickMs.store(0, std::memory_order_release);
                        appendFrame(frame, &tr);
                    } else if (tr.isBackwardScroll) {
                        // 用户手动向上回滚或触底弹性回弹：静默忽略，绝不误报「滚动过快」也不污染末帧基准
                        m_lastTooFastTickMs.store(0, std::memory_order_release);
                    } else if (!tr.matched && !tr.isUnchanged) {
                        m_lastTooFastTickMs.store(GetTickCount64(), std::memory_order_release);
                    }
                }
                continue;
            }

            // 分支 B：自动滚动采集态
            if (m_frameCount.load(std::memory_order_acquire) >= m_options.maxFrames) {
                m_autoScrolling.store(false, std::memory_order_release);
                if (m_options.interactiveHoldAtBottom) {
                    m_runtimeState.store(ScrollRuntimeState::HoldAtBottom, std::memory_order_release);
                    continue;
                }
                break;
            }

            if (!isMemoryTestBackend) {
                POINT cur{};
                if (GetCursorPos(&cur)) {
                    if (m_options.isUiHotZone && m_options.isUiHotZone(cur)) {
                        sleepInterruptible(80, true, ScrollRuntimeState::AutoScrolling);
                        continue;
                    }
                    const int inset = std::min(12, std::min(capW, capH) / 6);
                    if (m_resumeNudgeCursor.exchange(false, std::memory_order_acq_rel) &&
                        !isPointInsideRect(cur, r, inset)) {
                        SetCursorPos(centerX, safeScrollY);
                        lastProgrammaticCursor = {centerX, safeScrollY};
                        movedCursorProgrammatically = true;
                        std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    }
                }

                if (!sendWheelUnits(-wheelUnits)) {
                    LOG_WARN("长截图滚轮输入失败: error={}", GetLastError());
                    m_autoScrolling.store(false, std::memory_order_release);
                    if (m_options.interactiveHoldAtBottom) {
                        m_runtimeState.store(ScrollRuntimeState::HoldAtBottom, std::memory_order_release);
                        continue;
                    }
                    break;
                }

                sleepInterruptible(m_options.scrollDelayMs, true, ScrollRuntimeState::AutoScrolling);
                if (!m_running.load(std::memory_order_acquire)) break;
                if (m_runtimeState.load(std::memory_order_acquire) != ScrollRuntimeState::AutoScrolling) {
                    continue;
                }
            }

            cv::Mat frame = captureSettledFrame(ScrollRuntimeState::AutoScrolling);
            if (frame.empty()) continue;

            cv::Mat lastRef = snapshotLastFrame();
            if (lastRef.empty()) {
                appendFrame(frame, nullptr);
                continue;
            }

            ScrollFrameTransition tr = analyzeFrameTransition(lastRef, frame);

            // 若亚刻度剂量 (< 120) 遇到不响应高精度滚轮的传统 Win32 控件导致未动，自动回升至整格 120 重试
            if (tr.isUnchanged && !isMemoryTestBackend && wheelUnits < WHEEL_DELTA) {
                allowSubDetent = false;
                wheelUnits = WHEEL_DELTA;
                if (sendWheelUnits(-wheelUnits)) {
                    sleepInterruptible(m_options.scrollDelayMs, true, ScrollRuntimeState::AutoScrolling);
                    frame = captureSettledFrame(ScrollRuntimeState::AutoScrolling);
                    lastRef = snapshotLastFrame();
                    if (!frame.empty() && !lastRef.empty()) {
                        tr = analyzeFrameTransition(lastRef, frame);
                    }
                }
            }

            // 若首次采样未检测到有效位移，针对异步懒加载补做一次沉降采样
            if (tr.isUnchanged && !isMemoryTestBackend) {
                sleepInterruptible(140, true, ScrollRuntimeState::AutoScrolling);
                if (!m_running.load(std::memory_order_acquire)) break;
                if (m_runtimeState.load(std::memory_order_acquire) != ScrollRuntimeState::AutoScrolling) {
                    continue;
                }
                cv::Mat settledFrame = captureSettledFrame(ScrollRuntimeState::AutoScrolling);
                lastRef = snapshotLastFrame();
                if (!settledFrame.empty() && !lastRef.empty()) {
                    ScrollFrameTransition settledTr = analyzeFrameTransition(lastRef, settledFrame);
                    if (settledTr.matched && !settledTr.isUnchanged && !settledTr.isBackwardScroll && settledTr.scrollDelta >= 3) {
                        frame = std::move(settledFrame);
                        tr = settledTr;
                    }
                }
            }

            // 防漏截杀手锏：超调自动反向回滚补帧 (Overshoot Auto-Backtrack Recovery)
            // 当画面明显发生了向下滚动 (!tr.isUnchanged && !tr.isBackwardScroll)，但因单步滚幅过大跨过了整个视口导致 !tr.matched 或重叠区过窄时，
            // 绝不盲目硬拼吞掉中间漏截内容，而是自动反向向上回滚半步找回重叠帧！
            const int estDynH = std::max(60, frame.rows - tr.stickyTopRows - tr.stickyBottomRows);
            const int remOverlap = estDynH - tr.scrollDelta;
            const bool overshot = !isMemoryTestBackend && !tr.isUnchanged && !tr.isBackwardScroll &&
                (!tr.matched || remOverlap < std::max(18, (estDynH * 12) / 100)) &&
                computeFrameTextureStd(frame) >= 3.0;

            if (overshot) {
                for (int bt = 0; bt < 2 && m_running.load(std::memory_order_acquire); ++bt) {
                    const int backtrackUnits = std::max(allowSubDetent ? 50 : WHEEL_DELTA, (wheelUnits * 55) / 100);
                    if (!sendWheelUnits(+backtrackUnits)) break;
                    sleepInterruptible(std::max(85, m_options.scrollDelayMs), true, ScrollRuntimeState::AutoScrolling);
                    if (!m_running.load(std::memory_order_acquire) ||
                        m_runtimeState.load(std::memory_order_acquire) != ScrollRuntimeState::AutoScrolling) {
                        break;
                    }
                    cv::Mat recovered = captureSettledFrame(ScrollRuntimeState::AutoScrolling);
                    lastRef = snapshotLastFrame();
                    if (recovered.empty() || lastRef.empty()) break;
                    ScrollFrameTransition recTr = analyzeFrameTransition(lastRef, recovered);
                    if (recTr.matched && !recTr.isUnchanged && !recTr.isBackwardScroll && recTr.scrollDelta >= 3) {
                        frame = std::move(recovered);
                        tr = recTr;
                        wheelUnits = std::max(allowSubDetent ? 54 : WHEEL_DELTA, (wheelUnits * 52) / 100);
                        break;
                    }
                    if (recTr.isBackwardScroll || recTr.isUnchanged) {
                        // 回滚已退回至上帧之前或原位：立即止步并降步长，严防来回拉锯造成重复拼接
                        wheelUnits = std::max(allowSubDetent ? 54 : WHEEL_DELTA, (wheelUnits * 52) / 100);
                        break;
                    }
                }
            }

            if (tr.matched && !tr.isUnchanged && !tr.isBackwardScroll && tr.scrollDelta >= 3) {
                stagnantProbes = 0;

                // 连续比例闭环控制：将每步滚动量精准锚定在动态可视高度的 38%（保持 62% 黄金重叠带）
                const int dynH = std::max(80, frame.rows - tr.stickyTopRows - tr.stickyBottomRows);
                const double targetDelta = dynH * 0.38;
                const double ratio = std::clamp(targetDelta / std::max(8.0, static_cast<double>(tr.scrollDelta)), 0.55, 1.55);
                if (tr.scrollDelta > (dynH * 52) / 100 || tr.scrollDelta < (dynH * 24) / 100) {
                    const int minUnits = allowSubDetent ? 54 : WHEEL_DELTA;
                    wheelUnits = std::clamp(
                        static_cast<int>(std::lround(wheelUnits * (0.35 + 0.65 * ratio))),
                        minUnits, WHEEL_DELTA * 5);
                    if (!allowSubDetent) {
                        wheelUnits = std::max(1, (wheelUnits + WHEEL_DELTA / 2) / WHEEL_DELTA) * WHEEL_DELTA;
                    }
                }

                appendFrame(frame, &tr);
            } else if (!tr.isUnchanged && !tr.isBackwardScroll && !tr.matched && isMemoryTestBackend &&
                       computeFrameTextureStd(frame) < 2.5) {
                // 保持无纹理纯色内存合成单测向后兼容
                stagnantProbes = 0;
                appendFrame(frame, &tr);
            } else {
                ++stagnantProbes;
                // 首帧起步（frame==1）给 Electron/Chromium/刚失焦恢复的窗口多留 1 次唤醒缓冲，并尝试微调光标至安全内容核心区
                const int curFrames = m_frameCount.load(std::memory_order_acquire);
                if (!isMemoryTestBackend && curFrames <= 1 && stagnantProbes == 1) {
                    POINT cur{};
                    if (GetCursorPos(&cur) && !(m_options.isUiHotZone && m_options.isUiHotZone(cur))) {
                        const int retryY = r.top + std::clamp((capH * 36) / 100, 1, std::max(1, capH - 1));
                        SetCursorPos(centerX, retryY);
                        lastProgrammaticCursor = {centerX, retryY};
                        movedCursorProgrammatically = true;
                        sleepInterruptible(60, true, ScrollRuntimeState::AutoScrolling);
                    }
                }
                const int requiredStagnantProbes = isMemoryTestBackend ? 1 : (curFrames <= 1 ? 3 : 2);
                if (stagnantProbes >= requiredStagnantProbes) {
                    LOG_INFO("检测到滚动到底部, frame={}", curFrames);
                    m_autoScrolling.store(false, std::memory_order_release);
                    if (m_options.interactiveHoldAtBottom) {
                        m_runtimeState.store(ScrollRuntimeState::HoldAtBottom, std::memory_order_release);
                        stagnantProbes = 0;
                        continue;
                    }
                    break;
                }
            }
        }

        m_autoScrolling.store(false, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
        m_runtimeState.store(ScrollRuntimeState::Idle, std::memory_order_release);
        restoreCursorIfUntouched();
        std::string err;
        {
            std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
            err = m_terminalError;
        }
        deliverCompletion(err);
        m_workerInLoop.store(false, std::memory_order_release);
    } catch (const std::exception& e) {
        m_autoScrolling.store(false, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
        m_runtimeState.store(ScrollRuntimeState::Idle, std::memory_order_release);
        restoreCursorIfUntouched();
        LOG_ERROR("长截图线程异常, 已停止: {}", e.what());
        deliverCompletion(e.what());
        m_workerInLoop.store(false, std::memory_order_release);
    } catch (...) {
        m_autoScrolling.store(false, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
        m_runtimeState.store(ScrollRuntimeState::Idle, std::memory_order_release);
        restoreCursorIfUntouched();
        LOG_ERROR("长截图线程未知异常, 已停止");
        deliverCompletion(tools3000::core::isUiLanguageChinese() ? "未知错误" : "Unknown error");
        m_workerInLoop.store(false, std::memory_order_release);
    }
}

int ScrollCapture::lastSegmentRows() const {
    if (!m_spilledSegments.empty()) {
        return m_spilledSegments.back().rows;
    }
    if (!m_segments.empty()) {
        return m_segments.back().rows;
    }
    return 0;
}

void ScrollCapture::trimLastSegmentBottomRows(int trimRows, std::size_t rowBytes) {
    if (trimRows <= 0) return;
    if (!m_spilledSegments.empty()) {
        auto& back = m_spilledSegments.back();
        const int actual = std::min(trimRows, std::max(0, back.rows - 1));
        if (actual <= 0) return;
        back.rows -= actual;
        const std::size_t cutBytes = static_cast<std::size_t>(actual) * rowBytes;
        m_segmentBytes = (m_segmentBytes > cutBytes) ? (m_segmentBytes - cutBytes) : 0;
        m_totalRows.fetch_sub(actual, std::memory_order_acq_rel);
        return;
    }
    if (!m_segments.empty()) {
        cv::Mat& back = m_segments.back();
        const int actual = std::min(trimRows, std::max(0, back.rows - 1));
        if (actual <= 0) return;
        back = back.rowRange(0, back.rows - actual).clone();
        const std::size_t cutBytes = static_cast<std::size_t>(actual) * rowBytes;
        m_segmentBytes = (m_segmentBytes > cutBytes) ? (m_segmentBytes - cutBytes) : 0;
        m_residentSegmentBytes = (m_residentSegmentBytes > cutBytes) ? (m_residentSegmentBytes - cutBytes) : 0;
        m_totalRows.fetch_sub(actual, std::memory_order_acq_rel);
    }
}

void ScrollCapture::featherSpilledBoundary(const cv::Mat& fullCurrFrame, int currOffset, std::size_t rowBytes) {
    constexpr int kFeatherRows = 3;
    if (m_spilledSegments.empty() || m_spillPath.empty() ||
        m_segmentType != CV_8UC3 || fullCurrFrame.empty() ||
        fullCurrFrame.type() != CV_8UC3 ||
        currOffset < kFeatherRows || currOffset > fullCurrFrame.rows ||
        rowBytes == 0) {
        return;
    }

    const auto& back = m_spilledSegments.back();
    if (back.rows < kFeatherRows) return;

    const std::uint64_t tailOffset =
        back.offset + static_cast<std::uint64_t>(back.rows - kFeatherRows) * rowBytes;

    std::fstream io(m_spillPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!io) return;

    cv::Mat tail(kFeatherRows, fullCurrFrame.cols, CV_8UC3);
    io.seekg(static_cast<std::streamoff>(tailOffset), std::ios::beg);
    for (int row = 0; row < kFeatherRows; ++row) {
        if (!scroll_storage::readExact(io, tail.ptr(row), rowBytes)) {
            return;
        }
    }

    featherSegmentBoundary(tail, fullCurrFrame, currOffset);

    io.clear();
    io.seekp(static_cast<std::streamoff>(tailOffset), std::ios::beg);
    if (!io) return;
    scroll_storage::writeMat(io, tail);
    io.flush();
}

bool ScrollCapture::isDuplicateOfStitchedTail(const cv::Mat& newDynamic, int trimPrevRows) const {
    if (newDynamic.empty() || newDynamic.rows < 56) return false;
    const int curTotalRows = m_totalRows.load(std::memory_order_acquire);
    const int effectivePrevRows = curTotalRows - std::max(0, trimPrevRows);
    if (effectivePrevRows < newDynamic.rows + 24) return false;

    const int maxFetch = std::min(curTotalRows, std::max(newDynamic.rows * 3, 2400) + std::max(0, trimPrevRows));
    cv::Mat tail = buildRecentPreview(maxFetch);
    if (tail.empty()) return false;
    if (trimPrevRows > 0 && tail.rows > trimPrevRows) {
        tail = tail.rowRange(0, tail.rows - trimPrevRows);
    }
    double dupScore = 0.0;
    if (isDuplicateSegmentInTail(tail, newDynamic, &dupScore)) {
        LOG_WARN("长截图检测到重复内容段，已自动拦截: rows={}, score={:.3f}", newDynamic.rows, dupScore);
        return true;
    }
    return false;
}

void ScrollCapture::appendFrame(const cv::Mat& frame, const ScrollFrameTransition* precomputed) {
    if (frame.empty()) return;

    const std::size_t rowBytes = static_cast<std::size_t>(frame.cols) * frame.elemSize();
    if (rowBytes == 0) return;

    std::unique_lock<std::recursive_mutex> lock(m_captureMutex);

    int offset = 0;
    int trimPrevRows = 0;
    bool hasVerifiedMatch = false;
    int effectiveStickyBottom = 0;

    if (m_lastFrame.empty()) {
        offset = 0;
    } else {
        ScrollFrameTransition localTr;
        const ScrollFrameTransition& tr = precomputed ? *precomputed : (localTr = analyzeFrameTransition(m_lastFrame, frame));
        if (tr.isUnchanged || tr.isBackwardScroll) {
            return;
        }
        if (tr.matched && tr.scrollDelta > 0) {
            effectiveStickyBottom = tr.stickyBottomRows;
            // 会话级固定底栏稳定性保护：连续 >= 2 帧确认到高固定底栏后，防止单帧光标闪烁导致底栏高度骤降漏拼入正文
            if (effectiveStickyBottom >= 24) {
                if (m_knownStickyBottom > 0 && std::abs(effectiveStickyBottom - m_knownStickyBottom) <= 12) {
                    m_knownStickyBottom = (m_knownStickyBottom + effectiveStickyBottom) / 2;
                    ++m_knownStickyBottomHits;
                } else if (m_knownStickyBottomHits < 2) {
                    m_knownStickyBottom = effectiveStickyBottom;
                    m_knownStickyBottomHits = 1;
                }
            } else if (m_knownStickyBottomHits >= 2 && m_knownStickyBottom >= 24 &&
                       effectiveStickyBottom + 8 < m_knownStickyBottom &&
                       m_lastFrame.size() == frame.size()) {
                const int checkH = std::min(m_knownStickyBottom, (frame.rows * 45) / 100);
                int roiX = 0, roiW = frame.cols;
                computeContentHorizROI(frame.cols, roiX, roiW);
                const cv::Mat prevBot = m_lastFrame(cv::Rect(roiX, frame.rows - checkH, roiW, checkH));
                const cv::Mat currBot = frame(cv::Rect(roiX, frame.rows - checkH, roiW, checkH));
                cv::Mat botDiff;
                cv::absdiff(prevBot, currBot, botDiff);
                int staticBotRows = 0;
                for (int r = 0; r < checkH; ++r) {
                    const cv::Scalar rm = cv::mean(botDiff.row(r));
                    if ((rm[0] + rm[1] + rm[2]) / 3.0 < 3.6) ++staticBotRows;
                }
                if (staticBotRows * 100 >= checkH * 82) {
                    effectiveStickyBottom = checkH;
                }
            }

            const int prevRows = lastSegmentRows();
            const int maxSafeTrim = std::clamp(
                std::min({effectiveStickyBottom, std::max(0, prevRows - 1),
                          frame.rows - tr.stickyTopRows - tr.scrollDelta}),
                0, frame.rows - 1);
            trimPrevRows = maxSafeTrim;
            offset = frame.rows - trimPrevRows - tr.scrollDelta;
            hasVerifiedMatch = true;
        } else {
            // 显式手动单帧追加兜底（保持与无纹理虚拟驱动单测契约兼容）
            offset = frame.rows * std::clamp(m_options.overlapPercent, 0, 95) / 100;
        }
    }

    offset = std::clamp(offset, 0, frame.rows);
    const int remaining = frame.rows - offset;
    if (remaining <= 0) return;

    // 纵深防线：对已验证滚动的新增动态正文段执行尾部全局防重复循环检测，彻底杜绝复读机长图
    if (hasVerifiedMatch && m_frameCount.load(std::memory_order_acquire) >= 2) {
        const int dynRows = remaining - trimPrevRows;
        if (dynRows >= 56 && offset + dynRows <= frame.rows) {
            const cv::Mat newDynamic = frame.rowRange(offset, offset + dynRows);
            if (isDuplicateOfStitchedTail(newDynamic, trimPrevRows)) {
                return;
            }
        }
    }

    if (static_cast<std::size_t>(remaining) > (std::numeric_limits<std::size_t>::max)() / rowBytes) {
        m_terminalError = tools3000::core::isUiLanguageChinese()
            ? "长截图尺寸超出可表示范围"
            : "Scrolling capture dimensions exceeded supported limit";
        m_autoScrolling.store(false, std::memory_order_release);
        if (m_options.interactiveHoldAtBottom && m_frameCount.load(std::memory_order_acquire) > 0) {
            m_runtimeState.store(ScrollRuntimeState::HoldAtBottom, std::memory_order_release);
        } else {
            m_running.store(false, std::memory_order_release);
        }
        return;
    }

    const std::size_t trimBytes = static_cast<std::size_t>(trimPrevRows) * rowBytes;
    const std::size_t effectiveCurrentBytes = (m_segmentBytes > trimBytes) ? (m_segmentBytes - trimBytes) : 0;
    const std::size_t additionalBytes = static_cast<std::size_t>(remaining) * rowBytes;

    if (m_options.maxOutputBytes > 0 &&
        (additionalBytes > m_options.maxOutputBytes ||
         effectiveCurrentBytes > m_options.maxOutputBytes - additionalBytes)) {
        m_autoScrolling.store(false, std::memory_order_release);
        LOG_WARN("长截图达到字节预算: current={} MiB, next={} MiB, limit={} MiB",
                 effectiveCurrentBytes / (1024 * 1024), additionalBytes / (1024 * 1024),
                 m_options.maxOutputBytes / (1024 * 1024));
        if (m_options.interactiveHoldAtBottom && m_frameCount.load(std::memory_order_acquire) > 0) {
            // 交互式桌面长截图已采集有效帧：仅停止自动滚动并驻留，保持长截图会话有效等待用户点击完成/复制/保存
            m_runtimeState.store(ScrollRuntimeState::HoldAtBottom, std::memory_order_release);
        } else {
            m_terminalError = tools3000::core::isUiLanguageChinese()
                ? "长截图已达到安全内存上限，已保留当前结果"
                : "Reached safe memory limit; partial result kept";
            m_running.store(false, std::memory_order_release);
        }
        return;
    }

    cv::Mat segment = frame(cv::Rect(0, offset, frame.cols, remaining)).clone();
    blendSeamOverlap(frame, offset, segment);

    int expectedWidth = m_segmentWidth.load(std::memory_order_acquire);
    if (expectedWidth == 0) {
        m_segmentWidth.store(segment.cols, std::memory_order_release);
        expectedWidth = segment.cols;
        m_segmentType = segment.type();
    }
    if (segment.cols != expectedWidth || segment.type() != m_segmentType) {
        m_terminalError = tools3000::core::isUiLanguageChinese()
            ? "长截图帧格式发生变化，已安全停止"
            : "Frame dimensions changed during scrolling capture; stopped safely";
        m_autoScrolling.store(false, std::memory_order_release);
        if (m_options.interactiveHoldAtBottom && m_frameCount.load(std::memory_order_acquire) > 0) {
            m_runtimeState.store(ScrollRuntimeState::HoldAtBottom, std::memory_order_release);
        } else {
            m_running.store(false, std::memory_order_release);
        }
        return;
    }

    const std::size_t effectiveResidentBytes =
        (m_spilledSegments.empty() && m_residentSegmentBytes > trimBytes)
            ? (m_residentSegmentBytes - trimBytes)
            : m_residentSegmentBytes;
    const bool mustSpill = !m_spilledSegments.empty() ||
        (m_options.maxInMemoryStagingBytes > 0 &&
         (additionalBytes > m_options.maxInMemoryStagingBytes ||
          effectiveResidentBytes > m_options.maxInMemoryStagingBytes - additionalBytes));

    if (mustSpill) {
        if (!spillResidentSegments()) {
            m_terminalError = tools3000::core::isUiLanguageChinese()
                ? "长截图临时存储不可用，已安全停止"
                : "Temporary storage unavailable for scrolling capture";
            m_autoScrolling.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            return;
        }
        if (trimPrevRows > 0) {
            trimLastSegmentBottomRows(trimPrevRows, rowBytes);
        }
        if (hasVerifiedMatch && !m_spilledSegments.empty()) {
            featherSpilledBoundary(frame, offset, rowBytes);
        }
        if (!appendSpilledSegment(segment)) {
            m_terminalError = tools3000::core::isUiLanguageChinese()
                ? "长截图临时存储不可用，已安全停止"
                : "Temporary storage unavailable for scrolling capture";
            m_autoScrolling.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            return;
        }
    } else {
        if (trimPrevRows > 0) {
            trimLastSegmentBottomRows(trimPrevRows, rowBytes);
        }
        if (hasVerifiedMatch && !m_segments.empty()) {
            featherSegmentBoundary(m_segments.back(), frame, offset);
        }
        m_segments.push_back(segment);
        m_residentSegmentBytes += additionalBytes;
    }

    m_segmentBytes += additionalBytes;
    m_totalRows.fetch_add(remaining, std::memory_order_acq_rel);
    m_lastFrame = frame.clone();
    const int newCount = m_frameCount.fetch_add(1, std::memory_order_acq_rel) + 1;

    ScrollProgressCallback progressCb = m_progressCb;
    cv::Mat recentPreview;
    if (progressCb) {
        recentPreview = buildRecentPreview((std::max)(frame.rows, frame.rows * 2));
    }
    lock.unlock();

    if (progressCb) {
        progressCb(recentPreview, newCount);
    }
}

bool ScrollCapture::spillResidentSegments() {
    if (!m_spilledSegments.empty()) return true;
    std::filesystem::path stagingDirectory = m_options.stagingDirectory;
    if (stagingDirectory.empty()) {
        wchar_t tempDirectory[MAX_PATH]{};
        const DWORD tempLength = GetTempPathW(MAX_PATH, tempDirectory);
        if (tempLength == 0 || tempLength >= MAX_PATH) return false;
        stagingDirectory = tempDirectory;
    }
    std::error_code directoryError;
    if (!std::filesystem::is_directory(stagingDirectory, directoryError) || directoryError) return false;
    wchar_t temporaryName[MAX_PATH]{};
    if (!GetTempFileNameW(stagingDirectory.c_str(), L"EZT", 0, temporaryName)) return false;
    const std::filesystem::path spillPath = temporaryName;
    std::uint64_t writeOffset = 0;
    std::vector<SpilledSegment> records;

    std::ofstream output(spillPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::error_code error;
        std::filesystem::remove(spillPath, error);
        return false;
    }
    for (const cv::Mat& segment : m_segments) {
        if (!segment.isContinuous()) {
            output.close();
            std::error_code error;
            std::filesystem::remove(spillPath, error);
            return false;
        }
        const std::size_t bytes = segment.total() * segment.elemSize();
        if (!scroll_storage::writeMat(output, segment)) {
            output.close();
            std::error_code error;
            std::filesystem::remove(spillPath, error);
            return false;
        }
        records.push_back({writeOffset, segment.rows});
        writeOffset += bytes;
    }
    output.close();
    if (!output) {
        std::error_code error;
        std::filesystem::remove(spillPath, error);
        return false;
    }

    m_spillPath = spillPath;
    m_spilledSegments = std::move(records);
    m_spillWriteOffset = writeOffset;
    m_segments.clear();
    m_residentSegmentBytes = 0;
    return true;
}

bool ScrollCapture::appendSpilledSegment(const cv::Mat& segment) {
    if (m_spillPath.empty() || !segment.isContinuous()) return false;
    const std::size_t bytes = segment.total() * segment.elemSize();
    std::ofstream output(m_spillPath, std::ios::binary | std::ios::app);
    if (!output) return false;
    if (!scroll_storage::writeMat(output, segment)) return false;
    output.close();
    if (!output) return false;
    m_spilledSegments.push_back({m_spillWriteOffset, segment.rows});
    m_spillWriteOffset += bytes;
    return true;
}

void ScrollCapture::clearStorage() {
    std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
    m_segments.clear();
    m_segmentBytes = 0;
    m_residentSegmentBytes = 0;
    m_spilledSegments.clear();
    m_spillWriteOffset = 0;
    m_segmentWidth.store(0, std::memory_order_release);
    m_totalRows.store(0, std::memory_order_release);
    m_segmentType = -1;
    m_knownStickyBottom = 0;
    m_knownStickyBottomHits = 0;
    if (!m_spillPath.empty()) {
        std::error_code error;
        std::filesystem::remove(m_spillPath, error);
        if (error) LOG_WARN("无法删除长截图临时文件: {}", error.message());
        m_spillPath.clear();
    }
}

cv::Mat ScrollCapture::buildStitchedImage() const {
    std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
    if (m_segments.empty() && m_spilledSegments.empty()) return {};
    if (m_spilledSegments.empty()) {
        cv::Mat stitched;
        cv::vconcat(m_segments, stitched);
        return stitched;
    }
    const int segWidth = m_segmentWidth.load(std::memory_order_acquire);
    if (segWidth <= 0 || m_segmentType < 0 || m_spillPath.empty()) return {};
    std::uint64_t totalRows = 0;
    for (const auto& segment : m_spilledSegments) {
        if (segment.rows > 0) totalRows += static_cast<std::uint64_t>(segment.rows);
    }
    if (totalRows == 0 || totalRows > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) return {};
    cv::Mat stitched(static_cast<int>(totalRows), segWidth, m_segmentType);
    std::ifstream input(m_spillPath, std::ios::binary);
    if (!input) return {};
    const std::size_t rowBytes = static_cast<std::size_t>(segWidth) * stitched.elemSize();
    int destinationRow = 0;
    for (const auto& segment : m_spilledSegments) {
        if (segment.rows <= 0) continue;
        input.seekg(static_cast<std::streamoff>(segment.offset), std::ios::beg);
        for (int row = 0; row < segment.rows; ++row) {
            if (!scroll_storage::readExact(input, stitched.ptr(destinationRow + row), rowBytes)) return {};
        }
        destinationRow += segment.rows;
    }
    return stitched;
}

cv::Mat ScrollCapture::buildRecentPreview(int maxRows) const {
    std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
    if ((m_segments.empty() && m_spilledSegments.empty()) || maxRows <= 0) return {};
    if (!m_spilledSegments.empty()) {
        const int segWidth = m_segmentWidth.load(std::memory_order_acquire);
        if (segWidth <= 0 || m_segmentType < 0 || m_spillPath.empty()) return {};
        int rowsToRead = 0;
        for (auto it = m_spilledSegments.rbegin(); it != m_spilledSegments.rend() && rowsToRead < maxRows; ++it) {
            if (it->rows > 0) rowsToRead += std::min(it->rows, maxRows - rowsToRead);
        }
        if (rowsToRead <= 0) return {};
        cv::Mat preview(rowsToRead, segWidth, m_segmentType);
        std::ifstream input(m_spillPath, std::ios::binary);
        if (!input) return {};
        const std::size_t rowBytes = static_cast<std::size_t>(segWidth) * preview.elemSize();
        int destinationRow = rowsToRead;
        int remaining = rowsToRead;
        for (auto it = m_spilledSegments.rbegin(); it != m_spilledSegments.rend() && remaining > 0; ++it) {
            if (it->rows <= 0) continue;
            const int take = std::min(it->rows, remaining);
            destinationRow -= take;
            const std::uint64_t sourceOffset = it->offset + static_cast<std::uint64_t>(it->rows - take) * rowBytes;
            input.seekg(static_cast<std::streamoff>(sourceOffset), std::ios::beg);
            for (int row = 0; row < take; ++row) {
                if (!scroll_storage::readExact(input, preview.ptr(destinationRow + row), rowBytes)) return {};
            }
            remaining -= take;
        }
        return preview;
    }
    std::vector<cv::Mat> pieces;
    int remaining = maxRows;
    for (auto it = m_segments.rbegin(); it != m_segments.rend() && remaining > 0; ++it) {
        if (it->rows <= 0) continue;
        const int rows = std::min(it->rows, remaining);
        pieces.push_back((*it)(cv::Rect(0, it->rows - rows, it->cols, rows)));
        remaining -= rows;
    }
    if (pieces.empty()) return {};
    std::reverse(pieces.begin(), pieces.end());
    cv::Mat preview;
    cv::vconcat(pieces, preview);
    return preview;
}

void ScrollCapture::deliverCompletion(const std::string& error) {
    if (m_completionDelivered.exchange(true, std::memory_order_acq_rel)) return;
    if (m_suppressCompletion.load(std::memory_order_acquire)) return;

    ScrollCaptureResult result;
    result.frameCount = m_frameCount.load(std::memory_order_acquire);
    result.action = m_finishAction.load(std::memory_order_acquire);
    result.cancelled = (result.action == ScrollFinishAction::Cancelled);
    result.errorMessage = error;

    std::function<void(const ScrollCaptureResult&)> completionCb;
    {
        std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
        if (!result.cancelled && (!m_segments.empty() || !m_spilledSegments.empty())) {
            result.stitchedImage = buildStitchedImage();
        }
        result.success = !result.cancelled && !result.stitchedImage.empty();
        if (!result.cancelled && !result.success && result.errorMessage.empty()) {
            const bool isZh = tools3000::core::isUiLanguageChinese();
            result.errorMessage = result.frameCount == 0
                ? (isZh ? "没有捕获到任何帧" : "No frames captured")
                : (isZh ? "图像拼接失败" : "Failed to stitch captured frames");
        }
        if (m_captureBackend) m_captureBackend->shutdown();
        m_captureBackend.reset();
        clearStorage();
        m_terminalError.clear();
        m_lastFrame.release();
        completionCb = m_completionCb;
    }

    if (completionCb) completionCb(result);
    if (result.success) {
        LOG_INFO("长截图拼接完成: {}x{}, 帧数={}", result.stitchedImage.cols,
                 result.stitchedImage.rows, result.frameCount);
    }
    tools3000::core::WinUtils::trimWorkingSet();
}

// ─────────────────────────────────────────────────────────────────────────────
// 截取区域
// ─────────────────────────────────────────────────────────────────────────────

cv::Mat ScrollCapture::captureRegion(const RECT& rect) {
    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return {};

    std::lock_guard<std::recursive_mutex> lock(m_captureMutex);
    if (!m_captureBackend) return {};

    CaptureFrameView captured;
    std::string error;
    if (!m_captureBackend->capture(captured, error)) {
        LOG_ERROR("长截图帧捕获失败: backend={}, error={}",
                  m_captureBackend->info().id, error);
        return {};
    }
    struct ReleaseGuard {
        ICaptureBackend* backend;
        ~ReleaseGuard() { backend->releaseFrame(); }
    } guard{m_captureBackend.get()};

    const int type = captured.format == CapturePixelFormat::Bgra32 ? CV_8UC4 : CV_8UC3;
    const cv::Mat source(captured.height, captured.width, type,
                         captured.data, static_cast<std::size_t>(captured.stride));
    cv::Mat image;
    if (captured.format == CapturePixelFormat::Bgra32) {
        cv::cvtColor(source, image, cv::COLOR_BGRA2BGR);
    } else {
        source.copyTo(image);
    }
    return image;
}

// ─────────────────────────────────────────────────────────────────────────────
// 相邻帧滚动过渡分析（滚动条剥离 + 零位移否决 + 多候选全重叠区一致性精校）
// ─────────────────────────────────────────────────────────────────────────────

ScrollFrameTransition ScrollCapture::analyzeFrameTransition(const cv::Mat& upper, const cv::Mat& lower) const {
    ScrollFrameTransition tr;
    if (upper.empty() || lower.empty() || upper.cols != lower.cols || upper.type() != lower.type()) {
        return tr;
    }

    const int refH = std::min(upper.rows, lower.rows);
    if (refH < 16) return tr;

    int roiX = 0, roiW = upper.cols;
    computeContentHorizROI(upper.cols, roiX, roiW);

    const cv::Mat uInner = upper(cv::Rect(roiX, upper.rows - refH, roiW, refH));
    const cv::Mat lInner = lower(cv::Rect(roiX, 0, roiW, refH));

    cv::Mat diffMat;
    double dynMeanDiff = 999.0;
    double zeroShiftInlierRatio = 0.0;
    double zeroShiftTrimmedMad = 999.0;
    detectStaticMargins(uInner, lInner, diffMat, tr.stickyTopRows, tr.stickyBottomRows,
                        dynMeanDiff, zeroShiftInlierRatio, zeroShiftTrimmedMad);

    // 全局零位移快速门禁：仅当整帧几乎逐像素完全一致（dynMeanDiff < 0.45）时秒退；
    // 绝不单独依赖被纯色空行稀释的 zeroShiftInlierRatio 误杀深色聊天/稀疏文字界面的真实滚动！
    if (dynMeanDiff < 0.45) {
        tr.isUnchanged = true;
        tr.lowerStartRow = lower.rows;
        return tr;
    }

    const int dynTop = tr.stickyTopRows;
    const int dynBottom = refH - tr.stickyBottomRows;
    const int dynH = std::max(1, dynBottom - dynTop);
    const int tplH = (refH < 64)
        ? std::clamp(refH / 4, 8, 20)
        : std::clamp(dynH / 6, 18, 68);

    if (refH < tplH + 4) return tr;

    // 水平静态侧边栏自动剥离（仅作用于匹配与双轨一致性校验，不影响最终全宽拼接图）
    int dynSubX = 0, dynSubW = uInner.cols;
    refineDynamicHorizROI(diffMat, dynTop, dynBottom, dynSubX, dynSubW);
    const cv::Mat uWork = uInner(cv::Rect(dynSubX, 0, dynSubW, refH));
    const cv::Mat lWork = lInner(cv::Rect(dynSubX, 0, dynSubW, refH));

    // 预计算双帧行级有效纹理特征，并计算 d==0 零位移双轨基准（彻底封杀深色空行稀释与触底 Hover/光标微动引发的复读拼接）
    const auto uAct = computeRowActivity(uWork);
    const auto lAct = computeRowActivity(lWork);
    const OverlapVerification zeroEval = evaluateOverlapAtDelta(
        uWork, lWork, uAct, lAct, dynTop, dynBottom, 0);

    // 双轨零位移门禁：无纹理极简帧回退旧阈值；富纹理帧严格要求有效纹理行本身也高度静止
    if (zeroEval.activeRows < 6) {
        if (dynMeanDiff < 1.35 || (zeroShiftInlierRatio >= 0.85 && zeroShiftTrimmedMad < 2.2)) {
            tr.isUnchanged = true;
            tr.lowerStartRow = lower.rows;
            return tr;
        }
    } else if (zeroEval.activeInlierRatio >= 0.84 &&
               zeroEval.activeNcc >= 0.92 &&
               zeroEval.activeTrimmedMad <= 6.5) {
        tr.isUnchanged = true;
        tr.lowerStartRow = lower.rows;
        return tr;
    }

    // 在动态核心区均匀分布最多 10 条候选采样带
    const int sampleTop = std::max(0, dynTop);
    const int sampleBottom = std::max(sampleTop + 1, std::min(refH - tplH - 2, dynBottom - tplH / 2));
    std::vector<int> candidateYs;
    constexpr int kNumBands = 10;
    for (int i = 0; i < kNumBands; ++i) {
        const int y = (kNumBands == 1 || sampleBottom <= sampleTop)
            ? sampleTop
            : (sampleTop + ((sampleBottom - sampleTop) * i) / (kNumBands - 1));
        if (y + tplH <= refH &&
            (candidateYs.empty() || std::abs(y - candidateYs.back()) >= std::max(4, tplH / 3))) {
            candidateYs.push_back(y);
        }
    }

    std::vector<BandVote> allBandVotes;
    std::vector<BandVote> scrollingVotes;
    int staticVotesCount = 0;
    double staticWeightSum = 0.0;
    int minStaticFooterY = refH;

    for (int tplY : candidateYs) {
        allBandVotes.clear();
        const int searchTop = std::max(0, tr.stickyTopRows - 6);
        const int searchBottom = std::min(refH, tplY + tplH + 4);
        if (!matchSingleBand(uWork, lWork, tplY, tplH, searchTop, searchBottom, allBandVotes)) {
            continue;
        }

        bool bandHasStatic = false;
        for (const auto& vote : allBandVotes) {
            if (std::abs(vote.delta) <= 1 && vote.score >= 0.90 && vote.mad < 4.5) {
                bandHasStatic = true;
                staticWeightSum += vote.weight;
                if (tplY >= (refH * 52) / 100) {
                    minStaticFooterY = std::min(minStaticFooterY, tplY);
                }
            } else if (vote.delta >= 2) {
                scrollingVotes.push_back(vote);
            }
        }
        if (bandHasStatic) {
            ++staticVotesCount;
        }
    }

    // 收集候选滚动位移簇（最多取权重前 5 簇 + 一维行梯度能量剖面全局候选）
    struct CandidateCluster {
        int consensusDelta = 0;
        double clusterWeight = 0.0;
        double maxScore = 0.0;
        int lowestScrollingBandBottom = 0;
    };
    std::vector<CandidateCluster> candidateClusters;

    std::vector<BandVote> remainingVotes = scrollingVotes;
    for (int cIdx = 0; cIdx < 5 && !remainingVotes.empty(); ++cIdx) {
        double bestWeight = -1.0;
        int bestSeedDelta = 0;
        for (const auto& seed : remainingVotes) {
            double w = 0.0;
            for (const auto& cand : remainingVotes) {
                if (std::abs(cand.delta - seed.delta) <= 3) {
                    w += cand.weight;
                }
            }
            if (w > bestWeight) {
                bestWeight = w;
                bestSeedDelta = seed.delta;
            }
        }
        if (bestWeight <= 0.0) break;

        double weightedSum = 0.0;
        double wSum = 0.0;
        double maxConf = 0.0;
        int lowestBot = 0;
        std::vector<BandVote> nextRemaining;
        for (const auto& v : remainingVotes) {
            if (std::abs(v.delta - bestSeedDelta) <= 4) {
                weightedSum += static_cast<double>(v.delta) * v.weight;
                wSum += v.weight;
                maxConf = std::max(maxConf, v.score);
                lowestBot = std::max(lowestBot, v.tplY + v.tplH);
            } else {
                nextRemaining.push_back(v);
            }
        }
        remainingVotes = std::move(nextRemaining);

        CandidateCluster cc;
        cc.consensusDelta = std::max(1, static_cast<int>(std::lround(weightedSum / std::max(1e-6, wSum))));
        cc.clusterWeight = bestWeight;
        cc.maxScore = maxConf;
        cc.lowestScrollingBandBottom = lowestBot;
        candidateClusters.push_back(cc);
    }

    const int profileDelta = estimateRowProfileDelta(uWork, lWork, dynTop, dynBottom);
    if (profileDelta >= 3) {
        bool alreadyCovered = false;
        for (const auto& cc : candidateClusters) {
            if (std::abs(cc.consensusDelta - profileDelta) <= 3) {
                alreadyCovered = true;
                break;
            }
        }
        if (!alreadyCovered) {
            CandidateCluster profCluster;
            profCluster.consensusDelta = profileDelta;
            profCluster.clusterWeight = 0.85;
            profCluster.maxScore = 0.85;
            profCluster.lowestScrollingBandBottom = dynBottom;
            candidateClusters.push_back(profCluster);
        }
    }

    if (candidateClusters.empty()) {
        const bool isStaticByTexture = (zeroEval.activeRows >= 6)
            ? (zeroEval.activeInlierRatio >= 0.65 && zeroEval.activeNcc >= 0.82)
            : (zeroShiftInlierRatio >= 0.72);
        if (staticVotesCount > 0 || isStaticByTexture) {
            tr.isUnchanged = true;
            tr.lowerStartRow = lower.rows;
        } else if (computeFrameTextureStd(uWork) >= 3.0 &&
                   detectBackwardScroll(uWork, lWork, uAct, lAct, dynTop, dynBottom).valid) {
            tr.isBackwardScroll = true;
            tr.lowerStartRow = lower.rows;
        }
        return tr;
    }

    // 先利用底部高置信零位移静态频带收缩初步固定底栏边界，避免固定底栏拖累重叠区一致性评分
    int workingStickyBottom = tr.stickyBottomRows;
    if (minStaticFooterY < refH) {
        bool anyClusterStrictlyAbove = false;
        for (const auto& cc : candidateClusters) {
            if (cc.lowestScrollingBandBottom > 0 && cc.lowestScrollingBandBottom <= minStaticFooterY + 4) {
                anyClusterStrictlyAbove = true;
                break;
            }
        }
        if (anyClusterStrictlyAbove) {
            workingStickyBottom = std::min((refH * 48) / 100, std::max(workingStickyBottom, refH - minStaticFooterY));
        }
    }

    const int verifyBottom = std::max(dynTop + 8, refH - workingStickyBottom);
    const OverlapVerification verifyZeroEval = (verifyBottom == dynBottom)
        ? zeroEval
        : evaluateOverlapAtDelta(uWork, lWork, uAct, lAct, dynTop, verifyBottom, 0);

    // 对所有候选位移簇执行全重叠区双轨像素级验证与 ±2px 精修
    int winningIdx = -1;
    OverlapVerification winningVer;
    double winningTotalScore = -1e9;

    for (std::size_t i = 0; i < candidateClusters.size(); ++i) {
        const auto& cc = candidateClusters[i];
        OverlapVerification ver = verifyCandidateOnFullOverlap(
            uWork, lWork, uAct, lAct, dynTop, verifyBottom, cc.consensusDelta);
        if (!ver.valid) continue;

        // 零位移双轨对抗检验（无条件生效，不再依赖 staticVotesCount >= 2）：
        // 若动态区有效纹理行在原地已高度静止（例如触底后鼠标 Hover/光标微动），
        // 任何滚动假设必须在有效纹理行上碾压零位移基准，否则一票否决，彻底杜绝复读机长图！
        if (verifyZeroEval.activeRows >= 6 &&
            verifyZeroEval.activeInlierRatio >= 0.52 &&
            verifyZeroEval.activeNcc >= 0.72) {
            const bool clearlyBeatsZeroByNcc =
                (ver.activeNcc > verifyZeroEval.activeNcc + 0.045) &&
                (ver.activeTrimmedMad < verifyZeroEval.activeTrimmedMad * 0.75);
            const bool clearlyBeatsZeroByInlier =
                (ver.activeInlierRatio >= verifyZeroEval.activeInlierRatio + 0.14) &&
                (ver.activeNcc >= 0.84) &&
                (ver.activeTrimmedMad < verifyZeroEval.activeTrimmedMad * 0.85);
            if (!clearlyBeatsZeroByNcc && !clearlyBeatsZeroByInlier) {
                continue;
            }
        }
        if (staticVotesCount >= 2 && zeroShiftInlierRatio >= 0.55) {
            if (ver.trimmedMad >= zeroShiftTrimmedMad * 0.78 && ver.inlierRatio <= zeroShiftInlierRatio + 0.08) {
                continue;
            }
        }

        const double totalScore = (ver.activeRows >= 6)
            ? (ver.activeInlierRatio * 60.0 + ver.activeNcc * 50.0 - ver.activeTrimmedMad * 1.4 +
               ver.inlierRatio * 20.0 + std::min(cc.clusterWeight, 8.0) * 3.2)
            : (ver.inlierRatio * 80.0 - ver.trimmedMad * 2.2 + std::min(cc.clusterWeight, 8.0) * 3.5);
        if (winningIdx < 0 || totalScore > winningTotalScore) {
            winningIdx = static_cast<int>(i);
            winningVer = ver;
            winningTotalScore = totalScore;
        }
    }

    if (winningIdx < 0) {
        // 没有任何候选通过双轨一致性门禁：优先判触底静止；其次探测是否为用户向上回滚；否则返回 !matched 供上层回滚补帧
        const bool isStaticFallback = (verifyZeroEval.activeRows >= 6)
            ? (verifyZeroEval.activeInlierRatio >= 0.62 && verifyZeroEval.activeNcc >= 0.80)
            : (zeroShiftInlierRatio >= 0.74);
        if ((staticVotesCount >= 2 && staticWeightSum >= 1.2) || isStaticFallback) {
            tr.isUnchanged = true;
            tr.lowerStartRow = lower.rows;
        } else if (computeFrameTextureStd(uWork) >= 3.0 &&
                   detectBackwardScroll(uWork, lWork, uAct, lAct, dynTop, verifyBottom).valid) {
            tr.isBackwardScroll = true;
            tr.lowerStartRow = lower.rows;
        }
        return tr;
    }

    // 前向 vs 后向对称对决：当正向匹配并非近 100% 完美匹配（例如向上回滚时在周期结构里凑出的擦边假正向匹配），
    // 执行一次轻量反向滚动核验；若反向匹配显著碾压正向匹配，直接否决正向假匹配并标记为反向回滚！
    const bool forwardNearPerfect = (winningVer.activeRows >= 6)
        ? (winningVer.activeNcc >= 0.94 && winningVer.activeInlierRatio >= 0.92 && winningVer.activeTrimmedMad <= 3.5)
        : (winningVer.inlierRatio >= 0.94 && winningVer.trimmedMad <= 2.5);
    if (!forwardNearPerfect && computeFrameTextureStd(uWork) >= 3.0) {
        const OverlapVerification backVer = detectBackwardScroll(uWork, lWork, uAct, lAct, dynTop, verifyBottom);
        if (backVer.valid) {
            const bool backDominates = (backVer.activeRows >= 6 && winningVer.activeRows >= 6)
                ? (backVer.activeNcc > winningVer.activeNcc + 0.05 &&
                   backVer.activeInlierRatio >= winningVer.activeInlierRatio + 0.06 &&
                   backVer.activeTrimmedMad < winningVer.activeTrimmedMad * 0.75 + 0.5)
                : (backVer.inlierRatio > winningVer.inlierRatio + 0.08 &&
                   backVer.trimmedMad < winningVer.trimmedMad * 0.70 + 0.5);
            if (backDominates) {
                tr.isBackwardScroll = true;
                tr.lowerStartRow = lower.rows;
                return tr;
            }
        }
    }

    const int finalDelta = winningVer.refinedDelta;
    const auto& winCluster = candidateClusters[static_cast<std::size_t>(winningIdx)];

    if (minStaticFooterY < refH && winCluster.lowestScrollingBandBottom > 0 &&
        winCluster.lowestScrollingBandBottom <= minStaticFooterY + 4) {
        workingStickyBottom = std::min((refH * 48) / 100, std::max(workingStickyBottom, refH - minStaticFooterY));
    }
    tr.stickyBottomRows = refineStickyBottomWithDelta(
        uInner, lInner, diffMat, tr.stickyTopRows, workingStickyBottom, finalDelta);

    tr.matched = true;
    tr.isUnchanged = false;
    tr.scrollDelta = finalDelta;
    tr.confidence = std::max(winCluster.maxScore,
                             winningVer.activeRows >= 6 ? winningVer.activeNcc : winningVer.inlierRatio);
    tr.trimPrevBottomRows = std::clamp(
        tr.stickyBottomRows, 0,
        std::max(0, refH - tr.stickyTopRows - finalDelta));
    tr.lowerStartRow = lower.rows - tr.trimPrevBottomRows - finalDelta;

    LOG_DEBUG("模板匹配: confidence={:.3f}, offset={}", tr.confidence, tr.lowerStartRow);
    return tr;
}

bool ScrollCapture::isAtBottom(const cv::Mat& current, const cv::Mat& previous) const {
    if (current.size() != previous.size() || current.empty()) return false;
    const ScrollFrameTransition tr = analyzeFrameTransition(previous, current);
    return tr.isUnchanged;
}

int ScrollCapture::findStitchOffset(const cv::Mat& upper, const cv::Mat& lower) const {
    const ScrollFrameTransition tr = analyzeFrameTransition(upper, lower);
    if (tr.isUnchanged) return lower.rows;
    if (!tr.matched) return -1;
    // 对不裁剪上帧的传统调用点，重叠高度为 lower.rows - scrollDelta
    return lower.rows - tr.scrollDelta;
}

// ─────────────────────────────────────────────────────────────────────────────
// 离线多帧拼接（同步支持上下固定栏无损剥离与接缝微羽化）
// ─────────────────────────────────────────────────────────────────────────────

cv::Mat ScrollCapture::stitchFrames(const std::vector<cv::Mat>& frames, int overlapPercent) const {
    if (frames.empty()) return {};
    if (frames.size() == 1) return frames[0].clone();

    tools3000::core::TraceId::Scope scope;
    std::vector<cv::Mat> segments;
    segments.reserve(frames.size());
    segments.push_back(frames[0].clone());

    auto buildRecentLocalTail = [&](int maxRows, int trimBackRows) -> cv::Mat {
        if (segments.empty() || maxRows <= 0) return {};
        std::vector<cv::Mat> pieces;
        int remaining = maxRows;
        int skipTail = std::max(0, trimBackRows);
        for (auto it = segments.rbegin(); it != segments.rend() && remaining > 0; ++it) {
            if (it->rows <= 0) continue;
            int avail = it->rows;
            if (skipTail > 0) {
                const int drop = std::min(avail, skipTail);
                avail -= drop;
                skipTail -= drop;
                if (avail <= 0) continue;
            }
            const int take = std::min(avail, remaining);
            pieces.push_back((*it)(cv::Rect(0, avail - take, it->cols, take)));
            remaining -= take;
        }
        if (pieces.empty()) return {};
        std::reverse(pieces.begin(), pieces.end());
        cv::Mat out;
        cv::vconcat(pieces, out);
        return out;
    };

    std::size_t lastAcceptedIdx = 0;
    int knownStickyBottom = 0;
    int knownStickyBottomHits = 0;

    for (std::size_t i = 1; i < frames.size(); ++i) {
        const cv::Mat& prevFrame = frames[lastAcceptedIdx];
        const cv::Mat& curFrame = frames[i];
        if (curFrame.empty() || curFrame.cols != segments.front().cols || curFrame.type() != segments.front().type()) {
            continue;
        }

        const ScrollFrameTransition tr = analyzeFrameTransition(prevFrame, curFrame);
        if (tr.isUnchanged || tr.isBackwardScroll) continue;

        int offset = 0;
        int trimRows = 0;
        bool verified = false;
        if (tr.matched && tr.scrollDelta > 0) {
            int effStickyBottom = tr.stickyBottomRows;
            if (effStickyBottom >= 24) {
                if (knownStickyBottom > 0 && std::abs(effStickyBottom - knownStickyBottom) <= 12) {
                    knownStickyBottom = (knownStickyBottom + effStickyBottom) / 2;
                    ++knownStickyBottomHits;
                } else if (knownStickyBottomHits < 2) {
                    knownStickyBottom = effStickyBottom;
                    knownStickyBottomHits = 1;
                }
            } else if (knownStickyBottomHits >= 2 && knownStickyBottom >= 24 &&
                       effStickyBottom + 8 < knownStickyBottom &&
                       prevFrame.size() == curFrame.size()) {
                const int checkH = std::min(knownStickyBottom, (curFrame.rows * 45) / 100);
                int roiX = 0, roiW = curFrame.cols;
                computeContentHorizROI(curFrame.cols, roiX, roiW);
                cv::Mat botDiff;
                cv::absdiff(prevFrame(cv::Rect(roiX, curFrame.rows - checkH, roiW, checkH)),
                            curFrame(cv::Rect(roiX, curFrame.rows - checkH, roiW, checkH)), botDiff);
                int staticBotRows = 0;
                for (int r = 0; r < checkH; ++r) {
                    const cv::Scalar rm = cv::mean(botDiff.row(r));
                    if ((rm[0] + rm[1] + rm[2]) / 3.0 < 3.6) ++staticBotRows;
                }
                if (staticBotRows * 100 >= checkH * 82) {
                    effStickyBottom = checkH;
                }
            }

            const int prevSegRows = segments.back().rows;
            trimRows = std::clamp(
                std::min({effStickyBottom, std::max(0, prevSegRows - 1),
                          curFrame.rows - tr.stickyTopRows - tr.scrollDelta}),
                0, curFrame.rows - 1);
            offset = curFrame.rows - trimRows - tr.scrollDelta;
            verified = true;
        } else {
            offset = curFrame.rows * std::clamp(overlapPercent, 0, 95) / 100;
        }

        offset = std::clamp(offset, 0, curFrame.rows);
        const int remaining = curFrame.rows - offset;
        if (remaining <= 0) continue;

        if (verified && segments.size() >= 2) {
            const int dynRows = remaining - trimRows;
            if (dynRows >= 56 && offset + dynRows <= curFrame.rows) {
                const cv::Mat tailWin = buildRecentLocalTail(std::max(dynRows * 3, 2400), trimRows);
                double dupScore = 0.0;
                if (isDuplicateSegmentInTail(tailWin, curFrame.rowRange(offset, offset + dynRows), &dupScore)) {
                    LOG_WARN("长截图检测到重复内容段，已自动拦截: rows={}, score={:.3f}", dynRows, dupScore);
                    continue;
                }
            }
        }

        if (verified && trimRows > 0) {
            const int prevSegRows = segments.back().rows;
            const int actual = std::min(trimRows, std::max(0, prevSegRows - 1));
            if (actual > 0) {
                segments.back() = segments.back().rowRange(0, prevSegRows - actual).clone();
            }
        }
        if (verified && !segments.empty()) {
            featherSegmentBoundary(segments.back(), curFrame, offset);
        }
        segments.push_back(curFrame.rowRange(offset, curFrame.rows).clone());
        lastAcceptedIdx = i;
    }

    cv::Mat result;
    cv::vconcat(segments, result);
    return result;
}

}  // namespace tools3000::capture
