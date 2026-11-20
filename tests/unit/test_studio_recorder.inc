// ─────────────────────────────────────────────────────────────────────────────
// test_studio_recorder.inc — Screen Studio 级自动运镜、光标平滑、壁纸合成与转码回归测试
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/ScreenRecorder.h"
#include "capture/StudioTimeline.h"
#include "capture/StudioVideoCompositor.h"
#include "capture/StudioPreviewWindow.h"
#include "capture/VoiceEnhancer.h"
#include "capture/WatermarkRenderer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

bool writeSyntheticCleanMp4ForStudioTest(const std::string& outPathUtf8, int w, int h, int fps, int numFrames) {
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "mp4", outPathUtf8.c_str()) < 0 || !fmt) {
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!codec) {
        avformat_free_context(fmt);
        return false;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->width = w;
    ctx->height = h;
    ctx->time_base = {1, fps};
    ctx->framerate = {fps, 1};
    ctx->gop_size = fps;
    ctx->max_b_frames = 0;
    ctx->bit_rate = 2'000'000;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_free_context(fmt);
        return false;
    }

    AVStream* st = avformat_new_stream(fmt, codec);
    if (!st || avcodec_parameters_from_context(st->codecpar, ctx) < 0 ||
        avio_open(&fmt->pb, outPathUtf8.c_str(), AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(fmt, nullptr) < 0) {
        if (fmt->pb) avio_closep(&fmt->pb);
        avcodec_free_context(&ctx);
        avformat_free_context(fmt);
        return false;
    }
    st->time_base = ctx->time_base;

    SwsContext* sws = sws_getContext(w, h, AV_PIX_FMT_BGR24, w, h, ctx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    AVFrame* frm = av_frame_alloc();
    frm->format = ctx->pix_fmt;
    frm->width = w;
    frm->height = h;
    av_frame_get_buffer(frm, 0);
    AVPacket* pkt = av_packet_alloc();

    auto drain = [&]() {
        while (avcodec_receive_packet(ctx, pkt) >= 0) {
            av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
            av_packet_unref(pkt);
        }
    };

    for (int i = 0; i < numFrames; ++i) {
        cv::Mat img(h, w, CV_8UC3, cv::Scalar(242, 245, 250));
        // 绘制一个清晰的红色按钮目标区在左上象限 (120, 90)
        cv::rectangle(img, cv::Rect(90, 70, 80, 40), cv::Scalar(60, 70, 235), cv::FILLED);
        cv::rectangle(img, cv::Rect(20 + i * 6, h - 50, 40, 24), cv::Scalar(220, 140, 40), cv::FILLED);

        av_frame_make_writable(frm);
        const uint8_t* srcSlice[4] = {img.data, nullptr, nullptr, nullptr};
        int srcStride[4] = {static_cast<int>(img.step[0]), 0, 0, 0};
        sws_scale(sws, srcSlice, srcStride, 0, h, frm->data, frm->linesize);
        frm->pts = i;
        avcodec_send_frame(ctx, frm);
        drain();
    }
    avcodec_send_frame(ctx, nullptr);
    drain();
    av_write_trailer(fmt);

    sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&frm);
    avio_closep(&fmt->pb);
    avcodec_free_context(&ctx);
    avformat_free_context(fmt);
    return true;
}

} // namespace

TEST(StudioRecorderTest, OneEuroFilterSuppressesMicroJitterAndTracksFastSwipe) {
    using namespace tools3000::capture;

    OneEuroFilter2D filter(1.35, 0.018, 1.0);
    double rawDeltaSum = 0.0;
    double filteredDeltaSum = 0.0;
    float prevRawX = 500.0f;
    float prevOutX = 500.0f;

    // 1. 慢速悬停手部微抖动 (+-3px 高频抖动)
    for (int i = 0; i < 60; ++i) {
        const double t = i * 0.004; // 250Hz
        const float jitter = (i % 2 == 0) ? 3.0f : -3.0f;
        const float rawX = 500.0f + jitter;
        float outX = 0.0f, outY = 0.0f;
        filter.filter(rawX, 300.0f, t, outX, outY);
        if (i > 2) {
            rawDeltaSum += std::abs(rawX - prevRawX);
            filteredDeltaSum += std::abs(outX - prevOutX);
        }
        prevRawX = rawX;
        prevOutX = outX;
    }
    EXPECT_LT(filteredDeltaSum, rawDeltaSum * 0.35);

    // 2. 高速跨屏甩动应迅速跟上目标，不粘滞
    float fastX = 0.0f, fastY = 0.0f;
    for (int i = 0; i < 25; ++i) {
        const double t = 0.30 + i * 0.008;
        filter.filter(1400.0f, 700.0f, t, fastX, fastY);
    }
    EXPECT_GT(fastX, 1340.0f);
    EXPECT_GT(fastY, 670.0f);
}

TEST(StudioRecorderTest, CriticallyDampedSpringConvergesMonotonicallyWithoutOvershoot) {
    using namespace tools3000::capture;

    CriticallyDampedSpring1D spring(8.0);
    spring.reset(1.0, 0.0);

    double prev = 1.0;
    for (int i = 0; i < 90; ++i) {
        const double cur = spring.step(2.0, 1.0 / 60.0);
        EXPECT_GE(cur, prev - 1e-9);
        EXPECT_LE(cur, 2.0 + 1e-6); // 严格零超调
        prev = cur;
    }
    EXPECT_NEAR(spring.position(), 2.0, 0.005);
}

TEST(StudioRecorderTest, AutoZoomClusteringAndViewportBoundaryClamping) {
    using namespace tools3000::capture;

    StudioTimelineData tl;
    tl.sourceWidth = 1920;
    tl.sourceHeight = 1080;
    tl.fps = 30;
    tl.durationMs = 6000;
    // 左上极角连续两次点击 -> 应聚类合并为 1 段放大，且裁剪框钳制不小于 0
    tl.clickEvents.push_back({1000, 14.0f, 12.0f, false});
    tl.clickEvents.push_back({1450, 28.0f, 20.0f, false});
    // 右下极角键盘打字焦点 -> 形成第 2 段放大，且裁剪框右/下边界严格不超过 1920x1080
    tl.focusEvents.push_back({4200, 1912.0f, 1074.0f});

    tl.zoomSegments = StudioTimelineAnalyzer::buildAutoZoomSegments(tl, 2.0f);
    ASSERT_EQ(tl.zoomSegments.size(), 2u);
    EXPECT_LT(tl.zoomSegments[0].startMs, 1000);
    EXPECT_GT(tl.zoomSegments[0].endMs, 2400);

    StudioRenderConfig cfg;
    cfg.autoZoomEnabled = true;
    cfg.defaultZoomFactor = 2.0f;
    const auto baked = StudioTimelineAnalyzer::bakeEvaluatedFrames(tl, cfg, 30, 180);
    ASSERT_EQ(baked.size(), 180u);

    bool sawZoomedIn = false;
    for (const auto& f : baked) {
        EXPECT_GE(f.camera.cropX, 0.0f);
        EXPECT_GE(f.camera.cropY, 0.0f);
        EXPECT_LE(f.camera.cropX + f.camera.cropW, 1920.05f);
        EXPECT_LE(f.camera.cropY + f.camera.cropH, 1080.05f);
        if (f.camera.zoom > 1.65f) {
            sawZoomedIn = true;
        }
    }
    EXPECT_TRUE(sawZoomedIn);
}

TEST(StudioRecorderTest, SubRipSrtFormattingAndExport) {
    using namespace tools3000::capture;

    std::vector<StudioCaptionSegment> caps = {
        {250, 1800, "欢迎使用演播室自动运镜录屏"},
        {2100, 4050, "Auto-zoom and crisp vector cursor"}
    };
    const std::string srt = StudioTimelineAnalyzer::formatSrtContent(caps);
    EXPECT_NE(srt.find("00:00:00,250 --> 00:00:01,800"), std::string::npos);
    EXPECT_NE(srt.find("欢迎使用演播室自动运镜录屏"), std::string::npos);
    EXPECT_NE(srt.find("00:00:02,100 --> 00:00:04,050"), std::string::npos);
}

TEST(StudioRecorderTest, CompositorWallpaperZoomVectorCursorAndEndToEndMp4Export) {
    using namespace tools3000::capture;

    const auto tmpDir = std::filesystem::temp_directory_path() / L"tools3000_studio_test";
    std::error_code ec;
    std::filesystem::create_directories(tmpDir, ec);
    const std::wstring rawPathW = (tmpDir / L"clean_plate.mp4").wstring();
    const std::wstring outPathW = (tmpDir / L"studio_output.mp4").wstring();
    const std::wstring srtPathW = (tmpDir / L"studio_output.srt").wstring();
    const std::string rawUtf8 = tools3000::core::WinUtils::wstringToUtf8(rawPathW);
    const std::string outUtf8 = tools3000::core::WinUtils::wstringToUtf8(outPathW);

    ASSERT_TRUE(writeSyntheticCleanMp4ForStudioTest(rawUtf8, 640, 360, 24, 24));

    std::vector<cv::Mat> deck;
    int origW = 0, origH = 0;
    double durSec = 0.0;
    ASSERT_TRUE(StudioVideoCompositor::extractPreviewDeck(rawUtf8, 480, 30, deck, origW, origH, durSec));
    EXPECT_FALSE(deck.empty());
    EXPECT_EQ(origW, 640);
    EXPECT_EQ(origH, 360);

    StudioTimelineData tl;
    tl.sourceWidth = 640;
    tl.sourceHeight = 360;
    tl.fps = 24;
    tl.durationMs = 1000;
    for (int i = 0; i <= 24; ++i) {
        tl.cursorSamples.push_back({
            i * 40,
            100.0f + i * 3.0f,
            85.0f + i * 1.5f,
            StudioCursorKind::Arrow,
            i == 5,
            false
        });
    }
    tl.clickEvents.push_back({200, 125.0f, 90.0f, false});
    tl.captions.push_back({100, 900, "关键帧放大与平滑光标字幕对齐验证"});
    tl.zoomSegments = StudioTimelineAnalyzer::buildAutoZoomSegments(tl, 1.85f);

    StudioRenderConfig cfg;
    cfg.autoZoomEnabled = true;
    cfg.defaultZoomFactor = 1.85f;
    cfg.smoothCursorEnabled = true;
    cfg.cursorScale = 1.35f;
    cfg.autoFrameEnabled = true;
    cfg.backgroundTheme = BeautyBackgroundType::AuroraPurple;
    cfg.autoCaptionsEnabled = true;
    cfg.exportSrtSidecar = true;

    double lastProg = 0.0;
    ASSERT_TRUE(StudioVideoCompositor::exportRenderedVideo(
        rawUtf8, outUtf8, tl, cfg, 6, [&](double p) { lastProg = p; }));
    EXPECT_DOUBLE_EQ(lastProg, 1.0);
    EXPECT_TRUE(std::filesystem::exists(outPathW, ec));
    EXPECT_GT(std::filesystem::file_size(outPathW, ec), 1024u);
    EXPECT_TRUE(std::filesystem::exists(srtPathW, ec));

    // 重新解码导出的成片 MP4 验证合法封装与帧数
    std::vector<cv::Mat> outDeck;
    int renderedW = 0, renderedH = 0;
    double renderedDur = 0.0;
    ASSERT_TRUE(StudioVideoCompositor::extractPreviewDeck(outUtf8, 640, 30, outDeck, renderedW, renderedH, renderedDur));
    EXPECT_GE(outDeck.size(), 20u);
    EXPECT_EQ(renderedW, 640);
    EXPECT_EQ(renderedH, 360);

    std::filesystem::remove_all(tmpDir, ec);
}

TEST(StudioRecorderTest, SparseVfrRecordingPreservesFullDurationWithoutFastForwardCompression) {
    using namespace tools3000::capture;

    const auto tmpDir = std::filesystem::temp_directory_path() / L"tools3000_studio_vfr_test";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);
    const std::string rawUtf8 = tools3000::core::WinUtils::wstringToUtf8((tmpDir / L"sparse_vfr.mp4").wstring());
    const std::string outUtf8 = tools3000::core::WinUtils::wstringToUtf8((tmpDir / L"studio_cfr_out.mp4").wstring());

    // 构造一段模拟 4K 掉帧的稀疏 VFR 视频：仅编码 10 个物理帧，但 PTS 横跨 3.0 秒 (time_base = 1/30, pts = 0, 10, 20...90)
    {
        AVFormatContext* fmt = nullptr;
        ASSERT_GE(avformat_alloc_output_context2(&fmt, nullptr, "mp4", rawUtf8.c_str()), 0);
        ASSERT_NE(fmt, nullptr);
        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        ASSERT_NE(codec, nullptr);

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        ctx->width = 640;
        ctx->height = 360;
        ctx->time_base = {1, 30};
        ctx->framerate = {30, 1};
        ctx->gop_size = 15;
        ctx->max_b_frames = 0;
        ctx->bit_rate = 1'500'000;
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ASSERT_GE(avcodec_open2(ctx, codec, nullptr), 0);

        AVStream* st = avformat_new_stream(fmt, codec);
        ASSERT_NE(st, nullptr);
        ASSERT_GE(avcodec_parameters_from_context(st->codecpar, ctx), 0);
        st->time_base = ctx->time_base;
        ASSERT_GE(avio_open(&fmt->pb, rawUtf8.c_str(), AVIO_FLAG_WRITE), 0);
        ASSERT_GE(avformat_write_header(fmt, nullptr), 0);

        SwsContext* sws = sws_getContext(640, 360, AV_PIX_FMT_BGR24, 640, 360, ctx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
        AVFrame* frm = av_frame_alloc();
        frm->format = ctx->pix_fmt;
        frm->width = 640;
        frm->height = 360;
        av_frame_get_buffer(frm, 0);
        AVPacket* pkt = av_packet_alloc();

        auto drain = [&]() {
            while (avcodec_receive_packet(ctx, pkt) >= 0) {
                av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
                pkt->stream_index = st->index;
                av_interleaved_write_frame(fmt, pkt);
                av_packet_unref(pkt);
            }
        };

        for (int i = 0; i < 10; ++i) {
            cv::Mat img(360, 640, CV_8UC3, cv::Scalar(235, 240, 245));
            cv::rectangle(img, cv::Rect(40 + i * 45, 120, 60, 50), cv::Scalar(40, 110, 240), cv::FILLED);
            av_frame_make_writable(frm);
            const uint8_t* srcSlice[4] = {img.data, nullptr, nullptr, nullptr};
            int srcStride[4] = {static_cast<int>(img.step[0]), 0, 0, 0};
            sws_scale(sws, srcSlice, srcStride, 0, 360, frm->data, frm->linesize);
            frm->pts = static_cast<int64_t>(i) * 10; // 每帧跨 10/30 秒 = 333ms，共约 3.0 秒
            avcodec_send_frame(ctx, frm);
            drain();
        }
        avcodec_send_frame(ctx, nullptr);
        drain();
        av_write_trailer(fmt);
        sws_freeContext(sws);
        av_frame_free(&frm);
        av_packet_free(&pkt);
        avio_closep(&fmt->pb);
        avcodec_free_context(&ctx);
        avformat_free_context(fmt);
    }

    StudioTimelineData tl;
    tl.sourceWidth = 640;
    tl.sourceHeight = 360;
    tl.fps = 30;
    tl.durationMs = 3050;
    tl.clickEvents.push_back({600, 180.0f, 150.0f, false});
    tl.zoomSegments = StudioTimelineAnalyzer::buildAutoZoomSegments(tl, 1.50f);

    StudioRenderConfig cfg;
    cfg.autoZoomEnabled = true;
    cfg.defaultZoomFactor = 1.50f;
    cfg.smoothCursorEnabled = true;
    cfg.autoFrameEnabled = true;
    cfg.exportSrtSidecar = false;

    ASSERT_TRUE(StudioVideoCompositor::exportRenderedVideo(rawUtf8, outUtf8, tl, cfg, 4));

    // 解码校验输出视频：10 帧 VFR 绝不能被压缩成 10/30 = 0.33s，必须完整输出 ~90 帧 (3.0s CFR)！
    std::vector<cv::Mat> verifyDeck;
    int outW = 0, outH = 0;
    double outDur = 0.0;
    ASSERT_TRUE(StudioVideoCompositor::extractPreviewDeck(outUtf8, 320, 200, verifyDeck, outW, outH, outDur));
    EXPECT_GE(outDur, 2.85);
    EXPECT_GE(verifyDeck.size(), 80u);

    std::filesystem::remove_all(tmpDir, ec);
}

