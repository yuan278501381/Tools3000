// ─────────────────────────────────────────────────────────────────────────────
// RecordEncodeWorker.cpp — 录屏音视频编码复用短命子进程隔离实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/RecordEncodeWorker.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

namespace tools3000::capture {

namespace {

constexpr uint32_t kRecProtoMagic   = 0x54335245u; // "T3RE"
constexpr uint32_t kRecProtoVersion = 1u;
constexpr DWORD kRecInitTimeoutMs   = 20000;
constexpr DWORD kRecFrameTimeoutMs  = 30000;
constexpr DWORD kRecFinalTimeoutMs  = 30000;

enum class RecCommand : uint32_t {
    Init       = 1u,
    VideoFrame = 2u,
    AudioChunk = 3u,
    Finalize   = 4u,
    Abort      = 5u,
};

struct RecCmdHeader {
    uint32_t magic = kRecProtoMagic;
    uint32_t version = kRecProtoVersion;
    uint32_t command = 0;
    uint32_t reserved = 0;
};

struct RecInitWire {
    int32_t format = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t fps = 30;
    int32_t bitrateMbps = 8;
    int32_t enableAudio = 0;
    int32_t audioSampleRate = 48000;
    int32_t audioChannels = 2;
    uint64_t frameSlotBytes = 0;
};

struct RecInitReplyWire {
    int32_t ok = 0;
    int32_t hardwareEncoder = 0;
    int32_t audioEnabled = 0;
    int32_t audioFrameSamples = 1024;
};

struct RecVideoFrameWire {
    int64_t pts = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;
    int32_t pixelFormat = 0; // 0 = Bgra32, 1 = Bgr24
    int32_t isKeyFrame = 0;
    int32_t reserved = 0;
};

struct RecAudioChunkWire {
    int64_t pts = 0;
    int32_t nbSamples = 0;
    int32_t channels = 2;
    uint32_t byteCount = 0;
    uint32_t reserved = 0;
};

struct RecAckWire {
    int32_t ok = 0;
    int32_t extra = 0; // Finalize 时代表 closeOk
};

bool isHwEncoderName(const char* name) {
    if (!name) return false;
    return std::strstr(name, "_nvenc") || std::strstr(name, "_qsv") ||
           std::strstr(name, "_amf") || std::strstr(name, "_mf");
}

void addUniqueCodec(std::vector<const AVCodec*>& list, const AVCodec* c) {
    if (!c) return;
    for (const auto* ex : list) {
        if (ex == c || std::strcmp(ex->name, c->name) == 0) return;
    }
    list.push_back(c);
}

std::vector<const AVCodec*> buildVideoCandidates(RecordFormat fmt, int width, int height) {
    std::vector<const AVCodec*> codecs;
    const int64_t totalAreaPx = static_cast<int64_t>(width) * static_cast<int64_t>(height);
    if (fmt == RecordFormat::MP4_H264) {
        for (const char* name : {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf",
                                 "libx264", "libopenh264", "mpeg4"}) {
            addUniqueCodec(codecs, avcodec_find_encoder_by_name(name));
        }
        if (totalAreaPx > (1920LL * 1200LL)) {
            if (const AVCodec* hevcMf = avcodec_find_encoder_by_name("hevc_mf")) {
                auto posIt = std::find_if(codecs.begin(), codecs.end(), [](const AVCodec* c) {
                    return c && (std::strcmp(c->name, "libx264") == 0 || std::strcmp(c->name, "libopenh264") == 0);
                });
                codecs.insert(posIt, hevcMf);
            }
        }
    } else if (fmt == RecordFormat::MP4_H265) {
        for (const char* name : {"hevc_nvenc", "hevc_qsv", "hevc_amf", "hevc_mf",
                                 "libx265", "mpeg4"}) {
            addUniqueCodec(codecs, avcodec_find_encoder_by_name(name));
        }
    } else if (fmt == RecordFormat::WebM_VP9) {
        addUniqueCodec(codecs, avcodec_find_encoder_by_name("libvpx-vp9"));
        addUniqueCodec(codecs, avcodec_find_encoder(AV_CODEC_ID_VP9));
    }
    return codecs;
}

AVPixelFormat pickVideoPixFmt(const AVCodec* codec) {
    const bool mediaFoundation = codec && std::strstr(codec->name, "_mf") != nullptr;
    const void* configs = nullptr;
    int count = 0;
    const int result = avcodec_get_supported_config(
        nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count);
    if (result < 0 || !configs || count <= 0) {
        return mediaFoundation ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    }
    const auto* formats = static_cast<const AVPixelFormat*>(configs);
    const std::array<AVPixelFormat, 7> preferred = mediaFoundation
        ? std::array<AVPixelFormat, 7>{AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P,
                                       AV_PIX_FMT_BGRA, AV_PIX_FMT_BGR0, AV_PIX_FMT_YUV444P, AV_PIX_FMT_P010}
        : std::array<AVPixelFormat, 7>{AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_YUVJ420P,
                                       AV_PIX_FMT_BGRA, AV_PIX_FMT_BGR0, AV_PIX_FMT_YUV444P, AV_PIX_FMT_P010};
    for (const auto cand : preferred) {
        for (int i = 0; i < count; ++i) {
            if (formats[i] == cand) return cand;
        }
    }
    return formats[0];
}

AVSampleFormat pickAudioSampleFmt(const AVCodec* codec) {
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                     0, &configs, &count) >= 0 && configs && count > 0) {
        const auto* formats = static_cast<const AVSampleFormat*>(configs);
        for (const auto pref : {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT}) {
            for (int i = 0; i < count; ++i) {
                if (formats[i] == pref) return pref;
            }
        }
        return formats[0];
    }
    return AV_SAMPLE_FMT_FLTP;
}

