// ─────────────────────────────────────────────────────────────────────────────
// ScreenRecorder.cpp — 屏幕录制引擎实现
//
// 录制管道:
//   可替换捕获后端 → sws_scale 转换 → avcodec_send_frame
//   → avcodec_receive_packet → av_write_frame → 输出文件
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/ScreenRecorder.h"
#include "capture/ScreenCapture.h"
#include "capture/ClipboardUtils.h"
#include "common/AtomicFile.h"
#include "core/logger/Logger.h"
#include "core/stats/PerformanceMonitor.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"

#include <chrono>
#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <vector>
#include <shlobj.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

// FFmpeg C 头文件
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace tools3000::capture {

namespace {

constexpr std::uint64_t StorageReserveBytes = 256ULL * 1024 * 1024;
constexpr std::int64_t StorageWarningSeconds = 5 * 60;

std::string ffmpegError(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(error, buffer.data(), buffer.size());
    return buffer.data();
}

void addEncoderCandidate(std::vector<const AVCodec*>& codecs, const AVCodec* codec) {
    if (!codec) return;
    for (const auto* existing : codecs) {
        if (existing == codec || std::strcmp(existing->name, codec->name) == 0) return;
    }
    codecs.push_back(codec);
}

std::vector<const AVCodec*> encoderCandidates(RecordFormat format) {
    std::vector<const AVCodec*> codecs;
    AVCodecID codecId = AV_CODEC_ID_H264;
    switch (format) {
        case RecordFormat::MP4_H264:
            codecId = AV_CODEC_ID_H264;
            for (const char* name : {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf",
                                     "libx264", "libopenh264", "mpeg4"}) {
                addEncoderCandidate(codecs, avcodec_find_encoder_by_name(name));
            }
            break;
        case RecordFormat::MP4_H265:
            codecId = AV_CODEC_ID_HEVC;
            for (const char* name : {"hevc_nvenc", "hevc_qsv", "hevc_amf", "hevc_mf",
                                     "libx265", "mpeg4"}) {
                addEncoderCandidate(codecs, avcodec_find_encoder_by_name(name));
            }
            break;
        case RecordFormat::WebM_VP9:
            codecId = AV_CODEC_ID_VP9;
            addEncoderCandidate(codecs, avcodec_find_encoder_by_name("libvpx-vp9"));
            break;
        case RecordFormat::GIF:
            codecId = AV_CODEC_ID_GIF;
            break;
    }
    // 对 H.264/H.265 只使用明确知道输入约束的编码器。FFmpeg 的 generic lookup
    // 在某些构建中会返回仅接收 GPU frame 的 d3d12va encoder，软件帧必然失败。
    if (format != RecordFormat::MP4_H264 && format != RecordFormat::MP4_H265) {
        addEncoderCandidate(codecs, avcodec_find_encoder(codecId));
    }
    return codecs;
}

bool isHardwareEncoder(const char* name) {
    if (!name) return false;
    return std::strstr(name, "_nvenc") || std::strstr(name, "_qsv") ||
           std::strstr(name, "_amf") || std::strstr(name, "_mf");
}

const char* formatId(RecordFormat format) {
    switch (format) {
        case RecordFormat::MP4_H264: return "mp4_h264";
        case RecordFormat::MP4_H265: return "mp4_h265";
        case RecordFormat::WebM_VP9: return "webm_vp9";
        case RecordFormat::GIF: return "gif";
    }
    return "unknown";
}

AVPixelFormat choosePixelFormat(const AVCodec* codec, RecordFormat format) {
    const bool mediaFoundation = codec && std::strstr(codec->name, "_mf") != nullptr;
    const void* configs = nullptr;
    int count = 0;
    const int result = avcodec_get_supported_config(
        nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count);
    if (result < 0 || !configs || count <= 0) {
        // FFmpeg 的 Media Foundation wrapper 没有公布静态 pix_fmts 列表；
        // Windows H.264/H.265 MFT 的软件输入格式是 NV12。退回 YUV420P 会在
        // avcodec_open2 中直接返回 EINVAL，表现为用户无法开始录屏。
        if (mediaFoundation) return AV_PIX_FMT_NV12;
        return format == RecordFormat::GIF ? AV_PIX_FMT_PAL8 : AV_PIX_FMT_YUV420P;
    }

    const auto* formats = static_cast<const AVPixelFormat*>(configs);
    const std::array<AVPixelFormat, 7> preferred = format == RecordFormat::GIF
        ? std::array<AVPixelFormat, 7>{AV_PIX_FMT_PAL8, AV_PIX_FMT_RGB8, AV_PIX_FMT_BGR8,
                                      AV_PIX_FMT_BGRA, AV_PIX_FMT_BGR0, AV_PIX_FMT_YUV420P,
                                      AV_PIX_FMT_NV12}
        : mediaFoundation
            ? std::array<AVPixelFormat, 7>{AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P,
                                          AV_PIX_FMT_BGRA, AV_PIX_FMT_BGR0, AV_PIX_FMT_YUV444P,
                                          AV_PIX_FMT_P010}
            : std::array<AVPixelFormat, 7>{AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_YUVJ420P,
                                          AV_PIX_FMT_BGRA, AV_PIX_FMT_BGR0, AV_PIX_FMT_YUV444P,
                                          AV_PIX_FMT_P010};
    for (const auto candidate : preferred) {
        for (int index = 0; index < count; ++index) {
            if (formats[index] == candidate) return candidate;
        }
    }
    return formats[0];
}

AVSampleFormat chooseAudioSampleFormat(const AVCodec* codec) {
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                     0, &configs, &count) >= 0 && configs && count > 0) {
        const auto* formats = static_cast<const AVSampleFormat*>(configs);
        for (const auto preferred : {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT}) {
            for (int index = 0; index < count; ++index) {
                if (formats[index] == preferred) return preferred;
            }
        }
        return formats[0];
    }
    return AV_SAMPLE_FMT_FLTP;
}

std::filesystem::path outputDirectory(const std::string& outputPath) {
    std::error_code ec;
    auto directory = std::filesystem::path(
        tools3000::core::WinUtils::utf8ToWstring(outputPath)).parent_path();
    if (directory.empty()) directory = std::filesystem::current_path(ec);
    return directory;
}

bool probeDirectoryWritable(const std::filesystem::path& directory, DWORD& error) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto probe = directory /
            (L".tools3000-write-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt) + L".tmp");
        HANDLE file = CreateFileW(
            probe.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            error = ERROR_SUCCESS;
            return true;
        }
        error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

}  // namespace


void applyVideoFrameCornerMask(uint8_t* data, int width, int height, int stride, CapturePixelFormat format, float cornerRadius) {
    if (!data || cornerRadius <= 0.5f || width <= 0 || height <= 0) return;
    const float r = (std::min)({cornerRadius, static_cast<float>(width) / 2.0f, static_cast<float>(height) / 2.0f});
    if (r <= 0.5f) return;

    const int intR = static_cast<int>(std::ceil(r));
    const int bpp = (format == CapturePixelFormat::Bgra32) ? 4 : 3;

    auto applyCorner = [&](int startX, int endX, int startY, int endY, float centerX, float centerY) {
        for (int y = startY; y < endY; ++y) {
            uint8_t* row = data + y * stride;
            const float py = y + 0.5f;
            for (int x = startX; x < endX; ++x) {
                const float px = x + 0.5f;
                const float d = std::hypot(px - centerX, py - centerY);
                if (d > r - 0.5f) {
                    const float coverage = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
                    uint8_t* p = row + x * bpp;
                    if (d > r + 0.5f) {
                        p[0] = 0; p[1] = 0; p[2] = 0;
                        if (bpp == 4) p[3] = 0;
                    } else {
                        p[0] = static_cast<uint8_t>(p[0] * coverage);
                        p[1] = static_cast<uint8_t>(p[1] * coverage);
                        p[2] = static_cast<uint8_t>(p[2] * coverage);
                        if (bpp == 4) p[3] = static_cast<uint8_t>(p[3] * coverage);
                    }
                }
            }
        }
    };

    applyCorner(0, intR, 0, intR, r, r);
    applyCorner(width - intR, width, 0, intR, width - r, r);
    applyCorner(0, intR, height - intR, height, r, height - r);
    applyCorner(width - intR, width, height - intR, height, width - r, height - r);
}

namespace {

bool isOutsideRoundedCorner(int x, int y, int width, int height, float radius) noexcept {
    if (radius <= 0.5f || width <= 0 || height <= 0) return false;
    const float r = (std::min)({radius, static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f});
    if (r <= 0.5f) return false;
    const int intR = static_cast<int>(std::ceil(r));
    float cx = -1.0f;
    float cy = -1.0f;
    if (x < intR && y < intR) {
        cx = r; cy = r;
    } else if (x >= width - intR && y < intR) {
        cx = static_cast<float>(width) - r; cy = r;
    } else if (x < intR && y >= height - intR) {
        cx = r; cy = static_cast<float>(height) - r;
    } else if (x >= width - intR && y >= height - intR) {
        cx = static_cast<float>(width) - r; cy = static_cast<float>(height) - r;
    } else {
        return false;
    }
    const float dx = (static_cast<float>(x) + 0.5f) - cx;
    const float dy = (static_cast<float>(y) + 0.5f) - cy;
    return (dx * dx + dy * dy) > (r * r);
}

} // namespace

