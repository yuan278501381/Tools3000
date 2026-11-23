#include "capture/MediaProbeWorker.h"
#include "capture/CaptureWorkerCommon.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

namespace tools3000::capture {

namespace {

bool encodeBgraToJpegDataUri(const uint8_t* bgra,
                             int width,
                             int height,
                             int stride,
                             std::string& outDataUri) {
    outDataUri.clear();
    if (!bgra || width <= 0 || height <= 0 || stride < width * 4) return false;

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) || !factory) {
        return false;
    }

    ComPtr<IStream> memStream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memStream)) || !memStream) {
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) || !encoder) {
        return false;
    }
    if (FAILED(encoder->Initialize(memStream.Get(), WICBitmapEncoderNoCache))) {
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props)) || !frame) {
        return false;
    }
    if (props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_R4;
        var.fltVal = 0.80f;
        (void)props->Write(1, &opt, &var);
    }
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) return false;

    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    (void)frame->SetPixelFormat(&pf);

    // 转 24bpp BGR 供 WIC JPEG 编码器零误差写入
    const int bgrStride = width * 3;
    std::vector<uint8_t> bgr(static_cast<size_t>(bgrStride) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = bgra + static_cast<size_t>(y) * static_cast<size_t>(stride);
        uint8_t* dstRow = bgr.data() + static_cast<size_t>(y) * static_cast<size_t>(bgrStride);
        for (int x = 0; x < width; ++x) {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 0];
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 3 + 2] = srcRow[x * 4 + 2];
        }
    }

    if (FAILED(frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(bgrStride),
                                  static_cast<UINT>(bgr.size()), bgr.data()))) {
        return false;
    }
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;

    HGLOBAL hMem = nullptr;
    if (FAILED(GetHGlobalFromStream(memStream.Get(), &hMem)) || !hMem) return false;
    const SIZE_T sz = GlobalSize(hMem);
    if (sz == 0) return false;
    const void* ptr = GlobalLock(hMem);
    if (!ptr) return false;
    std::vector<uint8_t> jpeg(static_cast<const uint8_t*>(ptr),
                              static_cast<const uint8_t*>(ptr) + sz);
    GlobalUnlock(hMem);

    outDataUri = "data:image/jpeg;base64," + tools3000::core::WinUtils::base64Encode(jpeg);
    return true;
}

bool decodeOneFrameAt(AVFormatContext* fmtCtx,
                      AVCodecContext* decCtx,
                      int videoStreamIdx,
                      int64_t seekTargetTs,
                      int maxThumbWidth,
                      AVFrame* frame,
                      AVPacket* pkt,
                      std::string& outDataUri) {
    if (seekTargetTs > 0) {
        av_seek_frame(fmtCtx, videoStreamIdx, seekTargetTs, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(decCtx);
    }

    int packetsScanned = 0;
    bool gotFrame = false;
    while (packetsScanned < 180 && av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index != videoStreamIdx) {
            av_packet_unref(pkt);
            continue;
        }
        ++packetsScanned;
        const int sendRes = avcodec_send_packet(decCtx, pkt);
        av_packet_unref(pkt);
        if (sendRes < 0 && sendRes != AVERROR(EAGAIN)) {
            break;
        }
        const int recvRes = avcodec_receive_frame(decCtx, frame);
        if (recvRes == 0 && frame->width > 0 && frame->height > 0) {
            gotFrame = true;
            break;
        }
    }

    if (!gotFrame) {
        // 冲刷解码器缓冲
        (void)avcodec_send_packet(decCtx, nullptr);
        if (avcodec_receive_frame(decCtx, frame) == 0 && frame->width > 0 && frame->height > 0) {
            gotFrame = true;
        }
    }
    if (!gotFrame) return false;

    int dstW = frame->width;
    int dstH = frame->height;
    if (dstW > maxThumbWidth) {
        dstH = (std::max)(2, static_cast<int>(std::lround(static_cast<double>(dstH) * maxThumbWidth / dstW)));
        dstW = maxThumbWidth;
    }
    dstW = (dstW + 1) & ~1;
    dstH = (dstH + 1) & ~1;

    SwsContext* sws = sws_getContext(
        frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        dstW, dstH, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        av_frame_unref(frame);
        return false;
    }

    const int dstStride = dstW * 4;
    std::vector<uint8_t> bgra(static_cast<size_t>(dstStride) * static_cast<size_t>(dstH));
    uint8_t* dstData[4] = {bgra.data(), nullptr, nullptr, nullptr};
    int dstLinesize[4] = {dstStride, 0, 0, 0};
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);
    sws_freeContext(sws);
    av_frame_unref(frame);

    return encodeBgraToJpegDataUri(bgra.data(), dstW, dstH, dstStride, outDataUri);
}

}  // namespace

