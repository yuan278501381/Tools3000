// ─────────────────────────────────────────────────────────────────────────────
// DeepOcrEnginePostprocess.cpp — PP-OCRv4 检测后处理、透视矫正、CTC 字符切分、UI 下拉箭头剥离与 JSON 标点规整
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/DeepOcrEngineInternal.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tools3000::ocr {

using namespace deep_ocr_detail;

namespace deep_ocr_detail {

std::array<cv::Point2f, 4> orderQuadPointsClockwise(const cv::Point2f rawPts[4]) noexcept {
    std::array<cv::Point2f, 4> sorted{rawPts[0], rawPts[1], rawPts[2], rawPts[3]};
    std::sort(sorted.begin(), sorted.end(), [](const cv::Point2f& a, const cv::Point2f& b) {
        return (a.x != b.x) ? (a.x < b.x) : (a.y < b.y);
    });

    cv::Point2f tl = sorted[0];
    cv::Point2f bl = sorted[1];
    if (tl.y > bl.y) std::swap(tl, bl);

    cv::Point2f tr = sorted[2];
    cv::Point2f br = sorted[3];
    if (tr.y > br.y) std::swap(tr, br);

    return {tl, tr, br, bl};
}


cv::Vec3b estimateBorderBackgroundColor(const cv::Mat& patch, bool* outUniform) {
    if (outUniform) *outUniform = false;
    if (patch.empty() || patch.type() != CV_8UC3) {
        return cv::Vec3b(255, 255, 255);
    }
    const int rows = patch.rows;
    const int cols = patch.cols;
    const int stride = (std::max)(1, cols / 64);

    std::vector<uint8_t> bVals, gVals, rVals;
    const size_t approx = static_cast<size_t>(((cols + stride - 1) / stride) * (rows > 1 ? 2 : 1));
    bVals.reserve(approx);
    gVals.reserve(approx);
    rVals.reserve(approx);

    auto collectRow = [&](int ry) {
        const auto* row = patch.ptr<cv::Vec3b>(ry);
        for (int x = 0; x < cols; x += stride) {
            bVals.push_back(row[x][0]);
            gVals.push_back(row[x][1]);
            rVals.push_back(row[x][2]);
        }
    };
    collectRow(0);
    if (rows > 1) collectRow(rows - 1);
    if (bVals.empty()) {
        return cv::Vec3b(255, 255, 255);
    }

    auto medianOf = [](std::vector<uint8_t>& v) -> uint8_t {
        const size_t mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
        return v[mid];
    };
    std::vector<uint8_t> bCopy = bVals, gCopy = gVals, rCopy = rVals;
    const cv::Vec3b bg(medianOf(bCopy), medianOf(gCopy), medianOf(rCopy));

    if (outUniform) {
        size_t closeCount = 0;
        for (size_t i = 0; i < bVals.size(); ++i) {
            const int db = std::abs(static_cast<int>(bVals[i]) - static_cast<int>(bg[0]));
            const int dg = std::abs(static_cast<int>(gVals[i]) - static_cast<int>(bg[1]));
            const int dr = std::abs(static_cast<int>(rVals[i]) - static_cast<int>(bg[2]));
            if ((std::max)({db, dg, dr}) <= 24) {
                ++closeCount;
            }
        }
        *outUniform = (static_cast<float>(closeCount) / static_cast<float>(bVals.size())) >= 0.55f;
    }
    return bg;
}


bool isAxisAlignedHorizontalQuad(const DbDetectedQuad& q, cv::Rect* outBox) noexcept {
    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
    for (const auto& pt : q.corners) {
        minX = (std::min)(minX, pt.x);
        minY = (std::min)(minY, pt.y);
        maxX = (std::max)(maxX, pt.x);
        maxY = (std::max)(maxY, pt.y);
    }
    const int bx1 = static_cast<int>(std::floor(minX));
    const int by1 = static_cast<int>(std::floor(minY));
    const int bx2 = (std::max)(bx1 + 1, static_cast<int>(std::ceil(maxX)));
    const int by2 = (std::max)(by1 + 1, static_cast<int>(std::ceil(maxY)));
    if (outBox) {
        *outBox = cv::Rect(bx1, by1, bx2 - bx1, by2 - by1);
    }
    const float topSkew = std::abs(q.corners[1].y - q.corners[0].y);
    const float botSkew = std::abs(q.corners[2].y - q.corners[3].y);
    const float leftSkew = std::abs(q.corners[3].x - q.corners[0].x);
    const float rightSkew = std::abs(q.corners[2].x - q.corners[1].x);
    return topSkew <= DeepOcrConstants::kAxisAlignedMaxSkewPx &&
           botSkew <= DeepOcrConstants::kAxisAlignedMaxSkewPx &&
           leftSkew <= DeepOcrConstants::kAxisAlignedMaxSkewPx &&
           rightSkew <= DeepOcrConstants::kAxisAlignedMaxSkewPx &&
           (by2 - by1) < static_cast<int>(static_cast<float>(bx2 - bx1) * 1.45f);
}


} // namespace deep_ocr_detail

bool DeepOcrEngine::isLowContrastScreenPhoto(const cv::Mat& bgr) {
    if (bgr.empty() || bgr.cols < 64 || bgr.rows < 64 || bgr.type() != CV_8UC3) {
        return false;
    }

    // 4px 步长快采样 (< 0.2ms)，精准区分原生数字渲染截图与手机实拍/屏摄低反差摩尔纹照片
    constexpr int kStep = 4;
    uint32_t sampled = 0;
    uint32_t midGrayCount = 0;
    uint32_t pureWhiteCount = 0;
    uint32_t flatPairs = 0;
    uint32_t totalPairs = 0;

    for (int y = 0; y < bgr.rows; y += kStep) {
        const auto* row = bgr.ptr<cv::Vec3b>(y);
        int prevLum = -1;
        for (int x = 0; x < bgr.cols; x += kStep) {
            const int lum = (static_cast<int>(row[x][0]) * 29 +
                             static_cast<int>(row[x][1]) * 150 +
                             static_cast<int>(row[x][2]) * 77) >> 8;
            ++sampled;
            if (lum >= 50 && lum <= 200) ++midGrayCount;
            if (lum >= 242) ++pureWhiteCount;
            if (x + 1 < bgr.cols) {
                const int nextLum = (static_cast<int>(row[x + 1][0]) * 29 +
                                     static_cast<int>(row[x + 1][1]) * 150 +
                                     static_cast<int>(row[x + 1][2]) * 77) >> 8;
                ++totalPairs;
                if (nextLum == lum) ++flatPairs;
            }
            prevLum = lum;
        }
        (void)prevLum;
    }

    if (sampled < 64 || totalPairs < 64) {
        return false;
    }

    const float flatRatio = static_cast<float>(flatPairs) / static_cast<float>(totalPairs);
    const float midGrayRatio = static_cast<float>(midGrayCount) / static_cast<float>(sampled);
    const float pureWhiteRatio = static_cast<float>(pureWhiteCount) / static_cast<float>(sampled);

    // 原生数字截图平坦度通常 >= 0.70 或具有大片纯白底色；实拍/屏摄照片通常平坦度 < 0.45 且大面积挤在中灰区间
    return (flatRatio < 0.45f) && (midGrayRatio >= 0.45f) && (pureWhiteRatio < 0.15f);
}

cv::Mat DeepOcrEngine::enhanceLowContrastForDetection(const cv::Mat& bgr) {
    if (!isLowContrastScreenPhoto(bgr)) {
        return bgr;
    }

    // LAB 空间分离亮度和色度：平滑 a/b 色度通道压制手机拍屏 RGB 彩色摩尔纹，
    // 对 L 亮度通道执行受限局部直方图均衡 (CLAHE) 并与原图柔和融合，避免过度锐化振铃伪影。
    cv::Mat lab;
    cv::cvtColor(bgr, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> channels;
    cv::split(lab, channels);
    if (channels.size() != 3) {
        return bgr;
    }

    cv::GaussianBlur(channels[1], channels[1], cv::Size(5, 5), 1.2);
    cv::GaussianBlur(channels[2], channels[2], cv::Size(5, 5), 1.2);

    auto clahe = cv::createCLAHE(2.2, cv::Size(8, 8));
    cv::Mat lEq;
    clahe->apply(channels[0], lEq);
    cv::addWeighted(lEq, 0.68, channels[0], 0.32, 0.0, channels[0]);

    cv::Mat enhancedLab, enhancedBgr;
    cv::merge(channels, enhancedLab);
    cv::cvtColor(enhancedLab, enhancedBgr, cv::COLOR_Lab2BGR);
    return enhancedBgr;
}

std::vector<DbDetectedQuad> DeepOcrEngine::postprocessDbMap(
    const cv::Mat& probMapInput,
    int origWidth,
    int origHeight,
    float ratioW,
    float ratioH,
    float binaryThresh,
    float boxScoreThresh,
    float unclipRatio) {
    std::vector<DbDetectedQuad> results;
    if (probMapInput.empty() || origWidth <= 0 || origHeight <= 0) {
        return results;
    }

    cv::Mat prob2d;
    if (probMapInput.dims == 4 && probMapInput.size[2] > 0 && probMapInput.size[3] > 0) {
        prob2d = cv::Mat(probMapInput.size[2], probMapInput.size[3], CV_32F,
                         const_cast<float*>(probMapInput.ptr<float>())).clone();
    } else if (probMapInput.dims == 2) {
        if (probMapInput.type() == CV_32FC1) {
            prob2d = probMapInput;
        } else {
            probMapInput.convertTo(prob2d, CV_32FC1);
        }
    } else {
        return results;
    }

    double minV = 0.0, maxV = 1.0;
    cv::minMaxLoc(prob2d, &minV, &maxV);
    if (minV < -0.02 || maxV > 1.02) {
        cv::Mat neg;
        cv::exp(-prob2d, neg);
        prob2d = 1.0f / (1.0f + neg);
    }

    cv::Mat bitMap;
    cv::threshold(prob2d, bitMap, binaryThresh, 255.0, cv::THRESH_BINARY);
    bitMap.convertTo(bitMap, CV_8UC1);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bitMap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) return results;

    if (contours.size() > static_cast<size_t>(DeepOcrConstants::kDetMaxCandidates)) {
        contours.resize(static_cast<size_t>(DeepOcrConstants::kDetMaxCandidates));
    }

    const float safeRatioW = (ratioW > 1e-5f) ? ratioW : 1.0f;
    const float safeRatioH = (ratioH > 1e-5f) ? ratioH : 1.0f;
    results.reserve(contours.size());

    for (const auto& contour : contours) {
        if (contour.size() < 4) continue;

        cv::RotatedRect minRect = cv::minAreaRect(contour);
        const float minSide = (std::min)(minRect.size.width, minRect.size.height);
        if (minSide < DeepOcrConstants::kDetMinBoxSidePx) continue;

        // 计算局部多边形 ROI 内的概率图均分
        cv::Rect polyRoi = cv::boundingRect(contour) & cv::Rect(0, 0, prob2d.cols, prob2d.rows);
        if (polyRoi.width <= 0 || polyRoi.height <= 0) continue;

        cv::Mat maskRoi = cv::Mat::zeros(polyRoi.size(), CV_8UC1);
        std::vector<cv::Point> shiftedContour;
        shiftedContour.reserve(contour.size());
        for (const auto& pt : contour) {
            shiftedContour.emplace_back(pt.x - polyRoi.x, pt.y - polyRoi.y);
        }
        const cv::Point* ptsPtr = shiftedContour.data();
        const int npts = static_cast<int>(shiftedContour.size());
        cv::fillPoly(maskRoi, &ptsPtr, &npts, 1, cv::Scalar(255));

        const float score = static_cast<float>(cv::mean(prob2d(polyRoi), maskRoi)[0]);
        if (score < boxScoreThresh) continue;

        // DBNet Unclip 外扩: distance = Area * unclipRatio / Perimeter
        const double polyArea = (std::max)(1.0, std::abs(cv::contourArea(contour)));
        const double polyPerim = (std::max)(1.0, cv::arcLength(contour, true));
        const float expandDist = (std::max)(
            minSide * 0.30f,
            static_cast<float>((polyArea * static_cast<double>(unclipRatio)) / polyPerim));

        minRect.size.width += 2.0f * expandDist;
        minRect.size.height += 2.0f * expandDist;

        cv::Point2f boxPts[4]{};
        minRect.points(boxPts);
        auto ordered = orderQuadPointsClockwise(boxPts);

        float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
        for (auto& pt : ordered) {
            pt.x = std::clamp(pt.x / safeRatioW, 0.0f, static_cast<float>(origWidth));
            pt.y = std::clamp(pt.y / safeRatioH, 0.0f, static_cast<float>(origHeight));
            minX = (std::min)(minX, pt.x);
            minY = (std::min)(minY, pt.y);
            maxX = (std::max)(maxX, pt.x);
            maxY = (std::max)(maxY, pt.y);
        }
        if ((maxX - minX) < 3.0f || (maxY - minY) < 3.0f) continue;

        results.push_back(DbDetectedQuad{ordered, score});
    }

    // 阅读序初排：按垂直位置为主，同一水平带按左边界升序
    std::sort(results.begin(), results.end(), [](const DbDetectedQuad& a, const DbDetectedQuad& b) {
        const float cyA = (a.corners[0].y + a.corners[2].y) * 0.5f;
        const float cyB = (b.corners[0].y + b.corners[2].y) * 0.5f;
        const float hA = (std::max)(4.0f, std::abs(a.corners[3].y - a.corners[0].y));
        const float hB = (std::max)(4.0f, std::abs(b.corners[3].y - b.corners[0].y));
        if (std::abs(cyA - cyB) <= (std::min)(hA, hB) * 0.5f) {
            return a.corners[0].x < b.corners[0].x;
        }
        return cyA < cyB;
    });

    return results;
}

cv::Mat DeepOcrEngine::warpPerspectiveQuad(
    const cv::Mat& image,
    const std::array<cv::Point2f, 4>& quad,
    cv::Mat* outInverseTransform,
    bool* outRotatedVertical) {
    if (outRotatedVertical) *outRotatedVertical = false;
    if (image.empty()) return {};

    const float wTop = std::hypot(quad[1].x - quad[0].x, quad[1].y - quad[0].y);
    const float wBot = std::hypot(quad[2].x - quad[3].x, quad[2].y - quad[3].y);
    const float hLeft = std::hypot(quad[3].x - quad[0].x, quad[3].y - quad[0].y);
    const float hRight = std::hypot(quad[2].x - quad[1].x, quad[2].y - quad[1].y);

    const int cropW = (std::max)(4, static_cast<int>(std::round((std::max)(wTop, wBot))));
    const int cropH = (std::max)(4, static_cast<int>(std::round((std::max)(hLeft, hRight))));

    const cv::Point2f srcPts[4] = {quad[0], quad[1], quad[2], quad[3]};
    const cv::Point2f dstPts[4] = {
        {0.0f, 0.0f},
        {static_cast<float>(cropW), 0.0f},
        {static_cast<float>(cropW), static_cast<float>(cropH)},
        {0.0f, static_cast<float>(cropH)}
    };

    if (outInverseTransform) {
        *outInverseTransform = cv::getPerspectiveTransform(dstPts, srcPts);
    }

    // 先截取紧致局部 ROI 再做局部透视变换，消除在 2K/4K 全屏大图上的跨页缓存未命中
    float minX = quad[0].x, maxX = quad[0].x, minY = quad[0].y, maxY = quad[0].y;
    for (size_t i = 1; i < 4; ++i) {
        minX = (std::min)(minX, quad[i].x);
        maxX = (std::max)(maxX, quad[i].x);
        minY = (std::min)(minY, quad[i].y);
        maxY = (std::max)(maxY, quad[i].y);
    }
    const int rx0 = std::clamp(static_cast<int>(std::floor(minX)) - 2, 0, image.cols - 1);
    const int ry0 = std::clamp(static_cast<int>(std::floor(minY)) - 2, 0, image.rows - 1);
    const int rx1 = std::clamp(static_cast<int>(std::ceil(maxX)) + 2, rx0 + 1, image.cols);
    const int ry1 = std::clamp(static_cast<int>(std::ceil(maxY)) + 2, ry0 + 1, image.rows);

    const cv::Point2f localSrcPts[4] = {
        {quad[0].x - static_cast<float>(rx0), quad[0].y - static_cast<float>(ry0)},
        {quad[1].x - static_cast<float>(rx0), quad[1].y - static_cast<float>(ry0)},
        {quad[2].x - static_cast<float>(rx0), quad[2].y - static_cast<float>(ry0)},
        {quad[3].x - static_cast<float>(rx0), quad[3].y - static_cast<float>(ry0)}
    };
    const cv::Mat localM = cv::getPerspectiveTransform(localSrcPts, dstPts);
    cv::Mat roiView = image(cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0));

    cv::Mat warped;
    cv::warpPerspective(roiView, warped, localM, cv::Size(cropW, cropH), cv::INTER_LINEAR, cv::BORDER_REPLICATE);

    if (cropH >= static_cast<int>(static_cast<float>(cropW) * 1.5f) && cropW >= 8) {
        cv::rotate(warped, warped, cv::ROTATE_90_COUNTERCLOCKWISE);
        if (outRotatedVertical) *outRotatedVertical = true;
    }
    return warped;
}