void GifPaletteOptimizer::updatePalette(const uint8_t* srcData, int width, int height, int stride, int bpp) {
    const bool useTransparency = reserveTransparent;
    const int maxOpaqueSlots = useTransparency ? 255 : 256;

    const int sampleStep = (static_cast<int64_t>(width) * static_cast<int64_t>(height) <= 65536LL) ? 1 : 2;
    std::vector<uint32_t> hist(32768, 0);
    if (srcData && width > 0 && height > 0) {
        for (int y = 0; y < height; y += sampleStep) {
            const uint8_t* row = srcData + static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < width; x += sampleStep) {
                if (useTransparency && isOutsideRoundedCorner(x, y, width, height, cornerRadius)) {
                    continue;
                }
                const uint8_t* px = row + static_cast<std::size_t>(x) * bpp;
                if (useTransparency && bpp == 4 && px[3] < 128) {
                    continue;
                }
                const int r5 = px[2] >> 3;
                const int g5 = px[1] >> 3;
                const int b5 = px[0] >> 3;
                hist[(r5 << 10) | (g5 << 5) | b5]++;
            }
        }
    }

    struct ColorSample {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint32_t count;
    };
    std::vector<ColorSample> activeColors;
    activeColors.reserve(4096);
    for (int c15 = 0; c15 < 32768; ++c15) {
        if (hist[c15] == 0) continue;
        const int r5 = (c15 >> 10) & 0x1F;
        const int g5 = (c15 >> 5) & 0x1F;
        const int b5 = c15 & 0x1F;
        activeColors.push_back({
            static_cast<uint8_t>((r5 << 3) | (r5 >> 2)),
            static_cast<uint8_t>((g5 << 3) | (g5 >> 2)),
            static_cast<uint8_t>((b5 << 3) | (b5 >> 2)),
            hist[c15]
        });
    }

    int numColors = 0;
    if (static_cast<int>(activeColors.size()) <= maxOpaqueSlots) {
        std::sort(activeColors.begin(), activeColors.end(),
                  [](const ColorSample& a, const ColorSample& b) { return a.count > b.count; });
        numColors = static_cast<int>(activeColors.size());
        for (int i = 0; i < numColors; ++i) {
            paletteR[i] = activeColors[i].r;
            paletteG[i] = activeColors[i].g;
            paletteB[i] = activeColors[i].b;
            palette[i] = (0xFFU << 24) |
                         (static_cast<uint32_t>(paletteR[i]) << 16) |
                         (static_cast<uint32_t>(paletteG[i]) << 8) |
                         static_cast<uint32_t>(paletteB[i]);
        }
    } else {
        // 3D RGB Median-Cut adaptive color quantization: preserves smooth gradients and UI nuances
        struct ColorBox {
            std::size_t begin = 0;
            std::size_t end = 0;
            uint64_t totalCount = 0;
            uint8_t minR = 255, maxR = 0;
            uint8_t minG = 255, maxG = 0;
            uint8_t minB = 255, maxB = 0;
        };

        auto recomputeBox = [&](ColorBox& box) {
            box.totalCount = 0;
            box.minR = box.minG = box.minB = 255;
            box.maxR = box.maxG = box.maxB = 0;
            for (std::size_t i = box.begin; i < box.end; ++i) {
                const auto& c = activeColors[i];
                box.totalCount += c.count;
                if (c.r < box.minR) box.minR = c.r;
                if (c.r > box.maxR) box.maxR = c.r;
                if (c.g < box.minG) box.minG = c.g;
                if (c.g > box.maxG) box.maxG = c.g;
                if (c.b < box.minB) box.minB = c.b;
                if (c.b > box.maxB) box.maxB = c.b;
            }
        };

        std::vector<ColorBox> boxes;
        boxes.reserve(static_cast<std::size_t>(maxOpaqueSlots));
        ColorBox root{0, activeColors.size()};
        recomputeBox(root);
        boxes.push_back(root);

        while (static_cast<int>(boxes.size()) < maxOpaqueSlots) {
            int bestIdx = -1;
            double bestScore = 0.0;
            for (std::size_t i = 0; i < boxes.size(); ++i) {
                const auto& b = boxes[i];
                if (b.end - b.begin <= 1) continue;
                const double spanR = static_cast<double>(b.maxR - b.minR);
                const double spanG = static_cast<double>(b.maxG - b.minG) * 1.15;
                const double spanB = static_cast<double>(b.maxB - b.minB) * 0.90;
                const double maxSpan = (std::max)({spanR, spanG, spanB});
                if (maxSpan <= 0.0) continue;
                const double weight = std::log2(2.0 + static_cast<double>(b.totalCount));
                const double score = maxSpan * weight;
                if (score > bestScore) {
                    bestScore = score;
                    bestIdx = static_cast<int>(i);
                }
            }
            if (bestIdx < 0) break;

            ColorBox target = boxes[static_cast<std::size_t>(bestIdx)];
            const int rangeR = target.maxR - target.minR;
            const int rangeG = target.maxG - target.minG;
            const int rangeB = target.maxB - target.minB;
            if (rangeG >= rangeR && rangeG >= rangeB) {
                std::sort(activeColors.begin() + static_cast<std::ptrdiff_t>(target.begin),
                          activeColors.begin() + static_cast<std::ptrdiff_t>(target.end),
                          [](const ColorSample& a, const ColorSample& b) { return a.g < b.g; });
            } else if (rangeR >= rangeB) {
                std::sort(activeColors.begin() + static_cast<std::ptrdiff_t>(target.begin),
                          activeColors.begin() + static_cast<std::ptrdiff_t>(target.end),
                          [](const ColorSample& a, const ColorSample& b) { return a.r < b.r; });
            } else {
                std::sort(activeColors.begin() + static_cast<std::ptrdiff_t>(target.begin),
                          activeColors.begin() + static_cast<std::ptrdiff_t>(target.end),
                          [](const ColorSample& a, const ColorSample& b) { return a.b < b.b; });
            }

            const uint64_t halfCount = target.totalCount / 2;
            uint64_t accum = 0;
            std::size_t split = target.begin + 1;
            for (std::size_t i = target.begin; i + 1 < target.end; ++i) {
                accum += activeColors[i].count;
                if (accum >= halfCount) {
                    split = i + 1;
                    break;
                }
            }
            ColorBox leftBox{target.begin, split};
            ColorBox rightBox{split, target.end};
            recomputeBox(leftBox);
            recomputeBox(rightBox);
            boxes[static_cast<std::size_t>(bestIdx)] = leftBox;
            boxes.push_back(rightBox);
        }

        numColors = (std::min)(maxOpaqueSlots, static_cast<int>(boxes.size()));
        for (int i = 0; i < numColors; ++i) {
            const auto& b = boxes[static_cast<std::size_t>(i)];
            uint64_t sumR = 0, sumG = 0, sumB = 0, total = 0;
            for (std::size_t k = b.begin; k < b.end; ++k) {
                const uint64_t w = activeColors[k].count;
                sumR += static_cast<uint64_t>(activeColors[k].r) * w;
                sumG += static_cast<uint64_t>(activeColors[k].g) * w;
                sumB += static_cast<uint64_t>(activeColors[k].b) * w;
                total += w;
            }
            const uint8_t cr = total > 0 ? static_cast<uint8_t>((sumR + total / 2) / total) : 0;
            const uint8_t cg = total > 0 ? static_cast<uint8_t>((sumG + total / 2) / total) : 0;
            const uint8_t cb = total > 0 ? static_cast<uint8_t>((sumB + total / 2) / total) : 0;
            paletteR[i] = cr;
            paletteG[i] = cg;
            paletteB[i] = cb;
            palette[i] = (0xFFU << 24) |
                         (static_cast<uint32_t>(cr) << 16) |
                         (static_cast<uint32_t>(cg) << 8) |
                         static_cast<uint32_t>(cb);
        }
    }

    const int remain = maxOpaqueSlots - numColors;
    for (int i = numColors; i < maxOpaqueSlots; ++i) {
        const uint8_t v = (remain > 1) ? static_cast<uint8_t>(((i - numColors) * 255) / (remain - 1)) : 0;
        paletteR[i] = v;
        paletteG[i] = v;
        paletteB[i] = v;
        palette[i] = (0xFFU << 24) | (static_cast<uint32_t>(v) << 16) | (static_cast<uint32_t>(v) << 8) | static_cast<uint32_t>(v);
    }
    if (useTransparency) {
        paletteR[255] = 0;
        paletteG[255] = 0;
        paletteB[255] = 0;
        palette[255] = 0x00000000U; // Reserved transparent color index for GIF Graphic Control Extension
    }

    validColorCount = (numColors > 0) ? numColors : maxOpaqueSlots;
    for (int c15 = 0; c15 < 32768; ++c15) {
        const int r5 = (c15 >> 10) & 0x1F;
        const int g5 = (c15 >> 5) & 0x1F;
        const int b5 = c15 & 0x1F;
        const int r = (r5 << 3) | (r5 >> 2);
        const int g = (g5 << 3) | (g5 >> 2);
        const int b = (b5 << 3) | (b5 >> 2);

        int bestIdx = 0;
        int bestDist = (std::numeric_limits<int>::max)();
        for (int i = 0; i < validColorCount; ++i) {
            const int dr = r - paletteR[i];
            const int dg = g - paletteG[i];
            const int db = b - paletteB[i];
            const int dist = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = i;
                if (dist == 0) break;
            }
        }
        colorMap[c15] = static_cast<uint8_t>(bestIdx);
    }
    initialized = true;
}