class WorkerEncoderSession {
public:
    ~WorkerEncoderSession() {
        bool ignored = true;
        cleanup(ignored);
    }

    bool init(const RecInitWire& cfg, const std::string& outputPathUtf8, RecInitReplyWire& reply, std::string& encName, std::string& audioEncName) {
        const auto fmt = static_cast<RecordFormat>(cfg.format);
        const char* fmtName = (fmt == RecordFormat::WebM_VP9) ? "webm" : "mp4";
        if (avformat_alloc_output_context2(&m_fmtCtx, nullptr, fmtName, outputPathUtf8.c_str()) < 0 || !m_fmtCtx) {
            return false;
        }

        const auto candidates = buildVideoCandidates(fmt, cfg.width, cfg.height);
        if (candidates.empty()) return false;

        const int64_t totalAreaPx = static_cast<int64_t>(cfg.width) * static_cast<int64_t>(cfg.height);
        const double areaScale = std::clamp(static_cast<double>(totalAreaPx) / (1920.0 * 1080.0), 0.5, 6.0);
        const int64_t minAreaBitrate = static_cast<int64_t>(std::llround(areaScale * 8'500'000.0));
        const int64_t targetBitrate = (std::max)(static_cast<int64_t>(cfg.bitrateMbps) * 1'000'000LL, minAreaBitrate);

        const AVCodec* chosen = nullptr;
        AVCodecContext* ctx = nullptr;
        for (const auto* candidate : candidates) {
            const bool mediaFoundation = std::strstr(candidate->name, "_mf") != nullptr;
            const int mfPasses = mediaFoundation ? 2 : 1;
            for (int mfPass = 0; mfPass < mfPasses; ++mfPass) {
                AVCodecContext* attempt = avcodec_alloc_context3(candidate);
                if (!attempt) continue;
                attempt->width = cfg.width;
                attempt->height = cfg.height;
                attempt->time_base = {1, cfg.fps};
                attempt->framerate = {cfg.fps, 1};
                attempt->gop_size = mediaFoundation ? 0 : cfg.fps * 2;
                attempt->max_b_frames = 0;
                attempt->bit_rate = targetBitrate;
                attempt->pix_fmt = pickVideoPixFmt(candidate);
                if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
                    attempt->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                }
                if (mediaFoundation) {
                    if (mfPass == 0 && attempt->priv_data) {
                        av_opt_set_int(attempt->priv_data, "hw_encoding", 1, 0);
                    }
                } else {
                    attempt->thread_count = 0;
                }
                if (std::strcmp(candidate->name, "libx264") == 0 && attempt->priv_data) {
                    av_opt_set(attempt->priv_data, "preset", totalAreaPx >= (2560LL * 1440LL) ? "ultrafast" : "veryfast", 0);
                    av_opt_set(attempt->priv_data, "tune", "zerolatency", 0);
                } else if (std::strcmp(candidate->name, "libx265") == 0 && attempt->priv_data) {
                    av_opt_set(attempt->priv_data, "preset", "ultrafast", 0);
                } else if (std::strcmp(candidate->name, "libopenh264") == 0 && attempt->priv_data) {
                    av_opt_set_int(attempt->priv_data, "allow_skip_frames", 0, 0);
                    av_opt_set(attempt->priv_data, "slice_mode", "auto", 0);
                    attempt->rc_max_rate = targetBitrate * 2;
                    attempt->rc_buffer_size = static_cast<int>((std::min<int64_t>)(targetBitrate * 2, 120'000'000LL));
                }

                int ret = avcodec_open2(attempt, candidate, nullptr);
                if (ret >= 0 && mediaFoundation &&
                    (attempt->flags & AV_CODEC_FLAG_GLOBAL_HEADER) &&
                    attempt->extradata_size <= 0) {
                    if (AVCodecContext* probeCtx = avcodec_alloc_context3(candidate)) {
                        probeCtx->width = attempt->width;
                        probeCtx->height = attempt->height;
                        probeCtx->time_base = attempt->time_base;
                        probeCtx->framerate = attempt->framerate;
                        probeCtx->gop_size = attempt->gop_size;
                        probeCtx->max_b_frames = attempt->max_b_frames;
                        probeCtx->bit_rate = attempt->bit_rate;
                        probeCtx->pix_fmt = attempt->pix_fmt;
                        probeCtx->flags = attempt->flags;
                        if (mfPass == 0 && probeCtx->priv_data) {
                            av_opt_set_int(probeCtx->priv_data, "hw_encoding", 1, 0);
                        }
                        if (avcodec_open2(probeCtx, candidate, nullptr) >= 0) {
                            AVFrame* pf = av_frame_alloc();
                            AVPacket* pp = av_packet_alloc();
                            if (pf && pp) {
                                pf->format = probeCtx->pix_fmt;
                                pf->width = probeCtx->width;
                                pf->height = probeCtx->height;
                                if (av_frame_get_buffer(pf, 0) >= 0) {
                                    if (pf->data[0]) std::memset(pf->data[0], 16, static_cast<size_t>(pf->linesize[0]) * pf->height);
                                    if (pf->data[1]) std::memset(pf->data[1], 128, static_cast<size_t>(pf->linesize[1]) * (pf->height / 2));
                                    if (pf->data[2]) std::memset(pf->data[2], 128, static_cast<size_t>(pf->linesize[2]) * (pf->height / 2));
                                    for (int k = 0; k < 6 && attempt->extradata_size <= 0; ++k) {
                                        pf->pts = k;
                                        pf->pict_type = (k == 0) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
                                        avcodec_send_frame(probeCtx, pf);
                                        while (avcodec_receive_packet(probeCtx, pp) >= 0) {
                                            size_t sdBytes = 0;
                                            const uint8_t* sd = av_packet_get_side_data(pp, AV_PKT_DATA_NEW_EXTRADATA, &sdBytes);
                                            if (probeCtx->extradata && probeCtx->extradata_size > 0) {
                                                const int sz = probeCtx->extradata_size;
                                                attempt->extradata = static_cast<uint8_t*>(av_mallocz(static_cast<size_t>(sz) + AV_INPUT_BUFFER_PADDING_SIZE));
                                                if (attempt->extradata) {
                                                    std::memcpy(attempt->extradata, probeCtx->extradata, static_cast<size_t>(sz));
                                                    attempt->extradata_size = sz;
                                                }
                                            } else if (sd && sdBytes > 0) {
                                                attempt->extradata = static_cast<uint8_t*>(av_mallocz(sdBytes + AV_INPUT_BUFFER_PADDING_SIZE));
                                                if (attempt->extradata) {
                                                    std::memcpy(attempt->extradata, sd, sdBytes);
                                                    attempt->extradata_size = static_cast<int>(sdBytes);
                                                }
                                            }
                                            av_packet_unref(pp);
                                            if (attempt->extradata_size > 0) break;
                                        }
                                    }
                                }
                            }
                            if (pp) av_packet_free(&pp);
                            if (pf) av_frame_free(&pf);
                        }
                        avcodec_free_context(&probeCtx);
                    }
                    if (attempt->extradata_size <= 0) {
                        ret = AVERROR_EXTERNAL;
                    }
                }

                if (ret >= 0) {
                    chosen = candidate;
                    ctx = attempt;
                    break;
                }
                avcodec_free_context(&attempt);
            }
            if (chosen && ctx) break;
        }
        if (!chosen || !ctx) return false;
        m_videoCtx = ctx;

