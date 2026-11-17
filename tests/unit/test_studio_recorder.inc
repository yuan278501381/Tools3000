// ─────────────────────────────────────────────────────────────────────────────
// test_studio_recorder.inc — Screen Studio 级自动运镜、光标平滑、壁纸合成与转码回归测试
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/StudioTimeline.h"
#include "capture/StudioVideoCompositor.h"
#include "capture/StudioPreviewWindow.h"
#include "capture/VoiceEnhancer.h"

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