void GifPaletteOptimizer::quantizeFrame(const uint8_t* srcData, int width, int height, int stride, int bpp,
                                        uint8_t* dstData, int dstLinesize, uint32_t* dstPalette) {
    if (!srcData || !dstData || width <= 0 || height <= 0) return;

    const bool refreshPalette = !initialized || (++frameCounter % 30 == 0);
    if (refreshPalette) {
        updatePalette(srcData, width, height, stride, bpp);
    }
    if (dstPalette) {
        std::memcpy(dstPalette, palette, 256 * sizeof(uint32_t));
    }

    const bool useTransparency = reserveTransparent;
    const bool checkCorners = useTransparency && (cornerRadius > 0.5f);
    const bool useDiff = enableFrameDiff && useTransparency;
    const std::size_t compactBgrBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3ULL;

    bool hasPrevFrame = useDiff &&
                        (prevWidth == width) &&
                        (prevHeight == height) &&
                        (prevFrameBgr.size() == compactBgrBytes) &&
                        (frameCounter % 90 != 0);

    int dirtyL = 0;
    int dirtyT = 0;
    int dirtyR = width;
    int dirtyB = height;

    if (useDiff) {
        if (!hasPrevFrame) {
            prevFrameBgr.resize(compactBgrBytes);
            prevWidth = width;
            prevHeight = height;
            for (int y = 0; y < height; ++y) {
                const uint8_t* srcRow = srcData + static_cast<std::size_t>(y) * stride;
                uint8_t* histRow = prevFrameBgr.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3ULL;
                for (int x = 0; x < width; ++x) {
                    const uint8_t* px = srcRow + static_cast<std::size_t>(x) * bpp;
                    histRow[x * 3 + 0] = px[0];
                    histRow[x * 3 + 1] = px[1];
                    histRow[x * 3 + 2] = px[2];
                }
            }
        } else {
            dirtyL = width;
            dirtyT = height;
            dirtyR = 0;
            dirtyB = 0;
            for (int y = 0; y < height; ++y) {
                const uint8_t* srcRow = srcData + static_cast<std::size_t>(y) * stride;
                const uint8_t* histRow = prevFrameBgr.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3ULL;
                for (int x = 0; x < width; ++x) {
                    if (checkCorners && isOutsideRoundedCorner(x, y, width, height, cornerRadius)) {
                        continue;
                    }
                    const uint8_t* px = srcRow + static_cast<std::size_t>(x) * bpp;
                    if (bpp == 4 && px[3] < 128) {
                        continue;
                    }
                    const int db = std::abs(static_cast<int>(px[0]) - static_cast<int>(histRow[x * 3 + 0]));
                    const int dg = std::abs(static_cast<int>(px[1]) - static_cast<int>(histRow[x * 3 + 1]));
                    const int dr = std::abs(static_cast<int>(px[2]) - static_cast<int>(histRow[x * 3 + 2]));
                    if (db > 2 || dg > 2 || dr > 2) {
                        if (x < dirtyL) dirtyL = x;
                        if (x + 1 > dirtyR) dirtyR = x + 1;
                        if (y < dirtyT) dirtyT = y;
                        if (y + 1 > dirtyB) dirtyB = y + 1;
                    }
                }
            }
            if (dirtyL >= dirtyR || dirtyT >= dirtyB) {
                lastDirtyLeft = 0;
                lastDirtyTop = 0;
                lastDirtyRight = 0;
                lastDirtyBottom = 0;
                for (int y = 0; y < height; ++y) {
                    std::memset(dstData + static_cast<std::size_t>(y) * dstLinesize, 255, static_cast<std::size_t>(width));
                }
                return;
            }
        }
    } else if (!prevFrameBgr.empty()) {
        resetFrameHistory();
    }

    lastDirtyLeft = dirtyL;
    lastDirtyTop = dirtyT;
    lastDirtyRight = dirtyR;
    lastDirtyBottom = dirtyB;

    if (hasPrevFrame) {
        for (int y = 0; y < dirtyT; ++y) {
            std::memset(dstData + static_cast<std::size_t>(y) * dstLinesize, 255, static_cast<std::size_t>(width));
        }
        for (int y = dirtyB; y < height; ++y) {
            std::memset(dstData + static_cast<std::size_t>(y) * dstLinesize, 255, static_cast<std::size_t>(width));
        }
    }

    // Sierra-2-4A lightweight error diffusion dithering (exact zero error on solid colors; eliminates gradient banding)
    const std::size_t errStride = static_cast<std::size_t>(width) + 2ULL;
    std::vector<int16_t> errCurR, errCurG, errCurB;
    std::vector<int16_t> errNextR, errNextG, errNextB;
    if (enableDithering) {
        errCurR.assign(errStride, 0);
        errCurG.assign(errStride, 0);
        errCurB.assign(errStride, 0);
        errNextR.assign(errStride, 0);
        errNextG.assign(errStride, 0);
        errNextB.assign(errStride, 0);
    }

    for (int y = dirtyT; y < dirtyB; ++y) {
        const uint8_t* srcRow = srcData + static_cast<std::size_t>(y) * stride;
        uint8_t* dstRow = dstData + static_cast<std::size_t>(y) * dstLinesize;
        uint8_t* histRow = hasPrevFrame
            ? (prevFrameBgr.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3ULL)
            : nullptr;

        if (hasPrevFrame) {
            if (dirtyL > 0) {
                std::memset(dstRow, 255, static_cast<std::size_t>(dirtyL));
            }
            if (dirtyR < width) {
                std::memset(dstRow + dirtyR, 255, static_cast<std::size_t>(width - dirtyR));
            }
        }

        if (enableDithering) {
            std::fill(errNextR.begin(), errNextR.end(), int16_t{0});
            std::fill(errNextG.begin(), errNextG.end(), int16_t{0});
            std::fill(errNextB.begin(), errNextB.end(), int16_t{0});
        }

        for (int x = dirtyL; x < dirtyR; ++x) {
            if (checkCorners && isOutsideRoundedCorner(x, y, width, height, cornerRadius)) {
                dstRow[x] = 255;
                continue;
            }
            const uint8_t* px = srcRow + static_cast<std::size_t>(x) * bpp;
            if (useTransparency && bpp == 4 && px[3] < 128) {
                dstRow[x] = 255;
                continue;
            }
            if (histRow) {
                const int db = std::abs(static_cast<int>(px[0]) - static_cast<int>(histRow[x * 3 + 0]));
                const int dg = std::abs(static_cast<int>(px[1]) - static_cast<int>(histRow[x * 3 + 1]));
                const int dr = std::abs(static_cast<int>(px[2]) - static_cast<int>(histRow[x * 3 + 2]));
                if (db <= 2 && dg <= 2 && dr <= 2) {
                    dstRow[x] = 255;
                    continue;
                }
                histRow[x * 3 + 0] = px[0];
                histRow[x * 3 + 1] = px[1];
                histRow[x * 3 + 2] = px[2];
            }

            int r = px[2];
            int g = px[1];
            int b = px[0];
            if (enableDithering) {
                const std::size_t ex = static_cast<std::size_t>(x) + 1ULL;
                r = std::clamp(r + std::clamp<int>(errCurR[ex], -20, 20), 0, 255);
                g = std::clamp(g + std::clamp<int>(errCurG[ex], -20, 20), 0, 255);
                b = std::clamp(b + std::clamp<int>(errCurB[ex], -20, 20), 0, 255);
            }

            const int c15 = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
            const uint8_t idx = colorMap[c15];
            dstRow[x] = idx;

            if (enableDithering) {
                const int er = r - static_cast<int>(paletteR[idx]);
                const int eg = g - static_cast<int>(paletteG[idx]);
                const int eb = b - static_cast<int>(paletteB[idx]);
                if ((er | eg | eb) != 0) {
                    const std::size_t ex = static_cast<std::size_t>(x) + 1ULL;
                    // Sierra-2-4A: 2/4 right, 1/4 bottom-left, 1/4 bottom
                    errCurR[ex + 1] = static_cast<int16_t>(errCurR[ex + 1] + (er >> 1));
                    errCurG[ex + 1] = static_cast<int16_t>(errCurG[ex + 1] + (eg >> 1));
                    errCurB[ex + 1] = static_cast<int16_t>(errCurB[ex + 1] + (eb >> 1));

                    errNextR[ex - 1] = static_cast<int16_t>(errNextR[ex - 1] + (er >> 2));
                    errNextG[ex - 1] = static_cast<int16_t>(errNextG[ex - 1] + (eg >> 2));
                    errNextB[ex - 1] = static_cast<int16_t>(errNextB[ex - 1] + (eb >> 2));

                    errNextR[ex] = static_cast<int16_t>(errNextR[ex] + (er >> 2));
                    errNextG[ex] = static_cast<int16_t>(errNextG[ex] + (eg >> 2));
                    errNextB[ex] = static_cast<int16_t>(errNextB[ex] + (eb >> 2));
                }
            }
        }

        if (enableDithering) {
            errCurR.swap(errNextR);
            errCurG.swap(errNextG);
            errCurB.swap(errNextB);
        }
    }
}

static std::atomic<std::uint64_t> s_maxGifBytesForTesting{0};

void ScreenRecorder::setMaxGifBytesForTesting(std::uint64_t bytes) {
    s_maxGifBytesForTesting.store(bytes);
}

std::uint64_t ScreenRecorder::maxGifBytesForTesting() {
    return s_maxGifBytesForTesting.load();
}

ScreenRecorder& ScreenRecorder::instance() {
    static ScreenRecorder inst;
    return inst;
}

bool ScreenRecorder::initialize() {
    LOG_INFO("屏幕录制引擎已初始化");
    return true;
}

void ScreenRecorder::shutdown() {
    waitForAsyncStop();
    stopRecording();
    reapFinishedSnapshots(true);
    LOG_INFO("屏幕录制引擎已关闭");
}

// ─────────────────────────────────────────────────────────────────────────────
// 录制控制
// ─────────────────────────────────────────────────────────────────────────────

bool ScreenRecorder::startRecording(const RecordOptions& options) {
    waitForAsyncStop();
    std::lock_guard controlLock(m_controlMutex);
    if (m_state != RecordState::Idle || m_stopping.load(std::memory_order_acquire)) {
        LOG_WARN("已在录制中, 请先停止当前录制");
        return false;
    }

    reapFinishedSnapshots(false);

    // 上一次若因捕获/编码错误自行退出，线程仍需 join，编码器也需封尾清理。
    if (m_recordThread.joinable() && std::this_thread::get_id() != m_recordThread.get_id()) {
        m_recordThread.join();
        finalizeEncoder();
        commitOutputFile(stats().frameCount > 0);
    }

    tools3000::core::TraceId::Scope scope;
    m_options = options;
    {
        std::lock_guard lock(m_statsMutex);
        m_stats = {};
    }
    m_frameIndex = 0;
    m_audioFrameIndex = 0;
    m_accountedDroppedAudioFrames = 0;
    m_hasEncodedVideoFrame = false;
    m_audioEncodingFailed = false;
    m_lastFrameTimings = {};
    m_adaptiveFrameStep = 1;
    m_overloadScore = 0;
    m_recoveryScore = 0;

    // 自动检测屏幕尺寸
    if (options.fps < 1 || options.fps > 120 ||
        options.bitrateMbps < 1 || options.bitrateMbps > 100 ||
        options.countdownSeconds < 0 || options.countdownSeconds > 10) {
        LOG_ERROR("录屏参数无效: fps={}, bitrate={}, countdown={}",
                  options.fps, options.bitrateMbps, options.countdownSeconds);
        return false;
    }

    if (options.fullScreen || options.width <= 0 || options.height <= 0) {
        m_options.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        m_options.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        m_options.regionX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        m_options.regionY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    }

    // 确保宽高为偶数（编码器要求）
    m_options.width &= ~1;
    m_options.height &= ~1;
    if (m_options.width <= 0 || m_options.height <= 0 ||
        m_options.width > 32768 || m_options.height > 32768) {
        LOG_ERROR("录屏区域无效: {}x{}", m_options.width, m_options.height);
        return false;
    }

    // 生成输出文件路径
    m_outputPath = options.outputPath.empty()
        ? generateOutputPath(options.format)
        : options.outputPath;
    m_workingOutputPath = m_outputPath + ".partial";

    // 确保输出目录存在
    auto dir = outputDirectory(m_outputPath);
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::lock_guard lock(m_statsMutex);
            m_stats.stopReason = "output_directory_unavailable";
            LOG_ERROR("无法创建录屏输出目录: path={}, error={}", m_outputPath, ec.message());
            m_workingOutputPath.clear();
            return false;
        }
    }
    {
        std::error_code ec;
        std::filesystem::remove(
            std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(m_workingOutputPath)), ec);
    }
    if (!preflightOutputStorage()) {
        commitOutputFile(false);
        return false;
    }

    // 音频设备失败只关闭对应音轨，视频录制仍然可以开始。
    const bool wantsAudio = m_options.captureSystemAudio || m_options.captureMicrophone;
    if (wantsAudio && m_options.format != RecordFormat::GIF) {
        const AudioCaptureOptions audioOptions{
            m_options.captureSystemAudio, m_options.captureMicrophone,
            m_options.systemAudioDeviceId, m_options.microphoneDeviceId,
            std::clamp(m_options.systemAudioVolume, 0.0f, 2.0f),
            std::clamp(m_options.microphoneVolume, 0.0f, 2.0f)
        };
        m_audioCapture.start(audioOptions);
        const auto audioStatus = m_audioCapture.status();
        // Hold audio producer paused while encoder and capture backend initialize so zero stale
        // samples accumulate before the first video frame is actually ready to be captured.
        m_audioCapture.setPaused(true);
        m_options.captureSystemAudio = audioStatus.systemAudioActive;
        m_options.captureMicrophone = audioStatus.microphoneActive;
        std::lock_guard lock(m_statsMutex);
        m_stats.systemAudioActive = audioStatus.systemAudioActive;
        m_stats.microphoneActive = audioStatus.microphoneActive;
        m_stats.audioError = audioStatus.error;
    } else if (wantsAudio) {
        std::lock_guard lock(m_statsMutex);
        m_stats.audioError = "GIF does not support an audio track";
        m_options.captureSystemAudio = false;
        m_options.captureMicrophone = false;
    }

    // 初始化编码器
    if (!initializeEncoder(m_options)) {
        LOG_ERROR("FFmpeg 编码器初始化失败");
        m_audioCapture.stop();
        cleanupEncoder();
        commitOutputFile(false);
        return false;
    }
    m_finalizePending = true;
    m_skipCountdown.store(false);

    const bool countingDown = m_options.countdownSeconds > 0;
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.countdownRemaining = countingDown ? m_options.countdownSeconds : 0;
    }
    m_state = countingDown ? RecordState::Countdown : RecordState::Recording;

    // 启动录制线程
    m_recordThread = std::thread([this]() {
        recordingLoop();
    });

    LOG_INFO("屏幕录制已准备: {}x{} @ {}fps, countdown={}s, format={}, output={}",
             m_options.width, m_options.height, m_options.fps,
             m_options.countdownSeconds,
             formatExtension(m_options.format), m_outputPath);

    notifyState(m_state.load());

    return true;
}