        m_videoStream = avformat_new_stream(m_fmtCtx, chosen);
        if (!m_videoStream) return false;
        if (avcodec_parameters_from_context(m_videoStream->codecpar, m_videoCtx) < 0) return false;
        m_videoStream->time_base = m_videoCtx->time_base;

        bool audioReady = false;
        if (cfg.enableAudio != 0) {
            audioReady = initAudio(fmt, cfg.audioSampleRate, audioEncName);
        }

        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&m_fmtCtx->pb, outputPathUtf8.c_str(), AVIO_FLAG_WRITE) < 0) {
                return false;
            }
        }
        AVDictionary* muxOpts = nullptr;
        if (fmt == RecordFormat::MP4_H264 || fmt == RecordFormat::MP4_H265) {
            av_dict_set(&muxOpts, "movflags", "+frag_keyframe+empty_moov+default_base_moof", 0);
        }
        const int hdrRet = avformat_write_header(m_fmtCtx, &muxOpts);
        av_dict_free(&muxOpts);
        if (hdrRet < 0) return false;
        m_headerWritten = true;

        m_videoFrame = av_frame_alloc();
        m_videoPkt = av_packet_alloc();
        if (!m_videoFrame || !m_videoPkt) return false;
        m_videoFrame->format = m_videoCtx->pix_fmt;
        m_videoFrame->width = cfg.width;
        m_videoFrame->height = cfg.height;
        if (av_frame_get_buffer(m_videoFrame, 0) < 0) return false;

        m_width = cfg.width;
        m_height = cfg.height;
        encName = chosen->name;
        reply.ok = 1;
        reply.hardwareEncoder = isHwEncoderName(chosen->name) ? 1 : 0;
        reply.audioEnabled = audioReady ? 1 : 0;
        reply.audioFrameSamples = (m_audioFrame != nullptr) ? m_audioFrame->nb_samples : 1024;
        return true;
    }

    bool encodeVideo(const uint8_t* srcData, const RecVideoFrameWire& wf) {
        if (!m_videoCtx || !m_videoFrame || !m_videoPkt || !srcData) return false;
        const AVPixelFormat srcFmt = (wf.pixelFormat == 0) ? AV_PIX_FMT_BGRA : AV_PIX_FMT_BGR24;
        if (!m_swsCtx || m_swsSrcFmt != srcFmt) {
            if (m_swsCtx) {
                sws_freeContext(m_swsCtx);
                m_swsCtx = nullptr;
            }
            m_swsCtx = sws_getContext(
                m_width, m_height, srcFmt,
                m_width, m_height, m_videoCtx->pix_fmt,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (!m_swsCtx) return false;
            m_swsSrcFmt = srcFmt;
        }

        if (av_frame_make_writable(m_videoFrame) < 0) return false;
        const uint8_t* srcSlice[] = {srcData};
        int srcStride[] = {wf.stride};
        if (sws_scale(m_swsCtx, srcSlice, srcStride, 0, m_height, m_videoFrame->data, m_videoFrame->linesize) <= 0) {
            return false;
        }
        m_videoFrame->pts = wf.pts;
        m_videoFrame->pict_type = (wf.isKeyFrame != 0) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (avcodec_send_frame(m_videoCtx, m_videoFrame) < 0) return false;
        return drainVideoPackets();
    }

    bool encodeAudio(const uint8_t* pcmBytes, int nbSamples, int64_t pts) {
        if (!m_audioCtx || !m_audioFrame || !m_audioSwr || !m_audioPkt || !pcmBytes) return false;
        if (nbSamples != m_audioFrame->nb_samples) return false;
        if (av_frame_make_writable(m_audioFrame) < 0) return false;
        const uint8_t* inPtr[] = {pcmBytes};
        const int converted = swr_convert(m_audioSwr, m_audioFrame->data, m_audioFrame->nb_samples, inPtr, nbSamples);
        if (converted != nbSamples) return false;
        m_audioFrame->pts = pts;
        m_audioFrame->duration = converted;
        if (avcodec_send_frame(m_audioCtx, m_audioFrame) < 0) return false;
        return drainAudioPackets();
    }

    bool finalize(bool& outCloseOk) {
        outCloseOk = true;
        bool ok = true;
        if (m_headerWritten && m_fmtCtx) {
            if (m_videoCtx) {
                if (avcodec_send_frame(m_videoCtx, nullptr) >= 0) {
                    if (!drainVideoPackets()) ok = false;
                } else {
                    ok = false;
                }
            }
            if (m_audioCtx) {
                if (avcodec_send_frame(m_audioCtx, nullptr) >= 0) {
                    if (!drainAudioPackets()) ok = false;
                }
            }
            if (av_write_trailer(m_fmtCtx) < 0) {
                ok = false;
            }
        }
        cleanup(outCloseOk);
        return ok && outCloseOk;
    }

private:
    bool initAudio(RecordFormat fmt, int sampleRate, std::string& audioEncName) {
        const AVCodecID codecId = (fmt == RecordFormat::WebM_VP9) ? AV_CODEC_ID_OPUS : AV_CODEC_ID_AAC;
        const AVCodec* codec = avcodec_find_encoder(codecId);
        if (!codec) return false;

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) return false;
        ctx->sample_rate = sampleRate;
        ctx->time_base = {1, sampleRate};
        ctx->bit_rate = 192'000;
        ctx->sample_fmt = pickAudioSampleFmt(codec);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (av_channel_layout_copy(&ctx->ch_layout, &stereo) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }
        if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
            ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }

        AVFrame* frame = av_frame_alloc();
        AVPacket* pkt = av_packet_alloc();
        SwrContext* swr = nullptr;
        auto rollback = [&]() {
            if (pkt) av_packet_free(&pkt);
            if (frame) av_frame_free(&frame);
            if (swr) swr_free(&swr);
            if (ctx) avcodec_free_context(&ctx);
        };
        if (!frame || !pkt) { rollback(); return false; }
        frame->format = ctx->sample_fmt;
        frame->sample_rate = ctx->sample_rate;
        frame->nb_samples = ctx->frame_size > 0 ? ctx->frame_size : 1024;
        if (av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout) < 0 ||
            av_frame_get_buffer(frame, 0) < 0) {
            rollback();
            return false;
        }
        AVChannelLayout inLayout = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate,
                                &inLayout, AV_SAMPLE_FMT_FLT, sampleRate, 0, nullptr) < 0 ||
            !swr || swr_init(swr) < 0) {
            rollback();
            return false;
        }
        AVStream* stream = avformat_new_stream(m_fmtCtx, codec);
        if (!stream || avcodec_parameters_from_context(stream->codecpar, ctx) < 0) {
            rollback();
            return false;
        }
        stream->time_base = ctx->time_base;

        m_audioCtx = ctx;
        m_audioStream = stream;
        m_audioFrame = frame;
        m_audioPkt = pkt;
        m_audioSwr = swr;
        audioEncName = codec->name;
        return true;
    }

    bool drainVideoPackets() {
        while (true) {
            const int r = avcodec_receive_packet(m_videoCtx, m_videoPkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) return false;
            av_packet_rescale_ts(m_videoPkt, m_videoCtx->time_base, m_videoStream->time_base);
            m_videoPkt->stream_index = m_videoStream->index;
            const int w = av_interleaved_write_frame(m_fmtCtx, m_videoPkt);
            av_packet_unref(m_videoPkt);
            if (w < 0) return false;
        }
    }

    bool drainAudioPackets() {
        while (true) {
            const int r = avcodec_receive_packet(m_audioCtx, m_audioPkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) return false;
            av_packet_rescale_ts(m_audioPkt, m_audioCtx->time_base, m_audioStream->time_base);
            m_audioPkt->stream_index = m_audioStream->index;
            const int w = av_interleaved_write_frame(m_fmtCtx, m_audioPkt);
            av_packet_unref(m_audioPkt);
            if (w < 0) return false;
        }
    }

    void cleanup(bool& outCloseOk) {
        if (m_audioPkt) { av_packet_free(&m_audioPkt); m_audioPkt = nullptr; }
        if (m_audioSwr) { swr_free(&m_audioSwr); m_audioSwr = nullptr; }
        if (m_audioFrame) { av_frame_free(&m_audioFrame); m_audioFrame = nullptr; }
        if (m_audioCtx) { avcodec_free_context(&m_audioCtx); m_audioCtx = nullptr; }
        if (m_videoPkt) { av_packet_free(&m_videoPkt); m_videoPkt = nullptr; }
        if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
        if (m_videoFrame) { av_frame_free(&m_videoFrame); m_videoFrame = nullptr; }
        if (m_videoCtx) { avcodec_free_context(&m_videoCtx); m_videoCtx = nullptr; }
        if (m_fmtCtx) {
            if (m_fmtCtx->pb && avio_closep(&m_fmtCtx->pb) < 0) {
                outCloseOk = false;
            }
            avformat_free_context(m_fmtCtx);
            m_fmtCtx = nullptr;
        }
        m_videoStream = nullptr;
        m_audioStream = nullptr;
        m_headerWritten = false;
    }

    AVFormatContext* m_fmtCtx = nullptr;
    AVCodecContext* m_videoCtx = nullptr;
    AVStream* m_videoStream = nullptr;
    AVFrame* m_videoFrame = nullptr;
    AVPacket* m_videoPkt = nullptr;
    SwsContext* m_swsCtx = nullptr;
    AVPixelFormat m_swsSrcFmt = AV_PIX_FMT_NONE;
    AVCodecContext* m_audioCtx = nullptr;
    AVStream* m_audioStream = nullptr;
    AVFrame* m_audioFrame = nullptr;
    AVPacket* m_audioPkt = nullptr;
    SwrContext* m_audioSwr = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_headerWritten = false;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 宿主侧 RecordEncodeWorkerClient 实现