CtcDecodeOutput DeepOcrEngine::decodeCtcGreedy(
    const cv::Mat& timeByClassMat,
    const std::vector<std::string>& dictKeys,
    float validWidthRatio) {
    CtcDecodeOutput output{};
    if (timeByClassMat.empty() || timeByClassMat.rows <= 0 || timeByClassMat.cols <= 1) {
        return output;
    }

    cv::Mat mat32f;
    if (timeByClassMat.type() == CV_32FC1) {
        mat32f = timeByClassMat;
    } else {
        timeByClassMat.convertTo(mat32f, CV_32FC1);
    }

    const int T = mat32f.rows;
    const int C = mat32f.cols;

    struct RawSpan {
        int classIdx = 0;
        int startStep = 0;
        int endStep = 0;
        int peakStep = 0;
        float peakProb = 0.0f;
    };

    const float safeValidRatio = std::clamp(validWidthRatio, 0.05f, 1.0f);
    const float validSteps = (std::max)(1.0f, static_cast<float>(T) * safeValidRatio);
    const int scanSteps = (safeValidRatio < 0.99f)
        ? (std::min)(T, static_cast<int>(std::ceil(validSteps)) + 2)
        : T;

    std::vector<RawSpan> spans;
    spans.reserve(static_cast<size_t>((std::min)(scanSteps, 256)));

    int prevIdx = 0;
    for (int t = 0; t < scanSteps; ++t) {
        const float* row = mat32f.ptr<float>(t);
        int bestIdx = 0;
        float bestVal = row[0];
        for (int c = 1; c < C; ++c) {
            const float v = row[c];
            if (v > bestVal) {
                bestVal = v;
                bestIdx = c;
            }
        }

        if (bestIdx > 0) {
            float prob = bestVal;
            if (bestVal > 1.001f || row[0] < -0.001f) {
                double sumExp = 0.0;
                for (int c = 0; c < C; ++c) {
                    sumExp += std::exp(static_cast<double>(row[c] - bestVal));
                }
                prob = sumExp > 0.0 ? static_cast<float>(1.0 / sumExp) : 0.0f;
            }
            prob = std::clamp(prob, 0.0f, 1.0f);

            if (bestIdx == prevIdx && !spans.empty()) {
                auto& back = spans.back();
                back.endStep = t;
                if (prob > back.peakProb) {
                    back.peakProb = prob;
                    back.peakStep = t;
                }
            } else {
                spans.push_back(RawSpan{bestIdx, t, t, t, prob});
            }
        }
        prevIdx = bestIdx;
    }

    if (spans.empty()) {
        return output;
    }

    std::vector<CtcDecodedChar> rawChars;
    rawChars.reserve(spans.size());
    float probSum = 0.0f;

    for (const auto& sp : spans) {
        std::string tokenStr;
        if (sp.classIdx >= 1 && static_cast<size_t>(sp.classIdx) <= dictKeys.size()) {
            tokenStr = dictKeys[static_cast<size_t>(sp.classIdx - 1)];
        } else if (static_cast<size_t>(sp.classIdx) == dictKeys.size() + 1) {
            tokenStr = " ";
        }
        if (tokenStr.empty()) continue;

        CtcDecodedChar dc;
        dc.utf8Char = std::move(tokenStr);
        dc.confidence = sp.peakProb;
        dc.startStep = sp.startStep;
        dc.peakStep = sp.peakStep;
        dc.endStep = sp.endStep;
        rawChars.push_back(std::move(dc));
        probSum += sp.peakProb;
    }

    if (rawChars.empty()) {
        return output;
    }

    // 计算每个解码字符在有效裁剪宽度上的精确归一化区间 [normLeft, normRight]
    const size_t N = rawChars.size();
    std::vector<float> centers(N, 0.0f);
    for (size_t i = 0; i < N; ++i) {
        const float midStep = 0.5f * static_cast<float>(rawChars[i].startStep + rawChars[i].endStep) + 0.5f;
        centers[i] = std::clamp(midStep / validSteps, 0.0f, 1.0f);
    }

    for (size_t i = 0; i < N; ++i) {
        const float spanHalf = static_cast<float>(rawChars[i].endStep - rawChars[i].startStep + 1) / (2.0f * validSteps);
        float neighborHalf = 0.45f / static_cast<float>(N);
        if (N > 1) {
            if (i == 0) neighborHalf = (centers[1] - centers[0]) * 0.5f;
            else if (i + 1 == N) neighborHalf = (centers[N - 1] - centers[N - 2]) * 0.5f;
            else neighborHalf = ((centers[i] - centers[i - 1]) + (centers[i + 1] - centers[i])) * 0.25f;
        }
        const float halfW = (std::max)(spanHalf, (std::max)(0.005f, neighborHalf));

        float left = (std::max)(0.0f, centers[i] - halfW);
        float right = (std::min)(1.0f, centers[i] + halfW);
        if (i > 0) {
            const float midPrev = 0.5f * (centers[i - 1] + centers[i]);
            left = (std::max)(left, midPrev);
        }
        if (i + 1 < N) {
            const float midNext = 0.5f * (centers[i] + centers[i + 1]);
            right = (std::min)(right, midNext);
        }
        if (right <= left) {
            right = (std::min)(1.0f, left + (1.0f / static_cast<float>(N + 1)));
            if (right <= left) left = (std::max)(0.0f, right - 0.01f);
        }
        rawChars[i].normLeft = left;
        rawChars[i].normRight = right;
    }

    // 若某字典项包含多个 Unicode 码点，按码点数线性细分，严格保证 1 个码点 = 1 个 CtcDecodedChar
    output.chars.reserve(rawChars.size());
    for (const auto& rc : rawChars) {
        output.text += rc.utf8Char;
        const auto cps = OcrEngine::splitUtf8Codepoints(rc.utf8Char);
        if (cps.size() <= 1) {
            output.chars.push_back(rc);
        } else {
            const float totalSpan = rc.normRight - rc.normLeft;
            for (size_t k = 0; k < cps.size(); ++k) {
                CtcDecodedChar sub = rc;
                sub.utf8Char = cps[k];
                sub.normLeft = rc.normLeft + totalSpan * (static_cast<float>(k) / static_cast<float>(cps.size()));
                sub.normRight = rc.normLeft + totalSpan * (static_cast<float>(k + 1) / static_cast<float>(cps.size()));
                output.chars.push_back(std::move(sub));
            }
        }
    }

    output.confidence = probSum / static_cast<float>(rawChars.size());
    return output;
}