void ScreenRecorder::pauseRecording() {
    std::lock_guard controlLock(m_controlMutex);
    if (m_state == RecordState::Recording) {
        // Publish Paused only after the audio producer is paused and its queued
        // samples are cleared. A recorder thread that observes Paused can then
        // never see the old audio state from the transition window.
        m_audioCapture.setPaused(true);
        {
            std::lock_guard lock(m_statsMutex);
            m_stats.systemAudioPeak = 0.0f;
            m_stats.microphonePeak = 0.0f;
            m_stats.mixedAudioPeak = 0.0f;
        }
        m_state = RecordState::Paused;
        LOG_INFO("屏幕录制已暂停, frames={}", stats().frameCount);
        notifyState(RecordState::Paused);
    }
}

void ScreenRecorder::resumeRecording() {
    std::lock_guard controlLock(m_controlMutex);
    if (m_state == RecordState::Paused) {
        m_audioCapture.resetAndResumeForSyncStart();
        m_state = RecordState::Recording;
        LOG_INFO("屏幕录制已恢复");
        notifyState(RecordState::Recording);
    }
}

void ScreenRecorder::skipCountdown() {
    m_skipCountdown.store(true);
}

void ScreenRecorder::requestSnapshot() {
    if (m_state == RecordState::Recording || m_state == RecordState::Paused) {
        m_snapshotRequested.store(true);
    }
}

void ScreenRecorder::waitForAsyncStop() {
    std::thread workerToJoin;
    {
        std::lock_guard lock(m_stopThreadMutex);
        if (m_stopThread.joinable() && std::this_thread::get_id() != m_stopThread.get_id()) {
            workerToJoin = std::move(m_stopThread);
        }
    }
    if (workerToJoin.joinable()) {
        workerToJoin.join();
    }
}

bool ScreenRecorder::stopRecordingAsync(AsyncStopCallback onComplete) {
    bool expectedStopping = false;
    if (!m_stopping.compare_exchange_strong(expectedStopping, true, std::memory_order_acq_rel)) {
        LOG_INFO("ScreenRecorder::stopRecordingAsync ignored because finalization is already in progress");
        return false;
    }

    if (m_state.load(std::memory_order_acquire) == RecordState::Idle &&
        !m_finalizePending.load(std::memory_order_acquire) &&
        !m_recordThread.joinable()) {
        m_stopping.store(false, std::memory_order_release);
        if (onComplete) {
            onComplete({}, stats());
        }
        return false;
    }

    // 立即发出停止信号让录制循环在下一拍跳出，而耗时 join + 编码冲刷 + 原子重命名移交后台收尾线程
    m_state.store(RecordState::Idle, std::memory_order_release);

    waitForAsyncStop();
    try {
        std::lock_guard lock(m_stopThreadMutex);
        m_stopThread = std::thread([this, cb = std::move(onComplete)]() mutable {
            tools3000::core::TraceId::Scope traceScope;
            std::string savedPath;
            RecordStats finalStats{};
            try {
                savedPath = stopRecording();
                finalStats = stats();
            } catch (const std::exception& ex) {
                LOG_ERROR("Async recording finalization failed: {}", ex.what());
            } catch (...) {
                LOG_ERROR("Async recording finalization encountered an unknown error");
            }
            m_lastAsyncCompletedPath = savedPath;
            m_stopping.store(false, std::memory_order_release);
            if (cb) {
                cb(savedPath, finalStats);
            }
        });
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("Failed to spawn async recording stop worker: {}", ex.what());
        m_stopping.store(false, std::memory_order_release);
        const std::string fallbackPath = stopRecording();
        if (onComplete) {
            onComplete(fallbackPath, stats());
        }
        return true;
    }
}

std::string ScreenRecorder::stopRecording() {
    // 若由外部线程调用同步 stopRecording() 且已有异步收尾线程正在运行，等待其完成并返回结果
    bool inAsyncStopThread = false;
    {
        std::lock_guard lock(m_stopThreadMutex);
        inAsyncStopThread = (m_stopThread.joinable() && std::this_thread::get_id() == m_stopThread.get_id());
    }
    if (!inAsyncStopThread && m_stopping.load(std::memory_order_acquire)) {
        waitForAsyncStop();
        return m_lastAsyncCompletedPath;
    }

    std::lock_guard controlLock(m_controlMutex);
    if (m_state == RecordState::Idle && !m_recordThread.joinable() && !m_finalizePending.load()) return "";

    tools3000::core::TraceId::Scope scope;
    m_state = RecordState::Idle;

    // 等待录制线程结束（避免在工作线程自身中 join 产生死锁）
    if (m_recordThread.joinable() && std::this_thread::get_id() != m_recordThread.get_id()) {
        m_recordThread.join();
    }

    const bool finalized = finalizeEncoder();

    const auto finalStats = stats();
    std::string completedPath;
    if (finalStats.frameCount == 0) {
        // 无有效帧时清理空临时文件
        commitOutputFile(false);
    } else if (finalized) {
        if (commitOutputFile(true)) {
            completedPath = m_outputPath;
        } else {
            LOG_WARN("录屏结束封装或原子重命名未完全成功，保留临时文件供手动恢复: {}", m_workingOutputPath);
        }
    } else {
        // 封装或关闭失败但存在有效帧时，保留临时文件供手动恢复，严防数据丢失 (A05)
        LOG_WARN("录屏结束封装或原子重命名未完全成功，保留临时文件供手动恢复: {}", m_workingOutputPath);
    }

    LOG_INFO("屏幕录制已停止: frames={}, dropped={}, duration={:.1f}s, output={}",
             finalStats.frameCount, finalStats.droppedFrameCount,
             finalStats.durationSec, completedPath);

    notifyState(RecordState::Idle);

    tools3000::core::WinUtils::trimWorkingSet();

    return completedPath;
}

// ─────────────────────────────────────────────────────────────────────────────
// 录制线程
// ─────────────────────────────────────────────────────────────────────────────

