// ─────────────────────────────────────────────────────────────────────────────
// BeautyShell.cpp — 截图美化外壳导出引擎实现 (CleanShot X / PixPin 风格)
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/BeautyShell.h"
#include "capture/MarkupEngine.h"

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

namespace tools3000::capture {
namespace {

void drawFilledRoundedRect(cv::Mat& mask, const cv::Rect& rect, int radius, uint8_t val = 255) {
    if (rect.width <= 0 || rect.height <= 0) return;
    int r = std::clamp(radius, 0, std::min(rect.width / 2, rect.height / 2));
    if (r <= 0) {
        cv::rectangle(mask, rect, cv::Scalar(val), cv::FILLED);
        return;
    }

    // 中间主要矩形块
    cv::rectangle(mask, cv::Rect(rect.x + r, rect.y, rect.width - 2 * r, rect.height),
                  cv::Scalar(val), cv::FILLED);
    cv::rectangle(mask, cv::Rect(rect.x, rect.y + r, r, rect.height - 2 * r),
                  cv::Scalar(val), cv::FILLED);
    cv::rectangle(mask, cv::Rect(rect.x + rect.width - r, rect.y + r, r, rect.height - 2 * r),
                  cv::Scalar(val), cv::FILLED);

    // 四个圆角
    cv::circle(mask, cv::Point(rect.x + r, rect.y + r), r, cv::Scalar(val), cv::FILLED, cv::LINE_AA);
    cv::circle(mask, cv::Point(rect.x + rect.width - r - 1, rect.y + r), r, cv::Scalar(val), cv::FILLED, cv::LINE_AA);
    cv::circle(mask, cv::Point(rect.x + r, rect.y + rect.height - r - 1), r, cv::Scalar(val), cv::FILLED, cv::LINE_AA);
    cv::circle(mask, cv::Point(rect.x + rect.width - r - 1, rect.y + rect.height - r - 1), r, cv::Scalar(val), cv::FILLED, cv::LINE_AA);
}

void getBackgroundPalette(BeautyBackgroundType type, cv::Vec3f& c1, cv::Vec3f& c2, cv::Vec3f& c3) {
    switch (type) {
        case BeautyBackgroundType::StudioSlate:
            // 极光星云: Indigo (#6366F1 -> BGR: 241, 102, 99) -> Violet (#8B5CF6 -> BGR: 246, 92, 139) -> Rose (#EC4899 -> BGR: 153, 72, 236)
            c1 = cv::Vec3f(241.0f, 102.0f, 99.0f);
            c2 = cv::Vec3f(246.0f, 92.0f, 139.0f);
            c3 = cv::Vec3f(153.0f, 72.0f, 236.0f);
            break;
        case BeautyBackgroundType::PaperChalk:
            // 晨曦落日: Coral (#FB7185 -> BGR: 133, 113, 251) -> Amber (#F97316 -> BGR: 22, 115, 249) -> Gold (#FACC15 -> BGR: 21, 204, 250)
            c1 = cv::Vec3f(133.0f, 113.0f, 251.0f);
            c2 = cv::Vec3f(22.0f, 115.0f, 249.0f);
            c3 = cv::Vec3f(21.0f, 204.0f, 250.0f);
            break;
        case BeautyBackgroundType::SilkMist:
            // 碧海极光: Sky (#0EA5E9 -> BGR: 233, 165, 14) -> Teal (#14B8A6 -> BGR: 166, 184, 20) -> Emerald (#10B981 -> BGR: 129, 185, 16)
            c1 = cv::Vec3f(233.0f, 165.0f, 14.0f);
            c2 = cv::Vec3f(166.0f, 184.0f, 20.0f);
            c3 = cv::Vec3f(129.0f, 185.0f, 16.0f);
            break;
        case BeautyBackgroundType::MidnightGraphite:
            // 曜石黑晶: Slate (#334155 -> BGR: 85, 65, 51) -> Obsidian (#1E293B -> BGR: 59, 41, 30) -> Carbon (#090D16 -> BGR: 22, 13, 9)
            c1 = cv::Vec3f(85.0f, 65.0f, 51.0f);
            c2 = cv::Vec3f(59.0f, 41.0f, 30.0f);
            c3 = cv::Vec3f(22.0f, 13.0f, 9.0f);
            break;
        case BeautyBackgroundType::PureMinimal:
        default:
            // 钛金画廊: Alabaster (#F8FAFC -> BGR: 252, 250, 248) -> Silver (#E2E8F0 -> BGR: 240, 232, 226) -> Titanium (#CBD5E1 -> BGR: 225, 213, 203)
            c1 = cv::Vec3f(252.0f, 250.0f, 248.0f);
            c2 = cv::Vec3f(240.0f, 232.0f, 226.0f);
            c3 = cv::Vec3f(225.0f, 213.0f, 203.0f);
            break;
    }
}

float roundedRectSdf(float px, float py, float width, float height, float radius) {
    const float r = std::clamp(radius, 0.0f, std::min(width, height) * 0.5f);
    const float hx = width * 0.5f - r;
    const float hy = height * 0.5f - r;
    const float dx = std::max(std::abs(px - width * 0.5f) - hx, 0.0f);
    const float dy = std::max(std::abs(py - height * 0.5f) - hy, 0.0f);
    return std::hypot(dx, dy) - r;
}

} // namespace

cv::Mat applyBeautyShell(const cv::Mat& src, const BeautyShellOptions& options) {
    if (src.empty() || src.cols <= 0 || src.rows <= 0) {
        return src.clone();
    }

    // 确保处理输入为 3 通道 BGR 格式
    cv::Mat inputBgr;
    if (src.channels() == 4) {
        cv::cvtColor(src, inputBgr, cv::COLOR_BGRA2BGR);
    } else if (src.channels() == 3) {
        inputBgr = src;
    } else {
        cv::cvtColor(src, inputBgr, cv::COLOR_GRAY2BGR);
    }

    const int pad = std::max(0, options.padding);
    const int outW = inputBgr.cols + pad * 2;
    const int outH = inputBgr.rows + pad * 2;

    // 1. 生成三色段平滑网格渐变衬底画布（带左上角环境柔光高光）
    cv::Mat canvas(outH, outW, CV_8UC3);
    cv::Vec3f c1, c2, c3;
    getBackgroundPalette(options.bgType, c1, c2, c3);

    const float invOutW = 1.0f / static_cast<float>(std::max(1, outW));
    const float invOutH = 1.0f / static_cast<float>(std::max(1, outH));
    const float bloomStrength = (options.bgType == BeautyBackgroundType::MidnightGraphite) ? 18.0f : 26.0f;

    for (int y = 0; y < outH; ++y) {
        cv::Vec3b* row = canvas.ptr<cv::Vec3b>(y);
        const float fy = y * invOutH;
        for (int x = 0; x < outW; ++x) {
            const float fx = x * invOutW;
            const float t = std::clamp(fx * 0.58f + fy * 0.42f, 0.0f, 1.0f);
            cv::Vec3f baseColor;
            if (t < 0.5f) {
                const float localT = t * 2.0f;
                baseColor = c1 * (1.0f - localT) + c2 * localT;
            } else {
                const float localT = (t - 0.5f) * 2.0f;
                baseColor = c2 * (1.0f - localT) + c3 * localT;
            }
            // 左上环境漫射高光 (Mesh Bloom)
            const float distSq = (fx - 0.18f) * (fx - 0.18f) + (fy - 0.15f) * (fy - 0.15f);
            const float bloom = std::max(0.0f, 1.0f - distSq * 2.2f) * bloomStrength;
            row[x] = cv::Vec3b(
                static_cast<uint8_t>(std::clamp(baseColor[0] + bloom, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(baseColor[1] + bloom, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(baseColor[2] + bloom, 0.0f, 255.0f))
            );
        }
    }

    // 2. 渲染物理双层高斯弥散投影 (Ambient 空气感大弥散 + Key 边缘紧致下沉接触阴影)
    if (pad > 0 && options.shadowRadius > 0.0f && options.shadowOpacity > 0.0f) {
        int maxK = std::min(outW, outH);
        if (maxK % 2 == 0) maxK--;
        if (maxK >= 3) {
            int shadowR = static_cast<int>(std::round(options.cornerRadius));

            auto renderShadowLayer = [&](float radiusMult, float offsetMult, float alphaMult) {
                float effectiveRadius = options.shadowRadius * radiusMult;
                if (effectiveRadius <= 0.5f) return;
                int sOffsetY = std::clamp(static_cast<int>(std::round(options.shadowOffsetY * offsetMult)), -pad, pad);
                cv::Rect shadowRect(pad, pad + sOffsetY, inputBgr.cols, inputBgr.rows);
                shadowRect &= cv::Rect(0, 0, outW, outH);
                if (shadowRect.width <= 0 || shadowRect.height <= 0) return;

                cv::Mat shadowMask(outH, outW, CV_8UC1, cv::Scalar(0));
                drawFilledRoundedRect(shadowMask, shadowRect, shadowR, 255);

                int ksize = static_cast<int>(std::round(effectiveRadius * 2.2f)) | 1;
                ksize = std::max(3, std::min(ksize, std::min(121, maxK)));
                double sigma = effectiveRadius * 0.58;
                try {
                    cv::GaussianBlur(shadowMask, shadowMask, cv::Size(ksize, ksize), sigma);
                } catch (...) {
                    return;
                }

                const float shadowAlpha = std::clamp(options.shadowOpacity * alphaMult, 0.0f, 1.0f);
                for (int y = 0; y < outH; ++y) {
                    const uint8_t* sRow = shadowMask.ptr<uint8_t>(y);
                    cv::Vec3b* cRow = canvas.ptr<cv::Vec3b>(y);
                    for (int x = 0; x < outW; ++x) {
                        if (sRow[x] > 0) {
                            float a = (sRow[x] / 255.0f) * shadowAlpha;
                            float invA = 1.0f - a;
                            cRow[x][0] = static_cast<uint8_t>(cRow[x][0] * invA);
                            cRow[x][1] = static_cast<uint8_t>(cRow[x][1] * invA);
                            cRow[x][2] = static_cast<uint8_t>(cRow[x][2] * invA);
                        }
                    }
                }
            };

            // Layer 1: Ambient 空气感大范围弥散阴影
            renderShadowLayer(1.45f, 1.15f, 0.65f);
            // Layer 2: Key 近距离边缘紧致接触阴影
            renderShadowLayer(0.42f, 0.35f, 0.50f);
        }
    }

    // 3. 亚像素级 SDF 圆角裁切与高光玻璃内嵌描边合成
    cv::Rect targetRoi(pad, pad, inputBgr.cols, inputBgr.rows);
    cv::Mat canvasRoi = canvas(targetRoi);
    const float wF = static_cast<float>(inputBgr.cols);
    const float hF = static_cast<float>(inputBgr.rows);
    const float rF = std::clamp(options.cornerRadius, 0.0f, std::min(wF, hF) * 0.5f);

    for (int y = 0; y < inputBgr.rows; ++y) {
        const cv::Vec3b* sRow = inputBgr.ptr<cv::Vec3b>(y);
        cv::Vec3b* dRow = canvasRoi.ptr<cv::Vec3b>(y);
        const float py = y + 0.5f;
        for (int x = 0; x < inputBgr.cols; ++x) {
            const float px = x + 0.5f;
            const float dist = roundedRectSdf(px, py, wF, hF, rF);
            if (dist <= -1.8f) {
                dRow[x] = sRow[x];
            } else if (dist < 0.8f) {
                // 亚像素抗锯齿边缘覆盖度
                const float cov = std::clamp(0.5f - dist, 0.0f, 1.0f);
                cv::Vec3f srcCol(sRow[x][0], sRow[x][1], sRow[x][2]);

                // 距离边界 [-1.6, -0.3] 区间叠加微晶高光玻璃内环 (顶部/左侧偏亮，呈现立体倒角光泽)
                if (pad > 0 && dist > -1.7f && dist < -0.1f) {
                    const float ringFactor = std::max(0.0f, 1.0f - std::abs(dist + 0.85f) * 1.3f);
                    const float topLight = std::clamp(1.0f - (py / std::max(1.0f, hF)) * 0.65f, 0.35f, 1.0f);
                    const float specAlpha = ringFactor * 0.36f * topLight;
                    srcCol = srcCol * (1.0f - specAlpha) + cv::Vec3f(255.0f, 255.0f, 255.0f) * specAlpha;
                }

                const float invCov = 1.0f - cov;
                dRow[x][0] = static_cast<uint8_t>(std::clamp(srcCol[0] * cov + dRow[x][0] * invCov, 0.0f, 255.0f));
                dRow[x][1] = static_cast<uint8_t>(std::clamp(srcCol[1] * cov + dRow[x][1] * invCov, 0.0f, 255.0f));
                dRow[x][2] = static_cast<uint8_t>(std::clamp(srcCol[2] * cov + dRow[x][2] * invCov, 0.0f, 255.0f));
            }
        }
    }

    cv::Mat outBgra;
    cv::cvtColor(canvas, outBgra, cv::COLOR_BGR2BGRA);
    if (pad > 0 && options.cornerRadius > 0.0f) {
        outBgra = applyRoundedCorners(outBgra, std::max(12.0f, options.cornerRadius + 6.0f));
    }
    return outBgra;
}

} // namespace tools3000::capture