std::vector<cv::Rect> DeepOcrEngine::projectCharSpansToImage(
    const std::string& lineText,
    const std::vector<CtcDecodedChar>& decodedChars,
    int cropWidth,
    int cropHeight,
    bool rotated180,
    bool rotatedVertical,
    const cv::Mat& inversePerspective,
    const cv::Rect& fallbackLineBox,
    int maxImageWidth,
    int maxImageHeight) {
    const size_t expectedCodepoints = OcrEngine::countUtf8Codepoints(lineText);
    if (expectedCodepoints == 0) return {};

    if (decodedChars.size() != expectedCodepoints || cropWidth <= 0 || cropHeight <= 0) {
        return OcrEngine::buildProportionalCharBoxes(lineText, fallbackLineBox);
    }

    // 轴对齐水平框零矩阵快速精确映射：直接按 CTC 时间步区间投影至原图物理像素包围盒
    if (inversePerspective.empty() || inversePerspective.rows != 3 || inversePerspective.cols != 3) {
        if (!rotatedVertical && fallbackLineBox.width > 0 && fallbackLineBox.height > 0) {
            std::vector<cv::Rect> fastBoxes;
            fastBoxes.reserve(decodedChars.size());
            const float baseLeft = static_cast<float>(fallbackLineBox.x);
            const float spanW = static_cast<float>(fallbackLineBox.width);
            for (const auto& dc : decodedChars) {
                float u0 = std::clamp(dc.normLeft, 0.0f, 1.0f);
                float u1 = std::clamp(dc.normRight, u0, 1.0f);
                if (rotated180) {
                    const float nu0 = 1.0f - u1;
                    const float nu1 = 1.0f - u0;
                    u0 = nu0;
                    u1 = nu1;
                }
                int rx1 = static_cast<int>(std::floor(baseLeft + u0 * spanW));
                int rx2 = static_cast<int>(std::ceil(baseLeft + u1 * spanW));
                int ry1 = fallbackLineBox.y;
                int ry2 = fallbackLineBox.y + fallbackLineBox.height;
                if (maxImageWidth > 0 && maxImageHeight > 0) {
                    rx1 = std::clamp(rx1, 0, maxImageWidth - 1);
                    ry1 = std::clamp(ry1, 0, maxImageHeight - 1);
                    rx2 = std::clamp(rx2, rx1 + 1, maxImageWidth);
                    ry2 = std::clamp(ry2, ry1 + 1, maxImageHeight);
                } else {
                    rx2 = (std::max)(rx1 + 1, rx2);
                    ry2 = (std::max)(ry1 + 1, ry2);
                }
                fastBoxes.emplace_back(rx1, ry1, rx2 - rx1, ry2 - ry1);
            }
            return fastBoxes;
        }
        return OcrEngine::buildProportionalCharBoxes(lineText, fallbackLineBox);
    }

    std::vector<cv::Rect> charBoxes;
    charBoxes.reserve(decodedChars.size());

    // 注意: 若竖排框经历过逆时针 90° 旋转，原正交拉平框尺寸为 (origCropW = cropHeight, origCropH = cropWidth)
    const float unrotW = rotatedVertical ? static_cast<float>(cropHeight) : static_cast<float>(cropWidth);
    const float unrotH = rotatedVertical ? static_cast<float>(cropWidth) : static_cast<float>(cropHeight);

    for (const auto& dc : decodedChars) {
        float u0 = std::clamp(dc.normLeft, 0.0f, 1.0f);
        float u1 = std::clamp(dc.normRight, u0, 1.0f);
        if (rotated180) {
            const float nu0 = 1.0f - u1;
            const float nu1 = 1.0f - u0;
            u0 = nu0;
            u1 = nu1;
        }

        std::vector<cv::Point2f> localQuad(4);
        if (!rotatedVertical) {
            const float x0 = u0 * unrotW;
            const float x1 = (std::max)(x0 + 1.0f, u1 * unrotW);
            localQuad[0] = {x0, 0.0f};
            localQuad[1] = {x1, 0.0f};
            localQuad[2] = {x1, unrotH};
            localQuad[3] = {x0, unrotH};
        } else {
            // 竖排经过 90° CCW，横轴 u 对应旋转前从底到顶或从顶到底的纵轴区间
            const float y0 = (1.0f - u1) * unrotH;
            const float y1 = (std::max)(y0 + 1.0f, (1.0f - u0) * unrotH);
            localQuad[0] = {0.0f, y0};
            localQuad[1] = {unrotW, y0};
            localQuad[2] = {unrotW, y1};
            localQuad[3] = {0.0f, y1};
        }

        std::vector<cv::Point2f> imgQuad(4);
        cv::perspectiveTransform(localQuad, imgQuad, inversePerspective);

        float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
        for (const auto& p : imgQuad) {
            minX = (std::min)(minX, p.x);
            minY = (std::min)(minY, p.y);
            maxX = (std::max)(maxX, p.x);
            maxY = (std::max)(maxY, p.y);
        }

        int rx1 = static_cast<int>(std::floor(minX));
        int ry1 = static_cast<int>(std::floor(minY));
        int rx2 = static_cast<int>(std::ceil(maxX));
        int ry2 = static_cast<int>(std::ceil(maxY));

        if (maxImageWidth > 0 && maxImageHeight > 0) {
            rx1 = std::clamp(rx1, 0, maxImageWidth - 1);
            ry1 = std::clamp(ry1, 0, maxImageHeight - 1);
            rx2 = std::clamp(rx2, rx1 + 1, maxImageWidth);
            ry2 = std::clamp(ry2, ry1 + 1, maxImageHeight);
        } else {
            rx2 = (std::max)(rx1 + 1, rx2);
            ry2 = (std::max)(ry1 + 1, ry2);
        }

        charBoxes.emplace_back(rx1, ry1, rx2 - rx1, ry2 - ry1);
    }

    return charBoxes;
}