void ScreenRecorder::recordingLoop() {
    using clock = std::chrono::steady_clock;
    const auto frameDuration = std::chrono::nanoseconds(1'000'000'000LL / m_options.fps);
    auto nextFrameAt = clock::now();
    auto nextStorageCheck = clock::now() + std::chrono::seconds(1);
    auto nextTelemetryAt = clock::now() + std::chrono::seconds(1);
    bool wasPaused = false;
    const auto emitPipelineMetrics = [this]() {
        const auto snapshot = stats();
        if (snapshot.frameCount <= 0) return;
        auto& monitor = tools3000::core::PerformanceMonitor::instance();
        monitor.recordLatency("recording.capture", snapshot.captureLatencyMs);
        monitor.recordLatency("recording.conversion", snapshot.conversionLatencyMs);
        monitor.recordLatency("recording.encode", snapshot.encodeLatencyMs);
        monitor.recordLatency("recording.pipeline", snapshot.pipelineLatencyMs);
        monitor.recordCounter("recording.outputFrames",
                              static_cast<std::uint64_t>(snapshot.frameCount));
        monitor.recordCounter("recording.droppedFrames",
                              static_cast<std::uint64_t>(snapshot.droppedFrameCount));
    };

    // Probe only on the recorder-owned worker and only for the explicit
    // experiment flag. The result is diagnostics for a future backend; this
    // recording session remains on the initialized CPU conversion/FFmpeg path.
    if (m_options.experimentalGpuEncoding) {
        const auto probe = probeRecordingGpuPath();
        {
            std::lock_guard lock(m_statsMutex);
            m_stats.gpuExperimentRequested = true;
            m_stats.gpuExperimentAvailable = probe.canExperiment();
            m_stats.gpuExperimentStatus = probe.canExperiment()
                ? "capability detected; CPU fallback remains active"
                : (!probe.d3d11Error.empty() ? probe.d3d11Error
                   : (!probe.mediaFoundationError.empty() ? probe.mediaFoundationError
                      : probe.ffmpegError));
        }
        LOG_INFO("GPU encoding experiment probe: d3d11={}, featureLevel=0x{:X}, mfMft={}, ffmpegD3d11va={}; CPU pipeline retained",
                 probe.d3d11Available, probe.d3d11FeatureLevel,
                 probe.mediaFoundationHardwareMftAvailable, probe.ffmpegD3d11vaCompiled);
    }

    // 线程入口兜底: 任何 std 异常(如全屏帧缓冲 std::bad_alloc、日志 std::format 抛错)若逃逸出
    // 线程函数会直接 std::terminate 整个进程。这里全程兜底, 异常时干净停止录制。
    try {
        if (m_state == RecordState::Countdown) {
            const auto deadline = clock::now() + std::chrono::seconds(m_options.countdownSeconds);
            while (m_state == RecordState::Countdown) {
                if (m_skipCountdown.load() || (GetAsyncKeyState(VK_SPACE) & 0x8000)) {
                    m_skipCountdown.store(true);
                    break;
                }
                const auto now = clock::now();
                if (now >= deadline) break;
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now).count();
                {
                    std::lock_guard lock(m_statsMutex);
                    m_stats.countdownRemaining = static_cast<int>((remaining + 999) / 1000);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (m_state == RecordState::Countdown) {
                {
                    std::lock_guard lock(m_statsMutex);
                    m_stats.countdownRemaining = 0;
                }
                m_state = RecordState::Recording;
                notifyState(RecordState::Recording);
            }
        }
        if (m_state != RecordState::Idle && !initializeCaptureResources()) {
            LOG_ERROR("录屏捕获资源初始化失败");
            {
                std::lock_guard lock(m_statsMutex);
                m_stats.stopReason = "capture_initialization_failed";
            }
            m_state = RecordState::Idle;
        }
        if (m_state != RecordState::Idle) {
            m_audioFrameIndex = 0;
            m_accountedDroppedAudioFrames = 0;
            if (m_state == RecordState::Recording) {
                m_audioCapture.resetAndResumeForSyncStart();
            }
            nextFrameAt = clock::now();
        }
        while (m_state != RecordState::Idle) {
            if (m_state == RecordState::Paused) {
                wasPaused = true;
                if (m_snapshotRequested.exchange(false) && m_captureBackend) {
                    CaptureFrameView captured;
                    std::string captureError;
                    if (m_captureBackend->capture(captured, captureError)) {
                        [[maybe_unused]] auto cursorPatch = m_cursorOverlay.apply(
                            captured,
                            {m_options.regionX, m_options.regionY, m_options.width, m_options.height},
                            m_options.includeCursor, m_options.showClickEffects, m_options.showClickZoom);
                        if (m_options.cornerRadius > 0.5f) {
                            applyVideoFrameCornerMask(captured.data, m_options.width, m_options.height, captured.stride, captured.format, m_options.cornerRadius);
                        }
                        dispatchSnapshotSave(captured.data, m_options.width, m_options.height, captured.stride, captured.format);
                        m_captureBackend->releaseFrame();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (wasPaused) {
                // 暂停时间不计入视频时间线，恢复后从“现在”继续。
                nextFrameAt = clock::now();
                wasPaused = false;
            }

            auto now = clock::now();
            if (now < nextFrameAt) {
                std::this_thread::sleep_until(nextFrameAt);
                now = clock::now();
            }

            // 捕获或编码比目标帧周期慢时，跳过已经错过的时间槽并让 PTS 留出
            // 对应间隔。这样不会形成无界帧队列，也不会让输出视频加速播放。
            int skipped = 0;
            if (now > nextFrameAt + frameDuration) {
                skipped = static_cast<int>((now - nextFrameAt) / frameDuration);
                m_frameIndex += skipped;
                nextFrameAt += frameDuration * skipped;
            }

            if (!captureAndEncodeFrame()) {
                LOG_ERROR("帧捕获/编码失败, 停止录制");
                {
                    std::lock_guard lock(m_statsMutex);
                    m_stats.stopReason = "capture_or_encode_failed";
                }
                m_state = RecordState::Idle;
                break;
            }
            if (!encodeAvailableAudio()) {
                LOG_WARN("音频编码失败, 已降级为仅视频录制");
            }
            nextFrameAt += frameDuration;
            {
                std::lock_guard lock(m_statsMutex);
                ++m_stats.frameCount;
                const double budgetMs = 1000.0 / m_options.fps;
                const bool overloadedSample = skipped > 0 ||
                    m_lastFrameTimings.totalMs > budgetMs * 0.90;
                if (overloadedSample) {
                    m_overloadScore = std::min(m_options.fps * 4,
                        m_overloadScore + std::max(1, skipped));
                    m_recoveryScore = 0;
                } else if (m_lastFrameTimings.totalMs < budgetMs * 0.65) {
                    m_overloadScore = std::max(0, m_overloadScore - 1);
                    ++m_recoveryScore;
                } else {
                    m_recoveryScore = 0;
                }

                const int overloadThreshold = std::max(8, m_options.fps / 2);
                if (m_overloadScore >= overloadThreshold && m_adaptiveFrameStep < 4) {
                    m_adaptiveFrameStep *= 2;
                    m_overloadScore = 0;
                    m_recoveryScore = 0;
                    LOG_WARN("录屏持续过载，自适应帧步长调整为 {}", m_adaptiveFrameStep);
                } else {
                    const int recoveryThreshold = std::max(30,
                        m_options.fps * 3 / m_adaptiveFrameStep);
                    if (m_adaptiveFrameStep > 1 && m_recoveryScore >= recoveryThreshold) {
                        m_adaptiveFrameStep /= 2;
                        m_recoveryScore = 0;
                        LOG_INFO("录屏负载已恢复，自适应帧步长调整为 {}", m_adaptiveFrameStep);
                    }
                }

                const int adaptiveSkipped = m_adaptiveFrameStep - 1;
                m_frameIndex += adaptiveSkipped;
                nextFrameAt += frameDuration * adaptiveSkipped;
                m_stats.droppedFrameCount += skipped + adaptiveSkipped;
                m_stats.durationSec = static_cast<double>(m_frameIndex) / m_options.fps;
                const auto smooth = [](double previous, double current) {
                    return previous <= 0.0 ? current : previous * 0.88 + current * 0.12;
                };
                m_stats.captureLatencyMs = smooth(
                    m_stats.captureLatencyMs, m_lastFrameTimings.captureMs);
                m_stats.conversionLatencyMs = smooth(
                    m_stats.conversionLatencyMs, m_lastFrameTimings.conversionMs);
                m_stats.encodeLatencyMs = smooth(
                    m_stats.encodeLatencyMs, m_lastFrameTimings.encodeMs);
                m_stats.pipelineLatencyMs = smooth(
                    m_stats.pipelineLatencyMs, m_lastFrameTimings.totalMs);
                m_stats.effectiveFps = m_stats.durationSec > 0.0
                    ? m_stats.frameCount / m_stats.durationSec : 0.0;
                m_stats.adaptiveFrameStep = m_adaptiveFrameStep;
                m_stats.performanceLimited = m_adaptiveFrameStep > 1 ||
                    m_overloadScore >= overloadThreshold / 2;
            }
            if (clock::now() >= nextStorageCheck) {
                if (!updateOutputStorageStatus()) {
                    LOG_ERROR("录屏因磁盘安全余量不足而停止");
                    {
                        std::lock_guard lock(m_statsMutex);
                        m_stats.stopReason = "low_disk_space";
                    }
                    m_state = RecordState::Idle;
                    break;
                }
                if (m_options.format == RecordFormat::GIF && !m_workingOutputPath.empty()) {
                    std::error_code ec;
                    auto curBytes = std::filesystem::file_size(
                        std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(m_workingOutputPath)), ec);
                    const uint64_t testLimit = maxGifBytesForTesting();
                    const uint64_t limitBytes = (testLimit > 0) ? testLimit : (20ULL * 1024 * 1024 - 512 * 1024);
                    if (!ec && curBytes >= limitBytes) {
                        LOG_INFO("GIF reached WeChat 20MB limit, safely finishing recording");
                        {
                            std::lock_guard lock(m_statsMutex);
                            m_stats.stopReason = "gif_wechat_limit";
                        }
                        m_state = RecordState::Idle;
                        break;
                    }
                }
                nextStorageCheck = clock::now() + std::chrono::seconds(1);
            }
            if (clock::now() >= nextTelemetryAt) {
                emitPipelineMetrics();
                nextTelemetryAt = clock::now() + std::chrono::seconds(1);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("录制线程异常, 已停止录制: {}", e.what());
        {
            std::lock_guard lock(m_statsMutex);
            m_stats.stopReason = "recording_thread_exception";
        }
        m_state = RecordState::Idle;
    } catch (...) {
        LOG_ERROR("录制线程未知异常, 已停止录制");
        {
            std::lock_guard lock(m_statsMutex);
            m_stats.stopReason = "recording_thread_exception";
        }
        m_state = RecordState::Idle;
    }

    emitPipelineMetrics();
    cleanupCaptureResources();

    if (m_state == RecordState::Idle) notifyState(RecordState::Idle);
}

void ScreenRecorder::setAudioVolumes(float systemVolume, float microphoneVolume) {
    m_audioCapture.setVolumes(systemVolume, microphoneVolume);
}

bool ScreenRecorder::toggleSystemAudioMuted() {
    bool muted = false;
    {
        std::lock_guard lock(m_statsMutex);
        if (!m_stats.systemAudioActive || m_state == RecordState::Idle) return false;
        muted = !m_stats.systemAudioMuted;
    }
    m_audioCapture.setSystemMuted(muted);
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.systemAudioMuted = muted;
        if (muted) m_stats.systemAudioPeak = 0.0f;
    }
    return true;
}

bool ScreenRecorder::toggleMicrophoneMuted() {
    bool muted = false;
    {
        std::lock_guard lock(m_statsMutex);
        if (!m_stats.microphoneActive || m_state == RecordState::Idle) return false;
        muted = !m_stats.microphoneMuted;
    }
    m_audioCapture.setMicrophoneMuted(muted);
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.microphoneMuted = muted;
        if (muted) m_stats.microphonePeak = 0.0f;
    }
    return true;
}

RecordStats ScreenRecorder::stats() const {
    std::lock_guard lock(m_statsMutex);
    return m_stats;
}

RecordingCapabilities ScreenRecorder::capabilities() {
    RecordingCapabilities result;
    result.captureBackends = captureBackendCapabilities();
    result.audioDevices = AudioCapture::devices();
    for (const auto format : {RecordFormat::MP4_H264, RecordFormat::MP4_H265,
                              RecordFormat::WebM_VP9, RecordFormat::GIF}) {
        for (const auto* codec : encoderCandidates(format)) {
            result.encoders.push_back({formatId(format), codec->name,
                                       isHardwareEncoder(codec->name)});
        }
    }
    return result;
}

void ScreenRecorder::setStateCallback(RecordStateCallback callback) {
    std::lock_guard lock(m_callbackMutex);
    m_stateCallback = std::move(callback);
}

void ScreenRecorder::notifyState(RecordState state) {
    RecordStateCallback callback;
    {
        std::lock_guard lock(m_callbackMutex);
        callback = m_stateCallback;
    }
    if (callback) callback(state, stats());
}

bool ScreenRecorder::finalizeEncoder() {
    auto* codecCtx = static_cast<AVCodecContext*>(m_codecCtx);
    auto* audioCodecCtx = static_cast<AVCodecContext*>(m_audioCodecCtx);
    auto* fmtCtx = static_cast<AVFormatContext*>(m_fmtCtx);
    m_audioCapture.stop();
    encodeAvailableAudio();
    bool success = true;
    if (codecCtx && m_headerWritten) {
        if (avcodec_send_frame(codecCtx, nullptr) >= 0) {
            if (!writeAvailablePackets()) success = false;
        } else {
            success = false;
        }
    }
    if (audioCodecCtx && m_headerWritten) {
        if (avcodec_send_frame(audioCodecCtx, nullptr) >= 0) {
            if (!writeAvailableAudioPackets()) success = false;
        } else {
            success = false;
        }
    }
    if (fmtCtx && m_headerWritten) {
        const int trailerRet = av_write_trailer(fmtCtx);
        if (trailerRet < 0) {
            LOG_ERROR("av_write_trailer 写入封装尾部失败: error={}", trailerRet);
            success = false;
        }
    }
    bool closeOk = true;
    cleanupEncoder(closeOk);
    if (!closeOk) success = false;
    m_finalizePending = false;
    return success;
}

bool ScreenRecorder::commitOutputFile(bool keepRecording) {
    if (m_workingOutputPath.empty()) return false;
    const auto working = std::filesystem::path(
        tools3000::core::WinUtils::utf8ToWstring(m_workingOutputPath));
    const auto destination = std::filesystem::path(
        tools3000::core::WinUtils::utf8ToWstring(m_outputPath));
    std::error_code ec;
    if (!keepRecording) {
        std::filesystem::remove(working, ec);
        m_workingOutputPath.clear();
        return false;
    }
    if (!std::filesystem::exists(working, ec) || ec) {
        LOG_ERROR("录屏临时文件不存在, 无法提交: {}", m_workingOutputPath);
        return false;
    }
    if (!MoveFileExW(working.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOG_ERROR("录屏文件原子提交失败: error={}, temporary={}",
                  GetLastError(), m_workingOutputPath);
        return false;
    }
    const auto size = std::filesystem::file_size(destination, ec);
    if (!ec) {
        std::lock_guard lock(m_statsMutex);
        m_stats.fileSizeBytes = static_cast<int64_t>(size);
    }
    m_workingOutputPath.clear();
    return true;
}

std::uint64_t ScreenRecorder::estimatedBytesPerSecond() const {
    const std::uint64_t video = static_cast<std::uint64_t>(
        std::clamp(m_options.bitrateMbps, 1, 100)) * 1'000'000ULL / 8;
    const std::uint64_t audio = (m_options.captureSystemAudio || m_options.captureMicrophone)
        ? 192'000ULL / 8 : 0;
    return std::max<std::uint64_t>(video + audio, 1);
}

bool ScreenRecorder::preflightOutputStorage() {
    const auto directory = outputDirectory(m_outputPath);
    DWORD writeError = ERROR_SUCCESS;
    if (directory.empty() || !probeDirectoryWritable(directory, writeError)) {
        std::lock_guard lock(m_statsMutex);
        m_stats.stopReason = "output_directory_not_writable";
        LOG_ERROR("录屏输出目录不可写: path={}, error={}", m_outputPath, writeError);
        return false;
    }

    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(directory.c_str(), &available, nullptr, nullptr)) {
        std::lock_guard lock(m_statsMutex);
        m_stats.stopReason = "disk_space_query_failed";
        LOG_ERROR("无法查询录屏目录剩余空间: path={}, error={}", m_outputPath, GetLastError());
        return false;
    }
    const auto bytesPerSecond = estimatedBytesPerSecond();
    const auto minimum = StorageReserveBytes + bytesPerSecond * 10;
    const auto remaining = available.QuadPart > StorageReserveBytes
        ? static_cast<std::int64_t>((available.QuadPart - StorageReserveBytes) / bytesPerSecond)
        : 0;
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.diskFreeBytes = available.QuadPart;
        m_stats.estimatedRemainingSec = remaining;
        m_stats.storageWarning = remaining <= StorageWarningSeconds;
        if (available.QuadPart < minimum) m_stats.stopReason = "insufficient_disk_space";
    }
    if (available.QuadPart < minimum) {
        LOG_ERROR("录屏剩余空间不足: available={}, required={}",
                  available.QuadPart, minimum);
        return false;
    }
    return true;
}

bool ScreenRecorder::updateOutputStorageStatus() {
    const auto directory = outputDirectory(m_outputPath);
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(directory.c_str(), &available, nullptr, nullptr)) {
        std::lock_guard lock(m_statsMutex);
        m_stats.stopReason = "disk_space_query_failed";
        return false;
    }
    const auto bytesPerSecond = estimatedBytesPerSecond();
    const auto remaining = available.QuadPart > StorageReserveBytes
        ? static_cast<std::int64_t>((available.QuadPart - StorageReserveBytes) / bytesPerSecond)
        : 0;
    std::lock_guard lock(m_statsMutex);
    m_stats.diskFreeBytes = available.QuadPart;
    m_stats.estimatedRemainingSec = remaining;
    m_stats.storageWarning = remaining <= StorageWarningSeconds;
    if (available.QuadPart <= StorageReserveBytes) {
        m_stats.stopReason = "low_disk_space";
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FFmpeg 编码器
// ─────────────────────────────────────────────────────────────────────────────

bool ScreenRecorder::initializeEncoder(const RecordOptions& options) {
    const char* formatName = nullptr;

    switch (options.format) {
        case RecordFormat::MP4_H264:
            formatName = "mp4";
            break;
        case RecordFormat::MP4_H265:
            formatName = "mp4";
            break;
        case RecordFormat::WebM_VP9:
            formatName = "webm";
            break;
        case RecordFormat::GIF:
            formatName = "gif";
            break;
    }

    const auto candidates = encoderCandidates(options.format);
    if (candidates.empty()) {
        LOG_ERROR("未找到编码器: format={}", formatExtension(options.format));
        return false;
    }

    // 创建输出上下文
    AVFormatContext* fmtCtx = nullptr;
    int ret = avformat_alloc_output_context2(
        &fmtCtx, nullptr, formatName, m_workingOutputPath.c_str());
    if (ret < 0 || !fmtCtx) {
        LOG_ERROR("avformat_alloc_output_context2 失败");
        return false;
    }
    m_fmtCtx = fmtCtx;

    // 不同机器上的 FFmpeg 可能提供 libx264、Media Foundation 或显卡编码器。
    // 逐个尝试，并依据编码器实际能力选择像素格式，避免固定 YUV420P 导致 EINVAL。
    const AVCodec* codec = nullptr;
    AVCodecContext* codecCtx = nullptr;
    for (const auto* candidate : candidates) {
        AVCodecContext* attempt = avcodec_alloc_context3(candidate);
        if (!attempt) continue;
        const bool mediaFoundation = std::strstr(candidate->name, "_mf") != nullptr;
        attempt->width = options.width;
        attempt->height = options.height;
        attempt->time_base = {1, options.fps};
        attempt->framerate = {options.fps, 1};
        // MF wrapper/部分 Windows MFT 不接受显式 GOP 长度；FFmpeg 自身也把
        // MF encoder 的默认 g 设为 0。强行写 60 会让 avcodec_open2 返回 EINVAL。
        attempt->gop_size = mediaFoundation ? 0 : options.fps * 2;
        attempt->max_b_frames = 0;
        attempt->bit_rate = static_cast<int64_t>(options.bitrateMbps) * 1'000'000;
        attempt->pix_fmt = choosePixelFormat(candidate, options.format);
        if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
            attempt->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        if (std::strcmp(candidate->name, "libx264") == 0) {
            av_opt_set(attempt->priv_data, "preset", "veryfast", 0);
            av_opt_set(attempt->priv_data, "tune", "zerolatency", 0);
        } else if (std::strcmp(candidate->name, "libx265") == 0) {
            av_opt_set(attempt->priv_data, "preset", "fast", 0);
        }

        ret = avcodec_open2(attempt, candidate, nullptr);
        if (ret >= 0) {
            codec = candidate;
            codecCtx = attempt;
            break;
        }
        LOG_WARN("编码器不可用: codec={}, pixelFormat={}, error={} ({})",
                 candidate->name, static_cast<int>(attempt->pix_fmt), ret, ffmpegError(ret));
        avcodec_free_context(&attempt);
    }
    if (!codec || !codecCtx) {
        LOG_ERROR("所有候选编码器均初始化失败: format={}", formatExtension(options.format));
        return false;
    }
    m_codecCtx = codecCtx;
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.encoderName = codec->name;
        m_stats.hardwareEncoder = isHardwareEncoder(codec->name);
    }

    if ((options.format == RecordFormat::MP4_H264 && codecCtx->codec_id != AV_CODEC_ID_H264) ||
        (options.format == RecordFormat::MP4_H265 && codecCtx->codec_id != AV_CODEC_ID_HEVC)) {
        LOG_WARN("请求的编码器在当前系统不可用，已回退兼容编码器: codec={}", codec->name);
    }

    AVStream* stream = avformat_new_stream(fmtCtx, codec);
    if (!stream) {
        LOG_ERROR("avformat_new_stream 失败");
        return false;
    }
    m_stream = stream;

    avcodec_parameters_from_context(stream->codecpar, codecCtx);
    stream->time_base = codecCtx->time_base;

    if ((options.captureSystemAudio || options.captureMicrophone) &&
        !initializeAudioEncoder(options)) {
        LOG_WARN("音频编码器不可用, 已降级为仅视频录制");
        m_audioCapture.stop();
        std::lock_guard lock(m_statsMutex);
        m_stats.systemAudioActive = false;
        m_stats.microphoneActive = false;
        if (m_stats.audioError.empty()) m_stats.audioError = "audio encoder unavailable";
    }

    // 打开输出文件
    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmtCtx->pb, m_workingOutputPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOG_ERROR("avio_open 失败: {}", m_workingOutputPath);
            return false;
        }
    }

    // 写入文件头 (MP4 写入分片参数，保障异常中断时已录制内容仍可直接播放)
    AVDictionary* muxerOpts = nullptr;
    if (m_options.format == RecordFormat::MP4_H264 || m_options.format == RecordFormat::MP4_H265) {
        av_dict_set(&muxerOpts, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);
    } else if (m_options.format == RecordFormat::GIF) {
        av_dict_set(&muxerOpts, "loop", "0", 0);
        av_dict_set(&muxerOpts, "final_delay", "5", 0);
    }
    ret = avformat_write_header(fmtCtx, &muxerOpts);
    av_dict_free(&muxerOpts);
    if (ret < 0) {
        LOG_ERROR("avformat_write_header 失败");
        return false;
    }
    m_headerWritten = true;

    // 分配编码帧。输入像素格式取决于运行时选择的捕获后端，转换器在首帧创建。
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR("av_frame_alloc 失败");
        return false;
    }
    frame->format = codecCtx->pix_fmt;
    frame->width = codecCtx->width;
    frame->height = codecCtx->height;
    if (av_frame_get_buffer(frame, 0) < 0) {
        LOG_ERROR("av_frame_get_buffer 失败");
        av_frame_free(&frame);
        return false;
    }
    m_frame = frame;
    m_packet = av_packet_alloc();
    if (!m_packet) {
        LOG_ERROR("av_packet_alloc 失败");
        return false;
    }

    LOG_DEBUG("FFmpeg 编码器初始化成功: codec={}, {}x{} @ {}fps",
              codec->name, options.width, options.height, options.fps);
    return true;
}