// ─────────────────────────────────────────────────────────────────────────────

RecordEncodeWorkerClient::~RecordEncodeWorkerClient() {
    abortAndClose();
}

bool RecordEncodeWorkerClient::isAvailableForFormat(
    RecordFormat format,
    const std::filesystem::path& workerExeOverride) {
    if (format == RecordFormat::GIF) {
        return false;
    }
    if (worker::isRunningInsideWorkerProcess() || worker::envFlagEnabled(L"TOOLS3000_RECORD_ENCODE_INPROC")) {
        return false;
    }
    return !worker::resolveHostWorkerExecutablePath(workerExeOverride).empty();
}

bool RecordEncodeWorkerClient::start(
    const RecordEncodeInitConfig& cfg,
    RecordEncodeInitResult& outResult,
    const std::filesystem::path& workerExeOverride) {
    abortAndClose();
    outResult = {};
    if (!isAvailableForFormat(cfg.format, workerExeOverride)) {
        return false;
    }
    if (cfg.width <= 0 || cfg.height <= 0 || cfg.fps <= 0 || cfg.workingOutputPathUtf8.empty()) {
        return false;
    }

    const std::filesystem::path exePath = worker::resolveHostWorkerExecutablePath(workerExeOverride);
    if (exePath.empty()) return false;

    const size_t slotBytes = (std::max<size_t>)(65536ULL, static_cast<size_t>(cfg.width) * static_cast<size_t>(cfg.height) * 4ULL);
    const std::wstring shmName = worker::makeLocalObjectName(L"RecFrame");
    const DWORD sizeHigh = static_cast<DWORD>(static_cast<uint64_t>(slotBytes) >> 32);
    const DWORD sizeLow = static_cast<DWORD>(static_cast<uint64_t>(slotBytes) & 0xFFFFFFFFULL);
    worker::ScopedHandle shm(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, sizeHigh, sizeLow, shmName.c_str()));
    if (!shm) return false;
    worker::ScopedMapView view(MapViewOfFile(shm.get(), FILE_MAP_WRITE, 0, 0, slotBytes));
    if (!view) return false;

    const std::wstring pipeName = worker::makeLocalPipeName(L"RecEncode");
    worker::ScopedHandle pipeServer(CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        worker::Constants::kPipeBufferBytes,
        worker::Constants::kPipeBufferBytes,
        0,
        nullptr));
    if (!pipeServer) return false;

    const std::vector<std::wstring> args = {
        std::wstring(worker::Constants::kFlagWorkerMode) + worker::Constants::kModeRecordEncode,
        std::wstring(worker::Constants::kFlagPipeName) + pipeName,
        std::wstring(worker::Constants::kFlagParentPid) + std::to_wstring(GetCurrentProcessId()),
    };
    worker::ScopedHandle childProc;
    DWORD childPid = 0;
    if (!worker::launchHiddenWorkerProcess(exePath, args, childProc, &childPid)) {
        return false;
    }
    if (!worker::waitForPipeClientWithTimeout(pipeServer.get(), childProc.get(), worker::Constants::kPipeConnectTimeoutMs)) {
        TerminateProcess(childProc.get(), 1);
        return false;
    }

    RecCmdHeader cmd{};
    cmd.command = static_cast<uint32_t>(RecCommand::Init);
    RecInitWire initWire{};
    initWire.format = static_cast<int32_t>(cfg.format);
    initWire.width = cfg.width;
    initWire.height = cfg.height;
    initWire.fps = cfg.fps;
    initWire.bitrateMbps = cfg.bitrateMbps;
    initWire.enableAudio = cfg.enableAudio ? 1 : 0;
    initWire.audioSampleRate = cfg.audioSampleRate;
    initWire.audioChannels = cfg.audioChannels;
    initWire.frameSlotBytes = static_cast<uint64_t>(slotBytes);

    const std::string shmNameUtf8 = tools3000::core::WinUtils::wstringToUtf8(shmName);
    if (!worker::writePod(pipeServer.get(), cmd, /*overlapped=*/true) ||
        !worker::writePod(pipeServer.get(), initWire, /*overlapped=*/true) ||
        !worker::writeString(pipeServer.get(), shmNameUtf8, /*overlapped=*/true) ||
        !worker::writeString(pipeServer.get(), cfg.workingOutputPathUtf8, /*overlapped=*/true) ||
        !worker::writeString(pipeServer.get(), cfg.traceId, /*overlapped=*/true)) {
        TerminateProcess(childProc.get(), 1);
        return false;
    }

    RecInitReplyWire reply{};
    std::string encName;
    std::string audioEncName;
    if (!worker::readPod(pipeServer.get(), reply, /*overlapped=*/true, kRecInitTimeoutMs) ||
        !worker::readString(pipeServer.get(), encName, /*overlapped=*/true, kRecInitTimeoutMs) ||
        !worker::readString(pipeServer.get(), audioEncName, /*overlapped=*/true, kRecInitTimeoutMs) ||
        reply.ok == 0) {
        TerminateProcess(childProc.get(), 1);
        return false;
    }

    m_pipe = std::move(pipeServer);
    m_childProcess = std::move(childProc);
    m_frameShm = std::move(shm);
    m_frameView = std::move(view);
    m_frameSlotBytes = slotBytes;
    m_childPid = childPid;
    m_audioEnabled = (reply.audioEnabled != 0);
    m_audioFrameSamples = (reply.audioFrameSamples > 0) ? reply.audioFrameSamples : 1024;
    m_finalized = false;

    outResult.ok = true;
    outResult.encoderName = std::move(encName);
    outResult.hardwareEncoder = (reply.hardwareEncoder != 0);
    outResult.audioEnabled = m_audioEnabled;
    outResult.audioEncoderName = std::move(audioEncName);
    outResult.audioFrameSamples = m_audioFrameSamples;

    LOG_INFO("RecordEncodeWorker: isolated encoder started (pid={}, video={}, hw={}, audio={})",
             m_childPid, outResult.encoderName, outResult.hardwareEncoder,
             outResult.audioEnabled ? outResult.audioEncoderName : "none");
    return true;
}