namespace {

bool isChevronCandidateCodepoint(std::string_view cp) noexcept {
    return cp == "v" || cp == "V" || cp == "~" || cp == "^" || cp == ">" ||
           cp == "\xE2\x88\x9A" || // √
           cp == "\xCB\x87" ||     // ˇ
           cp == "\xE2\x88\xA8" || // ∨
           cp == "\xE2\x8C\x84";   // ⌄
}


bool isAsciiKeyCodepoint(std::string_view cp, bool isFirst) noexcept {
    if (cp.size() != 1) return false;
    const unsigned char c = static_cast<unsigned char>(cp[0]);
    if (isFirst) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '@' || c == '$';
    }
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-' || c == ' ';
}

bool isSmartOrAsciiQuote(std::string_view cp) noexcept {
    return cp == "\"" || cp == "\xE2\x80\x9C" || cp == "\xE2\x80\x9D"; // ", “, ”
}

bool isColonOrFullWidthColon(std::string_view cp) noexcept {
    return cp == ":" || cp == "\xEF\xBC\x9A"; // :, ：
}

// 若从 openQuoteIdx 开始是合法的 `"<ascii_key>"` + 可选空白 + `:`/`：`，返回冒号下标，否则返回 npos
size_t matchAsciiJsonKeyColon(const std::vector<std::string>& cps, size_t openQuoteIdx) noexcept {
    const size_t n = cps.size();
    if (openQuoteIdx + 3 >= n || !isSmartOrAsciiQuote(cps[openQuoteIdx])) {
        return std::string::npos;
    }
    size_t i = openQuoteIdx + 1;
    if (!isAsciiKeyCodepoint(cps[i], true)) {
        return std::string::npos;
    }
    size_t keyLen = 0;
    while (i < n && !isSmartOrAsciiQuote(cps[i])) {
        if (!isAsciiKeyCodepoint(cps[i], false) || ++keyLen > 48) {
            return std::string::npos;
        }
        ++i;
    }
    if (i >= n || keyLen == 0 || !isSmartOrAsciiQuote(cps[i])) {
        return std::string::npos;
    }
    ++i; // skip closing quote
    while (i < n && cps[i] == " ") {
        ++i;
    }
    if (i < n && isColonOrFullWidthColon(cps[i])) {
        return i;
    }
    return std::string::npos;
}


} // namespace