bool ScreenRecorder::initializeAudioEncoder(const RecordOptions& options) {
    auto* fmtCtx = static_cast<AVFormatContext*>(m_fmtCtx);
    if (!fmtCtx) return false;
    const AVCodecID codecId = options.format == RecordFormat::WebM_VP9
        ? AV_CODEC_ID_OPUS : AV_CODEC_ID_AAC;
    const AVCodec* codec = avcodec_find_encoder(codecId);
    if (!codec) return false;

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwrContext* resampler = nullptr;
    auto releaseLocals = [&]() {
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        if (resampler) swr_free(&resampler);
        if (codecCtx) avcodec_free_context(&codecCtx);
    };
    if (!codecCtx) return false;

    codecCtx->sample_rate = AudioCapture::SampleRate;
    codecCtx->time_base = {1, AudioCapture::SampleRate};
    codecCtx->bit_rate = 192'000;
    codecCtx->sample_fmt = chooseAudioSampleFormat(codec);
    AVChannelLayout stereoLayout = AV_CHANNEL_LAYOUT_STEREO;
    if (av_channel_layout_copy(&codecCtx->ch_layout, &stereoLayout) < 0) {
        releaseLocals();
        return false;
    }
    if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    int result = avcodec_open2(codecCtx, codec, nullptr);
    if (result < 0) {
        LOG_WARN("音频编码器初始化失败: codec={}, error={}", codec->name, ffmpegError(result));
        releaseLocals();
        return false;
    }

    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!frame || !packet) {
        releaseLocals();
        return false;
    }
    frame->format = codecCtx->sample_fmt;
    frame->sample_rate = codecCtx->sample_rate;
    frame->nb_samples = codecCtx->frame_size > 0 ? codecCtx->frame_size : 1024;
    if (av_channel_layout_copy(&frame->ch_layout, &codecCtx->ch_layout) < 0 ||
        av_frame_get_buffer(frame, 0) < 0) {
        releaseLocals();
        return false;
    }

    AVChannelLayout inputLayout = AV_CHANNEL_LAYOUT_STEREO;
    result = swr_alloc_set_opts2(
        &resampler, &codecCtx->ch_layout, codecCtx->sample_fmt, codecCtx->sample_rate,
        &inputLayout, AV_SAMPLE_FMT_FLT, AudioCapture::SampleRate, 0, nullptr);
    if (result < 0 || !resampler || swr_init(resampler) < 0) {
        releaseLocals();
        return false;
    }

    AVStream* stream = avformat_new_stream(fmtCtx, codec);
    if (!stream || avcodec_parameters_from_context(stream->codecpar, codecCtx) < 0) {
        releaseLocals();
        return false;
    }
    stream->time_base = codecCtx->time_base;

    m_audioCodecCtx = codecCtx;
    m_audioStream = stream;
    m_audioFrame = frame;
    m_audioSwrCtx = resampler;
    m_audioPacket = packet;
    m_audioInput.reserve(static_cast<std::size_t>(frame->nb_samples) * AudioCapture::Channels);
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.audioEncoderName = codec->name;
    }
    LOG_INFO("录屏音频已启用: codec={}, rate={}, channels={}",
             codec->name, codecCtx->sample_rate, codecCtx->ch_layout.nb_channels);
    return true;
}