TEST(StudioRecorderTest, AllEightArtisticBackdropThemesAndThumbnailsRenderDistinctHighEndVisuals) {
    using namespace tools3000::capture;

    std::vector<cv::Scalar> meanColors;
    for (int idx = 0; idx < static_cast<int>(BeautyBackgroundType::COUNT); ++idx) {
        const auto theme = static_cast<BeautyBackgroundType>(idx);
        cv::Mat bg;
        StudioVideoCompositor::renderArtisticBackdropBgr(bg, 320, 180, theme);
        ASSERT_EQ(bg.cols, 320);
        ASSERT_EQ(bg.rows, 180);
        ASSERT_EQ(bg.type(), CV_8UC3);

        cv::Scalar meanVal, stdVal;
        cv::meanStdDev(bg, meanVal, stdVal);
        const double totalStd = stdVal[0] + stdVal[1] + stdVal[2];
        // 每款高定背景均包含多层柔光网格或远山风景渐变，绝非死板纯色块
        EXPECT_GT(totalStd, 4.5) << "Theme index " << idx << " lacks tonal depth";
        meanColors.push_back(meanVal);

        cv::Mat thumb = StudioVideoCompositor::renderThemeThumbnailBgra(theme, 96, 54);
        ASSERT_EQ(thumb.cols, 96);
        ASSERT_EQ(thumb.rows, 54);
        ASSERT_EQ(thumb.type(), CV_8UC4);
    }

    // 校验风景系列（暮色苍山 DuskMountains=6 与 拂晓湖山 MistyLakeDawn=7）天空与山峦/湖面具有显著层次差异
    for (BeautyBackgroundType landscapeTheme : {BeautyBackgroundType::DuskMountains, BeautyBackgroundType::MistyLakeDawn}) {
        cv::Mat scene;
        StudioVideoCompositor::renderArtisticBackdropBgr(scene, 320, 200, landscapeTheme);
        const cv::Scalar skyMean = cv::mean(scene(cv::Rect(0, 0, 320, 60)));
        const cv::Scalar ridgeMean = cv::mean(scene(cv::Rect(0, 120, 320, 80)));
        const double l1Diff = std::abs(skyMean[0] - ridgeMean[0]) +
                              std::abs(skyMean[1] - ridgeMean[1]) +
                              std::abs(skyMean[2] - ridgeMean[2]);
        EXPECT_GT(l1Diff, 35.0);
    }

    // 沉稳系列首款与轻盈系列首款亮度均值应拉开明显明暗对比
    const double calmLuma = meanColors[static_cast<int>(BeautyBackgroundType::MidnightGraphite)][0] +
                            meanColors[static_cast<int>(BeautyBackgroundType::MidnightGraphite)][1] +
                            meanColors[static_cast<int>(BeautyBackgroundType::MidnightGraphite)][2];
    const double airyLuma = meanColors[static_cast<int>(BeautyBackgroundType::PureMinimal)][0] +
                            meanColors[static_cast<int>(BeautyBackgroundType::PureMinimal)][1] +
                            meanColors[static_cast<int>(BeautyBackgroundType::PureMinimal)][2];
    EXPECT_LT(calmLuma, 160.0);
    EXPECT_GT(airyLuma, 620.0);
}

TEST(StudioRecorderTest, MultiFormatExportWeChatAutoPlayGifAnimatedWebPAndSizeEstimator) {
    using namespace tools3000::capture;

    EXPECT_STREQ(StudioVideoCompositor::extensionForExportFormat(StudioExportFormat::MP4_H264), ".mp4");
    EXPECT_STREQ(StudioVideoCompositor::extensionForExportFormat(StudioExportFormat::MP4_HEVC), ".mp4");
    EXPECT_STREQ(StudioVideoCompositor::extensionForExportFormat(StudioExportFormat::GIF_WeChat), ".gif");
    EXPECT_STREQ(StudioVideoCompositor::extensionForExportFormat(StudioExportFormat::WebP_Animated), ".webp");
    EXPECT_STREQ(StudioVideoCompositor::extensionForExportFormat(StudioExportFormat::WebM_VP9), ".webm");

    // 微信自动播放 GIF 即使在 4K 原画输入 + 1080P 选项下，也必须自动钳制长边 <= 800px 偶数尺寸
    StudioExportOptions wxGifOpts;
    wxGifOpts.format = StudioExportFormat::GIF_WeChat;
    wxGifOpts.resolution = StudioExportResolution::Res1080P;
    wxGifOpts.fps = 15;
    wxGifOpts.quality = StudioExportQuality::Recommended;
    const cv::Size wxDim = StudioVideoCompositor::resolveExportDimensions(3456, 2088, wxGifOpts);
    EXPECT_LE(wxDim.width, 800);
    EXPECT_LE(wxDim.height, 800);
    EXPECT_EQ(wxDim.width % 2, 0);
    EXPECT_EQ(wxDim.height % 2, 0);

    const uint64_t estWxBytes = StudioVideoCompositor::estimateExportSizeBytes(3456, 2088, 12.0, wxGifOpts);
    EXPECT_GT(estWxBytes, 50'000u);
    EXPECT_LE(estWxBytes, 4'850'000u);

    const auto tmpDir = std::filesystem::temp_directory_path() / L"tools3000_studio_multifmt_test";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    const std::string rawUtf8 = tools3000::core::WinUtils::wstringToUtf8((tmpDir / L"src_clip.mp4").wstring());
    const std::wstring gifPathW = (tmpDir / L"wechat_autoplay.gif").wstring();
    const std::wstring webpPathW = (tmpDir / L"web_anim.webp").wstring();
    const std::string gifUtf8 = tools3000::core::WinUtils::wstringToUtf8(gifPathW);
    const std::string webpUtf8 = tools3000::core::WinUtils::wstringToUtf8(webpPathW);

    ASSERT_TRUE(writeSyntheticCleanMp4ForStudioTest(rawUtf8, 960, 540, 24, 24));

    StudioTimelineData tl;
    tl.sourceWidth = 960;
    tl.sourceHeight = 540;
    tl.fps = 24;
    tl.durationMs = 1000;
    tl.clickEvents.push_back({250, 130.0f, 90.0f, false});
    tl.zoomSegments = StudioTimelineAnalyzer::buildAutoZoomSegments(tl, 1.75f);

    StudioRenderConfig cfg;
    cfg.autoZoomEnabled = true;
    cfg.defaultZoomFactor = 1.75f;
    cfg.smoothCursorEnabled = true;
    cfg.autoFrameEnabled = true;
    cfg.backgroundTheme = BeautyBackgroundType::DuskMountains;
    cfg.exportSrtSidecar = false;

    // 1. 导出微信自动播放 GIF：验证 GIF89a 文件头、逻辑屏幕尺寸 <= 800px、体积严格 <= 4.85 MB
    ASSERT_TRUE(StudioVideoCompositor::exportStudioMedia(rawUtf8, gifUtf8, tl, cfg, wxGifOpts));
    ASSERT_TRUE(std::filesystem::exists(gifPathW, ec));
    const auto gifBytes = std::filesystem::file_size(gifPathW, ec);
    EXPECT_GT(gifBytes, 512u);
    EXPECT_LE(gifBytes, 4'850'000u);
    {
        std::ifstream ifs(gifPathW, std::ios::binary);
        uint8_t hdr[10]{};
        ifs.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        ASSERT_EQ(ifs.gcount(), 10);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(hdr), 6), "GIF89a");
        const uint16_t logicalW = static_cast<uint16_t>(hdr[6] | (hdr[7] << 8));
        const uint16_t logicalH = static_cast<uint16_t>(hdr[8] | (hdr[9] << 8));
        EXPECT_LE(logicalW, 800u);
        EXPECT_LE(logicalH, 800u);
    }

    // 2. 导出网页真彩动态 WebP：验证 RIFF....WEBPVP8X / ANIM 容器结构
    StudioExportOptions webpOpts;
    webpOpts.format = StudioExportFormat::WebP_Animated;
    webpOpts.resolution = StudioExportResolution::Res720P;
    webpOpts.fps = 15;
    webpOpts.quality = StudioExportQuality::Recommended;
    ASSERT_TRUE(StudioVideoCompositor::exportStudioMedia(rawUtf8, webpUtf8, tl, cfg, webpOpts));
    ASSERT_TRUE(std::filesystem::exists(webpPathW, ec));
    const auto webpBytes = std::filesystem::file_size(webpPathW, ec);
    EXPECT_GT(webpBytes, 256u);
    {
        std::ifstream ifs(webpPathW, std::ios::binary);
        std::vector<char> head(64, 0);
        ifs.read(head.data(), static_cast<std::streamsize>(head.size()));
        const std::string headStr(head.data(), static_cast<size_t>(ifs.gcount()));
        EXPECT_EQ(headStr.substr(0, 4), "RIFF");
        EXPECT_EQ(headStr.substr(8, 4), "WEBP");
        EXPECT_NE(headStr.find("ANIM"), std::string::npos);
    }

    std::filesystem::remove_all(tmpDir, ec);
}

TEST(StudioRecorderTest, VoiceEnhancerSuppressesLowFreqHumBoostsSpeechAndPreservesSystemAudio) {
    using namespace tools3000::capture;

    VoiceEnhancer enhancer(48000);
    enhancer.reset();

    // 1. 馈入 45Hz 低频空调/电源哼声 (幅值 0.012 模拟稳态环境底噪)，验证 85Hz 二阶高通 + 底噪门显著衰减底噪
    constexpr int kFrames = 4800; // 100ms @ 48kHz
    std::vector<float> humStereo(static_cast<std::size_t>(kFrames) * 2, 0.0f);
    for (int i = 0; i < kFrames; ++i) {
        const float s = 0.012f * std::sin(2.0f * 3.14159265f * 45.0f * static_cast<float>(i) / 48000.0f);
        humStereo[static_cast<std::size_t>(i) * 2] = s;
        humStereo[static_cast<std::size_t>(i) * 2 + 1] = s;
    }
    for (int pass = 0; pass < 4; ++pass) {
        auto block = humStereo;
        enhancer.processInterleavedStereo(block.data(), kFrames);
        if (pass == 3) {
            double outRms = 0.0;
            for (float v : block) outRms += static_cast<double>(v) * v;
            outRms = std::sqrt(outRms / static_cast<double>(block.size()));
            EXPECT_LT(outRms, 0.0045); // 底噪被显著压制
        }
    }

    // 2. 馈入模拟强人声信号 (220Hz 基频 + 2800Hz 齿音泛音)，验证仅处理麦克风且电脑系统背景声音增益恒为 1.0x 原样保留
    std::vector<float> voiceStereo(static_cast<std::size_t>(kFrames) * 2, 0.0f);
    for (int i = 0; i < kFrames; ++i) {
        const float t = static_cast<float>(i) / 48000.0f;
        const float v = 0.65f * std::sin(2.0f * 3.14159265f * 220.0f * t) +
                        0.45f * std::sin(2.0f * 3.14159265f * 2800.0f * t);
        voiceStereo[static_cast<std::size_t>(i) * 2] = v;
        voiceStereo[static_cast<std::size_t>(i) * 2 + 1] = v;
    }
    const float activity = enhancer.processInterleavedStereo(voiceStereo.data(), kFrames);
    const float sysGain = enhancer.currentSystemDuckingGain();

    float maxAbsOut = 0.0f;
    for (float v : voiceStereo) {
        maxAbsOut = (std::max)(maxAbsOut, std::abs(v));
    }
    EXPECT_GT(maxAbsOut, 0.35f);
    EXPECT_LE(maxAbsOut, 0.90f); // -1.0 dBFS 软限幅器确保 0 削波爆音
    EXPECT_GT(activity, 0.50f);
    EXPECT_FLOAT_EQ(sysGain, 1.0f); // 电脑系统背景声音 100% 原样保留，绝不自动压低或抽走
}

TEST(StudioRecorderTest, AntiJitterAutoZoomFiltersFlyByClicksAndTypingDefaultOff) {
    using namespace tools3000::capture;

    StudioTimelineData tl;
    tl.sourceWidth = 1920;
    tl.sourceHeight = 1080;
    tl.durationMs = 8000;

    // 1) 400ms 处发生一次高速路过点击（点击后 230ms 光标高速飞离超过 400px），应被识别为路过/碎操作过滤掉
    for (int i = 0; i < 12; ++i) {
        const int64_t t = i * 60;
        const float px = (t <= 400) ? (100.0f + i * 28.0f) : (300.0f + (i - 6) * 160.0f);
        const float py = (t <= 400) ? (120.0f + i * 18.0f) : (260.0f + (i - 6) * 95.0f);
        tl.cursorSamples.push_back({t, px, py, StudioCursorKind::Arrow});
    }
    tl.clickEvents.push_back({400, 296.0f, 246.0f, false});

    // 2) 1500ms 处打字焦点事件：默认 typingAutoZoom=false 下绝不自动触发放大
    tl.focusEvents.push_back({1500, 620.0f, 480.0f});

    // 3) 4200ms 处稳定驻留点击按钮：正常生成高质量运镜段
    for (int i = 0; i < 8; ++i) {
        tl.cursorSamples.push_back({3960 + i * 70, 1200.0f + i * 1.5f, 640.0f + i * 1.0f, StudioCursorKind::Hand});
    }
    tl.clickEvents.push_back({4200, 1205.0f, 642.0f, false});

    const auto segsDefault = StudioTimelineAnalyzer::buildAutoZoomSegments(tl, 1.50f, /*includeTypingFocus=*/false);
    ASSERT_EQ(segsDefault.size(), 1u);
    EXPECT_GE(segsDefault[0].startMs, 3900);

    // 显式开启打字自动缩放后，打字区间亦参与运镜
    const auto segsWithTyping = StudioTimelineAnalyzer::buildAutoZoomSegments(tl, 1.50f, /*includeTypingFocus=*/true);
    EXPECT_EQ(segsWithTyping.size(), 2u);
}

TEST(StudioRecorderTest, StudioCut2xSpeed3DMotionAnnotationsAndStaticVfrCompactMp4) {
    using namespace tools3000::capture;

    const auto tmpDir = std::filesystem::temp_directory_path() / L"tools3000_studio_cut_ann_vfr_test";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    const std::string rawUtf8 = tools3000::core::WinUtils::wstringToUtf8((tmpDir / L"raw_4s.mp4").wstring());
    const std::wstring outCutMp4W = (tmpDir / L"cut_compact.mp4").wstring();
    const std::string outCutUtf8 = tools3000::core::WinUtils::wstringToUtf8(outCutMp4W);

    // 生成 4.0 秒 (120 帧 @ 30fps) 底片
    ASSERT_TRUE(writeSyntheticCleanMp4ForStudioTest(rawUtf8, 960, 540, 30, 120));

    StudioTimelineData tl;
    tl.sourceWidth = 960;
    tl.sourceHeight = 540;
    tl.fps = 30;
    tl.durationMs = 4000;
    // 剪掉 1.0s ~ 2.5s (剪除 1.5s)，预期保留成片净时长约 2.5s！
    tl.cutSegments.push_back({1000, 2500, 0.0f, true});
    // 添加一段 3D 运镜与一条时间轴隐私模糊遮罩 + 高亮框
    tl.zoomSegments.push_back({200, 900, 320.0f, 200.0f, 1.65f, true});
    tl.annotations.push_back({StudioAnnotationKind::BlurMask, 0, 3800, 0.15f, 0.15f, 0.45f, 0.45f, "#EF4444", "", 3.0f, true});
    tl.annotations.push_back({StudioAnnotationKind::Rectangle, 0, 3800, 0.55f, 0.20f, 0.85f, 0.60f, "#3B82F6", "", 3.0f, true});

    StudioRenderConfig cfg;
    cfg.autoZoomEnabled = true;
    cfg.zoomMode = StudioZoomMode::Motion3D;
    cfg.motionBlurEnabled = true;
    cfg.autoFrameEnabled = true;
    cfg.aspectRatio = StudioAspectRatio::Ratio16x9;
    cfg.backgroundBlur = 12.0f;
    cfg.cornerRadius = 20.0f;
    cfg.shadowIntensity = 0.75f;
    cfg.cursorHighlightHalo = true;
    cfg.exportSrtSidecar = false;

    StudioExportOptions opts;
    opts.format = StudioExportFormat::MP4_H264;
    opts.resolution = StudioExportResolution::Res720P;
    opts.fps = 30;
    opts.quality = StudioExportQuality::Recommended;

    ASSERT_TRUE(StudioVideoCompositor::exportStudioMedia(rawUtf8, outCutUtf8, tl, cfg, opts));
    ASSERT_TRUE(std::filesystem::exists(outCutMp4W, ec));
    const uint64_t fileBytes = std::filesystem::file_size(outCutMp4W, ec);
    EXPECT_GT(fileBytes, 1024u);
    EXPECT_LT(fileBytes, 1'200'000u); // 2.5s 720p 视频在 VFR 静态抽帧与桌面码率控制下远小于 1.2MB

    std::vector<cv::Mat> verifyDeck;
    int outW = 0, outH = 0;
    double outDur = 0.0;
    ASSERT_TRUE(StudioVideoCompositor::extractPreviewDeck(outCutUtf8, 320, 120, verifyDeck, outW, outH, outDur));
    // 4.0s 原片剪掉 1.5s 后，导出成片时长严格落在 [2.15s, 2.85s] 区间
    EXPECT_GE(outDur, 2.15);
    EXPECT_LE(outDur, 2.85);

    std::filesystem::remove_all(tmpDir, ec);
}