bool RecordEncodeWorkerClient::stageVideoFrame(
    const uint8_t* data,
    int width,
    int height,
    int stride,
    CapturePixelFormat format) {
    if (!m_pipe || !m_frameView || !data || width <= 0 || height <= 0 || stride <= 0) {
        return false;
    }
    const int bpp = (format == CapturePixelFormat::Bgra32) ? 4 : 3;
    const int packedStride = width * bpp;
    const size_t totalBytes = static_cast<size_t>(packedStride) * static_cast<size_t>(height);
    if (totalBytes > m_frameSlotBytes) {
        return false;
    }

    uint8_t* dst = m_frameView.bytes();
    if (stride == packedStride) {
        std::memcpy(dst, data, totalBytes);
    } else {
        for (int y = 0; y < height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * packedStride,
                        data + static_cast<size_t>(y) * stride,
                        static_cast<size_t>(packedStride));
        }
    }
    m_stagedWidth = width;
    m_stagedHeight = height;
    m_stagedStride = packedStride;
    m_stagedPixelFormat = (format == CapturePixelFormat::Bgra32) ? 0 : 1;
    m_hasStagedFrame = true;
    return true;
}

bool RecordEncodeWorkerClient::commitStagedVideoFrame(int64_t pts, bool isKeyFrame) {
    if (!m_pipe || !m_hasStagedFrame) {
        return false;
    }
    RecCmdHeader cmd{};
    cmd.command = static_cast<uint32_t>(RecCommand::VideoFrame);
    RecVideoFrameWire wf{};
    wf.pts = pts;
    wf.width = m_stagedWidth;
    wf.height = m_stagedHeight;
    wf.stride = m_stagedStride;
    wf.pixelFormat = m_stagedPixelFormat;
    wf.isKeyFrame = isKeyFrame ? 1 : 0;

    RecAckWire ack{};
    if (!worker::writePod(m_pipe.get(), cmd, /*overlapped=*/true) ||
        !worker::writePod(m_pipe.get(), wf, /*overlapped=*/true) ||
        !worker::readPod(m_pipe.get(), ack, /*overlapped=*/true, kRecFrameTimeoutMs)) {
        return false;
    }
    return ack.ok != 0;
}