bool ScreenRecorder::encodeAvailableAudio() {
    if (!m_audioCodecCtx || m_audioEncodingFailed) return true;
    auto* codecCtx = static_cast<AVCodecContext*>(m_audioCodecCtx);
    auto* frame = static_cast<AVFrame*>(m_audioFrame);
    auto* resampler = static_cast<SwrContext*>(m_audioSwrCtx);
    if (!codecCtx || !frame || !resampler) return true;

    const auto preStatus = m_audioCapture.status();
    if (preStatus.droppedFrames > m_accountedDroppedAudioFrames) {
        const std::uint64_t deltaDropped = preStatus.droppedFrames - m_accountedDroppedAudioFrames;
        m_audioFrameIndex += static_cast<int64_t>(deltaDropped);
        m_accountedDroppedAudioFrames = preStatus.droppedFrames;
    }

    while (m_audioCapture.readFrames(frame->nb_samples, m_audioInput)) {
        if (av_frame_make_writable(frame) < 0) {
            m_audioEncodingFailed = true;
            return false;
        }
        const std::uint8_t* input[]{
            reinterpret_cast<const std::uint8_t*>(m_audioInput.data())
        };
        const int converted = swr_convert(resampler, frame->data, frame->nb_samples,
                                          input, frame->nb_samples);
        if (converted != frame->nb_samples) {
            m_audioEncodingFailed = true;
            return false;
        }
        frame->pts = m_audioFrameIndex;
        frame->duration = converted;
        m_audioFrameIndex += converted;
        if (avcodec_send_frame(codecCtx, frame) < 0 || !writeAvailableAudioPackets()) {
            m_audioEncodingFailed = true;
            return false;
        }
    }

    const auto audioStatus = m_audioCapture.status();
    if (audioStatus.droppedFrames > m_accountedDroppedAudioFrames) {
        const std::uint64_t deltaDropped = audioStatus.droppedFrames - m_accountedDroppedAudioFrames;
        m_audioFrameIndex += static_cast<int64_t>(deltaDropped);
        m_accountedDroppedAudioFrames = audioStatus.droppedFrames;
    }
    std::lock_guard lock(m_statsMutex);
    m_stats.systemAudioActive = audioStatus.systemAudioActive;
    m_stats.microphoneActive = audioStatus.microphoneActive;
    m_stats.droppedAudioFrames = audioStatus.droppedFrames;
    m_stats.audioDiscontinuities = audioStatus.discontinuities;
    m_stats.audioReconnectAttempts = audioStatus.reconnectAttempts;
    m_stats.audioReconnectSuccesses = audioStatus.reconnectSuccesses;
    m_stats.systemAudioPeak = audioStatus.systemPeak;
    m_stats.microphonePeak = audioStatus.microphonePeak;
    m_stats.mixedAudioPeak = audioStatus.mixedPeak;
    m_stats.systemAudioMuted = audioStatus.systemMuted;
    m_stats.microphoneMuted = audioStatus.microphoneMuted;
    if (!audioStatus.error.empty()) m_stats.audioError = audioStatus.error;
    return true;
}

bool ScreenRecorder::writeAvailableAudioPackets() {
    auto* codecCtx = static_cast<AVCodecContext*>(m_audioCodecCtx);
    auto* fmtCtx = static_cast<AVFormatContext*>(m_fmtCtx);
    auto* stream = static_cast<AVStream*>(m_audioStream);
    auto* packet = static_cast<AVPacket*>(m_audioPacket);
    if (!codecCtx || !fmtCtx || !stream || !packet) return false;
    while (true) {
        const int result = avcodec_receive_packet(codecCtx, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
        if (result < 0) return false;
        av_packet_rescale_ts(packet, codecCtx->time_base, stream->time_base);
        packet->stream_index = stream->index;
        const int writeResult = av_interleaved_write_frame(fmtCtx, packet);
        av_packet_unref(packet);
        if (writeResult < 0) return false;
    }
}

void ScreenRecorder::reapFinishedSnapshots(bool waitAll) {
    std::vector<std::thread> toJoin;
    {
        std::lock_guard lock(m_snapshotMutex);
        auto it = m_snapshotTasks.begin();
        while (it != m_snapshotTasks.end()) {
            if (waitAll || (it->done && it->done->load(std::memory_order_acquire))) {
                if (it->worker.joinable()) {
                    toJoin.push_back(std::move(it->worker));
                }
                it = m_snapshotTasks.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& t : toJoin) {
        if (t.joinable() && std::this_thread::get_id() != t.get_id()) {
            t.join();
        }
    }
}

void ScreenRecorder::dispatchSnapshotSave(
    const uint8_t* data, int width, int height, int stride, CapturePixelFormat format) {
    if (!data || width <= 0 || height <= 0 || stride <= 0) return;

    reapFinishedSnapshots(false);

    const int cvType = (format == CapturePixelFormat::Bgra32) ? CV_8UC4 : CV_8UC3;
    cv::Mat snapshot = cv::Mat(height, width, cvType, const_cast<uint8_t*>(data), static_cast<size_t>(stride)).clone();
    if (snapshot.empty()) return;

    const std::string baseOutputPath = m_outputPath;
    auto doneFlag = std::make_shared<std::atomic<bool>>(false);

    std::thread worker([snapshot = std::move(snapshot), baseOutputPath, doneFlag]() {
        struct DoneGuard {
            std::shared_ptr<std::atomic<bool>> flag;
            ~DoneGuard() {
                if (flag) flag->store(true, std::memory_order_release);
            }
        } doneGuard{doneFlag};

        tools3000::core::TraceId::Scope traceScope;

        std::error_code ec;
        std::filesystem::path dir;
        if (!baseOutputPath.empty()) {
            dir = std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(baseOutputPath)).parent_path();
        }
        if (dir.empty() || !std::filesystem::exists(dir, ec)) {
            wchar_t myPictures[MAX_PATH] = {0};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, 0, myPictures))) {
                dir = std::filesystem::path(myPictures) / L"Tools3000";
            } else {
                dir = std::filesystem::current_path(ec);
            }
        }
        std::filesystem::create_directories(dir, ec);

        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        tm localTm{};
        localtime_s(&localTm, &t);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        wchar_t filename[64]{};
        std::swprintf(filename, std::size(filename), L"Snapshot_%04d%02d%02d_%02d%02d%02d_%03d.png",
                      localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday,
                      localTm.tm_hour, localTm.tm_min, localTm.tm_sec, static_cast<int>(ms));
        const std::filesystem::path fullPath = dir / filename;
        const std::wstring fullPathW = fullPath.wstring();
        const std::string fullPathUtf8 = tools3000::core::WinUtils::wstringToUtf8(fullPathW);

        std::vector<uchar> pngBytes;
        bool savedOk = false;
        if (cv::imencode(".png", snapshot, pngBytes) && !pngBytes.empty()) {
            savedOk = tools3000::common::atomicWriteBinaryFileWithFlush(
                fullPathW, pngBytes.data(), pngBytes.size());
        }
        if (savedOk) {
            LOG_INFO("Recording snapshot saved: {}", fullPathUtf8);
        } else {
            LOG_ERROR("Failed to save recording snapshot: {}", fullPathUtf8);
        }

        auto deliverToClipboard = [snapshot, fullPathW, savedOk]() {
            ClipboardUtils::copyImageToClipboard(snapshot, savedOk ? fullPathW : std::wstring{});

            const bool isZh = tools3000::core::isUiLanguageChinese();
            if (savedOk) {
                tools3000::core::EventBus::instance().publish(
                    tools3000::core::ShowToastEvent{isZh ? L"已保存录屏截图并复制到剪贴板" : L"Frame saved and copied to clipboard"});
            } else {
                tools3000::core::EventBus::instance().publish(
                    tools3000::core::ShowToastEvent{isZh ? L"录屏截图已复制到剪贴板（保存文件失败）" : L"Frame copied to clipboard (file save failed)"});
            }
        };
        auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
        if (dispatcher.isInitialized()) {
            dispatcher.post(std::move(deliverToClipboard));
        } else {
            deliverToClipboard();
        }
    });

    std::lock_guard lock(m_snapshotMutex);
    m_snapshotTasks.push_back(SnapshotTask{std::move(worker), std::move(doneFlag)});
}

