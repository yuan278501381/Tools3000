// ─────────────────────────────────────────────────────────────────────────────
// OcrEngine.cpp — 基于 Windows.Media.Ocr (WinRT) 的离线文字识别
//
// 选型理由 (相较 PaddleOCR Lite 外部进程方案):
//   • 完全离线、随系统内置，无需打包 ~30MB 模型与 exe;
//   • 官方 API，支持 25+ 语言 (依赖已安装的语言包);
//   • 与需求最低版本 Windows 10 1903 完全吻合。
//
// cv::Mat (BGR/BGRA/灰度) → SoftwareBitmap(Bgra8) → OcrEngine.RecognizeAsync。
// ─────────────────────────────────────────────────────────────────────────────

#include "ocr/OcrEngine.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>

#include <unknwn.h>
#include <windows.h>
#include <MemoryBuffer.h>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <fstream>
#include <iterator>

namespace tools3000::ocr {

namespace wf  = winrt::Windows::Foundation;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wmo = winrt::Windows::Media::Ocr;
namespace wgl = winrt::Windows::Globalization;

namespace {

/// 确保当前线程已进入 COM/WinRT 公寓 (OCR 可能在工作线程上被调用)。
void ensureApartment() {
    static thread_local bool inited = false;
    if (inited) return;
    inited = true;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error const&) {
        // 线程已用其它模型初始化亦可正常调用 OCR，忽略。
    }
}

/// cv::Mat → SoftwareBitmap(Bgra8)。失败返回 nullptr。
wgi::SoftwareBitmap matToBitmap(const cv::Mat& src) {
    cv::Mat bgra;
    switch (src.channels()) {
        case 4: bgra = src; break;
        case 3: cv::cvtColor(src, bgra, cv::COLOR_BGR2BGRA); break;
        case 1: cv::cvtColor(src, bgra, cv::COLOR_GRAY2BGRA); break;
        default: return nullptr;
    }
    if (!bgra.isContinuous()) bgra = bgra.clone();

    wgi::SoftwareBitmap bitmap(wgi::BitmapPixelFormat::Bgra8, bgra.cols, bgra.rows,
                               wgi::BitmapAlphaMode::Premultiplied);
    {
        wgi::BitmapBuffer buffer = bitmap.LockBuffer(wgi::BitmapBufferAccessMode::Write);
        wf::IMemoryBufferReference ref = buffer.CreateReference();
        auto byteAccess = ref.as<::Windows::Foundation::IMemoryBufferByteAccess>();

        uint8_t* data = nullptr;
        uint32_t capacity = 0;
        winrt::check_hresult(byteAccess->GetBuffer(&data, &capacity));

        wgi::BitmapPlaneDescription desc = buffer.GetPlaneDescription(0);
        const size_t rowBytes = static_cast<size_t>(bgra.cols) * 4;
        for (int y = 0; y < bgra.rows; ++y) {
            std::memcpy(data + desc.StartIndex + static_cast<size_t>(desc.Stride) * y,
                        bgra.ptr(y), rowBytes);
        }
    }
    return bitmap;
}

/// 创建一个 OCR 引擎: 优先用户语言, 回退英文。
wmo::OcrEngine createEngine() {
    if (auto e = wmo::OcrEngine::TryCreateFromUserProfileLanguages()) return e;
    try {
        return wmo::OcrEngine::TryCreateFromLanguage(wgl::Language(L"en-US"));
    } catch (winrt::hresult_error const&) {
        return nullptr;
    }
}

bool isCjkOrFullwidth(wchar_t ch) noexcept {
    return (ch >= 0x2E80 && ch <= 0x9FFF) ||
           (ch >= 0xAC00 && ch <= 0xD7AF) ||
           (ch >= 0xF900 && ch <= 0xFAFF) ||
           (ch >= 0xFE30 && ch <= 0xFE4F) ||
           (ch >= 0xFF00 && ch <= 0xFFEF) ||
           (ch >= 0x3000 && ch <= 0x303F);
}

bool isTightConnectorLeft(wchar_t ch) noexcept {
    return ch == L'/' || ch == L'\\' || ch == L'.' || ch == L'_' || ch == L'-' ||
           ch == L'#' || ch == L'(' || ch == L'[' || ch == L'{' || ch == L'@' ||
           ch == L':' || ch == L'—' || ch == L'–';
}

bool isTightConnectorRight(wchar_t ch) noexcept {
    return ch == L'/' || ch == L'\\' || ch == L'.' || ch == L'_' || ch == L'-' ||
           ch == L'#' || ch == L')' || ch == L']' || ch == L'}' || ch == L':' ||
           ch == L',' || ch == L';' || ch == L'—' || ch == L'–';
}

bool isAsciiDigit(wchar_t ch) noexcept {
    return ch >= L'0' && ch <= L'9';
}

struct WordToken {
    std::wstring text;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float cy() const noexcept { return y + h * 0.5f; }
    float right() const noexcept { return x + w; }
};

}  // namespace