bool RecordEncodeWorkerClient::sendVideoFrame(
    const uint8_t* data,
    int width,
    int height,
    int stride,
    CapturePixelFormat format,
    int64_t pts,
    bool isKeyFrame) {
    if (!stageVideoFrame(data, width, height, stride, format)) {
        return false;
    }
    return commitStagedVideoFrame(pts, isKeyFrame);
}

bool RecordEncodeWorkerClient::sendAudioSamples(
    const float* interleavedSamples,
    int nbSamples,
    int channels,
    int64_t pts) {
    if (!m_pipe || !m_audioEnabled || !interleavedSamples || nbSamples <= 0 || channels <= 0) {
        return false;
    }
    const uint32_t byteCount = static_cast<uint32_t>(nbSamples) * static_cast<uint32_t>(channels) * sizeof(float);
    RecCmdHeader cmd{};
    cmd.command = static_cast<uint32_t>(RecCommand::AudioChunk);
    RecAudioChunkWire ac{};
    ac.pts = pts;
    ac.nbSamples = nbSamples;
    ac.channels = channels;
    ac.byteCount = byteCount;

    RecAckWire ack{};
    if (!worker::writePod(m_pipe.get(), cmd, /*overlapped=*/true) ||
        !worker::writePod(m_pipe.get(), ac, /*overlapped=*/true) ||
        !worker::writeExact(m_pipe.get(), interleavedSamples, byteCount, /*overlapped=*/true) ||
        !worker::readPod(m_pipe.get(), ack, /*overlapped=*/true, kRecFrameTimeoutMs)) {
        return false;
    }
    return ack.ok != 0;
}