TEST(StudioRecorderTest, RichVideoAnnotationsInsetBorderAndSquareSwatchesRenderCleanly) {
    using namespace tools3000::capture;

    cv::Mat srcBgr(540, 960, CV_8UC3, cv::Scalar(34, 42, 56));
    cv::rectangle(srcBgr, cv::Rect(120, 100, 720, 340), cv::Scalar(225, 232, 240), cv::FILLED);

    std::vector<StudioAnnotationItem> anns = {
        {StudioAnnotationKind::TextPlain,    0, 3000, 0.15f, 0.12f, 0.30f, 0.20f, "#F59E0B", "Plain", 3.0f, true},
        {StudioAnnotationKind::TextBubble,   0, 3000, 0.35f, 0.12f, 0.55f, 0.22f, "#A855F7", "Bubble", 3.0f, true},
        {StudioAnnotationKind::TextDashed,   0, 3000, 0.60f, 0.12f, 0.80f, 0.22f, "#94A3B8", "Dashed", 3.0f, true},
        {StudioAnnotationKind::TextNeon,     0, 3000, 0.15f, 0.28f, 0.35f, 0.38f, "#EC4899", "Neon", 3.0f, true},
        {StudioAnnotationKind::StepBadge,    0, 3000, 0.44f, 0.28f, 0.52f, 0.36f, "#3B82F6", "1", 3.0f, true},
        {StudioAnnotationKind::BoldArrow,    0, 3000, 0.50f, 0.38f, 0.50f, 0.62f, "#EC4899", "", 5.0f, true},
        {StudioAnnotationKind::SketchCircle, 0, 3000, 0.18f, 0.48f, 0.38f, 0.72f, "#F97316", "", 3.5f, true},
        {StudioAnnotationKind::HandPointer,  0, 3000, 0.64f, 0.50f, 0.72f, 0.62f, "#EC4899", "", 3.5f, true},
        {StudioAnnotationKind::Magnifier,    0, 3000, 0.74f, 0.42f, 0.92f, 0.70f, "#FFFFFF", "", 3.5f, true},
        {StudioAnnotationKind::GlowLine,     0, 3000, 0.15f, 0.80f, 0.45f, 0.80f, "#10B981", "", 3.0f, true},
        {StudioAnnotationKind::DashedLine,   0, 3000, 0.52f, 0.80f, 0.85f, 0.80f, "#38BDF8", "", 3.0f, true},
    };

    StudioRenderConfig cfg;
    cfg.autoFrameEnabled = true;
    cfg.insetBorderWidth = 4.0f;
    cfg.insetBorderColorHex = "#EC4899";
    cfg.cornerRadius = 18.0f;

    StudioEvaluatedFrame eval{};
    eval.ptsMs = 600;
    eval.camera.zoom = 1.0f;
    eval.camera.cropX = 0.0f;
    eval.camera.cropY = 0.0f;
    eval.camera.cropW = 960.0f;
    eval.camera.cropH = 540.0f;

    StudioVideoCompositor comp;
    const cv::Mat plainOut = comp.composeFrame(srcBgr, eval, cfg, cv::Size(960, 540), nullptr);
    const cv::Mat richOut  = comp.composeFrame(srcBgr, eval, cfg, cv::Size(960, 540), &anns);
    ASSERT_FALSE(plainOut.empty());
    ASSERT_FALSE(richOut.empty());
    EXPECT_GT(cv::norm(plainOut, richOut, cv::NORM_L1), 25000.0);

    const cv::Mat sqThumb = StudioVideoCompositor::renderThemeThumbnailBgra(BeautyBackgroundType::MistyLakeDawn, 64, 64);
    EXPECT_EQ(sqThumb.cols, 64);
    EXPECT_EQ(sqThumb.rows, 64);
    EXPECT_EQ(sqThumb.channels(), 4);
}

TEST(StudioRecorderTest, MagneticSplitSelectDeleteMiddleClipAndSpeedMapping) {
    using namespace tools3000::capture;

    constexpr int64_t kRawDurationMs = 12000;
    std::vector<StudioVideoClip> clips = StudioTimelineAnalyzer::buildClipsFromCutSegments(kRawDurationMs, {});
    ASSERT_EQ(clips.size(), 1u);
    EXPECT_EQ(clips[0].srcStartMs, 0);
    EXPECT_EQ(clips[0].srcEndMs, 12000);
    EXPECT_EQ(StudioTimelineAnalyzer::computeClipsKeptDurationMs(clips), 12000);

    // 第 1 步：在 3.0s 处点击“拆分” -> 切分为 [0..3000] 与 [3000..12000]
    const int rightIdx1 = StudioTimelineAnalyzer::splitClipAtSourceMs(clips, 3000);
    ASSERT_EQ(rightIdx1, 1);
    ASSERT_EQ(clips.size(), 2u);

    // 第 2 步：在 8.0s 处再次点击“拆分” -> 切分为 [0..3000], [3000..8000], [8000..12000]
    const int rightIdx2 = StudioTimelineAnalyzer::splitClipAtSourceMs(clips, 8000);
    ASSERT_EQ(rightIdx2, 2);
    ASSERT_EQ(clips.size(), 3u);
    EXPECT_EQ(clips[1].srcStartMs, 3000);
    EXPECT_EQ(clips[1].srcEndMs, 8000);

    // 第 3 步：选中中间段 [3000..8000] 并删除 -> 磁性波纹自动闭合为 7000ms 无缝时间轴
    clips.erase(clips.begin() + 1);
    ASSERT_EQ(clips.size(), 2u);
    EXPECT_EQ(StudioTimelineAnalyzer::computeClipsKeptDurationMs(clips), 7000);

    const auto derivedCuts = StudioTimelineAnalyzer::buildCutSegmentsFromClips(clips, kRawDurationMs);
    ASSERT_EQ(derivedCuts.size(), 1u);
    EXPECT_EQ(derivedCuts[0].startMs, 3000);
    EXPECT_EQ(derivedCuts[0].endMs, 8000);

    // 验证滑轨实时跟画映射：在拼接缝 3000ms 前后丝滑跳过被删区间 [3000..8000]
    int hitClipIdx = -1;
    EXPECT_EQ(StudioTimelineAnalyzer::mapKeptMsToSourceMs(clips, 2900, &hitClipIdx), 2900);
    EXPECT_EQ(hitClipIdx, 0);
    EXPECT_EQ(StudioTimelineAnalyzer::mapKeptMsToSourceMs(clips, 3050, &hitClipIdx), 8050);
    EXPECT_EQ(hitClipIdx, 1);
    EXPECT_EQ(StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(clips, 8500), 3500);

    // 验证对第二段切片 [8000..12000] 设置 2.0x 加速后的磁性时长与双向映射
    clips[1].speed = 2.0f;
    EXPECT_EQ(StudioTimelineAnalyzer::computeClipsKeptDurationMs(clips), 5000); // 3000 + 4000/2
    EXPECT_EQ(StudioTimelineAnalyzer::mapKeptMsToSourceMs(clips, 4000, &hitClipIdx), 10000);
    EXPECT_EQ(hitClipIdx, 1);
    EXPECT_EQ(StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(clips, 10000), 4000);
}

TEST(StudioRecorderTest, AudioWaveformRangeMaxPoolingAndSilenceGate) {
    using namespace tools3000::capture;
    // 模拟 10 秒录屏共 100 个 100ms 声波峰值桶：
    // [0..2s] 说话段 (峰值 0.45), [2s..5s] 静音停顿气口 (底噪 0.003), [5s..10s] 高潮段 (峰值 0.80)
    std::vector<float> peaks(100, 0.003f);
    for (int i = 0; i < 20; ++i) peaks[i] = 0.25f + (i % 5) * 0.05f; // max 0.45f
    for (int i = 50; i < 100; ++i) peaks[i] = (i == 72) ? 0.80f : 0.35f;

    constexpr int64_t kTotalMs = 10000;
    constexpr float kRefPeak = 0.80f;

    // 1. 静音停顿区间 [2200..4800ms] 底噪 < 0.008f 应被门限完全滤为 0.0f（清晰指示拆分剪辑点）
    const float silentLevel = StudioTimelineAnalyzer::sampleWaveformRangePeak(
        peaks, kTotalMs, 2200, 4800, kRefPeak);
    EXPECT_FLOAT_EQ(silentLevel, 0.0f);

    // 2. 第一段说话区间 [0..2000ms] 应捕获局部波峰并做感知映射 (> 0.5f 且 < 1.0f)
    const float speechLevel = StudioTimelineAnalyzer::sampleWaveformRangePeak(
        peaks, kTotalMs, 0, 2000, kRefPeak);
    EXPECT_GT(speechLevel, 0.55f);
    EXPECT_LT(speechLevel, 0.95f);

    // 3. 含瞬态重音的第 7.2s 区间 [7000..7500ms] Max-Pooling 绝不漏掉单桶 0.80f 峰值 -> 归一化满幅 1.0f
    const float burstLevel = StudioTimelineAnalyzer::sampleWaveformRangePeak(
        peaks, kTotalMs, 7000, 7500, kRefPeak);
    EXPECT_NEAR(burstLevel, 1.0f, 1e-4f);

    // 4. 空数组或非法时长安全回退 0.0f
    EXPECT_FLOAT_EQ(StudioTimelineAnalyzer::sampleWaveformRangePeak({}, kTotalMs, 0, 1000, 1.0f), 0.0f);
    std::vector<float> extractedEmpty;
    EXPECT_FALSE(StudioVideoCompositor::extractAudioWaveformPeaks("", 128, extractedEmpty));
    EXPECT_TRUE(extractedEmpty.empty());
}

TEST(StudioRecorderTest, ClickSnapZeroLagAndHandCursorSubpixelAlignment) {
    using namespace tools3000::capture;

    StudioTimelineData tl;
    tl.sourceWidth = 1920;
    tl.sourceHeight = 1080;
    tl.fps = 30;
    tl.durationMs = 1000;

    // 模拟鼠标从左上角 (80, 60) 高速冲刺至按钮 (960, 540) 急停在 t=300ms 点击，随后在 t=450ms 快速移向 (1500, 800)
    tl.cursorSamples.push_back({0,   80.0f,  60.0f,  StudioCursorKind::Arrow});
    tl.cursorSamples.push_back({150, 520.0f, 300.0f, StudioCursorKind::Arrow});
    tl.cursorSamples.push_back({300, 960.0f, 540.0f, StudioCursorKind::Hand});
    tl.cursorSamples.push_back({450, 1500.0f, 800.0f, StudioCursorKind::Arrow});
    tl.clickEvents.push_back({300, 960.0f, 540.0f, false});

    StudioRenderConfig cfg;
    cfg.smoothCursorEnabled = true;
    cfg.showClickRipple = true;
    cfg.autoFrameEnabled = false;
    cfg.autoZoomEnabled = true;
    tl.zoomSegments.push_back({100, 900, 960.0f, 540.0f, 1.80f, true, true});

    const auto baked = StudioTimelineAnalyzer::bakeEvaluatedFrames(tl, cfg, 30, 30);
    ASSERT_EQ(baked.size(), 30u);

    // 第 9 帧对应 t = 9 / 30 = 300ms 点击瞬间：Click-Snap 必须保证平滑光标 0.00px 零误差命中真实点击点 (960, 540)！
    const auto& clickFrame = baked[9];
    EXPECT_EQ(clickFrame.ptsMs, 300);
    EXPECT_NEAR(clickFrame.smoothCursorX, 960.0f, 0.01f);
    EXPECT_NEAR(clickFrame.smoothCursorY, 540.0f, 0.01f);
    EXPECT_EQ(clickFrame.cursorKind, StudioCursorKind::Hand);

    // 第 14 帧对应 t = 467ms：鼠标已离开至 ~1500，但点击水波纹源坐标必须死死锚定在原点击点 (960, 540) 不漂移！
    const auto& postClickFrame = baked[14];
    EXPECT_GT(postClickFrame.rippleProgress, 0.0f);
    EXPECT_NEAR(postClickFrame.rippleSrcX, 960.0f, 0.01f);
    EXPECT_NEAR(postClickFrame.rippleSrcY, 540.0f, 0.01f);

    // 验证亚像素裁剪与小手光标渲染在按钮目标处产生清晰像素变化
    cv::Mat srcBgr(1080, 1920, CV_8UC3, cv::Scalar(30, 34, 44));
    StudioVideoCompositor comp;
    StudioRenderConfig noCursorCfg = cfg;
    noCursorCfg.smoothCursorEnabled = false;
    noCursorCfg.showClickRipple = false;
    StudioEvaluatedFrame noRippleEval = clickFrame;
    noRippleEval.rippleProgress = -1.0f;

    const cv::Mat frameWithoutCursor = comp.composeFrame(srcBgr, noRippleEval, noCursorCfg, cv::Size(960, 540));
    const cv::Mat frameWithHandCursor = comp.composeFrame(srcBgr, clickFrame, cfg, cv::Size(960, 540));
    ASSERT_FALSE(frameWithoutCursor.empty());
    ASSERT_FALSE(frameWithHandCursor.empty());
    EXPECT_GT(cv::norm(frameWithoutCursor, frameWithHandCursor, cv::NORM_L1), 1500.0);
}