OcrEngine& OcrEngine::instance() {
    static OcrEngine inst;
    return inst;
}

bool OcrEngine::initialize() {
    try {
        ensureApartment();
        m_available = static_cast<bool>(createEngine());
        if (m_available) {
            LOG_INFO("OCR 引擎已就绪 (Windows.Media.Ocr)");
        } else {
            LOG_WARN("未检测到 OCR 语言包，OCR 功能不可用。可在系统设置中添加语言的“可选功能 → 光学字符识别”");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("OCR 引擎初始化异常: {}", e.what());
        m_available = false;
    }
    return m_available;
}

void OcrEngine::shutdown() {
    LOG_INFO("OCR 引擎已关闭");
}

std::vector<OcrResult> OcrEngine::extractText(const cv::Mat& image) {
    tools3000::core::TraceId::Scope scope;
    std::vector<OcrResult> results;
    if (image.empty()) {
        LOG_WARN("OCR 输入图像为空");
        return results;
    }

    try {
        ensureApartment();
        wmo::OcrEngine engine = createEngine();
        if (!engine) {
            LOG_ERROR("OCR 不可用: 无语言包");
            return results;
        }

        // 图像预处理：
        //   1) 中小尺寸截图：边缘安全留白 + 2x 双三次放大，提升小字号识别率
        //   2) 超宽截图 (> MaxImageDimension)：按比例等比缩宽至上限以内，防止 WinRT E_INVALIDARG 崩溃
        //   3) 超高长截图 (> MaxImageDimension)：保持清晰字号按重叠分片纵向逐块识别并去重拼接
        cv::Mat prepared = image;
        int borderPad = 0;
        float scaleFactor = 1.0f;
        const int maxDim = std::max(256, static_cast<int>(wmo::OcrEngine::MaxImageDimension()));
        if (image.cols >= 8 && image.rows >= 8 && image.cols + 24 < maxDim && image.rows + 24 < maxDim) {
            borderPad = 12;
            cv::copyMakeBorder(image, prepared, borderPad, borderPad, borderPad, borderPad, cv::BORDER_REPLICATE);
            if (std::max(prepared.cols, prepared.rows) < 1800 &&
                prepared.cols * 2 <= maxDim && prepared.rows * 2 <= maxDim) {
                scaleFactor = 2.0f;
                cv::resize(prepared, prepared, cv::Size(), scaleFactor, scaleFactor, cv::INTER_CUBIC);
            }
        } else if (image.cols > maxDim) {
            scaleFactor = static_cast<float>(maxDim) / static_cast<float>(image.cols);
            const int targetW = std::min(maxDim, std::max(1, static_cast<int>(std::floor(image.cols * scaleFactor))));
            const int targetH = std::max(1, static_cast<int>(std::floor(image.rows * scaleFactor)));
            scaleFactor = static_cast<float>(targetW) / static_cast<float>(image.cols);
            cv::resize(image, prepared, cv::Size(targetW, targetH), 0.0, 0.0, cv::INTER_AREA);
        }

        // 收集所有识别词块并按视觉行聚类、自上而下/从左到右重排与智能无损拼接
        std::vector<WordToken> tokens;
        if (prepared.rows <= maxDim) {
            wgi::SoftwareBitmap bitmap = matToBitmap(prepared);
            if (!bitmap) {
                LOG_ERROR("OCR: 不支持的图像通道数: {}", image.channels());
                return results;
            }
            wmo::OcrResult result = engine.RecognizeAsync(bitmap).get();
            for (auto const& line : result.Lines()) {
                for (auto const& w : line.Words()) {
                    std::wstring wt(w.Text().c_str());
                    if (wt.empty()) continue;
                    wf::Rect br = w.BoundingRect();
                    tokens.push_back(WordToken{std::move(wt), br.X, br.Y, br.Width, br.Height});
                }
            }
        } else {
            const int overlap = std::min(96, maxDim / 8);
            const int step = std::max(64, maxDim - overlap);
            for (int tileTop = 0; tileTop < prepared.rows; tileTop += step) {
                const int tileH = std::min(maxDim, prepared.rows - tileTop);
                if (tileH < 8) break;
                cv::Mat tile = prepared(cv::Rect(0, tileTop, prepared.cols, tileH));
                wgi::SoftwareBitmap bitmap = matToBitmap(tile);
                if (!bitmap) {
                    LOG_ERROR("OCR: 不支持的图像通道数: {}", image.channels());
                    return results;
                }
                const size_t prevTokenCount = tokens.size();
                wmo::OcrResult result = engine.RecognizeAsync(bitmap).get();
                for (auto const& line : result.Lines()) {
                    for (auto const& w : line.Words()) {
                        std::wstring wt(w.Text().c_str());
                        if (wt.empty()) continue;
                        wf::Rect br = w.BoundingRect();
                        const float globalY = br.Y + static_cast<float>(tileTop);
                        if (tileTop > 0 && br.Y < static_cast<float>(overlap) + br.Height) {
                            bool duplicate = false;
                            const float candCy = globalY + br.Height * 0.5f;
                            for (size_t k = 0; k < prevTokenCount; ++k) {
                                const auto& existing = tokens[k];
                                if (existing.y + existing.h < static_cast<float>(tileTop) - 4.0f) continue;
                                if (existing.text == wt &&
                                    std::abs(existing.cy() - candCy) <= std::max(6.0f, br.Height * 0.6f) &&
                                    std::abs(existing.x - br.X) <= std::max(8.0f, br.Width * 0.5f)) {
                                    duplicate = true;
                                    break;
                                }
                            }
                            if (duplicate) continue;
                        }
                        tokens.push_back(WordToken{std::move(wt), br.X, globalY, br.Width, br.Height});
                    }
                }
                if (tileTop + tileH >= prepared.rows) break;
            }
        }

        if (tokens.empty()) {
            LOG_INFO("OCR 完成, 识别行数: {}", results.size());
            return results;
        }

        // 先按垂直中心排序
        std::sort(tokens.begin(), tokens.end(), [](const WordToken& a, const WordToken& b) {
            return a.cy() < b.cy();
        });

        struct VisualLine {
            std::vector<WordToken> words;
            float sumCy = 0.0f;
            float sumH = 0.0f;
            float avgCy() const noexcept { return words.empty() ? 0.0f : sumCy / words.size(); }
            float avgH() const noexcept { return words.empty() ? 16.0f : sumH / words.size(); }
        };

        std::vector<VisualLine> visualLines;
        for (auto& tok : tokens) {
            int bestIdx = -1;
            float bestDist = FLT_MAX;
            for (int i = static_cast<int>(visualLines.size()) - 1; i >= 0; --i) {
                const float refH = std::max(8.0f, std::min(visualLines[i].avgH(), tok.h));
                const float dy = std::abs(tok.cy() - visualLines[i].avgCy());
                if (dy <= refH * 0.58f && dy < bestDist) {
                    bestDist = dy;
                    bestIdx = i;
                }
            }
            if (bestIdx >= 0) {
                visualLines[bestIdx].sumCy += tok.cy();
                visualLines[bestIdx].sumH += tok.h;
                visualLines[bestIdx].words.push_back(std::move(tok));
            } else {
                VisualLine vl;
                vl.sumCy = tok.cy();
                vl.sumH = tok.h;
                vl.words.push_back(std::move(tok));
                visualLines.push_back(std::move(vl));
            }
        }

        std::sort(visualLines.begin(), visualLines.end(), [](const VisualLine& a, const VisualLine& b) {
            return a.avgCy() < b.avgCy();
        });

        for (auto& vl : visualLines) {
            if (vl.words.empty()) continue;
            std::sort(vl.words.begin(), vl.words.end(), [](const WordToken& a, const WordToken& b) {
                return a.x < b.x;
            });

            std::wstring lineW;
            float minX = FLT_MAX, minY = FLT_MAX, maxX = 0.0f, maxY = 0.0f;
            const float lineH = std::max(10.0f, vl.avgH());

            for (size_t i = 0; i < vl.words.size(); ++i) {
                const auto& cur = vl.words[i];
                minX = std::min(minX, cur.x);
                minY = std::min(minY, cur.y);
                maxX = std::max(maxX, cur.right());
                maxY = std::max(maxY, cur.y + cur.h);

                if (i > 0) {
                    const auto& prev = vl.words[i - 1];
                    const float gap = cur.x - prev.right();
                    const wchar_t lastCh = prev.text.back();
                    const wchar_t firstCh = cur.text.front();

                    if (gap > lineH * 1.35f) {
                        // 明显的分栏或表格大间距
                        lineW += L"    ";
                    } else if (isCjkOrFullwidth(lastCh) || isCjkOrFullwidth(firstCh)) {
                        // 中文与全角标点之间绝不插入多余空格（除非间隔超过大半个字宽）
                        if (gap > lineH * 0.65f) {
                            lineW += L' ';
                        }
                    } else if (isTightConnectorLeft(lastCh) || isTightConnectorRight(firstCh)) {
                        // 路径、文件名、代码标点（如 .cpp#L1—319）紧密相连
                        if (gap > lineH * 0.55f) {
                            lineW += L' ';
                        }
                    } else if (isAsciiDigit(lastCh) && isAsciiDigit(firstCh) && gap < lineH * 0.32f) {
                        // 被拆碎的连续数字（如 31 9 -> 319）
                    } else if (gap >= lineH * 0.14f) {
                        // 常规西文单词间距
                        lineW += L' ';
                    }
                }
                lineW += cur.text;
            }

            OcrResult r;
            r.text = tools3000::core::WinUtils::wstringToUtf8(lineW);
            r.confidence = 1.0f;

            const int ox1 = std::clamp(static_cast<int>(std::round(minX / scaleFactor)) - borderPad, 0, image.cols);
            const int oy1 = std::clamp(static_cast<int>(std::round(minY / scaleFactor)) - borderPad, 0, image.rows);
            const int ox2 = std::clamp(static_cast<int>(std::round(maxX / scaleFactor)) - borderPad, ox1, image.cols);
            const int oy2 = std::clamp(static_cast<int>(std::round(maxY / scaleFactor)) - borderPad, oy1, image.rows);
            r.box = cv::Rect(ox1, oy1, ox2 - ox1, oy2 - oy1);
            results.push_back(std::move(r));
        }

        LOG_INFO("OCR 完成, 识别行数: {}", results.size());
    } catch (winrt::hresult_error const& e) {
        LOG_ERROR("OCR WinRT 错误: 0x{:08X} {}",
                  static_cast<uint32_t>(e.code()),
                  tools3000::core::WinUtils::wstringToUtf8(e.message().c_str()));
    } catch (const std::exception& e) {
        LOG_ERROR("OCR 异常: {}", e.what());
    }
    return results;
}

std::string OcrEngine::recognizeToText(const cv::Mat& image) {
    std::string text;
    for (auto const& r : extractText(image)) {
        if (!text.empty()) text += '\n';
        text += r.text;
    }
    return text;
}

std::string OcrEngine::recognizeImageFile(const std::string& utf8Path) {
    cv::Mat img;
    try {
        // OpenCV imread 需要本地编码路径; 用宽字符读文件再解码以支持非 ASCII 路径。
        std::wstring wpath = tools3000::core::WinUtils::utf8ToWstring(utf8Path);
        std::ifstream f(wpath.c_str(), std::ios::binary);
        if (f) {
            std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
            if (!buf.empty()) img = cv::imdecode(buf, cv::IMREAD_COLOR);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("OCR 读取图片失败: {} ({})", utf8Path, e.what());
    }
    if (img.empty()) {
        LOG_WARN("OCR: 无法加载图片 {}", utf8Path);
        return {};
    }
    return recognizeToText(img);
}

}  // namespace tools3000::ocr