int runMediaProbeWorkerProcess(int argc, const wchar_t* const* argv) {
    const std::wstring jobFile = tools3000::worker::findCliOptionValue(
        argc, argv, tools3000::worker::Constants::kFlagJobFile);
    if (jobFile.empty()) return 2;

    std::ifstream ifs(std::filesystem::path(jobFile), std::ios::binary);
    if (!ifs) return 3;
    const json job = json::parse(ifs, nullptr, false);
    if (!job.is_object()) return 3;

    const std::string inputPathUtf8 = job.value("inputPath", "");
    const std::string outputPathUtf8 = job.value("outputPath", "");
    const int maxFrames = std::clamp(job.value("maxFrames", 5), 1, 8);
    const int maxThumbWidth = std::clamp(job.value("maxThumbWidth", 420), 160, 720);
    if (inputPathUtf8.empty() || outputPathUtf8.empty()) return 3;

    json result = {
        {"ok", false},
        {"durationSec", 0.0},
        {"width", 0},
        {"height", 0},
        {"fps", 0.0},
        {"bitrateKbps", 0},
        {"videoCodec", ""},
        {"audioCodec", ""},
        {"audioChannels", 0},
        {"audioSampleRate", 0},
        {"containerFormat", ""},
        {"thumbnails", json::array()}
    };

    av_log_set_level(AV_LOG_QUIET);

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, inputPathUtf8.c_str(), nullptr, nullptr) == 0 && fmtCtx) {
        if (avformat_find_stream_info(fmtCtx, nullptr) >= 0) {
            result["ok"] = true;
            if (fmtCtx->iformat && fmtCtx->iformat->name) {
                result["containerFormat"] = std::string(fmtCtx->iformat->name);
            }
            if (fmtCtx->duration > 0 && fmtCtx->duration != AV_NOPTS_VALUE) {
                result["durationSec"] = static_cast<double>(fmtCtx->duration) / static_cast<double>(AV_TIME_BASE);
            }
            if (fmtCtx->bit_rate > 0) {
                result["bitrateKbps"] = static_cast<int64_t>(fmtCtx->bit_rate / 1000);
            }

            const int audioIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (audioIdx >= 0) {
                const AVCodecParameters* ap = fmtCtx->streams[audioIdx]->codecpar;
                if (ap) {
                    const char* aName = avcodec_get_name(ap->codec_id);
                    if (aName) result["audioCodec"] = std::string(aName);
                    result["audioChannels"] = ap->ch_layout.nb_channels;
                    result["audioSampleRate"] = ap->sample_rate;
                }
            }

            const int videoIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (videoIdx >= 0) {
                AVStream* vs = fmtCtx->streams[videoIdx];
                const AVCodecParameters* vp = vs->codecpar;
                if (vp) {
                    result["width"] = vp->width;
                    result["height"] = vp->height;
                    const char* vName = avcodec_get_name(vp->codec_id);
                    if (vName) result["videoCodec"] = std::string(vName);
                }
                const AVRational fr = av_guess_frame_rate(fmtCtx, vs, nullptr);
                if (fr.num > 0 && fr.den > 0) {
                    result["fps"] = std::round((static_cast<double>(fr.num) / fr.den) * 100.0) / 100.0;
                }

                const AVCodec* decoder = vp ? avcodec_find_decoder(vp->codec_id) : nullptr;
                if (decoder) {
                    AVCodecContext* decCtx = avcodec_alloc_context3(decoder);
                    if (decCtx) {
                        if (avcodec_parameters_to_context(decCtx, vp) >= 0) {
                            decCtx->thread_count = 2;
                            if (avcodec_open2(decCtx, decoder, nullptr) == 0) {
                                AVFrame* frame = av_frame_alloc();
                                AVPacket* pkt = av_packet_alloc();
                                if (frame && pkt) {
                                    const double durSec = result.value("durationSec", 0.0);
                                    const double tb = av_q2d(vs->time_base);
                                    json thumbs = json::array();

                                    if (durSec > 2.0 && tb > 0.0) {
                                        for (int i = 0; i < maxFrames; ++i) {
                                            const double frac = (static_cast<double>(i) + 0.5) / static_cast<double>(maxFrames);
                                            const double targetSec = durSec * frac;
                                            const int64_t ts = static_cast<int64_t>(targetSec / tb);
                                            std::string uri;
                                            if (decodeOneFrameAt(fmtCtx, decCtx, videoIdx, ts, maxThumbWidth, frame, pkt, uri) && !uri.empty()) {
                                                thumbs.push_back({
                                                    {"timestampSec", std::round(targetSec * 10.0) / 10.0},
                                                    {"dataUri", std::move(uri)}
                                                });
                                            }
                                        }
                                    } else {
                                        std::string uri;
                                        if (decodeOneFrameAt(fmtCtx, decCtx, videoIdx, 0, maxThumbWidth, frame, pkt, uri) && !uri.empty()) {
                                            thumbs.push_back({
                                                {"timestampSec", 0.0},
                                                {"dataUri", std::move(uri)}
                                            });
                                        }
                                    }
                                    result["thumbnails"] = std::move(thumbs);
                                }
                                if (pkt) av_packet_free(&pkt);
                                if (frame) av_frame_free(&frame);
                            }
                        }
                        avcodec_free_context(&decCtx);
                    }
                }
            }
        }
        avformat_close_input(&fmtCtx);
    }

    const std::wstring outPathW = tools3000::core::WinUtils::utf8ToWstring(outputPathUtf8);
    tools3000::core::WinUtils::atomicWriteFileWithFlush(outPathW, result.dump());
    return result.value("ok", false) ? 0 : 1;
}

}  // namespace tools3000::capture