TEST(StudioRecorderTest, GifExportPreservesDarkBackgroundAcrossFramesAndHdZoomableSize) {
    using namespace tools3000::capture;

    // 1. 验证 GIF_HD 即使录制小选区 (640x400) 也自动保底长边 >= 1080px，突破微信 1000px 表情限制以支持双击打开放大
    StudioExportOptions hdOpts{};
    hdOpts.format = StudioExportFormat::GIF_HD;
    hdOpts.resolution = StudioExportResolution::Res1080P;
    const cv::Size hdFromSmall = StudioVideoCompositor::resolveExportDimensions(640, 400, hdOpts);
    EXPECT_GE((std::max)(hdFromSmall.width, hdFromSmall.height), 1080);

    // 2. 验证深色编辑器背景导出多帧 GIF 后：
    //    (a) GIF89a 二进制流中绝不出现 disposal=2 (Restore to Background 导致微信擦成纯白底花屏)；
    //    (b) 第 2+ 帧经解码回读后静止深色背景严格保持深暗色 (< 80)，绝不变白！
    const auto tmpDir = std::filesystem::temp_directory_path() / L"tools3000_gif_dark_bg_verify";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    const std::string rawMp4 = tools3000::core::WinUtils::wstringToUtf8((tmpDir / L"dark_ide.mp4").wstring());
    const std::wstring outGifW = (tmpDir / L"wechat_test.gif").wstring();
    const std::string outGif = tools3000::core::WinUtils::wstringToUtf8(outGifW);

    // 合成真实深色 IDE 底片 (BGR = 24, 26, 32)
    {
        AVFormatContext* mfmt = nullptr;
        ASSERT_GE(avformat_alloc_output_context2(&mfmt, nullptr, "mp4", rawMp4.c_str()), 0);
        const AVCodec* mcodec = avcodec_find_encoder_by_name("libx264");
        if (!mcodec) mcodec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!mcodec) mcodec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        ASSERT_NE(mcodec, nullptr);
        AVCodecContext* mctx = avcodec_alloc_context3(mcodec);
        mctx->width = 640;
        mctx->height = 360;
        mctx->time_base = {1, 15};
        mctx->framerate = {15, 1};
        mctx->gop_size = 15;
        mctx->max_b_frames = 0;
        mctx->bit_rate = 1'500'000;
        mctx->pix_fmt = AV_PIX_FMT_YUV420P;
        if (mfmt->oformat->flags & AVFMT_GLOBALHEADER) mctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ASSERT_GE(avcodec_open2(mctx, mcodec, nullptr), 0);
        AVStream* mst = avformat_new_stream(mfmt, mcodec);
        ASSERT_NE(mst, nullptr);
        ASSERT_GE(avcodec_parameters_from_context(mst->codecpar, mctx), 0);
        mst->time_base = mctx->time_base;
        ASSERT_GE(avio_open(&mfmt->pb, rawMp4.c_str(), AVIO_FLAG_WRITE), 0);
        ASSERT_GE(avformat_write_header(mfmt, nullptr), 0);

        SwsContext* msws = sws_getContext(640, 360, AV_PIX_FMT_BGR24, 640, 360, mctx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
        AVFrame* mfrm = av_frame_alloc();
        mfrm->format = mctx->pix_fmt;
        mfrm->width = 640;
        mfrm->height = 360;
        av_frame_get_buffer(mfrm, 0);
        AVPacket* mpkt = av_packet_alloc();
        for (int i = 0; i < 12; ++i) {
            cv::Mat darkImg(360, 640, CV_8UC3, cv::Scalar(24, 26, 32));
            cv::rectangle(darkImg, cv::Rect(40 + i * 12, 280, 48, 24), cv::Scalar(80, 170, 250), cv::FILLED);
            av_frame_make_writable(mfrm);
            const uint8_t* srcSlice[4] = {darkImg.data, nullptr, nullptr, nullptr};
            int srcStride[4] = {static_cast<int>(darkImg.step[0]), 0, 0, 0};
            sws_scale(msws, srcSlice, srcStride, 0, 360, mfrm->data, mfrm->linesize);
            mfrm->pts = i;
            avcodec_send_frame(mctx, mfrm);
            while (avcodec_receive_packet(mctx, mpkt) >= 0) {
                av_packet_rescale_ts(mpkt, mctx->time_base, mst->time_base);
                mpkt->stream_index = mst->index;
                av_interleaved_write_frame(mfmt, mpkt);
                av_packet_unref(mpkt);
            }
        }
        avcodec_send_frame(mctx, nullptr);
        while (avcodec_receive_packet(mctx, mpkt) >= 0) {
            av_packet_rescale_ts(mpkt, mctx->time_base, mst->time_base);
            mpkt->stream_index = mst->index;
            av_interleaved_write_frame(mfmt, mpkt);
            av_packet_unref(mpkt);
        }
        av_write_trailer(mfmt);
        sws_freeContext(msws);
        av_packet_free(&mpkt);
        av_frame_free(&mfrm);
        avcodec_free_context(&mctx);
        avio_closep(&mfmt->pb);
        avformat_free_context(mfmt);
    }

    StudioTimelineData tl;
    tl.sourceWidth = 640;
    tl.sourceHeight = 360;
    tl.fps = 15;
    tl.durationMs = 800;
    tl.cursorSamples.push_back({0,   120.0f, 120.0f, StudioCursorKind::Arrow});
    tl.cursorSamples.push_back({800, 420.0f, 240.0f, StudioCursorKind::Arrow});

    StudioRenderConfig cfg;
    cfg.autoFrameEnabled = true;
    cfg.backgroundTheme = BeautyBackgroundType::MidnightGraphite;

    StudioExportOptions wxOpts{};
    wxOpts.format = StudioExportFormat::GIF_WeChat;
    wxOpts.resolution = StudioExportResolution::Res640P;
    wxOpts.fps = 10;

    ASSERT_TRUE(StudioVideoCompositor::exportStudioMedia(rawMp4, outGif, tl, cfg, wxOpts));
    ASSERT_TRUE(std::filesystem::exists(outGifW, ec));

    // 二进制校验 GIF89a Graphic Control Extension: 严禁出现 disposal == 2 (Restore to Background 导致微信擦成白底)！
    {
        std::ifstream gifIn(outGifW, std::ios::binary);
        std::vector<uint8_t> gifBytes((std::istreambuf_iterator<char>(gifIn)), std::istreambuf_iterator<char>());
        ASSERT_GT(gifBytes.size(), 32u);
        const uint8_t gPacked = gifBytes[10];
        const size_t gctBytes = (gPacked & 0x80) ? (3u * (size_t(1) << ((gPacked & 7) + 1))) : 0u;
        size_t pos = 13 + gctBytes;
        int gceChecked = 0;
        while (pos + 8 < gifBytes.size()) {
            const uint8_t b = gifBytes[pos];
            if (b == 0x21) {
                if (gifBytes[pos + 1] == 0xF9) {
                    const int disposal = (gifBytes[pos + 3] >> 2) & 7;
                    EXPECT_NE(disposal, 2) << "GIF GCE must never use disposal=2 (Restore to Background) which erases dark frames to white in WeChat!";
                    ++gceChecked;
                    pos += 8;
                } else {
                    pos += 2;
                    while (pos < gifBytes.size() && gifBytes[pos] != 0) pos += 1 + gifBytes[pos];
                    ++pos;
                }
            } else if (b == 0x2C) {
                if (pos + 10 >= gifBytes.size()) break;
                const uint8_t ip = gifBytes[pos + 9];
                const size_t lct = (ip & 0x80) ? (3u * (size_t(1) << ((ip & 7) + 1))) : 0u;
                pos += 10 + lct + 1;
                while (pos < gifBytes.size() && gifBytes[pos] != 0) pos += 1 + gifBytes[pos];
                ++pos;
            } else {
                break;
            }
        }
        EXPECT_GE(gceChecked, 3);
    }

    // 用 FFmpeg GIF 解码器逐帧解码合成后的 RGBA 帧，断言后续帧静止背景保持深暗色 (< 120) 绝不变纯白 (255)
    AVFormatContext* fmt = nullptr;
    ASSERT_GE(avformat_open_input(&fmt, outGif.c_str(), nullptr, nullptr), 0);
    ASSERT_GE(avformat_find_stream_info(fmt, nullptr), 0);
    const AVCodec* dec = avcodec_find_decoder(fmt->streams[0]->codecpar->codec_id);
    ASSERT_NE(dec, nullptr);
    AVCodecContext* dctx = avcodec_alloc_context3(dec);
    ASSERT_GE(avcodec_parameters_to_context(dctx, fmt->streams[0]->codecpar), 0);
    ASSERT_GE(avcodec_open2(dctx, dec, nullptr), 0);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frm = av_frame_alloc();
    int decodedCount = 0;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == 0 && avcodec_send_packet(dctx, pkt) >= 0) {
            while (avcodec_receive_frame(dctx, frm) >= 0) {
                ++decodedCount;
                if (decodedCount >= 2 && frm->data[0]) {
                    // 取内框左上深色静止区域采样校验：绝不允许被透明索引清成 255 纯白
                    SwsContext* sws = sws_getContext(
                        frm->width, frm->height, static_cast<AVPixelFormat>(frm->format),
                        frm->width, frm->height, AV_PIX_FMT_BGR24, SWS_POINT, nullptr, nullptr, nullptr);
                    if (sws) {
                        cv::Mat bgr(frm->height, frm->width, CV_8UC3);
                        uint8_t* dst[4] = {bgr.data, nullptr, nullptr, nullptr};
                        int stride[4] = {static_cast<int>(bgr.step[0]), 0, 0, 0};
                        sws_scale(sws, frm->data, frm->linesize, 0, frm->height, dst, stride);
                        sws_freeContext(sws);
                        const cv::Rect sampleRoi(frm->width / 5, frm->height / 5, 32, 32);
                        const cv::Scalar meanBgr = cv::mean(bgr(sampleRoi));
                        const double lum = (meanBgr[0] + meanBgr[1] + meanBgr[2]) / 3.0;
                        EXPECT_LT(lum, 135.0) << "Frame " << decodedCount << " background unexpectedly turned white!";
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }
    EXPECT_GE(decodedCount, 3);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&dctx);
    avformat_close_input(&fmt);

    std::filesystem::remove_all(tmpDir, ec);
}

TEST(StudioRecorderTest, FourStudioClickEffectStylesSdfOpticalAndTactileSpring) {
    using namespace tools3000::capture;

    StudioTimelineData tl;
    tl.sourceWidth = 1280;
    tl.sourceHeight = 720;
    tl.fps = 60;
    tl.durationMs = 600;
    tl.cursorSamples.push_back({0,   640.0f, 360.0f, StudioCursorKind::Arrow});
    tl.cursorSamples.push_back({600, 640.0f, 360.0f, StudioCursorKind::Arrow});
    tl.clickEvents.push_back({100, 640.0f, 360.0f, false});

    StudioRenderConfig cfg;
    cfg.autoZoomEnabled = false;
    cfg.autoFrameEnabled = false;
    cfg.smoothCursorEnabled = true;
    cfg.showClickRipple = true;
    cfg.clickEffectStyle = StudioClickEffectStyle::CrystalPulse;

    const auto crystalBaked = StudioTimelineAnalyzer::bakeEvaluatedFrames(tl, cfg, 60, 36);
    ASSERT_EQ(crystalBaked.size(), 36u);

    // t = 150ms (点击后 50ms): 光标物理按压下沉至 ~0.80x 且尖端支点顺时针轻叩 < -2.5 deg
    const auto& pressFrame = crystalBaked[9];
    EXPECT_LT(pressFrame.clickScale, 0.84f);
    EXPECT_LT(pressFrame.clickTiltDeg, -2.5f);
    EXPECT_GT(pressFrame.rippleProgress, 0.05f);

    // 切换为 TactileOnly (仅物理按压): 零波纹，但完整保留物理按压缩放与轻叩倾角
    StudioRenderConfig tactileCfg = cfg;
    tactileCfg.clickEffectStyle = StudioClickEffectStyle::TactileOnly;
    const auto tactileBaked = StudioTimelineAnalyzer::bakeEvaluatedFrames(tl, tactileCfg, 60, 36);
    ASSERT_EQ(tactileBaked.size(), 36u);
    EXPECT_LT(tactileBaked[9].rippleProgress, 0.0f);
    EXPECT_NEAR(tactileBaked[9].clickScale, pressFrame.clickScale, 1e-4f);
    EXPECT_NEAR(tactileBaked[9].clickTiltDeg, pressFrame.clickTiltDeg, 1e-4f);

    // 验证四档风格在 composeFrame 下的 SDF 光学渲染互不相同且完全无越界异常
    cv::Mat darkPlate(720, 1280, CV_8UC3, cv::Scalar(22, 26, 36));
    StudioVideoCompositor comp;

    StudioRenderConfig softCfg = cfg;
    softCfg.clickEffectStyle = StudioClickEffectStyle::SoftTouch;
    const auto softBaked = StudioTimelineAnalyzer::bakeEvaluatedFrames(tl, softCfg, 60, 36);

    StudioRenderConfig dualCfg = cfg;
    dualCfg.clickEffectStyle = StudioClickEffectStyle::DualRipple;
    const auto dualBaked = StudioTimelineAnalyzer::bakeEvaluatedFrames(tl, dualCfg, 60, 36);

    // 取点击后 ~117ms (frame 13) 比较四档光学剖面差异
    const cv::Mat outCrystal = comp.composeFrame(darkPlate, crystalBaked[13], cfg, cv::Size(640, 360));
    const cv::Mat outSoft    = comp.composeFrame(darkPlate, softBaked[13], softCfg, cv::Size(640, 360));
    const cv::Mat outDual    = comp.composeFrame(darkPlate, dualBaked[13], dualCfg, cv::Size(640, 360));
    const cv::Mat outTactile = comp.composeFrame(darkPlate, tactileBaked[13], tactileCfg, cv::Size(640, 360));

    EXPECT_GT(cv::norm(outCrystal, outTactile, cv::NORM_L1), 400.0);
    EXPECT_GT(cv::norm(outSoft, outTactile, cv::NORM_L1), 400.0);
    EXPECT_GT(cv::norm(outDual, outCrystal, cv::NORM_L1), 150.0);
    EXPECT_GT(cv::norm(outCrystal, outSoft, cv::NORM_L1), 150.0);
}

TEST(StudioRecorderTest, GifExactCentroidZeroSolidNoiseAndTypingAutoZoomWiring) {
    using namespace tools3000::capture;

    // 1. 验证 GifPaletteOptimizer 在非 8 倍数深色 IDE 纯底色 BGR(41, 32, 29) + 外圈暖色渐变壁纸下：
    //    即使开启 enableDithering=true，内部深色纯底区域必须 100% 命中同一精确调色板索引（0 颗斜纹麻点），且重建颜色误差 == 0！
    const int W = 320, H = 200;
    cv::Mat frameBgr(H, W, CV_8UC3);
    for (int y = 0; y < H; ++y) {
        uint8_t* row = frameBgr.ptr<uint8_t>(y);
        for (int x = 0; x < W; ++x) {
            if (x >= 28 && x < W - 28 && y >= 22 && y < H - 22) {
                // 刻意选用旧版 5-bit 截断会产生 5 灰阶残差的深色 IDE 灰底 (R=29, G=32, B=41)
                row[x * 3 + 0] = 41;
                row[x * 3 + 1] = 32;
                row[x * 3 + 2] = 29;
            } else {
                // 外圈暖杏羊绒连续渐变壁纸
                row[x * 3 + 0] = static_cast<uint8_t>(180 + (x * 45) / W);
                row[x * 3 + 1] = static_cast<uint8_t>(195 + (y * 40) / H);
                row[x * 3 + 2] = static_cast<uint8_t>(215 + ((x + y) * 35) / (W + H));
            }
        }
    }

    GifPaletteOptimizer opt{};
    opt.reserveTransparent = false;
    opt.enableFrameDiff = false;
    opt.enableDithering = true;

    std::vector<uint8_t> indexed(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    uint32_t pal[256]{};
    opt.quantizeFrame(frameBgr.data, W, H, static_cast<int>(frameBgr.step[0]), 3,
                      indexed.data(), W, pal);

    const uint8_t centerIdx = indexed[static_cast<size_t>(H / 2) * W + (W / 2)];
    int mismatchPixels = 0;
    for (int y = 30; y < H - 30; ++y) {
        for (int x = 36; x < W - 36; ++x) {
            if (indexed[static_cast<size_t>(y) * W + x] != centerIdx) {
                ++mismatchPixels;
            }
        }
    }
    EXPECT_EQ(mismatchPixels, 0);

    const uint32_t argb = pal[centerIdx];
    const int palR = static_cast<int>((argb >> 16) & 0xFF);
    const int palG = static_cast<int>((argb >> 8) & 0xFF);
    const int palB = static_cast<int>(argb & 0xFF);
    EXPECT_NEAR(palR, 29, 1);
    EXPECT_NEAR(palG, 32, 1);
    EXPECT_NEAR(palB, 41, 1);

    // 2. 验证 typingAutoZoom 开启时，无鼠标点击纯打字 focusEvents 能真实驱动自动运镜片段生成与焦点牵引
    StudioTimelineData typingTl;
    typingTl.sourceWidth = 1920;
    typingTl.sourceHeight = 1080;
    typingTl.fps = 30;
    typingTl.durationMs = 3000;
    typingTl.focusEvents.push_back({600, 420.0f, 760.0f});
    typingTl.focusEvents.push_back({1100, 460.0f, 760.0f});

    const auto zoomOffSegs = StudioTimelineAnalyzer::buildAutoZoomSegments(typingTl, 1.85f, /*includeTypingFocus=*/false);
    EXPECT_TRUE(zoomOffSegs.empty());

    const auto zoomOnSegs = StudioTimelineAnalyzer::buildAutoZoomSegments(typingTl, 1.85f, /*includeTypingFocus=*/true);
    ASSERT_EQ(zoomOnSegs.size(), 1u);
    EXPECT_LE(zoomOnSegs[0].startMs, 600);
    EXPECT_GE(zoomOnSegs[0].endMs, 2200);
    EXPECT_NEAR(zoomOnSegs[0].focusY, 760.0f, 10.0f);
}

TEST(StudioRecorderTest, DynamicMotionCalloutsAnimateSmoothlyWhileBlurMaskProtectsFirstFrame) {
    using namespace tools3000::capture;

    cv::Mat srcBgr(360, 640, CV_8UC3, cv::Scalar(32, 38, 52));
    // 模拟敏感文本棋盘区供 BlurMask 首帧全遮挡测试
    for (int y = 90; y < 170; y += 8) {
        for (int x = 80; x < 240; x += 8) {
            if (((x + y) / 8) % 2 == 0) {
                cv::rectangle(srcBgr, cv::Rect(x, y, 8, 8), cv::Scalar(245, 248, 252), cv::FILLED);
            }
        }
    }

    StudioRenderConfig cfg;
    cfg.autoFrameEnabled = false;
    cfg.smoothCursorEnabled = false;

    StudioEvaluatedFrame eval{};
    eval.sourceWidth = 640.0f;
    eval.sourceHeight = 360.0f;
    eval.camera.zoom = 1.0f;
    eval.camera.cropX = 0.0f;
    eval.camera.cropY = 0.0f;
    eval.camera.cropW = 640.0f;
    eval.camera.cropH = 360.0f;

    StudioVideoCompositor comp;
    const cv::Mat cleanFrame = comp.composeFrame(srcBgr, eval, cfg, cv::Size(640, 360), nullptr);

    // 1. 动态重点标注：验证入场勾勒阶段 (70ms) -> 饱满驻留期 (420ms) -> 声呐/行军律动期 (780ms) 持续动态抓眼
    std::vector<StudioAnnotationItem> motionAnns = {
        {StudioAnnotationKind::RoundedBox,   0, 2400, 0.45f, 0.20f, 0.85f, 0.52f, "#F59E0B", "", 3.5f, true},
        {StudioAnnotationKind::SketchCircle, 0, 2400, 0.48f, 0.56f, 0.78f, 0.86f, "#F97316", "", 3.5f, true},
        {StudioAnnotationKind::DashedLine,   0, 2400, 0.12f, 0.76f, 0.42f, 0.76f, "#38BDF8", "", 3.5f, true},
        {StudioAnnotationKind::StepBadge,    0, 2400, 0.36f, 0.24f, 0.43f, 0.34f, "#3B82F6", "1", 3.5f, true},
    };

    eval.ptsMs = 70;
    const cv::Mat frameEnter = comp.composeFrame(srcBgr, eval, cfg, cv::Size(640, 360), &motionAnns, true);
    eval.ptsMs = 420;
    const cv::Mat frameHoldA = comp.composeFrame(srcBgr, eval, cfg, cv::Size(640, 360), &motionAnns, true);
    eval.ptsMs = 780;
    const cv::Mat frameHoldB = comp.composeFrame(srcBgr, eval, cfg, cv::Size(640, 360), &motionAnns, true);

    EXPECT_GT(cv::norm(cleanFrame, frameEnter, cv::NORM_L1), 2000.0);
    EXPECT_GT(cv::norm(frameEnter, frameHoldA, cv::NORM_L1), 4000.0); // 入场一笔勾勒过程中画面持续生长
    EXPECT_GT(cv::norm(frameHoldA, frameHoldB, cv::NORM_L1), 1500.0); // 驻留期声呐脉冲与虚线流动持续律动

    // 暂停静态拖拽模式 (animateAnnotations=false)：即使停在 startMs=0 第 0 毫秒，框体亦 100% 闭合清晰可见
    eval.ptsMs = 0;
    const cv::Mat framePausedAtZero = comp.composeFrame(srcBgr, eval, cfg, cv::Size(640, 360), &motionAnns, false);
    EXPECT_GT(cv::norm(cleanFrame, framePausedAtZero, cv::NORM_L1), 15000.0);

    // 2. 隐私马赛克遮罩 (BlurMask)：首帧 (0ms) 与中间帧 (500ms) 必须 100% 同等严密打码，零渐显泄露
    std::vector<StudioAnnotationItem> blurOnly = {
        {StudioAnnotationKind::BlurMask, 0, 2000, 0.10f, 0.22f, 0.40f, 0.50f, "#94A3B8", "", 3.5f, true}
    };
    eval.ptsMs = 0;
    const cv::Mat blurAt0 = comp.composeFrame(srcBgr, eval, cfg, cv::Size(640, 360), &blurOnly, true);
    eval.ptsMs = 500;
    const cv::Mat blurAt500 = comp.composeFrame(srcBgr, eval, cfg, cv::Size(640, 360), &blurOnly, true);
    EXPECT_GT(cv::norm(cleanFrame, blurAt0, cv::NORM_L1), 10000.0);
    EXPECT_DOUBLE_EQ(cv::norm(blurAt0, blurAt500, cv::NORM_L1), 0.0);

    // 3. 验证全部 7 款底部功能按钮微缩剧场画廊卡片（深/浅双主题）离屏高分屏渲染完好且动画帧间真实律动
    const float keyPhases[7] = {0.46f, 0.38f, 0.56f, 0.30f, 0.52f, 0.56f, 0.58f};
    std::vector<cv::Mat> lightCards, darkCards;
    for (int slot = 0; slot < 7; ++slot) {
        cv::Mat cardL = StudioPreviewWindow::renderCoachCardPreviewForTest(slot, keyPhases[slot], false, true);
        cv::Mat cardL2 = StudioPreviewWindow::renderCoachCardPreviewForTest(slot, 0.10f, false, true);
        cv::Mat cardD = StudioPreviewWindow::renderCoachCardPreviewForTest(slot, keyPhases[slot], true, true);
        ASSERT_FALSE(cardL.empty());
        ASSERT_FALSE(cardD.empty());
        EXPECT_GT(cv::norm(cardL, cardL2, cv::NORM_L1), 1500.0) << "Coach slot " << slot << " should animate";
        lightCards.push_back(cardL);
        darkCards.push_back(cardD);
    }

    if (const char* dumpDir = std::getenv("TOOLS3000_DUMP_STUDIO_GALLERY_DIR"); dumpDir && *dumpDir) {
        const std::filesystem::path outDir(dumpDir);
        std::filesystem::create_directories(outDir);

        auto tileGallery = [](const std::vector<cv::Mat>& cards, cv::Scalar bg) -> cv::Mat {
            const int cw = cards[0].cols;
            const int ch = cards[0].rows;
            cv::Mat grid(ch * 3, cw * 3, CV_8UC3, bg);
            for (size_t i = 0; i < cards.size(); ++i) {
                const int r = static_cast<int>(i) / 3;
                const int c = static_cast<int>(i) % 3;
                cards[i].copyTo(grid(cv::Rect(c * cw, r * ch, cw, ch)));
            }
            return grid;
        };
        cv::imwrite((outDir / "coach_gallery_light.png").string(), tileGallery(lightCards, cv::Scalar(250, 246, 242)));
        cv::imwrite((outDir / "coach_gallery_dark.png").string(), tileGallery(darkCards, cv::Scalar(26, 18, 14)));

        // 生成一张模拟明亮与深色双区 UI 的 1280x720 画面，检验新版「手绘重点圈」与各重点标注的真实颜值
        cv::Mat uiDemo(720, 1280, CV_8UC3, cv::Scalar(248, 250, 253));
        cv::rectangle(uiDemo, cv::Rect(40, 40, 560, 640), cv::Scalar(255, 255, 255), cv::FILLED);
        cv::rectangle(uiDemo, cv::Rect(640, 40, 600, 640), cv::Scalar(38, 29, 24), cv::FILLED);
        cv::rectangle(uiDemo, cv::Rect(120, 180, 260, 48), cv::Scalar(245, 130, 59), cv::FILLED);
        cv::rectangle(uiDemo, cv::Rect(760, 180, 280, 48), cv::Scalar(160, 180, 60), cv::FILLED);

        std::vector<StudioAnnotationItem> showcaseAnns = {
            // 浅色背景上的宽扁手绘重点圈 + 正圆手绘重点圈
            {StudioAnnotationKind::SketchCircle, 0, 3000, 0.08f, 0.22f, 0.32f, 0.35f, "#F97316", "", 3.8f, true},
            {StudioAnnotationKind::SketchCircle, 0, 3000, 0.12f, 0.48f, 0.28f, 0.72f, "#EF4444", "", 3.8f, true},
            // 深色背景上的手绘重点圈 + 圆角框 + 圆环 + 步骤章
            {StudioAnnotationKind::SketchCircle, 0, 3000, 0.57f, 0.22f, 0.84f, 0.35f, "#F59E0B", "", 3.8f, true},
            {StudioAnnotationKind::RoundedBox,   0, 3000, 0.57f, 0.46f, 0.84f, 0.62f, "#10B981", "", 3.5f, true},
            {StudioAnnotationKind::CircleRing,   0, 3000, 0.58f, 0.68f, 0.72f, 0.88f, "#38BDF8", "", 3.5f, true},
            {StudioAnnotationKind::StepBadge,    0, 3000, 0.51f, 0.24f, 0.55f, 0.32f, "#3B82F6", "1", 3.5f, true},
        };
        StudioEvaluatedFrame demoEval{};
        demoEval.ptsMs = 620;
        demoEval.sourceWidth = 1280.0f;
        demoEval.sourceHeight = 720.0f;
        demoEval.camera.zoom = 1.0f;
        demoEval.camera.cropW = 1280.0f;
        demoEval.camera.cropH = 720.0f;
        cv::Mat renderedDemo = comp.composeFrame(uiDemo, demoEval, cfg, cv::Size(1280, 720), &showcaseAnns, true);
        cv::imwrite((outDir / "sketch_circle_showcase.png").string(), renderedDemo);
    }
}

TEST(StudioRecorderTest, DynamicStepBadgeResequencingAndTimelineAnnotationLabels) {
    using namespace tools3000::capture;

    std::vector<StudioVideoClip> clips = {{0, 10000, 1.0f}};
    std::vector<StudioAnnotationItem> anns = {
        {StudioAnnotationKind::SketchCircle, 300,  2200, 0.2f, 0.2f, 0.5f, 0.5f, "#F97316", "", 3.5f, true, false},
        {StudioAnnotationKind::StepBadge,    800,  2600, 0.4f, 0.4f, 0.5f, 0.5f, "#3B82F6", "10", 3.5f, true, false},
        {StudioAnnotationKind::TextBubble,   1500, 3500, 0.3f, 0.3f, 0.6f, 0.4f, "#A855F7", "点击这里", 3.5f, true, false},
        {StudioAnnotationKind::StepBadge,    3200, 5000, 0.5f, 0.5f, 0.6f, 0.6f, "#3B82F6", "11", 3.5f, true, false},
        {StudioAnnotationKind::StepBadge,    6000, 8200, 0.6f, 0.4f, 0.7f, 0.5f, "#3B82F6", "12", 3.5f, true, false},
    };

    // 1. 自动重排消除历史遗留 "10, 11, 12"，严格按时间轴出场顺序重算为 "1, 2, 3"
    std::vector<int> ranks;
    const int nextSeq = StudioTimelineAnalyzer::resequenceAutoStepBadges(anns, clips, &ranks);
    EXPECT_EQ(nextSeq, 4);
    ASSERT_EQ(ranks.size(), anns.size());
    EXPECT_EQ(anns[1].text, "1");
    EXPECT_EQ(anns[3].text, "2");
    EXPECT_EQ(anns[4].text, "3");
    EXPECT_EQ(ranks[1], 1);
    EXPECT_EQ(ranks[3], 2);
    EXPECT_EQ(ranks[4], 3);

    // 2. 删除中间第 2 个步骤章 (index 3) 后，原第 3 个立即自动递补变为 "2"
    anns.erase(anns.begin() + 3);
    StudioTimelineAnalyzer::resequenceAutoStepBadges(anns, clips, &ranks);
    EXPECT_EQ(anns[1].text, "1");
    EXPECT_EQ(anns[3].text, "2");
    EXPECT_EQ(ranks[1], 1);
    EXPECT_EQ(ranks[3], 2);

    // 3. 在时间线上把后面的步骤章拖到最前面 (startMs = 200 < 800) -> 动态互换为 "1" 和 "2"
    anns[3].startMs = 200;
    anns[3].endMs = 1600;
    StudioTimelineAnalyzer::resequenceAutoStepBadges(anns, clips, &ranks);
    EXPECT_EQ(anns[3].text, "1");
    EXPECT_EQ(anns[1].text, "2");
    EXPECT_EQ(ranks[3], 1);
    EXPECT_EQ(ranks[1], 2);

    // 4. 用户手动将第二个步骤章文本改为自定义 "2B" (customStepText = true)：画面保留 "2B"，时间轴仍精确标注第 2 个
    anns[1].text = "2B";
    anns[1].customStepText = true;
    StudioTimelineAnalyzer::resequenceAutoStepBadges(anns, clips, &ranks);
    EXPECT_EQ(anns[1].text, "2B");
    EXPECT_EQ(StudioTimelineAnalyzer::formatTimelineAnnotationLabel(anns[1], ranks[1], 110.0f, false, true), L"步骤 2·2B");
    EXPECT_EQ(StudioTimelineAnalyzer::formatTimelineAnnotationLabel(anns[1], ranks[1], 0.0f, true, true), L"步骤序号 · 第 2 个 (显示 2B)");

    // 5. 验证各标注在轨道宽块、窄块与悬停气泡中的直观名称展示
    EXPECT_EQ(StudioTimelineAnalyzer::formatTimelineAnnotationLabel(anns[3], ranks[3], 90.0f, false, true), L"步骤 1");
    EXPECT_EQ(StudioTimelineAnalyzer::formatTimelineAnnotationLabel(anns[3], ranks[3], 38.0f, false, true), L"#1");
    EXPECT_EQ(StudioTimelineAnalyzer::formatTimelineAnnotationLabel(anns[3], ranks[3], 0.0f, true, true), L"步骤序号 · 第 1 个");

    EXPECT_EQ(StudioTimelineAnalyzer::formatTimelineAnnotationLabel(anns[0], 0, 120.0f, false, true), L"手绘重点圈");
    EXPECT_EQ(StudioTimelineAnalyzer::formatTimelineAnnotationLabel(anns[0], 0, 48.0f, false, true), L"重点圈");
    EXPECT_EQ(StudioTimelineAnalyzer::formatTimelineAnnotationLabel(anns[2], 0, 140.0f, false, true), L"气泡: 点击这里");
    EXPECT_EQ(StudioTimelineAnalyzer::formatTimelineAnnotationLabel(anns[2], 0, 0.0f, true, true), L"对话气泡: 点击这里");

    // 6. 剪辑联动：若前 1700ms 片段被剪除，落在被剪除区内的步骤章自动不占号，后续步骤顺延为第 1 个
    std::vector<StudioVideoClip> trimmedClips = {{1700, 10000, 1.0f}};
    anns[1].customStepText = false;
    StudioTimelineAnalyzer::resequenceAutoStepBadges(anns, trimmedClips, &ranks);
    EXPECT_EQ(ranks[3], 0); // [200..1600ms] 已被剪除，不占序号
    EXPECT_EQ(ranks[1], 1); // [800..2600ms] 在 1700ms 后仍有 900ms 可见区间，顺延成为第 1 个步骤！
    EXPECT_EQ(anns[1].text, "1");
}

TEST(StudioRecorderTest, GifGlobalPalettePreservesMidVideoWhiteErpScreenWithoutYellowTint) {
    using namespace tools3000::capture;

    const auto tmpDir = std::filesystem::temp_directory_path() / L"tools3000_gif_white_notint_test";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    const std::string rawMp4Utf8 = tools3000::core::WinUtils::wstringToUtf8((tmpDir / L"warm_to_white.mp4").wstring());
    const std::wstring outGifW   = (tmpDir / L"warm_to_white_hd.gif").wstring();
    const std::string outGifUtf8 = tools3000::core::WinUtils::wstringToUtf8(outGifW);

    // 合成一段「前半段暖橙金多彩桌面 -> 后半段突然切入灰白底单据报表」的视频
    {
        const int w = 640, h = 400, fps = 12, totalFrames = 24;
        AVFormatContext* fmt = nullptr;
        ASSERT_GE(avformat_alloc_output_context2(&fmt, nullptr, "mp4", rawMp4Utf8.c_str()), 0);
        ASSERT_NE(fmt, nullptr);
        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        ASSERT_NE(codec, nullptr);

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        ctx->width = w; ctx->height = h;
        ctx->time_base = {1, fps}; ctx->framerate = {fps, 1};
        ctx->gop_size = fps; ctx->max_b_frames = 0;
        ctx->bit_rate = 3'000'000; ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ASSERT_GE(avcodec_open2(ctx, codec, nullptr), 0);

        AVStream* st = avformat_new_stream(fmt, codec);
        ASSERT_NE(st, nullptr);
        ASSERT_GE(avcodec_parameters_from_context(st->codecpar, ctx), 0);
        ASSERT_GE(avio_open(&fmt->pb, rawMp4Utf8.c_str(), AVIO_FLAG_WRITE), 0);
        ASSERT_GE(avformat_write_header(fmt, nullptr), 0);
        st->time_base = ctx->time_base;

        SwsContext* sws = sws_getContext(w, h, AV_PIX_FMT_BGR24, w, h, ctx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
        AVFrame* frm = av_frame_alloc();
        frm->format = ctx->pix_fmt; frm->width = w; frm->height = h;
        ASSERT_GE(av_frame_get_buffer(frm, 32), 0);
        AVPacket* pkt = av_packet_alloc();

        for (int i = 0; i < totalFrames; ++i) {
            cv::Mat bgr(h, w, CV_8UC3);
            if (i < 12) {
                // 前半段：高饱和暖橙黄紫渐变画面（复现第 0 帧被暖色垄断调色板的极端条件）
                for (int y = 0; y < h; ++y) {
                    auto* row = bgr.ptr<cv::Vec3b>(y);
                    for (int x = 0; x < w; ++x) {
                        row[x] = cv::Vec3b(
                            static_cast<uint8_t>(40 + (x * 120) / w),
                            static_cast<uint8_t>(120 + (y * 110) / h),
                            static_cast<uint8_t>(245));
                    }
                }
            } else {
                // 后半段：纯正灰白表格单据界面 (BGR: 白底 252,252,252 + 浅冷灰表头 238,236,234 + 黄色底部按钮 130,220,248)
                bgr.setTo(cv::Scalar(252, 252, 252));
                cv::rectangle(bgr, cv::Rect(0, 0, w, 56), cv::Scalar(238, 236, 234), cv::FILLED);
                cv::rectangle(bgr, cv::Rect(24, h - 46, 130, 30), cv::Scalar(130, 220, 248), cv::FILLED);
            }

            av_frame_make_writable(frm);
            const uint8_t* srcSlice[1] = {bgr.data};
            const int srcStride[1] = {static_cast<int>(bgr.step[0])};
            sws_scale(sws, srcSlice, srcStride, 0, h, frm->data, frm->linesize);
            frm->pts = i;
            ASSERT_GE(avcodec_send_frame(ctx, frm), 0);
            while (avcodec_receive_packet(ctx, pkt) == 0) {
                av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
                pkt->stream_index = st->index;
                av_interleaved_write_frame(fmt, pkt);
                av_packet_unref(pkt);
            }
        }
        avcodec_send_frame(ctx, nullptr);
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
            av_packet_unref(pkt);
        }
        av_write_trailer(fmt);
        av_packet_free(&pkt);
        av_frame_free(&frm);
        sws_freeContext(sws);
        avio_closep(&fmt->pb);
        avcodec_free_context(&ctx);
        avformat_free_context(fmt);
    }

    StudioTimelineData tl{};
    tl.sourceWidth = 640;
    tl.sourceHeight = 400;
    tl.fps = 12;
    tl.durationMs = 2000;

    StudioRenderConfig cfg{};
    cfg.autoZoomEnabled = false;
    cfg.autoFrameEnabled = false;
    cfg.smoothCursorEnabled = false;
    cfg.exportSrtSidecar = false;

    StudioExportOptions gifOpts{};
    gifOpts.format = StudioExportFormat::GIF_HD;
    gifOpts.resolution = StudioExportResolution::Original;
    gifOpts.fps = 12;
    gifOpts.quality = StudioExportQuality::High;

    ASSERT_TRUE(StudioVideoCompositor::exportStudioMedia(rawMp4Utf8, outGifUtf8, tl, cfg, gifOpts));
    ASSERT_TRUE(std::filesystem::exists(outGifW, ec));

    // 解码生成的 GIF 文件，验证切入浅白底界面的首帧即保持中性纯净白（零偏黄染色）
    AVFormatContext* decFmt = nullptr;
    ASSERT_GE(avformat_open_input(&decFmt, outGifUtf8.c_str(), nullptr, nullptr), 0);
    ASSERT_GE(avformat_find_stream_info(decFmt, nullptr), 0);
    int vIdx = -1;
    for (unsigned s = 0; s < decFmt->nb_streams; ++s) {
        if (decFmt->streams[s]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vIdx = static_cast<int>(s);
            break;
        }
    }
    ASSERT_GE(vIdx, 0);
    const AVCodec* gifDec = avcodec_find_decoder(decFmt->streams[vIdx]->codecpar->codec_id);
    ASSERT_NE(gifDec, nullptr);
    AVCodecContext* decCtx = avcodec_alloc_context3(gifDec);
    ASSERT_GE(avcodec_parameters_to_context(decCtx, decFmt->streams[vIdx]->codecpar), 0);
    ASSERT_GE(avcodec_open2(decCtx, gifDec, nullptr), 0);

    SwsContext* toBgr = nullptr;
    AVFrame* dFrm = av_frame_alloc();
    AVPacket* dPkt = av_packet_alloc();
    int decodedIdx = 0;
    int verifiedWhiteFrames = 0;

    while (av_read_frame(decFmt, dPkt) >= 0) {
        if (dPkt->stream_index == vIdx && avcodec_send_packet(decCtx, dPkt) >= 0) {
            while (avcodec_receive_frame(decCtx, dFrm) == 0) {
                if (!toBgr) {
                    toBgr = sws_getContext(
                        dFrm->width, dFrm->height, static_cast<AVPixelFormat>(dFrm->format),
                        dFrm->width, dFrm->height, AV_PIX_FMT_BGR24, SWS_POINT, nullptr, nullptr, nullptr);
                }
                cv::Mat frameBgr(dFrm->height, dFrm->width, CV_8UC3);
                uint8_t* dstSlice[1] = {frameBgr.data};
                int dstStride[1] = {static_cast<int>(frameBgr.step[0])};
                sws_scale(toBgr, dFrm->data, dFrm->linesize, 0, dFrm->height, dstSlice, dstStride);

                if (decodedIdx >= 13) {
                    // 采样表格中央白底区域：B、G、R 必须均 >= 238，且红蓝差 |R - B| <= 8（绝无泛黄染色！）
                    const cv::Scalar whiteRoi = cv::mean(frameBgr(cv::Rect(220, 160, 160, 100)));
                    EXPECT_GE(whiteRoi[0], 238.0) << "Blue channel dropped (yellow tint!) at frame " << decodedIdx;
                    EXPECT_GE(whiteRoi[1], 238.0) << "Green channel dropped at frame " << decodedIdx;
                    EXPECT_GE(whiteRoi[2], 238.0) << "Red channel dropped at frame " << decodedIdx;
                    EXPECT_LE(std::abs(whiteRoi[2] - whiteRoi[0]), 8.0)
                        << "White table background tinted yellow at frame " << decodedIdx;
                    ++verifiedWhiteFrames;
                }
                ++decodedIdx;
            }
        }
        av_packet_unref(dPkt);
    }

    EXPECT_GE(verifiedWhiteFrames, 6);
    if (toBgr) sws_freeContext(toBgr);
    av_packet_free(&dPkt);
    av_frame_free(&dFrm);
    avcodec_free_context(&decCtx);
    avformat_close_input(&decFmt);
    std::filesystem::remove_all(tmpDir, ec);
}

TEST(StudioRecorderTest, EnterpriseTiledAndLogoWatermarkAdaptiveContrastAndBounds) {
    using namespace tools3000::capture;

    // 1. 验证动态溯源变量解析器
    const std::wstring resolved = WatermarkRenderer::resolveDynamicText("{user} · {host} · {date} {time}");
    EXPECT_FALSE(resolved.empty());
    EXPECT_EQ(resolved.find(L"{user}"), std::wstring::npos);
    EXPECT_EQ(resolved.find(L"{date}"), std::wstring::npos);

    // 2. 验证全屏斜铺企业溯源水印在「左半浅白底 + 右半深黑底」分屏图上的逐像素极性自适应显影
    cv::Mat splitScreen(540, 960, CV_8UC3);
    splitScreen(cv::Rect(0, 0, 480, 540)).setTo(cv::Scalar(250, 250, 250)); // 左半白底
    splitScreen(cv::Rect(480, 0, 480, 540)).setTo(cv::Scalar(20, 24, 34));   // 右半深黑底
    const cv::Mat origSplit = splitScreen.clone();

    WatermarkConfig tiledCfg{};
    tiledCfg.enabled = true;
    tiledCfg.mode = WatermarkMode::TiledTraceability;
    tiledCfg.textTemplate = "CONFIDENTIAL · {user} · {date}";
    tiledCfg.scale = 1.15f;
    tiledCfg.opacity = 0.22f;

    ASSERT_TRUE(WatermarkRenderer::applyInPlace(splitScreen, tiledCfg));

    int darkenedOnWhiteSide = 0;
    int lightenedOnDarkSide = 0;
    for (int y = 20; y < 520; ++y) {
        const auto* srcR = origSplit.ptr<cv::Vec3b>(y);
        const auto* dstR = splitScreen.ptr<cv::Vec3b>(y);
        for (int x = 20; x < 460; ++x) {
            if (dstR[x][0] + 6 < srcR[x][0] && dstR[x][1] + 6 < srcR[x][1]) {
                ++darkenedOnWhiteSide;
            }
        }
        for (int x = 500; x < 940; ++x) {
            if (dstR[x][0] > srcR[x][0] + 6 && dstR[x][1] > srcR[x][1] + 6) {
                ++lightenedOnDarkSide;
            }
        }
    }
    EXPECT_GT(darkenedOnWhiteSide, 400) << "Tiled watermark must darken pixels on bright ERP background";
    EXPECT_GT(lightenedOnDarkSide, 400) << "Tiled watermark must lighten pixels on dark IDE background";

    // 3. 验证透明 PNG 品牌 Logo 角标解码、自由坐标定位与舞台包围盒命中计算
    const auto tmpDir = std::filesystem::temp_directory_path() / L"tools3000_watermark_logo_test";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);

    const std::filesystem::path logoPath = tmpDir / L"brand_logo_透明.png";
    {
        cv::Mat logoBgra(96, 200, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        cv::circle(logoBgra, cv::Point(48, 48), 36, cv::Scalar(246, 130, 59, 255), cv::FILLED, cv::LINE_AA);
        cv::rectangle(logoBgra, cv::Rect(96, 28, 88, 40), cv::Scalar(255, 255, 255, 230), cv::FILLED);
        std::vector<uint8_t> pngBuf;
        ASSERT_TRUE(cv::imencode(".png", logoBgra, pngBuf));
        std::ofstream ofs(logoPath, std::ios::binary | std::ios::trunc);
        ofs.write(reinterpret_cast<const char*>(pngBuf.data()), static_cast<std::streamsize>(pngBuf.size()));
    }

    WatermarkConfig logoCfg{};
    logoCfg.enabled = true;
    logoCfg.mode = WatermarkMode::ImageLogo;
    logoCfg.imagePathUtf8 = tools3000::core::WinUtils::wstringToUtf8(logoPath.wstring());
    logoCfg.anchor = WatermarkAnchor::CustomFree;
    logoCfg.customNormX = 0.80f;
    logoCfg.customNormY = 0.82f;
    logoCfg.scale = 1.0f;
    logoCfg.opacity = 0.90f;

    cv::Rect expectedBounds{};
    ASSERT_TRUE(WatermarkRenderer::computeSingleBadgeBounds(800, 500, logoCfg, expectedBounds));
    EXPECT_GT(expectedBounds.width, 40);
    EXPECT_GT(expectedBounds.height, 20);
    EXPECT_NEAR(expectedBounds.x + expectedBounds.width * 0.5, 800 * 0.80, 3.0);
    EXPECT_NEAR(expectedBounds.y + expectedBounds.height * 0.5, 500 * 0.82, 3.0);

    cv::Mat stageImg(500, 800, CV_8UC3, cv::Scalar(30, 36, 48));
    cv::Rect appliedBounds{};
    ASSERT_TRUE(WatermarkRenderer::applyInPlace(stageImg, logoCfg, &appliedBounds));
    EXPECT_EQ(appliedBounds, expectedBounds);

    // 验证圆章蓝色区域已清晰混合进目标画布，而圆章外的透明边角保持原底色不脏污
    const cv::Vec3b cornerBg = stageImg.at<cv::Vec3b>(appliedBounds.y + 2, appliedBounds.x + 2);
    EXPECT_NEAR(cornerBg[0], 30, 2);
    const cv::Scalar badgeMean = cv::mean(stageImg(appliedBounds));
    EXPECT_GT(badgeMean[0] + badgeMean[1] + badgeMean[2], 140.0);

    std::filesystem::remove_all(tmpDir, ec);
}

TEST(StudioRecorderTest, WaveformNoiseFloorPreventsFlatCaterpillarAndHiResReaderDecodesCrispNativeFrames) {
    using namespace tools3000::capture;

    // 1. 验证声学本底噪声自适应归一化：稳态微弱风扇底噪 (0.055..0.072) 绝不炸成平顶栅栏，真实音节保持高对比度波峰波谷
    std::vector<float> steadyFanNoise(400, 0.0f);
    for (size_t i = 0; i < steadyFanNoise.size(); ++i) {
        steadyFanNoise[i] = 0.058f + 0.008f * std::sin(static_cast<float>(i) * 0.35f);
    }
    const float fanBar = StudioTimelineAnalyzer::sampleWaveformRangePeak(
        steadyFanNoise, 4000, 1200, 1250, 0.38f, 0.052f);
    EXPECT_LT(fanBar, 0.08f);

    std::vector<float> crispSpeech(500, 0.015f);
    for (size_t i = 100; i < 160; ++i) {
        crispSpeech[i] = 0.56f; // 清晰说话重读音节
    }
    const float vowelPeak = StudioTimelineAnalyzer::sampleWaveformRangePeak(
        crispSpeech, 5000, 1050, 1500, 0.56f, 0.014f);
    const float breathGap = StudioTimelineAnalyzer::sampleWaveformRangePeak(
        crispSpeech, 5000, 2400, 2800, 0.56f, 0.014f);
    EXPECT_GT(vowelPeak, 0.85f);
    EXPECT_LT(breathGap, 0.02f);

    // 2. 验证 StudioHiResFrameReader 驻留式原片原生全清解码器 (1600x900 1:1 清晰帧读取 + 随机 Seek + 0ms LRU 命中)
    const auto tmpDir = std::filesystem::temp_directory_path() / L"tools3000_hires_reader_test";
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::filesystem::create_directories(tmpDir, ec);
    const std::string hiResMp4Utf8 = tools3000::core::WinUtils::wstringToUtf8((tmpDir / L"native_1600x900.mp4").wstring());

    {
        const int w = 1600, h = 900, fps = 15, totalFrames = 18;
        AVFormatContext* fmt = nullptr;
        ASSERT_GE(avformat_alloc_output_context2(&fmt, nullptr, "mp4", hiResMp4Utf8.c_str()), 0);
        ASSERT_NE(fmt, nullptr);
        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        ASSERT_NE(codec, nullptr);

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        ctx->width = w; ctx->height = h;
        ctx->time_base = {1, fps}; ctx->framerate = {fps, 1};
        ctx->gop_size = 6; ctx->max_b_frames = 0;
        ctx->bit_rate = 8'000'000; ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        if (fmt->oformat->flags & AVFMT_GLOBALHEADER) ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ASSERT_GE(avcodec_open2(ctx, codec, nullptr), 0);

        AVStream* st = avformat_new_stream(fmt, codec);
        ASSERT_NE(st, nullptr);
        st->time_base = ctx->time_base;
        ASSERT_GE(avcodec_parameters_from_context(st->codecpar, ctx), 0);
        ASSERT_GE(avio_open(&fmt->pb, hiResMp4Utf8.c_str(), AVIO_FLAG_WRITE), 0);
        ASSERT_GE(avformat_write_header(fmt, nullptr), 0);

        SwsContext* sws = sws_getContext(w, h, AV_PIX_FMT_BGR24, w, h, ctx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
        AVFrame* frm = av_frame_alloc();
        frm->format = ctx->pix_fmt; frm->width = w; frm->height = h;
        ASSERT_GE(av_frame_get_buffer(frm, 32), 0);
        AVPacket* pkt = av_packet_alloc();

        for (int i = 0; i < totalFrames; ++i) {
            cv::Mat bgr(h, w, CV_8UC3, (i < 9) ? cv::Scalar(240, 240, 240) : cv::Scalar(24, 32, 180));
            cv::rectangle(bgr, cv::Rect(200 + i * 20, 180, 320, 240), cv::Scalar(20, 180, 40), cv::FILLED);
            ASSERT_GE(av_frame_make_writable(frm), 0);
            const uint8_t* srcSlice[1] = {bgr.data};
            int srcStride[1] = {static_cast<int>(bgr.step[0])};
            sws_scale(sws, srcSlice, srcStride, 0, h, frm->data, frm->linesize);
            frm->pts = i;
            ASSERT_GE(avcodec_send_frame(ctx, frm), 0);
            while (avcodec_receive_packet(ctx, pkt) >= 0) {
                av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
                pkt->stream_index = st->index;
                av_interleaved_write_frame(fmt, pkt);
                av_packet_unref(pkt);
            }
        }
        avcodec_send_frame(ctx, nullptr);
        while (avcodec_receive_packet(ctx, pkt) >= 0) {
            av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
            av_packet_unref(pkt);
        }
        av_write_trailer(fmt);
        sws_freeContext(sws);
        av_packet_free(&pkt);
        av_frame_free(&frm);
        avcodec_free_context(&ctx);
        avio_closep(&fmt->pb);
        avformat_free_context(fmt);
    }

    StudioHiResFrameReader reader;
    ASSERT_TRUE(reader.open(hiResMp4Utf8, 3200));
    EXPECT_EQ(reader.nativeWidth(), 1600);
    EXPECT_EQ(reader.nativeHeight(), 900);

    cv::Mat fStart, fSecondHalf, fSeekBack, fCached;
    ASSERT_TRUE(reader.decodeFrameAt(0.05, fStart));
    EXPECT_EQ(fStart.cols, 1600);
    EXPECT_EQ(fStart.rows, 900);
    EXPECT_GT(fStart.at<cv::Vec3b>(50, 50)[0], 200); // 前半段亮白底

    ASSERT_TRUE(reader.decodeFrameAt(0.85, fSecondHalf));
    EXPECT_EQ(fSecondHalf.cols, 1600);
    EXPECT_GT(fSecondHalf.at<cv::Vec3b>(50, 50)[2], 140); // 后半段深红底
    EXPECT_LT(fSecondHalf.at<cv::Vec3b>(50, 50)[0], 60);

    // 验证向后随机 Seek 与 0ms 缓存命中
    ASSERT_TRUE(reader.decodeFrameAt(0.12, fSeekBack));
    EXPECT_GT(fSeekBack.at<cv::Vec3b>(50, 50)[0], 200);
    EXPECT_TRUE(reader.tryGetCachedFrame(0.12, fCached, 0.020));
    EXPECT_EQ(fCached.cols, 1600);

    reader.close();
    EXPECT_FALSE(reader.isOpen());
    std::filesystem::remove_all(tmpDir, ec);
}

TEST(StudioRecorderTest, SculptedWaveformFilmstripWatermarkScaleAndFourHourTimelinePerf) {
    using namespace tools3000::capture;
    // 1. 验证重压缩音乐 + 前奏静音的雕刻声学山峦动态展开（防平顶梳子退化）
    std::vector<float> compressedTrack(4096, 0.0f);
    for (size_t i = 1050; i < compressedTrack.size(); ++i) {
        const double beat = 0.5 + 0.5 * std::sin(static_cast<double>(i) * 0.085);
        compressedTrack[i] = static_cast<float>(0.33 + 0.21 * std::pow(beat, 2.2));
    }
    const auto prof = StudioTimelineAnalyzer::analyzeWaveformProfile(compressedTrack);
    EXPECT_TRUE(prof.hasAudio);
    EXPECT_GT(prof.activePedestal, prof.noiseFloor + 0.05f);

    float silBody = 1.0f, silPeak = 1.0f;
    StudioTimelineAnalyzer::sampleSculptedWaveformColumn(
        compressedTrack, 22000, 1000, 1200, prof, silBody, silPeak);
    EXPECT_FLOAT_EQ(silBody, 0.0f);
    EXPECT_FLOAT_EQ(silPeak, 0.0f);

    float valleyBody = 0.0f, valleyPeak = 0.0f;
    float crestBody = 0.0f, crestPeak = 0.0f;
    float minActive = 1.0f, maxActive = 0.0f;
    for (int64_t t = 6500; t < 20000; t += 60) {
        StudioTimelineAnalyzer::sampleSculptedWaveformColumn(
            compressedTrack, 22000, t, t + 60, prof, valleyBody, valleyPeak);
        minActive = (std::min)(minActive, valleyBody);
        maxActive = (std::max)(maxActive, valleyBody);
    }
    (void)crestBody;
    (void)crestPeak;
    EXPECT_LT(minActive, 0.32f);
    EXPECT_GT(maxActive, 0.78f);
    EXPECT_GT(maxActive - minActive, 0.45f) << "Compressed music must preserve rich mountain-and-valley dynamics";

    // 2. 验证胶片画廊 + 雕刻山峦声波离屏渲染（含可选导出快照供人工像素审查）
    std::vector<cv::Mat> mockDeck;
    for (int f = 0; f < 16; ++f) {
        cv::Mat frame(360, 640, CV_8UC3, cv::Scalar(34 + f * 6, 42 + f * 4, 58 + f * 5));
        cv::rectangle(frame, cv::Rect(60 + f * 18, 50, 220, 180), cv::Scalar(235, 240, 246), cv::FILLED);
        mockDeck.push_back(std::move(frame));
    }
    const cv::Mat stripLight = StudioPreviewWindow::renderTimelineStripPreviewForTest(
        mockDeck, compressedTrack, 22.0, 10.1, false, true);
    const cv::Mat stripDark = StudioPreviewWindow::renderTimelineStripPreviewForTest(
        mockDeck, compressedTrack, 22.0, 10.1, true, true);
    const cv::Mat stripShort4s = StudioPreviewWindow::renderTimelineStripPreviewForTest(
        mockDeck, compressedTrack, 4.6, 1.2, false, true);
    ASSERT_FALSE(stripLight.empty());
    ASSERT_FALSE(stripDark.empty());
    ASSERT_FALSE(stripShort4s.empty());
    EXPECT_GT(stripLight.cols, 1200);
    EXPECT_GT(stripLight.rows, 190);

    // 验证中枢立体主题蓝核心播放按钮在顶栏居中舱内清晰显影
    int accentPlayPixels = 0;
    for (int y = 10; y <= 48; ++y) {
        const auto* row = stripShort4s.ptr<cv::Vec3b>(y);
        for (int x = 720; x <= 1180 && x < stripShort4s.cols; ++x) {
            const int b = row[x][0], g = row[x][1], r = row[x][2];
            if (b >= 200 && b > r + 90 && g >= 85) {
                ++accentPlayPixels;
            }
        }
    }
    EXPECT_GT(accentPlayPixels, 120) << "Centered integrated transport console must render saturated accent play button";

    // 验证三级长·中·短游标卡尺刻度：4.6s 标尺在 0.5s 半格中点处有中高度竖线，而在 0.1s..0.4s 处仅有底部短齿
    auto countDarkColumnsAtRow = [&](int physY) -> int {
        if (physY < 0 || physY >= stripShort4s.rows) return 0;
        const auto* row = stripShort4s.ptr<cv::Vec3b>(physY);
        int hits = 0;
        bool inTick = false;
        for (int x = 122; x < stripShort4s.cols - 24; ++x) {
            const int lum = (static_cast<int>(row[x][0]) + static_cast<int>(row[x][1]) + static_cast<int>(row[x][2])) / 3;
            const bool darkPx = (lum < 244);
            if (darkPx && !inTick) {
                ++hits;
                inTick = true;
            } else if (!darkPx) {
                inTick = false;
            }
        }
        return hits;
    };
    // kScale = 1.5 => rulerBottomY = 59.5 * 1.5 = 89.25px
    // 细齿顶 ~83.5px (y=86 穿过主+中+细全部刻度)；中线顶 ~77px (y=80 穿过主+半秒中线，不穿细齿)
    const int fineBandTicks = countDarkColumnsAtRow(86);
    const int midBandTicks  = countDarkColumnsAtRow(80);
    EXPECT_GE(fineBandTicks, 35) << "Fine 0.1s ticks must populate between seconds on 4.6s ruler";
    EXPECT_GE(midBandTicks, 8)   << "Half-second midpoint ticks (0.5s, 1.5s...) must rise taller than fine ticks";
    EXPECT_GT(fineBandTicks, midBandTicks + 15) << "Fine ticks must stay shorter than half-second midpoint ticks";

    wchar_t dumpEnv[MAX_PATH]{};
    wchar_t realMp4Env[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"TOOLS3000_DUMP_TIMELINE_PREVIEW_DIR", dumpEnv, MAX_PATH) > 0) {
        const std::filesystem::path outDir(dumpEnv);
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);
        std::vector<cv::Mat> realDeck;
        std::vector<float> realPeaks;
        int rw = 0, rh = 0;
        double rdur = 22.0;
        if (GetEnvironmentVariableW(L"TOOLS3000_DUMP_TIMELINE_REAL_MP4", realMp4Env, MAX_PATH) > 0 &&
            std::filesystem::exists(realMp4Env, ec)) {
            const std::string realMp4Utf8 = tools3000::core::WinUtils::wstringToUtf8(realMp4Env);
            if (StudioVideoCompositor::extractPreviewDeck(realMp4Utf8, 640, 32, realDeck, rw, rh, rdur) &&
                StudioVideoCompositor::extractAudioWaveformPeaks(realMp4Utf8, 4096, realPeaks)) {
                cv::imwrite((outDir / "real_215708_light.png").string(),
                            StudioPreviewWindow::renderTimelineStripPreviewForTest(realDeck, realPeaks, rdur, 10.1, false, true));
                cv::imwrite((outDir / "real_215708_dark.png").string(),
                            StudioPreviewWindow::renderTimelineStripPreviewForTest(realDeck, realPeaks, rdur, 10.1, true, true));
                cv::imwrite((outDir / "real_4s_vernier_light.png").string(),
                            StudioPreviewWindow::renderTimelineStripPreviewForTest(realDeck, realPeaks, 4.6, 1.2, false, true));
            }
        } else {
            cv::imwrite((outDir / "synth_light.png").string(), stripLight);
            cv::imwrite((outDir / "synth_dark.png").string(), stripDark);
            cv::imwrite((outDir / "synth_4s_vernier_light.png").string(), stripShort4s);
        }
    }

    // 3. 验证平铺水印在 35%~68% 低缩放段无死区响应，且中间调灰底 (gray=134) 清晰显影
    WatermarkConfig wmSmall{};
    wmSmall.enabled = true;
    wmSmall.mode = WatermarkMode::TiledTraceability;
    wmSmall.textTemplate = "CorpSec · 2026-11-18";
    wmSmall.scale = 0.38f;
    wmSmall.opacity = 0.16f;

    WatermarkConfig wmNormal = wmSmall;
    wmNormal.scale = 0.95f;

    cv::Mat midGraySmall(720, 1280, CV_8UC3, cv::Scalar(134, 134, 134));
    cv::Mat midGrayNormal(720, 1280, CV_8UC3, cv::Scalar(134, 134, 134));
    ASSERT_TRUE(WatermarkRenderer::applyInPlace(midGraySmall, wmSmall));
    ASSERT_TRUE(WatermarkRenderer::applyInPlace(midGrayNormal, wmNormal));

    int maxDiffOnMidGray = 0;
    int changedSmall = 0, changedNormal = 0;
    for (int y = 0; y < midGraySmall.rows; y += 2) {
        const auto* ps = midGraySmall.ptr<cv::Vec3b>(y);
        const auto* pn = midGrayNormal.ptr<cv::Vec3b>(y);
        for (int x = 0; x < midGraySmall.cols; x += 2) {
            const int ds = std::abs(static_cast<int>(ps[x][0]) - 134);
            const int dn = std::abs(static_cast<int>(pn[x][0]) - 134);
            maxDiffOnMidGray = (std::max)(maxDiffOnMidGray, dn);
            if (ds >= 6) ++changedSmall;
            if (dn >= 6) ++changedNormal;
        }
    }
    EXPECT_GE(maxDiffOnMidGray, 16) << "Tiled watermark must remain clearly visible on mid-gray backgrounds";
    EXPECT_NE(changedSmall, changedNormal) << "Scale slider between 38% and 95% must actively resize tiled watermark";

    // 4. 验证 4 小时 (14,400s) 超长录屏时间轴预烘焙 O(F + C) 亚百毫秒级性能
    StudioTimelineData fourHourTl{};
    fourHourTl.sourceWidth = 2560;
    fourHourTl.sourceHeight = 1440;
    fourHourTl.durationMs = 4LL * 3600LL * 1000LL; // 14,400,000 ms
    for (int i = 0; i < 4800; ++i) {
        const int64_t pts = static_cast<int64_t>(i) * 3000LL + 500LL;
        fourHourTl.cursorSamples.push_back({pts, 400.0f + (i % 900), 300.0f + (i % 500), StudioCursorKind::Arrow, false, false});
        fourHourTl.clickEvents.push_back({pts, 400.0f + (i % 900), 300.0f + (i % 500), false});
    }
    fourHourTl.zoomSegments = StudioTimelineAnalyzer::buildAutoZoomSegments(fourHourTl, 1.80f, true);
    for (int c = 0; c < 1800; ++c) {
        const int64_t cs = static_cast<int64_t>(c) * 8000LL;
        fourHourTl.captions.push_back({cs, cs + 3500LL, "Long session subtitle segment"});
    }

    StudioRenderConfig renderCfg{};
    renderCfg.autoZoomEnabled = true;
    renderCfg.smoothCursorEnabled = true;
    renderCfg.autoCaptionsEnabled = true;

    const auto t0 = std::chrono::steady_clock::now();
    const auto baked = StudioTimelineAnalyzer::bakeEvaluatedFrames(fourHourTl, renderCfg, 5, 72002);
    const double bakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(baked.size(), 72002u);
    EXPECT_LT(bakeMs, 450.0) << "4-hour recording timeline bake must complete well under 450ms";
}

TEST(StudioRecorderTest, FreezeCaptureImmediatelyBlocksPostStopOverlayAndEliminatesGreenTailFrames) {
    using namespace tools3000::capture;

    auto postStopGreenFlag = std::make_shared<std::atomic<bool>>(false);
    auto postStopCapturedCount = std::make_shared<std::atomic<int>>(0);

    class StopBoundaryMockBackend final : public ICaptureBackend {
    public:
        StopBoundaryMockBackend(std::shared_ptr<std::atomic<bool>> greenFlag,
                                std::shared_ptr<std::atomic<int>> leakCount)
            : m_greenFlag(std::move(greenFlag)), m_leakCount(std::move(leakCount)) {
            m_info = {"mock-stop-boundary", "Mock Stop Boundary Backend", true, true, {}};
        }

        const CaptureBackendInfo& info() const noexcept override { return m_info; }

        bool initialize(const CaptureRegion& region, std::string&) override {
            m_region = region;
            m_stride = region.width * 4;
            m_buf.resize(static_cast<size_t>(m_stride) * static_cast<size_t>(region.height), 0);
            return true;
        }

        bool capture(CaptureFrameView& frame, std::string&) override {
            const bool isGreen = m_greenFlag->load(std::memory_order_acquire);
            if (isGreen) {
                m_leakCount->fetch_add(1, std::memory_order_relaxed);
            }
            for (int y = 0; y < m_region.height; ++y) {
                uint8_t* row = m_buf.data() + static_cast<size_t>(y) * static_cast<size_t>(m_stride);
                for (int x = 0; x < m_region.width; ++x) {
                    if (isGreen) {
                        // 模拟停止后弹出的未初始化 YUV(0,0,0) 草绿屏 BGR=[5, 129, 72]
                        row[x * 4 + 0] = 5;
                        row[x * 4 + 1] = 129;
                        row[x * 4 + 2] = 72;
                        row[x * 4 + 3] = 255;
                    } else {
                        // 正常深灰聊天窗口 BGR=[64, 54, 49]
                        row[x * 4 + 0] = 64;
                        row[x * 4 + 1] = 54;
                        row[x * 4 + 2] = 49;
                        row[x * 4 + 3] = 255;
                    }
                }
            }
            frame = {m_buf.data(), m_stride, m_region.width, m_region.height, CapturePixelFormat::Bgra32};
            return true;
        }

        void releaseFrame() noexcept override {}
        void shutdown() noexcept override { m_buf.clear(); }

    private:
        CaptureBackendInfo m_info;
        CaptureRegion m_region;
        int m_stride = 0;
        std::vector<uint8_t> m_buf;
        std::shared_ptr<std::atomic<bool>> m_greenFlag;
        std::shared_ptr<std::atomic<int>> m_leakCount;
    };

    struct BackendResetGuard {
        ~BackendResetGuard() { setCaptureBackendFactoryForTesting(nullptr); }
    } resetGuard;

    setCaptureBackendFactoryForTesting([postStopGreenFlag, postStopCapturedCount]() {
        return std::make_unique<StopBoundaryMockBackend>(postStopGreenFlag, postStopCapturedCount);
    });

    const auto tmpMp4 = std::filesystem::temp_directory_path() /
                        (L"tools3000_stop_no_green_" + std::to_wstring(GetCurrentProcessId()) + L".mp4");
    std::error_code ec;
    std::filesystem::remove(tmpMp4, ec);

    auto& rec = ScreenRecorder::instance();
    rec.initialize();

    RecordOptions opts;
    opts.format = RecordFormat::MP4_H264;
    opts.fps = 30;
    opts.fullScreen = false;
    opts.regionX = 0;
    opts.regionY = 0;
    opts.width = 640;
    opts.height = 360;
    opts.countdownSeconds = 0;
    opts.captureSystemAudio = false;
    opts.captureMicrophone = false;
    opts.includeCursor = false;
    opts.studioMode = true;
    opts.outputPath = tools3000::core::WinUtils::wstringToUtf8(tmpMp4.wstring());

    ASSERT_TRUE(rec.startRecording(opts));
    std::this_thread::sleep_for(std::chrono::milliseconds(320));

    // 点击结束瞬间：先调用 freezeCaptureImmediately()，随即屏幕上立刻出现草绿色过渡界面并阻塞 200ms 再调异步停止
    rec.freezeCaptureImmediately();
    postStopGreenFlag->store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(180));

    std::promise<std::string> donePromise;
    auto doneFuture = donePromise.get_future();
    ASSERT_TRUE(rec.stopRecordingAsync([&donePromise](const std::string& saved, const RecordStats&) {
        donePromise.set_value(saved);
    }));
    const std::string finalPath = doneFuture.get();
    rec.waitForAsyncStop();

    ASSERT_FALSE(finalPath.empty());
    EXPECT_EQ(postStopCapturedCount->load(), 0) << "Zero frames may be captured after freezeCaptureImmediately()";

    const auto deck = rec.takeLastStudioPreviewDeck();
    ASSERT_FALSE(deck.empty());
    for (const auto& thumb : deck) {
        const cv::Scalar m = cv::mean(thumb);
        EXPECT_LT(m[1], 90.0) << "Preview deck must not contain any green BGR=[5,129,72] frame";
    }

    std::vector<cv::Mat> decodedDeck;
    int ow = 0, oh = 0;
    double dur = 0.0;
    ASSERT_TRUE(StudioVideoCompositor::extractPreviewDeck(finalPath, 640, 64, decodedDeck, ow, oh, dur));
    ASSERT_FALSE(decodedDeck.empty());
    for (size_t i = 0; i < decodedDeck.size(); ++i) {
        const cv::Scalar m = cv::mean(decodedDeck[i]);
        EXPECT_LT(m[1], 90.0) << "Encoded MP4 frame " << i << " must be free of post-stop green screen";
    }

    std::filesystem::remove(tmpMp4, ec);
}