bool ScreenRecorder::captureAndEncodeFrame() {
    using clock = std::chrono::steady_clock;
    const auto pipelineStarted = clock::now();
    auto* codecCtx = static_cast<AVCodecContext*>(m_codecCtx);
    auto* frame = static_cast<AVFrame*>(m_frame);

    if (!codecCtx || !frame) return false;

    if (!m_captureBackend) return false;
    CaptureFrameView captured;
    std::string captureError;
    if (!m_captureBackend->capture(captured, captureError)) {
        if (isDesktopTemporarilyInaccessible()) {
            // Secure desktop (UAC prompt / Win+L lock screen): keep constant-timeline encoding
            // alive by reusing the last valid encoded frame (or encoding a solid black frame
            // if the session started directly on a secure desktop).
            const auto capturedAt = clock::now();
            if (!m_hasEncodedVideoFrame) {
                const std::size_t rowBytes = static_cast<std::size_t>(m_options.width) * 3ULL;
                std::vector<uint8_t> blackBgr(rowBytes * static_cast<std::size_t>(m_options.height), 0);
                if (m_options.format == RecordFormat::GIF && codecCtx->pix_fmt == AV_PIX_FMT_PAL8) {
                    if (!m_gifOptimizer) {
                        m_gifOptimizer = std::make_unique<GifPaletteOptimizer>();
                    }
                    m_gifOptimizer->reserveTransparent = true;
                    m_gifOptimizer->enableFrameDiff = true;
                    m_gifOptimizer->cornerRadius = m_options.cornerRadius;
                    if (av_frame_make_writable(frame) < 0) return false;
                    m_gifOptimizer->quantizeFrame(
                        blackBgr.data(), m_options.width, m_options.height,
                        static_cast<int>(rowBytes), 3,
                        frame->data[0], frame->linesize[0],
                        reinterpret_cast<uint32_t*>(frame->data[1]));
                } else {
                    if (!ensureScaler(CapturePixelFormat::Bgr24)) return false;
                    auto* swsCtx = static_cast<SwsContext*>(m_swsCtx);
                    const uint8_t* srcSlice[] = { blackBgr.data() };
                    int srcStride[] = { static_cast<int>(rowBytes) };
                    if (av_frame_make_writable(frame) < 0 ||
                        sws_scale(swsCtx, srcSlice, srcStride, 0, m_options.height,
                                  frame->data, frame->linesize) <= 0) {
                        return false;
                    }
                }
                m_hasEncodedVideoFrame = true;
            }
            const auto convertedAt = clock::now();
            frame->pts = m_frameIndex++;
            if (avcodec_send_frame(codecCtx, frame) < 0) return false;
            const bool written = writeAvailablePackets();
            const auto encodedAt = clock::now();
            const auto milliseconds = [](auto duration) {
                return std::chrono::duration<double, std::milli>(duration).count();
            };
            m_lastFrameTimings.captureMs = milliseconds(capturedAt - pipelineStarted);
            m_lastFrameTimings.conversionMs = milliseconds(convertedAt - capturedAt);
            m_lastFrameTimings.encodeMs = milliseconds(encodedAt - convertedAt);
            m_lastFrameTimings.totalMs = milliseconds(encodedAt - pipelineStarted);
            return written;
        }
        LOG_ERROR("录屏帧捕获失败: backend={}, error={}",
                  m_captureBackend->info().id, captureError);
        return false;
    }
    const auto capturedAt = clock::now();
    struct FrameReleaseGuard {
        ICaptureBackend* backend;
        ~FrameReleaseGuard() { backend->releaseFrame(); }
    } releaseGuard{m_captureBackend.get()};

    const auto& backendInfo = m_captureBackend->info();
    if (backendInfo.id != m_activeCaptureBackendId) {
        LOG_INFO("录屏捕获后端已切换: {} -> {}", m_activeCaptureBackendId, backendInfo.id);
        m_activeCaptureBackendId = backendInfo.id;
        std::lock_guard lock(m_statsMutex);
        m_stats.captureBackend = backendInfo.id;
    }
    [[maybe_unused]] auto cursorPatch = m_cursorOverlay.apply(
        captured,
        {m_options.regionX, m_options.regionY, m_options.width, m_options.height},
        m_options.includeCursor, m_options.showClickEffects, m_options.showClickZoom);

    const bool isPal8Gif = (m_options.format == RecordFormat::GIF && codecCtx->pix_fmt == AV_PIX_FMT_PAL8);
    if (m_options.cornerRadius > 0.5f && !isPal8Gif) {
        applyVideoFrameCornerMask(captured.data, m_options.width, m_options.height, captured.stride, captured.format, m_options.cornerRadius);
    }

    if (m_snapshotRequested.exchange(false)) {
        if (m_options.cornerRadius > 0.5f && isPal8Gif) {
            const int bpp = (captured.format == CapturePixelFormat::Bgra32) ? 4 : 3;
            const int snapStride = m_options.width * bpp;
            std::vector<uint8_t> snapBuf(static_cast<std::size_t>(snapStride) * static_cast<std::size_t>(m_options.height));
            for (int y = 0; y < m_options.height; ++y) {
                std::memcpy(snapBuf.data() + static_cast<std::size_t>(y) * snapStride,
                            captured.data + static_cast<std::size_t>(y) * captured.stride,
                            static_cast<std::size_t>(snapStride));
            }
            applyVideoFrameCornerMask(snapBuf.data(), m_options.width, m_options.height, snapStride, captured.format, m_options.cornerRadius);
            dispatchSnapshotSave(snapBuf.data(), m_options.width, m_options.height, snapStride, captured.format);
        } else {
            dispatchSnapshotSave(captured.data, m_options.width, m_options.height, captured.stride, captured.format);
        }
    }

    if (isPal8Gif) {
        if (!m_gifOptimizer) {
            m_gifOptimizer = std::make_unique<GifPaletteOptimizer>();
        }
        m_gifOptimizer->reserveTransparent = true;
        m_gifOptimizer->enableFrameDiff = true;
        m_gifOptimizer->cornerRadius = m_options.cornerRadius;
        if (av_frame_make_writable(frame) < 0) return false;
        int bpp = captured.format == CapturePixelFormat::Bgra32 ? 4 : 3;
        m_gifOptimizer->quantizeFrame(
            captured.data, m_options.width, m_options.height, captured.stride, bpp,
            frame->data[0], frame->linesize[0],
            reinterpret_cast<uint32_t*>(frame->data[1]));
    } else {
        if (!ensureScaler(captured.format)) return false;
        auto* swsCtx = static_cast<SwsContext*>(m_swsCtx);

        const uint8_t* srcSlice[] = { captured.data };
        int srcStride[] = { captured.stride };

        if (av_frame_make_writable(frame) < 0 ||
            sws_scale(swsCtx, srcSlice, srcStride, 0, m_options.height,
                      frame->data, frame->linesize) <= 0) {
            return false;
        }
    }
    const auto convertedAt = clock::now();

    frame->pts = m_frameIndex++;

    // 编码
    int ret = avcodec_send_frame(codecCtx, frame);
    if (ret < 0) return false;

    const bool written = writeAvailablePackets();
    if (written) {
        m_hasEncodedVideoFrame = true;
    }
    const auto encodedAt = clock::now();
    const auto milliseconds = [](auto duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    m_lastFrameTimings.captureMs = milliseconds(capturedAt - pipelineStarted);
    m_lastFrameTimings.conversionMs = milliseconds(convertedAt - capturedAt);
    m_lastFrameTimings.encodeMs = milliseconds(encodedAt - convertedAt);
    m_lastFrameTimings.totalMs = milliseconds(encodedAt - pipelineStarted);
    return written;
}

bool ScreenRecorder::ensureScaler(CapturePixelFormat format) {
    if (m_swsCtx && m_scalerInputFormat == format) return true;
    if (m_swsCtx) {
        sws_freeContext(static_cast<SwsContext*>(m_swsCtx));
        m_swsCtx = nullptr;
    }
    auto* codecCtx = static_cast<AVCodecContext*>(m_codecCtx);
    if (!codecCtx) return false;
    const AVPixelFormat sourceFormat = format == CapturePixelFormat::Bgra32
        ? AV_PIX_FMT_BGRA : AV_PIX_FMT_BGR24;
    m_swsCtx = sws_getContext(
        m_options.width, m_options.height, sourceFormat,
        m_options.width, m_options.height, codecCtx->pix_fmt,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        LOG_ERROR("sws_getContext 失败: sourceFormat={}", static_cast<int>(sourceFormat));
        return false;
    }
    m_scalerInputFormat = format;
    return true;
}

bool ScreenRecorder::initializeCaptureResources() {
    cleanupCaptureResources();
    m_captureBackend = createCaptureBackend();
    if (!m_captureBackend) return false;
    std::string error;
    const CaptureRegion region{
        m_options.regionX, m_options.regionY, m_options.width, m_options.height
    };
    if (!m_captureBackend->initialize(region, error)) {
        LOG_ERROR("录屏捕获后端初始化失败: error={}", error);
        m_captureBackend.reset();
        return false;
    }
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.captureBackend = m_captureBackend->info().id;
    }
    m_activeCaptureBackendId = m_captureBackend->info().id;
    LOG_INFO("录屏捕获后端已选择: backend={}, accelerated={}",
             m_captureBackend->info().id, m_captureBackend->info().accelerated);
    return true;
}

void ScreenRecorder::cleanupCaptureResources() {
    if (m_captureBackend) m_captureBackend->shutdown();
    m_captureBackend.reset();
    m_activeCaptureBackendId.clear();
}

bool ScreenRecorder::writeAvailablePackets() {
    auto* codecCtx = static_cast<AVCodecContext*>(m_codecCtx);
    auto* fmtCtx = static_cast<AVFormatContext*>(m_fmtCtx);
    auto* stream = static_cast<AVStream*>(m_stream);
    auto* packet = static_cast<AVPacket*>(m_packet);
    if (!codecCtx || !fmtCtx || !stream || !packet) return false;

    while (true) {
        const int ret = avcodec_receive_packet(codecCtx, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        if (ret < 0) return false;
        av_packet_rescale_ts(packet, codecCtx->time_base, stream->time_base);
        packet->stream_index = stream->index;
        const int writeResult = av_interleaved_write_frame(fmtCtx, packet);
        av_packet_unref(packet);
        if (writeResult < 0) return false;
    }
}

void ScreenRecorder::cleanupEncoder() {
    bool ignored = true;
    cleanupEncoder(ignored);
}

void ScreenRecorder::cleanupEncoder(bool& closeOk) {
    cleanupCaptureResources();
    m_audioCapture.stop();
    if (m_audioPacket) {
        av_packet_free(reinterpret_cast<AVPacket**>(&m_audioPacket));
        m_audioPacket = nullptr;
    }
    if (m_audioSwrCtx) {
        swr_free(reinterpret_cast<SwrContext**>(&m_audioSwrCtx));
        m_audioSwrCtx = nullptr;
    }
    if (m_audioFrame) {
        av_frame_free(reinterpret_cast<AVFrame**>(&m_audioFrame));
        m_audioFrame = nullptr;
    }
    if (m_audioCodecCtx) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&m_audioCodecCtx));
        m_audioCodecCtx = nullptr;
    }
    if (m_packet) {
        av_packet_free(reinterpret_cast<AVPacket**>(&m_packet));
        m_packet = nullptr;
    }
    if (m_swsCtx) {
        sws_freeContext(static_cast<SwsContext*>(m_swsCtx));
        m_swsCtx = nullptr;
    }
    m_scalerInputFormat = CapturePixelFormat::Bgr24;
    if (m_frame) {
        av_frame_free(reinterpret_cast<AVFrame**>(&m_frame));
        m_frame = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&m_codecCtx));
        m_codecCtx = nullptr;
    }
    if (m_fmtCtx) {
        auto* fmtCtx = static_cast<AVFormatContext*>(m_fmtCtx);
        if (fmtCtx->pb) {
            const int closeRet = avio_closep(&fmtCtx->pb);
            if (closeRet < 0) {
                closeOk = false;
                LOG_ERROR("avio_closep 关闭输出文件失败: error={}", closeRet);
            }
        }
        avformat_free_context(fmtCtx);
        m_fmtCtx = nullptr;
    }
    m_stream = nullptr;
    m_audioStream = nullptr;
    m_audioInput.clear();
    m_audioEncodingFailed = false;
    m_headerWritten = false;
    m_gifOptimizer.reset();
}

std::string ScreenRecorder::generateOutputPath(RecordFormat format) const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};
    localtime_s(&localTime, &time_t);

    std::ostringstream oss;
    oss << "Tools3000_Rec_" << std::put_time(&localTime, "%Y%m%d_%H%M%S")
        << "." << formatExtension(format);

    auto dir = tools3000::core::WinUtils::getAppDataDirectory() / L"Recordings";
    return tools3000::core::WinUtils::wstringToUtf8((dir / oss.str()).wstring());
}

std::string ScreenRecorder::formatExtension(RecordFormat format) {
    switch (format) {
        case RecordFormat::MP4_H264: return "mp4";
        case RecordFormat::MP4_H265: return "mp4";
        case RecordFormat::WebM_VP9: return "webm";
        case RecordFormat::GIF:      return "gif";
        default: return "mp4";
    }
}

}  // namespace tools3000::capture