bool DeepOcrEngine::isUiDropdownChevronPatch(
    const cv::Mat& bgr,
    const cv::Rect& candidateRect,
    const cv::Rect* prevTextInkRect) {
    if (bgr.empty() || bgr.type() != CV_8UC3) return false;
    const cv::Rect imgBounds(0, 0, bgr.cols, bgr.rows);
    const cv::Rect roi = candidateRect & imgBounds;
    if (roi.width < 4 || roi.height < 3) return false;

    const cv::Mat patch = bgr(roi);
    const cv::Vec3b bg = estimateBorderBackgroundColor(patch);

    std::vector<float> diff(static_cast<size_t>(roi.height) * static_cast<size_t>(roi.width), 0.0f);
    float peak = 0.0f;
    for (int y = 0; y < roi.height; ++y) {
        const auto* row = patch.ptr<cv::Vec3b>(y);
        for (int x = 0; x < roi.width; ++x) {
            const float d = static_cast<float>((std::max)({
                std::abs(static_cast<int>(row[x][0]) - static_cast<int>(bg[0])),
                std::abs(static_cast<int>(row[x][1]) - static_cast<int>(bg[1])),
                std::abs(static_cast<int>(row[x][2]) - static_cast<int>(bg[2])),
            }));
            diff[static_cast<size_t>(y) * static_cast<size_t>(roi.width) + static_cast<size_t>(x)] = d;
            if (d > peak) peak = d;
        }
    }
    if (peak < 22.0f) return false;

    const float thr = (std::max)(15.0f, peak * 0.34f);
    int ix0 = roi.width, iy0 = roi.height, ix1 = -1, iy1 = -1;
    int fgCount = 0;
    for (int y = 0; y < roi.height; ++y) {
        for (int x = 0; x < roi.width; ++x) {
            if (diff[static_cast<size_t>(y) * static_cast<size_t>(roi.width) + static_cast<size_t>(x)] >= thr) {
                ix0 = (std::min)(ix0, x);
                iy0 = (std::min)(iy0, y);
                ix1 = (std::max)(ix1, x + 1);
                iy1 = (std::max)(iy1, y + 1);
                ++fgCount;
            }
        }
    }
    if (fgCount < 4 || ix1 <= ix0 || iy1 <= iy0) return false;

    const int cw = ix1 - ix0;
    const int ch = iy1 - iy0;
    if (ch < 3 || ch > 12 || cw < 5 || static_cast<float>(cw) < static_cast<float>(ch) * 1.18f) {
        return false;
    }

    if (prevTextInkRect && prevTextInkRect->height > 0) {
        const float prevH = static_cast<float>(prevTextInkRect->height);
        if (static_cast<float>(ch) > prevH * 0.68f) {
            return false;
        }
        const float chevCy = static_cast<float>(roi.y) + 0.5f * static_cast<float>(iy0 + iy1);
        const float prevCy = static_cast<float>(prevTextInkRect->y) + 0.5f * prevH;
        if (std::abs(chevCy - prevCy) > prevH * 0.32f) {
            return false;
        }
        const int chevBot = roi.y + iy1;
        const int prevBot = prevTextInkRect->y + prevTextInkRect->height;
        if (chevBot >= prevBot && static_cast<float>(ch) > prevH * 0.55f) {
            return false;
        }
    }

    const float invPeak = 1.0f / (std::max)(1.0f, peak);
    auto normAt = [&](int lx, int ly) noexcept -> float {
        return diff[static_cast<size_t>(iy0 + ly) * static_cast<size_t>(roi.width) + static_cast<size_t>(ix0 + lx)] * invPeak;
    };

    float symNum = 0.0f;
    float symDen = 1e-6f;
    for (int ly = 0; ly < ch; ++ly) {
        for (int lx = 0; lx < cw; ++lx) {
            const float a = normAt(lx, ly);
            const float b = normAt(cw - 1 - lx, ly);
            symNum += (std::min)(a, b);
            symDen += (std::max)(a, b);
        }
    }
    if ((symNum / symDen) < 0.68f) return false;

    auto regionStats = [&](int rx0, int ry0, int rx1, int ry1, float& outMean, float& outMax) {
        rx0 = std::clamp(rx0, 0, cw);
        rx1 = std::clamp(rx1, rx0, cw);
        ry0 = std::clamp(ry0, 0, ch);
        ry1 = std::clamp(ry1, ry0, ch);
        outMean = 0.0f;
        outMax = 0.0f;
        const int cnt = (rx1 - rx0) * (ry1 - ry0);
        if (cnt <= 0) return;
        float sum = 0.0f;
        for (int ly = ry0; ly < ry1; ++ly) {
            for (int lx = rx0; lx < rx1; ++lx) {
                const float v = normAt(lx, ly);
                sum += v;
                if (v > outMax) outMax = v;
            }
        }
        outMean = sum / static_cast<float>(cnt);
    };

    const int halfH = (std::max)(1, ch / 2);
    const int thirdW = (std::max)(1, cw / 3);
    const int quarterW = (std::max)(1, cw / 4);

    float ucMean = 0.0f, ucMax = 0.0f;
    float bcMean = 0.0f, bcMax = 0.0f;
    float ulMean = 0.0f, ulMax = 0.0f;
    float urMean = 0.0f, urMax = 0.0f;
    float blMean = 0.0f, blMax = 0.0f;
    float brMean = 0.0f, brMax = 0.0f;

    regionStats(thirdW, 0, cw - thirdW, halfH, ucMean, ucMax);
    regionStats(thirdW, halfH, cw - thirdW, ch, bcMean, bcMax);
    regionStats(0, 0, thirdW, halfH, ulMean, ulMax);
    regionStats(cw - thirdW, 0, cw, halfH, urMean, urMax);
    regionStats(0, ch - halfH, quarterW, ch, blMean, blMax);
    regionStats(cw - quarterW, ch - halfH, cw, ch, brMean, brMax);

    const bool isDownChevron =
        ucMean <= 0.30f && blMean <= 0.26f && brMean <= 0.26f &&
        bcMax >= 0.45f && ulMax >= 0.32f && urMax >= 0.32f;

    float tlCornerMean = 0.0f, tlCornerMax = 0.0f;
    float trCornerMean = 0.0f, trCornerMax = 0.0f;
    float blArmMean = 0.0f, blArmMax = 0.0f;
    float brArmMean = 0.0f, brArmMax = 0.0f;
    regionStats(0, 0, quarterW, halfH, tlCornerMean, tlCornerMax);
    regionStats(cw - quarterW, 0, cw, halfH, trCornerMean, trCornerMax);
    regionStats(0, halfH, thirdW, ch, blArmMean, blArmMax);
    regionStats(cw - thirdW, halfH, cw, ch, brArmMean, brArmMax);

    const bool isUpChevron =
        bcMean <= 0.30f && tlCornerMean <= 0.26f && trCornerMean <= 0.26f &&
        ucMax >= 0.45f && blArmMax >= 0.32f && brArmMax >= 0.32f;

    return isDownChevron || isUpChevron;
}

void DeepOcrEngine::coalesceSameLineTinyFragments(
    std::vector<DbDetectedQuad>& quads,
    const cv::Mat& bgr) {
    if (quads.size() < 2) return;

    struct QuadEntry {
        DbDetectedQuad quad;
        cv::Rect box;
        bool axisAligned = false;
    };
    std::vector<QuadEntry> entries;
    entries.reserve(quads.size());
    for (const auto& q : quads) {
        cv::Rect r;
        const bool axis = isAxisAlignedHorizontalQuad(q, &r);
        entries.push_back(QuadEntry{q, r, axis});
    }

    std::sort(entries.begin(), entries.end(), [](const QuadEntry& a, const QuadEntry& b) {
        if (a.box.y != b.box.y) return a.box.y < b.box.y;
        return a.box.x < b.box.x;
    });

    bool mergedAny = true;
    while (mergedAny) {
        mergedAny = false;
        std::vector<bool> consumed(entries.size(), false);
        std::vector<QuadEntry> nextEntries;
        nextEntries.reserve(entries.size());

        for (size_t i = 0; i < entries.size(); ++i) {
            if (consumed[i]) continue;
            QuadEntry cur = entries[i];
            consumed[i] = true;

            if (cur.axisAligned) {
                bool absorbed = true;
                while (absorbed) {
                    absorbed = false;
                    int bestJ = -1;
                    int bestGap = INT_MAX;
                    const int h = (std::max)(1, cur.box.height);
                    const int maxFragW = (std::max)(32, static_cast<int>(std::round(static_cast<float>(h) * 2.6f)));
                    const int maxGap = (std::max)(10, static_cast<int>(std::round(static_cast<float>(h) * 1.15f)));

                    for (size_t j = 0; j < entries.size(); ++j) {
                        if (consumed[j] || !entries[j].axisAligned) continue;
                        const cv::Rect& fb = entries[j].box;
                        const int jh = (std::max)(1, fb.height);
                        const int jw = fb.width;
                        const int gap = fb.x - (cur.box.x + cur.box.width);
                        const int vOv = (std::min)(cur.box.y + cur.box.height, fb.y + fb.height) -
                                        (std::max)(cur.box.y, fb.y);
                        if (vOv < static_cast<int>(static_cast<float>((std::min)(h, jh)) * 0.55f)) continue;
                        if (fb.y < cur.box.y - 3 || (fb.y + fb.height) > (cur.box.y + cur.box.height) + 3) continue;
                        if (static_cast<float>(jh) > static_cast<float>(h) * 0.75f || jw > maxFragW) continue;
                        if (gap < -4 || gap > maxGap || gap >= bestGap) continue;

                        if (!bgr.empty()) {
                            const cv::Rect paddedFrag(fb.x - 2, fb.y - 2, fb.width + 4, fb.height + 4);
                            if (isUiDropdownChevronPatch(bgr, paddedFrag, &cur.box)) {
                                continue;
                            }
                        }
                        bestGap = gap;
                        bestJ = static_cast<int>(j);
                    }

                    if (bestJ >= 0) {
                        const auto& fb = entries[static_cast<size_t>(bestJ)].box;
                        const int nx1 = (std::min)(cur.box.x, fb.x);
                        const int ny1 = (std::min)(cur.box.y, fb.y);
                        const int nx2 = (std::max)(cur.box.x + cur.box.width, fb.x + fb.width);
                        const int ny2 = (std::max)(cur.box.y + cur.box.height, fb.y + fb.height);
                        cur.box = cv::Rect(nx1, ny1, nx2 - nx1, ny2 - ny1);
                        cur.quad.corners = {
                            cv::Point2f(static_cast<float>(nx1), static_cast<float>(ny1)),
                            cv::Point2f(static_cast<float>(nx2), static_cast<float>(ny1)),
                            cv::Point2f(static_cast<float>(nx2), static_cast<float>(ny2)),
                            cv::Point2f(static_cast<float>(nx1), static_cast<float>(ny2)),
                        };
                        cur.quad.score = (std::max)(cur.quad.score, entries[static_cast<size_t>(bestJ)].quad.score);
                        consumed[static_cast<size_t>(bestJ)] = true;
                        absorbed = true;
                        mergedAny = true;
                    }
                }
            }
            nextEntries.push_back(std::move(cur));
        }
        entries = std::move(nextEntries);
    }

    quads.clear();
    quads.reserve(entries.size());
    for (auto& e : entries) {
        quads.push_back(std::move(e.quad));
    }
    std::sort(quads.begin(), quads.end(), [](const DbDetectedQuad& a, const DbDetectedQuad& b) {
        const float cyA = (a.corners[0].y + a.corners[2].y) * 0.5f;
        const float cyB = (b.corners[0].y + b.corners[2].y) * 0.5f;
        const float hA = (std::max)(4.0f, std::abs(a.corners[3].y - a.corners[0].y));
        const float hB = (std::max)(4.0f, std::abs(b.corners[3].y - b.corners[0].y));
        if (std::abs(cyA - cyB) <= (std::min)(hA, hB) * 0.5f) {
            return a.corners[0].x < b.corners[0].x;
        }
        return cyA < cyB;
    });
}