TEST(StudioRecorderTest, RawDirectOutputPolicyAndUnsavedEditDetection) {
    using namespace tools3000::capture;

    // 1. 纯净原样配置（无背景修饰、无运镜、无平滑光标叠加、原始比例、无剪辑/标注/水印/字幕）
    StudioRenderConfig rawCfg{};
    rawCfg.autoFrameEnabled = false;
    rawCfg.autoZoomEnabled = false;
    rawCfg.zoomMode = StudioZoomMode::None;
    rawCfg.aspectRatio = StudioAspectRatio::Original;
    rawCfg.smoothCursorEnabled = false;
    rawCfg.showClickRipple = false;
    rawCfg.cursorHighlightHalo = false;
    rawCfg.autoCaptionsEnabled = false;
    rawCfg.watermark.enabled = false;

    StudioTimelineData tl{};
    tl.durationMs = 5000;
    std::vector<StudioVideoClip> fullClip = {{0, 5000, 1.0f}};

    EXPECT_TRUE(StudioPreviewWindow::isRawOutputModeConfig(rawCfg));
    EXPECT_FALSE(StudioPreviewWindow::hasStudioEditsToRender(rawCfg, tl, fullClip, 5000));

    // 2. 开启背景边框即进入演播室编辑态，需重编码导出
    StudioRenderConfig framedCfg = rawCfg;
    framedCfg.autoFrameEnabled = true;
    EXPECT_FALSE(StudioPreviewWindow::isRawOutputModeConfig(framedCfg));
    EXPECT_TRUE(StudioPreviewWindow::hasStudioEditsToRender(framedCfg, tl, fullClip, 5000));

    // 3. 开启自动运镜且存在启用运镜段时需重编码导出
    StudioRenderConfig zoomCfg = rawCfg;
    zoomCfg.autoZoomEnabled = true;
    zoomCfg.zoomMode = StudioZoomMode::Zoom2D;
    tl.zoomSegments.push_back({1000, 3000, 0.5f, 0.5f, 1.85f, true, true});
    EXPECT_FALSE(StudioPreviewWindow::isRawOutputModeConfig(zoomCfg));
    EXPECT_TRUE(StudioPreviewWindow::hasStudioEditsToRender(zoomCfg, tl, fullClip, 5000));
    tl.zoomSegments.clear();

    // 4. 即使处于原样配置，若对时间轴做了裁剪、倍速或添加了标注，仍判定为有编辑需导出
    std::vector<StudioVideoClip> trimmedClip = {{500, 4500, 1.0f}};
    EXPECT_TRUE(StudioPreviewWindow::hasStudioEditsToRender(rawCfg, tl, trimmedClip, 5000));

    std::vector<StudioVideoClip> spedUpClip = {{0, 5000, 1.5f}};
    EXPECT_TRUE(StudioPreviewWindow::hasStudioEditsToRender(rawCfg, tl, spedUpClip, 5000));

    StudioTimelineData annTl = tl;
    StudioAnnotationItem ann{};
    ann.startMs = 500;
    ann.endMs = 2000;
    ann.text = "Note";
    annTl.annotations.push_back(ann);
    EXPECT_TRUE(StudioPreviewWindow::hasStudioEditsToRender(rawCfg, annTl, fullClip, 5000));

    // 5. 默认关闭态下未导出防丢确认框保持关闭
    EXPECT_FALSE(StudioPreviewWindow::instance().isCloseConfirmModalOpen());
}