bool RecordEncodeWorkerClient::finalize(bool& outCloseOk) {
    outCloseOk = true;
    if (!m_pipe) return false;
    m_finalized = true;

    RecCmdHeader cmd{};
    cmd.command = static_cast<uint32_t>(RecCommand::Finalize);
    RecAckWire ack{};
    const bool ioOk =
        worker::writePod(m_pipe.get(), cmd, /*overlapped=*/true) &&
        worker::readPod(m_pipe.get(), ack, /*overlapped=*/true, kRecFinalTimeoutMs);

    outCloseOk = ioOk && (ack.extra != 0);
    const bool ok = ioOk && (ack.ok != 0);
    abortAndClose();
    return ok;
}

void RecordEncodeWorkerClient::abortAndClose() {
    if (m_pipe) {
        if (!m_finalized) {
            RecCmdHeader cmd{};
            cmd.command = static_cast<uint32_t>(RecCommand::Abort);
            worker::writePod(m_pipe.get(), cmd, /*overlapped=*/true);
            FlushFileBuffers(m_pipe.get());
        }
        DisconnectNamedPipe(m_pipe.get());
        m_pipe.reset();
    }
    m_frameView.reset();
    m_frameShm.reset();
    m_frameSlotBytes = 0;
    m_stagedWidth = 0;
    m_stagedHeight = 0;
    m_stagedStride = 0;
    m_stagedPixelFormat = 0;
    m_hasStagedFrame = false;
    if (m_childProcess) {
        if (WaitForSingleObject(m_childProcess.get(), worker::Constants::kWorkerGracefulExitWaitMs) == WAIT_TIMEOUT) {
            TerminateProcess(m_childProcess.get(), 0);
            WaitForSingleObject(m_childProcess.get(), 1000);
        }
        m_childProcess.reset();
    }
    m_childPid = 0;
    m_audioEnabled = false;
    m_finalized = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// 子进程侧 runRecordEncodeWorkerProcess 实现
// ─────────────────────────────────────────────────────────────────────────────

int runRecordEncodeWorkerProcess(int argc, const wchar_t* const* argv) {
    const std::wstring pipeName = worker::findCliOptionValue(argc, argv, worker::Constants::kFlagPipeName);
    const std::wstring parentPidStr = worker::findCliOptionValue(argc, argv, worker::Constants::kFlagParentPid);
    const DWORD parentPid = parentPidStr.empty() ? 0u : static_cast<DWORD>(std::wcstoul(parentPidStr.c_str(), nullptr, 10));
    if (pipeName.empty()) {
        return 2;
    }

    worker::ParentProcessWatchdog watchdog(parentPid);
    worker::ScopedHandle pipe = worker::connectPipeClientWithRetry(pipeName, worker::Constants::kPipeConnectTimeoutMs);
    if (!pipe) {
        return 3;
    }

    WorkerEncoderSession session;
    worker::ScopedHandle shm;
    worker::ScopedMapView shmView;
    uint64_t slotBytes = 0;
    std::vector<uint8_t> audioScratch;
    std::optional<tools3000::core::TraceId::Scope> traceScope;

    while (true) {
        RecCmdHeader hdr{};
        if (!worker::readPod(pipe.get(), hdr, /*overlapped=*/false)) {
            break;
        }
        if (hdr.magic != kRecProtoMagic || hdr.version != kRecProtoVersion) {
            break;
        }
        const auto cmd = static_cast<RecCommand>(hdr.command);
        if (cmd == RecCommand::Init) {
            RecInitWire initWire{};
            std::string shmNameUtf8;
            std::string outPathUtf8;
            std::string traceId;
            if (!worker::readPod(pipe.get(), initWire, /*overlapped=*/false) ||
                !worker::readString(pipe.get(), shmNameUtf8, /*overlapped=*/false) ||
                !worker::readString(pipe.get(), outPathUtf8, /*overlapped=*/false) ||
                !worker::readString(pipe.get(), traceId, /*overlapped=*/false)) {
                break;
            }
            if (!traceId.empty()) {
                traceScope.emplace(traceId);
            }
            const std::wstring shmNameW = tools3000::core::WinUtils::utf8ToWstring(shmNameUtf8);
            shm.reset(OpenFileMappingW(FILE_MAP_READ, FALSE, shmNameW.c_str()));
            slotBytes = initWire.frameSlotBytes;
            if (shm && slotBytes > 0) {
                shmView.reset(MapViewOfFile(shm.get(), FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(slotBytes)));
            }

            RecInitReplyWire reply{};
            std::string encName;
            std::string audioEncName;
            if (shmView) {
                session.init(initWire, outPathUtf8, reply, encName, audioEncName);
            }
            if (!worker::writePod(pipe.get(), reply, /*overlapped=*/false) ||
                !worker::writeString(pipe.get(), encName, /*overlapped=*/false) ||
                !worker::writeString(pipe.get(), audioEncName, /*overlapped=*/false)) {
                break;
            }
            if (reply.ok == 0) {
                break;
            }
        } else if (cmd == RecCommand::VideoFrame) {
            RecVideoFrameWire wf{};
            if (!worker::readPod(pipe.get(), wf, /*overlapped=*/false)) {
                break;
            }
            const bool boundsOk = shmView && wf.stride > 0 && wf.height > 0 &&
                static_cast<uint64_t>(wf.stride) * static_cast<uint64_t>(wf.height) <= slotBytes;
            const bool encOk = boundsOk && session.encodeVideo(shmView.bytes(), wf);
            RecAckWire ack{encOk ? 1 : 0, 0};
            if (!worker::writePod(pipe.get(), ack, /*overlapped=*/false) || !encOk) {
                break;
            }
        } else if (cmd == RecCommand::AudioChunk) {
            RecAudioChunkWire ac{};
            if (!worker::readPod(pipe.get(), ac, /*overlapped=*/false)) {
                break;
            }
            if (ac.byteCount == 0 || ac.byteCount > 8u * 1024u * 1024u) {
                break;
            }
            audioScratch.resize(ac.byteCount);
            if (!worker::readExact(pipe.get(), audioScratch.data(), ac.byteCount, /*overlapped=*/false)) {
                break;
            }
            const bool encOk = session.encodeAudio(audioScratch.data(), ac.nbSamples, ac.pts);
            RecAckWire ack{encOk ? 1 : 0, 0};
            if (!worker::writePod(pipe.get(), ack, /*overlapped=*/false)) {
                break;
            }
        } else if (cmd == RecCommand::Finalize) {
            bool closeOk = true;
            const bool finOk = session.finalize(closeOk);
            RecAckWire ack{finOk ? 1 : 0, closeOk ? 1 : 0};
            worker::writePod(pipe.get(), ack, /*overlapped=*/false);
            FlushFileBuffers(pipe.get());
            break;
        } else {
            break;
        }
    }

    shmView.reset();
    shm.reset();
    pipe.reset();
    watchdog.stop();
    return 0;
}

} // namespace tools3000::capture