cv::Rect DeepOcrEngine::refineAxisAlignedLineBox(
    const cv::Mat& bgr,
    const cv::Rect& box,
    const std::vector<cv::Rect>& allBoxes,
    size_t selfIdx,
    cv::Vec3b* outBgColor,
    bool* outUniformBg) {
    if (outBgColor) *outBgColor = cv::Vec3b(255, 255, 255);
    if (outUniformBg) *outUniformBg = false;
    if (bgr.empty() || bgr.type() != CV_8UC3) return box;

    const cv::Rect imgBounds(0, 0, bgr.cols, bgr.rows);
    const cv::Rect safeBox = box & imgBounds;
    if (safeBox.width <= 0 || safeBox.height <= 0) return safeBox;

    bool uniformBg = false;
    const cv::Vec3b bg = estimateBorderBackgroundColor(bgr(safeBox), &uniformBg);
    if (outBgColor) *outBgColor = bg;
    if (outUniformBg) *outUniformBg = uniformBg;
    if (!uniformBg) return safeBox;

    const int x1 = safeBox.x;
    const int y1 = safeBox.y;
    const int x2 = safeBox.x + safeBox.width;
    const int y2 = safeBox.y + safeBox.height;
    const int h = safeBox.height;

    int leftLimit = (std::max)(0, x1 - (std::max)(5, static_cast<int>(std::round(static_cast<float>(h) * 0.55f))));
    int rightLimit = (std::min)(bgr.cols, x2 + (std::max)(6, static_cast<int>(std::round(static_cast<float>(h) * 0.60f))));

    for (size_t j = 0; j < allBoxes.size(); ++j) {
        if (j == selfIdx) continue;
        const cv::Rect& ob = allBoxes[j];
        if (ob.width <= 0 || ob.height <= 0) continue;
        const int vOv = (std::min)(y2, ob.y + ob.height) - (std::max)(y1, ob.y);
        if (vOv > static_cast<int>(static_cast<float>((std::min)(h, ob.height)) * 0.30f)) {
            if (ob.x + ob.width <= x1 + 2) {
                leftLimit = (std::max)(leftLimit, ob.x + ob.width + 1);
            }
            if (ob.x >= x2 - 2) {
                rightLimit = (std::min)(rightLimit, ob.x - 1);
            }
        }
    }

    auto colHasInk = [&](int cx) noexcept -> bool {
        if (cx < 0 || cx >= bgr.cols) return false;
        for (int ry = y1; ry < y2; ++ry) {
            const auto& p = bgr.at<cv::Vec3b>(ry, cx);
            const int d = (std::max)({
                std::abs(static_cast<int>(p[0]) - static_cast<int>(bg[0])),
                std::abs(static_cast<int>(p[1]) - static_cast<int>(bg[1])),
                std::abs(static_cast<int>(p[2]) - static_cast<int>(bg[2])),
            });
            if (d > 36) return true;
        }
        return false;
    };

    const int maxPunctW = (std::max)(7, static_cast<int>(std::round(static_cast<float>(h) * 0.55f)));

    // 右边界挽救（覆盖贴边逗号 `,`、引号 `"`、分号 `;`、右花括号 `}` 等）
    int newX2 = x2;
    if (newX2 < rightLimit) {
        int foundEnd = -1;
        int gapRun = 0;
        int inkRun = 0;
        for (int cx = (std::max)(x1, x2 - 1); cx < rightLimit; ++cx) {
            if (colHasInk(cx)) {
                if (gapRun <= 3) {
                    ++inkRun;
                    if (inkRun <= maxPunctW) {
                        foundEnd = cx + 1;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            } else {
                if (foundEnd >= 0) break;
                if (++gapRun > 3) break;
            }
        }
        if (foundEnd > newX2) {
            newX2 = (std::min)(rightLimit, foundEnd + 2);
        } else if (colHasInk(x2 - 1)) {
            newX2 = (std::min)(rightLimit, x2 + 2);
        }
    }

    // 左边界挽救（覆盖贴边 `{`、`[`、`"` 或首字符左缘裁切）
    int newX1 = x1;
    if (newX1 > leftLimit) {
        int foundStart = -1;
        int gapRun = 0;
        int inkRun = 0;
        for (int cx = (std::min)(bgr.cols - 1, x1); cx >= leftLimit; --cx) {
            if (colHasInk(cx)) {
                if (gapRun <= 3) {
                    ++inkRun;
                    if (inkRun <= maxPunctW) {
                        foundStart = cx;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            } else {
                if (foundStart >= 0) break;
                if (++gapRun > 3) break;
            }
        }
        if (foundStart >= 0 && foundStart < newX1) {
            newX1 = (std::max)(leftLimit, foundStart - 2);
        } else if (colHasInk(x1)) {
            newX1 = (std::max)(leftLimit, x1 - 2);
        }
    }

    return cv::Rect(newX1, y1, (std::max)(1, newX2 - newX1), h);
}

void DeepOcrEngine::stripUiDropdownChevrons(
    const cv::Mat& bgr,
    std::vector<OcrResult>& blocks) {
    if (bgr.empty() || bgr.type() != CV_8UC3 || blocks.empty()) return;
    const cv::Rect imgBounds(0, 0, bgr.cols, bgr.rows);

    std::vector<OcrResult> kept;
    kept.reserve(blocks.size());

    for (auto& item : blocks) {
        auto cps = OcrEngine::splitUtf8Codepoints(item.text);
        if (cps.empty()) continue;

        // 1. 独立单字符下拉箭头块（如按钮旁的独立 `v`/`√`）
        if (cps.size() == 1 && isChevronCandidateCodepoint(cps[0])) {
            const cv::Rect padded(item.box.x - 2, item.box.y - 2, item.box.width + 4, item.box.height + 4);
            if (isUiDropdownChevronPatch(bgr, padded, nullptr)) {
                continue;
            }
            kept.push_back(std::move(item));
            continue;
        }

        // 2. 按钮标签尾部伴随的下拉箭头（如 `GET v`、`Body~`、`utf8 √`、`保存 v`）
        int lastIdx = static_cast<int>(cps.size()) - 1;
        while (lastIdx >= 0 && cps[static_cast<size_t>(lastIdx)] == " ") {
            --lastIdx;
        }
        if (lastIdx >= 1 &&
            isChevronCandidateCodepoint(cps[static_cast<size_t>(lastIdx)]) &&
            item.charBoxes.size() == cps.size()) {
            int prevIdx = lastIdx - 1;
            while (prevIdx >= 0 && cps[static_cast<size_t>(prevIdx)] == " ") {
                --prevIdx;
            }
            if (prevIdx >= 0) {
                const cv::Rect& prevBox = item.charBoxes[static_cast<size_t>(prevIdx)];
                const cv::Rect& lastBox = item.charBoxes[static_cast<size_t>(lastIdx)];
                const int prevEndX = (std::min)(bgr.cols, (std::max)(item.box.x + 2, prevBox.x + prevBox.width));
                const cv::Rect textSpanRoi = cv::Rect(item.box.x, item.box.y, prevEndX - item.box.x, item.box.height) & imgBounds;
                if (textSpanRoi.width >= 2 && textSpanRoi.height >= 3) {
                    const cv::Mat textPatch = bgr(textSpanRoi);
                    const cv::Vec3b bg = estimateBorderBackgroundColor(textPatch);
                    int pMinY = textSpanRoi.height, pMaxY = -1;
                    float pPeak = 0.0f;
                    for (int y = 0; y < textSpanRoi.height; ++y) {
                        const auto* row = textPatch.ptr<cv::Vec3b>(y);
                        for (int x = 0; x < textSpanRoi.width; ++x) {
                            const float d = static_cast<float>((std::max)({
                                std::abs(static_cast<int>(row[x][0]) - static_cast<int>(bg[0])),
                                std::abs(static_cast<int>(row[x][1]) - static_cast<int>(bg[1])),
                                std::abs(static_cast<int>(row[x][2]) - static_cast<int>(bg[2])),
                            }));
                            if (d > pPeak) pPeak = d;
                        }
                    }
                    const float pThr = (std::max)(18.0f, pPeak * 0.35f);
                    for (int y = 0; y < textSpanRoi.height; ++y) {
                        const auto* row = textPatch.ptr<cv::Vec3b>(y);
                        for (int x = 0; x < textSpanRoi.width; ++x) {
                            const float d = static_cast<float>((std::max)({
                                std::abs(static_cast<int>(row[x][0]) - static_cast<int>(bg[0])),
                                std::abs(static_cast<int>(row[x][1]) - static_cast<int>(bg[1])),
                                std::abs(static_cast<int>(row[x][2]) - static_cast<int>(bg[2])),
                            }));
                            if (d >= pThr) {
                                pMinY = (std::min)(pMinY, y);
                                pMaxY = (std::max)(pMaxY, y + 1);
                            }
                        }
                    }

                    if (pMaxY > pMinY) {
                        const cv::Rect prevInkRect(textSpanRoi.x, textSpanRoi.y + pMinY, textSpanRoi.width, pMaxY - pMinY);
                        const int rightBound = (std::min)(bgr.cols, (std::max)(item.box.x + item.box.width, lastBox.x + lastBox.width) + 2);
                        // 从前字右缘向右跨过前字微悬垂，精确定位将文字与下拉箭头隔开的干净空白列谷底
                        int valleyStart = -1;
                        int valleyEnd = -1;
                        const int scanStart = (std::max)(item.box.x, (std::min)(prevEndX, lastBox.x) - 2);
                        for (int cx = scanStart; cx < rightBound; ++cx) {
                            int colMaxDiff = 0;
                            for (int ry = textSpanRoi.y; ry < textSpanRoi.y + textSpanRoi.height; ++ry) {
                                const auto& p = bgr.at<cv::Vec3b>(ry, cx);
                                const int d = (std::max)({
                                    std::abs(static_cast<int>(p[0]) - static_cast<int>(bg[0])),
                                    std::abs(static_cast<int>(p[1]) - static_cast<int>(bg[1])),
                                    std::abs(static_cast<int>(p[2]) - static_cast<int>(bg[2])),
                                });
                                if (d > colMaxDiff) colMaxDiff = d;
                            }
                            if (colMaxDiff < 24) {
                                if (valleyStart < 0) valleyStart = cx;
                                valleyEnd = cx + 1;
                            } else if (valleyStart >= 0) {
                                break;
                            }
                        }

                        if (valleyStart >= 0 && (valleyEnd - valleyStart) >= 2 && valleyEnd < rightBound) {
                            const cv::Rect chevCandidate(
                                valleyEnd - 1,
                                item.box.y,
                                rightBound - (valleyEnd - 1),
                                item.box.height);
                            if (isUiDropdownChevronPatch(bgr, chevCandidate, &prevInkRect)) {
                                const size_t keepCount = static_cast<size_t>(prevIdx) + 1;
                                cps.resize(keepCount);
                                item.charBoxes.resize(keepCount);
                                std::string trimmed;
                                for (const auto& cp : cps) trimmed += cp;
                                item.text = std::move(trimmed);
                                const int tightenedRight = (std::min)(
                                    item.box.x + item.box.width,
                                    (std::max)(item.box.x + 2, prevBox.x + prevBox.width + 2));
                                item.box.width = (std::max)(1, tightenedRight - item.box.x);
                            }
                        }
                    }
                }
            }
        }

        kept.push_back(std::move(item));
    }

    blocks = std::move(kept);
}

void DeepOcrEngine::rescueMissedUnderscores(
    const cv::Mat& bgr,
    OcrResult& item) {
    if (bgr.empty() || bgr.type() != CV_8UC3 || item.text.size() < 3) return;
    if (item.text.find(' ') == std::string::npos) return;

    auto cps = OcrEngine::splitUtf8Codepoints(item.text);
    if (cps.size() < 3 || item.charBoxes.size() != cps.size()) return;

    const cv::Rect imgBounds(0, 0, bgr.cols, bgr.rows);
    const cv::Rect lineSafe = item.box & imgBounds;
    if (lineSafe.width < 8 || lineSafe.height < 8) return;

    auto isAsciiAlnum = [](const std::string& cp) noexcept -> bool {
        if (cp.size() != 1) return false;
        const unsigned char c = static_cast<unsigned char>(cp[0]);
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };

    const int sy1 = lineSafe.y;
    const int sy2 = (std::min)(bgr.rows, lineSafe.y + lineSafe.height + 1);
    const int gh = sy2 - sy1;
    if (gh < 8) return;

    const int u0 = std::clamp(static_cast<int>(std::lround(static_cast<float>(gh) * 0.12f)), 0, gh - 2);
    const int u1 = std::clamp(static_cast<int>(std::lround(static_cast<float>(gh) * 0.72f)), u0 + 1, gh - 1);
    const int b0 = std::clamp(static_cast<int>(std::lround(static_cast<float>(gh) * 0.72f)), 1, gh - 1);
    const int b1 = gh;

    bool changed = false;
    for (size_t i = 1; i + 1 < cps.size(); ++i) {
        if (cps[i] != " ") continue;
        if (!isAsciiAlnum(cps[i - 1]) || !isAsciiAlnum(cps[i + 1])) continue;

        const cv::Rect& prevBox = item.charBoxes[i - 1];
        const cv::Rect& spBox = item.charBoxes[i];
        const cv::Rect& nextBox = item.charBoxes[i + 1];

        const int sx1 = (std::max)(lineSafe.x, (std::min)(prevBox.x, spBox.x) - 2);
        const int sx2 = (std::min)(lineSafe.x + lineSafe.width,
                                   (std::max)(nextBox.x + nextBox.width, spBox.x + spBox.width) + 2);
        if (sx2 - sx1 < 6) continue;

        // 使用局部包围窗背景色（兼容单行内局部高亮选区如浅蓝底与周边白底混合场景）
        const int bgX1 = (std::max)(0, sx1 - 10);
        const int bgX2 = (std::min)(bgr.cols, sx2 + 10);
        bool localUniform = false;
        const cv::Vec3b bg = estimateBorderBackgroundColor(
            bgr(cv::Rect(bgX1, sy1, bgX2 - bgX1, gh)), &localUniform);
        if (!localUniform) continue;

        auto pixelDiff = [&](int absY, int absX) noexcept -> int {
            const auto& p = bgr.at<cv::Vec3b>(absY, absX);
            return (std::max)({
                std::abs(static_cast<int>(p[0]) - static_cast<int>(bg[0])),
                std::abs(static_cast<int>(p[1]) - static_cast<int>(bg[1])),
                std::abs(static_cast<int>(p[2]) - static_cast<int>(bg[2])),
            });
        };

        // 在上部字符主体带 [u0, u1) 定位前后字母之间干净无墨水的真实空隙 [gapL, gapR)
        const int sw = sx2 - sx1;
        std::vector<uint8_t> upperClean(static_cast<size_t>(sw), 1);
        for (int dx = 0; dx < sw; ++dx) {
            const int ax = sx1 + dx;
            for (int dy = u0; dy < u1; ++dy) {
                if (pixelDiff(sy1 + dy, ax) >= 36) {
                    upperClean[static_cast<size_t>(dx)] = 0;
                    break;
                }
            }
        }

        const float spCenterX = static_cast<float>(spBox.x) + 0.5f * static_cast<float>(spBox.width);
        const int maxGapW = (std::max)(14, gh);
        int bestGapL = -1;
        int bestGapR = -1;
        float bestDist = 1e9f;
        for (int dx = 0; dx < sw; ) {
            if (!upperClean[static_cast<size_t>(dx)]) {
                ++dx;
                continue;
            }
            int end = dx;
            while (end < sw && upperClean[static_cast<size_t>(end)]) ++end;
            const int gw = end - dx;
            if (dx > 0 && end < sw && gw >= 3 && gw <= maxGapW) {
                const float gMid = static_cast<float>(sx1 + dx) + 0.5f * static_cast<float>(gw);
                const float dist = std::abs(gMid - spCenterX);
                if (dist < bestDist) {
                    bestDist = dist;
                    bestGapL = sx1 + dx;
                    bestGapR = sx1 + end;
                }
            }
            dx = end;
        }
        if (bestGapL < 0 || bestDist > static_cast<float>((std::max)(10, gh))) continue;

        const int gw = bestGapR - bestGapL;
        int bestRow = -1;
        int bestRun = 0;
        int bestPeak = 0;
        int activeRows = 0;
        for (int dy = b0; dy < b1; ++dy) {
            const int ay = sy1 + dy;
            int curRun = 0;
            int rowMaxRun = 0;
            int rowPeak = 0;
            for (int ax = bestGapL; ax < bestGapR; ++ax) {
                const int d = pixelDiff(ay, ax);
                if (d >= 40) {
                    ++curRun;
                    if (curRun > rowMaxRun) rowMaxRun = curRun;
                    if (d > rowPeak) rowPeak = d;
                } else {
                    curRun = 0;
                }
            }
            if (rowMaxRun >= 3) ++activeRows;
            if (rowMaxRun > bestRun || (rowMaxRun == bestRun && rowPeak > bestPeak)) {
                bestRun = rowMaxRun;
                bestPeak = rowPeak;
                bestRow = ay;
            }
        }

        const int minRequiredRun = (std::max)(4, static_cast<int>(std::ceil(static_cast<float>(gw) * 0.60f)));
        const int maxAllowedThick = (std::max)(2, gh / 4);
        if (bestRow < 0 || bestRun < minRequiredRun || bestPeak < 48 || activeRows > maxAllowedThick) {
            continue;
        }

        // 反超链接通长下划线护栏：真实下划线 `_` 仅限于空隙底部，左右相邻字符脚下必为干净背景；
        // 若左右翼均沿同一像素行延伸出连续通长横线，说明属于带下划线的普通文本/超链接，严禁误改！
        auto countWingDark = [&](int xStart, int xEnd) noexcept -> int {
            int darkCols = 0;
            for (int ax = xStart; ax < xEnd; ++ax) {
                if (ax < 0 || ax >= bgr.cols) continue;
                int maxD = pixelDiff(bestRow, ax);
                if (bestRow - 1 >= sy1 + b0) maxD = (std::max)(maxD, pixelDiff(bestRow - 1, ax));
                if (bestRow + 1 < sy2) maxD = (std::max)(maxD, pixelDiff(bestRow + 1, ax));
                if (maxD >= 40) ++darkCols;
            }
            return darkCols;
        };
        const int leftWingDark = countWingDark(bestGapL - 5, bestGapL - 1);
        const int rightWingDark = countWingDark(bestGapR + 1, bestGapR + 5);
        if (leftWingDark >= 3 && rightWingDark >= 3) {
            continue;
        }

        cps[i] = "_";
        changed = true;
    }

    if (changed) {
        std::string rebuilt;
        rebuilt.reserve(item.text.size());
        for (const auto& cp : cps) {
            rebuilt += cp;
        }
        item.text = std::move(rebuilt);
    }
}

void DeepOcrEngine::normalizeStructuredJsonPunctuation(OcrResult& item) {
    if (item.text.empty()) return;
    auto cps = OcrEngine::splitUtf8Codepoints(item.text);
    if (cps.size() < 5) return;

    // 门禁：仅当行内存在至少一个明确的 JSON 键值结构 `"<ascii_key>"` + `:`/`：` 时才激活，
    // 严格确保中文散文、自然语言句子与常规代码 100% 零改动。
    bool hasJsonKeyColon = false;
    for (size_t i = 0; i < cps.size(); ++i) {
        if (matchAsciiJsonKeyColon(cps, i) != std::string::npos) {
            hasJsonKeyColon = true;
            break;
        }
    }
    if (!hasJsonKeyColon) return;

    bool changed = false;

    // 1. JSON 语法行中的中式弯引号 `“` / `”` 统一规整为标准 ASCII `"`（1-to-1 码点替换）
    for (auto& cp : cps) {
        if (cp == "\xE2\x80\x9C" || cp == "\xE2\x80\x9D") {
            cp = "\"";
            changed = true;
        }
    }

    // 2. JSON 键后的结构性全角冒号 `：` -> `:`（绝不碰字符串值内部的中文冒号，如 `型号：SY-F19-L`）
    for (size_t i = 0; i < cps.size(); ++i) {
        const size_t colonIdx = matchAsciiJsonKeyColon(cps, i);
        if (colonIdx != std::string::npos && cps[colonIdx] == "\xEF\xBC\x9A") {
            cps[colonIdx] = ":";
            changed = true;
        }
    }

    // 3. JSON 字段结束位的结构性全角逗号 `，` -> `,`（紧邻 `"`/数字/`]`/`}` 之后且位于行尾或下一键引号前）
    for (size_t i = 1; i < cps.size(); ++i) {
        if (cps[i] != "\xEF\xBC\x8C") continue; // `，`
        const std::string& prev = cps[i - 1];
        const bool prevEndsValue =
            prev == "\"" || prev == "]" || prev == "}" ||
            (prev.size() == 1 && prev[0] >= '0' && prev[0] <= '9');
        if (!prevEndsValue) continue;
        size_t next = i + 1;
        while (next < cps.size() && cps[next] == " ") ++next;
        const bool nextStartsKeyOrEol = (next >= cps.size()) || (cps[next] == "\"");
        if (nextStartsKeyOrEol) {
            cps[i] = ",";
            changed = true;
        }
    }

    // 4. 保守 JSON 对象花括号修复（合法 JSON 中不存在圆括号对象或裸单字符 `f` 值）：
    //    4a. 行首 `("Key":` / `f"Key":` -> `{"Key":`
    {
        size_t p = 0;
        while (p < cps.size() && cps[p] == " ") ++p;
        if (p < cps.size() && (cps[p] == "(" || cps[p] == "f")) {
            size_t q = p + 1;
            while (q < cps.size() && cps[q] == " ") ++q;
            if (matchAsciiJsonKeyColon(cps, q) != std::string::npos) {
                cps[p] = "{";
                changed = true;
            }
        }
    }

    //    4b. 数组内对象 `[("Key":` / `[f"Key":` -> `[{"Key":`（并同步规整尾部 `)]` -> `}]`）
    bool hasArrayObjOpen = false;
    for (size_t i = 0; i + 1 < cps.size(); ++i) {
        if (cps[i] == "[" && (cps[i + 1] == "(" || cps[i + 1] == "f")) {
            size_t q = i + 2;
            while (q < cps.size() && cps[q] == " ") ++q;
            if (matchAsciiJsonKeyColon(cps, q) != std::string::npos) {
                cps[i + 1] = "{";
                hasArrayObjOpen = true;
                changed = true;
            }
        } else if (cps[i] == "[" && cps[i + 1] == "{") {
            hasArrayObjOpen = true;
        }
    }
    if (hasArrayObjOpen) {
        for (size_t i = 0; i + 1 < cps.size(); ++i) {
            if (cps[i] == ")" && cps[i + 1] == "]") {
                cps[i] = "}";
                changed = true;
            }
        }
    }

    //    4c. 键值对嵌套对象起始 `"key": f` (行尾孤立单字符 `f`/`(`) 或 `"key": f"childKey":` -> `"key": {`
    //        （严格保护 `"flag": false` 与 `"code": "f"` 绝不误改）
    for (size_t i = 0; i < cps.size(); ++i) {
        const size_t colonIdx = matchAsciiJsonKeyColon(cps, i);
        if (colonIdx == std::string::npos) continue;
        size_t v = colonIdx + 1;
        while (v < cps.size() && cps[v] == " ") ++v;
        if (v < cps.size() && (cps[v] == "f" || cps[v] == "(")) {
            size_t after = v + 1;
            while (after < cps.size() && cps[after] == " ") ++after;
            if (after >= cps.size() || matchAsciiJsonKeyColon(cps, after) != std::string::npos) {
                cps[v] = "{";
                changed = true;
            }
        }
    }

    if (changed) {
        std::string rebuilt;
        rebuilt.reserve(item.text.size());
        for (const auto& cp : cps) {
            rebuilt += cp;
        }
        item.text = std::move(rebuilt);
    }
}

} // namespace tools3000::ocr