TEST(StudioRecorderTest, MagneticTimelineSnappingToClipSplitPlayheadAndBoundaries) {
    using namespace tools3000::capture;

    // 构造两段拆分视频：片段 1 [0..8400ms], 片段 2 [8400..18500ms]（在 8400ms 处拆分）
    std::vector<StudioVideoClip> clips = {
        {0, 8400, 1.0f},
        {8400, 18500, 1.0f}
    };
    std::vector<StudioZoomSegment> zooms = {
        {2000, 4200, 0.5f, 0.5f, 1.85f, true, true},
        {9000, 11000, 0.5f, 0.5f, 1.85f, true, true}
    };
    StudioAnnotationItem ann{};
    ann.startMs = 5000;
    ann.endMs = 7500;
    std::vector<StudioAnnotationItem> anns = {ann};

    // 收集除 zooms[1] 外的全部吸附锚点（含拆分点 8400ms、播放头 12400ms、zooms[0] 2000/4200、ann 5000/7500）
    const auto anchors = StudioTimelineAnalyzer::collectTimelineSnapAnchors(
        clips, zooms, anns, 12400, /*excludeZoomIdx=*/1, /*excludeAnnIdx=*/-1, true);

    // 1. 整块拖动 zooms[1] 使其左缘靠近拆分点 8400ms (8490ms, 距拆分点 90ms <= 窗口 150ms)：
    //    必须精准吸附到 8400ms 并保持块长 2000ms 不变！
    const auto snapLeftToSplit = StudioTimelineAnalyzer::snapTimelineRangeDrag(
        8490, 10490, /*MoveBoth=*/0, anchors, /*snapWin=*/150, /*minSpan=*/220, /*maxTotal=*/18500);
    EXPECT_TRUE(snapLeftToSplit.snapped);
    EXPECT_EQ(snapLeftToSplit.targetKind, TimelineSnapTargetKind::ClipSplit);
    EXPECT_EQ(snapLeftToSplit.snapLineKeptMs, 8400);
    EXPECT_EQ(snapLeftToSplit.snappedStartMs, 8400);
    EXPECT_EQ(snapLeftToSplit.snappedEndMs, 10400);

    // 2. 整块拖动使其右缘靠近拆分点 8400ms (6310..8310ms, 右缘距拆分点 90ms)：
    //    必须右缘精准吸附至 8400ms，块长依然保持 2000ms (6400..8400ms)！
    const auto snapRightToSplit = StudioTimelineAnalyzer::snapTimelineRangeDrag(
        6310, 8310, /*MoveBoth=*/0, anchors, 150, 220, 18500);
    EXPECT_TRUE(snapRightToSplit.snapped);
    EXPECT_EQ(snapRightToSplit.targetKind, TimelineSnapTargetKind::ClipSplit);
    EXPECT_EQ(snapRightToSplit.snappedStartMs, 6400);
    EXPECT_EQ(snapRightToSplit.snappedEndMs, 8400);

    // 3. 拖动左缘拉伸靠近标注右缘 7500ms：精准贴合 7500ms 且右缘保持不变
    const auto snapResizeLeft = StudioTimelineAnalyzer::snapTimelineRangeDrag(
        7580, 10000, /*LeftEdge=*/1, anchors, 150, 220, 18500);
    EXPECT_TRUE(snapResizeLeft.snapped);
    EXPECT_EQ(snapResizeLeft.targetKind, TimelineSnapTargetKind::AnnBoundary);
    EXPECT_EQ(snapResizeLeft.snappedStartMs, 7500);
    EXPECT_EQ(snapResizeLeft.snappedEndMs, 10000);

    // 4. Alt 自由微调（snapWindowMs = 0）：绝不触发吸附
    const auto freeDrag = StudioTimelineAnalyzer::snapTimelineRangeDrag(
        8450, 10450, 0, anchors, 0, 220, 18500);
    EXPECT_FALSE(freeDrag.snapped);
    EXPECT_EQ(freeDrag.snappedStartMs, 8450);

    // 5. 关键点跳转：从 7500ms 向后必跳到拆分点 8400ms，再向前必回 7500ms
    EXPECT_EQ(StudioTimelineAnalyzer::findNextTimelineKeyframeMs(7500, +1, anchors), 8400);
    EXPECT_EQ(StudioTimelineAnalyzer::findNextTimelineKeyframeMs(8400, -1, anchors), 7500);
}

