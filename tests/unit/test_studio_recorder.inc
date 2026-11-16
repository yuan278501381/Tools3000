// ─────────────────────────────────────────────────────────────────────────────
// test_studio_recorder.inc — Screen Studio 级自动运镜、光标平滑、壁纸合成与转码回归测试
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/StudioTimeline.h"
#include "capture/StudioVideoCompositor.h"
#include "capture/StudioPreviewWindow.h"

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