TEST(StudioRecorderTest, CognitiveFriendlyAutoZoomPresetsCausalFocusTripodHoldAndSpotlight) {
    using namespace tools3000::capture;

    // 1. 三档预置应用与自定义微调态探测
    StudioRenderConfig cfg;
    applyStudioZoomPreset(cfg, StudioZoomPreset::Gentle);
    EXPECT_EQ(detectStudioZoomPreset(cfg), 0);
    EXPECT_NEAR(cfg.defaultZoomFactor, 1.32f, 0.001f);
    EXPECT_TRUE(cfg.lockCameraWhileZoomed);
    EXPECT_GT(cfg.focusSpotlightDim, 0.10f);

    applyStudioZoomPreset(cfg, StudioZoomPreset::Balanced);
    EXPECT_EQ(detectStudioZoomPreset(cfg), 1);
    EXPECT_NEAR(cfg.defaultZoomFactor, 1.60f, 0.001f);
    EXPECT_TRUE(cfg.lockCameraWhileZoomed);

    applyStudioZoomPreset(cfg, StudioZoomPreset::Dynamic);
    EXPECT_EQ(detectStudioZoomPreset(cfg), 2);
    EXPECT_NEAR(cfg.defaultZoomFactor, 1.95f, 0.001f);
    EXPECT_FALSE(cfg.lockCameraWhileZoomed);

    // 手动无级微调后应识别为自定义态 (-1)
    cfg.defaultZoomFactor = 1.45f;
    EXPECT_EQ(detectStudioZoomPreset(cfg), -1);

    // 2. 片头建立镜头保护 + 触发频率分级 + 弹窗因果聚焦校正（点左栏 A -> 弹中间弹窗 B 自动锁 B）
    StudioTimelineData tl;
    tl.sourceWidth = 1920;
    tl.sourceHeight = 1080;
    tl.fps = 30;
    tl.durationMs = 12000;
    // 片头 400ms 孤立单点（应被柔和档片头建立镜头 + 精选门槛过滤）
    tl.clickEvents.push_back({400, 120.0f, 120.0f, false});
    // 4000ms 点击左侧导航菜单 A (110, 420)，160ms 后屏幕正中弹出对话框 B [560, 240, 800x600]
    tl.clickEvents.push_back({4000, 110.0f, 420.0f, false});
    tl.windowEvents.push_back({4160, 0xCAFEBABEu, 560.0f, 240.0f, 800.0f, 600.0f});
    // 8500ms 同一区域连续双击（高价值操作簇）
    tl.clickEvents.push_back({8500, 1450.0f, 620.0f, false});
    tl.clickEvents.push_back({8920, 1470.0f, 635.0f, false});

    const auto gentleSegs = StudioTimelineAnalyzer::buildAutoZoomSegments(tl, 1.32f, false, 0.32f);
    const auto dynamicSegs = StudioTimelineAnalyzer::buildAutoZoomSegments(tl, 1.95f, false, 0.88f);
    ASSERT_EQ(gentleSegs.size(), 2u);
    EXPECT_GT(dynamicSegs.size(), gentleSegs.size());
    // 4000ms 段必须因弹窗因果校正将焦点从左侧 (110, 420) 迁移到对话框中心 (960, 540)
    EXPECT_NEAR(gentleSegs[0].focusX, 960.0f, 5.0f);
    EXPECT_NEAR(gentleSegs[0].focusY, 540.0f, 5.0f);

    // 3. 视觉差分因果校正：点左栏刷新右面板 -> 焦点自动迁右；全屏大刷新 -> 柔和档抑制放大
    StudioTimelineData diffTl;
    diffTl.sourceWidth = 1920;
    diffTl.sourceHeight = 1080;
    diffTl.fps = 30;
    diffTl.durationMs = 6000;
    StudioZoomSegment leftClickSeg;
    leftClickSeg.startMs = 1500;
    leftClickSeg.endMs = 3400;
    leftClickSeg.focusX = 120.0f;
    leftClickSeg.focusY = 300.0f;
    leftClickSeg.targetZoom = 1.35f;
    StudioZoomSegment pageJumpSeg;
    pageJumpSeg.startMs = 4200;
    pageJumpSeg.endMs = 5600;
    pageJumpSeg.focusX = 200.0f;
    pageJumpSeg.focusY = 200.0f;
    pageJumpSeg.targetZoom = 1.35f;
    diffTl.zoomSegments = {leftClickSeg, pageJumpSeg};

    // 构造 60 帧 (0..6s, 每帧 100ms) 代理帧序列
    std::vector<cv::Mat> deck(61);
    for (int i = 0; i < 61; ++i) {
        cv::Mat frame(180, 320, CV_8UC3, cv::Scalar(32, 32, 36));
        if (i >= 19 && i < 45) {
            // t >= 1.9s：右侧局部面板刷新 (约 22% 面积)
            cv::rectangle(frame, cv::Rect(195, 30, 105, 115), cv::Scalar(225, 228, 235), cv::FILLED);
        } else if (i >= 45) {
            // t >= 4.5s：整屏大跳转（全屏颜色剧变 > 75%）
            frame.setTo(cv::Scalar(235, 238, 244));
        }
        deck[static_cast<size_t>(i)] = std::move(frame);
    }
    StudioTimelineAnalyzer::refineZoomSegmentsWithVisualDiff(diffTl, deck, 6.0, 0.32f);
    ASSERT_EQ(diffTl.zoomSegments.size(), 1u); // 全屏大跳转段被柔和档抑制
    EXPECT_GT(diffTl.zoomSegments[0].focusX, 1300.0f); // 焦点从左侧 120 校准至右侧刷新面板重心

    // 4. 三脚架锁镜防晃 (lockCameraWhileZoomed=true) vs 自由跟鼠 (false)
    StudioTimelineData holdTl;
    holdTl.sourceWidth = 1920;
    holdTl.sourceHeight = 1080;
    holdTl.fps = 30;
    holdTl.durationMs = 4000;
    StudioZoomSegment holdSeg;
    holdSeg.startMs = 300;
    holdSeg.endMs = 3500;
    holdSeg.focusX = 960.0f;
    holdSeg.focusY = 540.0f;
    holdSeg.targetZoom = 1.60f;
    holdTl.zoomSegments.push_back(holdSeg);
    // 聚焦期间鼠标在中心周围 280px 范围内移动（超出自由跟鼠 38% 死区，但在锁镜 84% 安全区内）
    for (int t = 0; t <= 4000; t += 50) {
        const float cx = (t >= 1200 && t <= 2800) ? (960.0f + 280.0f) : 960.0f;
        holdTl.cursorSamples.push_back({t, cx, 540.0f, StudioCursorKind::Arrow, false, false});
    }

    StudioRenderConfig lockCfg;
    applyStudioZoomPreset(lockCfg, StudioZoomPreset::Balanced);
    lockCfg.lockCameraWhileZoomed = true;
    const auto bakedLocked = StudioTimelineAnalyzer::bakeEvaluatedFrames(holdTl, lockCfg, 30, 120);

    StudioRenderConfig freeCfg = lockCfg;
    freeCfg.lockCameraWhileZoomed = false;
    const auto bakedFree = StudioTimelineAnalyzer::bakeEvaluatedFrames(holdTl, freeCfg, 30, 120);

    // 在 t = 2.4s (第 72 帧) 处，锁镜应纹丝不动停在 960，自由跟鼠应明显右漂
    EXPECT_NEAR(bakedLocked[72].camera.centerX, 960.0f, 3.0f);
    EXPECT_GT(bakedFree[72].camera.centerX, 1005.0f);

    // 5. 零缩放纯聚光模式 (defaultZoomFactor = 1.00x + focusSpotlightDim = 0.40) + 真实合成暗角像素验证
    StudioRenderConfig spotCfg;
    spotCfg.autoZoomEnabled = true;
    spotCfg.zoomMode = StudioZoomMode::Zoom2D;
    spotCfg.defaultZoomFactor = 1.00f;
    spotCfg.focusSpotlightDim = 0.40f;
    spotCfg.autoFrameEnabled = false;
    spotCfg.smoothCursorEnabled = false;
    spotCfg.showClickRipple = false;
    const auto bakedSpot = StudioTimelineAnalyzer::bakeEvaluatedFrames(holdTl, spotCfg, 30, 120);
    EXPECT_NEAR(bakedSpot[60].camera.zoom, 1.00f, 0.001f);
    EXPECT_GT(bakedSpot[60].spotlightAlpha, 0.30f);

    cv::Mat brightSrc(360, 640, CV_8UC3, cv::Scalar(200, 200, 200));
    StudioVideoCompositor compositor;
    const cv::Mat composed = compositor.composeFrame(
        brightSrc, bakedSpot[60], spotCfg, cv::Size(640, 360));
    ASSERT_FALSE(composed.empty());
    const cv::Vec3b centerPx = composed.at<cv::Vec3b>(180, 320);
    const cv::Vec3b cornerPx = composed.at<cv::Vec3b>(8, 8);
    EXPECT_GT(centerPx[0], 190); // 焦点中心保持原亮度
    EXPECT_LT(cornerPx[0], 155); // 外围明显柔和压暗

    // 6. rebuildAutoZoomSegmentsPreservingManual 严格保留手动运镜块且零重叠
    StudioTimelineData rebuildTl = tl;
    StudioZoomSegment userManual;
    userManual.startMs = 3800;
    userManual.endMs = 5200;
    userManual.focusX = 500.0f;
    userManual.focusY = 500.0f;
    userManual.targetZoom = 2.10f;
    userManual.manualInserted = true;
    rebuildTl.zoomSegments = {userManual};

    StudioRenderConfig dynCfg;
    applyStudioZoomPreset(dynCfg, StudioZoomPreset::Dynamic);
    StudioTimelineAnalyzer::rebuildAutoZoomSegmentsPreservingManual(rebuildTl, dynCfg);
    bool foundManual = false;
    for (const auto& seg : rebuildTl.zoomSegments) {
        if (seg.manualInserted) {
            foundManual = true;
            EXPECT_FLOAT_EQ(seg.targetZoom, 2.10f);
        }
    }
    EXPECT_TRUE(foundManual);
    EXPECT_GE(rebuildTl.zoomSegments.size(), 2u);
}
